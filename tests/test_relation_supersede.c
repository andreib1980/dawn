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
 * Unit tests for relation-driven fact supersede.  Verifies that when an
 * exclusive relation is superseded, the old relation's linked fact_id is
 * returned so the caller can propagate the supersede to the fact layer.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db_internal.h"
#include "memory/memory_db.h"

/* ============================================================================
 * Test Harness
 * ============================================================================ */

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(condition, msg)    \
   do {                                \
      if (condition) {                 \
         printf("  [PASS] %s\n", msg); \
         tests_passed++;               \
      } else {                         \
         printf("  [FAIL] %s\n", msg); \
         tests_failed++;               \
      }                                \
   } while (0)

/* ============================================================================
 * Schema + Statement Setup
 * ============================================================================ */

static const char *DDL = "CREATE TABLE IF NOT EXISTS memory_facts ("
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
                         "  embedding_norm REAL DEFAULT NULL"
                         ");"
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
                         "  UNIQUE(user_id, canonical_name)"
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

static void setup_db(void) {
   memset(&s_db, 0, sizeof(s_db));
   int rc = sqlite3_open(":memory:", &s_db.db);
   if (rc != SQLITE_OK) {
      fprintf(stderr, "Failed to open in-memory DB: %s\n", sqlite3_errmsg(s_db.db));
      exit(1);
   }
   pthread_mutex_init(&s_db.mutex, NULL);

   char *errmsg = NULL;
   rc = sqlite3_exec(s_db.db, DDL, NULL, NULL, &errmsg);
   if (rc != SQLITE_OK) {
      fprintf(stderr, "DDL failed: %s\n", errmsg);
      sqlite3_free(errmsg);
      exit(1);
   }

   /* Prepare statements used by memory_db_relation_supersede() */
   rc = sqlite3_prepare_v2(s_db.db,
                           "INSERT INTO memory_relations (user_id, subject_entity_id, relation, "
                           "object_entity_id, object_value, fact_id, confidence, created_at, "
                           "valid_from, valid_to) "
                           "VALUES (?, ?, ?, ?, ?, ?, ?, strftime('%s','now'), ?, ?)",
                           -1, &s_db.stmt_memory_relation_create, NULL);
   if (rc != SQLITE_OK) {
      fprintf(stderr, "prepare relation_create failed: %s\n", sqlite3_errmsg(s_db.db));
      exit(1);
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "UPDATE memory_relations SET valid_to = ? "
                           "WHERE user_id = ? AND subject_entity_id = ? AND relation = ? "
                           "  AND valid_to IS NULL "
                           "  AND (COALESCE(object_entity_id, 0) != COALESCE(?, 0) "
                           "    OR COALESCE(object_value, '') != COALESCE(?, '')) "
                           "RETURNING fact_id",
                           -1, &s_db.stmt_memory_relation_close_open, NULL);
   if (rc != SQLITE_OK) {
      fprintf(stderr, "prepare relation_close_open failed: %s\n", sqlite3_errmsg(s_db.db));
      exit(1);
   }

   /* Prepare statements used by memory_db_fact_supersede() */
   rc = sqlite3_prepare_v2(s_db.db, "UPDATE memory_facts SET superseded_by = ? WHERE id = ?", -1,
                           &s_db.stmt_memory_fact_supersede, NULL);
   if (rc != SQLITE_OK) {
      fprintf(stderr, "prepare fact_supersede failed: %s\n", sqlite3_errmsg(s_db.db));
      exit(1);
   }

   s_db.initialized = true;
}

static void teardown_db(void) {
   if (s_db.stmt_memory_relation_create)
      sqlite3_finalize(s_db.stmt_memory_relation_create);
   if (s_db.stmt_memory_relation_close_open)
      sqlite3_finalize(s_db.stmt_memory_relation_close_open);
   if (s_db.stmt_memory_fact_supersede)
      sqlite3_finalize(s_db.stmt_memory_fact_supersede);
   if (s_db.db)
      sqlite3_close(s_db.db);
   memset(&s_db, 0, sizeof(s_db));
}

/* ============================================================================
 * Helpers — direct SQL inserts for test setup
 * ============================================================================ */

static int64_t insert_fact(int user_id, const char *text) {
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(s_db.db, "INSERT INTO memory_facts (user_id, fact_text) VALUES (?, ?)", -1,
                      &stmt, NULL);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, text, -1, SQLITE_STATIC);
   sqlite3_step(stmt);
   int64_t id = sqlite3_last_insert_rowid(s_db.db);
   sqlite3_finalize(stmt);
   return id;
}

