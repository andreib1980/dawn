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
 * the project author(s).
 *
 * Unit tests for v40 memory provenance:
 *   - source columns on memory_facts, memory_preferences, memory_summaries
 *   - memory_db_fact_get_source (ownership, privacy, NULL sentinel)
 *   - NULL provenance → NOT_FOUND from get_source
 *   - pref upsert latest-source-wins
 *   - null bind produces SQL NULL not integer 0
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db_internal.h"
#include "dawn_error.h"
#include "memory/memory_db.h"
#include "memory/memory_db_provenance.h"
#include "memory/memory_types.h"
#include "unity.h"

/* Minimal DDL matching the v40 schema (conversations + messages needed for
 * conv_db_is_private + source range queries). */
/* clang-format off */
static const char *DDL =
   "CREATE TABLE IF NOT EXISTS users ("
   "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
   "  username TEXT UNIQUE NOT NULL"
   ");"
   "INSERT INTO users (id, username) VALUES (1, 'alice_test');"
   "INSERT INTO users (id, username) VALUES (2, 'bob_test');"

   "CREATE TABLE IF NOT EXISTS conversations ("
   "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
   "  user_id INTEGER NOT NULL,"
   "  title TEXT NOT NULL DEFAULT 'Test',"
   "  created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
   "  updated_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
   "  message_count INTEGER DEFAULT 0,"
   "  is_archived INTEGER DEFAULT 0,"
   "  last_extracted_msg_count INTEGER DEFAULT 0,"
   "  last_extracted_msg_id    INTEGER NOT NULL DEFAULT 0,"
   "  extraction_attempts INTEGER DEFAULT 0,"
   "  extraction_last_attempt_at INTEGER DEFAULT 0,"
   "  is_private INTEGER DEFAULT 0,"
   "  title_locked INTEGER DEFAULT 0,"
   "  origin TEXT DEFAULT 'webui',"
   "  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE"
   ");"
   "INSERT INTO conversations (id, user_id, is_private) VALUES (10, 1, 0);"
   "INSERT INTO conversations (id, user_id, is_private) VALUES (20, 1, 1);"  /* private */

   "CREATE TABLE IF NOT EXISTS messages ("
   "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
   "  conversation_id INTEGER NOT NULL,"
   "  role TEXT NOT NULL,"
   "  content TEXT NOT NULL,"
   "  created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
   "  FOREIGN KEY (conversation_id) REFERENCES conversations(id) ON DELETE CASCADE"
   ");"
   "INSERT INTO messages (id, conversation_id, role, content) VALUES (100, 10, 'user', 'hello');"
   "INSERT INTO messages (id, conversation_id, role, content) VALUES (101, 10, 'assistant', 'hi');"
   "INSERT INTO messages (id, conversation_id, role, content) VALUES (200, 20, 'user', 'secret');"

   "CREATE TABLE IF NOT EXISTS memory_facts ("
   "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
   "  user_id INTEGER NOT NULL,"
   "  fact_text TEXT NOT NULL,"
   "  confidence REAL DEFAULT 1.0,"
   "  source TEXT DEFAULT 'inferred',"
   "  category TEXT NOT NULL DEFAULT 'general',"
   "  created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
   "  last_accessed INTEGER,"
   "  access_count INTEGER DEFAULT 0,"
   "  superseded_by INTEGER,"
   "  normalized_hash INTEGER DEFAULT 0,"
   "  embedding BLOB DEFAULT NULL,"
   "  embedding_norm REAL DEFAULT NULL,"
   "  source_conversation_id INTEGER DEFAULT NULL,"
   "  source_msg_id_start    INTEGER DEFAULT NULL,"
   "  source_msg_id_end      INTEGER DEFAULT NULL,"
   "  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,"
   "  FOREIGN KEY (source_conversation_id) REFERENCES conversations(id) ON DELETE SET NULL"
   ");"

   "CREATE TABLE IF NOT EXISTS memory_preferences ("
   "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
   "  user_id INTEGER NOT NULL,"
   "  category TEXT NOT NULL,"
   "  value TEXT NOT NULL,"
   "  confidence REAL DEFAULT 0.5,"
   "  source TEXT DEFAULT 'inferred',"
   "  created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
   "  updated_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
   "  reinforcement_count INTEGER DEFAULT 1,"
   "  source_conversation_id INTEGER DEFAULT NULL,"
   "  source_msg_id_start    INTEGER DEFAULT NULL,"
   "  source_msg_id_end      INTEGER DEFAULT NULL,"
   "  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,"
   "  FOREIGN KEY (source_conversation_id) REFERENCES conversations(id) ON DELETE SET NULL,"
   "  UNIQUE(user_id, category)"
   ");"

   "CREATE TABLE IF NOT EXISTS memory_summaries ("
   "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
   "  user_id INTEGER NOT NULL,"
   "  session_id TEXT NOT NULL,"
   "  summary TEXT NOT NULL,"
   "  topics TEXT,"
   "  sentiment TEXT,"
   "  created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
   "  message_count INTEGER,"
   "  duration_seconds INTEGER,"
   "  consolidated INTEGER DEFAULT 0,"
   "  source_conversation_id INTEGER DEFAULT NULL,"
   "  source_msg_id_start    INTEGER DEFAULT NULL,"
   "  source_msg_id_end      INTEGER DEFAULT NULL,"
   "  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,"
   "  FOREIGN KEY (source_conversation_id) REFERENCES conversations(id) ON DELETE SET NULL"
   ");"

   /* Phase B: relations table (subset — entities/links not needed for source tests) */
   "CREATE TABLE IF NOT EXISTS memory_relations ("
   "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
   "  user_id INTEGER NOT NULL,"
   "  subject_entity_id INTEGER NOT NULL DEFAULT 0,"
   "  relation TEXT NOT NULL,"
   "  object_entity_id INTEGER DEFAULT 0,"
   "  object_name TEXT,"
   "  confidence REAL DEFAULT 1.0,"
   "  fact_id INTEGER DEFAULT 0,"
   "  valid_from INTEGER DEFAULT NULL,"
   "  valid_to INTEGER DEFAULT NULL,"
   "  created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
   "  source_conversation_id INTEGER DEFAULT NULL,"
   "  source_msg_id_start    INTEGER DEFAULT NULL,"
   "  source_msg_id_end      INTEGER DEFAULT NULL,"
   "  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,"
   "  FOREIGN KEY (source_conversation_id) REFERENCES conversations(id) ON DELETE SET NULL"
   ");";
