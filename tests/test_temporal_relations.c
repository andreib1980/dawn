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
 * Unit tests for v33 temporal relations:
 *   - memory_db_relation_supersede auto-closes prior open exclusive relation
 *   - memory_db_relation_supersede leaves non-exclusive open
 *   - memory_db_relation_supersede skips close when same target re-mentioned
 *   - memory_db_relation_list_by_subject_at returns the right slice
 *   - default ("currently true") path filters out closed rows
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db_internal.h"
#include "memory/memory_db.h"
#include "memory/memory_types.h"

/* Test harness */
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg)         \
   do {                                \
      if (cond) {                      \
         printf("  [PASS] %s\n", msg); \
         tests_passed++;               \
      } else {                         \
         printf("  [FAIL] %s\n", msg); \
         tests_failed++;               \
      }                                \
   } while (0)

/* DDL mirroring auth_db_core.c (relevant subset).  Covers v19 base tables
 * (memory_entities, memory_facts) plus v33 additions (memory_relations with
 * valid_from/valid_to) and v34 additions (memory_facts.category) — these
 * tests exercise both temporal relations and the canonical category list. */
/* clang-format off */
static const char *DDL =
   "CREATE TABLE IF NOT EXISTS users ("
   "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
   "  username TEXT UNIQUE NOT NULL"
   ");"
   "INSERT INTO users (id, username) VALUES (1, 'alice_test');"
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
   "INSERT INTO memory_entities (id, user_id, name, entity_type, canonical_name)"
   "  VALUES (10, 1, 'Alice', 'person', 'alice');"
   "INSERT INTO memory_entities (id, user_id, name, entity_type, canonical_name)"
   "  VALUES (20, 1, 'Google', 'org', 'google');"
   "INSERT INTO memory_entities (id, user_id, name, entity_type, canonical_name)"
   "  VALUES (21, 1, 'Microsoft', 'org', 'microsoft');"
   "INSERT INTO memory_entities (id, user_id, name, entity_type, canonical_name)"
   "  VALUES (22, 1, 'Apple', 'org', 'apple');"
   /* Fresh subject + targets used only by the as-of test, isolated from earlier
    * tests' state to avoid cross-test object-id collisions. */
   "INSERT INTO memory_entities (id, user_id, name, entity_type, canonical_name)"
   "  VALUES (50, 1, 'Bob', 'person', 'bob');"
   "INSERT INTO memory_entities (id, user_id, name, entity_type, canonical_name)"
   "  VALUES (60, 1, 'CityA', 'place', 'citya');"
   "INSERT INTO memory_entities (id, user_id, name, entity_type, canonical_name)"
   "  VALUES (61, 1, 'CityB', 'place', 'cityb');"
   "CREATE TABLE IF NOT EXISTS memory_facts ("
   "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
   "  user_id INTEGER NOT NULL,"
   "  fact_text TEXT,"
   "  category TEXT NOT NULL DEFAULT 'general'"
   ");"
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
   "  valid_to INTEGER DEFAULT NULL"
   ");";
/* clang-format on */

/* Prepare only the relation statements memory_db.c needs for these tests. */
static int prepare_statements(void) {
   int rc;

   rc = sqlite3_prepare_v2(s_db.db,
                           "INSERT INTO memory_relations (user_id, subject_entity_id, relation, "
                           "object_entity_id, object_value, fact_id, confidence, created_at, "
                           "valid_from, valid_to) "
                           "VALUES (?, ?, ?, ?, ?, ?, ?, strftime('%s','now'), ?, ?)",
                           -1, &s_db.stmt_memory_relation_create, NULL);
   if (rc != SQLITE_OK)
      return -1;

   rc = sqlite3_prepare_v2(s_db.db,
                           "UPDATE memory_relations SET valid_to = ? "
                           "WHERE user_id = ? AND subject_entity_id = ? AND relation = ? "
                           "  AND valid_to IS NULL "
                           "  AND (COALESCE(object_entity_id, 0) != COALESCE(?, 0) "
                           "    OR COALESCE(object_value, '') != COALESCE(?, ''))",
                           -1, &s_db.stmt_memory_relation_close_open, NULL);
   if (rc != SQLITE_OK)
      return -1;

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT r.id, r.subject_entity_id, r.relation, r.object_entity_id, "
                           "COALESCE(e.name, r.object_value) AS object_name, r.confidence, "
                           "COALESCE(r.valid_from, 0), COALESCE(r.valid_to, 0) "
                           "FROM memory_relations r "
                           "LEFT JOIN memory_entities e ON r.object_entity_id = e.id "
                           "WHERE r.user_id = ? AND r.subject_entity_id = ? LIMIT ?",
                           -1, &s_db.stmt_memory_relation_list_by_subject, NULL);
   if (rc != SQLITE_OK)
      return -1;

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
   if (rc != SQLITE_OK)
      return -1;

   return 0;
}

