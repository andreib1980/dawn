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

/* ============================================================================
 * Call Log Tests
 * ============================================================================ */

static void test_call_log_insert(void) {
   printf("\n--- test_call_log_insert ---\n");

   time_t now = time(NULL);
   int64_t id = phone_db_call_log_insert(1, PHONE_DIR_OUTGOING, "+15551234567", "Mom", 0, now,
                                         PHONE_CALL_ANSWERED);
   TEST_ASSERT(id >= 1, "call log insert returns positive ID");

   int64_t id2 = phone_db_call_log_insert(1, PHONE_DIR_INCOMING, "+15559876543", "Dad", 0, now + 1,
                                          PHONE_CALL_MISSED);
   TEST_ASSERT(id2 > id, "second insert has higher ID");
}

static void test_call_log_update(void) {
   printf("\n--- test_call_log_update ---\n");

   time_t now = time(NULL);
   int64_t id = phone_db_call_log_insert(1, PHONE_DIR_OUTGOING, "+15551234567", "Mom", 0, now,
                                         PHONE_CALL_ANSWERED);

   int rc = phone_db_call_log_update(id, 120, PHONE_CALL_ANSWERED);
   TEST_ASSERT(rc == 0, "update returns success");

   phone_call_log_t entries[10];
   int count = phone_db_call_log_recent(1, entries, 10);
   TEST_ASSERT(count >= 1, "recent returns at least 1 entry");

   /* Find our entry */
   int found = 0;
   for (int i = 0; i < count; i++) {
      if (entries[i].id == id) {
         TEST_ASSERT(entries[i].duration_sec == 120, "duration updated to 120");
         found = 1;
         break;
      }
   }
   TEST_ASSERT(found, "updated entry found in recent results");
}

static void test_call_log_recent(void) {
   printf("\n--- test_call_log_recent ---\n");

   phone_call_log_t entries[10];
   int count = phone_db_call_log_recent(1, entries, 10);
   TEST_ASSERT(count >= 2, "recent returns multiple entries");
   TEST_ASSERT(entries[0].timestamp >= entries[1].timestamp, "entries sorted by timestamp DESC");
}

static void test_call_log_user_isolation(void) {
   printf("\n--- test_call_log_user_isolation ---\n");

   time_t now = time(NULL);
   phone_db_call_log_insert(2, PHONE_DIR_OUTGOING, "+15551111111", "Other User", 30, now,
                            PHONE_CALL_ANSWERED);

   phone_call_log_t entries[10];
   int count = phone_db_call_log_recent(2, entries, 10);
   TEST_ASSERT(count == 1, "user 2 sees only their own entries");

   int count1 = phone_db_call_log_recent(1, entries, 10);
   TEST_ASSERT(count1 >= 2, "user 1 still has their entries");
}

/* ============================================================================
 * SMS Log Tests
 * ============================================================================ */

static void test_sms_log_insert(void) {
   printf("\n--- test_sms_log_insert ---\n");

   time_t now = time(NULL);
   int64_t id = phone_db_sms_log_insert(1, PHONE_DIR_INCOMING, "+15551234567", "Mom",
                                        "Hello from Mom!", now);
   TEST_ASSERT(id >= 1, "sms insert returns positive ID");
}

static void test_sms_get_unread(void) {
   printf("\n--- test_sms_get_unread ---\n");

   phone_sms_log_t entries[10];
   int count = phone_db_sms_get_unread(1, entries, 10);
   TEST_ASSERT(count >= 1, "unread returns at least 1 entry");
   TEST_ASSERT(entries[0].read == 0, "entry is unread");
   TEST_ASSERT(strcmp(entries[0].body, "Hello from Mom!") == 0, "body matches");
}

static void test_sms_mark_read(void) {
   printf("\n--- test_sms_mark_read ---\n");

   phone_sms_log_t entries[10];
   int count = phone_db_sms_get_unread(1, entries, 10);
   TEST_ASSERT(count >= 1, "has unread before mark");

   int rc = phone_db_sms_mark_read(entries[0].id);
   TEST_ASSERT(rc == 0, "mark_read returns success");

   int count2 = phone_db_sms_get_unread(1, entries, 10);
   TEST_ASSERT(count2 < count, "fewer unread after mark");
}

