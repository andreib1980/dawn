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
 * Unit tests for phone_db.c — call log and SMS log CRUD.
 * Uses an in-memory SQLite database via the stubbed s_db global.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db_internal.h"
#include "tools/phone_db.h"
#include "unity.h"

/* ============================================================================
 * Setup / Teardown
 * ============================================================================ */

static const char *DDL = "CREATE TABLE IF NOT EXISTS phone_call_log ("
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

   s_db.initialized = true;
}

static void teardown_db(void) {
   if (s_db.db) {
      sqlite3_close(s_db.db);
   }
   pthread_mutex_destroy(&s_db.mutex);
   memset(&s_db, 0, sizeof(s_db));
}

void setUp(void) {
   setup_db();
}

void tearDown(void) {
   teardown_db();
}

/* ============================================================================
 * Call Log Tests
 * ============================================================================ */

static void test_call_log_insert(void) {
   time_t now = time(NULL);
   int64_t id = 0;
   int rc = phone_db_call_log_insert(1, PHONE_DIR_OUTGOING, "+15551234567", "Mom", 0, now,
                                     PHONE_CALL_ANSWERED, &id);
   TEST_ASSERT_EQUAL_INT(PHONE_DB_SUCCESS, rc);
   TEST_ASSERT_TRUE(id >= 1);

   int64_t id2 = 0;
   rc = phone_db_call_log_insert(1, PHONE_DIR_INCOMING, "+15559876543", "Dad", 0, now + 1,
                                 PHONE_CALL_MISSED, &id2);
   TEST_ASSERT_EQUAL_INT(PHONE_DB_SUCCESS, rc);
   TEST_ASSERT_TRUE(id2 > id);
}

static void test_call_log_update(void) {
   time_t now = time(NULL);
   int64_t id = 0;
   phone_db_call_log_insert(1, PHONE_DIR_OUTGOING, "+15551234567", "Mom", 0, now,
                            PHONE_CALL_ANSWERED, &id);

   int rc = phone_db_call_log_update(id, 120, PHONE_CALL_ANSWERED);
   TEST_ASSERT_EQUAL_INT(0, rc);

   phone_call_log_t entries[10];
   int count = 0;
   phone_db_call_log_recent(1, entries, 10, &count);
   TEST_ASSERT_TRUE(count >= 1);

   int found = 0;
   for (int i = 0; i < count; i++) {
      if (entries[i].id == id) {
         TEST_ASSERT_EQUAL_INT(120, entries[i].duration_sec);
         found = 1;
         break;
      }
   }
   TEST_ASSERT_TRUE_MESSAGE(found, "updated entry found in recent results");
}

static void test_call_log_recent(void) {
   time_t now = time(NULL);
   phone_db_call_log_insert(1, PHONE_DIR_OUTGOING, "+15551234567", "Mom", 0, now,
                            PHONE_CALL_ANSWERED, NULL);
   phone_db_call_log_insert(1, PHONE_DIR_INCOMING, "+15559876543", "Dad", 0, now + 1,
                            PHONE_CALL_MISSED, NULL);

   phone_call_log_t entries[10];
   int count = 0;
   phone_db_call_log_recent(1, entries, 10, &count);
   TEST_ASSERT_TRUE(count >= 2);
   TEST_ASSERT_TRUE(entries[0].timestamp >= entries[1].timestamp);
}

static void test_call_log_user_isolation(void) {
   time_t now = time(NULL);
   phone_db_call_log_insert(1, PHONE_DIR_OUTGOING, "+15551234567", "Mom", 0, now,
                            PHONE_CALL_ANSWERED, NULL);
   phone_db_call_log_insert(2, PHONE_DIR_OUTGOING, "+15551111111", "Other User", 30, now,
                            PHONE_CALL_ANSWERED, NULL);

   phone_call_log_t entries[10];
   int count = 0;
   phone_db_call_log_recent(2, entries, 10, &count);
   TEST_ASSERT_EQUAL_INT(1, count);

   int count1 = 0;
   phone_db_call_log_recent(1, entries, 10, &count1);
   TEST_ASSERT_EQUAL_INT(1, count1);
}

/* ============================================================================
 * SMS Log Tests
 * ============================================================================ */

static void test_sms_log_insert(void) {
   time_t now = time(NULL);
   int64_t id = 0;
   int rc = phone_db_sms_log_insert(1, PHONE_DIR_INCOMING, "+15551234567", "Mom", "Hello from Mom!",
                                    now, &id);
   TEST_ASSERT_EQUAL_INT(PHONE_DB_SUCCESS, rc);
   TEST_ASSERT_TRUE(id >= 1);
}

