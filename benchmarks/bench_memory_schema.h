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
 * Bench memory-pipeline mode DDL.
 *
 * Subset of src/auth/auth_db_core.c:80-389 covering the tables the bench
 * exercises in --memory-pipeline mode: users, conversations, messages,
 * memory_facts, memory_preferences, memory_summaries, memory_entities,
 * memory_relations, system_metadata. Tables outside this set (sessions,
 * login_attempts, auth_log, user_settings, session_metrics*, images,
 * satellite_mappings, scheduled_events) are not duplicated — the bench
 * doesn't touch them.
 *
 * Drift detection: a unit test (tests/test_bench_schema_drift.c) opens two
 * :memory: DBs, applies this DDL to one and the production schema to the
 * other, then diffs PRAGMA table_info(<table>) for each table the bench
 * uses. Robust against formatting; catches real schema drift.
 *
 * When auth_db_core.c gains a column or table the bench needs, copy the
 * relevant CREATE statement here; the drift test will fail until done.
 */

#ifndef BENCH_MEMORY_SCHEMA_H
#define BENCH_MEMORY_SCHEMA_H

/* clang-format off */
static const char *BENCH_MEMORY_DDL =
    /* System-wide key/value metadata (v41).  Used to track daemon-level state
     * that spans all users — e.g., embedding_model_id for recompute detection. */
    "CREATE TABLE IF NOT EXISTS system_metadata ("
    "   key   TEXT PRIMARY KEY,"
    "   value TEXT NOT NULL"
    ");"

    /* Users table (categories_backfilled_at added in v34;
     * embeddings_model_id added in v41 — per-user gate for embedding recomputation) */
    "CREATE TABLE IF NOT EXISTS users ("
    "   id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "   username TEXT UNIQUE NOT NULL,"
    "   password_hash TEXT NOT NULL,"
    "   is_admin INTEGER DEFAULT 0,"
    "   created_at INTEGER NOT NULL,"
    "   last_login INTEGER,"
    "   failed_attempts INTEGER DEFAULT 0,"
    "   lockout_until INTEGER DEFAULT 0,"
    "   categories_backfilled_at INTEGER DEFAULT 0,"
    "   embeddings_model_id TEXT DEFAULT NULL"
    ");"

    /* Conversations table (extraction tracking columns from v15, privacy v16, origin v17) */
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
    "   last_extracted_msg_id    INTEGER NOT NULL DEFAULT 0,"
    "   extraction_attempts INTEGER DEFAULT 0,"
    "   extraction_last_attempt_at INTEGER DEFAULT 0,"
    "   is_private INTEGER DEFAULT 0,"
    "   title_locked INTEGER DEFAULT 0,"
    "   origin TEXT DEFAULT 'webui',"
    "   FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,"
    "   FOREIGN KEY (continued_from) REFERENCES conversations(id) ON DELETE SET NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_conversations_user ON conversations(user_id, updated_at DESC);"
    "CREATE INDEX IF NOT EXISTS idx_conversations_search ON conversations(user_id, title);"

    /* Messages table */
    "CREATE TABLE IF NOT EXISTS messages ("
    "   id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "   conversation_id INTEGER NOT NULL,"
    "   role TEXT NOT NULL CHECK(role IN ('system', 'user', 'assistant', 'tool')),"
    "   content TEXT NOT NULL,"
    "   created_at INTEGER NOT NULL,"
    "   FOREIGN KEY (conversation_id) REFERENCES conversations(id) ON DELETE CASCADE"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_messages_conversation ON messages(conversation_id, id ASC);"

    /* memory_facts (v14, columns extended through v40) */
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
    "   source_conversation_id INTEGER DEFAULT NULL,"
    "   source_msg_id_start    INTEGER DEFAULT NULL,"
    "   source_msg_id_end      INTEGER DEFAULT NULL,"
    "   FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,"
    "   FOREIGN KEY (superseded_by) REFERENCES memory_facts(id) ON DELETE SET NULL,"
    "   FOREIGN KEY (source_conversation_id) REFERENCES conversations(id) ON DELETE SET NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_memory_facts_user ON memory_facts(user_id);"
    "CREATE INDEX IF NOT EXISTS idx_memory_facts_confidence ON "
    "memory_facts(user_id, confidence DESC);"
    "CREATE INDEX IF NOT EXISTS idx_memory_facts_hash ON memory_facts(user_id, normalized_hash);"
    "CREATE INDEX IF NOT EXISTS idx_memory_facts_user_category ON "
    "memory_facts(user_id, category);"

    /* memory_preferences */
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
    "   source_conversation_id INTEGER DEFAULT NULL,"
    "   source_msg_id_start    INTEGER DEFAULT NULL,"
    "   source_msg_id_end      INTEGER DEFAULT NULL,"
    "   FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,"
    "   FOREIGN KEY (source_conversation_id) REFERENCES conversations(id) ON DELETE SET NULL,"
    "   UNIQUE(user_id, category)"
    ");"

    /* memory_summaries */
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
    "   source_conversation_id INTEGER DEFAULT NULL,"
    "   source_msg_id_start    INTEGER DEFAULT NULL,"
    "   source_msg_id_end      INTEGER DEFAULT NULL,"
    "   FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,"
    "   FOREIGN KEY (source_conversation_id) REFERENCES conversations(id) ON DELETE SET NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_memory_summaries_user ON "
    "memory_summaries(user_id, created_at DESC);"

    /* memory_entities (v19) */
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

    /* memory_relations (v19, valid_from/valid_to in v33, source_* in v40) */
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
    "  source_conversation_id INTEGER DEFAULT NULL,"
    "  source_msg_id_start    INTEGER DEFAULT NULL,"
    "  source_msg_id_end      INTEGER DEFAULT NULL,"
    "  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,"
    "  FOREIGN KEY (subject_entity_id) REFERENCES memory_entities(id) ON DELETE CASCADE,"
    "  FOREIGN KEY (object_entity_id) REFERENCES memory_entities(id) ON DELETE SET NULL,"
    "  FOREIGN KEY (fact_id) REFERENCES memory_facts(id) ON DELETE SET NULL,"
    "  FOREIGN KEY (source_conversation_id) REFERENCES conversations(id) ON DELETE SET NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_memory_relations_subject ON "
    "memory_relations(subject_entity_id);"
    "CREATE INDEX IF NOT EXISTS idx_memory_relations_object ON memory_relations(object_entity_id);"
    "CREATE INDEX IF NOT EXISTS idx_memory_relations_user ON memory_relations(user_id);"
    "CREATE INDEX IF NOT EXISTS idx_memory_relations_user_validity ON "
    "memory_relations(user_id, valid_from, valid_to);"
    ;
/* clang-format on */

#endif /* BENCH_MEMORY_SCHEMA_H */
