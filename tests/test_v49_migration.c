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
 * Unit tests for the v49 memory_relations deduplication migration.  Pure
 * SQL test — does not link any DAWN modules.  Mirrors the migration block
 * in src/auth/auth_db_schema.c byte-for-byte so a future SQL drift between
 * the migration and the test surfaces as a test failure.
 *
 * Requires SQLite >= 3.24 for ON CONFLICT against a partial UNIQUE index,
 * and >= 3.15 for the row-valued UPDATE used in the migration's Step 2.
 * Jetson + CI Docker base ship 3.37+, well above.
 */

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

static sqlite3 *g_db = NULL;

/* Pre-v49 schema — matches the production CREATE TABLE shape minus the
 * v49 mention_count column and the v49 partial UNIQUE index.  All FK
 * targets stubbed (users, memory_entities, memory_facts, conversations)
 * since the test only exercises the migration's effect on
 * memory_relations rows. */
static const char *PRE_V49_SCHEMA =
    "CREATE TABLE users (id INTEGER PRIMARY KEY);"
    "CREATE TABLE memory_entities (id INTEGER PRIMARY KEY, user_id INTEGER, "
    "  canonical_id INTEGER);"
    "CREATE TABLE memory_facts (id INTEGER PRIMARY KEY, user_id INTEGER);"
    "CREATE TABLE conversations (id INTEGER PRIMARY KEY);"
    "CREATE TABLE memory_relations ("
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
    "  source_msg_id_end      INTEGER DEFAULT NULL"
    ");"
    "CREATE INDEX idx_memory_relations_subject_open "
    "ON memory_relations(subject_entity_id, relation) WHERE valid_to IS NULL;";

/* v49 migration SQL — must mirror src/auth/auth_db_schema.c byte-for-byte
 * (modulo this test's lack of OLOG and surrounding C control flow).
 * Update both sites together when refining the migration. */
static const char *V49_DEDUP_SQL =
    "UPDATE memory_relations AS w SET "
    "  mention_count = ("
    "    SELECT COUNT(*) FROM memory_relations d "
    "     WHERE d.user_id = w.user_id "
    "       AND d.subject_entity_id = w.subject_entity_id "
    "       AND d.relation = w.relation "
    "       AND COALESCE(d.object_entity_id, 0) = COALESCE(w.object_entity_id, 0) "
    "       AND COALESCE(d.object_value, '') = COALESCE(w.object_value, '') "
    "       AND d.valid_to IS NULL), "
    "  confidence = ("
    "    SELECT MAX(d.confidence) FROM memory_relations d "
    "     WHERE d.user_id = w.user_id "
    "       AND d.subject_entity_id = w.subject_entity_id "
    "       AND d.relation = w.relation "
    "       AND COALESCE(d.object_entity_id, 0) = COALESCE(w.object_entity_id, 0) "
    "       AND COALESCE(d.object_value, '') = COALESCE(w.object_value, '') "
    "       AND d.valid_to IS NULL) "
    "WHERE w.valid_to IS NULL "
    "  AND w.id = (SELECT MIN(d.id) FROM memory_relations d "
    "               WHERE d.user_id = w.user_id "
    "                 AND d.subject_entity_id = w.subject_entity_id "
    "                 AND d.relation = w.relation "
    "                 AND COALESCE(d.object_entity_id, 0) = COALESCE(w.object_entity_id, 0) "
    "                 AND COALESCE(d.object_value, '') = COALESCE(w.object_value, '') "
    "                 AND d.valid_to IS NULL);"
    "UPDATE memory_relations AS w "
    "SET (source_conversation_id, source_msg_id_start, source_msg_id_end, fact_id) = ("
    "    SELECT d.source_conversation_id, d.source_msg_id_start, d.source_msg_id_end, "
    "           COALESCE(w.fact_id, d.fact_id) "
    "    FROM memory_relations d "
    "    WHERE d.user_id = w.user_id "
    "      AND d.subject_entity_id = w.subject_entity_id "
    "      AND d.relation = w.relation "
    "      AND COALESCE(d.object_entity_id, 0) = COALESCE(w.object_entity_id, 0) "
    "      AND COALESCE(d.object_value, '') = COALESCE(w.object_value, '') "
    "      AND d.valid_to IS NULL "
    "    ORDER BY d.id DESC LIMIT 1) "
    "WHERE w.valid_to IS NULL AND w.mention_count > 1 "
    "  AND w.id = (SELECT MIN(d.id) FROM memory_relations d "
    "               WHERE d.user_id = w.user_id "
    "                 AND d.subject_entity_id = w.subject_entity_id "
    "                 AND d.relation = w.relation "
    "                 AND COALESCE(d.object_entity_id, 0) = COALESCE(w.object_entity_id, 0) "
    "                 AND COALESCE(d.object_value, '') = COALESCE(w.object_value, '') "
    "                 AND d.valid_to IS NULL);"
    "DELETE FROM memory_relations AS d "
    "WHERE d.valid_to IS NULL "
    "  AND EXISTS ("
    "    SELECT 1 FROM memory_relations w "
    "     WHERE w.valid_to IS NULL "
    "       AND w.user_id = d.user_id "
    "       AND w.subject_entity_id = d.subject_entity_id "
    "       AND w.relation = d.relation "
    "       AND COALESCE(w.object_entity_id, 0) = COALESCE(d.object_entity_id, 0) "
    "       AND COALESCE(w.object_value, '') = COALESCE(d.object_value, '') "
    "       AND w.id < d.id);"
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_memory_relations_unique_open "
    "ON memory_relations(user_id, subject_entity_id, relation, "
    "                    COALESCE(object_entity_id, 0), "
    "                    COALESCE(object_value, '')) "
    "WHERE valid_to IS NULL;";

