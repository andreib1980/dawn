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
 * Unit tests for scheduler_db.c — CRUD, queries, and string conversions.
 * Uses an in-memory SQLite database via the stubbed s_db global.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db_internal.h"
#include "core/scheduler_db.h"
#include "unity.h"

/* Must match auth_db_core.c v18 migration */
static const char *DDL = "CREATE TABLE IF NOT EXISTS users ("
                         "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
                         "  username TEXT UNIQUE NOT NULL"
                         ");"
                         "INSERT INTO users (id, username) VALUES (1, 'testuser');"
                         "INSERT INTO users (id, username) VALUES (2, 'otheruser');"
                         "CREATE TABLE IF NOT EXISTS scheduled_events ("
                         "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
                         "  user_id INTEGER NOT NULL,"
                         "  event_type TEXT NOT NULL DEFAULT 'timer',"
                         "  status TEXT NOT NULL DEFAULT 'pending',"
                         "  name TEXT NOT NULL,"
                         "  message TEXT,"
                         "  fire_at INTEGER NOT NULL,"
                         "  created_at INTEGER NOT NULL,"
                         "  duration_sec INTEGER DEFAULT 0,"
                         "  snoozed_until INTEGER DEFAULT 0,"
                         "  recurrence TEXT DEFAULT 'once',"
                         "  recurrence_days TEXT,"
                         "  original_time TEXT,"
                         "  source_uuid TEXT,"
                         "  source_location TEXT,"
                         "  source_client_type INTEGER DEFAULT 0,"
                         "  announce_all INTEGER DEFAULT 0,"
                         "  tool_name TEXT,"
                         "  tool_action TEXT,"
                         "  tool_value TEXT,"
                         "  fired_at INTEGER DEFAULT 0,"
                         "  snooze_count INTEGER DEFAULT 0,"
                         "  FOREIGN KEY (user_id) REFERENCES users(id)"
                         ");"
                         "CREATE INDEX IF NOT EXISTS idx_sched_status_fire "
                         "  ON scheduled_events(status, fire_at);"
                         "CREATE INDEX IF NOT EXISTS idx_sched_user "
                         "  ON scheduled_events(user_id, status);"
                         "CREATE INDEX IF NOT EXISTS idx_sched_user_name "
                         "  ON scheduled_events(user_id, status, name);"
                         "CREATE INDEX IF NOT EXISTS idx_sched_source "
                         "  ON scheduled_events(source_uuid);";

static void setup_db(void) {
   int rc = sqlite3_open(":memory:", &s_db.db);
   if (rc != SQLITE_OK) {
      fprintf(stderr, "Failed to open in-memory DB: %s\n", sqlite3_errmsg(s_db.db));
      exit(1);
   }
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
   s_db.initialized = false;
   if (s_db.db) {
      sqlite3_close(s_db.db);
      s_db.db = NULL;
   }
}

void setUp(void) {
   setup_db();
}

void tearDown(void) {
   teardown_db();
}

/* ============================================================================
 * Helper: create a populated event with sensible defaults
 * ============================================================================ */

static sched_event_t make_event(void) {
   sched_event_t ev;
   memset(&ev, 0, sizeof(ev));
   ev.user_id = 1;
   ev.event_type = SCHED_EVENT_ALARM;
   ev.status = SCHED_STATUS_PENDING;
   strncpy(ev.name, "Test Alarm", SCHED_NAME_MAX - 1);
   strncpy(ev.message, "Wake up!", SCHED_MESSAGE_MAX - 1);
   ev.fire_at = time(NULL) + 3600;
   ev.recurrence = SCHED_RECUR_ONCE;
   return ev;
}

/* ============================================================================
 * Test: String Conversions
 * ============================================================================ */

