/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * By contributing to this project, you agree to license your contributions
 * under the GPLv3 (or any later version) or any future licenses chosen by
 * the project author(s). Contributions include any modifications,
 * enhancements, or additions to the project. These contributions become
 * part of the project and are adopted by the project author(s).
 *
 * Authentication Database Core Module
 *
 * Provides database lifecycle management, schema creation, and prepared
 * statement initialization. This module defines the shared s_db state
 * used by all other auth_db modules.
 *
 * SECURITY: All database operations use prepared statements.
 * NEVER use sqlite3_exec() or sqlite3_mprintf() with user input.
 * See: CWE-89, OWASP SQL Injection Prevention Cheat Sheet
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "auth/auth_db_internal.h"
#include "core/path_utils.h"
#include "logging.h"

/* =============================================================================
 * Shared State Definition
 * ============================================================================= */

auth_db_state_t s_db = {
   .db = NULL,
   .mutex = PTHREAD_MUTEX_INITIALIZER,
   .initialized = false,
   .last_cleanup = 0,
   .last_vacuum = 0,
};

/* =============================================================================
 * Forward Declarations
 * ============================================================================= */

static int create_schema(const char *db_path);
static int prepare_statements(void);
static void finalize_statements(void);

/* =============================================================================
 * Schema SQL
 * ============================================================================= */

/* Base schema for fresh installs.  Must match AUTH_DB_SCHEMA_VERSION.
 *
 * IMPORTANT: When adding a new column or table via migration, also add it here
 * so that fresh installs get the complete schema.  All statements use
 * IF NOT EXISTS / ADD COLUMN guards for idempotency with the migration path. */
static const char *SCHEMA_SQL =
    /* Schema version tracking */
    "CREATE TABLE IF NOT EXISTS schema_version ("
    "   version INTEGER PRIMARY KEY"
    ");"

    /* Users table (categories_backfilled_at added in v34 — gates lazy fact-category backfill) */
    "CREATE TABLE IF NOT EXISTS users ("
    "   id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "   username TEXT UNIQUE NOT NULL,"
    "   password_hash TEXT NOT NULL,"
    "   is_admin INTEGER DEFAULT 0,"
    "   created_at INTEGER NOT NULL,"
    "   last_login INTEGER,"
    "   failed_attempts INTEGER DEFAULT 0,"
    "   lockout_until INTEGER DEFAULT 0,"
    "   categories_backfilled_at INTEGER DEFAULT 0"
    ");"

    /* Sessions table */
    "CREATE TABLE IF NOT EXISTS sessions ("
    "   token TEXT PRIMARY KEY,"
    "   user_id INTEGER NOT NULL,"
    "   created_at INTEGER NOT NULL,"
    "   last_activity INTEGER NOT NULL,"
    "   expires_at INTEGER,"
    "   ip_address TEXT,"
    "   user_agent TEXT,"
    "   FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_sessions_user ON sessions(user_id);"
    "CREATE INDEX IF NOT EXISTS idx_sessions_activity ON sessions(last_activity);"
    /* idx_sessions_expires created by v10 migration after expires_at column is added */

    /* Login attempts for rate limiting */
    "CREATE TABLE IF NOT EXISTS login_attempts ("
    "   id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "   ip_address TEXT NOT NULL,"
    "   username TEXT,"
    "   timestamp INTEGER NOT NULL,"
    "   success INTEGER DEFAULT 0"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_attempts_ip ON login_attempts(ip_address, timestamp);"

    /* Audit log */
    "CREATE TABLE IF NOT EXISTS auth_log ("
    "   id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "   timestamp INTEGER NOT NULL,"
    "   event TEXT NOT NULL,"
    "   username TEXT,"
    "   ip_address TEXT,"
    "   details TEXT"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_log_timestamp ON auth_log(timestamp);"

    /* Per-user settings (added in schema v2, persona_mode added in v3) */
    "CREATE TABLE IF NOT EXISTS user_settings ("
    "   user_id INTEGER PRIMARY KEY,"
    "   persona_description TEXT,"
    "   persona_mode TEXT DEFAULT 'append',"
    "   location TEXT,"
    "   timezone TEXT DEFAULT 'UTC',"
    "   units TEXT DEFAULT 'metric',"
    "   tts_voice_model TEXT,"
    "   tts_length_scale REAL DEFAULT 1.0,"
    "   theme TEXT DEFAULT 'cyan',"
    "   updated_at INTEGER NOT NULL,"
    "   FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE"
    ");"

    /* Conversations table (added in schema v4, context columns in v5, continuation in v7,
     * LLM settings in v11, extraction tracking in v15, privacy in v16, origin in v17) */
    "CREATE TABLE IF NOT EXISTS conversations ("
    "   id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "   user_id INTEGER NOT NULL,"
    "   title TEXT NOT NULL DEFAULT 'New Conversation',"
    "   created_at INTEGER NOT NULL,"
    "   updated_at INTEGER NOT NULL,"
    "   message_count INTEGER DEFAULT 0,"
    "   is_archived INTEGER DEFAULT 0,"
    "   context_tokens INTEGER DEFAULT 0,"
    "   context_max INTEGER DEFAULT 0,"
    "   continued_from INTEGER DEFAULT NULL,"
    "   compaction_summary TEXT DEFAULT NULL,"
    "   llm_type TEXT DEFAULT NULL,"
    "   cloud_provider TEXT DEFAULT NULL,"
    "   model TEXT DEFAULT NULL,"
    "   tools_mode TEXT DEFAULT NULL,"
    "   thinking_mode TEXT DEFAULT NULL,"
    "   reasoning_effort TEXT DEFAULT NULL,"
    "   last_extracted_msg_count INTEGER DEFAULT 0,"
    "   is_private INTEGER DEFAULT 0,"
    "   title_locked INTEGER DEFAULT 0,"
    "   origin TEXT DEFAULT 'webui',"
    "   FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,"
    "   FOREIGN KEY (continued_from) REFERENCES conversations(id) ON DELETE SET NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_conversations_user ON conversations(user_id, updated_at DESC);"
    "CREATE INDEX IF NOT EXISTS idx_conversations_search ON conversations(user_id, title);"
    /* Note: idx_conversations_continued is created during migration or post-init
     * to handle both new databases and upgrades from v6 */

    /* Messages table (added in schema v4) */
    "CREATE TABLE IF NOT EXISTS messages ("
    "   id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "   conversation_id INTEGER NOT NULL,"
    "   role TEXT NOT NULL CHECK(role IN ('system', 'user', 'assistant', 'tool')),"
    "   content TEXT NOT NULL,"
    "   created_at INTEGER NOT NULL,"
    "   FOREIGN KEY (conversation_id) REFERENCES conversations(id) ON DELETE CASCADE"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_messages_conversation ON messages(conversation_id, id ASC);"

    /* Session metrics table (added in schema v8) */
    "CREATE TABLE IF NOT EXISTS session_metrics ("
    "   id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "   session_id INTEGER NOT NULL,"
    "   user_id INTEGER,"
    "   session_type TEXT NOT NULL,"
    "   started_at INTEGER NOT NULL,"
    "   ended_at INTEGER,"
    "   queries_total INTEGER DEFAULT 0,"
    "   queries_cloud INTEGER DEFAULT 0,"
    "   queries_local INTEGER DEFAULT 0,"
    "   errors_count INTEGER DEFAULT 0,"
    "   fallbacks_count INTEGER DEFAULT 0,"
    "   avg_asr_ms REAL,"
    "   avg_llm_ttft_ms REAL,"
    "   avg_llm_total_ms REAL,"
    "   avg_tts_ms REAL,"
    "   avg_pipeline_ms REAL,"
    "   FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE SET NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_session_metrics_user ON session_metrics(user_id, started_at "
    "DESC);"
    "CREATE INDEX IF NOT EXISTS idx_session_metrics_time ON session_metrics(started_at DESC);"

    /* Per-provider token usage breakdown (added in schema v8) */
    "CREATE TABLE IF NOT EXISTS session_metrics_providers ("
    "   id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "   session_metrics_id INTEGER NOT NULL,"
    "   provider TEXT NOT NULL,"
    "   tokens_input INTEGER DEFAULT 0,"
    "   tokens_output INTEGER DEFAULT 0,"
    "   tokens_cached INTEGER DEFAULT 0,"
    "   queries INTEGER DEFAULT 0,"
    "   FOREIGN KEY (session_metrics_id) REFERENCES session_metrics(id) ON DELETE CASCADE"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_metrics_providers_session ON "
    "session_metrics_providers(session_metrics_id);"

    /* Images table — filesystem-backed metadata (v30, migrated from BLOB in v12-v29) */
    "CREATE TABLE IF NOT EXISTS images ("
    "   id TEXT PRIMARY KEY,"
    "   user_id INTEGER NOT NULL,"
    "   source INTEGER NOT NULL DEFAULT 0,"
    "   retention_policy INTEGER NOT NULL DEFAULT 0,"
    "   mime_type TEXT NOT NULL,"
    "   size INTEGER NOT NULL,"
    "   filename TEXT NOT NULL,"
    "   created_at INTEGER NOT NULL,"
    "   last_accessed INTEGER,"
    "   FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_images_user ON images(user_id);"
    "CREATE INDEX IF NOT EXISTS idx_images_created ON images(created_at);"

    /* Satellite mappings table (added in schema v20) */
    "CREATE TABLE IF NOT EXISTS satellite_mappings ("
    "   uuid TEXT PRIMARY KEY,"
    "   name TEXT NOT NULL DEFAULT '',"
    "   location TEXT NOT NULL DEFAULT '',"
    "   ha_area TEXT DEFAULT '',"
    "   user_id INTEGER DEFAULT NULL,"
    "   tier INTEGER DEFAULT 1,"
    "   last_seen INTEGER DEFAULT 0,"
    "   created_at INTEGER NOT NULL,"
    "   enabled INTEGER DEFAULT 1,"
    "   FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE SET NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_satellite_user ON satellite_mappings(user_id);"

    /* Memory system tables (v14, columns extended in v15/v19) */
    "CREATE TABLE IF NOT EXISTS memory_facts ("
    "   id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "   user_id INTEGER NOT NULL,"
    "   fact_text TEXT NOT NULL,"
    "   confidence REAL DEFAULT 1.0,"
    "   source TEXT DEFAULT 'inferred',"
    "   category TEXT NOT NULL DEFAULT 'general',"
    "   created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
    "   last_accessed INTEGER,"
    "   access_count INTEGER DEFAULT 0,"
    "   superseded_by INTEGER,"
    "   normalized_hash INTEGER DEFAULT 0,"
    "   embedding BLOB DEFAULT NULL,"
    "   embedding_norm REAL DEFAULT NULL,"
    "   FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,"
    "   FOREIGN KEY (superseded_by) REFERENCES memory_facts(id) ON DELETE SET NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_memory_facts_user ON memory_facts(user_id);"
    "CREATE INDEX IF NOT EXISTS idx_memory_facts_confidence ON "
    "memory_facts(user_id, confidence DESC);"
    "CREATE INDEX IF NOT EXISTS idx_memory_facts_hash ON memory_facts(user_id, normalized_hash);"
    /* idx_memory_facts_user_category is created by the v34 migration block (runs
     * after ALTER TABLE adds the column).  Keeping it here would fail on an
     * existing pre-v34 DB because CREATE TABLE IF NOT EXISTS is a no-op for
     * the already-existing table, so the column isn't added until migrations run. */

    "CREATE TABLE IF NOT EXISTS memory_preferences ("
    "   id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "   user_id INTEGER NOT NULL,"
    "   category TEXT NOT NULL,"
    "   value TEXT NOT NULL,"
    "   confidence REAL DEFAULT 0.5,"
    "   source TEXT DEFAULT 'inferred',"
    "   created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
    "   updated_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
    "   reinforcement_count INTEGER DEFAULT 1,"
    "   FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,"
    "   UNIQUE(user_id, category)"
    ");"

    "CREATE TABLE IF NOT EXISTS memory_summaries ("
    "   id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "   user_id INTEGER NOT NULL,"
    "   session_id TEXT NOT NULL,"
    "   summary TEXT NOT NULL,"
    "   topics TEXT,"
    "   sentiment TEXT,"
    "   created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
    "   message_count INTEGER,"
    "   duration_seconds INTEGER,"
    "   consolidated INTEGER DEFAULT 0,"
    "   FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_memory_summaries_user ON "
    "memory_summaries(user_id, created_at DESC);"

    /* Entity/relation tables (v19) */
    "CREATE TABLE IF NOT EXISTS memory_entities ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  user_id INTEGER NOT NULL,"
    "  name TEXT NOT NULL,"
    "  entity_type TEXT NOT NULL,"
    "  canonical_name TEXT NOT NULL,"
    "  embedding BLOB DEFAULT NULL,"
    "  embedding_norm REAL DEFAULT NULL,"
    "  photo_id TEXT DEFAULT NULL,"
    "  first_seen INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
    "  last_seen INTEGER,"
    "  mention_count INTEGER DEFAULT 1,"
    "  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,"
    "  UNIQUE(user_id, canonical_name)"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_memory_entities_user ON memory_entities(user_id);"

    /* memory_relations: valid_from/valid_to added in v33.  NULL = open-ended (no bound).
     * "currently true" predicate: valid_to IS NULL OR valid_to > now() */
    "CREATE TABLE IF NOT EXISTS memory_relations ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  user_id INTEGER NOT NULL,"
    "  subject_entity_id INTEGER NOT NULL,"
    "  relation TEXT NOT NULL,"
    "  object_entity_id INTEGER,"
    "  object_value TEXT,"
    "  fact_id INTEGER,"
    "  confidence REAL DEFAULT 0.8,"
    "  created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
    "  valid_from INTEGER DEFAULT NULL,"
    "  valid_to INTEGER DEFAULT NULL,"
    "  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,"
    "  FOREIGN KEY (subject_entity_id) REFERENCES memory_entities(id) ON DELETE CASCADE,"
    "  FOREIGN KEY (object_entity_id) REFERENCES memory_entities(id) ON DELETE SET NULL,"
    "  FOREIGN KEY (fact_id) REFERENCES memory_facts(id) ON DELETE SET NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_memory_relations_subject ON "
    "memory_relations(subject_entity_id);"
    "CREATE INDEX IF NOT EXISTS idx_memory_relations_object ON memory_relations(object_entity_id);"
    "CREATE INDEX IF NOT EXISTS idx_memory_relations_user ON memory_relations(user_id);"
    /* idx_memory_relations_user_validity + idx_memory_relations_subject_open are
     * created by the v33 migration block (same reason — runs after the
     * valid_from/valid_to ALTER so the columns exist). */

    /* Scheduler events (v18) */
    "CREATE TABLE IF NOT EXISTS scheduled_events ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  user_id INTEGER NOT NULL,"
    "  event_type TEXT NOT NULL DEFAULT 'timer',"
    "  status TEXT NOT NULL DEFAULT 'pending',"
    "  name TEXT NOT NULL,"
    "  message TEXT,"
    "  fire_at INTEGER NOT NULL,"
    "  created_at INTEGER NOT NULL,"
    "  duration_sec INTEGER DEFAULT 0,"
    "  snoozed_until INTEGER DEFAULT 0,"
    "  recurrence TEXT DEFAULT 'once',"
    "  recurrence_days TEXT,"
    "  original_time TEXT,"
    "  source_uuid TEXT,"
    "  source_location TEXT,"
    "  source_client_type INTEGER DEFAULT 0,"
    "  announce_all INTEGER DEFAULT 0,"
    "  tool_name TEXT,"
    "  tool_action TEXT,"
    "  tool_value TEXT,"
    "  fired_at INTEGER DEFAULT 0,"
    "  snooze_count INTEGER DEFAULT 0,"
    "  FOREIGN KEY (user_id) REFERENCES users(id)"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_sched_status_fire ON scheduled_events(status, fire_at);"
    "CREATE INDEX IF NOT EXISTS idx_sched_user ON scheduled_events(user_id, status);"
    "CREATE INDEX IF NOT EXISTS idx_sched_user_name ON scheduled_events(user_id, status, name);"
    "CREATE INDEX IF NOT EXISTS idx_sched_source ON scheduled_events(source_uuid);"

    /* Missed scheduler notifications (v32) — queued when a ringing event has no
     * connected clients for the target user; replayed on reconnect. */
    "CREATE TABLE IF NOT EXISTS missed_notifications ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  user_id INTEGER NOT NULL,"
    "  event_id INTEGER NOT NULL,"
    "  event_type TEXT NOT NULL,"
    "  status TEXT NOT NULL,"
    "  name TEXT NOT NULL,"
    "  message TEXT,"
    "  fire_at INTEGER NOT NULL,"
    "  conversation_id INTEGER DEFAULT 0,"
    "  created_at INTEGER NOT NULL,"
    "  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_missed_notif_user "
    "  ON missed_notifications(user_id, created_at);"

    /* Documents and chunks for RAG search (v22) */
    "CREATE TABLE IF NOT EXISTS documents ("
    "  id INTEGER PRIMARY KEY,"
    "  user_id INTEGER,"
    "  filename TEXT NOT NULL,"
    "  filepath TEXT NOT NULL,"
    "  filetype TEXT NOT NULL,"
    "  file_hash TEXT NOT NULL,"
    "  num_chunks INTEGER NOT NULL,"
    "  is_global INTEGER DEFAULT 0,"
    "  created_at INTEGER NOT NULL,"
    "  FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE"
    ");"
    /* document_chunks.created_at added in v35 — used by temporal-query scoring to
     * boost chunks whose origin date is near the user's referenced point in time
     * (e.g., "what did we discuss in summer 2021"). 0 = unknown (no boost). */
    "CREATE TABLE IF NOT EXISTS document_chunks ("
    "  id INTEGER PRIMARY KEY,"
    "  document_id INTEGER NOT NULL,"
    "  chunk_index INTEGER NOT NULL,"
    "  text TEXT NOT NULL,"
    "  embedding BLOB NOT NULL,"
    "  embedding_norm REAL NOT NULL,"
    "  created_at INTEGER NOT NULL DEFAULT 0,"
    "  FOREIGN KEY(document_id) REFERENCES documents(id) ON DELETE CASCADE"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_doc_chunks_doc ON document_chunks(document_id);"
    "CREATE INDEX IF NOT EXISTS idx_documents_user ON documents(user_id);"
    "CREATE INDEX IF NOT EXISTS idx_documents_hash ON documents(file_hash);"

    /* Calendar tables (v23, read_only from v24, oauth from v25) */
    "CREATE TABLE IF NOT EXISTS calendar_accounts ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  user_id INTEGER NOT NULL,"
    "  name TEXT NOT NULL,"
    "  caldav_url TEXT NOT NULL,"
    "  username TEXT NOT NULL,"
    "  encrypted_password BLOB NOT NULL,"
    "  auth_type TEXT DEFAULT 'basic',"
    "  principal_url TEXT DEFAULT '',"
    "  calendar_home_url TEXT DEFAULT '',"
    "  enabled INTEGER DEFAULT 1,"
    "  read_only INTEGER DEFAULT 0,"
    "  last_sync INTEGER DEFAULT 0,"
    "  sync_interval_sec INTEGER DEFAULT 900,"
    "  created_at INTEGER NOT NULL,"
    "  oauth_account_key TEXT DEFAULT '',"
    "  FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_cal_acct_user ON calendar_accounts(user_id);"

    "CREATE TABLE IF NOT EXISTS calendar_calendars ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  account_id INTEGER NOT NULL,"
    "  caldav_path TEXT NOT NULL,"
    "  display_name TEXT DEFAULT '',"
    "  color TEXT DEFAULT '',"
    "  is_active INTEGER DEFAULT 1,"
    "  ctag TEXT DEFAULT '',"
    "  created_at INTEGER NOT NULL,"
    "  FOREIGN KEY(account_id) REFERENCES calendar_accounts(id) ON DELETE CASCADE"
    ");"

    "CREATE TABLE IF NOT EXISTS calendar_events ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  calendar_id INTEGER NOT NULL,"
    "  uid TEXT NOT NULL,"
    "  etag TEXT DEFAULT '',"
    "  summary TEXT DEFAULT '',"
    "  description TEXT DEFAULT '',"
    "  location TEXT DEFAULT '',"
    "  dtstart INTEGER DEFAULT 0,"
    "  dtend INTEGER DEFAULT 0,"
    "  duration_sec INTEGER DEFAULT 0,"
    "  all_day INTEGER DEFAULT 0,"
    "  dtstart_date TEXT DEFAULT '',"
    "  dtend_date TEXT DEFAULT '',"
    "  rrule TEXT DEFAULT '',"
    "  raw_ical TEXT,"
    "  last_synced INTEGER DEFAULT 0,"
    "  FOREIGN KEY(calendar_id) REFERENCES calendar_calendars(id) ON DELETE CASCADE"
    ");"
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_cal_events_uid ON calendar_events(calendar_id, uid);"

    "CREATE TABLE IF NOT EXISTS calendar_occurrences ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  event_id INTEGER NOT NULL,"
    "  dtstart INTEGER DEFAULT 0,"
    "  dtend INTEGER DEFAULT 0,"
    "  all_day INTEGER DEFAULT 0,"
    "  dtstart_date TEXT DEFAULT '',"
    "  dtend_date TEXT DEFAULT '',"
    "  summary TEXT DEFAULT '',"
    "  location TEXT DEFAULT '',"
    "  is_override INTEGER DEFAULT 0,"
    "  is_cancelled INTEGER DEFAULT 0,"
    "  recurrence_id TEXT DEFAULT '',"
    "  FOREIGN KEY(event_id) REFERENCES calendar_events(id) ON DELETE CASCADE"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_cal_occ_event ON calendar_occurrences(event_id);"
    "CREATE INDEX IF NOT EXISTS idx_cal_occ_time ON calendar_occurrences(dtstart, dtend);"
    "CREATE INDEX IF NOT EXISTS idx_cal_occ_date ON calendar_occurrences(dtstart_date);"

    /* OAuth token storage (v25) */
    "CREATE TABLE IF NOT EXISTS oauth_tokens ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  user_id INTEGER NOT NULL,"
    "  provider TEXT NOT NULL,"
    "  account_key TEXT NOT NULL,"
    "  encrypted_data BLOB NOT NULL,"
    "  encrypted_data_len INTEGER NOT NULL,"
    "  scopes TEXT DEFAULT '',"
    "  created_at INTEGER NOT NULL,"
    "  updated_at INTEGER NOT NULL,"
    "  FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE,"
    "  UNIQUE(user_id, provider, account_key)"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_oauth_user_provider ON oauth_tokens(user_id, provider);"

    /* Contacts (v26) */
    "CREATE TABLE IF NOT EXISTS contacts ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  user_id INTEGER NOT NULL,"
    "  entity_id INTEGER NOT NULL,"
    "  field_type TEXT NOT NULL,"
    "  value TEXT NOT NULL,"
    "  label TEXT DEFAULT '',"
    "  created_at INTEGER NOT NULL,"
    "  FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE,"
    "  FOREIGN KEY(entity_id) REFERENCES memory_entities(id) ON DELETE CASCADE"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_contacts_entity ON contacts(entity_id);"
    "CREATE INDEX IF NOT EXISTS idx_contacts_user_type ON contacts(user_id, field_type);"

    /* Email accounts (v26) */
    "CREATE TABLE IF NOT EXISTS email_accounts ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  user_id INTEGER NOT NULL,"
    "  name TEXT NOT NULL,"
    "  imap_server TEXT NOT NULL,"
    "  imap_port INTEGER DEFAULT 993,"
    "  imap_ssl INTEGER DEFAULT 1,"
    "  smtp_server TEXT NOT NULL,"
    "  smtp_port INTEGER DEFAULT 465,"
    "  smtp_ssl INTEGER DEFAULT 1,"
    "  username TEXT NOT NULL,"
    "  display_name TEXT DEFAULT '',"
    "  encrypted_password BLOB,"
    "  encrypted_password_len INTEGER DEFAULT 0,"
    "  auth_type TEXT DEFAULT 'app_password',"
    "  oauth_account_key TEXT DEFAULT '',"
    "  enabled INTEGER DEFAULT 1,"
    "  read_only INTEGER DEFAULT 0,"
    "  max_recent INTEGER DEFAULT 10,"
    "  max_body_chars INTEGER DEFAULT 4000,"
    "  created_at INTEGER NOT NULL,"
    "  FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_email_acct_user ON email_accounts(user_id);"

    /* Phone call and SMS logs (v29) */
    "CREATE TABLE IF NOT EXISTS phone_call_log ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  user_id INTEGER NOT NULL,"
    "  direction INTEGER NOT NULL,"
    "  number TEXT NOT NULL,"
    "  contact_name TEXT DEFAULT '',"
    "  duration_sec INTEGER DEFAULT 0,"
    "  timestamp INTEGER NOT NULL,"
    "  status INTEGER NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_phone_call_user_ts "
    "  ON phone_call_log(user_id, timestamp DESC);"
    "CREATE TABLE IF NOT EXISTS phone_sms_log ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  user_id INTEGER NOT NULL,"
    "  direction INTEGER NOT NULL,"
    "  number TEXT NOT NULL,"
    "  contact_name TEXT DEFAULT '',"
    "  body TEXT NOT NULL,"
    "  timestamp INTEGER NOT NULL,"
    "  read INTEGER DEFAULT 0,"
    "  image_id TEXT DEFAULT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_phone_sms_user_ts "
    "  ON phone_sms_log(user_id, timestamp DESC);"
    "CREATE INDEX IF NOT EXISTS idx_phone_sms_unread "
    "  ON phone_sms_log(user_id, read) WHERE read = 0;";