static void test_sms_unread_excludes_outbound(void) {
   printf("\n--- test_sms_unread_excludes_outbound ---\n");

   time_t now = time(NULL);
   /* Insert one inbound (read=0 by default) and one outbound (also read=0 but
    * direction=OUTGOING). The unread query should only return the inbound. */
   int user = 50;
   int64_t in_id = phone_db_sms_log_insert(user, PHONE_DIR_INCOMING, "+15551112222", "In",
                                           "incoming msg", now);
   int64_t out_id = phone_db_sms_log_insert(user, PHONE_DIR_OUTGOING, "+15553334444", "Out",
                                            "outgoing msg", now + 1);
   (void)in_id;
   (void)out_id;

   phone_sms_log_t entries[10];
   int count = phone_db_sms_get_unread(user, entries, 10);
   TEST_ASSERT(count == 1, "only inbound shows as unread");
   TEST_ASSERT(entries[0].direction == PHONE_DIR_INCOMING, "result is the inbound row");

   /* Cleanup */
   phone_db_sms_log_delete(user, in_id);
   phone_db_sms_log_delete(user, out_id);
}

static void test_sms_log_recent(void) {
   printf("\n--- test_sms_log_recent ---\n");

   /* Add another SMS */
   phone_db_sms_log_insert(1, PHONE_DIR_OUTGOING, "+15559876543", "Dad", "Hey Dad", time(NULL));

   phone_sms_log_t entries[10];
   int count = phone_db_sms_log_recent(1, entries, 10);
   TEST_ASSERT(count >= 2, "recent returns multiple SMS entries");
   TEST_ASSERT(entries[0].timestamp >= entries[1].timestamp,
               "SMS entries sorted by timestamp DESC");
}

static void test_sms_user_isolation(void) {
   printf("\n--- test_sms_user_isolation ---\n");

   phone_db_sms_log_insert(3, PHONE_DIR_INCOMING, "+15552222222", "Jane", "Hi there", time(NULL));

   phone_sms_log_t entries[10];
   int count = phone_db_sms_log_recent(3, entries, 10);
   TEST_ASSERT(count == 1, "user 3 sees only their SMS");
}

/* ============================================================================
 * Cleanup Tests
 * ============================================================================ */

static void test_cleanup(void) {
   printf("\n--- test_cleanup ---\n");

   /* Insert old entries (200 days ago) */
   time_t old_time = time(NULL) - (200 * 86400);
   phone_db_call_log_insert(1, PHONE_DIR_OUTGOING, "+15550000000", "Old", 10, old_time,
                            PHONE_CALL_ANSWERED);
   phone_db_sms_log_insert(1, PHONE_DIR_INCOMING, "+15550000000", "Old", "Old message", old_time);

   /* Verify they exist */
   phone_call_log_t calls[20];
   int call_count_before = phone_db_call_log_recent(1, calls, 20);
   phone_sms_log_t sms[20];
   int sms_count_before = phone_db_sms_log_recent(1, sms, 20);

   /* Cleanup with 90-day retention */
   int rc = phone_db_cleanup(90, 90);
   TEST_ASSERT(rc == 0, "cleanup returns success");

   /* Old entries should be gone */
   int call_count_after = phone_db_call_log_recent(1, calls, 20);
   int sms_count_after = phone_db_sms_log_recent(1, sms, 20);

   TEST_ASSERT(call_count_after < call_count_before, "old call log entries cleaned up");
   TEST_ASSERT(sms_count_after < sms_count_before, "old SMS log entries cleaned up");
}

/* ============================================================================
 * Edge Cases
 * ============================================================================ */

static void test_null_params(void) {
   printf("\n--- test_null_params ---\n");

   int64_t id = phone_db_call_log_insert(1, PHONE_DIR_OUTGOING, NULL, NULL, 0, time(NULL),
                                         PHONE_CALL_ANSWERED);
   TEST_ASSERT(id >= 1, "insert with NULL strings succeeds (defaults to empty)");

   int count = phone_db_call_log_recent(1, NULL, 10);
   TEST_ASSERT(count == -1, "recent with NULL out returns -1");

   count = phone_db_sms_get_unread(1, NULL, 10);
   TEST_ASSERT(count == -1, "unread with NULL out returns -1");
}