static void test_string_conversions(void) {
   /* Event type round-trip */
   TEST_ASSERT_EQUAL_INT(SCHED_EVENT_TIMER, sched_event_type_from_str("timer"));
   TEST_ASSERT_EQUAL_INT(SCHED_EVENT_ALARM, sched_event_type_from_str("alarm"));
   TEST_ASSERT_EQUAL_INT(SCHED_EVENT_REMINDER, sched_event_type_from_str("reminder"));
   TEST_ASSERT_EQUAL_INT(SCHED_EVENT_TASK, sched_event_type_from_str("task"));
   TEST_ASSERT_EQUAL_INT(SCHED_EVENT_TIMER, sched_event_type_from_str("bogus"));
   TEST_ASSERT_EQUAL_INT(SCHED_EVENT_TIMER, sched_event_type_from_str(NULL));
   TEST_ASSERT_EQUAL_STRING("alarm", sched_event_type_to_str(SCHED_EVENT_ALARM));
   TEST_ASSERT_EQUAL_STRING("timer", sched_event_type_to_str(99));

   /* Status round-trip */
   TEST_ASSERT_EQUAL_INT(SCHED_STATUS_PENDING, sched_status_from_str("pending"));
   TEST_ASSERT_EQUAL_INT(SCHED_STATUS_RINGING, sched_status_from_str("ringing"));
   TEST_ASSERT_EQUAL_INT(SCHED_STATUS_FIRED, sched_status_from_str("fired"));
   TEST_ASSERT_EQUAL_INT(SCHED_STATUS_CANCELLED, sched_status_from_str("cancelled"));
   TEST_ASSERT_EQUAL_INT(SCHED_STATUS_SNOOZED, sched_status_from_str("snoozed"));
   TEST_ASSERT_EQUAL_INT(SCHED_STATUS_MISSED, sched_status_from_str("missed"));
   TEST_ASSERT_EQUAL_INT(SCHED_STATUS_DISMISSED, sched_status_from_str("dismissed"));
   TEST_ASSERT_EQUAL_INT(SCHED_STATUS_TIMED_OUT, sched_status_from_str("timed_out"));
   TEST_ASSERT_EQUAL_INT(SCHED_STATUS_PENDING, sched_status_from_str("nope"));
   TEST_ASSERT_EQUAL_STRING("dismissed", sched_status_to_str(SCHED_STATUS_DISMISSED));

   /* Recurrence round-trip */
   TEST_ASSERT_EQUAL_INT(SCHED_RECUR_ONCE, sched_recurrence_from_str("once"));
   TEST_ASSERT_EQUAL_INT(SCHED_RECUR_DAILY, sched_recurrence_from_str("daily"));
   TEST_ASSERT_EQUAL_INT(SCHED_RECUR_WEEKDAYS, sched_recurrence_from_str("weekdays"));
   TEST_ASSERT_EQUAL_INT(SCHED_RECUR_WEEKENDS, sched_recurrence_from_str("weekends"));
   TEST_ASSERT_EQUAL_INT(SCHED_RECUR_WEEKLY, sched_recurrence_from_str("weekly"));
   TEST_ASSERT_EQUAL_INT(SCHED_RECUR_CUSTOM, sched_recurrence_from_str("custom"));
   TEST_ASSERT_EQUAL_INT(SCHED_RECUR_ONCE, sched_recurrence_from_str("xyz"));
   TEST_ASSERT_EQUAL_STRING("weekly", sched_recurrence_to_str(SCHED_RECUR_WEEKLY));
}

/* ============================================================================
 * Test: Insert and Get
 * ============================================================================ */

static void test_insert_and_get(void) {
   sched_event_t ev = make_event();
   int64_t id = 0;
   int irc = scheduler_db_insert(&ev, &id);
   TEST_ASSERT_EQUAL_INT(SCHED_DB_SUCCESS, irc);
   TEST_ASSERT_TRUE(id > 0);
   TEST_ASSERT_EQUAL_INT64(id, ev.id);
   TEST_ASSERT_TRUE(ev.created_at > 0);

   sched_event_t got;
   int rc = scheduler_db_get(id, &got);
   TEST_ASSERT_EQUAL_INT(0, rc);
   TEST_ASSERT_EQUAL_INT64(id, got.id);
   TEST_ASSERT_EQUAL_INT(1, got.user_id);
   TEST_ASSERT_EQUAL_INT(SCHED_EVENT_ALARM, got.event_type);
   TEST_ASSERT_EQUAL_INT(SCHED_STATUS_PENDING, got.status);
   TEST_ASSERT_EQUAL_STRING("Test Alarm", got.name);
   TEST_ASSERT_EQUAL_STRING("Wake up!", got.message);
   TEST_ASSERT_EQUAL_INT64(ev.fire_at, got.fire_at);
   TEST_ASSERT_EQUAL_INT(SCHED_RECUR_ONCE, got.recurrence);

   rc = scheduler_db_get(99999, &got);
   TEST_ASSERT_NOT_EQUAL(0, rc);
}

