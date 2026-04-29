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
 * Unit tests for missed_notifications_db.c — offline-user notification queue.
 * Uses an in-memory SQLite database via the stubbed s_db global.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db_internal.h"
#include "core/missed_notifications_db.h"
#include "unity.h"


/* ddl mirrors the v32 migration in src/auth/auth_db_core.c — keep in sync. */
static const char *ddl = "CREATE TABLE IF NOT EXISTS missed_notifications ("
                         "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
                         "  user_id INTEGER NOT NULL,"
                         "  event_id INTEGER NOT NULL,"
                         "  event_type TEXT NOT NULL,"
                         "  status TEXT NOT NULL,"
                         "  name TEXT NOT NULL,"
                         "  message TEXT,"
                         "  fire_at INTEGER NOT NULL,"
                         "  conversation_id INTEGER DEFAULT 0,"
                         "  created_at INTEGER NOT NULL"
                         ");"
                         "CREATE INDEX IF NOT EXISTS idx_missed_notif_user "
                         "  ON missed_notifications(user_id, created_at);";

static void setup_db(void) {
   memset(&s_db, 0, sizeof(s_db));
   int rc = sqlite3_open(":memory:", &s_db.db);
   if (rc != SQLITE_OK) {
      fprintf(stderr, "Failed to open in-memory DB: %s\n", sqlite3_errmsg(s_db.db));
      exit(1);
   }
   pthread_mutex_init(&s_db.mutex, NULL);

   char *errmsg = NULL;
   rc = sqlite3_exec(s_db.db, ddl, NULL, NULL, &errmsg);
   if (rc != SQLITE_OK) {
      fprintf(stderr, "ddl failed: %s\n", errmsg);
      sqlite3_free(errmsg);
      exit(1);
   }

   s_db.initialized = true;
}

static void reset_table(void) {
   sqlite3_exec(s_db.db, "DELETE FROM missed_notifications", NULL, NULL, NULL);
}

static void teardown_db(void) {
   if (s_db.db) {
      sqlite3_close(s_db.db);
   }
   pthread_mutex_destroy(&s_db.mutex);
   memset(&s_db, 0, sizeof(s_db));
}

/* ============================================================================
 * Insert Tests
 * ============================================================================ */

static void test_insert_basic(void) {
   printf("\n--- test_insert_basic ---\n");
   reset_table();

   time_t now = time(NULL);
   int rc = missed_notif_insert(1, 100, "timer", "ringing", "Egg", "Timer done", now, 0);
   TEST_ASSERT_TRUE_MESSAGE(rc == AUTH_DB_SUCCESS, "basic insert succeeds");

   missed_notif_t rows[10];
   int count = 0;
   TEST_ASSERT_TRUE_MESSAGE(missed_notif_get_for_user(1, 10, rows, &count) == AUTH_DB_SUCCESS,
                            "get succeeds");
   TEST_ASSERT_TRUE_MESSAGE(count == 1, "one row for user 1");
   TEST_ASSERT_TRUE_MESSAGE(rows[0].event_id == 100, "event_id round-trips");
   TEST_ASSERT_TRUE_MESSAGE(strcmp(rows[0].event_type, "timer") == 0, "event_type round-trips");
   TEST_ASSERT_TRUE_MESSAGE(strcmp(rows[0].status, "ringing") == 0, "status round-trips");
   TEST_ASSERT_TRUE_MESSAGE(strcmp(rows[0].name, "Egg") == 0, "name round-trips");
   TEST_ASSERT_TRUE_MESSAGE(strcmp(rows[0].message, "Timer done") == 0, "message round-trips");
   TEST_ASSERT_TRUE_MESSAGE(rows[0].fire_at == now, "fire_at round-trips");
   TEST_ASSERT_TRUE_MESSAGE(rows[0].conversation_id == 0, "conversation_id defaults to 0");
}