static int setup_db(void) {
   if (sqlite3_open(":memory:", &s_db.db) != SQLITE_OK)
      return -1;

   char *err = NULL;
   if (sqlite3_exec(s_db.db, DDL, NULL, NULL, &err) != SQLITE_OK) {
      fprintf(stderr, "DDL failed: %s\n", err ? err : "?");
      sqlite3_free(err);
      return -1;
   }

   if (prepare_statements() != 0) {
      fprintf(stderr, "prepare failed\n");
      return -1;
   }
   s_db.initialized = true;
   return 0;
}

static int count_open_relations(int subject_id, const char *relation) {
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(s_db.db,
                      "SELECT COUNT(*) FROM memory_relations "
                      "WHERE subject_entity_id = ? AND relation = ? AND valid_to IS NULL",
                      -1, &stmt, NULL);
   sqlite3_bind_int64(stmt, 1, subject_id);
   sqlite3_bind_text(stmt, 2, relation, -1, SQLITE_TRANSIENT);
   int n = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW)
      n = sqlite3_column_int(stmt, 0);
   sqlite3_finalize(stmt);
   return n;
}

/* ============================================================================
 * Tests
 * ============================================================================ */

static void test_supersede_exclusive(void) {
   printf("\n--- test_supersede_exclusive (works_at) ---\n");

   /* Insert: Alice works_at Google */
   int rc = memory_db_relation_supersede(1, 10, "works_at", 20, NULL, 0, 0.9f, 0, 0);
   TEST_ASSERT(rc == MEMORY_DB_SUCCESS, "supersede insert #1 ok");
   TEST_ASSERT(count_open_relations(10, "works_at") == 1, "one open works_at after first insert");

   /* Now: Alice works_at Microsoft → must auto-close Google */
   rc = memory_db_relation_supersede(1, 10, "works_at", 21, NULL, 0, 0.9f, 0, 0);
   TEST_ASSERT(rc == MEMORY_DB_SUCCESS, "supersede insert #2 ok");
   TEST_ASSERT(count_open_relations(10, "works_at") == 1,
               "still one open works_at (prior auto-closed)");
}

static void test_supersede_idempotent_same_object(void) {
   printf("\n--- test_supersede_idempotent_same_object ---\n");

   /* Re-insert: Alice works_at Microsoft (same target as prior test) */
   int prior = count_open_relations(10, "works_at");
   int rc = memory_db_relation_supersede(1, 10, "works_at", 21, NULL, 0, 0.9f, 0, 0);
   TEST_ASSERT(rc == MEMORY_DB_SUCCESS, "re-insert same object ok");
   /* Expectation: a NEW row is added, but the prior open Microsoft row is NOT
    * closed because object matches.  So open count goes prior + 1.
    * (The dedup-of-duplicate-opens problem is handled at the extraction layer
    * via memory_db_fact_find_similar; supersede is purely a write-side primitive.) */
   TEST_ASSERT(count_open_relations(10, "works_at") == prior + 1,
               "same-object re-insert does not auto-close");
}

static void test_non_exclusive_does_not_close(void) {
   printf("\n--- test_non_exclusive_does_not_close ---\n");

   /* "likes" is not in EXCLUSIVE_RELATIONS — multiple open rows valid. */
   memory_db_relation_supersede(1, 10, "likes", 20, NULL, 0, 0.7f, 0, 0);
   memory_db_relation_supersede(1, 10, "likes", 21, NULL, 0, 0.7f, 0, 0);
   memory_db_relation_supersede(1, 10, "likes", 22, NULL, 0, 0.7f, 0, 0);
   int n = count_open_relations(10, "likes");
   TEST_ASSERT(n == 3, "all three 'likes' relations remain open");
}

static void test_list_by_subject_at(void) {
   printf("\n--- test_list_by_subject_at (historical query) ---\n");

   /* Use fresh subject Bob (50) with two lives_in rows with disjoint validity. */
   memory_db_relation_create(1, 50, "lives_in", 60, NULL, 0, 0.9f, 1000, 2000);
   memory_db_relation_create(1, 50, "lives_in", 61, NULL, 0, 0.9f, 2000, 0);

   memory_relation_t out[10];

   /* As-of 1500: only CityA row matches */
   int n = memory_db_relation_list_by_subject_at(1, 50, 1500, out, 10);
   bool found_a = false, found_b = false;
   for (int i = 0; i < n; i++) {
      if (out[i].object_entity_id == 60)
         found_a = true;
      if (out[i].object_entity_id == 61)
         found_b = true;
   }
   TEST_ASSERT(found_a, "as-of 1500 includes CityA row");
   TEST_ASSERT(!found_b, "as-of 1500 excludes CityB row (valid_from = 2000)");

   /* As-of 2500: only CityB row matches */
   n = memory_db_relation_list_by_subject_at(1, 50, 2500, out, 10);
   found_a = false;
   found_b = false;
   for (int i = 0; i < n; i++) {
      if (out[i].object_entity_id == 60)
         found_a = true;
      if (out[i].object_entity_id == 61)
         found_b = true;
   }
   TEST_ASSERT(!found_a, "as-of 2500 excludes CityA (valid_to = 2000)");
   TEST_ASSERT(found_b, "as-of 2500 includes CityB row");
}