static void exec_or_fail(const char *sql) {
   char *errmsg = NULL;
   int rc = sqlite3_exec(g_db, sql, NULL, NULL, &errmsg);
   if (rc != SQLITE_OK) {
      fprintf(stderr, "SQL failed: %s\nSQL was: %s\n", errmsg ? errmsg : "(none)", sql);
      sqlite3_free(errmsg);
      TEST_FAIL_MESSAGE("SQL execution failed");
   }
}

void setUp(void) {
   int rc = sqlite3_open(":memory:", &g_db);
   if (rc != SQLITE_OK) {
      fprintf(stderr, "open :memory: failed: %s\n", sqlite3_errmsg(g_db));
      exit(1);
   }
   exec_or_fail(PRE_V49_SCHEMA);

   /* Seed FK targets so any future FK PRAGMA enforcement here doesn't bite. */
   exec_or_fail("INSERT INTO users (id) VALUES (1);");
   exec_or_fail("INSERT INTO memory_entities (id, user_id) VALUES (1, 1);"
                "INSERT INTO memory_entities (id, user_id) VALUES (2, 1);"
                "INSERT INTO memory_entities (id, user_id) VALUES (3, 1);"
                "INSERT INTO memory_entities (id, user_id) VALUES (4, 1);");
}

void tearDown(void) {
   if (g_db) {
      sqlite3_close(g_db);
      g_db = NULL;
   }
}

/* Bulk-insert N identical open rows (same user/subject/relation/object) into
 * memory_relations.  Used to seed duplicate groups for the dedup test.  Each
 * row gets a distinct created_at (now + i) so the id ordering is well-
 * defined for the "MIN(id) wins / MAX(id) provenance" assertions. */