/* clang-format on */

static int prepare_statements(void) {
   int rc;

   rc = sqlite3_prepare_v2(s_db.db,
                           "INSERT INTO memory_facts (user_id, fact_text, confidence, source, "
                           "category, created_at, normalized_hash, "
                           "source_conversation_id, source_msg_id_start, source_msg_id_end) "
                           "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                           -1, &s_db.stmt_memory_fact_create, NULL);
   if (rc != SQLITE_OK)
      return FAILURE;

   rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO memory_preferences (user_id, category, value, confidence, source, created_at, "
       "updated_at, source_conversation_id, source_msg_id_start, source_msg_id_end) "
       "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
       "ON CONFLICT(user_id, category) DO UPDATE SET "
       "value=excluded.value, confidence=excluded.confidence, updated_at=excluded.updated_at, "
       "source_conversation_id=excluded.source_conversation_id, "
       "source_msg_id_start=excluded.source_msg_id_start, "
       "source_msg_id_end=excluded.source_msg_id_end, "
       "reinforcement_count=reinforcement_count+1",
       -1, &s_db.stmt_memory_pref_upsert, NULL);
   if (rc != SQLITE_OK)
      return FAILURE;

   rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO memory_summaries (user_id, session_id, summary, topics, sentiment, "
       "created_at, message_count, duration_seconds, "
       "source_conversation_id, source_msg_id_start, source_msg_id_end) "
       "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
       -1, &s_db.stmt_memory_summary_create, NULL);
   if (rc != SQLITE_OK)
      return FAILURE;

   /* conv_set_last_extracted — not used in these tests but must be valid */
   rc = sqlite3_prepare_v2(s_db.db, "SELECT 1", -1, &s_db.stmt_conv_set_last_extracted, NULL);
   if (rc != SQLITE_OK)
      return FAILURE;

   return SUCCESS;
}

static void open_db(void) {
   if (sqlite3_open(":memory:", &s_db.db) != SQLITE_OK) {
      fprintf(stderr, "open failed\n");
      exit(1);
   }
   sqlite3_exec(s_db.db, "PRAGMA foreign_keys=ON", NULL, NULL, NULL);
   char *err = NULL;
   if (sqlite3_exec(s_db.db, DDL, NULL, NULL, &err) != SQLITE_OK) {
      fprintf(stderr, "DDL failed: %s\n", err ? err : "?");
      sqlite3_free(err);
      exit(1);
   }
   if (prepare_statements() != 0) {
      fprintf(stderr, "prepare failed\n");
      exit(1);
   }
   s_db.initialized = true;
}

static void close_db(void) {
   s_db.initialized = false;
   if (s_db.stmt_memory_fact_create)
      sqlite3_finalize(s_db.stmt_memory_fact_create);
   if (s_db.stmt_memory_pref_upsert)
      sqlite3_finalize(s_db.stmt_memory_pref_upsert);
   if (s_db.stmt_memory_summary_create)
      sqlite3_finalize(s_db.stmt_memory_summary_create);
   if (s_db.stmt_conv_set_last_extracted)
      sqlite3_finalize(s_db.stmt_conv_set_last_extracted);
   s_db.stmt_memory_fact_create = NULL;
   s_db.stmt_memory_pref_upsert = NULL;
   s_db.stmt_memory_summary_create = NULL;
   s_db.stmt_conv_set_last_extracted = NULL;
   sqlite3_close(s_db.db);
   s_db.db = NULL;
}

void setUp(void) {
   open_db();
}
void tearDown(void) {
   close_db();
}

/* ================================================================
 * Tests
 * ================================================================ */

void test_fact_create_with_source_roundtrip(void) {
   memory_provenance_t prov = { .conv_id = 10, .msg_id_start = 100, .msg_id_end = 101 };
   int64_t id = 0;
   int rc = memory_db_fact_create(1, "User likes coffee", 0.9f, "inferred", "interests", &prov,
                                  &id);
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_GREATER_THAN(0, id);

   int64_t conv_id = 0, start = 0, end = 0;
   rc = memory_db_fact_get_source(id, 1, &conv_id, &start, &end);
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL(10, conv_id);
   TEST_ASSERT_EQUAL(100, start);
   TEST_ASSERT_EQUAL(101, end);
}

void test_fact_create_null_provenance_returns_not_found(void) {
   int64_t id = 0;
   int rc = memory_db_fact_create(1, "No prov fact", 0.8f, "inferred", "general", NULL, &id);
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_GREATER_THAN(0, id);

   int64_t conv_id = 0, start = 0, end = 0;
   rc = memory_db_fact_get_source(id, 1, &conv_id, &start, &end);
   TEST_ASSERT_EQUAL(MEMORY_DB_NOT_FOUND, rc);
}

void test_null_bind_produces_sql_null_not_zero(void) {
   int64_t id = 0;
   memory_db_fact_create(1, "Null bind test", 0.7f, "inferred", "general", NULL, &id);
   TEST_ASSERT_GREATER_THAN(0, id);

   /* Verify column is SQL NULL, not integer 0 */
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(s_db.db, "SELECT source_conversation_id FROM memory_facts WHERE id = ?", -1,
                      &stmt, NULL);
   sqlite3_bind_int64(stmt, 1, id);
   TEST_ASSERT_EQUAL(SQLITE_ROW, sqlite3_step(stmt));
   TEST_ASSERT_EQUAL(SQLITE_NULL, sqlite3_column_type(stmt, 0));
   sqlite3_finalize(stmt);
}

void test_get_source_wrong_user_returns_not_found(void) {
   memory_provenance_t prov = { .conv_id = 10, .msg_id_start = 100, .msg_id_end = 101 };
   int64_t id = 0;
   memory_db_fact_create(1, "Alice's fact", 0.9f, "inferred", "personal", &prov, &id);
   TEST_ASSERT_GREATER_THAN(0, id);

   /* User 2 should not see user 1's source */
   int64_t conv_id = 0, start = 0, end = 0;
   int rc = memory_db_fact_get_source(id, 2, &conv_id, &start, &end);
   TEST_ASSERT_EQUAL(MEMORY_DB_NOT_FOUND, rc);
}

void test_get_source_private_conv_returns_not_found(void) {
   /* Conv 20 is marked is_private=1 in DDL */
   memory_provenance_t prov = { .conv_id = 20, .msg_id_start = 200, .msg_id_end = 200 };
   int64_t id = 0;
   memory_db_fact_create(1, "Private source fact", 0.9f, "inferred", "personal", &prov, &id);
   TEST_ASSERT_GREATER_THAN(0, id);

   int64_t conv_id = 0, start = 0, end = 0;
   int rc = memory_db_fact_get_source(id, 1, &conv_id, &start, &end);
   /* conv is private → NOT_FOUND */
   TEST_ASSERT_EQUAL(MEMORY_DB_NOT_FOUND, rc);
}

void test_pref_upsert_with_source_roundtrip(void) {
   memory_provenance_t prov = { .conv_id = 10, .msg_id_start = 100, .msg_id_end = 101 };
   int rc = memory_db_pref_upsert(1, "theme", "dark", 0.8f, "inferred", &prov);
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, rc);

   /* Verify columns via direct SQL */
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(s_db.db,
                      "SELECT source_conversation_id, source_msg_id_start, source_msg_id_end "
                      "FROM memory_preferences WHERE user_id=1 AND category='theme'",
                      -1, &stmt, NULL);
   TEST_ASSERT_EQUAL(SQLITE_ROW, sqlite3_step(stmt));
   TEST_ASSERT_EQUAL(10, sqlite3_column_int64(stmt, 0));
   TEST_ASSERT_EQUAL(100, sqlite3_column_int64(stmt, 1));
   TEST_ASSERT_EQUAL(101, sqlite3_column_int64(stmt, 2));
   sqlite3_finalize(stmt);
}

