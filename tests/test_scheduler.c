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
   printf("\n--- test_string_conversions ---\n");

   /* Event type round-trip */
   TEST_ASSERT(sched_event_type_from_str("timer") == SCHED_EVENT_TIMER, "type: timer round-trip");
   TEST_ASSERT(sched_event_type_from_str("alarm") == SCHED_EVENT_ALARM, "type: alarm round-trip");
   TEST_ASSERT(sched_event_type_from_str("reminder") == SCHED_EVENT_REMINDER,
               "type: reminder round-trip");
   TEST_ASSERT(sched_event_type_from_str("task") == SCHED_EVENT_TASK, "type: task round-trip");
   TEST_ASSERT(sched_event_type_from_str("bogus") == SCHED_EVENT_TIMER, "type: unknown -> timer");
   TEST_ASSERT(sched_event_type_from_str(NULL) == SCHED_EVENT_TIMER, "type: NULL -> timer");
   TEST_ASSERT(strcmp(sched_event_type_to_str(SCHED_EVENT_ALARM), "alarm") == 0,
               "type: to_str alarm");
   TEST_ASSERT(strcmp(sched_event_type_to_str(99), "timer") == 0, "type: to_str out-of-range");

   /* Status round-trip */
   TEST_ASSERT(sched_status_from_str("pending") == SCHED_STATUS_PENDING, "status: pending");
   TEST_ASSERT(sched_status_from_str("ringing") == SCHED_STATUS_RINGING, "status: ringing");
   TEST_ASSERT(sched_status_from_str("fired") == SCHED_STATUS_FIRED, "status: fired");
   TEST_ASSERT(sched_status_from_str("cancelled") == SCHED_STATUS_CANCELLED, "status: cancelled");
   TEST_ASSERT(sched_status_from_str("snoozed") == SCHED_STATUS_SNOOZED, "status: snoozed");
   TEST_ASSERT(sched_status_from_str("missed") == SCHED_STATUS_MISSED, "status: missed");
   TEST_ASSERT(sched_status_from_str("dismissed") == SCHED_STATUS_DISMISSED, "status: dismissed");
   TEST_ASSERT(sched_status_from_str("timed_out") == SCHED_STATUS_TIMED_OUT, "status: timed_out");
   TEST_ASSERT(sched_status_from_str("nope") == SCHED_STATUS_PENDING, "status: unknown -> pending");
   TEST_ASSERT(strcmp(sched_status_to_str(SCHED_STATUS_DISMISSED), "dismissed") == 0,
               "status: to_str dismissed");

   /* Recurrence round-trip */
   TEST_ASSERT(sched_recurrence_from_str("once") == SCHED_RECUR_ONCE, "recur: once");
   TEST_ASSERT(sched_recurrence_from_str("daily") == SCHED_RECUR_DAILY, "recur: daily");
   TEST_ASSERT(sched_recurrence_from_str("weekdays") == SCHED_RECUR_WEEKDAYS, "recur: weekdays");
   TEST_ASSERT(sched_recurrence_from_str("weekends") == SCHED_RECUR_WEEKENDS, "recur: weekends");
   TEST_ASSERT(sched_recurrence_from_str("weekly") == SCHED_RECUR_WEEKLY, "recur: weekly");
   TEST_ASSERT(sched_recurrence_from_str("custom") == SCHED_RECUR_CUSTOM, "recur: custom");
   TEST_ASSERT(sched_recurrence_from_str("xyz") == SCHED_RECUR_ONCE, "recur: unknown -> once");
   TEST_ASSERT(strcmp(sched_recurrence_to_str(SCHED_RECUR_WEEKLY), "weekly") == 0,
               "recur: to_str weekly");
}

/* ============================================================================
 * Test: Insert and Get
 * ============================================================================ */