/* ============================================================================
 * Test: Insert Checked (per-user and global limits)
 * ============================================================================ */

static void test_insert_checked_limits(void) {
   int max_per_user = 3;
   int max_total = 5;

   for (int i = 0; i < max_per_user; i++) {
      sched_event_t ev = make_event();
      snprintf(ev.name, SCHED_NAME_MAX, "User1 Event %d", i);
      int64_t id = 0;
      int irc = scheduler_db_insert_checked(&ev, max_per_user, max_total, &id);
      TEST_ASSERT_EQUAL_INT(SCHED_DB_SUCCESS, irc);
      TEST_ASSERT_TRUE(id > 0);
   }

   sched_event_t ev_over = make_event();
   strncpy(ev_over.name, "User1 Over Limit", SCHED_NAME_MAX - 1);
   int64_t dummy = 0;
   int rc = scheduler_db_insert_checked(&ev_over, max_per_user, max_total, &dummy);
   TEST_ASSERT_EQUAL_INT(SCHED_DB_USER_LIMIT, rc);

   for (int i = 0; i < 2; i++) {
      sched_event_t ev = make_event();
      ev.user_id = 2;
      snprintf(ev.name, SCHED_NAME_MAX, "User2 Event %d", i);
      int64_t id = 0;
      int irc = scheduler_db_insert_checked(&ev, max_per_user, max_total, &id);
      TEST_ASSERT_EQUAL_INT(SCHED_DB_SUCCESS, irc);
      TEST_ASSERT_TRUE(id > 0);
   }

   sched_event_t ev_global = make_event();
   ev_global.user_id = 2;
   strncpy(ev_global.name, "User2 Over Global", SCHED_NAME_MAX - 1);
   rc = scheduler_db_insert_checked(&ev_global, max_per_user, max_total, &dummy);
   TEST_ASSERT_EQUAL_INT(SCHED_DB_GLOBAL_LIMIT, rc);

   int u1_count = 0, u2_count = 0;
   scheduler_db_count_user_events(1, &u1_count);
   scheduler_db_count_user_events(2, &u2_count);
   TEST_ASSERT_EQUAL_INT(3, u1_count);
   TEST_ASSERT_EQUAL_INT(2, u2_count);
}

/* ============================================================================
 * Test: Update Status
 * ============================================================================ */

static void test_update_status(void) {
   sched_event_t ev = make_event();
   int64_t id = 0;
   scheduler_db_insert(&ev, &id);

   int rc = scheduler_db_update_status(id, SCHED_STATUS_RINGING);
   TEST_ASSERT_EQUAL_INT(0, rc);

   sched_event_t got;
   scheduler_db_get(id, &got);
   TEST_ASSERT_EQUAL_INT(SCHED_STATUS_RINGING, got.status);

   rc = scheduler_db_update_status(id, SCHED_STATUS_DISMISSED);
   TEST_ASSERT_EQUAL_INT(0, rc);

   scheduler_db_get(id, &got);
   TEST_ASSERT_EQUAL_INT(SCHED_STATUS_DISMISSED, got.status);
}

/* ============================================================================
 * Test: Update Status with Fired At
 * ============================================================================ */

static void test_update_status_fired(void) {
   sched_event_t ev = make_event();
   int64_t id = 0;
   scheduler_db_insert(&ev, &id);

   time_t now = time(NULL);
   int rc = scheduler_db_update_status_fired(id, SCHED_STATUS_RINGING, now);
   TEST_ASSERT_EQUAL_INT(0, rc);

   sched_event_t got;
   scheduler_db_get(id, &got);
   TEST_ASSERT_EQUAL_INT(SCHED_STATUS_RINGING, got.status);
   TEST_ASSERT_EQUAL_INT64(now, got.fired_at);
}

/* ============================================================================
 * Test: Cancel (Optimistic)
 * ============================================================================ */

static void test_cancel_optimistic(void) {
   sched_event_t ev = make_event();
   int64_t id = 0;
   scheduler_db_insert(&ev, &id);

   int rc = scheduler_db_cancel(id);
   TEST_ASSERT_EQUAL_INT(0, rc);

   sched_event_t got;
   scheduler_db_get(id, &got);
   TEST_ASSERT_EQUAL_INT(SCHED_STATUS_CANCELLED, got.status);

   rc = scheduler_db_cancel(id);
   TEST_ASSERT_NOT_EQUAL(0, rc);

   sched_event_t ev2 = make_event();
   int64_t id2 = 0;
   scheduler_db_insert(&ev2, &id2);
   scheduler_db_update_status(id2, SCHED_STATUS_DISMISSED);
   rc = scheduler_db_cancel(id2);
   TEST_ASSERT_NOT_EQUAL(0, rc);
}