void test_pref_upsert_latest_source_wins(void) {
   memory_provenance_t prov1 = { .conv_id = 10, .msg_id_start = 100, .msg_id_end = 100 };
   memory_provenance_t prov2 = { .conv_id = 10, .msg_id_start = 101, .msg_id_end = 101 };

   memory_db_pref_upsert(1, "language", "english", 0.7f, "inferred", &prov1);
   memory_db_pref_upsert(1, "language", "french", 0.9f, "inferred", &prov2);

   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(s_db.db,
                      "SELECT source_msg_id_start FROM memory_preferences "
                      "WHERE user_id=1 AND category='language'",
                      -1, &stmt, NULL);
   TEST_ASSERT_EQUAL(SQLITE_ROW, sqlite3_step(stmt));
   TEST_ASSERT_EQUAL(101, sqlite3_column_int64(stmt, 0)); /* prov2 wins */
   sqlite3_finalize(stmt);
}

void test_summary_create_with_source(void) {
   memory_provenance_t prov = { .conv_id = 10, .msg_id_start = 100, .msg_id_end = 101 };
   int64_t id = 0;
   int rc = memory_db_summary_create(1, "sess1", "Test summary", "topics", "neutral", 2, 60, &prov,
                                     &id);
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_GREATER_THAN(0, id);

   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(s_db.db,
                      "SELECT source_conversation_id, source_msg_id_start, source_msg_id_end "
                      "FROM memory_summaries WHERE id=?",
                      -1, &stmt, NULL);
   sqlite3_bind_int64(stmt, 1, id);
   TEST_ASSERT_EQUAL(SQLITE_ROW, sqlite3_step(stmt));
   TEST_ASSERT_EQUAL(10, sqlite3_column_int64(stmt, 0));
   TEST_ASSERT_EQUAL(100, sqlite3_column_int64(stmt, 1));
   TEST_ASSERT_EQUAL(101, sqlite3_column_int64(stmt, 2));
   sqlite3_finalize(stmt);
}