static int64_t insert_entity(int user_id, const char *name, const char *type) {
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(s_db.db,
                      "INSERT INTO memory_entities (user_id, name, entity_type, canonical_name) "
                      "VALUES (?, ?, ?, ?)",
                      -1, &stmt, NULL);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, name, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 3, type, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 4, name, -1, SQLITE_STATIC);
   sqlite3_step(stmt);
   int64_t id = sqlite3_last_insert_rowid(s_db.db);
   sqlite3_finalize(stmt);
   return id;
}

static int64_t get_fact_superseded_by(int64_t fact_id) {
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(s_db.db, "SELECT superseded_by FROM memory_facts WHERE id = ?", -1, &stmt,
                      NULL);
   sqlite3_bind_int64(stmt, 1, fact_id);
   int64_t result = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
      result = sqlite3_column_int64(stmt, 0);
   }
   sqlite3_finalize(stmt);
   return result;
}

/* ============================================================================
 * Tests
 * ============================================================================ */

static void test_exclusive_supersede_returns_old_fact_id(void) {
   printf("\n--- test_exclusive_supersede_returns_old_fact_id ---\n");

   int user_id = 1;
   int64_t fact_a = insert_fact(user_id, "Alice works at Google");
   int64_t alice = insert_entity(user_id, "alice", "person");
   int64_t google = insert_entity(user_id, "google", "org");
   int64_t microsoft = insert_entity(user_id, "microsoft", "org");

   /* Create initial relation with fact_id */
   memory_db_relation_supersede(user_id, alice, "works_at", google, NULL, fact_a, 0.9f, 0, 0, NULL);

   /* Supersede with new employer */
   int64_t fact_b = insert_fact(user_id, "Alice works at Microsoft");
   int64_t old_fact_id = 0;
   int rc = memory_db_relation_supersede(user_id, alice, "works_at", microsoft, NULL, fact_b, 0.9f,
                                         0, 0, &old_fact_id);

   TEST_ASSERT(rc == MEMORY_DB_SUCCESS, "supersede returns SUCCESS");
   TEST_ASSERT(old_fact_id == fact_a, "old_fact_id matches original fact");

   /* Propagate supersede to fact layer */
   memory_db_fact_supersede(old_fact_id, fact_b);
   int64_t superseded_by = get_fact_superseded_by(fact_a);
   TEST_ASSERT(superseded_by == fact_b, "old fact's superseded_by set to new fact");
}

static void test_no_fact_id_on_old_relation(void) {
   printf("\n--- test_no_fact_id_on_old_relation ---\n");

   int user_id = 2;
   int64_t bob = insert_entity(user_id, "bob", "person");
   int64_t nyc = insert_entity(user_id, "nyc", "place");
   int64_t sf = insert_entity(user_id, "sf", "place");

   /* Old relation without fact_id (legacy data, fact_id=0) */
   memory_db_relation_supersede(user_id, bob, "lives_in", nyc, NULL, 0, 0.8f, 0, 0, NULL);

   /* Supersede */
   int64_t old_fact_id = -1;
   int rc = memory_db_relation_supersede(user_id, bob, "lives_in", sf, NULL, 0, 0.8f, 0, 0,
                                         &old_fact_id);

   TEST_ASSERT(rc == MEMORY_DB_SUCCESS, "supersede returns SUCCESS");
   TEST_ASSERT(old_fact_id == 0, "old_fact_id is 0 when legacy relation has no linked fact");
}

static void test_non_exclusive_skips(void) {
   printf("\n--- test_non_exclusive_skips ---\n");

   int user_id = 3;
   int64_t fact_a = insert_fact(user_id, "Carol likes cats");
   int64_t carol = insert_entity(user_id, "carol", "person");
   int64_t cats = insert_entity(user_id, "cats", "thing");
   int64_t dogs = insert_entity(user_id, "dogs", "thing");

   memory_db_relation_supersede(user_id, carol, "likes", cats, NULL, fact_a, 0.8f, 0, 0, NULL);

   int64_t fact_b = insert_fact(user_id, "Carol likes dogs");
   int64_t old_fact_id = -1;
   int rc = memory_db_relation_supersede(user_id, carol, "likes", dogs, NULL, fact_b, 0.8f, 0, 0,
                                         &old_fact_id);

   TEST_ASSERT(rc == MEMORY_DB_SUCCESS, "non-exclusive supersede returns SUCCESS");
   TEST_ASSERT(old_fact_id == 0, "non-exclusive relation does not return old_fact_id");
}