static void test_insert_and_get(void) {
   printf("\n--- test_insert_and_get ---\n");

   sched_event_t ev = make_event();
   int64_t id = 0;
   int irc = scheduler_db_insert(&ev, &id);
   TEST_ASSERT(irc == SCHED_DB_SUCCESS, "insert returns SUCCESS");
   TEST_ASSERT(id > 0, "insert outputs positive ID");
   TEST_ASSERT(ev.id == id, "event.id set by insert");
   TEST_ASSERT(ev.created_at > 0, "created_at set by insert");

   sched_event_t got;
   int rc = scheduler_db_get(id, &got);
   TEST_ASSERT(rc == 0, "get by ID succeeds");
   TEST_ASSERT(got.id == id, "get: id matches");
   TEST_ASSERT(got.user_id == 1, "get: user_id matches");
   TEST_ASSERT(got.event_type == SCHED_EVENT_ALARM, "get: event_type matches");
   TEST_ASSERT(got.status == SCHED_STATUS_PENDING, "get: status matches");
   TEST_ASSERT(strcmp(got.name, "Test Alarm") == 0, "get: name matches");
   TEST_ASSERT(strcmp(got.message, "Wake up!") == 0, "get: message matches");
   TEST_ASSERT(got.fire_at == ev.fire_at, "get: fire_at matches");
   TEST_ASSERT(got.recurrence == SCHED_RECUR_ONCE, "get: recurrence matches");

   rc = scheduler_db_get(99999, &got);
   TEST_ASSERT(rc != 0, "get nonexistent ID returns failure");
}

/* ============================================================================
 * Test: Insert Checked (per-user and global limits)
 * ============================================================================ */

static void test_insert_checked_limits(void) {
   printf("\n--- test_insert_checked_limits ---\n");

   int max_per_user = 3;
   int max_total = 5;

   /* Insert up to per-user limit for user 1 */
   for (int i = 0; i < max_per_user; i++) {
      sched_event_t ev = make_event();
      snprintf(ev.name, SCHED_NAME_MAX, "User1 Event %d", i);
      int64_t id = 0;
      int irc = scheduler_db_insert_checked(&ev, max_per_user, max_total, &id);
      TEST_ASSERT(irc == SCHED_DB_SUCCESS && id > 0, "insert_checked within per-user limit");
   }

   /* Next insert for user 1 should fail with per-user limit */
   sched_event_t ev_over = make_event();
   strncpy(ev_over.name, "User1 Over Limit", SCHED_NAME_MAX - 1);
   int64_t dummy = 0;
   int rc = scheduler_db_insert_checked(&ev_over, max_per_user, max_total, &dummy);
   TEST_ASSERT(rc == SCHED_DB_USER_LIMIT, "insert_checked returns USER_LIMIT at per-user limit");

   /* Insert for user 2 up to global limit (5 total - 3 already = 2 more) */
   for (int i = 0; i < 2; i++) {
      sched_event_t ev = make_event();
      ev.user_id = 2;
      snprintf(ev.name, SCHED_NAME_MAX, "User2 Event %d", i);
      int64_t id = 0;
      int irc = scheduler_db_insert_checked(&ev, max_per_user, max_total, &id);
      TEST_ASSERT(irc == SCHED_DB_SUCCESS && id > 0, "insert_checked within global limit");
   }

   /* Next insert for user 2 should fail with global limit */
   sched_event_t ev_global = make_event();
   ev_global.user_id = 2;
   strncpy(ev_global.name, "User2 Over Global", SCHED_NAME_MAX - 1);
   rc = scheduler_db_insert_checked(&ev_global, max_per_user, max_total, &dummy);
   TEST_ASSERT(rc == SCHED_DB_GLOBAL_LIMIT, "insert_checked returns GLOBAL_LIMIT at global limit");

   /* Verify counts */
   int u1_count = 0, u2_count = 0;
   scheduler_db_count_user_events(1, &u1_count);
   scheduler_db_count_user_events(2, &u2_count);
   TEST_ASSERT(u1_count == 3, "user 1 has exactly 3 events");
   TEST_ASSERT(u2_count == 2, "user 2 has exactly 2 events");
}

/* ============================================================================
 * Test: Update Status
 * ============================================================================ */