void test_summary_create_null_provenance(void) {
   int64_t id = 0;
   int rc = memory_db_summary_create(1, "sess2", "No prov summary", "t", "neutral", 1, 30, NULL,
                                     &id);
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, rc);

   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(s_db.db, "SELECT source_conversation_id FROM memory_summaries WHERE id=?", -1,
                      &stmt, NULL);
   sqlite3_bind_int64(stmt, 1, id);
   TEST_ASSERT_EQUAL(SQLITE_ROW, sqlite3_step(stmt));
   TEST_ASSERT_EQUAL(SQLITE_NULL, sqlite3_column_type(stmt, 0));
   sqlite3_finalize(stmt);
}

void test_pref_upsert_null_provenance_sql_null(void) {
   memory_db_pref_upsert(1, "units", "metric", 0.8f, "inferred", NULL);

   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(s_db.db,
                      "SELECT source_conversation_id FROM memory_preferences "
                      "WHERE user_id=1 AND category='units'",
                      -1, &stmt, NULL);
   TEST_ASSERT_EQUAL(SQLITE_ROW, sqlite3_step(stmt));
   TEST_ASSERT_EQUAL(SQLITE_NULL, sqlite3_column_type(stmt, 0));
   sqlite3_finalize(stmt);
}

void test_get_source_nonexistent_fact(void) {
   int64_t conv_id = 0, start = 0, end = 0;
   int rc = memory_db_fact_get_source(99999, 1, &conv_id, &start, &end);
   TEST_ASSERT_EQUAL(MEMORY_DB_NOT_FOUND, rc);
}