static void test_insert_briefing_with_conversation(void) {
   printf("\n--- test_insert_briefing_with_conversation ---\n");
   reset_table();

   time_t now = time(NULL);
   int rc = missed_notif_insert(1, 200, "briefing", "ringing", "Morning", "Briefing ready", now,
                                42);
   TEST_ASSERT_TRUE_MESSAGE(rc == AUTH_DB_SUCCESS, "briefing insert succeeds");

   missed_notif_t rows[10];
   int count = 0;
   int get_rc = missed_notif_get_for_user(1, 10, rows, &count);
   TEST_ASSERT_TRUE_MESSAGE(get_rc == AUTH_DB_SUCCESS, "get_for_user succeeds");
   TEST_ASSERT_TRUE_MESSAGE(count == 1, "briefing row stored");
   TEST_ASSERT_TRUE_MESSAGE(rows[0].conversation_id == 42, "conversation_id round-trips");
}

static void test_insert_rejects_bad_args(void) {
   printf("\n--- test_insert_rejects_bad_args ---\n");
   reset_table();

   time_t now = time(NULL);
   TEST_ASSERT_TRUE_MESSAGE(missed_notif_insert(0, 1, "timer", "ringing", "n", "m", now, 0) ==
                                AUTH_DB_FAILURE,
                            "user_id 0 rejected");
   TEST_ASSERT_TRUE_MESSAGE(missed_notif_insert(-1, 1, "timer", "ringing", "n", "m", now, 0) ==
                                AUTH_DB_FAILURE,
                            "negative user_id rejected");
   TEST_ASSERT_TRUE_MESSAGE(missed_notif_insert(1, 1, NULL, "ringing", "n", "m", now, 0) ==
                                AUTH_DB_FAILURE,
                            "NULL event_type rejected");
   TEST_ASSERT_TRUE_MESSAGE(missed_notif_insert(1, 1, "timer", NULL, "n", "m", now, 0) ==
                                AUTH_DB_FAILURE,
                            "NULL status rejected");
   TEST_ASSERT_TRUE_MESSAGE(missed_notif_insert(1, 1, "timer", "ringing", NULL, "m", now, 0) ==
                                AUTH_DB_FAILURE,
                            "NULL name rejected");

   /* NULL message is allowed (stored as empty string). */
   int rc = missed_notif_insert(1, 1, "timer", "ringing", "n", NULL, now, 0);
   TEST_ASSERT_TRUE_MESSAGE(rc == AUTH_DB_SUCCESS, "NULL message accepted (treated as empty)");
}

static void test_insert_cap_enforced(void) {
   printf("\n--- test_insert_cap_enforced ---\n");
   reset_table();

   time_t now = time(NULL);
   int ok = 0;
   /* Fill to the cap. */
   for (int i = 0; i < MISSED_NOTIF_MAX_PER_USER; i++) {
      if (missed_notif_insert(1, i + 1, "timer", "ringing", "n", "m", now + i, 0) ==
          AUTH_DB_SUCCESS) {
         ok++;
      }
   }
   TEST_ASSERT_TRUE_MESSAGE(ok == MISSED_NOTIF_MAX_PER_USER,
                            "first N inserts succeed (where N == cap)");

   /* N+1 must be rejected. */
   int rc = missed_notif_insert(1, 999, "timer", "ringing", "n", "m", now, 0);
   TEST_ASSERT_TRUE_MESSAGE(rc == AUTH_DB_FAILURE, "insert past cap rejected");

   /* Row count unchanged. */
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(s_db.db, "SELECT COUNT(*) FROM missed_notifications WHERE user_id = 1", -1,
                      &stmt, NULL);
   sqlite3_step(stmt);
   int stored = sqlite3_column_int(stmt, 0);
   sqlite3_finalize(stmt);
   TEST_ASSERT_TRUE_MESSAGE(stored == MISSED_NOTIF_MAX_PER_USER,
                            "row count stays at cap after rejection");

   /* Another user is unaffected by user 1's cap. */
   int rc2 = missed_notif_insert(2, 1, "timer", "ringing", "n", "m", now, 0);
   TEST_ASSERT_TRUE_MESSAGE(rc2 == AUTH_DB_SUCCESS, "cap is per-user, other user can still insert");
}

/* ============================================================================
 * Get Tests
 * ============================================================================ */