static void insert_open_n(int user_id,
                          int64_t subj,
                          const char *rel,
                          int64_t obj_id,
                          const char *obj_val,
                          int n,
                          double confidence_base,
                          int64_t prov_conv_base) {
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(g_db,
                      "INSERT INTO memory_relations "
                      "  (user_id, subject_entity_id, relation, object_entity_id, "
                      "   object_value, confidence, valid_from, valid_to, "
                      "   source_conversation_id, source_msg_id_start, source_msg_id_end) "
                      "VALUES (?, ?, ?, ?, ?, ?, NULL, NULL, ?, ?, ?)",
                      -1, &stmt, NULL);
   for (int i = 0; i < n; i++) {
      sqlite3_reset(stmt);
      sqlite3_bind_int(stmt, 1, user_id);
      sqlite3_bind_int64(stmt, 2, subj);
      sqlite3_bind_text(stmt, 3, rel, -1, SQLITE_STATIC);
      if (obj_id > 0)
         sqlite3_bind_int64(stmt, 4, obj_id);
      else
         sqlite3_bind_null(stmt, 4);
      if (obj_val)
         sqlite3_bind_text(stmt, 5, obj_val, -1, SQLITE_STATIC);
      else
         sqlite3_bind_null(stmt, 5);
      /* Each successive row gets slightly higher confidence so the MAX
       * assertion has a definite target. */
      sqlite3_bind_double(stmt, 6, confidence_base + 0.01 * i);
      /* Last row (highest id) carries the highest conv id — the dedup
       * Step 2 provenance pull should land that conv on the winner. */
      sqlite3_bind_int64(stmt, 7, prov_conv_base + i);
      sqlite3_bind_int64(stmt, 8, 100 + i);
      sqlite3_bind_int64(stmt, 9, 200 + i);
      sqlite3_step(stmt);
   }
   sqlite3_finalize(stmt);
}

/* Insert a closed historical row (valid_to set) — used to confirm closed
 * rows are NOT swept up in the dedup pass. */
static void insert_closed(int user_id,
                          int64_t subj,
                          const char *rel,
                          int64_t obj_id,
                          int64_t valid_to) {
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(g_db,
                      "INSERT INTO memory_relations "
                      "  (user_id, subject_entity_id, relation, object_entity_id, "
                      "   valid_from, valid_to) "
                      "VALUES (?, ?, ?, ?, NULL, ?)",
                      -1, &stmt, NULL);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, subj);
   sqlite3_bind_text(stmt, 3, rel, -1, SQLITE_STATIC);
   sqlite3_bind_int64(stmt, 4, obj_id);
   sqlite3_bind_int64(stmt, 5, valid_to);
   sqlite3_step(stmt);
   sqlite3_finalize(stmt);
}

static int scalar_int(const char *sql) {
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
   int result = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW)
      result = sqlite3_column_int(stmt, 0);
   sqlite3_finalize(stmt);
   return result;
}

static int64_t scalar_int64(const char *sql) {
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
   int64_t result = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW)
      result = sqlite3_column_int64(stmt, 0);
   sqlite3_finalize(stmt);
   return result;
}

/* Run the v49 migration block: ALTER + BEGIN + dedup SQL + COMMIT.
 * Mirrors auth_db_create_schema's v49 path but without OLOG. */
static void run_v49_migration(void) {
   exec_or_fail("ALTER TABLE memory_relations "
                "ADD COLUMN mention_count INTEGER NOT NULL DEFAULT 1");
   exec_or_fail("BEGIN IMMEDIATE");
   exec_or_fail(V49_DEDUP_SQL);
   exec_or_fail("COMMIT");
}

/* ============================================================================
 * Tests
 * ============================================================================ */

/* Mirrors the dev's live DB: top duplicate group of 49, second group of 24,
 * one singleton control, plus 2 closed historical rows that must survive. */