void test_conversations_have_last_extracted_msg_id_column(void) {
   /* Verify the column exists and defaults to 0 */
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "SELECT last_extracted_msg_id FROM conversations WHERE id=10", -1,
                               &stmt, NULL);
   TEST_ASSERT_EQUAL(SQLITE_OK, rc);
   TEST_ASSERT_EQUAL(SQLITE_ROW, sqlite3_step(stmt));
   TEST_ASSERT_EQUAL(0, sqlite3_column_int64(stmt, 0));
   sqlite3_finalize(stmt);
}

/* =============================================================================
 * Phase B: batch source-reader tests for relations / summaries / preferences.
 *
 * Each record type gets four assertions:
 *   - roundtrip: stored provenance comes back through the batch API.
 *   - NULL provenance: row stored with NULL source columns → out_conv = 0.
 *   - private-conv filter: row stored pointing at a private conversation →
 *     out_conv = 0 (filtered in SQL via JOIN on is_private = 0).
 *   - N=64 chunked succeeds: post-fold-in (HIGH-1/HIGH-2 from Phase B
 *     architecture review), the public APIs accept any positive N and chunk
 *     internally in MAX_PROVENANCE_BATCH-sized passes.  64 IDs land in 2
 *     chunks; non-existent IDs yield out=0.  This test pins the new contract
 *     and replaces the original "n>cap fails closed" test — fail-closed
 *     semantics are still enforced at the inner SQL builder, just not at the
 *     public surface.
 * ============================================================================= */

/* Helper: insert a memory_relations row with explicit provenance (or NULL). */
static int64_t insert_relation(int user_id, int64_t conv_id, int64_t start, int64_t end) {
   sqlite3_stmt *stmt = NULL;
   const char *sql =
       (conv_id > 0)
           ? "INSERT INTO memory_relations (user_id, subject_entity_id, relation, object_name, "
             "source_conversation_id, source_msg_id_start, source_msg_id_end) "
             "VALUES (?, 1, 'is_a', 'literal', ?, ?, ?)"
           : "INSERT INTO memory_relations (user_id, subject_entity_id, relation, object_name) "
             "VALUES (?, 1, 'is_a', 'literal')";
   sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   sqlite3_bind_int(stmt, 1, user_id);
   if (conv_id > 0) {
      sqlite3_bind_int64(stmt, 2, conv_id);
      sqlite3_bind_int64(stmt, 3, start);
      sqlite3_bind_int64(stmt, 4, end);
   }
   sqlite3_step(stmt);
   int64_t id = sqlite3_last_insert_rowid(s_db.db);
   sqlite3_finalize(stmt);
   return id;
}

