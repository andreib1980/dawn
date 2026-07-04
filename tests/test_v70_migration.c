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
 * Unit tests for the v70 code_projects.name case-insensitive uniqueness
 * migration.
 *
 * Verifies the UPGRADE path (a real v69 DB with a code_projects row): after
 * auth_db_init() the NOCASE unique index exists and rejects a case-variant name
 * ("DAWN" when "dawn" is present). The fresh-install path + ladder wiring are
 * already exercised by test_auth_db / test_code_project_db (both run the full
 * migration ladder to the current version on a fresh DB).
 */

#include <sqlite3.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h> /* mkdtemp */
#include <unistd.h>

#include "auth/auth_db.h"
#include "unity.h"

static char s_tmpdir[256];
static char s_dbpath[320];

/* Build a v69-shaped DB with a code_projects table (v65 shape — the only column
 * v70 touches is `name`) holding one row. code_projects is created by the v65
 * migration, not the base schema, so a real v69 DB already has it; init must
 * migrate it to v70 (adds the NOCASE unique index) without re-running v65. */
void setUp(void) {
   snprintf(s_tmpdir, sizeof(s_tmpdir), "/tmp/dawn_v69_mig_XXXXXX");
   TEST_ASSERT_NOT_NULL(mkdtemp(s_tmpdir));
   snprintf(s_dbpath, sizeof(s_dbpath), "%s/store.db", s_tmpdir);

   sqlite3 *db = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_open(s_dbpath, &db));
   char *err = NULL;
   int rc = sqlite3_exec(
       db,
       "CREATE TABLE schema_version (version INTEGER);"
       "INSERT INTO schema_version (version) VALUES (69);"
       "CREATE TABLE code_projects ("
       "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
       "  name TEXT NOT NULL UNIQUE,"
       "  source_url TEXT NOT NULL,"
       "  local_path TEXT NOT NULL,"
       "  user_id INTEGER,"
       "  is_global INTEGER NOT NULL DEFAULT 0,"
       "  status TEXT NOT NULL DEFAULT 'pending',"
       "  status_msg TEXT,"
       "  created_at INTEGER NOT NULL,"
       "  updated_at INTEGER NOT NULL,"
       "  indexed_at INTEGER,"
       "  imported_by INTEGER);"
       "INSERT INTO code_projects (name, source_url, local_path, created_at, updated_at) "
       "VALUES ('dawn', 'https://example.com/dawn.git', '/tmp/dawn', 1000, 2000);",
       NULL, NULL, &err);
   if (rc != SQLITE_OK) {
      TEST_FAIL_MESSAGE(err ? err : "seed failed");
   }
   sqlite3_close(db);
}

void tearDown(void) {
   auth_db_shutdown();
   unlink(s_dbpath);
   char sib[336];
   snprintf(sib, sizeof(sib), "%s-wal", s_dbpath);
   unlink(sib);
   snprintf(sib, sizeof(sib), "%s-shm", s_dbpath);
   unlink(sib);
   rmdir(s_tmpdir);
}

static bool scalar_true(sqlite3 *db, const char *sql) {
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
      return false;
   }
   bool ok = (sqlite3_step(st) == SQLITE_ROW) && (sqlite3_column_int(st, 0) > 0);
   sqlite3_finalize(st);
   return ok;
}

static void test_v69_db_migrates_to_v70_nocase_index(void) {
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(s_dbpath));
   auth_db_shutdown();

   sqlite3 *db = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_open(s_dbpath, &db));

   /* Schema bumped to at least v70 (>= so a future bump doesn't re-break this). */
   sqlite3_stmt *st = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK,
                         sqlite3_prepare_v2(db, "SELECT version FROM schema_version LIMIT 1", -1,
                                            &st, NULL));
   TEST_ASSERT_EQUAL_INT(SQLITE_ROW, sqlite3_step(st));
   TEST_ASSERT_TRUE_MESSAGE(sqlite3_column_int(st, 0) >= 70, "schema not migrated to >= v70");
   sqlite3_finalize(st);

   /* The NOCASE unique index the migration adds. */
   TEST_ASSERT_TRUE(scalar_true(db, "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND "
                                    "name='idx_code_projects_name_nocase'"));

   /* Enforcement: inserting a case-variant of an existing name is rejected. */
   char *err = NULL;
   int rc = sqlite3_exec(db,
                         "INSERT INTO code_projects (name, source_url, local_path, created_at, "
                         "updated_at) VALUES ('DAWN', 'x', '/y', 1, 2)",
                         NULL, NULL, &err);
   TEST_ASSERT_EQUAL_INT_MESSAGE(SQLITE_CONSTRAINT, rc,
                                 "case-variant name should violate the NOCASE unique index");
   sqlite3_free(err);

   sqlite3_close(db);
}

/* Re-running init against the now-v70 DB is a clean no-op (idempotent). */
static void test_v70_reinit_is_idempotent(void) {
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(s_dbpath));
   auth_db_shutdown();
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(s_dbpath));
}

/* A legacy DB already holding two case-variant names: the CREATE UNIQUE INDEX
 * hits SQLITE_CONSTRAINT, which the migration treats as non-fatal — init must
 * still SUCCEED and the schema must advance (retrying can't help), but the index
 * is (correctly) absent until the operator removes the duplicate. */
static void test_v70_collision_is_nonfatal(void) {
   /* setUp seeded 'dawn'; add a case-variant, legal under the BINARY UNIQUE(name). */
   sqlite3 *db = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_open(s_dbpath, &db));
   char *err = NULL;
   TEST_ASSERT_EQUAL_INT_MESSAGE(SQLITE_OK,
                                 sqlite3_exec(db,
                                              "INSERT INTO code_projects (name, source_url, "
                                              "local_path, created_at, updated_at) "
                                              "VALUES ('DAWN', 'x', '/y', 1, 2)",
                                              NULL, NULL, &err),
                                 err ? err : "seed collision failed");
   sqlite3_close(db);

   /* init must SUCCEED despite the collision (warn + advance, no index). */
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(s_dbpath));
   auth_db_shutdown();

   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_open(s_dbpath, &db));
   TEST_ASSERT_TRUE(scalar_true(db, "SELECT COUNT(*) FROM schema_version WHERE version >= 70"));
   TEST_ASSERT_FALSE(scalar_true(db, "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND "
                                     "name='idx_code_projects_name_nocase'"));
   sqlite3_close(db);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_v69_db_migrates_to_v70_nocase_index);
   RUN_TEST(test_v70_reinit_is_idempotent);
   RUN_TEST(test_v70_collision_is_nonfatal);
   return UNITY_END();
}