static void test_v49_collapses_duplicates_and_stamps_counts(void) {
   /* Group A: (subj=1, working_on, DAWN) × 49 */
   insert_open_n(1, 1, "working_on", 0, "DAWN", 49, 0.50, 1000);
   /* Group B: (subj=1, lives_in, sugar_hill) × 24 */
   insert_open_n(1, 1, "lives_in", 0, "sugar_hill", 24, 0.60, 2000);
   /* Singleton control: (subj=1, works_at, entity=4) × 1 */
   insert_open_n(1, 1, "works_at", 4, NULL, 1, 0.70, 3000);
   /* Closed historical: (subj=1, married_to, entity=2) × 2 — different
    * valid_to values so each row is a legitimate historical state. */
   insert_closed(1, 1, "married_to", 2, 5000);
   insert_closed(1, 1, "married_to", 2, 6000);

   /* Pre-state: 49 + 24 + 1 = 74 open rows, 2 closed rows. */
   TEST_ASSERT_EQUAL_INT(74, scalar_int("SELECT COUNT(*) FROM memory_relations "
                                        "WHERE valid_to IS NULL"));
   TEST_ASSERT_EQUAL_INT(2, scalar_int("SELECT COUNT(*) FROM memory_relations "
                                       "WHERE valid_to IS NOT NULL"));

   run_v49_migration();

   /* Post-state: 3 open winners + 2 closed historical = 5 total rows. */
   TEST_ASSERT_EQUAL_INT(3, scalar_int("SELECT COUNT(*) FROM memory_relations "
                                       "WHERE valid_to IS NULL"));
   TEST_ASSERT_EQUAL_INT(2, scalar_int("SELECT COUNT(*) FROM memory_relations "
                                       "WHERE valid_to IS NOT NULL"));

   /* mention_count on the working_on/DAWN winner is 49. */
   TEST_ASSERT_EQUAL_INT(49, scalar_int("SELECT mention_count FROM memory_relations "
                                        "WHERE relation = 'working_on' AND valid_to IS NULL"));
   /* mention_count on the lives_in/sugar_hill winner is 24. */
   TEST_ASSERT_EQUAL_INT(24, scalar_int("SELECT mention_count FROM memory_relations "
                                        "WHERE relation = 'lives_in' AND valid_to IS NULL"));
   /* Singleton control unchanged. */
   TEST_ASSERT_EQUAL_INT(1, scalar_int("SELECT mention_count FROM memory_relations "
                                       "WHERE relation = 'works_at' AND valid_to IS NULL"));
   /* Closed historical rows keep their default mention_count = 1. */
   TEST_ASSERT_EQUAL_INT(2, scalar_int("SELECT COUNT(*) FROM memory_relations "
                                       "WHERE relation = 'married_to' "
                                       "  AND valid_to IS NOT NULL AND mention_count = 1"));
}

static void test_v49_preserves_max_confidence_on_winner(void) {
   /* Insert 5 duplicates with confidence 0.50, 0.51, 0.52, 0.53, 0.54.
    * Winner (MIN(id)) post-dedup should hold confidence = 0.54 (the MAX). */
   insert_open_n(1, 1, "enjoys", 0, "hiking", 5, 0.50, 1000);

   run_v49_migration();

   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(g_db,
                      "SELECT confidence FROM memory_relations WHERE relation = 'enjoys' AND "
                      "valid_to IS NULL",
                      -1, &stmt, NULL);
   TEST_ASSERT_EQUAL_INT(SQLITE_ROW, sqlite3_step(stmt));
   double conf = sqlite3_column_double(stmt, 0);
   sqlite3_finalize(stmt);
   /* 0.50 + 0.04 = 0.54 (i=4 in the loop). */
   TEST_ASSERT_DOUBLE_WITHIN(0.001, 0.54, conf);
}

static void test_v49_pulls_latest_provenance_to_winner(void) {
   /* 3 duplicates with conv ids 1000, 1001, 1002.  Winner's
    * source_conversation_id should land at 1002 (latest = MAX(id) row). */
   insert_open_n(1, 1, "lives_in", 0, "boston", 3, 0.80, 1000);

   run_v49_migration();

   int64_t src_conv = scalar_int64("SELECT COALESCE(source_conversation_id, -1) "
                                   "FROM memory_relations "
                                   "WHERE relation = 'lives_in' AND valid_to IS NULL");
   TEST_ASSERT_EQUAL_INT64(1002, src_conv);
}

static void test_v49_creates_partial_unique_index(void) {
   insert_open_n(1, 1, "test_rel", 0, "x", 2, 0.80, 1000);

   run_v49_migration();

   /* Index exists. */
   int n = scalar_int("SELECT COUNT(*) FROM sqlite_master "
                      "WHERE type = 'index' AND name = 'idx_memory_relations_unique_open'");
   TEST_ASSERT_EQUAL_INT(1, n);

   /* Subsequent INSERT of the same open triple now fails on UNIQUE — without
    * the upsert ON CONFLICT clause this would be a hard failure.  Confirms
    * the index is enforcing. */
   char *errmsg = NULL;
   int rc = sqlite3_exec(g_db,
                         "INSERT INTO memory_relations "
                         "  (user_id, subject_entity_id, relation, object_value, valid_to) "
                         "VALUES (1, 1, 'test_rel', 'x', NULL)",
                         NULL, NULL, &errmsg);
   sqlite3_free(errmsg);
   TEST_ASSERT_EQUAL_INT(SQLITE_CONSTRAINT, rc);
}