/* ------------------------------ relations ------------------------------ */

void test_relations_get_sources_roundtrip(void) {
   int64_t rid = insert_relation(1, 10, 100, 101);
   int64_t conv[1] = { 0 }, start[1] = { 0 }, end[1] = { 0 };
   int rc = memory_db_relations_get_sources(1, &rid, 1, conv, start, end);
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL(10, conv[0]);
   TEST_ASSERT_EQUAL(100, start[0]);
   TEST_ASSERT_EQUAL(101, end[0]);
}

void test_relations_get_sources_null_prov_returns_zero(void) {
   int64_t rid = insert_relation(1, 0, 0, 0);
   int64_t conv[1] = { 99 }, start[1] = { 99 }, end[1] = { 99 };
   int rc = memory_db_relations_get_sources(1, &rid, 1, conv, start, end);
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, rc);
   /* No provenance recorded — output should be zeroed by the API. */
   TEST_ASSERT_EQUAL(0, conv[0]);
   TEST_ASSERT_EQUAL(0, start[0]);
   TEST_ASSERT_EQUAL(0, end[0]);
}

void test_relations_get_sources_private_conv_filter(void) {
   int64_t rid = insert_relation(1, 20, 200, 200); /* conv 20 is is_private=1 */
   int64_t conv[1] = { 99 }, start[1] = { 99 }, end[1] = { 99 };
   int rc = memory_db_relations_get_sources(1, &rid, 1, conv, start, end);
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, rc);
   /* Privacy JOIN should suppress the row → output zeroed. */
   TEST_ASSERT_EQUAL(0, conv[0]);
}

void test_relations_get_sources_n_chunked_succeeds(void) {
   /* Insert one relation with provenance; query with N=64 (2× the cap).
    * Public API chunks internally — IDs without a row return out=0; the
    * known ID is found regardless of which chunk it falls into. */
   int64_t rid = insert_relation(1, 10, 100, 101);
   int64_t ids[64];
   int64_t conv[64] = { 0 }, start[64] = { 0 }, end[64] = { 0 };
   for (int i = 0; i < 64; i++)
      ids[i] = (i == 50) ? rid : (i + 1000); /* place the real ID in chunk 2 */
   int rc = memory_db_relations_get_sources(1, ids, 64, conv, start, end);
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL(10, conv[50]);
   TEST_ASSERT_EQUAL(100, start[50]);
   TEST_ASSERT_EQUAL(101, end[50]);
   /* Spot-check that other slots are 0. */
   TEST_ASSERT_EQUAL(0, conv[0]);
   TEST_ASSERT_EQUAL(0, conv[63]);
}

/* ------------------------------ summaries ------------------------------ */

static int64_t insert_summary_with_prov(int user_id, int64_t conv_id, int64_t start, int64_t end) {
   memory_provenance_t prov = { .conv_id = conv_id, .msg_id_start = start, .msg_id_end = end };
   int64_t id = 0;
   memory_db_summary_create(user_id, "sess", "summary text", "topics", "neutral", 1, 1,
                            (conv_id > 0) ? &prov : NULL, &id);
   return id;
}

void test_summaries_get_sources_roundtrip(void) {
   int64_t sid = insert_summary_with_prov(1, 10, 100, 101);
   TEST_ASSERT_GREATER_THAN(0, sid);
   int64_t conv[1] = { 0 }, start[1] = { 0 }, end[1] = { 0 };
   int rc = memory_db_summaries_get_sources(1, &sid, 1, conv, start, end);
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL(10, conv[0]);
   TEST_ASSERT_EQUAL(100, start[0]);
   TEST_ASSERT_EQUAL(101, end[0]);
}