static void test_sms_get_unread(void) {
   time_t now = time(NULL);
   phone_db_sms_log_insert(1, PHONE_DIR_INCOMING, "+15551234567", "Mom", "Hello from Mom!", now,
                           NULL);

   phone_sms_log_t entries[10];
   int count = 0;
   phone_db_sms_get_unread(1, entries, 10, &count);
   TEST_ASSERT_TRUE(count >= 1);
   TEST_ASSERT_EQUAL_INT(0, entries[0].read);
   TEST_ASSERT_EQUAL_STRING("Hello from Mom!", entries[0].body);
}

static void test_sms_mark_read(void) {
   time_t now = time(NULL);
   phone_db_sms_log_insert(1, PHONE_DIR_INCOMING, "+15551234567", "Mom", "Hello!", now, NULL);

   phone_sms_log_t entries[10];
   int count = 0;
   phone_db_sms_get_unread(1, entries, 10, &count);
   TEST_ASSERT_TRUE(count >= 1);

   int rc = phone_db_sms_mark_read(entries[0].id);
   TEST_ASSERT_EQUAL_INT(0, rc);

   int count2 = 0;
   phone_db_sms_get_unread(1, entries, 10, &count2);
   TEST_ASSERT_TRUE(count2 < count);
}

static void test_sms_unread_excludes_outbound(void) {
   time_t now = time(NULL);
   int user = 50;
   int64_t in_id = 0;
   phone_db_sms_log_insert(user, PHONE_DIR_INCOMING, "+15551112222", "In", "incoming msg", now,
                           &in_id);
   int64_t out_id = 0;
   phone_db_sms_log_insert(user, PHONE_DIR_OUTGOING, "+15553334444", "Out", "outgoing msg", now + 1,
                           &out_id);

   phone_sms_log_t entries[10];
   int count = 0;
   phone_db_sms_get_unread(user, entries, 10, &count);
   TEST_ASSERT_EQUAL_INT(1, count);
   TEST_ASSERT_EQUAL_INT(PHONE_DIR_INCOMING, entries[0].direction);
}

static void test_sms_log_recent(void) {
   time_t now = time(NULL);
   phone_db_sms_log_insert(1, PHONE_DIR_INCOMING, "+15551234567", "Mom", "Hi", now, NULL);
   phone_db_sms_log_insert(1, PHONE_DIR_OUTGOING, "+15559876543", "Dad", "Hey Dad", now + 1, NULL);

   phone_sms_log_t entries[10];
   int count = 0;
   phone_db_sms_log_recent(1, entries, 10, &count);
   TEST_ASSERT_TRUE(count >= 2);
   TEST_ASSERT_TRUE(entries[0].timestamp >= entries[1].timestamp);
}

static void test_sms_user_isolation(void) {
   phone_db_sms_log_insert(3, PHONE_DIR_INCOMING, "+15552222222", "Jane", "Hi there", time(NULL),
                           NULL);

   phone_sms_log_t entries[10];
   int count = 0;
   phone_db_sms_log_recent(3, entries, 10, &count);
   TEST_ASSERT_EQUAL_INT(1, count);
}

/* ============================================================================
 * Cleanup Tests
 * ============================================================================ */

static void test_cleanup(void) {
   time_t now = time(NULL);
   time_t old_time = now - (200 * 86400);

   /* Insert recent entries */
   phone_db_call_log_insert(1, PHONE_DIR_OUTGOING, "+15551234567", "Recent", 10, now,
                            PHONE_CALL_ANSWERED, NULL);
   phone_db_sms_log_insert(1, PHONE_DIR_INCOMING, "+15551234567", "Recent", "Recent msg", now,
                           NULL);

   /* Insert old entries */
   phone_db_call_log_insert(1, PHONE_DIR_OUTGOING, "+15550000000", "Old", 10, old_time,
                            PHONE_CALL_ANSWERED, NULL);
   phone_db_sms_log_insert(1, PHONE_DIR_INCOMING, "+15550000000", "Old", "Old message", old_time,
                           NULL);

   phone_call_log_t calls[20];
   int call_count_before = 0;
   phone_db_call_log_recent(1, calls, 20, &call_count_before);
   phone_sms_log_t sms[20];
   int sms_count_before = 0;
   phone_db_sms_log_recent(1, sms, 20, &sms_count_before);

   int rc = phone_db_cleanup(90, 90);
   TEST_ASSERT_EQUAL_INT(0, rc);

   int call_count_after = 0;
   phone_db_call_log_recent(1, calls, 20, &call_count_after);
   int sms_count_after = 0;
   phone_db_sms_log_recent(1, sms, 20, &sms_count_after);

   TEST_ASSERT_TRUE(call_count_after < call_count_before);
   TEST_ASSERT_TRUE(sms_count_after < sms_count_before);
}