/* ============================================================================
 * Test: Dismiss (Optimistic)
 * ============================================================================ */

static void test_dismiss_optimistic(void) {
   sched_event_t ev = make_event();
   int64_t id = 0;
   scheduler_db_insert(&ev, &id);

   int rc = scheduler_db_dismiss(id);
   TEST_ASSERT_NOT_EQUAL(0, rc);

   scheduler_db_update_status(id, SCHED_STATUS_RINGING);
   rc = scheduler_db_dismiss(id);
   TEST_ASSERT_EQUAL_INT(0, rc);

   sched_event_t got;
   scheduler_db_get(id, &got);
   TEST_ASSERT_EQUAL_INT(SCHED_STATUS_DISMISSED, got.status);
   TEST_ASSERT_TRUE(got.fired_at > 0);

   rc = scheduler_db_dismiss(id);
   TEST_ASSERT_NOT_EQUAL(0, rc);
}

/* ============================================================================
 * Test: Snooze
 * ============================================================================ */

static void test_snooze(void) {
   sched_event_t ev = make_event();
   int64_t id = 0;
   scheduler_db_insert(&ev, &id);

   scheduler_db_update_status(id, SCHED_STATUS_RINGING);

   time_t new_fire = time(NULL) + 600;
   int rc = scheduler_db_snooze(id, new_fire);
   TEST_ASSERT_EQUAL_INT(0, rc);

   sched_event_t got;
   scheduler_db_get(id, &got);
   TEST_ASSERT_EQUAL_INT(SCHED_STATUS_SNOOZED, got.status);
   TEST_ASSERT_EQUAL_INT64(new_fire, got.fire_at);
   TEST_ASSERT_EQUAL_INT(1, got.snooze_count);

   time_t new_fire2 = time(NULL) + 1200;
   rc = scheduler_db_snooze(id, new_fire2);
   TEST_ASSERT_EQUAL_INT(0, rc);

   scheduler_db_get(id, &got);
   TEST_ASSERT_EQUAL_INT(2, got.snooze_count);
   TEST_ASSERT_EQUAL_INT64(new_fire2, got.fire_at);
}

/* ============================================================================
 * Test: Due Events
 * ============================================================================ */

static void test_due_events(void) {
   time_t now = time(NULL);

   sched_event_t ev1 = make_event();
   ev1.fire_at = now - 3600;
   strncpy(ev1.name, "Past Event 1", SCHED_NAME_MAX - 1);
   scheduler_db_insert(&ev1, NULL);

   sched_event_t ev2 = make_event();
   ev2.fire_at = now - 1800;
   strncpy(ev2.name, "Past Event 2", SCHED_NAME_MAX - 1);
   scheduler_db_insert(&ev2, NULL);

   sched_event_t ev3 = make_event();
   ev3.fire_at = now + 7200;
   strncpy(ev3.name, "Future Event", SCHED_NAME_MAX - 1);
   scheduler_db_insert(&ev3, NULL);

   sched_event_t results[10];
   int count = scheduler_db_get_due_events(results, 10);
   TEST_ASSERT_EQUAL_INT(2, count);

   if (count == 2) {
      TEST_ASSERT_TRUE(results[0].fire_at <= results[1].fire_at);
   }
}

/* ============================================================================
 * Test: List User Events
 * ============================================================================ */

static void test_list_user_events(void) {
   sched_event_t ev1 = make_event();
   ev1.event_type = SCHED_EVENT_ALARM;
   strncpy(ev1.name, "User1 Alarm", SCHED_NAME_MAX - 1);
   scheduler_db_insert(&ev1, NULL);

   sched_event_t ev2 = make_event();
   ev2.event_type = SCHED_EVENT_TIMER;
   strncpy(ev2.name, "User1 Timer", SCHED_NAME_MAX - 1);
   scheduler_db_insert(&ev2, NULL);

   sched_event_t ev3 = make_event();
   ev3.user_id = 2;
   strncpy(ev3.name, "User2 Alarm", SCHED_NAME_MAX - 1);
   scheduler_db_insert(&ev3, NULL);

   sched_event_t results[10];

   int count = scheduler_db_list_user_events(1, -1, results, 10);
   TEST_ASSERT_EQUAL_INT(2, count);

   count = scheduler_db_list_user_events(1, SCHED_EVENT_ALARM, results, 10);
   TEST_ASSERT_EQUAL_INT(1, count);

   count = scheduler_db_list_user_events(2, -1, results, 10);
   TEST_ASSERT_EQUAL_INT(1, count);
}

