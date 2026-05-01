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
 * Regression tests for the ID-based extraction filter (Feature 3).
 *
 * These tests verify the filter algorithm directly — no DB, LLM, or daemon
 * harness required.  They catch off-by-one regressions in the cursor logic
 * that replaced the old count-based filter.
 */

#include <json-c/json.h>
#include <stdint.h>
#include <string.h>

#include "unity.h"

void setUp(void) {
}
void tearDown(void) {
}

/* =============================================================================
 * Re-implementation of the production filter algorithm under test.
 *
 * This mirrors memory_extraction.c's ID-based filter loop verbatim so the test
 * is explicitly pinned to the algorithm.  Any deviation between this and the
 * production code is a test defect to be fixed.
 *
 * Returns a new json_object array (caller calls json_object_put).
 * ============================================================================= */

static struct json_object *apply_filter(struct json_object *history, int64_t last_msg_id) {
   struct json_object *filtered = json_object_new_array();
   size_t arr_len = json_object_array_length(history);

   for (size_t i = 0; i < arr_len; i++) {
      struct json_object *msg = json_object_array_get_idx(history, i);
      struct json_object *role_obj, *id_obj;
      if (!json_object_object_get_ex(msg, "role", &role_obj))
         continue;
      if (strcmp(json_object_get_string(role_obj), "system") == 0)
         continue;
      int64_t msg_id = 0;
      if (json_object_object_get_ex(msg, "id", &id_obj))
         msg_id = json_object_get_int64(id_obj);
      if (msg_id == 0 || msg_id > last_msg_id)
         json_object_array_add(filtered, json_object_get(msg));
   }

   return filtered;
}

/* =============================================================================
 * Helpers
 * ============================================================================= */

static struct json_object *make_msg(const char *role, int64_t id) {
   struct json_object *m = json_object_new_object();
   json_object_object_add(m, "role", json_object_new_string(role));
   if (id > 0)
      json_object_object_add(m, "id", json_object_new_int64(id));
   return m;
}

/* Return the role of the i-th element of arr (or "" on error). */
static const char *arr_role(struct json_object *arr, int i) {
   struct json_object *m = json_object_array_get_idx(arr, i);
   if (!m)
      return "";
   struct json_object *r;
   if (!json_object_object_get_ex(m, "role", &r))
      return "";
   return json_object_get_string(r);
}

static int64_t arr_id(struct json_object *arr, int i) {
   struct json_object *m = json_object_array_get_idx(arr, i);
   if (!m)
      return -1;
   struct json_object *r;
   if (!json_object_object_get_ex(m, "id", &r))
      return 0;
   return json_object_get_int64(r);
}

/* =============================================================================
 * Test 1: mixed-role sequence with one new message
 *
 * History: [system, user(1), assistant(2), tool(3), assistant(4), user(5)]
 * last_msg_id = 4 (extraction cursor after the first four non-system messages)
 * Expected: only user(5) is included — no overlap, no gap.
 * ============================================================================= */

static void test_mixed_roles_one_new_message(void) {
   struct json_object *h = json_object_new_array();
   json_object_array_add(h, make_msg("system", 0));
   json_object_array_add(h, make_msg("user", 1));
   json_object_array_add(h, make_msg("assistant", 2));
   json_object_array_add(h, make_msg("tool", 3));
   json_object_array_add(h, make_msg("assistant", 4));
   json_object_array_add(h, make_msg("user", 5));

   struct json_object *filtered = apply_filter(h, 4);

   TEST_ASSERT_EQUAL_INT_MESSAGE(1, (int)json_object_array_length(filtered),
                                 "exactly one new message");
   TEST_ASSERT_EQUAL_STRING_MESSAGE("user", arr_role(filtered, 0), "new message is user");
   TEST_ASSERT_EQUAL_INT64_MESSAGE(5, arr_id(filtered, 0), "new message has id=5");

   json_object_put(filtered);
   json_object_put(h);
}

/* =============================================================================
 * Test 2: all-user history with one new message
 *
 * History: [user(1), user(2), user(3), user(4)]
 * last_msg_id = 3  →  only user(4) included.
 * ============================================================================= */

static void test_all_user_one_new(void) {
   struct json_object *h = json_object_new_array();
   json_object_array_add(h, make_msg("user", 1));
   json_object_array_add(h, make_msg("user", 2));
   json_object_array_add(h, make_msg("user", 3));
   json_object_array_add(h, make_msg("user", 4));

   struct json_object *filtered = apply_filter(h, 3);

   TEST_ASSERT_EQUAL_INT_MESSAGE(1, (int)json_object_array_length(filtered),
                                 "exactly one new message");
   TEST_ASSERT_EQUAL_INT64_MESSAGE(4, arr_id(filtered, 0), "new message has id=4");

   json_object_put(filtered);
   json_object_put(h);
}

/* =============================================================================
 * Test 3: sequential extractions — no overlap, no gap
 *
 * Pass 1: history [user(1), assistant(2)]  last_msg_id=0 → both included
 * Pass 2: history [user(1), assistant(2), user(3), assistant(4)]
 *         last_msg_id=2 → only user(3) + assistant(4) included.
 * ============================================================================= */

