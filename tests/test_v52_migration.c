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
 * Unit tests for the v52 messaging_channels.conversation_id migration.
 * Pure SQL — does not link any DAWN modules.  Mirrors the migration
 * block in src/auth/auth_db_schema.c byte-for-byte so a drift between
 * the migration and the test surfaces as a failure.
 *
 * Coverage:
 *   - ALTER TABLE ADD COLUMN with NULL default + FK works on the v51
 *     messaging_channels shape.
 *   - Existing rows survive with conversation_id = NULL.
 *   - UPDATE to non-NULL works.
 *   - ON DELETE SET NULL fires when the referenced conversations row
 *     is deleted (FK enforcement requires PRAGMA foreign_keys=ON).
 */

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

static sqlite3 *g_db = NULL;

/* Pre-v52 schema — v51 messaging_channels shape (no conversation_id
 * column).  Plus a stub conversations + users table for FK targets. */
static const char *PRE_V52_SCHEMA =
    "CREATE TABLE users (id INTEGER PRIMARY KEY);"
    "CREATE TABLE conversations (id INTEGER PRIMARY KEY AUTOINCREMENT, user_id INTEGER);"
    "CREATE TABLE messaging_channels ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  user_id INTEGER NOT NULL,"
    "  provider TEXT NOT NULL "
    "    CHECK(provider IN ('telegram','discord','slack','sms')),"
    "  provider_address TEXT NOT NULL,"
    "  address_json TEXT NOT NULL,"
    "  display_name TEXT,"
    "  credentials_ref TEXT,"
    "  is_enabled INTEGER NOT NULL DEFAULT 1,"
    "  rate_limit_per_min INTEGER NOT NULL DEFAULT 10,"
    "  rate_limit_per_day INTEGER NOT NULL DEFAULT 200,"
    "  created_at INTEGER NOT NULL,"
    "  last_used_at INTEGER,"
    "  UNIQUE(user_id, provider, provider_address),"
    "  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE"
    ");";

/* v52 migration SQL — must mirror src/auth/auth_db_schema.c byte-for-byte.
 * Update both sites together. */
static const char *V52_MIGRATION_SQL =
    "ALTER TABLE messaging_channels ADD COLUMN conversation_id INTEGER "
    "REFERENCES conversations(id) ON DELETE SET NULL";

static void exec_or_fail(const char *sql) {
   char *errmsg = NULL;
   int rc = sqlite3_exec(g_db, sql, NULL, NULL, &errmsg);
   if (rc != SQLITE_OK) {
      fprintf(stderr, "SQL failed: %s\nSQL was: %s\n", errmsg ? errmsg : "(none)", sql);
      sqlite3_free(errmsg);
      TEST_FAIL_MESSAGE("SQL execution failed");
   }
}

static int64_t query_int64(const char *sql) {
   sqlite3_stmt *stmt = NULL;
   int64_t result = -1;
   if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) == SQLITE_OK) {
      if (sqlite3_step(stmt) == SQLITE_ROW) {
         result = sqlite3_column_int64(stmt, 0);
      }
      sqlite3_finalize(stmt);
   }
   return result;
}

/* Returns 1 if the row's conversation_id column IS NULL, 0 otherwise.
 * Distinct from query_int64 because NULL vs 0 matters for FK semantics. */
static int conv_id_is_null(int64_t channel_id) {
   sqlite3_stmt *stmt = NULL;
   int result = -1;
   const char *sql = "SELECT conversation_id IS NULL FROM messaging_channels WHERE id = ?";
   if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_int64(stmt, 1, channel_id);
      if (sqlite3_step(stmt) == SQLITE_ROW) {
         result = sqlite3_column_int(stmt, 0);
      }
      sqlite3_finalize(stmt);
   }
   return result;
}

void setUp(void) {
   int rc = sqlite3_open(":memory:", &g_db);
   if (rc != SQLITE_OK) {
      fprintf(stderr, "open :memory: failed: %s\n", sqlite3_errmsg(g_db));
      exit(1);
   }
   /* FK enforcement matches the production runtime (PRAGMA
    * foreign_keys=ON in auth_db_core.c). */
   exec_or_fail("PRAGMA foreign_keys = ON");
   exec_or_fail(PRE_V52_SCHEMA);

   /* Seed user + a channel row at v51 shape. */
   exec_or_fail("INSERT INTO users (id) VALUES (1);");
   exec_or_fail("INSERT INTO messaging_channels "
                "(user_id, provider, provider_address, address_json, "
                " display_name, created_at) "
                "VALUES (1, 'telegram', '8877272321', "
                "'{\"chat_id\":\"8877272321\"}', 'telegram_main', 1700000000);");
}

void tearDown(void) {
   if (g_db) {
      sqlite3_close(g_db);
      g_db = NULL;
   }
}

/* Test 1: migration adds the column without breaking existing rows. */
static void test_migration_preserves_existing_rows(void) {
   /* Pre-migration: only the original row exists. */
   TEST_ASSERT_EQUAL_INT64(1, query_int64("SELECT COUNT(*) FROM messaging_channels"));

   /* Run migration. */
   exec_or_fail(V52_MIGRATION_SQL);

   /* Row still there with conversation_id = NULL (default for ALTER ADD COLUMN). */
   TEST_ASSERT_EQUAL_INT64(1, query_int64("SELECT COUNT(*) FROM messaging_channels"));
   TEST_ASSERT_EQUAL_INT(1, conv_id_is_null(1));

   /* Other columns unchanged. */
   TEST_ASSERT_EQUAL_INT64(1, query_int64("SELECT user_id FROM messaging_channels WHERE id = 1"));
   TEST_ASSERT_EQUAL_INT64(1700000000,
                           query_int64("SELECT created_at FROM messaging_channels WHERE id = 1"));
}

