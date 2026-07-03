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
 * Unit tests for the v69 conversations.is_pinned migration.
 *
 * Regression guard for the base-schema-vs-migration ordering trap: the base
 * SCHEMA_SQL runs before migrations on every boot, so an index that references
 * a migration-added column must live in the migration, NOT the base schema.
 * A first cut of v69 placed `idx_conversations_pinned` (which references
 * is_pinned) in the base schema; on a pre-existing DB the CREATE TABLE is a
 * no-op (is_pinned not yet added) and the base-schema index creation failed
 * with "no such column: is_pinned", aborting auth_db_init().  A fresh :memory:
 * test could never catch this — only an UPGRADE from a real prior version does,
 * which is exactly what this test seeds.
 */

#include <dirent.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "auth/auth_db.h"
#include "unity.h"

static char s_tmpdir[256];
static char s_dbpath[320];

/* Build a v68-shaped DB inside an isolated temp dir: schema_version = 68 and a
 * `conversations` table with the full v68 column set but WITHOUT is_pinned (i.e.
 * the current base schema minus the v69 column).  The full shape matters:
 * auth_db_init() runs the base SCHEMA_SQL (conversations CREATE is a no-op,
 * preserving this shape) and then prepares statements that reference the v68
 * columns (anchor_date, llm_*, etc.), so a too-minimal seed would fail statement
 * prep for an unrelated reason and mask the migration result.  Init then runs
 * the v69 migration (adds is_pinned + idx_conversations_pinned), backing up the
 * v68 DB first. */
void setUp(void) {
   snprintf(s_tmpdir, sizeof(s_tmpdir), "/tmp/dawn_v68_mig_XXXXXX");
   TEST_ASSERT_NOT_NULL(mkdtemp(s_tmpdir));
   snprintf(s_dbpath, sizeof(s_dbpath), "%s/store.db", s_tmpdir);

   sqlite3 *db = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_open(s_dbpath, &db));
   char *err = NULL;
   int rc = sqlite3_exec(db,
                         "CREATE TABLE schema_version (version INTEGER);"
                         "INSERT INTO schema_version (version) VALUES (68);"
                         "CREATE TABLE conversations ("
                         "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
                         "  user_id INTEGER NOT NULL,"
                         "  title TEXT NOT NULL DEFAULT 'New Conversation',"
                         "  created_at INTEGER NOT NULL,"
                         "  updated_at INTEGER NOT NULL,"
                         "  message_count INTEGER DEFAULT 0,"
                         "  is_archived INTEGER DEFAULT 0,"
                         "  context_tokens INTEGER DEFAULT 0,"
                         "  context_max INTEGER DEFAULT 0,"
                         "  continued_from INTEGER DEFAULT NULL,"
                         "  compaction_summary TEXT DEFAULT NULL,"
                         "  context_watermark_msg_id INTEGER NOT NULL DEFAULT 0,"
                         "  llm_type TEXT DEFAULT NULL,"
                         "  cloud_provider TEXT DEFAULT NULL,"
                         "  model TEXT DEFAULT NULL,"
                         "  tools_mode TEXT DEFAULT NULL,"
                         "  thinking_mode TEXT DEFAULT NULL,"
                         "  reasoning_effort TEXT DEFAULT NULL,"
                         "  last_extracted_msg_count INTEGER DEFAULT 0,"
                         "  last_extracted_msg_id INTEGER NOT NULL DEFAULT 0,"
                         "  extraction_attempts INTEGER DEFAULT 0,"
                         "  extraction_last_attempt_at INTEGER DEFAULT 0,"
                         "  is_private INTEGER DEFAULT 0,"
                         "  title_locked INTEGER DEFAULT 0,"
                         "  origin TEXT DEFAULT 'webui',"
                         "  anchor_date INTEGER NOT NULL DEFAULT 0);"
                         /* One existing row so the ALTER's DEFAULT-0 backfill is exercised. */
                         "INSERT INTO conversations (user_id, title, created_at, updated_at) "
                         "VALUES (1, 'Existing', 1000, 2000);",
                         NULL, NULL, &err);
   if (rc != SQLITE_OK) {
      TEST_FAIL_MESSAGE(err ? err : "seed failed");
   }
   sqlite3_close(db);
}

void tearDown(void) {
   auth_db_shutdown();
   unlink(s_dbpath);
}

/* Returns true if @sql's single-int result is > 0. */
static bool scalar_true(sqlite3 *db, const char *sql) {
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
      return false;
   }
   bool ok = (sqlite3_step(st) == SQLITE_ROW) && (sqlite3_column_int(st, 0) > 0);
   sqlite3_finalize(st);
   return ok;
}

static void test_v68_db_migrates_to_v69(void) {
   /* The bug: auth_db_init() returned FAILURE here ("no such column: is_pinned")
    * because base SCHEMA_SQL indexed a not-yet-migrated column on the
    * pre-existing conversations table. */
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(s_dbpath));
   auth_db_shutdown();

   sqlite3 *db = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_open(s_dbpath, &db));

   /* Schema bumped to at least v69.  init always migrates to the current max
    * (assert >= not == so a future schema bump doesn't re-break this).  The
    * v69-specific effect is verified by the is_pinned column/index checks below. */
   sqlite3_stmt *st = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK,
                         sqlite3_prepare_v2(db, "SELECT version FROM schema_version LIMIT 1", -1,
                                            &st, NULL));
   TEST_ASSERT_EQUAL_INT(SQLITE_ROW, sqlite3_step(st));
   TEST_ASSERT_TRUE_MESSAGE(sqlite3_column_int(st, 0) >= 69, "schema not migrated to >= v69");
   sqlite3_finalize(st);

   /* conversations.is_pinned added, and the index that caused the failure now
    * exists (created by the migration). */
   TEST_ASSERT_TRUE(scalar_true(
       db, "SELECT COUNT(*) FROM pragma_table_info('conversations') WHERE name='is_pinned'"));
   TEST_ASSERT_TRUE(scalar_true(db, "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND "
                                    "name='idx_conversations_pinned'"));

   /* Pre-existing row backfilled to is_pinned = 0 (the literal DEFAULT). */
   TEST_ASSERT_TRUE(
       scalar_true(db, "SELECT COUNT(*) FROM conversations WHERE is_pinned = 0 AND id = 1"));
   TEST_ASSERT_FALSE(scalar_true(db, "SELECT COUNT(*) FROM conversations WHERE is_pinned != 0"));

   sqlite3_close(db);
}

/* Re-running init against the now-v69 DB is a clean no-op (idempotent). */
static void test_v69_reinit_is_idempotent(void) {
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(s_dbpath));
   auth_db_shutdown();
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(s_dbpath));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_v68_db_migrates_to_v69);
   RUN_TEST(test_v69_reinit_is_idempotent);
   return UNITY_END();
}