/* ============================================================================
 * Delete Tests (SMS + call, user-scoped, with normalization)
 * ============================================================================ */

static void test_sms_delete_by_id(void) {
   printf("\n--- test_sms_delete_by_id ---\n");

   time_t now = time(NULL);
   int64_t id = phone_db_sms_log_insert(10, PHONE_DIR_INCOMING, "+15557778888", "Del1", "to delete",
                                        now);
   TEST_ASSERT(id >= 1, "insert returns id");

   int rc = phone_db_sms_log_delete(10, id);
   TEST_ASSERT(rc == PHONE_DB_SUCCESS, "delete existing row returns SUCCESS");

   rc = phone_db_sms_log_delete(10, id);
   TEST_ASSERT(rc == PHONE_DB_NOT_FOUND, "delete missing row returns NOT_FOUND");
}

static void test_sms_delete_user_isolation(void) {
   printf("\n--- test_sms_delete_user_isolation ---\n");

   time_t now = time(NULL);
   int64_t id = phone_db_sms_log_insert(11, PHONE_DIR_INCOMING, "+15557776666", "Iso", "isolated",
                                        now);

   /* Wrong user cannot delete */
   int rc = phone_db_sms_log_delete(12, id);
   TEST_ASSERT(rc == PHONE_DB_NOT_FOUND, "other user gets NOT_FOUND");

   /* Row still exists */
   phone_sms_log_t entries[10];
   int count = phone_db_sms_log_recent(11, entries, 10);
   int found = 0;
   for (int i = 0; i < count; i++)
      if (entries[i].id == id) {
         found = 1;
         break;
      }
   TEST_ASSERT(found, "row still present after wrong-user delete attempt");

   /* Cleanup */
   phone_db_sms_log_delete(11, id);
}

static void test_sms_delete_by_number(void) {
   printf("\n--- test_sms_delete_by_number ---\n");

   time_t now = time(NULL);
   phone_db_sms_log_insert(13, PHONE_DIR_INCOMING, "+15551112222", "Spam", "one", now);
   phone_db_sms_log_insert(13, PHONE_DIR_OUTGOING, "+15551112222", "Spam", "two", now + 1);
   phone_db_sms_log_insert(13, PHONE_DIR_INCOMING, "+15551112222", "Spam", "three", now + 2);
   /* One from a different number shouldn't be touched */
   phone_db_sms_log_insert(13, PHONE_DIR_INCOMING, "+15559990000", "Other", "keep", now + 3);

   int count = -1;
   int rc_cnt = phone_db_sms_log_count_by_number(13, "+15551112222", &count);
   TEST_ASSERT(rc_cnt == PHONE_DB_SUCCESS, "count_by_number returns SUCCESS");
   TEST_ASSERT(count == 3, "count_by_number returns 3");

   int deleted = -1;
   int rc = phone_db_sms_log_delete_by_number(13, "+15551112222", &deleted);
   TEST_ASSERT(rc == PHONE_DB_SUCCESS, "delete_by_number returns SUCCESS");
   TEST_ASSERT(deleted == 3, "delete_by_number reports 3 deleted");

   /* The other-number row must still be there */
   phone_sms_log_t entries[10];
   int remaining = phone_db_sms_log_recent(13, entries, 10);
   TEST_ASSERT(remaining == 1, "unrelated row preserved");
   TEST_ASSERT(strcmp(entries[0].number, "+15559990000") == 0, "right row preserved");

   /* Deleting again returns 0 count */
   deleted = -1;
   rc = phone_db_sms_log_delete_by_number(13, "+15551112222", &deleted);
   TEST_ASSERT(rc == PHONE_DB_SUCCESS, "second delete still SUCCESS");
   TEST_ASSERT(deleted == 0, "second delete reports 0 deleted");
}