/* ============================================================================
 * Test: Find by Name
 * ============================================================================ */

static void test_find_by_name(void) {
   sched_event_t ev = make_event();
   strncpy(ev.name, "Morning Alarm", SCHED_NAME_MAX - 1);
   scheduler_db_insert(&ev, NULL);

   sched_event_t found;

   int rc = scheduler_db_find_by_name(1, "morning alarm", &found);
   TEST_ASSERT_EQUAL_INT(0, rc);
   TEST_ASSERT_EQUAL_STRING("Morning Alarm", found.name);

   rc = scheduler_db_find_by_name(1, "morning alarm%", &found);
   TEST_ASSERT_NOT_EQUAL(0, rc);

   rc = scheduler_db_find_by_name(1, "nonexistent", &found);
   TEST_ASSERT_NOT_EQUAL(0, rc);

   rc = scheduler_db_find_by_name(2, "Morning Alarm", &found);
   TEST_ASSERT_NOT_EQUAL(0, rc);
}

/* ============================================================================
 * Test: Count Events
 * ============================================================================ */

static void test_count_events(void) {
   for (int i = 0; i < 3; i++) {
      sched_event_t ev = make_event();
      snprintf(ev.name, SCHED_NAME_MAX, "Count Event %d", i);
      scheduler_db_insert(&ev, NULL);
   }

   sched_event_t ev_cancel = make_event();
   strncpy(ev_cancel.name, "Cancelled One", SCHED_NAME_MAX - 1);
   int64_t cancel_id = 0;
   scheduler_db_insert(&ev_cancel, &cancel_id);
   scheduler_db_update_status(cancel_id, SCHED_STATUS_CANCELLED);

   int u1_count = 0;
   scheduler_db_count_user_events(1, &u1_count);
   TEST_ASSERT_EQUAL_INT(3, u1_count);

   int total = 0;
   scheduler_db_count_total_events(&total);
   TEST_ASSERT_EQUAL_INT(3, total);
}

/* ============================================================================
 * Test: Get Ringing
 * ============================================================================ */

static void test_get_ringing(void) {
   sched_event_t ev1 = make_event();
   strncpy(ev1.name, "Ringing One", SCHED_NAME_MAX - 1);
   int64_t id1 = 0;
   scheduler_db_insert(&ev1, &id1);

   sched_event_t ev2 = make_event();
   strncpy(ev2.name, "Still Pending", SCHED_NAME_MAX - 1);
   scheduler_db_insert(&ev2, NULL);

   scheduler_db_update_status(id1, SCHED_STATUS_RINGING);

   sched_event_t results[10];
   int count = scheduler_db_get_ringing(results, 10);
   TEST_ASSERT_EQUAL_INT(1, count);
   if (count == 1) {
      TEST_ASSERT_EQUAL_INT64(id1, results[0].id);
   }
}

/* ============================================================================
 * Test: Cleanup Old Events
 * ============================================================================ */

static void test_cleanup_old_events(void) {
   time_t now = time(NULL);

   sched_event_t ev_old = make_event();
   strncpy(ev_old.name, "Old Fired", SCHED_NAME_MAX - 1);
   int64_t id_old = 0;
   scheduler_db_insert(&ev_old, &id_old);
   time_t old_time = now - 86400 * 10;
   scheduler_db_update_status_fired(id_old, SCHED_STATUS_FIRED, old_time);

   sched_event_t ev_recent = make_event();
   strncpy(ev_recent.name, "Recent Fired", SCHED_NAME_MAX - 1);
   int64_t id_recent = 0;
   scheduler_db_insert(&ev_recent, &id_recent);
   scheduler_db_update_status_fired(id_recent, SCHED_STATUS_FIRED, now);

   sched_event_t ev_pending = make_event();
   strncpy(ev_pending.name, "Old Pending", SCHED_NAME_MAX - 1);
   scheduler_db_insert(&ev_pending, NULL);

   int deleted = 0;
   scheduler_db_cleanup_old_events(1, &deleted);
   TEST_ASSERT_EQUAL_INT(1, deleted);

   sched_event_t got;
   int rc = scheduler_db_get(id_old, &got);
   TEST_ASSERT_NOT_EQUAL(0, rc);

   rc = scheduler_db_get(id_recent, &got);
   TEST_ASSERT_EQUAL_INT(0, rc);
}