void test_summaries_get_sources_null_prov_returns_zero(void) {
   int64_t sid = insert_summary_with_prov(1, 0, 0, 0);
   TEST_ASSERT_GREATER_THAN(0, sid);
   int64_t conv[1] = { 99 }, start[1] = { 99 }, end[1] = { 99 };
   int rc = memory_db_summaries_get_sources(1, &sid, 1, conv, start, end);
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL(0, conv[0]);
}

void test_summaries_get_sources_private_conv_filter(void) {
   int64_t sid = insert_summary_with_prov(1, 20, 200, 200); /* conv 20 private */
   TEST_ASSERT_GREATER_THAN(0, sid);
   int64_t conv[1] = { 99 }, start[1] = { 99 }, end[1] = { 99 };
   int rc = memory_db_summaries_get_sources(1, &sid, 1, conv, start, end);
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL(0, conv[0]);
}

void test_summaries_get_sources_n_chunked_succeeds(void) {
   int64_t sid = insert_summary_with_prov(1, 10, 100, 101);
   TEST_ASSERT_GREATER_THAN(0, sid);
   int64_t ids[64];
   int64_t conv[64] = { 0 }, start[64] = { 0 }, end[64] = { 0 };
   for (int i = 0; i < 64; i++)
      ids[i] = (i == 50) ? sid : (i + 1000);
   int rc = memory_db_summaries_get_sources(1, ids, 64, conv, start, end);
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL(10, conv[50]);
   TEST_ASSERT_EQUAL(100, start[50]);
   TEST_ASSERT_EQUAL(101, end[50]);
}

/* ------------------------------ preferences ------------------------------ */

/* memory_db_pref_upsert returns MEMORY_DB_SUCCESS but does not surface the row
 * id; the test queries it back directly. */
static int64_t pref_id_for_category(int user_id, const char *cat) {
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(s_db.db, "SELECT id FROM memory_preferences WHERE user_id=? AND category=?",
                      -1, &stmt, NULL);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, cat, -1, SQLITE_TRANSIENT);
   int64_t id = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW)
      id = sqlite3_column_int64(stmt, 0);
   sqlite3_finalize(stmt);
   return id;
}

void test_prefs_get_sources_roundtrip(void) {
   memory_provenance_t prov = { .conv_id = 10, .msg_id_start = 100, .msg_id_end = 101 };
   memory_db_pref_upsert(1, "theme", "dark", 0.8f, "inferred", &prov);
   int64_t pid = pref_id_for_category(1, "theme");
   TEST_ASSERT_GREATER_THAN(0, pid);
   int64_t conv[1] = { 0 }, start[1] = { 0 }, end[1] = { 0 };
   int rc = memory_db_prefs_get_sources(1, &pid, 1, conv, start, end);
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL(10, conv[0]);
   TEST_ASSERT_EQUAL(100, start[0]);
   TEST_ASSERT_EQUAL(101, end[0]);
}

void test_prefs_get_sources_null_prov_returns_zero(void) {
   memory_db_pref_upsert(1, "units", "metric", 0.8f, "inferred", NULL);
   int64_t pid = pref_id_for_category(1, "units");
   TEST_ASSERT_GREATER_THAN(0, pid);
   int64_t conv[1] = { 99 }, start[1] = { 99 }, end[1] = { 99 };
   int rc = memory_db_prefs_get_sources(1, &pid, 1, conv, start, end);
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL(0, conv[0]);
}

void test_prefs_get_sources_private_conv_filter(void) {
   memory_provenance_t prov = { .conv_id = 20, .msg_id_start = 200, .msg_id_end = 200 };
   memory_db_pref_upsert(1, "secret_pref", "x", 0.8f, "inferred", &prov);
   int64_t pid = pref_id_for_category(1, "secret_pref");
   TEST_ASSERT_GREATER_THAN(0, pid);
   int64_t conv[1] = { 99 }, start[1] = { 99 }, end[1] = { 99 };
   int rc = memory_db_prefs_get_sources(1, &pid, 1, conv, start, end);
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL(0, conv[0]);
}