static void test_sms_delete_number_normalization(void) {
   printf("\n--- test_sms_delete_number_normalization ---\n");

   /* Row stored in E.164 */
   phone_db_sms_log_insert(14, PHONE_DIR_INCOMING, "+15553334444", "Norm", "formatted", time(NULL));

   /* LLM might send in many ways — all should match */
   int count = 0;
   phone_db_sms_log_count_by_number(14, "+1-555-333-4444", &count);
   TEST_ASSERT(count == 1, "dashes normalize");

   count = 0;
   phone_db_sms_log_count_by_number(14, "(555) 333-4444", &count);
   TEST_ASSERT(count == 1, "parens + spaces normalize to +1");

   count = 0;
   phone_db_sms_log_count_by_number(14, "5553334444", &count);
   TEST_ASSERT(count == 1, "bare 10-digit US number normalizes to +1");

   count = 0;
   phone_db_sms_log_count_by_number(14, "15553334444", &count);
   TEST_ASSERT(count == 1, "bare 11-digit with leading 1 normalizes");

   /* Cleanup */
   int deleted = 0;
   phone_db_sms_log_delete_by_number(14, "+15553334444", &deleted);
   TEST_ASSERT(deleted == 1, "cleanup delete succeeded");
}

static void test_call_delete_by_id(void) {
   printf("\n--- test_call_delete_by_id ---\n");

   time_t now = time(NULL);
   int64_t id = phone_db_call_log_insert(15, PHONE_DIR_OUTGOING, "+15550000001", "X", 60, now,
                                         PHONE_CALL_ANSWERED);
   TEST_ASSERT(id >= 1, "call insert returns id");

   int rc = phone_db_call_log_delete(15, id);
   TEST_ASSERT(rc == PHONE_DB_SUCCESS, "delete existing call returns SUCCESS");

   rc = phone_db_call_log_delete(15, id);
   TEST_ASSERT(rc == PHONE_DB_NOT_FOUND, "delete missing call returns NOT_FOUND");

   /* Wrong-user isolation */
   int64_t id2 = phone_db_call_log_insert(15, PHONE_DIR_INCOMING, "+15550000002", "Y", 30, now,
                                          PHONE_CALL_ANSWERED);
   rc = phone_db_call_log_delete(16, id2);
   TEST_ASSERT(rc == PHONE_DB_NOT_FOUND, "wrong user cannot delete");
   rc = phone_db_call_log_delete(15, id2);
   TEST_ASSERT(rc == PHONE_DB_SUCCESS, "correct user deletes it");
}

static void test_sms_get_by_id(void) {
   printf("\n--- test_sms_get_by_id ---\n");

   time_t now = time(NULL);
   int64_t id = phone_db_sms_log_insert(20, PHONE_DIR_INCOMING, "+15554445555", "Byid",
                                        "by id preview", now);
   TEST_ASSERT(id >= 1, "insert returns id");

   phone_sms_log_t out;
   int rc = phone_db_sms_log_get_by_id(20, id, &out);
   TEST_ASSERT(rc == PHONE_DB_SUCCESS, "get_by_id existing returns SUCCESS");
   TEST_ASSERT(out.id == id, "row id matches");
   TEST_ASSERT(strcmp(out.body, "by id preview") == 0, "body matches");

   /* Wrong user */
   rc = phone_db_sms_log_get_by_id(21, id, &out);
   TEST_ASSERT(rc == PHONE_DB_NOT_FOUND, "wrong user → NOT_FOUND");

   /* Missing id */
   rc = phone_db_sms_log_get_by_id(20, 999999, &out);
   TEST_ASSERT(rc == PHONE_DB_NOT_FOUND, "missing id → NOT_FOUND");

   phone_db_sms_log_delete(20, id);
}