/* ============================================================================
 * Edge Cases
 * ============================================================================ */

static void test_null_params(void) {
   int64_t id = 0;
   int rc = phone_db_call_log_insert(1, PHONE_DIR_OUTGOING, NULL, NULL, 0, time(NULL),
                                     PHONE_CALL_ANSWERED, &id);
   TEST_ASSERT_EQUAL_INT(PHONE_DB_SUCCESS, rc);
   TEST_ASSERT_TRUE(id >= 1);

   int count = 0;
   rc = phone_db_call_log_recent(1, NULL, 10, &count);
   TEST_ASSERT_EQUAL_INT(PHONE_DB_FAILURE, rc);

   rc = phone_db_sms_get_unread(1, NULL, 10, &count);
   TEST_ASSERT_EQUAL_INT(PHONE_DB_FAILURE, rc);
}

/* ============================================================================
 * Delete Tests
 * ============================================================================ */

static void test_sms_delete_by_id(void) {
   time_t now = time(NULL);
   int64_t id = 0;
   phone_db_sms_log_insert(10, PHONE_DIR_INCOMING, "+15557778888", "Del1", "to delete", now, &id);
   TEST_ASSERT_TRUE(id >= 1);

   int rc = phone_db_sms_log_delete(10, id);
   TEST_ASSERT_EQUAL_INT(PHONE_DB_SUCCESS, rc);

   rc = phone_db_sms_log_delete(10, id);
   TEST_ASSERT_EQUAL_INT(PHONE_DB_NOT_FOUND, rc);
}

static void test_sms_delete_user_isolation(void) {
   time_t now = time(NULL);
   int64_t id = 0;
   phone_db_sms_log_insert(11, PHONE_DIR_INCOMING, "+15557776666", "Iso", "isolated", now, &id);

   int rc = phone_db_sms_log_delete(12, id);
   TEST_ASSERT_EQUAL_INT(PHONE_DB_NOT_FOUND, rc);

   phone_sms_log_t entries[10];
   int count = 0;
   phone_db_sms_log_recent(11, entries, 10, &count);
   int found = 0;
   for (int i = 0; i < count; i++)
      if (entries[i].id == id) {
         found = 1;
         break;
      }
   TEST_ASSERT_TRUE_MESSAGE(found, "row still present after wrong-user delete attempt");
}