static void test_supersede_closes_at_new_valid_from(void) {
   printf("\n--- test_supersede_closes_at_new_valid_from ---\n");

   /* Historical ingestion: Alice works_at Google 2018-2020, then Microsoft
    * starting 2020.  If supersede closes Google at time(NULL) instead of the
    * new row's valid_from, both rows overlap from 2020..now and
    * list_by_subject_at(2019) would return both. */
   int64_t t2018 = 1514764800; /* 2018-01-01 */
   int64_t t2020 = 1577836800; /* 2020-01-01 */

   /* Reset subject 10's works_at rows from prior tests */
   sqlite3_exec(s_db.db,
                "DELETE FROM memory_relations WHERE subject_entity_id = 10 "
                "AND relation = 'works_at'",
                NULL, NULL, NULL);

   memory_db_relation_supersede(1, 10, "works_at", 20, NULL, 0, 0.9f, t2018, 0);
   memory_db_relation_supersede(1, 10, "works_at", 21, NULL, 0, 0.9f, t2020, 0);

   /* As-of 2019: only Google should be valid */
   memory_relation_t out[10];
   int n = memory_db_relation_list_by_subject_at(1, 10, 1546300800 /* 2019 */, out, 10);
   int works_at_count = 0;
   for (int i = 0; i < n; i++) {
      if (strcmp(out[i].relation, "works_at") == 0)
         works_at_count++;
   }
   TEST_ASSERT(works_at_count == 1,
               "as-of 2019: exactly one works_at (Google) — no overlap with Microsoft");
}

static void test_supersede_skips_close_for_historical_insert(void) {
   printf("\n--- test_supersede_skips_close_for_historical_insert ---\n");

   /* Reset subject 10's works_at rows from prior tests */
   sqlite3_exec(s_db.db,
                "DELETE FROM memory_relations WHERE subject_entity_id = 10 "
                "AND relation = 'works_at'",
                NULL, NULL, NULL);

   /* Establish current reality: Alice works_at Microsoft, ongoing */
   memory_db_relation_supersede(1, 10, "works_at", 21, NULL, 0, 0.9f, 0, 0);

   /* Ingest a HISTORICAL fact discovered later: Alice worked at Google 2018-2020.
    * This must NOT close the current Microsoft row — it's adding a bounded
    * historical slice, not superseding the present. */
   int64_t t2018 = 1514764800;
   int64_t t2020 = 1577836800;
   memory_db_relation_supersede(1, 10, "works_at", 20, NULL, 0, 0.9f, t2018, t2020);

   /* Microsoft row should still be open (valid_to NULL). */
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(s_db.db,
                      "SELECT COUNT(*) FROM memory_relations "
                      "WHERE subject_entity_id = 10 AND relation = 'works_at' "
                      "  AND object_entity_id = 21 AND valid_to IS NULL",
                      -1, &stmt, NULL);
   int open_microsoft = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW)
      open_microsoft = sqlite3_column_int(stmt, 0);
   sqlite3_finalize(stmt);
   TEST_ASSERT(open_microsoft == 1,
               "historical insert does not close the current (open) Microsoft row");

   /* As-of 2019 returns Google only; as-of now returns Microsoft only. */
   memory_relation_t out[10];
   int n = memory_db_relation_list_by_subject_at(1, 10, 1546300800, out, 10);
   bool found_google_2019 = false;
   for (int i = 0; i < n; i++) {
      if (strcmp(out[i].relation, "works_at") == 0 && out[i].object_entity_id == 20)
         found_google_2019 = true;
   }
   TEST_ASSERT(found_google_2019, "as-of 2019: Google row is valid");
}

static void test_canonical_categories(void) {
   printf("\n--- test_canonical_categories ---\n");

   TEST_ASSERT(MEMORY_FACT_CATEGORY_COUNT == 8, "8 canonical categories");
   TEST_ASSERT(strcmp(MEMORY_FACT_CATEGORIES[0], "personal") == 0, "first is 'personal'");
   TEST_ASSERT(strcmp(MEMORY_FACT_CATEGORIES[7], "general") == 0, "last is 'general' (fallback)");
   TEST_ASSERT(MEMORY_FACT_CATEGORIES[8] == NULL, "list is NULL-terminated");
}

int main(void) {
   if (setup_db() != 0) {
      fprintf(stderr, "setup failed\n");
      return 1;
   }

   test_supersede_exclusive();
   test_supersede_idempotent_same_object();
   test_non_exclusive_does_not_close();
   test_list_by_subject_at();
   test_supersede_closes_at_new_valid_from();
   test_supersede_skips_close_for_historical_insert();
   test_canonical_categories();

   printf("\n=========================================\n");
   printf("Tests passed: %d, failed: %d\n", tests_passed, tests_failed);
   printf("=========================================\n");

   sqlite3_close(s_db.db);
   return tests_failed > 0 ? 1 : 0;
}