static void test_sequential_no_overlap_no_gap(void) {
   /* Pass 1: first two messages, cursor at 0 → both included */
   struct json_object *h1 = json_object_new_array();
   json_object_array_add(h1, make_msg("user", 1));
   json_object_array_add(h1, make_msg("assistant", 2));

   struct json_object *f1 = apply_filter(h1, 0);
   TEST_ASSERT_EQUAL_INT_MESSAGE(2, (int)json_object_array_length(f1), "pass 1: both messages");
   json_object_put(f1);
   json_object_put(h1);

   /* Pass 2: four messages, cursor advanced to 2 → only last two */
   struct json_object *h2 = json_object_new_array();
   json_object_array_add(h2, make_msg("user", 1));
   json_object_array_add(h2, make_msg("assistant", 2));
   json_object_array_add(h2, make_msg("user", 3));
   json_object_array_add(h2, make_msg("assistant", 4));

   struct json_object *f2 = apply_filter(h2, 2);
   TEST_ASSERT_EQUAL_INT_MESSAGE(2, (int)json_object_array_length(f2),
                                 "pass 2: exactly two new messages");
   TEST_ASSERT_EQUAL_INT64_MESSAGE(3, arr_id(f2, 0), "pass 2 first new msg id=3");
   TEST_ASSERT_EQUAL_INT64_MESSAGE(4, arr_id(f2, 1), "pass 2 second new msg id=4");

   json_object_put(f2);
   json_object_put(h2);
}

/* =============================================================================
 * Test 4: voice-only path — no "id" fields, all non-system messages included
 *
 * The voice path sets conv_id=0 and never stamps ids onto history entries.
 * last_msg_id=0, all messages have no "id" field → msg_id==0 → include all.
 * System messages still filtered.  No crash on missing id field.
 * ============================================================================= */

static void test_voice_only_no_id_field(void) {
   struct json_object *h = json_object_new_array();
   /* No "id" fields on any entry (voice-only path) */
   json_object_array_add(h, make_msg("system", 0));    /* no id, system → skip */
   json_object_array_add(h, make_msg("user", 0));      /* no id (id arg 0 = not stamped) */
   json_object_array_add(h, make_msg("assistant", 0)); /* no id */
   json_object_array_add(h, make_msg("user", 0));      /* no id */

   /* make_msg with id=0 doesn't add the "id" key — verify that */
   struct json_object *first_user = json_object_array_get_idx(h, 1);
   struct json_object *id_check;
   TEST_ASSERT_FALSE_MESSAGE(json_object_object_get_ex(first_user, "id", &id_check),
                             "voice-only entries must have no id field");

   struct json_object *filtered = apply_filter(h, 0);

   TEST_ASSERT_EQUAL_INT_MESSAGE(3, (int)json_object_array_length(filtered),
                                 "all non-system messages included when no ids");
   TEST_ASSERT_EQUAL_STRING_MESSAGE("user", arr_role(filtered, 0), "first is user");
   TEST_ASSERT_EQUAL_STRING_MESSAGE("assistant", arr_role(filtered, 1), "second is assistant");
   TEST_ASSERT_EQUAL_STRING_MESSAGE("user", arr_role(filtered, 2), "third is user");

   json_object_put(filtered);
   json_object_put(h);
}

/* =============================================================================
 * Test 5: last_msg_id = 0 with stamped ids — all included (fresh conversation)
 * ============================================================================= */

static void test_fresh_cursor_includes_all(void) {
   struct json_object *h = json_object_new_array();
   json_object_array_add(h, make_msg("system", 0));
   json_object_array_add(h, make_msg("user", 10));
   json_object_array_add(h, make_msg("assistant", 11));

   struct json_object *filtered = apply_filter(h, 0);

   TEST_ASSERT_EQUAL_INT_MESSAGE(2, (int)json_object_array_length(filtered),
                                 "both non-system messages with last=0");

   json_object_put(filtered);
   json_object_put(h);
}

/* =============================================================================
 * Test 6: system-only history — nothing extracted
 * ============================================================================= */

static void test_system_only_empty_result(void) {
   struct json_object *h = json_object_new_array();
   json_object_array_add(h, make_msg("system", 1));
   json_object_array_add(h, make_msg("system", 2));

   struct json_object *filtered = apply_filter(h, 0);
   TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)json_object_array_length(filtered),
                                 "system-only history yields empty filter");

   json_object_put(filtered);
   json_object_put(h);
}

/* =============================================================================
 * Test 7: up-to-date cursor — no new messages
 * ============================================================================= */

static void test_up_to_date_cursor_empty(void) {
   struct json_object *h = json_object_new_array();
   json_object_array_add(h, make_msg("user", 1));
   json_object_array_add(h, make_msg("assistant", 2));

   struct json_object *filtered = apply_filter(h, 2);
   TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)json_object_array_length(filtered),
                                 "cursor at max id yields nothing");

   json_object_put(filtered);
   json_object_put(h);
}

/* =============================================================================
 * main
 * ============================================================================= */

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_mixed_roles_one_new_message);
   RUN_TEST(test_all_user_one_new);
   RUN_TEST(test_sequential_no_overlap_no_gap);
   RUN_TEST(test_voice_only_no_id_field);
   RUN_TEST(test_fresh_cursor_includes_all);
   RUN_TEST(test_system_only_empty_result);
   RUN_TEST(test_up_to_date_cursor_empty);
   return UNITY_END();
}