static void test_sms_delete_by_number(void) {
   time_t now = time(NULL);
   phone_db_sms_log_insert(13, PHONE_DIR_INCOMING, "+15551112222", "Spam", "one", now, NULL);
   phone_db_sms_log_insert(13, PHONE_DIR_OUTGOING, "+15551112222", "Spam", "two", now + 1, NULL);
   phone_db_sms_log_insert(13, PHONE_DIR_INCOMING, "+15551112222", "Spam", "three", now + 2, NULL);
   phone_db_sms_log_insert(13, PHONE_DIR_INCOMING, "+15559990000", "Other", "keep", now + 3, NULL);

   int count = -1;
   int rc_cnt = phone_db_sms_log_count_by_number(13, "+15551112222", &count);
   TEST_ASSERT_EQUAL_INT(PHONE_DB_SUCCESS, rc_cnt);
   TEST_ASSERT_EQUAL_INT(3, count);

   int deleted = -1;
   int rc = phone_db_sms_log_delete_by_number(13, "+15551112222", &deleted);
   TEST_ASSERT_EQUAL_INT(PHONE_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(3, deleted);

   phone_sms_log_t entries[10];
   int remaining = 0;
   phone_db_sms_log_recent(13, entries, 10, &remaining);
   TEST_ASSERT_EQUAL_INT(1, remaining);
   TEST_ASSERT_EQUAL_STRING("+15559990000", entries[0].number);

   deleted = -1;
   rc = phone_db_sms_log_delete_by_number(13, "+15551112222", &deleted);
   TEST_ASSERT_EQUAL_INT(PHONE_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(0, deleted);
}

static void test_sms_delete_number_normalization(void) {
   phone_db_sms_log_insert(14, PHONE_DIR_INCOMING, "+15553334444", "Norm", "formatted", time(NULL),
                           NULL);

   int count = 0;
   phone_db_sms_log_count_by_number(14, "+1-555-333-4444", &count);
   TEST_ASSERT_EQUAL_INT(1, count);

   count = 0;
   phone_db_sms_log_count_by_number(14, "(555) 333-4444", &count);
   TEST_ASSERT_EQUAL_INT(1, count);

   count = 0;
   phone_db_sms_log_count_by_number(14, "5553334444", &count);
   TEST_ASSERT_EQUAL_INT(1, count);

   count = 0;
   phone_db_sms_log_count_by_number(14, "15553334444", &count);
   TEST_ASSERT_EQUAL_INT(1, count);

   int deleted = 0;
   phone_db_sms_log_delete_by_number(14, "+15553334444", &deleted);
   TEST_ASSERT_EQUAL_INT(1, deleted);
}

static void test_call_delete_by_id(void) {
   time_t now = time(NULL);
   int64_t id = 0;
   phone_db_call_log_insert(15, PHONE_DIR_OUTGOING, "+15550000001", "X", 60, now,
                            PHONE_CALL_ANSWERED, &id);
   TEST_ASSERT_TRUE(id >= 1);

   int rc = phone_db_call_log_delete(15, id);
   TEST_ASSERT_EQUAL_INT(PHONE_DB_SUCCESS, rc);

   rc = phone_db_call_log_delete(15, id);
   TEST_ASSERT_EQUAL_INT(PHONE_DB_NOT_FOUND, rc);

   int64_t id2 = 0;
   phone_db_call_log_insert(15, PHONE_DIR_INCOMING, "+15550000002", "Y", 30, now,
                            PHONE_CALL_ANSWERED, &id2);
   rc = phone_db_call_log_delete(16, id2);
   TEST_ASSERT_EQUAL_INT(PHONE_DB_NOT_FOUND, rc);
   rc = phone_db_call_log_delete(15, id2);
   TEST_ASSERT_EQUAL_INT(PHONE_DB_SUCCESS, rc);
}

static void test_sms_get_by_id(void) {
   time_t now = time(NULL);
   int64_t id = 0;
   phone_db_sms_log_insert(20, PHONE_DIR_INCOMING, "+15554445555", "Byid", "by id preview", now,
                           &id);
   TEST_ASSERT_TRUE(id >= 1);

   phone_sms_log_t out;
   int rc = phone_db_sms_log_get_by_id(20, id, &out);
   TEST_ASSERT_EQUAL_INT(PHONE_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT64(id, out.id);
   TEST_ASSERT_EQUAL_STRING("by id preview", out.body);

   rc = phone_db_sms_log_get_by_id(21, id, &out);
   TEST_ASSERT_EQUAL_INT(PHONE_DB_NOT_FOUND, rc);

   rc = phone_db_sms_log_get_by_id(20, 999999, &out);
   TEST_ASSERT_EQUAL_INT(PHONE_DB_NOT_FOUND, rc);
}

static void test_call_get_by_id(void) {
   time_t now = time(NULL);
   int64_t id = 0;
   phone_db_call_log_insert(22, PHONE_DIR_OUTGOING, "+15556667777", "Byid", 60, now,
                            PHONE_CALL_ANSWERED, &id);

   phone_call_log_t out;
   int rc = phone_db_call_log_get_by_id(22, id, &out);
   TEST_ASSERT_EQUAL_INT(PHONE_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(60, out.duration_sec);

   rc = phone_db_call_log_get_by_id(23, id, &out);
   TEST_ASSERT_EQUAL_INT(PHONE_DB_NOT_FOUND, rc);
}

static void test_sms_delete_older_than(void) {
   time_t now = time(NULL);
   time_t old_time = now - (45 * 86400);
   time_t fresh_time = now - (5 * 86400);

   phone_db_sms_log_insert(24, PHONE_DIR_INCOMING, "+15550000001", "Old1", "old 1", old_time, NULL);
   phone_db_sms_log_insert(24, PHONE_DIR_INCOMING, "+15550000002", "Old2", "old 2", old_time + 100,
                           NULL);
   phone_db_sms_log_insert(24, PHONE_DIR_INCOMING, "+15550000003", "Fresh", "fresh", fresh_time,
                           NULL);

   time_t cutoff = now - (30 * 86400);
   int count = 0;
   int rc_cnt = phone_db_sms_log_count_older_than(24, cutoff, &count);
   TEST_ASSERT_EQUAL_INT(PHONE_DB_SUCCESS, rc_cnt);
   TEST_ASSERT_EQUAL_INT(2, count);

   int deleted = -1;
   int rc = phone_db_sms_log_delete_older_than(24, cutoff, &deleted);
   TEST_ASSERT_EQUAL_INT(PHONE_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(2, deleted);

   phone_sms_log_t remaining[10];
   int rem = 0;
   phone_db_sms_log_recent(24, remaining, 10, &rem);
   TEST_ASSERT_EQUAL_INT(1, rem);
}

static void test_phone_number_normalize(void) {
   char out[32];

   phone_number_normalize("+15551234567", out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("+15551234567", out);

   phone_number_normalize("+1 (555) 123-4567", out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("+15551234567", out);

   phone_number_normalize("5551234567", out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("+15551234567", out);

   phone_number_normalize("15551234567", out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("+15551234567", out);

   phone_number_normalize(NULL, out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("", out);

   phone_number_normalize("+1-555-123-4567", out, sizeof(out));
   char out2[32];
   phone_number_normalize(out, out2, sizeof(out2));
   TEST_ASSERT_EQUAL_STRING(out, out2);

   phone_number_normalize("abc-def-ghij", out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("", out);

   phone_number_normalize("+123456789012345678901234567890", out, sizeof(out));
   TEST_ASSERT_EQUAL('+', out[0]);
   TEST_ASSERT_TRUE(strlen(out) < sizeof(out));

   phone_number_normalize("911", out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("911", out);

   /* Redaction */
   phone_number_redact("+15551234567", out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("...4567", out);

   phone_number_redact(NULL, out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("(none)", out);

   phone_number_redact("911", out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("911", out);

   /* TTS formatter */
   phone_number_format_for_tts("+15551234567", out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("1 5 5 5 1 2 3 4 5 6 7", out);

   phone_number_format_for_tts("(555) 123-4567", out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("5 5 5 1 2 3 4 5 6 7", out);

   phone_number_format_for_tts("ABC-GARBAGE", out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("", out);

   phone_number_format_for_tts(NULL, out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("", out);
}

static void test_call_delete_older_than(void) {
   time_t now = time(NULL);
   time_t old_time = now - (60 * 86400);
   time_t fresh_time = now - (5 * 86400);

   phone_db_call_log_insert(17, PHONE_DIR_OUTGOING, "+15551000001", "Old1", 30, old_time,
                            PHONE_CALL_ANSWERED, NULL);
   phone_db_call_log_insert(17, PHONE_DIR_OUTGOING, "+15551000002", "Old2", 40, old_time + 100,
                            PHONE_CALL_ANSWERED, NULL);
   phone_db_call_log_insert(17, PHONE_DIR_OUTGOING, "+15551000003", "Fresh", 50, fresh_time,
                            PHONE_CALL_ANSWERED, NULL);

   time_t cutoff = now - (30 * 86400);
   int count = 0;
   int rc_cnt = phone_db_call_log_count_older_than(17, cutoff, &count);
   TEST_ASSERT_EQUAL_INT(PHONE_DB_SUCCESS, rc_cnt);
   TEST_ASSERT_EQUAL_INT(2, count);

   int deleted = -1;
   int rc = phone_db_call_log_delete_older_than(17, cutoff, &deleted);
   TEST_ASSERT_EQUAL_INT(PHONE_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(2, deleted);

   phone_call_log_t remaining[10];
   int rem = 0;
   phone_db_call_log_recent(17, remaining, 10, &rem);
   TEST_ASSERT_EQUAL_INT(1, rem);
   TEST_ASSERT_EQUAL_STRING("Fresh", remaining[0].contact_name);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
   UNITY_BEGIN();

   RUN_TEST(test_call_log_insert);
   RUN_TEST(test_call_log_update);
   RUN_TEST(test_call_log_recent);
   RUN_TEST(test_call_log_user_isolation);
   RUN_TEST(test_sms_log_insert);
   RUN_TEST(test_sms_get_unread);
   RUN_TEST(test_sms_mark_read);
   RUN_TEST(test_sms_unread_excludes_outbound);
   RUN_TEST(test_sms_log_recent);
   RUN_TEST(test_sms_user_isolation);
   RUN_TEST(test_cleanup);
   RUN_TEST(test_null_params);
   RUN_TEST(test_sms_delete_by_id);
   RUN_TEST(test_sms_delete_user_isolation);
   RUN_TEST(test_sms_delete_by_number);
   RUN_TEST(test_sms_delete_number_normalization);
   RUN_TEST(test_phone_number_normalize);
   RUN_TEST(test_sms_get_by_id);
   RUN_TEST(test_sms_delete_older_than);
   RUN_TEST(test_call_get_by_id);
   RUN_TEST(test_call_delete_by_id);
   RUN_TEST(test_call_delete_older_than);

   return UNITY_END();
}