static void test_get_ordering_oldest_first(void) {
   printf("\n--- test_get_ordering_oldest_first ---\n");
   reset_table();

   /* Insert three with sleeps so created_at differs. */
   missed_notif_insert(1, 10, "timer", "ringing", "first", "", 100, 0);
   /* created_at is set to time(NULL) at insert — sleep briefly to get distinct values. */
   struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
   nanosleep(&ts, NULL);
   missed_notif_insert(1, 20, "timer", "ringing", "second", "", 200, 0);
   nanosleep(&ts, NULL);
   missed_notif_insert(1, 30, "timer", "ringing", "third", "", 300, 0);

   missed_notif_t rows[10];
   int count = 0;
   TEST_ASSERT_TRUE_MESSAGE(missed_notif_get_for_user(1, 10, rows, &count) == AUTH_DB_SUCCESS,
                            "get succeeds");
   TEST_ASSERT_TRUE_MESSAGE(count == 3, "three rows returned");
   TEST_ASSERT_TRUE_MESSAGE(rows[0].event_id == 10, "oldest first");
   TEST_ASSERT_TRUE_MESSAGE(rows[1].event_id == 20, "middle next");
   TEST_ASSERT_TRUE_MESSAGE(rows[2].event_id == 30, "newest last");
}

static void test_get_respects_limit(void) {
   printf("\n--- test_get_respects_limit ---\n");
   reset_table();

   time_t now = time(NULL);
   for (int i = 0; i < 10; i++) {
      missed_notif_insert(1, i + 1, "timer", "ringing", "n", "m", now + i, 0);
   }

   missed_notif_t rows[5];
   int count = 0;
   TEST_ASSERT_TRUE_MESSAGE(missed_notif_get_for_user(1, 5, rows, &count) == AUTH_DB_SUCCESS,
                            "get succeeds");
   TEST_ASSERT_TRUE_MESSAGE(count == 5, "LIMIT caps returned rows");
}

static void test_get_empty_user(void) {
   printf("\n--- test_get_empty_user ---\n");
   reset_table();

   missed_notif_t rows[10];
   int count = 0;
   TEST_ASSERT_TRUE_MESSAGE(missed_notif_get_for_user(99, 10, rows, &count) == AUTH_DB_SUCCESS,
                            "get succeeds");
   TEST_ASSERT_TRUE_MESSAGE(count == 0, "user with no rows returns 0");
}

static void test_get_rejects_bad_args(void) {
   printf("\n--- test_get_rejects_bad_args ---\n");

   missed_notif_t rows[10];
   int count = 0;
   TEST_ASSERT_TRUE_MESSAGE(missed_notif_get_for_user(1, 10, NULL, &count) == AUTH_DB_FAILURE,
                            "NULL out returns FAILURE");
   TEST_ASSERT_TRUE_MESSAGE(count == 0, "count is 0 for NULL out");
   TEST_ASSERT_TRUE_MESSAGE(missed_notif_get_for_user(1, 0, rows, &count) == AUTH_DB_FAILURE,
                            "max_count 0 returns FAILURE");
   TEST_ASSERT_TRUE_MESSAGE(missed_notif_get_for_user(0, 10, rows, &count) == AUTH_DB_FAILURE,
                            "user_id 0 returns FAILURE");
}

/* ============================================================================
 * Delete Tests
 * ============================================================================ */

static void test_delete_by_user_matches_owner(void) {
   printf("\n--- test_delete_by_user_matches_owner ---\n");
   reset_table();

   missed_notif_insert(1, 10, "timer", "ringing", "n", "m", time(NULL), 0);

   missed_notif_t rows[10];
   int count = 0;
   TEST_ASSERT_TRUE_MESSAGE(missed_notif_get_for_user(1, 10, rows, &count) == AUTH_DB_SUCCESS,
                            "get succeeds");
   int64_t id = rows[0].id;

   int rc = missed_notif_delete_by_user(id, 1);
   TEST_ASSERT_TRUE_MESSAGE(rc == AUTH_DB_SUCCESS, "owner delete returns SUCCESS");

   TEST_ASSERT_TRUE_MESSAGE(missed_notif_get_for_user(1, 10, rows, &count) == AUTH_DB_SUCCESS,
                            "get succeeds");
   TEST_ASSERT_TRUE_MESSAGE(count == 0, "row is actually gone");
}