static void test_update_status(void) {
   printf("\n--- test_update_status ---\n");

   sched_event_t ev = make_event();
   int64_t id = 0;
   scheduler_db_insert(&ev, &id);

   int rc = scheduler_db_update_status(id, SCHED_STATUS_RINGING);
   TEST_ASSERT(rc == 0, "update pending -> ringing succeeds");

   sched_event_t got;
   scheduler_db_get(id, &got);
   TEST_ASSERT(got.status == SCHED_STATUS_RINGING, "status is now ringing");

   rc = scheduler_db_update_status(id, SCHED_STATUS_DISMISSED);
   TEST_ASSERT(rc == 0, "update ringing -> dismissed succeeds");

   scheduler_db_get(id, &got);
   TEST_ASSERT(got.status == SCHED_STATUS_DISMISSED, "status is now dismissed");
}

/* ============================================================================
 * Test: Update Status with Fired At
 * ============================================================================ */

static void test_update_status_fired(void) {
   printf("\n--- test_update_status_fired ---\n");

   sched_event_t ev = make_event();
   int64_t id = 0;
   scheduler_db_insert(&ev, &id);

   time_t now = time(NULL);
   int rc = scheduler_db_update_status_fired(id, SCHED_STATUS_RINGING, now);
   TEST_ASSERT(rc == 0, "update_status_fired succeeds");

   sched_event_t got;
   scheduler_db_get(id, &got);
   TEST_ASSERT(got.status == SCHED_STATUS_RINGING, "status is ringing");
   TEST_ASSERT(got.fired_at == now, "fired_at matches");
}

/* ============================================================================
 * Test: Cancel (Optimistic)
 * ============================================================================ */

static void test_cancel_optimistic(void) {
   printf("\n--- test_cancel_optimistic ---\n");

   sched_event_t ev = make_event();
   int64_t id = 0;
   scheduler_db_insert(&ev, &id);

   int rc = scheduler_db_cancel(id);
   TEST_ASSERT(rc == 0, "cancel pending event succeeds");

   sched_event_t got;
   scheduler_db_get(id, &got);
   TEST_ASSERT(got.status == SCHED_STATUS_CANCELLED, "status is cancelled after cancel");

   /* Cancel again should fail — already cancelled */
   rc = scheduler_db_cancel(id);
   TEST_ASSERT(rc != 0, "cancel already-cancelled returns failure");

   /* Cancel a dismissed event should also fail */
   sched_event_t ev2 = make_event();
   int64_t id2 = 0;
   scheduler_db_insert(&ev2, &id2);
   scheduler_db_update_status(id2, SCHED_STATUS_DISMISSED);
   rc = scheduler_db_cancel(id2);
   TEST_ASSERT(rc != 0, "cancel dismissed event returns failure");
}

/* ============================================================================
 * Test: Dismiss (Optimistic)
 * ============================================================================ */

static void test_dismiss_optimistic(void) {
   printf("\n--- test_dismiss_optimistic ---\n");

   sched_event_t ev = make_event();
   int64_t id = 0;
   scheduler_db_insert(&ev, &id);

   /* Dismiss pending should fail — must be ringing */
   int rc = scheduler_db_dismiss(id);
   TEST_ASSERT(rc != 0, "dismiss pending returns failure");

   /* Set to ringing, then dismiss */
   scheduler_db_update_status(id, SCHED_STATUS_RINGING);
   rc = scheduler_db_dismiss(id);
   TEST_ASSERT(rc == 0, "dismiss ringing event succeeds");

   sched_event_t got;
   scheduler_db_get(id, &got);
   TEST_ASSERT(got.status == SCHED_STATUS_DISMISSED, "status is dismissed");
   TEST_ASSERT(got.fired_at > 0, "fired_at set on dismiss");

   /* Dismiss again should fail */
   rc = scheduler_db_dismiss(id);
   TEST_ASSERT(rc != 0, "dismiss already-dismissed returns failure");
}

/* ============================================================================
 * Test: Snooze
 * ============================================================================ */