/* Test 2: INSERT-with-NULL-conversation-id works post-migration. */
static void test_insert_with_null_conv_id(void) {
   exec_or_fail(V52_MIGRATION_SQL);

   exec_or_fail("INSERT INTO messaging_channels "
                "(user_id, provider, provider_address, address_json, "
                " display_name, created_at, conversation_id) "
                "VALUES (1, 'sms', '+15551234567', "
                "'{\"phone_e164\":\"+15551234567\"}', 'sms_jonathan', 1700000100, NULL);");

   TEST_ASSERT_EQUAL_INT64(2, query_int64("SELECT COUNT(*) FROM messaging_channels"));
   TEST_ASSERT_EQUAL_INT(1, conv_id_is_null(2));
}

/* Test 3: UPDATE to non-NULL conv_id works (the forever-binding write
 * path), and survives across re-reads. */
static void test_update_to_non_null_conv_id(void) {
   exec_or_fail(V52_MIGRATION_SQL);

   /* Create a conversation row first so the FK satisfies. */
   exec_or_fail("INSERT INTO conversations (id, user_id) VALUES (42, 1);");

   /* Stamp the conv_id onto the channel.  Mirrors
    * resolve_channel_conversation_id's Phase 3 UPDATE. */
   exec_or_fail("UPDATE messaging_channels SET conversation_id = 42 WHERE id = 1");

   TEST_ASSERT_EQUAL_INT(0, conv_id_is_null(1));
   TEST_ASSERT_EQUAL_INT64(42, query_int64(
                                   "SELECT conversation_id FROM messaging_channels WHERE id = 1"));
}

/* Test 4: FK ON DELETE SET NULL — deleting the conversation row clears
 * the channel's conversation_id (so the next inbound starts a fresh
 * conv). */
static void test_fk_on_delete_set_null(void) {
   exec_or_fail(V52_MIGRATION_SQL);
   exec_or_fail("INSERT INTO conversations (id, user_id) VALUES (42, 1);");
   exec_or_fail("UPDATE messaging_channels SET conversation_id = 42 WHERE id = 1");

   /* Sanity: linked. */
   TEST_ASSERT_EQUAL_INT(0, conv_id_is_null(1));

   /* Delete the conversation row.  FK cascade should clear channel's
    * pointer to it. */
   exec_or_fail("DELETE FROM conversations WHERE id = 42");

   /* Channel row survives (conversations is not its parent FK), but
    * conversation_id is now NULL.  Next inbound on this channel will
    * see NULL and create a fresh conv. */
   TEST_ASSERT_EQUAL_INT64(1, query_int64("SELECT COUNT(*) FROM messaging_channels"));
   TEST_ASSERT_EQUAL_INT(1, conv_id_is_null(1));
}

/* Test 5: simulating /new — manual UPDATE to NULL.  Mirrors
 * clear_channel_conversation_id's UPDATE shape. */
static void test_reset_via_update_to_null(void) {
   exec_or_fail(V52_MIGRATION_SQL);
   exec_or_fail("INSERT INTO conversations (id, user_id) VALUES (42, 1);");
   exec_or_fail("UPDATE messaging_channels SET conversation_id = 42 WHERE id = 1");
   TEST_ASSERT_EQUAL_INT(0, conv_id_is_null(1));

   /* /new clears the binding via UPDATE — same shape as
    * clear_channel_conversation_id in messaging_engine.c. */
   exec_or_fail("UPDATE messaging_channels SET conversation_id = NULL "
                "WHERE provider = 'telegram' AND provider_address = '8877272321' "
                "  AND is_enabled = 1");

   TEST_ASSERT_EQUAL_INT(1, conv_id_is_null(1));
   /* Conversation row 42 still exists (NOT cascaded — /new preserves
    * history in the WebUI, just unbinds the channel). */
   TEST_ASSERT_EQUAL_INT64(1, query_int64("SELECT COUNT(*) FROM conversations WHERE id = 42"));
}

/* Test 6: migration is idempotent — running it twice on the same DB
 * should fail cleanly (ALTER TABLE ADD COLUMN errors on duplicate
 * column name).  Production migration block guards with the
 * `current_version < 52` check; the test confirms SQLite's own error
 * shape so the production guard is the right one. */
static void test_migration_is_not_idempotent_at_sql_level(void) {
   exec_or_fail(V52_MIGRATION_SQL);
   /* Second ALTER should fail. */
   char *errmsg = NULL;
   int rc = sqlite3_exec(g_db, V52_MIGRATION_SQL, NULL, NULL, &errmsg);
   TEST_ASSERT_NOT_EQUAL(SQLITE_OK, rc);
   sqlite3_free(errmsg);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_migration_preserves_existing_rows);
   RUN_TEST(test_insert_with_null_conv_id);
   RUN_TEST(test_update_to_non_null_conv_id);
   RUN_TEST(test_fk_on_delete_set_null);
   RUN_TEST(test_reset_via_update_to_null);
   RUN_TEST(test_migration_is_not_idempotent_at_sql_level);
   return UNITY_END();
}