/* ============================================================================
 * Test: Next Fire Time
 * ============================================================================ */

static void test_next_fire_time(void) {
   time_t now = time(NULL);

   sched_event_t ev1 = make_event();
   ev1.fire_at = now + 1000;
   strncpy(ev1.name, "Earliest", SCHED_NAME_MAX - 1);
   int64_t id1 = 0;
   scheduler_db_insert(&ev1, &id1);

   sched_event_t ev2 = make_event();
   ev2.fire_at = now + 2000;
   strncpy(ev2.name, "Middle", SCHED_NAME_MAX - 1);
   scheduler_db_insert(&ev2, NULL);

   sched_event_t ev3 = make_event();
   ev3.fire_at = now + 3000;
   strncpy(ev3.name, "Latest", SCHED_NAME_MAX - 1);
   scheduler_db_insert(&ev3, NULL);

   time_t next = scheduler_db_next_fire_time();
   TEST_ASSERT_EQUAL_INT64(now + 1000, next);

   scheduler_db_cancel(id1);
   next = scheduler_db_next_fire_time();
   TEST_ASSERT_EQUAL_INT64(now + 2000, next);
}

/* ============================================================================
 * Test: Get Active by UUID
 * ============================================================================ */

static void test_get_active_by_uuid(void) {
   sched_event_t ev1 = make_event();
   ev1.event_type = SCHED_EVENT_TIMER;
   strncpy(ev1.source_uuid, "sat-001", SCHED_UUID_MAX - 1);
   strncpy(ev1.name, "Timer for sat-001", SCHED_NAME_MAX - 1);
   scheduler_db_insert(&ev1, NULL);

   sched_event_t ev2 = make_event();
   ev2.event_type = SCHED_EVENT_TIMER;
   strncpy(ev2.source_uuid, "sat-001", SCHED_UUID_MAX - 1);
   strncpy(ev2.name, "Timer 2 for sat-001", SCHED_NAME_MAX - 1);
   scheduler_db_insert(&ev2, NULL);

   sched_event_t results[10];

   int count = scheduler_db_get_active_by_uuid("sat-001", results, 10);
   TEST_ASSERT_EQUAL_INT(2, count);

   count = scheduler_db_get_active_by_uuid("sat-999", results, 10);
   TEST_ASSERT_EQUAL_INT(0, count);
}

/* ============================================================================
 * Test: Get Missed Events
 * ============================================================================ */

static void test_get_missed_events(void) {
   time_t now = time(NULL);

   sched_event_t ev1 = make_event();
   ev1.fire_at = now - 3600;
   strncpy(ev1.name, "Missed One", SCHED_NAME_MAX - 1);
   scheduler_db_insert(&ev1, NULL);

   sched_event_t ev2 = make_event();
   ev2.fire_at = now + 3600;
   strncpy(ev2.name, "Future One", SCHED_NAME_MAX - 1);
   scheduler_db_insert(&ev2, NULL);

   sched_event_t results[10];
   int count = scheduler_db_get_missed_events(results, 10);
   TEST_ASSERT_EQUAL_INT(1, count);
   if (count == 1) {
      TEST_ASSERT_EQUAL_STRING("Missed One", results[0].name);
   }
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_string_conversions);
   RUN_TEST(test_insert_and_get);
   RUN_TEST(test_insert_checked_limits);
   RUN_TEST(test_update_status);
   RUN_TEST(test_update_status_fired);
   RUN_TEST(test_cancel_optimistic);
   RUN_TEST(test_dismiss_optimistic);
   RUN_TEST(test_snooze);
   RUN_TEST(test_due_events);
   RUN_TEST(test_list_user_events);
   RUN_TEST(test_find_by_name);
   RUN_TEST(test_count_events);
   RUN_TEST(test_get_ringing);
   RUN_TEST(test_cleanup_old_events);
   RUN_TEST(test_next_fire_time);
   RUN_TEST(test_get_active_by_uuid);
   RUN_TEST(test_get_missed_events);
   return UNITY_END();
}