static void test_call_get_by_id(void) {
   printf("\n--- test_call_get_by_id ---\n");

   time_t now = time(NULL);
   int64_t id = phone_db_call_log_insert(22, PHONE_DIR_OUTGOING, "+15556667777", "Byid", 60, now,
                                         PHONE_CALL_ANSWERED);

   phone_call_log_t out;
   int rc = phone_db_call_log_get_by_id(22, id, &out);
   TEST_ASSERT(rc == PHONE_DB_SUCCESS, "call get_by_id returns SUCCESS");
   TEST_ASSERT(out.duration_sec == 60, "duration matches");

   rc = phone_db_call_log_get_by_id(23, id, &out);
   TEST_ASSERT(rc == PHONE_DB_NOT_FOUND, "wrong user → NOT_FOUND");

   phone_db_call_log_delete(22, id);
}

static void test_sms_delete_older_than(void) {
   printf("\n--- test_sms_delete_older_than ---\n");

   time_t now = time(NULL);
   time_t old_time = now - (45 * 86400);
   time_t fresh_time = now - (5 * 86400);

   phone_db_sms_log_insert(24, PHONE_DIR_INCOMING, "+15550000001", "Old1", "old 1", old_time);
   phone_db_sms_log_insert(24, PHONE_DIR_INCOMING, "+15550000002", "Old2", "old 2", old_time + 100);
   phone_db_sms_log_insert(24, PHONE_DIR_INCOMING, "+15550000003", "Fresh", "fresh", fresh_time);

   time_t cutoff = now - (30 * 86400);
   int count = 0;
   int rc_cnt = phone_db_sms_log_count_older_than(24, cutoff, &count);
   TEST_ASSERT(rc_cnt == PHONE_DB_SUCCESS, "count_older_than returns SUCCESS");
   TEST_ASSERT(count == 2, "count_older_than returns 2");

   int deleted = -1;
   int rc = phone_db_sms_log_delete_older_than(24, cutoff, &deleted);
   TEST_ASSERT(rc == PHONE_DB_SUCCESS, "delete_older_than SUCCESS");
   TEST_ASSERT(deleted == 2, "deleted exactly 2 old SMS");

   phone_sms_log_t remaining[10];
   int rem = phone_db_sms_log_recent(24, remaining, 10);
   TEST_ASSERT(rem == 1, "fresh SMS preserved");
}

static void test_phone_number_normalize(void) {
   printf("\n--- test_phone_number_normalize ---\n");

   char out[32];

   phone_number_normalize("+15551234567", out, sizeof(out));
   TEST_ASSERT(strcmp(out, "+15551234567") == 0, "canonical passes through");

   phone_number_normalize("+1 (555) 123-4567", out, sizeof(out));
   TEST_ASSERT(strcmp(out, "+15551234567") == 0, "punctuation stripped");

   phone_number_normalize("5551234567", out, sizeof(out));
   TEST_ASSERT(strcmp(out, "+15551234567") == 0, "bare 10-digit gets +1");

   phone_number_normalize("15551234567", out, sizeof(out));
   TEST_ASSERT(strcmp(out, "+15551234567") == 0, "bare 11-digit leading 1 gets +");

   phone_number_normalize(NULL, out, sizeof(out));
   TEST_ASSERT(out[0] == '\0', "NULL input → empty out");

   /* Normalization is idempotent — critical for ingestion + delete consistency */
   phone_number_normalize("+1-555-123-4567", out, sizeof(out));
   char out2[32];
   phone_number_normalize(out, out2, sizeof(out2));
   TEST_ASSERT(strcmp(out, out2) == 0, "normalize is idempotent");

   /* Garbage / non-digit input produces empty — caller must fallback */
   phone_number_normalize("abc-def-ghij", out, sizeof(out));
   TEST_ASSERT(out[0] == '\0', "pure-alpha input produces empty");

   /* Overlong digit run truncates cleanly without crashing */
   phone_number_normalize("+123456789012345678901234567890", out, sizeof(out));
   TEST_ASSERT(out[0] == '+', "overlong digit input starts with +");
   TEST_ASSERT(strlen(out) < sizeof(out), "overlong digit input bounded");

   /* Short code (emergency / SMS shortcode) passes through bare */
   phone_number_normalize("911", out, sizeof(out));
   TEST_ASSERT(strcmp(out, "911") == 0, "short code preserved");

   /* Redaction */
   phone_number_redact("+15551234567", out, sizeof(out));
   TEST_ASSERT(strcmp(out, "...4567") == 0, "redact keeps last 4");

   phone_number_redact(NULL, out, sizeof(out));
   TEST_ASSERT(strcmp(out, "(none)") == 0, "redact NULL → (none)");

   phone_number_redact("911", out, sizeof(out));
   TEST_ASSERT(strcmp(out, "911") == 0, "redact short code passes through");

   /* TTS formatter: digits space-separated so Piper pronounces each individually */
   phone_number_format_for_tts("+15551234567", out, sizeof(out));
   TEST_ASSERT(strcmp(out, "1 5 5 5 1 2 3 4 5 6 7") == 0, "TTS format spaces digits");

   phone_number_format_for_tts("(555) 123-4567", out, sizeof(out));
   TEST_ASSERT(strcmp(out, "5 5 5 1 2 3 4 5 6 7") == 0, "TTS format strips punctuation");

   phone_number_format_for_tts("ABC-GARBAGE", out, sizeof(out));
   TEST_ASSERT(out[0] == '\0', "TTS format of all-alpha → empty");

   phone_number_format_for_tts(NULL, out, sizeof(out));
   TEST_ASSERT(out[0] == '\0', "TTS format NULL → empty");
}