static void test_snooze(void) {
   printf("\n--- test_snooze ---\n");

   sched_event_t ev = make_event();
   int64_t id = 0;
   scheduler_db_insert(&ev, &id);

   /* Set to ringing first */
   scheduler_db_update_status(id, SCHED_STATUS_RINGING);

   time_t new_fire = time(NULL) + 600;
   int rc = scheduler_db_snooze(id, new_fire);
   TEST_ASSERT(rc == 0, "snooze ringing event succeeds");

   sched_event_t got;
   scheduler_db_get(id, &got);
   TEST_ASSERT(got.status == SCHED_STATUS_SNOOZED, "status is snoozed");
   TEST_ASSERT(got.fire_at == new_fire, "fire_at updated to snooze time");
   TEST_ASSERT(got.snooze_count == 1, "snooze_count is 1");

   /* Snooze again (snoozed status is eligible) */
   time_t new_fire2 = time(NULL) + 1200;
   rc = scheduler_db_snooze(id, new_fire2);
   TEST_ASSERT(rc == 0, "snooze snoozed event succeeds");

   scheduler_db_get(id, &got);
   TEST_ASSERT(got.snooze_count == 2, "snooze_count is 2");
   TEST_ASSERT(got.fire_at == new_fire2, "fire_at updated to second snooze time");
}

/* ============================================================================
 * Test: Due Events
 * ============================================================================ */

static void test_due_events(void) {
   printf("\n--- test_due_events ---\n");

   time_t now = time(NULL);

   /* 2 events in the past (due) */
   sched_event_t ev1 = make_event();
   ev1.fire_at = now - 3600;
   strncpy(ev1.name, "Past Event 1", SCHED_NAME_MAX - 1);
   scheduler_db_insert(&ev1, NULL);

   sched_event_t ev2 = make_event();
   ev2.fire_at = now - 1800;
   strncpy(ev2.name, "Past Event 2", SCHED_NAME_MAX - 1);
   scheduler_db_insert(&ev2, NULL);

   /* 1 event in the future (not due) */
   sched_event_t ev3 = make_event();
   ev3.fire_at = now + 7200;
   strncpy(ev3.name, "Future Event", SCHED_NAME_MAX - 1);
   scheduler_db_insert(&ev3, NULL);

   sched_event_t results[10];
   int count = scheduler_db_get_due_events(results, 10);
   TEST_ASSERT(count == 2, "get_due_events returns exactly 2");

   /* Verify ordered by fire_at ASC */
   if (count == 2) {
      TEST_ASSERT(results[0].fire_at <= results[1].fire_at, "due events ordered by fire_at ASC");
   }
}

/* ============================================================================
 * Test: List User Events
 * ============================================================================ */

static void test_list_user_events(void) {
   printf("\n--- test_list_user_events ---\n");

   /* Insert mixed events for user 1 */
   sched_event_t ev1 = make_event();
   ev1.event_type = SCHED_EVENT_ALARM;
   strncpy(ev1.name, "User1 Alarm", SCHED_NAME_MAX - 1);
   scheduler_db_insert(&ev1, NULL);

   sched_event_t ev2 = make_event();
   ev2.event_type = SCHED_EVENT_TIMER;
   strncpy(ev2.name, "User1 Timer", SCHED_NAME_MAX - 1);
   scheduler_db_insert(&ev2, NULL);

   /* Insert event for user 2 */
   sched_event_t ev3 = make_event();
   ev3.user_id = 2;
   strncpy(ev3.name, "User2 Alarm", SCHED_NAME_MAX - 1);
   scheduler_db_insert(&ev3, NULL);

   sched_event_t results[10];

   /* List all for user 1 */
   int count = scheduler_db_list_user_events(1, -1, results, 10);
   TEST_ASSERT(count == 2, "list user 1 all types returns 2");

   /* List only alarms for user 1 */
   count = scheduler_db_list_user_events(1, SCHED_EVENT_ALARM, results, 10);
   TEST_ASSERT(count == 1, "list user 1 alarms only returns 1");

   /* List for user 2 */
   count = scheduler_db_list_user_events(2, -1, results, 10);
   TEST_ASSERT(count == 1, "list user 2 returns 1");
}

/* ============================================================================
 * Test: Find by Name (case-insensitive, no wildcards)
 * ============================================================================ */