void test_prefs_get_sources_n_chunked_succeeds(void) {
   memory_provenance_t prov = { .conv_id = 10, .msg_id_start = 100, .msg_id_end = 101 };
   memory_db_pref_upsert(1, "chunked_test", "x", 0.8f, "inferred", &prov);
   int64_t pid = pref_id_for_category(1, "chunked_test");
   TEST_ASSERT_GREATER_THAN(0, pid);
   int64_t ids[64];
   int64_t conv[64] = { 0 }, start[64] = { 0 }, end[64] = { 0 };
   for (int i = 0; i < 64; i++)
      ids[i] = (i == 50) ? pid : (i + 1000);
   int rc = memory_db_prefs_get_sources(1, ids, 64, conv, start, end);
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL(10, conv[50]);
   TEST_ASSERT_EQUAL(100, start[50]);
   TEST_ASSERT_EQUAL(101, end[50]);
}

/* Pin the existing facts batch reader against the same chunked-success
 * contract.  (Pre-Phase-B this silently truncated past ~32 IDs; B.1
 * introduced fail-closed; this fold-in re-expanded the public surface to any N
 * via internal chunking.) */
void test_facts_get_sources_n_chunked_succeeds(void) {
   memory_provenance_t prov = { .conv_id = 10, .msg_id_start = 100, .msg_id_end = 101 };
   int64_t fid = 0;
   memory_db_fact_create(1, "chunked fact", 0.9f, "inferred", "general", &prov, &fid);
   TEST_ASSERT_GREATER_THAN(0, fid);
   int64_t ids[64];
   int64_t conv[64] = { 0 }, start[64] = { 0 }, end[64] = { 0 };
   for (int i = 0; i < 64; i++)
      ids[i] = (i == 50) ? fid : (i + 1000);
   int rc = memory_db_facts_get_sources(1, ids, 64, conv, start, end);
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL(10, conv[50]);
   TEST_ASSERT_EQUAL(100, start[50]);
   TEST_ASSERT_EQUAL(101, end[50]);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_fact_create_with_source_roundtrip);
   RUN_TEST(test_fact_create_null_provenance_returns_not_found);
   RUN_TEST(test_null_bind_produces_sql_null_not_zero);
   RUN_TEST(test_get_source_wrong_user_returns_not_found);
   RUN_TEST(test_get_source_private_conv_returns_not_found);
   RUN_TEST(test_pref_upsert_with_source_roundtrip);
   RUN_TEST(test_pref_upsert_latest_source_wins);
   RUN_TEST(test_summary_create_with_source);
   RUN_TEST(test_summary_create_null_provenance);
   RUN_TEST(test_pref_upsert_null_provenance_sql_null);
   RUN_TEST(test_get_source_nonexistent_fact);
   RUN_TEST(test_conversations_have_last_extracted_msg_id_column);

   /* Phase B batch readers (12 new tests) */
   RUN_TEST(test_relations_get_sources_roundtrip);
   RUN_TEST(test_relations_get_sources_null_prov_returns_zero);
   RUN_TEST(test_relations_get_sources_private_conv_filter);
   RUN_TEST(test_relations_get_sources_n_chunked_succeeds);
   RUN_TEST(test_summaries_get_sources_roundtrip);
   RUN_TEST(test_summaries_get_sources_null_prov_returns_zero);
   RUN_TEST(test_summaries_get_sources_private_conv_filter);
   RUN_TEST(test_summaries_get_sources_n_chunked_succeeds);
   RUN_TEST(test_prefs_get_sources_roundtrip);
   RUN_TEST(test_prefs_get_sources_null_prov_returns_zero);
   RUN_TEST(test_prefs_get_sources_private_conv_filter);
   RUN_TEST(test_prefs_get_sources_n_chunked_succeeds);

   /* Pin the facts batch reader against the same chunked-success contract. */
   RUN_TEST(test_facts_get_sources_n_chunked_succeeds);
   return UNITY_END();
}