static void test_delete_by_user_rejects_non_owner(void) {
   printf("\n--- test_delete_by_user_rejects_non_owner ---\n");
   reset_table();

   missed_notif_insert(1, 10, "timer", "ringing", "user1", "m", time(NULL), 0);

   missed_notif_t rows[10];
   int count = 0;
   TEST_ASSERT_TRUE_MESSAGE(missed_notif_get_for_user(1, 10, rows, &count) == AUTH_DB_SUCCESS,
                            "get succeeds");
   int64_t id = rows[0].id;

   /* User 2 attempts to delete user 1's row. DB enforces AND user_id = ?. */
   int rc = missed_notif_delete_by_user(id, 2);
   TEST_ASSERT_TRUE_MESSAGE(rc == AUTH_DB_SUCCESS, "non-owner delete returns SUCCESS (no oracle)");

   TEST_ASSERT_TRUE_MESSAGE(missed_notif_get_for_user(1, 10, rows, &count) == AUTH_DB_SUCCESS,
                            "get succeeds");
   TEST_ASSERT_TRUE_MESSAGE(count == 1, "user 1's row still present (non-owner could not delete)");
}

static void test_delete_by_user_rejects_bad_args(void) {
   printf("\n--- test_delete_by_user_rejects_bad_args ---\n");

   TEST_ASSERT_TRUE_MESSAGE(missed_notif_delete_by_user(0, 1) == AUTH_DB_FAILURE, "id 0 rejected");
   TEST_ASSERT_TRUE_MESSAGE(missed_notif_delete_by_user(-1, 1) == AUTH_DB_FAILURE,
                            "negative id rejected");
   TEST_ASSERT_TRUE_MESSAGE(missed_notif_delete_by_user(1, 0) == AUTH_DB_FAILURE,
                            "user_id 0 rejected");
}

static void test_delete_all_for_user(void) {
   printf("\n--- test_delete_all_for_user ---\n");
   reset_table();

   time_t now = time(NULL);
   missed_notif_insert(1, 10, "timer", "ringing", "n", "m", now, 0);
   missed_notif_insert(1, 11, "timer", "ringing", "n", "m", now, 0);
   missed_notif_insert(1, 12, "timer", "ringing", "n", "m", now, 0);
   missed_notif_insert(2, 20, "timer", "ringing", "n", "m", now, 0);

   int deleted = 0;
   TEST_ASSERT_TRUE_MESSAGE(missed_notif_delete_all_for_user(1, &deleted) == AUTH_DB_SUCCESS,
                            "delete_all returns SUCCESS");
   TEST_ASSERT_TRUE_MESSAGE(deleted == 3, "delete_all reports 3 rows deleted");

   missed_notif_t rows[10];
   int count = 0;
   TEST_ASSERT_TRUE_MESSAGE(missed_notif_get_for_user(1, 10, rows, &count) == AUTH_DB_SUCCESS,
                            "get succeeds");
   TEST_ASSERT_TRUE_MESSAGE(count == 0, "user 1 is empty");
   TEST_ASSERT_TRUE_MESSAGE(missed_notif_get_for_user(2, 10, rows, &count) == AUTH_DB_SUCCESS,
                            "get succeeds");
   TEST_ASSERT_TRUE_MESSAGE(count == 1, "user 2 untouched");
}

/* ============================================================================
 * Expire Tests
 * ============================================================================ */

static void test_expire_removes_only_stale(void) {
   printf("\n--- test_expire_removes_only_stale ---\n");
   reset_table();

   time_t now = time(NULL);

   /* Fresh row via API. */
   missed_notif_insert(1, 1, "timer", "ringing", "fresh", "", now, 0);

   /* Directly insert a stale row (1 day + 100s old) bypassing the API so
    * created_at reflects the past rather than time(NULL). */
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(s_db.db,
                      "INSERT INTO missed_notifications "
                      "(user_id, event_id, event_type, status, name, message, fire_at, "
                      " conversation_id, created_at) "
                      "VALUES (1, 2, 'timer', 'ringing', 'stale', '', ?, 0, ?)",
                      -1, &stmt, NULL);
   sqlite3_bind_int64(stmt, 1, (int64_t)(now - 86500));
   sqlite3_bind_int64(stmt, 2, (int64_t)(now - 86500));
   sqlite3_step(stmt);
   sqlite3_finalize(stmt);

   int deleted = 0;
   TEST_ASSERT_TRUE_MESSAGE(missed_notif_expire(MISSED_NOTIF_EXPIRE_SEC, &deleted) ==
                                AUTH_DB_SUCCESS,
                            "expire returns SUCCESS");
   TEST_ASSERT_TRUE_MESSAGE(deleted == 1, "expire removes exactly one stale row");

   missed_notif_t rows[10];
   int count = 0;
   TEST_ASSERT_TRUE_MESSAGE(missed_notif_get_for_user(1, 10, rows, &count) == AUTH_DB_SUCCESS,
                            "get succeeds");
   TEST_ASSERT_TRUE_MESSAGE(count == 1, "fresh row remains");
   TEST_ASSERT_TRUE_MESSAGE(rows[0].event_id == 1, "fresh row is the survivor");
}