static void test_find_by_name(void) {
   printf("\n--- test_find_by_name ---\n");

   sched_event_t ev = make_event();
   strncpy(ev.name, "Morning Alarm", SCHED_NAME_MAX - 1);
   scheduler_db_insert(&ev, NULL);

   sched_event_t found;

   /* Case-insensitive match */
   int rc = scheduler_db_find_by_name(1, "morning alarm", &found);
   TEST_ASSERT(rc == 0, "find 'morning alarm' case-insensitive succeeds");
   TEST_ASSERT(strcmp(found.name, "Morning Alarm") == 0, "found name matches original case");

   /* No wildcard injection — "%" is literal */
   rc = scheduler_db_find_by_name(1, "morning alarm%", &found);
   TEST_ASSERT(rc != 0, "find with '%' suffix returns not found");

   /* Nonexistent name */
   rc = scheduler_db_find_by_name(1, "nonexistent", &found);
   TEST_ASSERT(rc != 0, "find nonexistent returns failure");

   /* Wrong user */
   rc = scheduler_db_find_by_name(2, "Morning Alarm", &found);
   TEST_ASSERT(rc != 0, "find for wrong user_id returns failure");
}

/* ============================================================================
 * Test: Count Events
 * ============================================================================ */

static void test_count_events(void) {
   printf("\n--- test_count_events ---\n");

   /* Insert 3 pending events for user 1 */
   for (int i = 0; i < 3; i++) {
      sched_event_t ev = make_event();
      snprintf(ev.name, SCHED_NAME_MAX, "Count Event %d", i);
      scheduler_db_insert(&ev, NULL);
   }

   /* Insert 1 cancelled event (should not be counted) */
   sched_event_t ev_cancel = make_event();
   strncpy(ev_cancel.name, "Cancelled One", SCHED_NAME_MAX - 1);
   int64_t cancel_id = 0;
   scheduler_db_insert(&ev_cancel, &cancel_id);
   scheduler_db_update_status(cancel_id, SCHED_STATUS_CANCELLED);

   int u1_count = 0;
   scheduler_db_count_user_events(1, &u1_count);
   TEST_ASSERT(u1_count == 3, "count_user_events = 3 (cancelled excluded)");

   int total = 0;
   scheduler_db_count_total_events(&total);
   TEST_ASSERT(total == 3, "count_total_events = 3 (cancelled excluded)");
}

/* ============================================================================
 * Test: Get Ringing
 * ============================================================================ */

static void test_get_ringing(void) {
   printf("\n--- test_get_ringing ---\n");

   sched_event_t ev1 = make_event();
   strncpy(ev1.name, "Ringing One", SCHED_NAME_MAX - 1);
   int64_t id1 = 0;
   scheduler_db_insert(&ev1, &id1);

   sched_event_t ev2 = make_event();
   strncpy(ev2.name, "Still Pending", SCHED_NAME_MAX - 1);
   scheduler_db_insert(&ev2, NULL);

   /* Set only ev1 to ringing */
   scheduler_db_update_status(id1, SCHED_STATUS_RINGING);

   sched_event_t results[10];
   int count = scheduler_db_get_ringing(results, 10);
   TEST_ASSERT(count == 1, "get_ringing returns exactly 1");
   if (count == 1) {
      TEST_ASSERT(results[0].id == id1, "ringing event is the correct one");
   }
}

/* ============================================================================
 * Test: Cleanup Old Events
 * ============================================================================ */

static void test_cleanup_old_events(void) {
   printf("\n--- test_cleanup_old_events ---\n");

   time_t now = time(NULL);

   /* Old fired event (fired_at far in the past) */
   sched_event_t ev_old = make_event();
   strncpy(ev_old.name, "Old Fired", SCHED_NAME_MAX - 1);
   int64_t id_old = 0;
   scheduler_db_insert(&ev_old, &id_old);
   time_t old_time = now - 86400 * 10; /* 10 days ago */
   scheduler_db_update_status_fired(id_old, SCHED_STATUS_FIRED, old_time);

   /* Recent fired event (should not be deleted with retention_days=1) */
   sched_event_t ev_recent = make_event();
   strncpy(ev_recent.name, "Recent Fired", SCHED_NAME_MAX - 1);
   int64_t id_recent = 0;
   scheduler_db_insert(&ev_recent, &id_recent);
   scheduler_db_update_status_fired(id_recent, SCHED_STATUS_FIRED, now);

   /* Pending event with old created_at (should NOT be deleted — still active) */
   sched_event_t ev_pending = make_event();
   strncpy(ev_pending.name, "Old Pending", SCHED_NAME_MAX - 1);
   scheduler_db_insert(&ev_pending, NULL);

   int deleted = 0;
   scheduler_db_cleanup_old_events(1, &deleted);
   TEST_ASSERT(deleted == 1, "cleanup deletes exactly 1 old fired event");

   /* Verify old fired event is gone */
   sched_event_t got;
   int rc = scheduler_db_get(id_old, &got);
   TEST_ASSERT(rc != 0, "old fired event no longer exists");

   /* Verify recent fired event still exists */
   rc = scheduler_db_get(id_recent, &got);
   TEST_ASSERT(rc == 0, "recent fired event still exists");
}