static void test_call_delete_older_than(void) {
   printf("\n--- test_call_delete_older_than ---\n");

   time_t now = time(NULL);
   time_t old_time = now - (60 * 86400);  /* 60 days ago */
   time_t fresh_time = now - (5 * 86400); /* 5 days ago */

   phone_db_call_log_insert(17, PHONE_DIR_OUTGOING, "+15551000001", "Old1", 30, old_time,
                            PHONE_CALL_ANSWERED);
   phone_db_call_log_insert(17, PHONE_DIR_OUTGOING, "+15551000002", "Old2", 40, old_time + 100,
                            PHONE_CALL_ANSWERED);
   phone_db_call_log_insert(17, PHONE_DIR_OUTGOING, "+15551000003", "Fresh", 50, fresh_time,
                            PHONE_CALL_ANSWERED);

   /* Cutoff at 30 days → expect 2 deleted */
   time_t cutoff = now - (30 * 86400);
   int count = 0;
   int rc_cnt = phone_db_call_log_count_older_than(17, cutoff, &count);
   TEST_ASSERT(rc_cnt == PHONE_DB_SUCCESS, "count_older_than returns SUCCESS");
   TEST_ASSERT(count == 2, "count_older_than returns 2");

   int deleted = -1;
   int rc = phone_db_call_log_delete_older_than(17, cutoff, &deleted);
   TEST_ASSERT(rc == PHONE_DB_SUCCESS, "delete_older_than SUCCESS");
   TEST_ASSERT(deleted == 2, "deleted exactly 2 old calls");

   /* Fresh one still there */
   phone_call_log_t remaining[10];
   int rem = phone_db_call_log_recent(17, remaining, 10);
   TEST_ASSERT(rem == 1, "fresh call preserved");
   TEST_ASSERT(strcmp(remaining[0].contact_name, "Fresh") == 0, "right call preserved");
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
   printf("=== Phone DB Unit Tests ===\n");

   setup_db();

   /* Call log */
   test_call_log_insert();
   test_call_log_update();
   test_call_log_recent();
   test_call_log_user_isolation();

   /* SMS log */
   test_sms_log_insert();
   test_sms_get_unread();
   test_sms_mark_read();
   test_sms_unread_excludes_outbound();
   test_sms_log_recent();
   test_sms_user_isolation();

   /* Cleanup */
   test_cleanup();

   /* Edge cases */
   test_null_params();

   /* Delete operations (new for SMS deletion feature) */
   test_sms_delete_by_id();
   test_sms_delete_user_isolation();
   test_sms_delete_by_number();
   test_sms_delete_number_normalization();
   test_phone_number_normalize();
   test_sms_get_by_id();
   test_sms_delete_older_than();
   test_call_get_by_id();
   test_call_delete_by_id();
   test_call_delete_older_than();

   teardown_db();

   printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
   return tests_failed > 0 ? 1 : 0;
}