static void test_null_out_param(void) {
   printf("\n--- test_null_out_param ---\n");

   int user_id = 4;
   int64_t dave = insert_entity(user_id, "dave", "person");
   int64_t mit = insert_entity(user_id, "mit", "org");

   int rc = memory_db_relation_supersede(user_id, dave, "attends_school", mit, NULL, 0, 0.8f, 0, 0,
                                         NULL);

   TEST_ASSERT(rc == MEMORY_DB_SUCCESS, "NULL out_old_fact_id does not crash");
}

static void test_same_object_idempotent(void) {
   printf("\n--- test_same_object_idempotent ---\n");

   int user_id = 5;
   int64_t fact_a = insert_fact(user_id, "Eve works at Apple");
   int64_t eve = insert_entity(user_id, "eve", "person");
   int64_t apple = insert_entity(user_id, "apple", "org");

   memory_db_relation_supersede(user_id, eve, "works_at", apple, NULL, fact_a, 0.9f, 0, 0, NULL);

   /* Re-mention same relation — should NOT close the existing row */
   int64_t old_fact_id = -1;
   int rc = memory_db_relation_supersede(user_id, eve, "works_at", apple, NULL, fact_a, 0.9f, 0, 0,
                                         &old_fact_id);

   TEST_ASSERT(rc == MEMORY_DB_SUCCESS, "idempotent supersede returns SUCCESS");
   TEST_ASSERT(old_fact_id == 0, "same object does not trigger close (idempotent)");
}

static void test_contradictory_pair(void) {
   printf("\n--- test_contradictory_pair ---\n");

   int user_id = 6;
   int64_t fact_a = insert_fact(user_id, "Frank likes spiders");
   int64_t frank = insert_entity(user_id, "frank", "person");
   int64_t spiders = insert_entity(user_id, "spiders", "thing");

   /* Create initial "likes" relation */
   memory_db_relation_supersede(user_id, frank, "likes", spiders, NULL, fact_a, 0.8f, 0, 0, NULL);

   /* Now store "dislikes" for same (subject, object) */
   int64_t fact_b = insert_fact(user_id, "Frank dislikes spiders");
   int64_t old_fact_id = 0;
   int rc = memory_db_relation_supersede(user_id, frank, "dislikes", spiders, NULL, fact_b, 0.9f, 0,
                                         0, &old_fact_id);

   TEST_ASSERT(rc == MEMORY_DB_SUCCESS, "contradictory pair supersede returns SUCCESS");
   TEST_ASSERT(old_fact_id == fact_a,
               "contradictory pair returns old fact_id from opposing relation");
}

static void test_contradictory_pair_different_object(void) {
   printf("\n--- test_contradictory_pair_different_object ---\n");

   int user_id = 7;
   int64_t fact_a = insert_fact(user_id, "Grace enjoys cooking");
   int64_t grace = insert_entity(user_id, "grace", "person");

   /* Literal-value relation (no object entity) */
   memory_db_relation_supersede(user_id, grace, "enjoys", 0, "cooking", fact_a, 0.8f, 0, 0, NULL);

   /* Different object — should NOT close the "cooking" relation */
   int64_t fact_b = insert_fact(user_id, "Grace hates gardening");
   int64_t old_fact_id = -1;
   int rc = memory_db_relation_supersede(user_id, grace, "hates", 0, "gardening", fact_b, 0.9f, 0,
                                         0, &old_fact_id);

   TEST_ASSERT(rc == MEMORY_DB_SUCCESS, "different-object contradictory returns SUCCESS");
   TEST_ASSERT(old_fact_id == 0,
               "contradictory pair with different object does NOT close old relation");
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
   printf("=== test_relation_supersede ===\n");
   setup_db();

   test_exclusive_supersede_returns_old_fact_id();
   test_no_fact_id_on_old_relation();
   test_non_exclusive_skips();
   test_null_out_param();
   test_same_object_idempotent();
   test_contradictory_pair();
   test_contradictory_pair_different_object();

   teardown_db();

   printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
   return tests_failed > 0 ? 1 : 0;
}