/* ============================================================================
 * Test: Next Fire Time
 * ============================================================================ */

static void test_next_fire_time(void) {
   printf("\n--- test_next_fire_time ---\n");

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
   TEST_ASSERT(next == now + 1000, "next_fire_time returns earliest pending");

   /* Cancel earliest, verify next one */
   scheduler_db_cancel(id1);
   next = scheduler_db_next_fire_time();
   TEST_ASSERT(next == now + 2000, "after cancel, next_fire_time returns second");
}

/* ============================================================================
 * Test: Get Active by UUID
 * ============================================================================ */

static void test_get_active_by_uuid(void) {
   printf("\n--- test_get_active_by_uuid ---\n");

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
   TEST_ASSERT(count == 2, "get_active_by_uuid 'sat-001' returns 2");

   count = scheduler_db_get_active_by_uuid("sat-999", results, 10);
   TEST_ASSERT(count == 0, "get_active_by_uuid 'sat-999' returns 0");
}

/* ============================================================================
 * Test: Get Missed Events
 * ============================================================================ */

static void test_get_missed_events(void) {
   printf("\n--- test_get_missed_events ---\n");

   time_t now = time(NULL);

   /* Pending event with fire_at in the past */
   sched_event_t ev1 = make_event();
   ev1.fire_at = now - 3600;
   strncpy(ev1.name, "Missed One", SCHED_NAME_MAX - 1);
   scheduler_db_insert(&ev1, NULL);

   /* Pending event in the future */
   sched_event_t ev2 = make_event();
   ev2.fire_at = now + 3600;
   strncpy(ev2.name, "Future One", SCHED_NAME_MAX - 1);
   scheduler_db_insert(&ev2, NULL);

   sched_event_t results[10];
   int count = scheduler_db_get_missed_events(results, 10);
   TEST_ASSERT(count == 1, "get_missed_events returns 1 past event");
   if (count == 1) {
      TEST_ASSERT(strcmp(results[0].name, "Missed One") == 0, "missed event is correct one");
   }
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
   printf("=== Scheduler DB Unit Tests ===\n");

   /* String conversions don't need a DB */
   test_string_conversions();

   /* Each DB test gets a fresh in-memory database */
   setup_db();
   test_insert_and_get();
   teardown_db();

   setup_db();
   test_insert_checked_limits();
   teardown_db();

   setup_db();
   test_update_status();
   teardown_db();

   setup_db();
   test_update_status_fired();
   teardown_db();

   setup_db();
   test_cancel_optimistic();
   teardown_db();

   setup_db();
   test_dismiss_optimistic();
   teardown_db();

   setup_db();
   test_snooze();
   teardown_db();

   setup_db();
   test_due_events();
   teardown_db();

   setup_db();
   test_list_user_events();
   teardown_db();

   setup_db();
   test_find_by_name();
   teardown_db();

   setup_db();
   test_count_events();
   teardown_db();

   setup_db();
   test_get_ringing();
   teardown_db();

   setup_db();
   test_cleanup_old_events();
   teardown_db();

   setup_db();
   test_next_fire_time();
   teardown_db();

   setup_db();
   test_get_active_by_uuid();
   teardown_db();

   setup_db();
   test_get_missed_events();
   teardown_db();

   printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
   return tests_failed > 0 ? 1 : 0;
}