/* =============================================================================
 * Schema Version and Migration
 * ============================================================================= */

static int get_current_schema_version(void) {
   sqlite3_stmt *stmt = NULL;
   int version = 0;

   int rc = sqlite3_prepare_v2(s_db.db, "SELECT version FROM schema_version LIMIT 1", -1, &stmt,
                               NULL);
   if (rc == SQLITE_OK) {
      if (sqlite3_step(stmt) == SQLITE_ROW) {
         version = sqlite3_column_int(stmt, 0);
      }
      sqlite3_finalize(stmt);
   }
   return version;
}

static int create_schema(const char *db_path) {
   char *errmsg = NULL;

   /* Check current schema version (0 if fresh install) */
   int current_version = get_current_schema_version();

   /* Execute schema SQL - all tables use IF NOT EXISTS for idempotency */
   int rc = sqlite3_exec(s_db.db, SCHEMA_SQL, NULL, NULL, &errmsg);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: schema creation failed: %s", errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      return AUTH_DB_FAILURE;
   }

   /* v3 migration: add persona_mode column to user_settings if missing
    * This handles upgrades from v1 or v2 where the table may exist without this column */
   if (current_version >= 1 && current_version < 3) {
      rc = sqlite3_exec(s_db.db,
                        "ALTER TABLE user_settings ADD COLUMN persona_mode TEXT DEFAULT 'append'",
                        NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         /* Column might already exist or table might not exist yet - not fatal */
         OLOG_INFO("auth_db: v3 migration note: %s (may be normal)", errmsg ? errmsg : "ok");
         sqlite3_free(errmsg);
         errmsg = NULL;
      } else {
         OLOG_INFO("auth_db: added persona_mode column to user_settings");
      }
   }

   /* v5 migration: add context_tokens and context_max columns to conversations
    * Only runs if conversations table already exists (v4+) without these columns */
   if (current_version >= 1 && current_version < 5) {
      rc = sqlite3_exec(s_db.db,
                        "ALTER TABLE conversations ADD COLUMN context_tokens INTEGER DEFAULT 0",
                        NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_INFO("auth_db: v5 migration note (context_tokens): %s", errmsg ? errmsg : "ok");
         sqlite3_free(errmsg);
         errmsg = NULL;
      }
      rc = sqlite3_exec(s_db.db,
                        "ALTER TABLE conversations ADD COLUMN context_max INTEGER DEFAULT 0", NULL,
                        NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_INFO("auth_db: v5 migration note (context_max): %s", errmsg ? errmsg : "ok");
         sqlite3_free(errmsg);
         errmsg = NULL;
      } else {
         OLOG_INFO("auth_db: added context columns to conversations");
      }
   }

   /* v6 migration: update messages table CHECK constraint to include 'tool' role
    * SQLite doesn't support ALTER TABLE to modify constraints, so we recreate the table */
   if (current_version >= 4 && current_version < 6) {
      OLOG_INFO("auth_db: migrating messages table to support 'tool' role");
      const char *migration_sql =
          "BEGIN TRANSACTION;"
          "CREATE TABLE messages_new ("
          "   id INTEGER PRIMARY KEY AUTOINCREMENT,"
          "   conversation_id INTEGER NOT NULL,"
          "   role TEXT NOT NULL CHECK(role IN ('system', 'user', 'assistant', 'tool')),"
          "   content TEXT NOT NULL,"
          "   created_at INTEGER NOT NULL,"
          "   FOREIGN KEY (conversation_id) REFERENCES conversations(id) ON DELETE CASCADE"
          ");"
          "INSERT INTO messages_new SELECT * FROM messages;"
          "DROP TABLE messages;"
          "ALTER TABLE messages_new RENAME TO messages;"
          "CREATE INDEX IF NOT EXISTS idx_messages_conversation ON messages(conversation_id, id "
          "ASC);"
          "COMMIT;";

      rc = sqlite3_exec(s_db.db, migration_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_ERROR("auth_db: v6 migration failed: %s", errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         errmsg = NULL;
         /* Rollback on failure */
         sqlite3_exec(s_db.db, "ROLLBACK;", NULL, NULL, NULL);
      } else {
         OLOG_INFO("auth_db: migrated messages table to v6 (added 'tool' role)");
      }
   }

   /* v7 migration: add continued_from and compaction_summary columns to conversations
    * These support conversation continuation when context compaction occurs */
   if (current_version >= 4 && current_version < 7) {
      rc = sqlite3_exec(s_db.db,
                        "ALTER TABLE conversations ADD COLUMN continued_from INTEGER DEFAULT NULL "
                        "REFERENCES conversations(id) ON DELETE SET NULL",
                        NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_INFO("auth_db: v7 migration note (continued_from): %s", errmsg ? errmsg : "ok");
         sqlite3_free(errmsg);
         errmsg = NULL;
      }
      rc = sqlite3_exec(s_db.db,
                        "ALTER TABLE conversations ADD COLUMN compaction_summary TEXT DEFAULT NULL",
                        NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_INFO("auth_db: v7 migration note (compaction_summary): %s", errmsg ? errmsg : "ok");
         sqlite3_free(errmsg);
         errmsg = NULL;
      }
      /* Add index for finding child conversations */
      rc = sqlite3_exec(
          s_db.db,
          "CREATE INDEX IF NOT EXISTS idx_conversations_continued ON conversations(continued_from)",
          NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_INFO("auth_db: v7 migration note (index): %s", errmsg ? errmsg : "ok");
         sqlite3_free(errmsg);
         errmsg = NULL;
      } else {
         OLOG_INFO("auth_db: added continuation columns to conversations (v7)");
      }
   }

   /* v8 migration: session_metrics table
    * The table is created by SCHEMA_SQL with IF NOT EXISTS, so no explicit
    * migration is needed. Just log the upgrade for existing databases. */
   if (current_version >= 1 && current_version < 8) {
      OLOG_INFO("auth_db: added session_metrics table (v8)");
   }

   /* v9 migration: add theme column to user_settings */
   if (current_version >= 1 && current_version < 9) {
      rc = sqlite3_exec(s_db.db, "ALTER TABLE user_settings ADD COLUMN theme TEXT DEFAULT 'cyan'",
                        NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_INFO("auth_db: v9 migration note (theme): %s", errmsg ? errmsg : "ok");
         sqlite3_free(errmsg);
         errmsg = NULL;
      } else {
         OLOG_INFO("auth_db: added theme column to user_settings");
      }
   }

   /* v10 migration: add expires_at column to sessions for "Remember Me" feature
    * Existing sessions get expires_at = last_activity + 24 hours */
   if (current_version >= 1 && current_version < 10) {
      rc = sqlite3_exec(s_db.db, "ALTER TABLE sessions ADD COLUMN expires_at INTEGER", NULL, NULL,
                        &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_INFO("auth_db: v10 migration note (expires_at): %s", errmsg ? errmsg : "ok");
         sqlite3_free(errmsg);
         errmsg = NULL;
      } else {
         /* Set default expires_at for existing sessions (last_activity + 24h) */
         char update_sql[128];
         snprintf(update_sql, sizeof(update_sql),
                  "UPDATE sessions SET expires_at = last_activity + %d WHERE expires_at IS NULL",
                  AUTH_SESSION_TIMEOUT_SEC);
         rc = sqlite3_exec(s_db.db, update_sql, NULL, NULL, &errmsg);
         if (rc != SQLITE_OK) {
            OLOG_WARNING("auth_db: v10 migration (set defaults): %s", errmsg ? errmsg : "ok");
            sqlite3_free(errmsg);
            errmsg = NULL;
         }
         OLOG_INFO("auth_db: added expires_at column to sessions (v10)");
      }
      /* Create index for efficient cleanup queries */
      rc = sqlite3_exec(s_db.db,
                        "CREATE INDEX IF NOT EXISTS idx_sessions_expires ON sessions(expires_at)",
                        NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_INFO("auth_db: v10 migration (index): %s", errmsg ? errmsg : "ok");
         sqlite3_free(errmsg);
         errmsg = NULL;
      }
   }

   /* v11 migration: add per-conversation LLM settings columns */
   if (current_version >= 4 && current_version < 11) {
      const char *cols[] = {
         "ALTER TABLE conversations ADD COLUMN llm_type TEXT DEFAULT NULL",
         "ALTER TABLE conversations ADD COLUMN cloud_provider TEXT DEFAULT NULL",
         "ALTER TABLE conversations ADD COLUMN model TEXT DEFAULT NULL",
         "ALTER TABLE conversations ADD COLUMN tools_mode TEXT DEFAULT NULL",
         "ALTER TABLE conversations ADD COLUMN thinking_mode TEXT DEFAULT NULL"
      };
      for (int i = 0; i < 5; i++) {
         rc = sqlite3_exec(s_db.db, cols[i], NULL, NULL, &errmsg);
         if (rc != SQLITE_OK) {
            OLOG_INFO("auth_db: v11 migration note: %s", errmsg ? errmsg : "ok");
            sqlite3_free(errmsg);
            errmsg = NULL;
         }
      }
      OLOG_INFO("auth_db: added LLM settings columns to conversations (v11)");
   }

   /* v12 migration: images table for vision uploads (now superseded by v13) */
   if (current_version >= 1 && current_version < 12) {
      OLOG_INFO("auth_db: added images table for vision uploads (v12)");
   }

   /* v13 migration: add data BLOB column to images table
    * Since v12 images table didn't have the data column, we need to recreate it.
    * Drop existing table (likely empty) and let SCHEMA_SQL recreate with data column. */
   if (current_version == 12) {
      rc = sqlite3_exec(s_db.db, "DROP TABLE IF EXISTS images", NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_WARNING("auth_db: v13 migration - failed to drop images: %s", errmsg ? errmsg : "ok");
         sqlite3_free(errmsg);
         errmsg = NULL;
      }
      /* Recreate with data column (from SCHEMA_SQL) */
      const char *images_sql =
          "CREATE TABLE IF NOT EXISTS images ("
          "   id TEXT PRIMARY KEY,"
          "   user_id INTEGER NOT NULL,"
          "   mime_type TEXT NOT NULL,"
          "   size INTEGER NOT NULL,"
          "   data BLOB NOT NULL,"
          "   created_at INTEGER NOT NULL,"
          "   last_accessed INTEGER,"
          "   FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE"
          ");"
          "CREATE INDEX IF NOT EXISTS idx_images_user ON images(user_id);"
          "CREATE INDEX IF NOT EXISTS idx_images_created ON images(created_at);";
      rc = sqlite3_exec(s_db.db, images_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_ERROR("auth_db: v13 migration - failed to create images: %s", errmsg ? errmsg : "ok");
         sqlite3_free(errmsg);
         return AUTH_DB_FAILURE;
      }
      OLOG_INFO("auth_db: migrated images table to include BLOB storage (v13)");
   }

   /* v14 migration: add memory system tables
    * Creates memory_facts, memory_preferences, and memory_summaries tables */
   if (current_version >= 1 && current_version < 14) {
      const char *memory_sql =
          /* memory_facts table */
          "CREATE TABLE IF NOT EXISTS memory_facts ("
          "   id INTEGER PRIMARY KEY AUTOINCREMENT,"
          "   user_id INTEGER NOT NULL,"
          "   fact_text TEXT NOT NULL,"
          "   confidence REAL DEFAULT 1.0,"
          "   source TEXT DEFAULT 'inferred',"
          "   created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
          "   last_accessed INTEGER,"
          "   access_count INTEGER DEFAULT 0,"
          "   superseded_by INTEGER,"
          "   FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,"
          "   FOREIGN KEY (superseded_by) REFERENCES memory_facts(id) ON DELETE SET NULL"
          ");"
          "CREATE INDEX IF NOT EXISTS idx_memory_facts_user ON memory_facts(user_id);"
          "CREATE INDEX IF NOT EXISTS idx_memory_facts_confidence ON "
          "memory_facts(user_id, confidence DESC);"

          /* memory_preferences table */
          "CREATE TABLE IF NOT EXISTS memory_preferences ("
          "   id INTEGER PRIMARY KEY AUTOINCREMENT,"
          "   user_id INTEGER NOT NULL,"
          "   category TEXT NOT NULL,"
          "   value TEXT NOT NULL,"
          "   confidence REAL DEFAULT 0.5,"
          "   source TEXT DEFAULT 'inferred',"
          "   created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
          "   updated_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
          "   reinforcement_count INTEGER DEFAULT 1,"
          "   FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,"
          "   UNIQUE(user_id, category)"
          ");"

          /* memory_summaries table */
          "CREATE TABLE IF NOT EXISTS memory_summaries ("
          "   id INTEGER PRIMARY KEY AUTOINCREMENT,"
          "   user_id INTEGER NOT NULL,"
          "   session_id TEXT NOT NULL,"
          "   summary TEXT NOT NULL,"
          "   topics TEXT,"
          "   sentiment TEXT,"
          "   created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
          "   message_count INTEGER,"
          "   duration_seconds INTEGER,"
          "   consolidated INTEGER DEFAULT 0,"
          "   FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE"
          ");"
          "CREATE INDEX IF NOT EXISTS idx_memory_summaries_user ON "
          "memory_summaries(user_id, created_at DESC);";

      rc = sqlite3_exec(s_db.db, memory_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_ERROR("auth_db: v14 migration - failed to create memory tables: %s",
                    errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         return AUTH_DB_FAILURE;
      }
      OLOG_INFO("auth_db: added memory system tables (v14)");
   }

   /* v15 migration: add deduplication and extraction tracking
    * - normalized_hash for fast duplicate detection in memory_facts
    * - last_extracted_msg_count for incremental extraction in conversations */
   if (current_version >= 1 && current_version < 15) {
      const char *v15_sql =
          "ALTER TABLE memory_facts ADD COLUMN normalized_hash INTEGER DEFAULT 0;"
          "CREATE INDEX IF NOT EXISTS idx_memory_facts_hash ON memory_facts(user_id, "
          "normalized_hash);"
          "ALTER TABLE conversations ADD COLUMN last_extracted_msg_count INTEGER DEFAULT 0;";

      rc = sqlite3_exec(s_db.db, v15_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_ERROR("auth_db: v15 migration failed: %s", errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         return AUTH_DB_FAILURE;
      }
      OLOG_INFO("auth_db: added deduplication and extraction tracking (v15)");
   }

   /* v16 migration: add is_private flag to conversations for privacy mode */
   if (current_version >= 1 && current_version < 16) {
      const char *v16_sql = "ALTER TABLE conversations ADD COLUMN is_private INTEGER DEFAULT 0;";

      rc = sqlite3_exec(s_db.db, v16_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_ERROR("auth_db: v16 migration failed: %s", errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         return AUTH_DB_FAILURE;
      }
      OLOG_INFO("auth_db: added conversation privacy flag (v16)");
   }

   /* v17 migration: add origin column to conversations for voice/webui distinction */
   if (current_version >= 1 && current_version < 17) {
      const char *v17_sql = "ALTER TABLE conversations ADD COLUMN origin TEXT DEFAULT 'webui';";

      rc = sqlite3_exec(s_db.db, v17_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_ERROR("auth_db: v17 migration failed: %s", errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         return AUTH_DB_FAILURE;
      }
      OLOG_INFO("auth_db: added conversation origin column (v17)");
   }

   /* v18 migration: scheduler events table */
   if (current_version >= 1 && current_version < 18) {
      const char *v18_sql = "CREATE TABLE IF NOT EXISTS scheduled_events ("
                            "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
                            "  user_id INTEGER NOT NULL,"
                            "  event_type TEXT NOT NULL DEFAULT 'timer',"
                            "  status TEXT NOT NULL DEFAULT 'pending',"
                            "  name TEXT NOT NULL,"
                            "  message TEXT,"
                            "  fire_at INTEGER NOT NULL,"
                            "  created_at INTEGER NOT NULL,"
                            "  duration_sec INTEGER DEFAULT 0,"
                            "  snoozed_until INTEGER DEFAULT 0,"
                            "  recurrence TEXT DEFAULT 'once',"
                            "  recurrence_days TEXT,"
                            "  original_time TEXT,"
                            "  source_uuid TEXT,"
                            "  source_location TEXT,"
                            "  announce_all INTEGER DEFAULT 0,"
                            "  tool_name TEXT,"
                            "  tool_action TEXT,"
                            "  tool_value TEXT,"
                            "  fired_at INTEGER DEFAULT 0,"
                            "  snooze_count INTEGER DEFAULT 0,"
                            "  FOREIGN KEY (user_id) REFERENCES users(id)"
                            ");"
                            "CREATE INDEX IF NOT EXISTS idx_sched_status_fire "
                            "  ON scheduled_events(status, fire_at);"
                            "CREATE INDEX IF NOT EXISTS idx_sched_user "
                            "  ON scheduled_events(user_id, status);"
                            "CREATE INDEX IF NOT EXISTS idx_sched_user_name "
                            "  ON scheduled_events(user_id, status, name);"
                            "CREATE INDEX IF NOT EXISTS idx_sched_source "
                            "  ON scheduled_events(source_uuid);";

      rc = sqlite3_exec(s_db.db, v18_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_ERROR("auth_db: v18 migration failed: %s", errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         return AUTH_DB_FAILURE;
      }
      OLOG_INFO("auth_db: added scheduled_events table (v18)");
   }

   /* v19 migration: semantic memory embeddings + entity/relation tables */
   if (current_version >= 1 && current_version < 19) {
      const char *v19_sql =
          /* Add embedding columns to existing memory_facts table */
          "ALTER TABLE memory_facts ADD COLUMN embedding BLOB DEFAULT NULL;"
          "ALTER TABLE memory_facts ADD COLUMN embedding_norm REAL DEFAULT NULL;"

          /* Entity table (populated in Phase S4, created now for schema stability) */
          "CREATE TABLE IF NOT EXISTS memory_entities ("
          "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
          "  user_id INTEGER NOT NULL,"
          "  name TEXT NOT NULL,"
          "  entity_type TEXT NOT NULL,"
          "  canonical_name TEXT NOT NULL,"
          "  embedding BLOB DEFAULT NULL,"
          "  embedding_norm REAL DEFAULT NULL,"
          "  first_seen INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
          "  last_seen INTEGER,"
          "  mention_count INTEGER DEFAULT 1,"
          "  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,"
          "  UNIQUE(user_id, canonical_name)"
          ");"
          "CREATE INDEX IF NOT EXISTS idx_memory_entities_user "
          "  ON memory_entities(user_id);"

          /* Relation triples (populated in Phase S4, created now) */
          "CREATE TABLE IF NOT EXISTS memory_relations ("
          "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
          "  user_id INTEGER NOT NULL,"
          "  subject_entity_id INTEGER NOT NULL,"
          "  relation TEXT NOT NULL,"
          "  object_entity_id INTEGER,"
          "  object_value TEXT,"
          "  fact_id INTEGER,"
          "  confidence REAL DEFAULT 0.8,"
          "  created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
          "  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,"
          "  FOREIGN KEY (subject_entity_id) REFERENCES memory_entities(id) ON DELETE CASCADE,"
          "  FOREIGN KEY (object_entity_id) REFERENCES memory_entities(id) ON DELETE SET NULL,"
          "  FOREIGN KEY (fact_id) REFERENCES memory_facts(id) ON DELETE SET NULL"
          ");"
          "CREATE INDEX IF NOT EXISTS idx_memory_relations_subject "
          "  ON memory_relations(subject_entity_id);"
          "CREATE INDEX IF NOT EXISTS idx_memory_relations_object "
          "  ON memory_relations(object_entity_id);"
          "CREATE INDEX IF NOT EXISTS idx_memory_relations_user "
          "  ON memory_relations(user_id);";

      rc = sqlite3_exec(s_db.db, v19_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_ERROR("auth_db: v19 migration failed: %s", errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         return AUTH_DB_FAILURE;
      }
      OLOG_INFO("auth_db: added embedding columns and entity/relation tables (v19)");
   }

   /* v20 migration: satellite_mappings table for persistent satellite-to-user mappings */
   if (current_version >= 1 && current_version < 20) {
      const char *v20_sql =
          "CREATE TABLE IF NOT EXISTS satellite_mappings ("
          "  uuid TEXT PRIMARY KEY,"
          "  name TEXT NOT NULL DEFAULT '',"
          "  location TEXT NOT NULL DEFAULT '',"
          "  ha_area TEXT DEFAULT '',"
          "  user_id INTEGER DEFAULT NULL,"
          "  tier INTEGER DEFAULT 1,"
          "  last_seen INTEGER DEFAULT 0,"
          "  created_at INTEGER NOT NULL,"
          "  enabled INTEGER DEFAULT 1,"
          "  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE SET NULL"
          ");"
          "CREATE INDEX IF NOT EXISTS idx_satellite_user ON satellite_mappings(user_id);";

      rc = sqlite3_exec(s_db.db, v20_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_ERROR("auth_db: v20 migration failed: %s", errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         return AUTH_DB_FAILURE;
      }
      OLOG_INFO("auth_db: added satellite_mappings table (v20)");
   }

   /* v21 migration: fix satellite_mappings FK (DEFAULT 0 -> DEFAULT NULL, SET NULL) */
   if (current_version >= 20 && current_version < 21) {
      const char *v21_sql =
          "BEGIN TRANSACTION;"
          "CREATE TABLE IF NOT EXISTS satellite_mappings_new ("
          "  uuid TEXT PRIMARY KEY,"
          "  name TEXT NOT NULL DEFAULT '',"
          "  location TEXT NOT NULL DEFAULT '',"
          "  ha_area TEXT DEFAULT '',"
          "  user_id INTEGER DEFAULT NULL,"
          "  tier INTEGER DEFAULT 1,"
          "  last_seen INTEGER DEFAULT 0,"
          "  created_at INTEGER NOT NULL,"
          "  enabled INTEGER DEFAULT 1,"
          "  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE SET NULL"
          ");"
          "INSERT INTO satellite_mappings_new SELECT uuid, name, location, ha_area,"
          "  CASE WHEN user_id = 0 THEN NULL ELSE user_id END,"
          "  tier, last_seen, created_at, enabled FROM satellite_mappings;"
          "DROP TABLE satellite_mappings;"
          "ALTER TABLE satellite_mappings_new RENAME TO satellite_mappings;"
          "CREATE INDEX IF NOT EXISTS idx_satellite_user ON satellite_mappings(user_id);"
          "COMMIT;";

      rc = sqlite3_exec(s_db.db, v21_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_ERROR("auth_db: v21 migration failed: %s", errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         return AUTH_DB_FAILURE;
      }
      OLOG_INFO("auth_db: fixed satellite_mappings FK constraints (v21)");
   }

   /* v22 migration: documents and document_chunks tables for RAG search */
   if (current_version >= 1 && current_version < 22) {
      const char *v22_sql =
          "CREATE TABLE IF NOT EXISTS documents ("
          "  id INTEGER PRIMARY KEY,"
          "  user_id INTEGER,"
          "  filename TEXT NOT NULL,"
          "  filepath TEXT NOT NULL,"
          "  filetype TEXT NOT NULL,"
          "  file_hash TEXT NOT NULL,"
          "  num_chunks INTEGER NOT NULL,"
          "  is_global INTEGER DEFAULT 0,"
          "  created_at INTEGER NOT NULL,"
          "  FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE"
          ");"
          "CREATE TABLE IF NOT EXISTS document_chunks ("
          "  id INTEGER PRIMARY KEY,"
          "  document_id INTEGER NOT NULL,"
          "  chunk_index INTEGER NOT NULL,"
          "  text TEXT NOT NULL,"
          "  embedding BLOB NOT NULL,"
          "  embedding_norm REAL NOT NULL,"
          "  FOREIGN KEY(document_id) REFERENCES documents(id) ON DELETE CASCADE"
          ");"
          "CREATE INDEX IF NOT EXISTS idx_doc_chunks_doc ON document_chunks(document_id);"
          "CREATE INDEX IF NOT EXISTS idx_documents_user ON documents(user_id);"
          "CREATE INDEX IF NOT EXISTS idx_documents_hash ON documents(file_hash);";

      rc = sqlite3_exec(s_db.db, v22_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_ERROR("auth_db: v22 migration failed: %s", errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         return AUTH_DB_FAILURE;
      }
      OLOG_INFO("auth_db: added documents and document_chunks tables (v22)");
   }

   /* v23 migration: calendar tables for CalDAV integration */
   if (current_version >= 1 && current_version < 23) {
      const char *v23_sql =
          "CREATE TABLE IF NOT EXISTS calendar_accounts ("
          "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
          "  user_id INTEGER NOT NULL,"
          "  name TEXT NOT NULL,"
          "  caldav_url TEXT NOT NULL,"
          "  username TEXT NOT NULL,"
          "  encrypted_password BLOB NOT NULL,"
          "  auth_type TEXT DEFAULT 'basic',"
          "  principal_url TEXT DEFAULT '',"
          "  calendar_home_url TEXT DEFAULT '',"
          "  enabled INTEGER DEFAULT 1,"
          "  read_only INTEGER DEFAULT 0,"
          "  last_sync INTEGER DEFAULT 0,"
          "  sync_interval_sec INTEGER DEFAULT 900,"
          "  created_at INTEGER NOT NULL,"
          "  FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE"
          ");"
          "CREATE TABLE IF NOT EXISTS calendar_calendars ("
          "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
          "  account_id INTEGER NOT NULL,"
          "  caldav_path TEXT NOT NULL,"
          "  display_name TEXT DEFAULT '',"
          "  color TEXT DEFAULT '',"
          "  is_active INTEGER DEFAULT 1,"
          "  ctag TEXT DEFAULT '',"
          "  created_at INTEGER NOT NULL,"
          "  FOREIGN KEY(account_id) REFERENCES calendar_accounts(id) ON DELETE CASCADE"
          ");"
          "CREATE TABLE IF NOT EXISTS calendar_events ("
          "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
          "  calendar_id INTEGER NOT NULL,"
          "  uid TEXT NOT NULL,"
          "  etag TEXT DEFAULT '',"
          "  summary TEXT DEFAULT '',"
          "  description TEXT DEFAULT '',"
          "  location TEXT DEFAULT '',"
          "  dtstart INTEGER DEFAULT 0,"
          "  dtend INTEGER DEFAULT 0,"
          "  duration_sec INTEGER DEFAULT 0,"
          "  all_day INTEGER DEFAULT 0,"
          "  dtstart_date TEXT DEFAULT '',"
          "  dtend_date TEXT DEFAULT '',"
          "  rrule TEXT DEFAULT '',"
          "  raw_ical TEXT,"
          "  last_synced INTEGER DEFAULT 0,"
          "  FOREIGN KEY(calendar_id) REFERENCES calendar_calendars(id) ON DELETE CASCADE"
          ");"
          "CREATE UNIQUE INDEX IF NOT EXISTS idx_cal_events_uid "
          "  ON calendar_events(calendar_id, uid);"
          "CREATE TABLE IF NOT EXISTS calendar_occurrences ("
          "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
          "  event_id INTEGER NOT NULL,"
          "  dtstart INTEGER DEFAULT 0,"
          "  dtend INTEGER DEFAULT 0,"
          "  all_day INTEGER DEFAULT 0,"
          "  dtstart_date TEXT DEFAULT '',"
          "  dtend_date TEXT DEFAULT '',"
          "  summary TEXT DEFAULT '',"
          "  location TEXT DEFAULT '',"
          "  is_override INTEGER DEFAULT 0,"
          "  is_cancelled INTEGER DEFAULT 0,"
          "  recurrence_id TEXT DEFAULT '',"
          "  FOREIGN KEY(event_id) REFERENCES calendar_events(id) ON DELETE CASCADE"
          ");"
          "CREATE INDEX IF NOT EXISTS idx_cal_occ_event ON calendar_occurrences(event_id);"
          "CREATE INDEX IF NOT EXISTS idx_cal_occ_time ON calendar_occurrences(dtstart, dtend);"
          "CREATE INDEX IF NOT EXISTS idx_cal_occ_date ON calendar_occurrences(dtstart_date);"
          "CREATE INDEX IF NOT EXISTS idx_cal_acct_user ON calendar_accounts(user_id);";

      rc = sqlite3_exec(s_db.db, v23_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_ERROR("auth_db: v23 migration failed: %s", errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         return AUTH_DB_FAILURE;
      }
      OLOG_INFO("auth_db: added calendar tables (v23)");
   }

   /* v24 migration: add read_only flag to calendar_accounts */
   if (current_version >= 23 && current_version < 24) {
      const char *v24_sql = "ALTER TABLE calendar_accounts ADD COLUMN read_only INTEGER DEFAULT 0;";
      rc = sqlite3_exec(s_db.db, v24_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_ERROR("auth_db: v24 migration failed: %s", errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         return AUTH_DB_FAILURE;
      }
      OLOG_INFO("auth_db: added calendar read_only column (v24)");
   }

   /* v25 migration: OAuth token storage + calendar account OAuth support */
   if (current_version >= 1 && current_version < 25) {
      const char *v25_sql = "CREATE TABLE IF NOT EXISTS oauth_tokens ("
                            "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
                            "  user_id INTEGER NOT NULL,"
                            "  provider TEXT NOT NULL,"
                            "  account_key TEXT NOT NULL,"
                            "  encrypted_data BLOB NOT NULL,"
                            "  encrypted_data_len INTEGER NOT NULL,"
                            "  scopes TEXT DEFAULT '',"
                            "  created_at INTEGER NOT NULL,"
                            "  updated_at INTEGER NOT NULL,"
                            "  FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE,"
                            "  UNIQUE(user_id, provider, account_key)"
                            ");"
                            "CREATE INDEX IF NOT EXISTS idx_oauth_user_provider "
                            "  ON oauth_tokens(user_id, provider);";

      rc = sqlite3_exec(s_db.db, v25_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_ERROR("auth_db: v25 migration (oauth_tokens) failed: %s",
                    errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         return AUTH_DB_FAILURE;
      }

      /* Add oauth_account_key column to calendar_accounts */
      rc = sqlite3_exec(
          s_db.db, "ALTER TABLE calendar_accounts ADD COLUMN oauth_account_key TEXT DEFAULT ''",
          NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_INFO("auth_db: v25 migration note (oauth_account_key): %s", errmsg ? errmsg : "ok");
         sqlite3_free(errmsg);
         errmsg = NULL;
      }

      OLOG_INFO("auth_db: added oauth_tokens table and calendar OAuth support (v25)");
   }

   /* v26 migration: contacts table + email_accounts table */
   if (current_version >= 1 && current_version < 26) {
      const char *v26_sql =
          "CREATE TABLE IF NOT EXISTS contacts ("
          "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
          "  user_id INTEGER NOT NULL,"
          "  entity_id INTEGER NOT NULL,"
          "  field_type TEXT NOT NULL,"
          "  value TEXT NOT NULL,"
          "  label TEXT DEFAULT '',"
          "  created_at INTEGER NOT NULL,"
          "  FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE,"
          "  FOREIGN KEY(entity_id) REFERENCES memory_entities(id) ON DELETE CASCADE"
          ");"
          "CREATE INDEX IF NOT EXISTS idx_contacts_entity ON contacts(entity_id);"
          "CREATE INDEX IF NOT EXISTS idx_contacts_user_type ON contacts(user_id, field_type);"
          "CREATE TABLE IF NOT EXISTS email_accounts ("
          "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
          "  user_id INTEGER NOT NULL,"
          "  name TEXT NOT NULL,"
          "  imap_server TEXT NOT NULL,"
          "  imap_port INTEGER DEFAULT 993,"
          "  imap_ssl INTEGER DEFAULT 1,"
          "  smtp_server TEXT NOT NULL,"
          "  smtp_port INTEGER DEFAULT 465,"
          "  smtp_ssl INTEGER DEFAULT 1,"
          "  username TEXT NOT NULL,"
          "  display_name TEXT DEFAULT '',"
          "  encrypted_password BLOB,"
          "  encrypted_password_len INTEGER DEFAULT 0,"
          "  auth_type TEXT DEFAULT 'app_password',"
          "  oauth_account_key TEXT DEFAULT '',"
          "  enabled INTEGER DEFAULT 1,"
          "  read_only INTEGER DEFAULT 0,"
          "  max_recent INTEGER DEFAULT 10,"
          "  max_body_chars INTEGER DEFAULT 4000,"
          "  created_at INTEGER NOT NULL,"
          "  FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE"
          ");"
          "CREATE INDEX IF NOT EXISTS idx_email_acct_user ON email_accounts(user_id);";

      rc = sqlite3_exec(s_db.db, v26_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_ERROR("auth_db: v26 migration (contacts + email_accounts) failed: %s",
                    errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         return AUTH_DB_FAILURE;
      }

      OLOG_INFO("auth_db: added contacts and email_accounts tables (v26)");
   }

   /* v27 migration: add title_locked column to conversations for auto-title feature */
   if (current_version >= 4 && current_version < 27) {
      rc = sqlite3_exec(s_db.db,
                        "ALTER TABLE conversations ADD COLUMN title_locked INTEGER DEFAULT 0", NULL,
                        NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_INFO("auth_db: v27 migration note (title_locked): %s", errmsg ? errmsg : "ok");
         sqlite3_free(errmsg);
         errmsg = NULL;
      } else {
         OLOG_INFO("auth_db: added title_locked column to conversations (v27)");
      }
   }

   /* v28 migration: add source_client_type to scheduled_events for notification routing */
   if (current_version >= 18 && current_version < 28) {
      rc = sqlite3_exec(
          s_db.db, "ALTER TABLE scheduled_events ADD COLUMN source_client_type INTEGER DEFAULT 0",
          NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_INFO("auth_db: v28 migration note (source_client_type): %s", errmsg ? errmsg : "ok");
         sqlite3_free(errmsg);
         errmsg = NULL;
      } else {
         OLOG_INFO("auth_db: added source_client_type to scheduled_events (v28)");
      }
   }

   /* v29 migration: phone call and SMS log tables */
   if (current_version >= 1 && current_version < 29) {
      const char *v29_sql = "CREATE TABLE IF NOT EXISTS phone_call_log ("
                            "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
                            "  user_id INTEGER NOT NULL,"
                            "  direction INTEGER NOT NULL,"
                            "  number TEXT NOT NULL,"
                            "  contact_name TEXT DEFAULT '',"
                            "  duration_sec INTEGER DEFAULT 0,"
                            "  timestamp INTEGER NOT NULL,"
                            "  status INTEGER NOT NULL"
                            ");"
                            "CREATE INDEX IF NOT EXISTS idx_phone_call_user_ts "
                            "  ON phone_call_log(user_id, timestamp DESC);"
                            "CREATE TABLE IF NOT EXISTS phone_sms_log ("
                            "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
                            "  user_id INTEGER NOT NULL,"
                            "  direction INTEGER NOT NULL,"
                            "  number TEXT NOT NULL,"
                            "  contact_name TEXT DEFAULT '',"
                            "  body TEXT NOT NULL,"
                            "  timestamp INTEGER NOT NULL,"
                            "  read INTEGER DEFAULT 0"
                            ");"
                            "CREATE INDEX IF NOT EXISTS idx_phone_sms_user_ts "
                            "  ON phone_sms_log(user_id, timestamp DESC);"
                            "CREATE INDEX IF NOT EXISTS idx_phone_sms_unread "
                            "  ON phone_sms_log(user_id, read) WHERE read = 0;";

      rc = sqlite3_exec(s_db.db, v29_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_ERROR("auth_db: v29 migration (phone tables) failed: %s",
                    errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         errmsg = NULL;
      } else {
         OLOG_INFO("auth_db: added phone_call_log and phone_sms_log tables (v29)");
      }
   }

   /* v30 migration: image store BLOB → filesystem + phone_sms_log image_id column
    * Export image BLOBs to <data_dir>/images/ files, rebuild table without BLOB column.
    * Also add image_id column to phone_sms_log for MMS attachment references. */
   if (current_version >= 12 && current_version < 30) {
      /* Derive images directory from db_path parent */
      char images_dir[PATH_MAX];
      char db_path_copy[PATH_MAX];
      strncpy(db_path_copy, db_path, sizeof(db_path_copy) - 1);
      db_path_copy[sizeof(db_path_copy) - 1] = '\0';
      char *parent = dirname(db_path_copy);
      snprintf(images_dir, sizeof(images_dir), "%s/images", parent);

      /* Create images directory */
      if (mkdir(images_dir, 0750) != 0 && errno != EEXIST) {
         OLOG_ERROR("auth_db: v30 migration - failed to create %s: %s", images_dir,
                    strerror(errno));
         return AUTH_DB_FAILURE;
      }

      /* Export BLOBs to files */
      sqlite3_stmt *export_stmt = NULL;
      rc = sqlite3_prepare_v2(s_db.db,
                              "SELECT id, mime_type, data FROM images WHERE data IS NOT NULL", -1,
                              &export_stmt, NULL);
      if (rc != SQLITE_OK) {
         OLOG_ERROR("auth_db: v30 migration - prepare export failed: %s", sqlite3_errmsg(s_db.db));
         return AUTH_DB_FAILURE;
      }

      int exported = 0;
      int export_failed = 0;
      while (sqlite3_step(export_stmt) == SQLITE_ROW) {
         const char *id = (const char *)sqlite3_column_text(export_stmt, 0);
         const char *mime = (const char *)sqlite3_column_text(export_stmt, 1);
         const void *blob = sqlite3_column_blob(export_stmt, 2);
         int blob_size = sqlite3_column_bytes(export_stmt, 2);

         if (!id || !blob || blob_size <= 0)
            continue;

         /* Determine file extension from MIME */
         const char *ext = "bin";
         if (mime) {
            if (strcmp(mime, "image/jpeg") == 0)
               ext = "jpg";
            else if (strcmp(mime, "image/png") == 0)
               ext = "png";
            else if (strcmp(mime, "image/gif") == 0)
               ext = "gif";
            else if (strcmp(mime, "image/webp") == 0)
               ext = "webp";
         }

         /* Write to tmp file, fsync, rename for atomicity */
         char filepath[PATH_MAX + 32];
         char tmppath[PATH_MAX + 32];
         snprintf(filepath, sizeof(filepath), "%s/%s.%s", images_dir, id, ext);
         snprintf(tmppath, sizeof(tmppath), "%s/.%s.%s.tmp", images_dir, id, ext);

         int fd = open(tmppath, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0640);
         if (fd < 0) {
            OLOG_WARNING("auth_db: v30 migration - failed to create %s: %s", tmppath,
                         strerror(errno));
            export_failed++;
            continue;
         }

         const unsigned char *wp = (const unsigned char *)blob;
         size_t remaining = (size_t)blob_size;
         bool write_ok = true;
         while (remaining > 0) {
            ssize_t written = write(fd, wp, remaining);
            if (written < 0) {
               if (errno == EINTR)
                  continue;
               OLOG_WARNING("auth_db: v30 migration - write failed for %s: %s", id,
                            strerror(errno));
               write_ok = false;
               break;
            }
            wp += written;
            remaining -= (size_t)written;
         }
         if (!write_ok) {
            close(fd);
            unlink(tmppath);
            export_failed++;
            continue;
         }

         fsync(fd);
         close(fd);

         if (rename(tmppath, filepath) != 0) {
            OLOG_WARNING("auth_db: v30 migration - rename failed for %s: %s", id, strerror(errno));
            unlink(tmppath);
            export_failed++;
            continue;
         }

         exported++;
      }
      sqlite3_finalize(export_stmt);

      if (export_failed > 0) {
         OLOG_ERROR("auth_db: v30 migration - %d/%d images failed to export", export_failed,
                    exported + export_failed);
         return AUTH_DB_FAILURE;
      }

      /* Rebuild images table without BLOB column (transactional) */
      const char *v30_images_sql =
          "BEGIN TRANSACTION;"
          "DROP TABLE IF EXISTS images_new;"
          "CREATE TABLE images_new ("
          "   id TEXT PRIMARY KEY,"
          "   user_id INTEGER NOT NULL,"
          "   source INTEGER NOT NULL DEFAULT 0,"
          "   retention_policy INTEGER NOT NULL DEFAULT 0,"
          "   mime_type TEXT NOT NULL,"
          "   size INTEGER NOT NULL,"
          "   filename TEXT NOT NULL,"
          "   created_at INTEGER NOT NULL,"
          "   last_accessed INTEGER,"
          "   FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE"
          ");"
          "INSERT INTO images_new (id, user_id, source, retention_policy, mime_type, size, "
          "filename, created_at, last_accessed) "
          "SELECT id, user_id, 0, 0, mime_type, size, "
          "id || '.' || CASE mime_type "
          "  WHEN 'image/jpeg' THEN 'jpg' "
          "  WHEN 'image/png' THEN 'png' "
          "  WHEN 'image/gif' THEN 'gif' "
          "  WHEN 'image/webp' THEN 'webp' "
          "  ELSE 'bin' END, "
          "created_at, last_accessed FROM images;"
          "DROP TABLE images;"
          "ALTER TABLE images_new RENAME TO images;"
          "CREATE INDEX IF NOT EXISTS idx_images_user ON images(user_id);"
          "CREATE INDEX IF NOT EXISTS idx_images_created ON images(created_at);"
          "CREATE INDEX IF NOT EXISTS idx_images_retention ON images(retention_policy);"
          "COMMIT;";

      rc = sqlite3_exec(s_db.db, v30_images_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_ERROR("auth_db: v30 migration (images table rebuild) failed: %s",
                    errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         sqlite3_exec(s_db.db, "ROLLBACK;", NULL, NULL, NULL);
         return AUTH_DB_FAILURE;
      }

      OLOG_INFO("auth_db: migrated %d images from BLOB to filesystem (v30)", exported);
   }

   /* v30 migration: add image_id to phone_sms_log (for MMS attachments) */
   if (current_version >= 29 && current_version < 30) {
      rc = sqlite3_exec(s_db.db, "ALTER TABLE phone_sms_log ADD COLUMN image_id TEXT DEFAULT NULL",
                        NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_WARNING("auth_db: v30 migration (sms image_id) failed: %s",
                      errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         errmsg = NULL;
      } else {
         OLOG_INFO("auth_db: added image_id column to phone_sms_log (v30)");
      }
   }

   /* v31 migration: add photo_id to memory_entities for contact photos */
   if (current_version >= 19 && current_version < 31) {
      rc = sqlite3_exec(s_db.db,
                        "ALTER TABLE memory_entities ADD COLUMN photo_id TEXT DEFAULT NULL", NULL,
                        NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_WARNING("auth_db: v31 migration (entity photo_id) failed: %s",
                      errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         errmsg = NULL;
      } else {
         OLOG_INFO("auth_db: added photo_id column to memory_entities (v31)");
      }
   }

   /* v32 migration: missed_notifications table for offline-user notification queue */
   if (current_version >= 1 && current_version < 32) {
      const char *v32_sql = "CREATE TABLE IF NOT EXISTS missed_notifications ("
                            "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
                            "  user_id INTEGER NOT NULL,"
                            "  event_id INTEGER NOT NULL,"
                            "  event_type TEXT NOT NULL,"
                            "  status TEXT NOT NULL,"
                            "  name TEXT NOT NULL,"
                            "  message TEXT,"
                            "  fire_at INTEGER NOT NULL,"
                            "  conversation_id INTEGER DEFAULT 0,"
                            "  created_at INTEGER NOT NULL,"
                            "  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE"
                            ");"
                            "CREATE INDEX IF NOT EXISTS idx_missed_notif_user "
                            "  ON missed_notifications(user_id, created_at);";
      rc = sqlite3_exec(s_db.db, v32_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_ERROR("auth_db: v32 migration (missed_notifications) failed: %s",
                    errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         return AUTH_DB_FAILURE;
      }
      OLOG_INFO("auth_db: added missed_notifications table (v32)");
   }

   /* v33 migration: temporal validity columns on memory_relations.
    * NULL = open-ended (no bound).  "currently true" predicate:
    *   valid_to IS NULL OR valid_to > strftime('%s','now')
    * Future relation-decay implementations should skip rows where valid_to is set
    * and in the past — those are historical facts, not stale beliefs. */
   if (current_version >= 19 && current_version < 33) {
      const char *v33_sql = "ALTER TABLE memory_relations ADD COLUMN valid_from INTEGER "
                            "  DEFAULT NULL;"
                            "ALTER TABLE memory_relations ADD COLUMN valid_to INTEGER "
                            "  DEFAULT NULL;"
                            "CREATE INDEX IF NOT EXISTS idx_memory_relations_user_validity "
                            "  ON memory_relations(user_id, valid_to);"
                            "CREATE INDEX IF NOT EXISTS idx_memory_relations_subject_open "
                            "  ON memory_relations(subject_entity_id, relation) "
                            "  WHERE valid_to IS NULL;";
      rc = sqlite3_exec(s_db.db, v33_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         /* Column may already exist if a previous migration partially ran — log and continue. */
         OLOG_WARNING("auth_db: v33 migration (memory_relations validity) returned: %s",
                      errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         errmsg = NULL;
      } else {
         OLOG_INFO("auth_db: added valid_from/valid_to to memory_relations (v33)");
      }
   }

   /* v34 migration: fact category column + per-user backfill gate.
    * categories_backfilled_at = 0 means embedding-centroid classification has not yet run
    * for that user; memory_embeddings_start_backfill() picks it up on next session. */
   if (current_version >= 1 && current_version < 34) {
      const char *v34_sql = "ALTER TABLE memory_facts ADD COLUMN category TEXT NOT NULL "
                            "  DEFAULT 'general';"
                            "ALTER TABLE users ADD COLUMN categories_backfilled_at INTEGER "
                            "  DEFAULT 0;"
                            "CREATE INDEX IF NOT EXISTS idx_memory_facts_user_category "
                            "  ON memory_facts(user_id, category);";
      rc = sqlite3_exec(s_db.db, v34_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_WARNING("auth_db: v34 migration (fact category) returned: %s",
                      errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         errmsg = NULL;
      } else {
         OLOG_INFO("auth_db: added category column + backfill gate (v34)");
      }
   }

   /* v35 migration: per-chunk created_at for temporal-query scoring.  0 = unknown
    * (chunk gets no proximity boost).  Backfill from documents.created_at would
    * be a follow-up — for v1, only chunks ingested after this migration get a
    * timestamp; older chunks default to 0 and behave as before. */
   if (current_version >= 22 && current_version < 35) {
      const char *v35_sql = "ALTER TABLE document_chunks ADD COLUMN created_at INTEGER "
                            "  NOT NULL DEFAULT 0;";
      rc = sqlite3_exec(s_db.db, v35_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_WARNING("auth_db: v35 migration (chunk created_at) returned: %s",
                      errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         errmsg = NULL;
      } else {
         OLOG_INFO("auth_db: added created_at to document_chunks (v35)");
      }
   }

   /* v36 migration: per-conversation reasoning_effort lock.  Without this column
    * the locked-settings restore on page refresh forgets the user's chosen
    * effort and the dropdown snaps back to the global default ("low").
    *
    * No lower bound on current_version: a DB at v10 will run the v11 block
    * above (which adds the conversations LLM-lock columns) AND this v36 block
    * in the same startup. `current_version` is captured once and not bumped
    * between migration blocks, so a `>= 11` guard here would incorrectly skip
    * the column add on v10-or-earlier DBs. ALTER TABLE errors (e.g. if the
    * column already exists on a concurrent path) are logged and swallowed, so
    * the migration is idempotent. */
   if (current_version < 36) {
      const char *v36_sql =
          "ALTER TABLE conversations ADD COLUMN reasoning_effort TEXT DEFAULT NULL;";
      rc = sqlite3_exec(s_db.db, v36_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_WARNING("auth_db: v36 migration (reasoning_effort) returned: %s",
                      errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         errmsg = NULL;
      } else {
         OLOG_INFO("auth_db: added reasoning_effort to conversations (v36)");
      }
   }

   /* v37 migration: backfill document_chunks.created_at from parent document.
    * Legacy chunks (ingested before v35 added created_at) have created_at = 0,
    * which forfeits temporal-query scoring.  Inherit the parent document's
    * created_at as a reasonable proxy.  Idempotent (WHERE created_at = 0).
    * Lower bound >= 35: the created_at column only exists from v35 onward. */
   if (current_version >= 35 && current_version < 37) {
      const char *v37_sql = "UPDATE document_chunks SET created_at = "
                            "(SELECT d.created_at FROM documents d "
                            "WHERE d.id = document_chunks.document_id) "
                            "WHERE created_at = 0;";
      rc = sqlite3_exec(s_db.db, v37_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_WARNING("auth_db: v37 migration (chunk created_at backfill): %s",
                      errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         errmsg = NULL;
      } else {
         int affected = sqlite3_changes(s_db.db);
         OLOG_INFO("auth_db: backfilled created_at on %d document chunks (v37)", affected);
      }
   }

   /* Create indexes that depend on migration-added columns.
    * Runs for both fresh installs and migrations — must come after all migrations. */
   rc = sqlite3_exec(s_db.db,
                     "CREATE INDEX IF NOT EXISTS idx_conversations_continued "
                     "ON conversations(continued_from)",
                     NULL, NULL, &errmsg);
   if (rc != SQLITE_OK) {
      OLOG_WARNING("auth_db: could not create continuation index: %s", errmsg ? errmsg : "ok");
      sqlite3_free(errmsg);
      errmsg = NULL;
   }

   rc = sqlite3_exec(s_db.db,
                     "CREATE INDEX IF NOT EXISTS idx_images_retention "
                     "ON images(retention_policy)",
                     NULL, NULL, &errmsg);
   if (rc != SQLITE_OK) {
      OLOG_WARNING("auth_db: could not create retention index: %s", errmsg ? errmsg : "ok");
      sqlite3_free(errmsg);
      errmsg = NULL;
   }

   /* Indexes on migration-added columns (v33/v34).  Must run here rather than
    * in SCHEMA_SQL because on an existing pre-migration DB, CREATE TABLE IF
    * NOT EXISTS is a no-op, so the new columns don't exist until migrations
    * run.  Migrations also create these indexes but only fire on DBs with
    * current_version >= 1 — fresh installs (version 0) skip all migrations
    * and reach this block instead. */
   rc = sqlite3_exec(s_db.db,
                     "CREATE INDEX IF NOT EXISTS idx_memory_facts_user_category "
                     "ON memory_facts(user_id, category);"
                     "CREATE INDEX IF NOT EXISTS idx_memory_relations_user_validity "
                     "ON memory_relations(user_id, valid_to);"
                     "CREATE INDEX IF NOT EXISTS idx_memory_relations_subject_open "
                     "ON memory_relations(subject_entity_id, relation) "
                     "WHERE valid_to IS NULL",
                     NULL, NULL, &errmsg);
   if (rc != SQLITE_OK) {
      OLOG_WARNING("auth_db: could not create memory v33/v34 indexes: %s", errmsg ? errmsg : "ok");
      sqlite3_free(errmsg);
      errmsg = NULL;
   }

   /* Log migration if upgrading from an older version */
   if (current_version > 0 && current_version < AUTH_DB_SCHEMA_VERSION) {
      OLOG_INFO("auth_db: migrated schema from v%d to v%d", current_version,
                AUTH_DB_SCHEMA_VERSION);
   } else if (current_version == 0) {
      OLOG_INFO("auth_db: created schema v%d", AUTH_DB_SCHEMA_VERSION);
   }

   /* Only update schema_version if we actually migrated or created fresh.
    * Never downgrade — prevents old code from corrupting a newer DB. */
   if (current_version < AUTH_DB_SCHEMA_VERSION) {
      rc = sqlite3_exec(s_db.db, "DELETE FROM schema_version", NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_WARNING("auth_db: failed to clear schema_version: %s", errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         errmsg = NULL;
      }
      rc = sqlite3_exec(s_db.db,
                        "INSERT INTO schema_version (version) VALUES (" STRINGIFY(
                            AUTH_DB_SCHEMA_VERSION) ")",
                        NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_ERROR("auth_db: failed to set schema version: %s", errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         return AUTH_DB_FAILURE;
      }
   } else if (current_version > AUTH_DB_SCHEMA_VERSION) {
      OLOG_WARNING("auth_db: database is newer (v%d) than code (v%d) — not downgrading",
                   current_version, AUTH_DB_SCHEMA_VERSION);
   }

   return AUTH_DB_SUCCESS;
}

/* =============================================================================
 * Prepared Statement Management
 * ============================================================================= */

static int prepare_statements(void) {
   int rc;

   /* User statements */
   rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO users (username, password_hash, is_admin, created_at) VALUES (?, ?, ?, ?)", -1,
       &s_db.stmt_create_user, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare create_user failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, username, password_hash, is_admin, created_at, "
       "last_login, failed_attempts, lockout_until FROM users WHERE username = ?",
       -1, &s_db.stmt_get_user, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare get_user failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "SELECT COUNT(*) FROM users", -1, &s_db.stmt_count_users, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare count_users failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db, "UPDATE users SET failed_attempts = failed_attempts + 1 WHERE username = ?", -1,
       &s_db.stmt_inc_failed_attempts, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare inc_failed_attempts failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "UPDATE users SET failed_attempts = 0 WHERE username = ?", -1,
                           &s_db.stmt_reset_failed_attempts, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare reset_failed_attempts failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "UPDATE users SET last_login = ? WHERE username = ?", -1,
                           &s_db.stmt_update_last_login, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare update_last_login failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "UPDATE users SET lockout_until = ? WHERE username = ?", -1,
                           &s_db.stmt_set_lockout, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare set_lockout failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Session statements */
   rc = sqlite3_prepare_v2(s_db.db,
                           "INSERT INTO sessions (token, user_id, created_at, last_activity, "
                           "expires_at, ip_address, user_agent) VALUES (?, ?, ?, ?, ?, ?, ?)",
                           -1, &s_db.stmt_create_session, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare create_session failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT s.token, s.user_id, u.username, u.is_admin, s.created_at, "
                           "s.last_activity, s.expires_at, s.ip_address, s.user_agent "
                           "FROM sessions s JOIN users u ON s.user_id = u.id WHERE s.token = ?",
                           -1, &s_db.stmt_get_session, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare get_session failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "UPDATE sessions SET last_activity = ? WHERE token = ?", -1,
                           &s_db.stmt_update_session_activity, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare update_session_activity failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM sessions WHERE token = ?", -1,
                           &s_db.stmt_delete_session, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare delete_session failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM sessions WHERE user_id = ?", -1,
                           &s_db.stmt_delete_user_sessions, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare delete_user_sessions failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "DELETE FROM sessions WHERE expires_at IS NOT NULL AND expires_at < ?",
                           -1, &s_db.stmt_delete_expired_sessions, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare delete_expired_sessions failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Rate limiting statements */
   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT COUNT(*) FROM login_attempts WHERE ip_address = ? AND timestamp > ? AND success = 0",
       -1, &s_db.stmt_count_recent_failures, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare count_recent_failures failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO login_attempts (ip_address, username, timestamp, success) VALUES (?, ?, ?, ?)",
       -1, &s_db.stmt_log_attempt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare log_attempt failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM login_attempts WHERE timestamp < ?", -1,
                           &s_db.stmt_delete_old_attempts, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare delete_old_attempts failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Audit log statements */
   rc = sqlite3_prepare_v2(s_db.db,
                           "INSERT INTO auth_log (timestamp, event, username, ip_address, details) "
                           "VALUES (?, ?, ?, ?, ?)",
                           -1, &s_db.stmt_log_event, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare log_event failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM auth_log WHERE timestamp < ?", -1,
                           &s_db.stmt_delete_old_logs, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare delete_old_logs failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* User settings statements */
   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT persona_description, persona_mode, location, timezone, units, "
                           "theme FROM user_settings WHERE user_id = ?",
                           -1, &s_db.stmt_get_user_settings, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare get_user_settings failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO user_settings (user_id, persona_description, persona_mode, location, timezone, "
       "units, theme, updated_at) "
       "VALUES (?, ?, ?, ?, ?, ?, ?, ?) "
       "ON CONFLICT(user_id) DO UPDATE SET "
       "persona_description=excluded.persona_description, persona_mode=excluded.persona_mode, "
       "location=excluded.location, timezone=excluded.timezone, units=excluded.units, "
       "theme=excluded.theme, updated_at=excluded.updated_at",
       -1, &s_db.stmt_set_user_settings, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare set_user_settings failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Conversation statements */
   rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO conversations (user_id, title, created_at, updated_at) VALUES (?, ?, ?, ?)", -1,
       &s_db.stmt_conv_create, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare conv_create failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, title, created_at, updated_at, message_count, is_archived, "
       "context_tokens, context_max, continued_from, compaction_summary, "
       "llm_type, cloud_provider, model, tools_mode, thinking_mode, is_private, origin, "
       "reasoning_effort "
       "FROM conversations WHERE id = ?",
       -1, &s_db.stmt_conv_get, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare conv_get failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, title, created_at, updated_at, message_count, is_archived, "
       "context_tokens, context_max, continued_from, compaction_summary, is_private, origin "
       "FROM conversations WHERE user_id = ? AND (is_archived = 0 OR ? = 1) "
       "ORDER BY updated_at DESC LIMIT ? OFFSET ?",
       -1, &s_db.stmt_conv_list, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare conv_list failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Admin-only: list all conversations across all users */
   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT c.id, c.user_id, c.title, c.created_at, c.updated_at, c.message_count, "
       "c.is_archived, c.context_tokens, c.context_max, c.continued_from, "
       "c.compaction_summary, c.is_private, c.origin, u.username "
       "FROM conversations c LEFT JOIN users u ON c.user_id = u.id "
       "WHERE (c.is_archived = 0 OR ? = 1) "
       "ORDER BY c.updated_at DESC LIMIT ? OFFSET ?",
       -1, &s_db.stmt_conv_list_all, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare conv_list_all failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, title, created_at, updated_at, message_count, is_archived, "
       "context_tokens, context_max, continued_from, compaction_summary, is_private, origin "
       "FROM conversations WHERE user_id = ? AND title LIKE ? "
       "ORDER BY updated_at DESC LIMIT ? OFFSET ?",
       -1, &s_db.stmt_conv_search, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare conv_search failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT DISTINCT c.id, c.user_id, c.title, c.created_at, c.updated_at, "
                           "c.message_count, c.is_archived, c.context_tokens, c.context_max, "
                           "c.continued_from, c.compaction_summary, c.is_private, c.origin "
                           "FROM conversations c "
                           "INNER JOIN messages m ON m.conversation_id = c.id "
                           "WHERE c.user_id = ? AND m.content LIKE ? "
                           "ORDER BY c.updated_at DESC LIMIT ? OFFSET ?",
                           -1, &s_db.stmt_conv_search_content, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare conv_search_content failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "UPDATE conversations SET title = ? WHERE id = ? AND user_id = ?", -1,
                           &s_db.stmt_conv_rename, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare conv_rename failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM conversations WHERE id = ? AND user_id = ?", -1,
                           &s_db.stmt_conv_delete, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare conv_delete failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Admin-only: delete any conversation without ownership check */
   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM conversations WHERE id = ?", -1,
                           &s_db.stmt_conv_delete_admin, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare conv_delete_admin failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "SELECT COUNT(*) FROM conversations WHERE user_id = ?", -1,
                           &s_db.stmt_conv_count, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare conv_count failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO messages (conversation_id, role, content, created_at) "
       "SELECT ?, ?, ?, ? WHERE EXISTS (SELECT 1 FROM conversations WHERE id = ? AND user_id = ?)",
       -1, &s_db.stmt_msg_add, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare msg_add failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT m.id, m.conversation_id, m.role, m.content, m.created_at "
                           "FROM messages m "
                           "INNER JOIN conversations c ON m.conversation_id = c.id "
                           "WHERE m.conversation_id = ? AND c.user_id = ? ORDER BY m.id ASC",
                           -1, &s_db.stmt_msg_get, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare msg_get failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Admin-only: get messages without user ownership check */
   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT id, conversation_id, role, content, created_at "
                           "FROM messages WHERE conversation_id = ? ORDER BY id ASC",
                           -1, &s_db.stmt_msg_get_admin, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare msg_get_admin failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "UPDATE conversations SET updated_at = ?, message_count = message_count + 1 WHERE id = ?",
       -1, &s_db.stmt_conv_update_meta, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare conv_update_meta failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "UPDATE conversations SET context_tokens = ?, context_max = ? "
                           "WHERE id = ? AND user_id = ?",
                           -1, &s_db.stmt_conv_update_context, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare conv_update_context failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO conversations (user_id, title, created_at, updated_at, origin) "
       "VALUES (?, ?, ?, ?, ?)",
       -1, &s_db.stmt_conv_create_origin, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare conv_create_origin failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "UPDATE conversations SET user_id = ? WHERE id = ?", -1,
                           &s_db.stmt_conv_reassign, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare conv_reassign failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Session metrics statements - token usage is in separate provider table */
   rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO session_metrics ("
       "session_id, user_id, session_type, started_at, ended_at, "
       "queries_total, queries_cloud, queries_local, errors_count, fallbacks_count, "
       "avg_asr_ms, avg_llm_ttft_ms, avg_llm_total_ms, avg_tts_ms, avg_pipeline_ms"
       ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
       -1, &s_db.stmt_metrics_save, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare metrics_save failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* UPDATE statement for per-query metrics updates (id is param 16) */
   rc = sqlite3_prepare_v2(
       s_db.db,
       "UPDATE session_metrics SET "
       "ended_at = ?, queries_total = ?, queries_cloud = ?, queries_local = ?, "
       "errors_count = ?, fallbacks_count = ?, avg_asr_ms = ?, avg_llm_ttft_ms = ?, "
       "avg_llm_total_ms = ?, avg_tts_ms = ?, avg_pipeline_ms = ? "
       "WHERE id = ?",
       -1, &s_db.stmt_metrics_update, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare metrics_update failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM session_metrics WHERE started_at < ?", -1,
                           &s_db.stmt_metrics_delete_old, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare metrics_delete_old failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Provider metrics insert (child table) */
   rc = sqlite3_prepare_v2(s_db.db,
                           "INSERT INTO session_metrics_providers ("
                           "session_metrics_id, provider, tokens_input, tokens_output, "
                           "tokens_cached, queries"
                           ") VALUES (?, ?, ?, ?, ?, ?)",
                           -1, &s_db.stmt_provider_metrics_save, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare provider_metrics_save failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Delete provider metrics before re-insert (for per-query updates) */
   rc = sqlite3_prepare_v2(s_db.db,
                           "DELETE FROM session_metrics_providers WHERE session_metrics_id = ?", -1,
                           &s_db.stmt_provider_metrics_delete, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare provider_metrics_delete failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Image statements (v30: filesystem-backed, no BLOB) */
   rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO images (id, user_id, source, retention_policy, mime_type, size, filename, "
       "created_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
       -1, &s_db.stmt_image_create, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare image_create failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, mime_type, size, filename, source, retention_policy, "
       "created_at, last_accessed FROM images WHERE id = ?",
       -1, &s_db.stmt_image_get, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare image_get failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT filename, user_id, source, mime_type, last_accessed FROM images WHERE id = ?", -1,
       &s_db.stmt_image_get_file, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare image_get_file failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM images WHERE id = ? AND user_id = ?", -1,
                           &s_db.stmt_image_delete, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare image_delete failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "UPDATE images SET last_accessed = ? WHERE id = ?", -1,
                           &s_db.stmt_image_update_access, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare image_update_access failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "UPDATE images SET retention_policy = ? "
                           "WHERE id = ? AND (? = 0 OR user_id = ?)",
                           -1, &s_db.stmt_image_update_retention, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare image_update_retention failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "SELECT COUNT(*) FROM images WHERE user_id = ?", -1,
                           &s_db.stmt_image_count_user, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare image_count_user failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "DELETE FROM images WHERE retention_policy = 0 AND created_at < ? "
       "AND id IN (SELECT id FROM images WHERE retention_policy = 0 AND created_at < ? "
       "ORDER BY created_at ASC LIMIT 100)",
       -1, &s_db.stmt_image_delete_old, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare image_delete_old failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT COALESCE(SUM(size), 0) FROM images WHERE retention_policy = 2",
                           -1, &s_db.stmt_image_cache_total_size, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare image_cache_total_size failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM images WHERE id = ?", -1,
                           &s_db.stmt_image_delete_cache_lru, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare image_delete_cache_lru failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, filename FROM images WHERE retention_policy = 0 AND created_at < ? "
       "ORDER BY created_at ASC LIMIT 100",
       -1, &s_db.stmt_image_get_expired_ids, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare image_get_expired_ids failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT id, filename, size FROM images WHERE retention_policy = 2 "
                           "ORDER BY COALESCE(last_accessed, created_at) ASC",
                           -1, &s_db.stmt_image_get_cache_lru_ids, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare image_get_cache_lru_ids failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "SELECT COUNT(*), COALESCE(SUM(size), 0) FROM images", -1,
                           &s_db.stmt_image_stats, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare image_stats failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Memory fact statements.  category column appended last in all SELECTs (column 9)
    * to preserve existing column indices in populate_fact_from_row. */
   rc = sqlite3_prepare_v2(s_db.db,
                           "INSERT INTO memory_facts (user_id, fact_text, confidence, source, "
                           "category, created_at, normalized_hash) "
                           "VALUES (?, ?, ?, ?, ?, ?, ?)",
                           -1, &s_db.stmt_memory_fact_create, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_fact_create failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, fact_text, confidence, source, created_at, last_accessed, "
       "access_count, superseded_by, category FROM memory_facts WHERE id = ?",
       -1, &s_db.stmt_memory_fact_get, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_fact_get failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, fact_text, confidence, source, created_at, last_accessed, "
       "access_count, superseded_by, category FROM memory_facts "
       "WHERE user_id = ? AND superseded_by IS NULL "
       "ORDER BY confidence DESC LIMIT ? OFFSET ?",
       -1, &s_db.stmt_memory_fact_list, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_fact_list failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, fact_text, confidence, source, created_at, last_accessed, "
       "access_count, superseded_by, category FROM memory_facts "
       "WHERE user_id = ? AND superseded_by IS NULL AND fact_text LIKE ? ESCAPE '\\' "
       "ORDER BY confidence DESC LIMIT ?",
       -1, &s_db.stmt_memory_fact_search, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_fact_search failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Category-filtered keyword search (v34).  Pre-filters fact-ID set so hybrid
    * scoring downstream operates only on facts in the requested category. */
   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, fact_text, confidence, source, created_at, last_accessed, "
       "access_count, superseded_by, category FROM memory_facts "
       "WHERE user_id = ? AND superseded_by IS NULL AND category = ? "
       "AND fact_text LIKE ? ESCAPE '\\' "
       "ORDER BY confidence DESC LIMIT ?",
       -1, &s_db.stmt_memory_fact_search_by_category, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_fact_search_by_category failed: %s",
                 sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Per-fact category UPDATE used by the centroid backfill pass (v34). */
   rc = sqlite3_prepare_v2(s_db.db, "UPDATE memory_facts SET category = ? WHERE id = ?", -1,
                           &s_db.stmt_memory_fact_update_category, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_fact_update_category failed: %s",
                 sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT id, user_id, fact_text, confidence, source, "
                           "  created_at, last_accessed, access_count, superseded_by, category "
                           "FROM memory_facts "
                           "WHERE user_id = ? AND superseded_by IS NULL "
                           "  AND category = 'general' AND id > ? "
                           "ORDER BY id ASC LIMIT ?",
                           -1, &s_db.stmt_memory_fact_list_general, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_fact_list_general failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT COUNT(*) FROM memory_facts "
                           "WHERE user_id = ? AND superseded_by IS NULL "
                           "  AND category = 'general'",
                           -1, &s_db.stmt_memory_fact_count_general, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_fact_count_general failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "UPDATE memory_facts SET last_accessed = ?,"
                           "  access_count = access_count + 1,"
                           "  confidence = CASE"
                           "    WHEN (CAST(strftime('%s','now') AS REAL) - last_accessed) > 3600"
                           "    THEN MIN(1.0, confidence + ?)"
                           "    ELSE confidence"
                           "  END "
                           "WHERE id = ? AND user_id = ?",
                           -1, &s_db.stmt_memory_fact_update_access, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_fact_update_access failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "UPDATE memory_facts SET confidence = ? WHERE id = ?", -1,
                           &s_db.stmt_memory_fact_update_confidence, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_fact_update_confidence failed: %s",
                 sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "UPDATE memory_facts SET superseded_by = ? WHERE id = ?", -1,
                           &s_db.stmt_memory_fact_supersede, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_fact_supersede failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM memory_facts WHERE id = ? AND user_id = ?", -1,
                           &s_db.stmt_memory_fact_delete, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_fact_delete failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT id, fact_text, confidence FROM memory_facts "
                           "WHERE user_id = ? AND superseded_by IS NULL "
                           "AND fact_text LIKE ? ESCAPE '\\' "
                           "ORDER BY confidence DESC LIMIT 5",
                           -1, &s_db.stmt_memory_fact_find_similar, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_fact_find_similar failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT id, fact_text, confidence FROM memory_facts "
                           "WHERE user_id = ? AND normalized_hash = ? AND superseded_by IS NULL",
                           -1, &s_db.stmt_memory_fact_find_by_hash, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_fact_find_by_hash failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "DELETE FROM memory_facts WHERE user_id = ? AND superseded_by IS NOT NULL "
       "AND created_at < ?",
       -1, &s_db.stmt_memory_fact_prune_superseded, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_fact_prune_superseded failed: %s",
                 sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "DELETE FROM memory_facts WHERE user_id = ? AND superseded_by IS NULL "
                           "AND last_accessed < ? AND confidence < ?",
                           -1, &s_db.stmt_memory_fact_prune_stale, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_fact_prune_stale failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Memory preference statements */
   rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO memory_preferences (user_id, category, value, confidence, source, created_at, "
       "updated_at) VALUES (?, ?, ?, ?, ?, ?, ?) "
       "ON CONFLICT(user_id, category) DO UPDATE SET "
       "value=excluded.value, confidence=excluded.confidence, updated_at=excluded.updated_at, "
       "reinforcement_count=reinforcement_count+1",
       -1, &s_db.stmt_memory_pref_upsert, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_pref_upsert failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, category, value, confidence, source, created_at, updated_at, "
       "reinforcement_count FROM memory_preferences WHERE user_id = ? AND category = ?",
       -1, &s_db.stmt_memory_pref_get, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_pref_get failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, category, value, confidence, source, created_at, updated_at, "
       "reinforcement_count FROM memory_preferences WHERE user_id = ? ORDER BY category "
       "LIMIT ? OFFSET ?",
       -1, &s_db.stmt_memory_pref_list, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_pref_list failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, category, value, confidence, source, created_at, updated_at, "
       "reinforcement_count FROM memory_preferences "
       "WHERE user_id = ? AND (category LIKE ? ESCAPE '\\' OR value LIKE ? ESCAPE '\\') "
       "ORDER BY confidence DESC LIMIT ?",
       -1, &s_db.stmt_memory_pref_search, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_pref_search failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "DELETE FROM memory_preferences WHERE user_id = ? AND category = ?", -1,
                           &s_db.stmt_memory_pref_delete, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_pref_delete failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Memory summary statements */
   rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO memory_summaries (user_id, session_id, summary, topics, sentiment, "
       "created_at, message_count, duration_seconds) VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
       -1, &s_db.stmt_memory_summary_create, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_summary_create failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, session_id, summary, topics, sentiment, created_at, "
       "message_count, duration_seconds, consolidated FROM memory_summaries "
       "WHERE user_id = ? AND consolidated = 0 ORDER BY created_at DESC LIMIT ? OFFSET ?",
       -1, &s_db.stmt_memory_summary_list, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_summary_list failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "UPDATE memory_summaries SET consolidated = 1 WHERE id = ?", -1,
                           &s_db.stmt_memory_summary_mark_consolidated, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_summary_mark_consolidated failed: %s",
                 sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, session_id, summary, topics, sentiment, created_at, "
       "message_count, duration_seconds, consolidated FROM memory_summaries "
       "WHERE user_id = ? AND (summary LIKE ? ESCAPE '\\' OR topics LIKE ? ESCAPE '\\') "
       "ORDER BY created_at DESC LIMIT ?",
       -1, &s_db.stmt_memory_summary_search, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_summary_search failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Date-filtered memory queries (for time_range search and fixed recent) */
   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, fact_text, confidence, source, created_at, last_accessed, "
       "access_count, superseded_by, category FROM memory_facts "
       "WHERE user_id = ? AND superseded_by IS NULL AND fact_text LIKE ? ESCAPE '\\' "
       "AND created_at >= ? ORDER BY confidence DESC LIMIT ?",
       -1, &s_db.stmt_memory_fact_search_since, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_fact_search_since failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, session_id, summary, topics, sentiment, created_at, "
       "message_count, duration_seconds, consolidated FROM memory_summaries "
       "WHERE user_id = ? AND (summary LIKE ? ESCAPE '\\' OR topics LIKE ? ESCAPE '\\') "
       "AND created_at >= ? ORDER BY created_at DESC LIMIT ?",
       -1, &s_db.stmt_memory_summary_search_since, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_summary_search_since failed: %s",
                 sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, fact_text, confidence, source, created_at, last_accessed, "
       "access_count, superseded_by, category FROM memory_facts "
       "WHERE user_id = ? AND superseded_by IS NULL AND created_at >= ? "
       "ORDER BY created_at DESC LIMIT ?",
       -1, &s_db.stmt_memory_fact_list_since, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_fact_list_since failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, session_id, summary, topics, sentiment, created_at, "
       "message_count, duration_seconds, consolidated FROM memory_summaries "
       "WHERE user_id = ? AND created_at >= ? "
       "ORDER BY created_at DESC LIMIT ?",
       -1, &s_db.stmt_memory_summary_list_since, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_summary_list_since failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Conversation extraction tracking statements */
   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT last_extracted_msg_count FROM conversations WHERE id = ?", -1,
                           &s_db.stmt_conv_get_last_extracted, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare conv_get_last_extracted failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "UPDATE conversations SET last_extracted_msg_count = ? WHERE id = ?", -1,
                           &s_db.stmt_conv_set_last_extracted, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare conv_set_last_extracted failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Conversation privacy statement */
   rc = sqlite3_prepare_v2(s_db.db,
                           "UPDATE conversations SET is_private = ? "
                           "WHERE id = ? AND user_id = ?",
                           -1, &s_db.stmt_conv_set_private, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare conv_set_private failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Auto-title statements */
   rc = sqlite3_prepare_v2(s_db.db,
                           "UPDATE conversations SET title = ?, title_locked = 1, updated_at = ? "
                           "WHERE id = ? AND user_id = ? AND title_locked = 0",
                           -1, &s_db.stmt_conv_auto_title, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare conv_auto_title failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "UPDATE conversations SET title_locked = ? "
                           "WHERE id = ? AND user_id = ?",
                           -1, &s_db.stmt_conv_set_title_locked, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare conv_set_title_locked failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Embedding statements */
   rc = sqlite3_prepare_v2(s_db.db,
                           "UPDATE memory_facts SET embedding = ?, embedding_norm = ? "
                           "WHERE id = ? AND user_id = ?",
                           -1, &s_db.stmt_memory_fact_update_embedding, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare fact_update_embedding failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* fact_get_embeddings: created_at appended last (col 3) for temporal-query
    * scoring (#3).  Cache loader reads it and stores per-fact for boost computation. */
   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT id, embedding, embedding_norm, created_at FROM memory_facts "
                           "WHERE user_id = ? AND superseded_by IS NULL AND embedding IS NOT NULL "
                           "ORDER BY confidence DESC LIMIT ?",
                           -1, &s_db.stmt_memory_fact_get_embeddings, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare fact_get_embeddings failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT id, fact_text FROM memory_facts "
                           "WHERE user_id = ? AND superseded_by IS NULL "
                           "AND (embedding IS NULL OR length(embedding)/4 != ?) "
                           "ORDER BY created_at ASC LIMIT ?",
                           -1, &s_db.stmt_memory_fact_list_without_embedding, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare fact_list_without_embedding failed: %s",
                 sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Entity graph statements */
   rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO memory_entities (user_id, name, entity_type, canonical_name, "
       "first_seen, last_seen, mention_count) "
       "VALUES (?, ?, ?, ?, strftime('%s','now'), strftime('%s','now'), 1) "
       "ON CONFLICT(user_id, canonical_name) DO UPDATE SET "
       "last_seen = strftime('%s','now'), mention_count = mention_count + 1, "
       "name = CASE WHEN length(excluded.name) > length(name) THEN excluded.name ELSE name END "
       "RETURNING id, mention_count",
       -1, &s_db.stmt_memory_entity_upsert, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare entity_upsert failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT id, user_id, name, entity_type, canonical_name, mention_count, "
                           "first_seen, last_seen FROM memory_entities "
                           "WHERE user_id = ? AND canonical_name = ?",
                           -1, &s_db.stmt_memory_entity_get_by_name, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare entity_get_by_name failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "UPDATE memory_entities SET embedding = ?, embedding_norm = ? "
                           "WHERE id = ? AND user_id = ?",
                           -1, &s_db.stmt_memory_entity_update_embedding, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare entity_update_embedding failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT id, name, entity_type, embedding, embedding_norm "
                           "FROM memory_entities "
                           "WHERE user_id = ? AND embedding IS NOT NULL "
                           "ORDER BY mention_count DESC LIMIT ?",
                           -1, &s_db.stmt_memory_entity_get_embeddings, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare entity_get_embeddings failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Memory relation statements.  valid_from/valid_to appended last in all SELECTs
    * (columns 6, 7) to preserve existing column indices in populate_relation_from_row. */
   rc = sqlite3_prepare_v2(s_db.db,
                           "INSERT INTO memory_relations (user_id, subject_entity_id, relation, "
                           "object_entity_id, object_value, fact_id, confidence, created_at, "
                           "valid_from, valid_to) "
                           "VALUES (?, ?, ?, ?, ?, ?, ?, strftime('%s','now'), ?, ?)",
                           -1, &s_db.stmt_memory_relation_create, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare relation_create failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Auto-close superseded exclusive relations (v33).  Used inside
    * memory_db_relation_supersede() before the new INSERT.  The (object_entity_id != ?
    * OR object_value != ?) clause skips when the user re-mentions the same target —
    * idempotency check.  COALESCE guards against NULL comparisons. */
   rc = sqlite3_prepare_v2(s_db.db,
                           "UPDATE memory_relations SET valid_to = ? "
                           "WHERE user_id = ? AND subject_entity_id = ? AND relation = ? "
                           "  AND valid_to IS NULL "
                           "  AND (COALESCE(object_entity_id, 0) != COALESCE(?, 0) "
                           "    OR COALESCE(object_value, '') != COALESCE(?, '')) "
                           "RETURNING fact_id",
                           -1, &s_db.stmt_memory_relation_close_open, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare relation_close_open failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT r.id, r.subject_entity_id, r.relation, r.object_entity_id, "
                           "COALESCE(e.name, r.object_value) AS object_name, r.confidence, "
                           "COALESCE(r.valid_from, 0), COALESCE(r.valid_to, 0) "
                           "FROM memory_relations r "
                           "LEFT JOIN memory_entities e ON r.object_entity_id = e.id "
                           "WHERE r.user_id = ? AND r.subject_entity_id = ? LIMIT ?",
                           -1, &s_db.stmt_memory_relation_list_by_subject, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare relation_list_by_subject failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* As-of variant: returns relations valid at the given timestamp.  Bounds:
    *   valid_from IS NULL or valid_from <= as_of
    *   valid_to   IS NULL or valid_to   >  as_of
    * Pass strftime('%s','now') as as_of for the "currently true" common case. */
   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT r.id, r.subject_entity_id, r.relation, r.object_entity_id, "
                           "COALESCE(e.name, r.object_value) AS object_name, r.confidence, "
                           "COALESCE(r.valid_from, 0), COALESCE(r.valid_to, 0) "
                           "FROM memory_relations r "
                           "LEFT JOIN memory_entities e ON r.object_entity_id = e.id "
                           "WHERE r.user_id = ? AND r.subject_entity_id = ? "
                           "  AND (r.valid_from IS NULL OR r.valid_from <= ?) "
                           "  AND (r.valid_to IS NULL OR r.valid_to > ?) "
                           "LIMIT ?",
                           -1, &s_db.stmt_memory_relation_list_by_subject_at, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare relation_list_by_subject_at failed: %s",
                 sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT r.id, r.subject_entity_id, r.relation, r.object_entity_id, "
                           "COALESCE(e.name, r.object_value) AS object_name, r.confidence, "
                           "COALESCE(r.valid_from, 0), COALESCE(r.valid_to, 0) "
                           "FROM memory_relations r "
                           "LEFT JOIN memory_entities e ON r.subject_entity_id = e.id "
                           "WHERE r.user_id = ? AND r.object_entity_id = ? LIMIT ?",
                           -1, &s_db.stmt_memory_relation_list_by_object, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare relation_list_by_object failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT id, user_id, name, entity_type, canonical_name, "
                           "mention_count, first_seen, last_seen "
                           "FROM memory_entities "
                           "WHERE user_id = ? AND canonical_name LIKE ? ESCAPE '\\' "
                           "ORDER BY mention_count DESC LIMIT ? OFFSET ?",
                           -1, &s_db.stmt_memory_entity_search, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare entity_search failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM memory_entities WHERE id = ? AND user_id = ?", -1,
                           &s_db.stmt_memory_entity_delete, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare entity_delete failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "UPDATE memory_entities SET photo_id = ? "
                           "WHERE id = ? AND user_id = ?",
                           -1, &s_db.stmt_memory_entity_set_photo, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare entity_set_photo failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT photo_id FROM memory_entities "
                           "WHERE id = ? AND user_id = ?",
                           -1, &s_db.stmt_memory_entity_get_photo, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare entity_get_photo failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "DELETE FROM memory_relations "
                           "WHERE user_id = ? AND (subject_entity_id = ? OR object_entity_id = ?)",
                           -1, &s_db.stmt_memory_relation_delete_by_entity, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare relation_delete_by_entity failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Satellite mapping statements */
   rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO satellite_mappings (uuid, name, location, ha_area, user_id, tier, "
       "last_seen, created_at, enabled) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?) "
       "ON CONFLICT(uuid) DO UPDATE SET name=excluded.name, location=excluded.location, "
       "tier=excluded.tier, last_seen=excluded.last_seen",
       -1, &s_db.stmt_satellite_upsert, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare satellite_upsert failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT uuid, name, location, ha_area, user_id, tier, last_seen, created_at, enabled "
       "FROM satellite_mappings WHERE uuid = ?",
       -1, &s_db.stmt_satellite_get, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare satellite_get failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM satellite_mappings WHERE uuid = ?", -1,
                           &s_db.stmt_satellite_delete, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare satellite_delete failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "UPDATE satellite_mappings SET user_id = ? WHERE uuid = ?", -1,
                           &s_db.stmt_satellite_update_user, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare satellite_update_user failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "UPDATE satellite_mappings SET location = ?, ha_area = ? WHERE uuid = ?",
                           -1, &s_db.stmt_satellite_update_location, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare satellite_update_location failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "UPDATE satellite_mappings SET enabled = ? WHERE uuid = ?", -1,
                           &s_db.stmt_satellite_set_enabled, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare satellite_set_enabled failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "UPDATE satellite_mappings SET last_seen = ? WHERE uuid = ?",
                           -1, &s_db.stmt_satellite_update_last_seen, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare satellite_update_last_seen failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT uuid, name, location, ha_area, user_id, tier, last_seen, created_at, enabled "
       "FROM satellite_mappings ORDER BY name ASC",
       -1, &s_db.stmt_satellite_list, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare satellite_list failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Document search statements */
   rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO documents (user_id, filename, filepath, filetype, file_hash, "
       "num_chunks, is_global, created_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
       -1, &s_db.stmt_doc_create, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare doc_create failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT id, user_id, filename, filepath, filetype, file_hash, "
                           "num_chunks, is_global, created_at FROM documents WHERE id = ?",
                           -1, &s_db.stmt_doc_get, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare doc_get failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT id FROM documents WHERE file_hash = ? "
                           "AND (user_id = ? OR is_global = 1)",
                           -1, &s_db.stmt_doc_get_by_hash, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare doc_get_by_hash failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT id, user_id, filename, filepath, filetype, file_hash, "
                           "num_chunks, is_global, created_at FROM documents "
                           "WHERE user_id = ? OR is_global = 1 ORDER BY created_at DESC "
                           "LIMIT ? OFFSET ?",
                           -1, &s_db.stmt_doc_list, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare doc_list failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT d.id, d.user_id, d.filename, d.filepath, d.filetype, "
                           "d.file_hash, d.num_chunks, d.is_global, d.created_at, "
                           "COALESCE(u.username, '') FROM documents d "
                           "LEFT JOIN users u ON d.user_id = u.id "
                           "ORDER BY d.created_at DESC LIMIT ? OFFSET ?",
                           -1, &s_db.stmt_doc_list_all, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare doc_list_all failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "UPDATE documents SET is_global = ? WHERE id = ?", -1,
                           &s_db.stmt_doc_update_global, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare doc_update_global failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM documents WHERE id = ?", -1, &s_db.stmt_doc_delete,
                           NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare doc_delete failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "SELECT COUNT(*) FROM documents WHERE user_id = ?", -1,
                           &s_db.stmt_doc_count_user, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare doc_count_user failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* doc_chunk_create: created_at appended last (col 6) — caller passes 0 for
    * unknown timestamps (older docs, manual ingests).  Schema default is 0. */
   rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO document_chunks (document_id, chunk_index, text, embedding, "
       "embedding_norm, created_at) VALUES (?, ?, ?, ?, ?, ?)",
       -1, &s_db.stmt_doc_chunk_create, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare doc_chunk_create failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* doc_chunk_search: created_at appended last so existing column indices in
    * downstream populators are preserved. */
   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT c.id, c.chunk_index, c.text, c.embedding, c.embedding_norm, "
                           "d.id, d.filename, d.filetype, c.created_at "
                           "FROM document_chunks c JOIN documents d ON c.document_id = d.id "
                           "WHERE d.user_id = ? OR d.is_global = 1 "
                           "LIMIT ?",
                           -1, &s_db.stmt_doc_chunk_search, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare doc_chunk_search failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT id, user_id, filename, filepath, filetype, file_hash, "
                           "num_chunks, is_global, created_at "
                           "FROM documents "
                           "WHERE (user_id = ? OR is_global = 1) "
                           "AND filename LIKE ? ESCAPE '\\' COLLATE NOCASE "
                           "ORDER BY CASE WHEN LOWER(filename) = LOWER(?) "
                           "THEN 0 ELSE 1 END, created_at DESC LIMIT 1",
                           -1, &s_db.stmt_doc_find_by_name, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare doc_find_by_name failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT chunk_index, text FROM document_chunks "
                           "WHERE document_id = ? ORDER BY chunk_index LIMIT ? OFFSET ?",
                           -1, &s_db.stmt_doc_chunk_read, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare doc_chunk_read failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* === Calendar statements === */

   rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO calendar_accounts (user_id, name, caldav_url, username, "
       "encrypted_password, auth_type, principal_url, calendar_home_url, enabled, "
       "last_sync, sync_interval_sec, created_at, read_only, oauth_account_key) "
       "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
       -1, &s_db.stmt_cal_acct_create, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare cal_acct_create failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT id, user_id, name, caldav_url, username, encrypted_password, "
                           "auth_type, principal_url, calendar_home_url, enabled, last_sync, "
                           "sync_interval_sec, created_at, read_only, oauth_account_key "
                           "FROM calendar_accounts WHERE id = ?",
                           -1, &s_db.stmt_cal_acct_get, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare cal_acct_get failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT id, user_id, name, caldav_url, username, encrypted_password, "
                           "auth_type, principal_url, calendar_home_url, enabled, last_sync, "
                           "sync_interval_sec, created_at, read_only, oauth_account_key "
                           "FROM calendar_accounts "
                           "WHERE user_id = ? ORDER BY name",
                           -1, &s_db.stmt_cal_acct_list, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare cal_acct_list failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT id, user_id, name, caldav_url, username, encrypted_password, "
                           "auth_type, principal_url, calendar_home_url, enabled, last_sync, "
                           "sync_interval_sec, created_at, read_only, oauth_account_key "
                           "FROM calendar_accounts "
                           "WHERE enabled = 1 ORDER BY last_sync ASC",
                           -1, &s_db.stmt_cal_acct_list_enabled, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare cal_acct_list_enabled failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "UPDATE calendar_accounts SET name=?, caldav_url=?, username=?, "
                           "encrypted_password=?, auth_type=?, enabled=?, sync_interval_sec=? "
                           "WHERE id=?",
                           -1, &s_db.stmt_cal_acct_update, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare cal_acct_update failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM calendar_accounts WHERE id = ?", -1,
                           &s_db.stmt_cal_acct_delete, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare cal_acct_delete failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "UPDATE calendar_accounts SET last_sync = ? WHERE id = ?", -1,
                           &s_db.stmt_cal_acct_update_sync, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare cal_acct_update_sync failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "UPDATE calendar_accounts SET principal_url = ?, "
                           "calendar_home_url = ? WHERE id = ?",
                           -1, &s_db.stmt_cal_acct_update_discovery, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare cal_acct_update_discovery failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "UPDATE calendar_accounts SET read_only = ? WHERE id = ?", -1,
                           &s_db.stmt_cal_acct_set_read_only, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare cal_acct_set_read_only failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "UPDATE calendar_accounts SET enabled = ? WHERE id = ?", -1,
                           &s_db.stmt_cal_acct_set_enabled, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare cal_acct_set_enabled failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "INSERT INTO calendar_calendars (account_id, caldav_path, display_name, "
                           "color, is_active, ctag, created_at) VALUES (?, ?, ?, ?, ?, ?, ?)",
                           -1, &s_db.stmt_cal_cal_create, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare cal_cal_create failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT id, account_id, caldav_path, display_name, color, "
                           "is_active, ctag, created_at FROM calendar_calendars WHERE id = ?",
                           -1, &s_db.stmt_cal_cal_get, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare cal_cal_get failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT id, account_id, caldav_path, display_name, color, "
                           "is_active, ctag, created_at FROM calendar_calendars "
                           "WHERE account_id = ? ORDER BY display_name",
                           -1, &s_db.stmt_cal_cal_list, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare cal_cal_list failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "UPDATE calendar_calendars SET ctag = ? WHERE id = ?", -1,
                           &s_db.stmt_cal_cal_update_ctag, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare cal_cal_update_ctag failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "UPDATE calendar_calendars SET is_active = ? WHERE id = ?", -1,
                           &s_db.stmt_cal_cal_set_active, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare cal_cal_set_active failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM calendar_calendars WHERE id = ?", -1,
                           &s_db.stmt_cal_cal_delete, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare cal_cal_delete failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT c.id, c.account_id, c.caldav_path, c.display_name, c.color, "
                           "c.is_active, c.ctag, c.created_at, a.read_only "
                           "FROM calendar_calendars c "
                           "JOIN calendar_accounts a ON c.account_id = a.id "
                           "WHERE a.user_id = ? AND a.enabled = 1 AND c.is_active = 1 "
                           "ORDER BY c.display_name",
                           -1, &s_db.stmt_cal_cal_active_for_user, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare cal_cal_active_for_user failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT OR REPLACE INTO calendar_events (calendar_id, uid, etag, summary, "
       "description, location, dtstart, dtend, duration_sec, all_day, "
       "dtstart_date, dtend_date, rrule, raw_ical, last_synced) "
       "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
       -1, &s_db.stmt_cal_evt_upsert, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare cal_evt_upsert failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT e.id, e.calendar_id, e.uid, e.etag, e.summary, e.description, "
                           "e.location, e.dtstart, e.dtend, e.duration_sec, e.all_day, "
                           "e.dtstart_date, e.dtend_date, e.rrule, e.raw_ical, e.last_synced "
                           "FROM calendar_events e "
                           "JOIN calendar_calendars c ON e.calendar_id = c.id "
                           "JOIN calendar_accounts a ON c.account_id = a.id "
                           "WHERE e.uid = ? AND a.user_id = ? LIMIT 1",
                           -1, &s_db.stmt_cal_evt_get_by_uid, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare cal_evt_get_by_uid failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM calendar_events WHERE id = ?", -1,
                           &s_db.stmt_cal_evt_delete, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare cal_evt_delete failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM calendar_events WHERE calendar_id = ?", -1,
                           &s_db.stmt_cal_evt_delete_by_cal, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare cal_evt_delete_by_cal failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO calendar_occurrences (event_id, dtstart, dtend, all_day, "
       "dtstart_date, dtend_date, summary, location, is_override, is_cancelled, "
       "recurrence_id) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
       -1, &s_db.stmt_cal_occ_insert, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare cal_occ_insert failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM calendar_occurrences WHERE event_id = ?", -1,
                           &s_db.stmt_cal_occ_delete_for_event, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare cal_occ_delete_for_event failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT o.id, o.event_id, o.dtstart, o.dtend, o.all_day, "
                           "o.dtstart_date, o.dtend_date, o.summary, o.location, "
                           "o.is_override, o.is_cancelled, o.recurrence_id, e.uid "
                           "FROM calendar_occurrences o "
                           "JOIN calendar_events e ON o.event_id = e.id "
                           "WHERE e.calendar_id IN (SELECT value FROM json_each(?)) "
                           "AND o.all_day = 0 AND o.is_cancelled = 0 "
                           "AND o.dtstart < ? AND o.dtend > ? "
                           "ORDER BY o.dtstart",
                           -1, &s_db.stmt_cal_occ_in_range, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare cal_occ_in_range failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT o.id, o.event_id, o.dtstart, o.dtend, o.all_day, "
                           "o.dtstart_date, o.dtend_date, o.summary, o.location, "
                           "o.is_override, o.is_cancelled, o.recurrence_id, e.uid "
                           "FROM calendar_occurrences o "
                           "JOIN calendar_events e ON o.event_id = e.id "
                           "WHERE e.calendar_id IN (SELECT value FROM json_each(?)) "
                           "AND o.all_day = 1 AND o.is_cancelled = 0 "
                           "AND o.dtstart_date < ? AND o.dtend_date > ? "
                           "ORDER BY o.dtstart_date",
                           -1, &s_db.stmt_cal_occ_allday_in_range, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare cal_occ_allday_in_range failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT o.id, o.event_id, o.dtstart, o.dtend, o.all_day, "
       "o.dtstart_date, o.dtend_date, o.summary, o.location, "
       "o.is_override, o.is_cancelled, o.recurrence_id, e.uid "
       "FROM calendar_occurrences o "
       "JOIN calendar_events e ON o.event_id = e.id "
       "WHERE e.calendar_id IN (SELECT value FROM json_each(?)) "
       "AND o.is_cancelled = 0 "
       "AND (o.summary LIKE ? COLLATE NOCASE OR o.location LIKE ? COLLATE NOCASE) "
       "ORDER BY o.dtstart LIMIT ?",
       -1, &s_db.stmt_cal_occ_search, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare cal_occ_search failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT o.id, o.event_id, o.dtstart, o.dtend, o.all_day, "
                           "o.dtstart_date, o.dtend_date, o.summary, o.location, "
                           "o.is_override, o.is_cancelled, o.recurrence_id, e.uid "
                           "FROM calendar_occurrences o "
                           "JOIN calendar_events e ON o.event_id = e.id "
                           "WHERE e.calendar_id IN (SELECT value FROM json_each(?)) "
                           "AND o.all_day = 0 AND o.is_cancelled = 0 "
                           "AND o.dtstart >= ? "
                           "ORDER BY o.dtstart LIMIT 1",
                           -1, &s_db.stmt_cal_occ_next, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare cal_occ_next failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* OAuth token statements */
   rc = sqlite3_prepare_v2(s_db.db,
                           "INSERT OR REPLACE INTO oauth_tokens "
                           "(user_id, provider, account_key, encrypted_data, encrypted_data_len, "
                           "scopes, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                           -1, &s_db.stmt_oauth_store, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare oauth_store failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT encrypted_data FROM oauth_tokens "
                           "WHERE user_id = ? AND provider = ? AND account_key = ?",
                           -1, &s_db.stmt_oauth_load, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare oauth_load failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "DELETE FROM oauth_tokens "
                           "WHERE user_id = ? AND provider = ? AND account_key = ?",
                           -1, &s_db.stmt_oauth_delete, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare oauth_delete failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT COUNT(*) FROM oauth_tokens "
                           "WHERE user_id = ? AND provider = ? AND account_key = ?",
                           -1, &s_db.stmt_oauth_exists, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare oauth_exists failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT account_key, scopes FROM oauth_tokens "
                           "WHERE user_id = ? AND provider = ?",
                           -1, &s_db.stmt_oauth_list_accounts, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare oauth_list_accounts failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* === Contacts statements === */
   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT c.id, c.entity_id, e.name, e.canonical_name, c.field_type, c.value, c.label, "
       "e.photo_id FROM contacts c JOIN memory_entities e ON c.entity_id = e.id "
       "WHERE c.user_id = ? AND e.canonical_name LIKE ? ESCAPE '\\' "
       "AND c.field_type LIKE ? ORDER BY e.name LIMIT ?",
       -1, &s_db.stmt_contacts_find, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare contacts_find failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO contacts (user_id, entity_id, field_type, value, label, created_at) "
       "SELECT ?, ?, ?, ?, ?, ? WHERE EXISTS "
       "(SELECT 1 FROM memory_entities WHERE id = ? AND user_id = ?)",
       -1, &s_db.stmt_contacts_add, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare contacts_add failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM contacts WHERE id = ? AND user_id = ?", -1,
                           &s_db.stmt_contacts_delete, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare contacts_delete failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT c.id, c.entity_id, e.name, e.canonical_name, c.field_type, c.value, c.label, "
       "e.photo_id FROM contacts c JOIN memory_entities e ON c.entity_id = e.id "
       "WHERE c.user_id = ? AND (? IS NULL OR c.field_type = ?) "
       "ORDER BY e.name LIMIT ? OFFSET ?",
       -1, &s_db.stmt_contacts_list, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare contacts_list failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "UPDATE contacts SET field_type = ?, value = ?, label = ? WHERE id = ? AND user_id = ?", -1,
       &s_db.stmt_contacts_update, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare contacts_update failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "SELECT COUNT(*) FROM contacts WHERE user_id = ?", -1,
                           &s_db.stmt_contacts_count, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare contacts_count failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* === Email account statements === */
   rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO email_accounts (user_id, name, imap_server, imap_port, imap_ssl, "
       "smtp_server, smtp_port, smtp_ssl, username, display_name, "
       "encrypted_password, encrypted_password_len, auth_type, oauth_account_key, "
       "enabled, read_only, max_recent, max_body_chars, created_at) "
       "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
       -1, &s_db.stmt_email_acct_create, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare email_acct_create failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, name, imap_server, imap_port, imap_ssl, "
       "smtp_server, smtp_port, smtp_ssl, username, display_name, "
       "encrypted_password, encrypted_password_len, auth_type, oauth_account_key, "
       "enabled, read_only, max_recent, max_body_chars, created_at "
       "FROM email_accounts WHERE id = ?",
       -1, &s_db.stmt_email_acct_get, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare email_acct_get failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, name, imap_server, imap_port, imap_ssl, "
       "smtp_server, smtp_port, smtp_ssl, username, display_name, "
       "encrypted_password, encrypted_password_len, auth_type, oauth_account_key, "
       "enabled, read_only, max_recent, max_body_chars, created_at "
       "FROM email_accounts WHERE user_id = ? ORDER BY name",
       -1, &s_db.stmt_email_acct_list, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare email_acct_list failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "UPDATE email_accounts SET name=?, imap_server=?, imap_port=?, imap_ssl=?, "
       "smtp_server=?, smtp_port=?, smtp_ssl=?, username=?, display_name=?, "
       "encrypted_password=?, encrypted_password_len=?, auth_type=?, oauth_account_key=?, "
       "max_recent=?, max_body_chars=? WHERE id=?",
       -1, &s_db.stmt_email_acct_update, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare email_acct_update failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM email_accounts WHERE id = ?", -1,
                           &s_db.stmt_email_acct_delete, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare email_acct_delete failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "UPDATE email_accounts SET read_only = ? WHERE id = ?", -1,
                           &s_db.stmt_email_acct_set_read_only, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare email_acct_set_read_only failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "UPDATE email_accounts SET enabled = ? WHERE id = ?", -1,
                           &s_db.stmt_email_acct_set_enabled, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare email_acct_set_enabled failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   return AUTH_DB_SUCCESS;
}

static void finalize_statements(void) {
   if (s_db.stmt_create_user)
      sqlite3_finalize(s_db.stmt_create_user);
   if (s_db.stmt_get_user)
      sqlite3_finalize(s_db.stmt_get_user);
   if (s_db.stmt_count_users)
      sqlite3_finalize(s_db.stmt_count_users);
   if (s_db.stmt_inc_failed_attempts)
      sqlite3_finalize(s_db.stmt_inc_failed_attempts);
   if (s_db.stmt_reset_failed_attempts)
      sqlite3_finalize(s_db.stmt_reset_failed_attempts);
   if (s_db.stmt_update_last_login)
      sqlite3_finalize(s_db.stmt_update_last_login);
   if (s_db.stmt_set_lockout)
      sqlite3_finalize(s_db.stmt_set_lockout);

   if (s_db.stmt_create_session)
      sqlite3_finalize(s_db.stmt_create_session);
   if (s_db.stmt_get_session)
      sqlite3_finalize(s_db.stmt_get_session);
   if (s_db.stmt_update_session_activity)
      sqlite3_finalize(s_db.stmt_update_session_activity);
   if (s_db.stmt_delete_session)
      sqlite3_finalize(s_db.stmt_delete_session);
   if (s_db.stmt_delete_user_sessions)
      sqlite3_finalize(s_db.stmt_delete_user_sessions);
   if (s_db.stmt_delete_expired_sessions)
      sqlite3_finalize(s_db.stmt_delete_expired_sessions);

   if (s_db.stmt_count_recent_failures)
      sqlite3_finalize(s_db.stmt_count_recent_failures);
   if (s_db.stmt_log_attempt)
      sqlite3_finalize(s_db.stmt_log_attempt);
   if (s_db.stmt_delete_old_attempts)
      sqlite3_finalize(s_db.stmt_delete_old_attempts);

   if (s_db.stmt_log_event)
      sqlite3_finalize(s_db.stmt_log_event);
   if (s_db.stmt_delete_old_logs)
      sqlite3_finalize(s_db.stmt_delete_old_logs);

   if (s_db.stmt_get_user_settings)
      sqlite3_finalize(s_db.stmt_get_user_settings);
   if (s_db.stmt_set_user_settings)
      sqlite3_finalize(s_db.stmt_set_user_settings);

   /* Conversation statements */
   if (s_db.stmt_conv_create)
      sqlite3_finalize(s_db.stmt_conv_create);
   if (s_db.stmt_conv_get)
      sqlite3_finalize(s_db.stmt_conv_get);
   if (s_db.stmt_conv_list)
      sqlite3_finalize(s_db.stmt_conv_list);
   if (s_db.stmt_conv_list_all)
      sqlite3_finalize(s_db.stmt_conv_list_all);
   if (s_db.stmt_conv_search)
      sqlite3_finalize(s_db.stmt_conv_search);
   if (s_db.stmt_conv_search_content)
      sqlite3_finalize(s_db.stmt_conv_search_content);
   if (s_db.stmt_conv_rename)
      sqlite3_finalize(s_db.stmt_conv_rename);
   if (s_db.stmt_conv_delete)
      sqlite3_finalize(s_db.stmt_conv_delete);
   if (s_db.stmt_conv_delete_admin)
      sqlite3_finalize(s_db.stmt_conv_delete_admin);
   if (s_db.stmt_conv_count)
      sqlite3_finalize(s_db.stmt_conv_count);
   if (s_db.stmt_msg_add)
      sqlite3_finalize(s_db.stmt_msg_add);
   if (s_db.stmt_msg_get)
      sqlite3_finalize(s_db.stmt_msg_get);
   if (s_db.stmt_msg_get_admin)
      sqlite3_finalize(s_db.stmt_msg_get_admin);
   if (s_db.stmt_conv_update_meta)
      sqlite3_finalize(s_db.stmt_conv_update_meta);
   if (s_db.stmt_conv_update_context)
      sqlite3_finalize(s_db.stmt_conv_update_context);
   if (s_db.stmt_conv_create_origin)
      sqlite3_finalize(s_db.stmt_conv_create_origin);
   if (s_db.stmt_conv_reassign)
      sqlite3_finalize(s_db.stmt_conv_reassign);

   /* Session metrics statements */
   if (s_db.stmt_metrics_save)
      sqlite3_finalize(s_db.stmt_metrics_save);
   if (s_db.stmt_metrics_update)
      sqlite3_finalize(s_db.stmt_metrics_update);
   if (s_db.stmt_metrics_delete_old)
      sqlite3_finalize(s_db.stmt_metrics_delete_old);
   if (s_db.stmt_provider_metrics_save)
      sqlite3_finalize(s_db.stmt_provider_metrics_save);
   if (s_db.stmt_provider_metrics_delete)
      sqlite3_finalize(s_db.stmt_provider_metrics_delete);

   /* Image statements */
   if (s_db.stmt_image_create)
      sqlite3_finalize(s_db.stmt_image_create);
   if (s_db.stmt_image_get)
      sqlite3_finalize(s_db.stmt_image_get);
   if (s_db.stmt_image_get_file)
      sqlite3_finalize(s_db.stmt_image_get_file);
   if (s_db.stmt_image_delete)
      sqlite3_finalize(s_db.stmt_image_delete);
   if (s_db.stmt_image_update_access)
      sqlite3_finalize(s_db.stmt_image_update_access);
   if (s_db.stmt_image_update_retention)
      sqlite3_finalize(s_db.stmt_image_update_retention);
   if (s_db.stmt_image_count_user)
      sqlite3_finalize(s_db.stmt_image_count_user);
   if (s_db.stmt_image_delete_old)
      sqlite3_finalize(s_db.stmt_image_delete_old);
   if (s_db.stmt_image_cache_total_size)
      sqlite3_finalize(s_db.stmt_image_cache_total_size);
   if (s_db.stmt_image_delete_cache_lru)
      sqlite3_finalize(s_db.stmt_image_delete_cache_lru);
   if (s_db.stmt_image_get_expired_ids)
      sqlite3_finalize(s_db.stmt_image_get_expired_ids);
   if (s_db.stmt_image_get_cache_lru_ids)
      sqlite3_finalize(s_db.stmt_image_get_cache_lru_ids);
   if (s_db.stmt_image_stats)
      sqlite3_finalize(s_db.stmt_image_stats);

   /* Memory statements */
   if (s_db.stmt_memory_fact_create)
      sqlite3_finalize(s_db.stmt_memory_fact_create);
   if (s_db.stmt_memory_fact_get)
      sqlite3_finalize(s_db.stmt_memory_fact_get);
   if (s_db.stmt_memory_fact_list)
      sqlite3_finalize(s_db.stmt_memory_fact_list);
   if (s_db.stmt_memory_fact_search)
      sqlite3_finalize(s_db.stmt_memory_fact_search);
   if (s_db.stmt_memory_fact_update_access)
      sqlite3_finalize(s_db.stmt_memory_fact_update_access);
   if (s_db.stmt_memory_fact_update_confidence)
      sqlite3_finalize(s_db.stmt_memory_fact_update_confidence);
   if (s_db.stmt_memory_fact_supersede)
      sqlite3_finalize(s_db.stmt_memory_fact_supersede);
   if (s_db.stmt_memory_fact_delete)
      sqlite3_finalize(s_db.stmt_memory_fact_delete);
   if (s_db.stmt_memory_fact_find_similar)
      sqlite3_finalize(s_db.stmt_memory_fact_find_similar);
   if (s_db.stmt_memory_pref_upsert)
      sqlite3_finalize(s_db.stmt_memory_pref_upsert);
   if (s_db.stmt_memory_pref_get)
      sqlite3_finalize(s_db.stmt_memory_pref_get);
   if (s_db.stmt_memory_pref_list)
      sqlite3_finalize(s_db.stmt_memory_pref_list);
   if (s_db.stmt_memory_pref_search)
      sqlite3_finalize(s_db.stmt_memory_pref_search);
   if (s_db.stmt_memory_pref_delete)
      sqlite3_finalize(s_db.stmt_memory_pref_delete);
   if (s_db.stmt_memory_summary_create)
      sqlite3_finalize(s_db.stmt_memory_summary_create);
   if (s_db.stmt_memory_summary_list)
      sqlite3_finalize(s_db.stmt_memory_summary_list);
   if (s_db.stmt_memory_summary_mark_consolidated)
      sqlite3_finalize(s_db.stmt_memory_summary_mark_consolidated);
   if (s_db.stmt_memory_summary_search)
      sqlite3_finalize(s_db.stmt_memory_summary_search);

   /* Date-filtered memory statements */
   if (s_db.stmt_memory_fact_search_since)
      sqlite3_finalize(s_db.stmt_memory_fact_search_since);
   if (s_db.stmt_memory_summary_search_since)
      sqlite3_finalize(s_db.stmt_memory_summary_search_since);
   if (s_db.stmt_memory_fact_list_since)
      sqlite3_finalize(s_db.stmt_memory_fact_list_since);
   if (s_db.stmt_memory_summary_list_since)
      sqlite3_finalize(s_db.stmt_memory_summary_list_since);

   /* Category-filtered fact statements (v34) */
   if (s_db.stmt_memory_fact_search_by_category)
      sqlite3_finalize(s_db.stmt_memory_fact_search_by_category);
   if (s_db.stmt_memory_fact_update_category)
      sqlite3_finalize(s_db.stmt_memory_fact_update_category);
   if (s_db.stmt_memory_fact_list_general)
      sqlite3_finalize(s_db.stmt_memory_fact_list_general);
   if (s_db.stmt_memory_fact_count_general)
      sqlite3_finalize(s_db.stmt_memory_fact_count_general);

   /* Deduplication and pruning statements */
   if (s_db.stmt_memory_fact_find_by_hash)
      sqlite3_finalize(s_db.stmt_memory_fact_find_by_hash);
   if (s_db.stmt_memory_fact_prune_superseded)
      sqlite3_finalize(s_db.stmt_memory_fact_prune_superseded);
   if (s_db.stmt_memory_fact_prune_stale)
      sqlite3_finalize(s_db.stmt_memory_fact_prune_stale);

   /* Extraction tracking statements */
   if (s_db.stmt_conv_get_last_extracted)
      sqlite3_finalize(s_db.stmt_conv_get_last_extracted);
   if (s_db.stmt_conv_set_last_extracted)
      sqlite3_finalize(s_db.stmt_conv_set_last_extracted);

   /* Privacy statement */
   if (s_db.stmt_conv_set_private)
      sqlite3_finalize(s_db.stmt_conv_set_private);

   /* Auto-title statements */
   if (s_db.stmt_conv_auto_title)
      sqlite3_finalize(s_db.stmt_conv_auto_title);
   if (s_db.stmt_conv_set_title_locked)
      sqlite3_finalize(s_db.stmt_conv_set_title_locked);

   /* Embedding statements */
   if (s_db.stmt_memory_fact_update_embedding)
      sqlite3_finalize(s_db.stmt_memory_fact_update_embedding);
   if (s_db.stmt_memory_fact_get_embeddings)
      sqlite3_finalize(s_db.stmt_memory_fact_get_embeddings);
   if (s_db.stmt_memory_fact_list_without_embedding)
      sqlite3_finalize(s_db.stmt_memory_fact_list_without_embedding);

   /* Entity graph statements */
   if (s_db.stmt_memory_entity_upsert)
      sqlite3_finalize(s_db.stmt_memory_entity_upsert);
   if (s_db.stmt_memory_entity_get_by_name)
      sqlite3_finalize(s_db.stmt_memory_entity_get_by_name);
   if (s_db.stmt_memory_entity_update_embedding)
      sqlite3_finalize(s_db.stmt_memory_entity_update_embedding);
   if (s_db.stmt_memory_entity_get_embeddings)
      sqlite3_finalize(s_db.stmt_memory_entity_get_embeddings);
   if (s_db.stmt_memory_relation_create)
      sqlite3_finalize(s_db.stmt_memory_relation_create);
   if (s_db.stmt_memory_relation_close_open)
      sqlite3_finalize(s_db.stmt_memory_relation_close_open);
   if (s_db.stmt_memory_relation_list_by_subject)
      sqlite3_finalize(s_db.stmt_memory_relation_list_by_subject);
   if (s_db.stmt_memory_relation_list_by_subject_at)
      sqlite3_finalize(s_db.stmt_memory_relation_list_by_subject_at);
   if (s_db.stmt_memory_relation_list_by_object)
      sqlite3_finalize(s_db.stmt_memory_relation_list_by_object);
   if (s_db.stmt_memory_entity_search)
      sqlite3_finalize(s_db.stmt_memory_entity_search);
   if (s_db.stmt_memory_entity_delete)
      sqlite3_finalize(s_db.stmt_memory_entity_delete);
   if (s_db.stmt_memory_relation_delete_by_entity)
      sqlite3_finalize(s_db.stmt_memory_relation_delete_by_entity);

   /* Satellite mapping statements */
   if (s_db.stmt_satellite_upsert)
      sqlite3_finalize(s_db.stmt_satellite_upsert);
   if (s_db.stmt_satellite_get)
      sqlite3_finalize(s_db.stmt_satellite_get);
   if (s_db.stmt_satellite_delete)
      sqlite3_finalize(s_db.stmt_satellite_delete);
   if (s_db.stmt_satellite_update_user)
      sqlite3_finalize(s_db.stmt_satellite_update_user);
   if (s_db.stmt_satellite_update_location)
      sqlite3_finalize(s_db.stmt_satellite_update_location);
   if (s_db.stmt_satellite_set_enabled)
      sqlite3_finalize(s_db.stmt_satellite_set_enabled);
   if (s_db.stmt_satellite_update_last_seen)
      sqlite3_finalize(s_db.stmt_satellite_update_last_seen);
   if (s_db.stmt_satellite_list)
      sqlite3_finalize(s_db.stmt_satellite_list);

   /* Document search statements */
   if (s_db.stmt_doc_create)
      sqlite3_finalize(s_db.stmt_doc_create);
   if (s_db.stmt_doc_get)
      sqlite3_finalize(s_db.stmt_doc_get);
   if (s_db.stmt_doc_get_by_hash)
      sqlite3_finalize(s_db.stmt_doc_get_by_hash);
   if (s_db.stmt_doc_list)
      sqlite3_finalize(s_db.stmt_doc_list);
   if (s_db.stmt_doc_list_all)
      sqlite3_finalize(s_db.stmt_doc_list_all);
   if (s_db.stmt_doc_delete)
      sqlite3_finalize(s_db.stmt_doc_delete);
   if (s_db.stmt_doc_count_user)
      sqlite3_finalize(s_db.stmt_doc_count_user);
   if (s_db.stmt_doc_chunk_create)
      sqlite3_finalize(s_db.stmt_doc_chunk_create);
   if (s_db.stmt_doc_chunk_search)
      sqlite3_finalize(s_db.stmt_doc_chunk_search);
   if (s_db.stmt_doc_find_by_name)
      sqlite3_finalize(s_db.stmt_doc_find_by_name);
   if (s_db.stmt_doc_chunk_read)
      sqlite3_finalize(s_db.stmt_doc_chunk_read);
   if (s_db.stmt_doc_update_global)
      sqlite3_finalize(s_db.stmt_doc_update_global);

   /* Calendar statements */
   if (s_db.stmt_cal_acct_create)
      sqlite3_finalize(s_db.stmt_cal_acct_create);
   if (s_db.stmt_cal_acct_get)
      sqlite3_finalize(s_db.stmt_cal_acct_get);
   if (s_db.stmt_cal_acct_list)
      sqlite3_finalize(s_db.stmt_cal_acct_list);
   if (s_db.stmt_cal_acct_list_enabled)
      sqlite3_finalize(s_db.stmt_cal_acct_list_enabled);
   if (s_db.stmt_cal_acct_set_read_only)
      sqlite3_finalize(s_db.stmt_cal_acct_set_read_only);
   if (s_db.stmt_cal_acct_set_enabled)
      sqlite3_finalize(s_db.stmt_cal_acct_set_enabled);
   if (s_db.stmt_cal_acct_update)
      sqlite3_finalize(s_db.stmt_cal_acct_update);
   if (s_db.stmt_cal_acct_delete)
      sqlite3_finalize(s_db.stmt_cal_acct_delete);
   if (s_db.stmt_cal_acct_update_sync)
      sqlite3_finalize(s_db.stmt_cal_acct_update_sync);
   if (s_db.stmt_cal_acct_update_discovery)
      sqlite3_finalize(s_db.stmt_cal_acct_update_discovery);
   if (s_db.stmt_cal_cal_create)
      sqlite3_finalize(s_db.stmt_cal_cal_create);
   if (s_db.stmt_cal_cal_get)
      sqlite3_finalize(s_db.stmt_cal_cal_get);
   if (s_db.stmt_cal_cal_list)
      sqlite3_finalize(s_db.stmt_cal_cal_list);
   if (s_db.stmt_cal_cal_update_ctag)
      sqlite3_finalize(s_db.stmt_cal_cal_update_ctag);
   if (s_db.stmt_cal_cal_set_active)
      sqlite3_finalize(s_db.stmt_cal_cal_set_active);
   if (s_db.stmt_cal_cal_delete)
      sqlite3_finalize(s_db.stmt_cal_cal_delete);
   if (s_db.stmt_cal_cal_active_for_user)
      sqlite3_finalize(s_db.stmt_cal_cal_active_for_user);
   if (s_db.stmt_cal_evt_upsert)
      sqlite3_finalize(s_db.stmt_cal_evt_upsert);
   if (s_db.stmt_cal_evt_get_by_uid)
      sqlite3_finalize(s_db.stmt_cal_evt_get_by_uid);
   if (s_db.stmt_cal_evt_delete)
      sqlite3_finalize(s_db.stmt_cal_evt_delete);
   if (s_db.stmt_cal_evt_delete_by_cal)
      sqlite3_finalize(s_db.stmt_cal_evt_delete_by_cal);
   if (s_db.stmt_cal_occ_insert)
      sqlite3_finalize(s_db.stmt_cal_occ_insert);
   if (s_db.stmt_cal_occ_delete_for_event)
      sqlite3_finalize(s_db.stmt_cal_occ_delete_for_event);
   if (s_db.stmt_cal_occ_in_range)
      sqlite3_finalize(s_db.stmt_cal_occ_in_range);
   if (s_db.stmt_cal_occ_allday_in_range)
      sqlite3_finalize(s_db.stmt_cal_occ_allday_in_range);
   if (s_db.stmt_cal_occ_search)
      sqlite3_finalize(s_db.stmt_cal_occ_search);
   if (s_db.stmt_cal_occ_next)
      sqlite3_finalize(s_db.stmt_cal_occ_next);

   /* Contacts statements */
   if (s_db.stmt_contacts_find)
      sqlite3_finalize(s_db.stmt_contacts_find);
   if (s_db.stmt_contacts_add)
      sqlite3_finalize(s_db.stmt_contacts_add);
   if (s_db.stmt_contacts_delete)
      sqlite3_finalize(s_db.stmt_contacts_delete);
   if (s_db.stmt_contacts_list)
      sqlite3_finalize(s_db.stmt_contacts_list);
   if (s_db.stmt_contacts_update)
      sqlite3_finalize(s_db.stmt_contacts_update);
   if (s_db.stmt_contacts_count)
      sqlite3_finalize(s_db.stmt_contacts_count);

   /* Email account statements */
   if (s_db.stmt_email_acct_create)
      sqlite3_finalize(s_db.stmt_email_acct_create);
   if (s_db.stmt_email_acct_get)
      sqlite3_finalize(s_db.stmt_email_acct_get);
   if (s_db.stmt_email_acct_list)
      sqlite3_finalize(s_db.stmt_email_acct_list);
   if (s_db.stmt_email_acct_update)
      sqlite3_finalize(s_db.stmt_email_acct_update);
   if (s_db.stmt_email_acct_delete)
      sqlite3_finalize(s_db.stmt_email_acct_delete);
   if (s_db.stmt_email_acct_set_read_only)
      sqlite3_finalize(s_db.stmt_email_acct_set_read_only);
   if (s_db.stmt_email_acct_set_enabled)
      sqlite3_finalize(s_db.stmt_email_acct_set_enabled);

   /* OAuth statements */
   if (s_db.stmt_oauth_store)
      sqlite3_finalize(s_db.stmt_oauth_store);
   if (s_db.stmt_oauth_load)
      sqlite3_finalize(s_db.stmt_oauth_load);
   if (s_db.stmt_oauth_delete)
      sqlite3_finalize(s_db.stmt_oauth_delete);
   if (s_db.stmt_oauth_exists)
      sqlite3_finalize(s_db.stmt_oauth_exists);
   if (s_db.stmt_oauth_list_accounts)
      sqlite3_finalize(s_db.stmt_oauth_list_accounts);

   /* Clear all statement pointers using offsetof for safety
    * MAINTENANCE: If statements are reordered, update first/last_stmt names */
   size_t first_stmt_offset = offsetof(auth_db_state_t, stmt_create_user);
   size_t last_stmt_end = offsetof(auth_db_state_t, stmt_oauth_list_accounts) +
                          sizeof(sqlite3_stmt *);
   memset((char *)&s_db + first_stmt_offset, 0, last_stmt_end - first_stmt_offset);
}

/* =============================================================================
 * File Permission Helpers
 * ============================================================================= */

int auth_db_internal_create_parent_dir(const char *path) {
   /* Set restrictive umask — auth databases need 0700 directories */
   mode_t old_umask = umask(0077);
   bool ok = path_ensure_parent_dir_mode(path, 0700);
   umask(old_umask);

   return ok ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
}

int auth_db_internal_verify_permissions(const char *path) {
   struct stat st;

   if (stat(path, &st) != 0) {
      /* File doesn't exist yet, that's OK */
      if (errno == ENOENT) {
         return AUTH_DB_SUCCESS;
      }
      OLOG_ERROR("auth_db: stat(%s) failed: %s", path, strerror(errno));
      return AUTH_DB_FAILURE;
   }

   /* Check for world/group readable or writable */
   if ((st.st_mode & 0077) != 0) {
      OLOG_WARNING("auth_db: SECURITY: %s has unsafe permissions %04o, fixing to 0600", path,
                   st.st_mode & 0777);

      if (chmod(path, 0600) != 0) {
         OLOG_ERROR("auth_db: failed to fix permissions on %s: %s", path, strerror(errno));
         return AUTH_DB_FAILURE;
      }
   }

   return AUTH_DB_SUCCESS;
}

/* =============================================================================
 * Custom SQLite Functions
 * ============================================================================= */

/**
 * @brief SQLite custom function: powf(base, exp)
 *
 * Enables atomic confidence decay in UPDATE statements without
 * a SELECT-compute-UPDATE loop. Used by memory decay system.
 */
static void sqlite_powf(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
   (void)argc;
   double base = sqlite3_value_double(argv[0]);
   double exp = sqlite3_value_double(argv[1]);
   double result = pow(base, exp);
   /* Guard against NaN/Inf from edge cases (negative base, huge exponent) */
   if (isnan(result) || isinf(result)) {
      sqlite3_result_double(ctx, 0.0);
   } else {
      sqlite3_result_double(ctx, result);
   }
}

/* =============================================================================
 * Lifecycle Functions
 * ============================================================================= */

int auth_db_init(const char *db_path) {
   pthread_mutex_lock(&s_db.mutex);

   if (s_db.initialized) {
      OLOG_WARNING("auth_db_init: already initialized");
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_SUCCESS;
   }

   const char *path = db_path ? db_path : AUTH_DB_DEFAULT_PATH;

   /* Create parent directory with secure permissions */
   if (auth_db_internal_create_parent_dir(path) != AUTH_DB_SUCCESS) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   /* Check existing file permissions */
   if (auth_db_internal_verify_permissions(path) != AUTH_DB_SUCCESS) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   /* Set restrictive umask for database file creation */
   mode_t old_umask = umask(0077);

   /* Open database with FULLMUTEX for thread safety */
   int rc = sqlite3_open_v2(path, &s_db.db,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                            NULL);

   umask(old_umask);

   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db_init: failed to open %s: %s", path,
                 s_db.db ? sqlite3_errmsg(s_db.db) : "unknown");
      if (s_db.db) {
         sqlite3_close(s_db.db);
         s_db.db = NULL;
      }
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   /* Verify permissions after creation */
   if (auth_db_internal_verify_permissions(path) != AUTH_DB_SUCCESS) {
      sqlite3_close(s_db.db);
      s_db.db = NULL;
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   /* Enable WAL mode for better concurrency */
   char *errmsg = NULL;
   rc = sqlite3_exec(s_db.db, "PRAGMA journal_mode=WAL", NULL, NULL, &errmsg);
   if (rc != SQLITE_OK) {
      OLOG_WARNING("auth_db: failed to enable WAL mode: %s", errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      /* Continue anyway - DELETE mode works too */
   }

   /* Set conservative cache size for embedded systems (64 pages × 4KB = 256KB) */
   sqlite3_exec(s_db.db, "PRAGMA cache_size=64", NULL, NULL, NULL);

   /* Enable foreign keys */
   sqlite3_exec(s_db.db, "PRAGMA foreign_keys=ON", NULL, NULL, NULL);

   /* Register custom SQL functions */
   sqlite3_create_function(s_db.db, "powf", 2, SQLITE_UTF8, NULL, sqlite_powf, NULL, NULL);

   /* Create schema if needed */
   if (create_schema(path) != AUTH_DB_SUCCESS) {
      sqlite3_close(s_db.db);
      s_db.db = NULL;
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   /* Prepare statements */
   if (prepare_statements() != AUTH_DB_SUCCESS) {
      finalize_statements();
      sqlite3_close(s_db.db);
      s_db.db = NULL;
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   s_db.initialized = true;
   s_db.last_cleanup = time(NULL);

   OLOG_INFO("auth_db_init: initialized at %s", path);

   pthread_mutex_unlock(&s_db.mutex);

   /* Ensure the local pseudo-satellite row exists. Runs outside the init
    * mutex because it calls other auth_db_* helpers that acquire the mutex
    * themselves. Safe: s_db.initialized is already true here. */
   if (satellite_db_ensure_local_pseudo() != AUTH_DB_SUCCESS) {
      OLOG_WARNING("auth_db_init: could not ensure local pseudo-satellite row");
   }

   return AUTH_DB_SUCCESS;
}

void auth_db_shutdown(void) {
   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized) {
      pthread_mutex_unlock(&s_db.mutex);
      return;
   }

   /* Checkpoint WAL to main database */
   if (s_db.db) {
      sqlite3_wal_checkpoint_v2(s_db.db, NULL, SQLITE_CHECKPOINT_TRUNCATE, NULL, NULL);
   }

   /* Finalize all statements */
   finalize_statements();

   /* Close database */
   if (s_db.db) {
      sqlite3_close(s_db.db);
      s_db.db = NULL;
   }

   s_db.initialized = false;

   OLOG_INFO("auth_db_shutdown: complete");

   pthread_mutex_unlock(&s_db.mutex);
}

bool auth_db_is_ready(void) {
   pthread_mutex_lock(&s_db.mutex);
   bool ready = s_db.initialized;
   pthread_mutex_unlock(&s_db.mutex);
   return ready;
}