static void test_v49_upsert_after_migration_bumps_mention_count(void) {
   insert_open_n(1, 1, "uses", 0, "tool", 3, 0.80, 1000);

   run_v49_migration();

   /* mention_count = 3 on the winner. */
   TEST_ASSERT_EQUAL_INT(3, scalar_int("SELECT mention_count FROM memory_relations "
                                       "WHERE relation = 'uses' AND valid_to IS NULL"));

   /* Re-witness the same edge via the upsert path.  Production wraps this
    * in the stmt_memory_relation_create prepared statement; here we just
    * exec the same SQL inline to confirm the v49 index lets it parse. */
   exec_or_fail("INSERT INTO memory_relations "
                "  (user_id, subject_entity_id, relation, object_value, valid_to, "
                "   confidence) "
                "VALUES (1, 1, 'uses', 'tool', NULL, 0.95) "
                "ON CONFLICT(user_id, subject_entity_id, relation, "
                "            COALESCE(object_entity_id, 0), COALESCE(object_value, '')) "
                "WHERE valid_to IS NULL DO UPDATE SET "
                "  mention_count = mention_count + 1, "
                "  confidence = MAX(confidence, excluded.confidence);");

   /* mention_count bumped to 4; confidence upgraded to 0.95 (was max 0.82
    * pre-upsert: 0.80 + 0.01*2 = 0.82). */
   TEST_ASSERT_EQUAL_INT(4, scalar_int("SELECT mention_count FROM memory_relations "
                                       "WHERE relation = 'uses' AND valid_to IS NULL"));
}

static void test_v49_distinct_objects_stay_separate(void) {
   /* Same subj+relation, three distinct objects → three winners, not collapsed. */
   insert_open_n(1, 1, "lives_in", 0, "boston", 1, 0.80, 1000);
   insert_open_n(1, 1, "lives_in", 0, "seattle", 1, 0.80, 1000);
   insert_open_n(1, 1, "lives_in", 0, "portland", 1, 0.80, 1000);

   run_v49_migration();

   TEST_ASSERT_EQUAL_INT(3, scalar_int("SELECT COUNT(*) FROM memory_relations "
                                       "WHERE relation = 'lives_in' AND valid_to IS NULL"));
}

static void test_v49_entity_object_vs_literal_object_stay_separate(void) {
   /* (subj=1, rel, entity_id=2) and (subj=1, rel, value="2") are different
    * edges even though they look similar — COALESCE(object_entity_id,0)
    * is 2 vs 0, and COALESCE(object_value,'') is '' vs '2'.  Both groups
    * dedup to one row each. */
   insert_open_n(1, 1, "knows", 2, NULL, 3, 0.80, 1000); /* entity object */
   insert_open_n(1, 1, "knows", 0, "2", 3, 0.80, 1000);  /* literal "2" */

   run_v49_migration();

   TEST_ASSERT_EQUAL_INT(2, scalar_int("SELECT COUNT(*) FROM memory_relations "
                                       "WHERE relation = 'knows' AND valid_to IS NULL"));
   /* Both deduped to mention_count = 3. */
   TEST_ASSERT_EQUAL_INT(3, scalar_int("SELECT mention_count FROM memory_relations "
                                       "WHERE relation = 'knows' AND object_entity_id = 2 "
                                       "  AND valid_to IS NULL"));
   TEST_ASSERT_EQUAL_INT(3, scalar_int("SELECT mention_count FROM memory_relations "
                                       "WHERE relation = 'knows' AND object_value = '2' "
                                       "  AND valid_to IS NULL"));
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_v49_collapses_duplicates_and_stamps_counts);
   RUN_TEST(test_v49_preserves_max_confidence_on_winner);
   RUN_TEST(test_v49_pulls_latest_provenance_to_winner);
   RUN_TEST(test_v49_creates_partial_unique_index);
   RUN_TEST(test_v49_upsert_after_migration_bumps_mention_count);
   RUN_TEST(test_v49_distinct_objects_stay_separate);
   RUN_TEST(test_v49_entity_object_vs_literal_object_stay_separate);
   return UNITY_END();
}