static void test_expire_no_stale_is_noop(void) {
   printf("\n--- test_expire_no_stale_is_noop ---\n");
   reset_table();

   missed_notif_insert(1, 1, "timer", "ringing", "n", "m", time(NULL), 0);

   int deleted = 0;
   missed_notif_expire(MISSED_NOTIF_EXPIRE_SEC, &deleted);
   TEST_ASSERT_TRUE_MESSAGE(deleted == 0, "nothing expired when all rows are fresh");
}

static void test_expire_rejects_bad_arg(void) {
   printf("\n--- test_expire_rejects_bad_arg ---\n");

   TEST_ASSERT_TRUE_MESSAGE(missed_notif_expire(0, NULL) == AUTH_DB_FAILURE, "max_age 0 rejected");
   TEST_ASSERT_TRUE_MESSAGE(missed_notif_expire(-1, NULL) == AUTH_DB_FAILURE,
                            "negative max_age rejected");
}

/* ============================================================================
 * User Isolation
 * ============================================================================ */

static void test_user_isolation(void) {
   printf("\n--- test_user_isolation ---\n");
   reset_table();

   time_t now = time(NULL);
   missed_notif_insert(1, 10, "timer", "ringing", "u1-a", "", now, 0);
   missed_notif_insert(1, 11, "timer", "ringing", "u1-b", "", now, 0);
   missed_notif_insert(2, 20, "timer", "ringing", "u2-a", "", now, 0);

   missed_notif_t rows[10];
   int count = 0;
   TEST_ASSERT_TRUE_MESSAGE(missed_notif_get_for_user(1, 10, rows, &count) == AUTH_DB_SUCCESS,
                            "get succeeds");
   TEST_ASSERT_TRUE_MESSAGE(count == 2, "user 1 sees 2 rows");
   TEST_ASSERT_TRUE_MESSAGE(missed_notif_get_for_user(2, 10, rows, &count) == AUTH_DB_SUCCESS,
                            "get succeeds");
   TEST_ASSERT_TRUE_MESSAGE(count == 1, "user 2 sees 1 row");
   TEST_ASSERT_TRUE_MESSAGE(missed_notif_get_for_user(3, 10, rows, &count) == AUTH_DB_SUCCESS,
                            "get succeeds");
   TEST_ASSERT_TRUE_MESSAGE(count == 0, "unrelated user sees 0");
}

/* ============================================================================
 * Main
 * ============================================================================ */

void setUp(void) {
   setup_db();
}

void tearDown(void) {
   teardown_db();
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_insert_basic);
   RUN_TEST(test_insert_briefing_with_conversation);
   RUN_TEST(test_insert_rejects_bad_args);
   RUN_TEST(test_insert_cap_enforced);
   RUN_TEST(test_get_ordering_oldest_first);
   RUN_TEST(test_get_respects_limit);
   RUN_TEST(test_get_empty_user);
   RUN_TEST(test_get_rejects_bad_args);
   RUN_TEST(test_delete_by_user_matches_owner);
   RUN_TEST(test_delete_by_user_rejects_non_owner);
   RUN_TEST(test_delete_by_user_rejects_bad_args);
   RUN_TEST(test_delete_all_for_user);
   RUN_TEST(test_expire_removes_only_stale);
   RUN_TEST(test_expire_no_stale_is_noop);
   RUN_TEST(test_expire_rejects_bad_arg);
   RUN_TEST(test_user_isolation);
   return UNITY_END();
}
