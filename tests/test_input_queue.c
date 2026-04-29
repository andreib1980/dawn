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
 * Unit tests for the unified input queue (circular buffer, FIFO, overflow).
 */

#include <string.h>

#include "input_queue.h"
#include "unity.h"

void setUp(void) {
   input_queue_clear();
}

void tearDown(void) {
}

/* ── Empty Queue ────────────────────────────────────────────────────────── */

static void test_empty_queue(void) {
   TEST_ASSERT_EQUAL_INT(0, input_queue_has_item());
   TEST_ASSERT_EQUAL_INT(0, input_queue_get_count());
   queued_input_t out;
   TEST_ASSERT_EQUAL_INT(0, input_queue_pop(&out));
}

/* ── Push and Pop ───────────────────────────────────────────────────────── */

static void test_push_and_pop(void) {
   TEST_ASSERT_EQUAL_INT(1, input_queue_push(INPUT_SOURCE_VOICE, "hello"));
   TEST_ASSERT_EQUAL_INT(1, input_queue_has_item());
   TEST_ASSERT_EQUAL_INT(1, input_queue_get_count());

   queued_input_t out;
   TEST_ASSERT_EQUAL_INT(1, input_queue_pop(&out));
   TEST_ASSERT_EQUAL_INT(INPUT_SOURCE_VOICE, out.source);
   TEST_ASSERT_EQUAL_STRING("hello", out.text);

   TEST_ASSERT_EQUAL_INT(0, input_queue_has_item());
   TEST_ASSERT_EQUAL_INT(0, input_queue_get_count());
}

/* ── FIFO Order ─────────────────────────────────────────────────────────── */

static void test_fifo_order(void) {
   input_queue_push(INPUT_SOURCE_TUI, "first");
   input_queue_push(INPUT_SOURCE_TUI, "second");
   input_queue_push(INPUT_SOURCE_TUI, "third");

   queued_input_t out;
   input_queue_pop(&out);
   TEST_ASSERT_EQUAL_STRING("first", out.text);
   input_queue_pop(&out);
   TEST_ASSERT_EQUAL_STRING("second", out.text);
   input_queue_pop(&out);
   TEST_ASSERT_EQUAL_STRING("third", out.text);
}

/* ── Multiple Sources ───────────────────────────────────────────────────── */

static void test_multiple_sources(void) {
   input_queue_push(INPUT_SOURCE_VOICE, "voice cmd");
   input_queue_push(INPUT_SOURCE_MQTT, "mqtt cmd");
   input_queue_push(INPUT_SOURCE_WEBSOCKET, "ws cmd");

   queued_input_t out;
   input_queue_pop(&out);
   TEST_ASSERT_EQUAL_INT(INPUT_SOURCE_VOICE, out.source);
   TEST_ASSERT_EQUAL_STRING("voice cmd", out.text);

   input_queue_pop(&out);
   TEST_ASSERT_EQUAL_INT(INPUT_SOURCE_MQTT, out.source);
   TEST_ASSERT_EQUAL_STRING("mqtt cmd", out.text);

   input_queue_pop(&out);
   TEST_ASSERT_EQUAL_INT(INPUT_SOURCE_WEBSOCKET, out.source);
   TEST_ASSERT_EQUAL_STRING("ws cmd", out.text);
}

/* ── Full Queue Eviction ────────────────────────────────────────────────── */

static void test_full_queue_eviction(void) {
   char buf[32];
   for (int i = 0; i < INPUT_QUEUE_MAX_ITEMS; i++) {
      snprintf(buf, sizeof(buf), "item_%d", i);
      TEST_ASSERT_EQUAL_INT(1, input_queue_push(INPUT_SOURCE_TUI, buf));
   }
   TEST_ASSERT_EQUAL_INT(INPUT_QUEUE_MAX_ITEMS, input_queue_get_count());

   /* Push one more — oldest (item_0) should be evicted */
   TEST_ASSERT_EQUAL_INT(1, input_queue_push(INPUT_SOURCE_TUI, "overflow"));
   TEST_ASSERT_EQUAL_INT(INPUT_QUEUE_MAX_ITEMS, input_queue_get_count());

   queued_input_t out;
   input_queue_pop(&out);
   TEST_ASSERT_EQUAL_STRING("item_1", out.text);
}

/* ── Count After Pop ────────────────────────────────────────────────────── */

static void test_count_after_pop(void) {
   input_queue_push(INPUT_SOURCE_TUI, "a");
   input_queue_push(INPUT_SOURCE_TUI, "b");
   input_queue_push(INPUT_SOURCE_TUI, "c");
   TEST_ASSERT_EQUAL_INT(3, input_queue_get_count());

   queued_input_t out;
   input_queue_pop(&out);
   TEST_ASSERT_EQUAL_INT(2, input_queue_get_count());
}

/* ── Clear ──────────────────────────────────────────────────────────────── */

static void test_clear(void) {
   input_queue_push(INPUT_SOURCE_TUI, "a");
   input_queue_push(INPUT_SOURCE_TUI, "b");
   TEST_ASSERT_EQUAL_INT(2, input_queue_get_count());

   input_queue_clear();
   TEST_ASSERT_EQUAL_INT(0, input_queue_get_count());
   TEST_ASSERT_EQUAL_INT(0, input_queue_has_item());
}

/* ── Null Text ──────────────────────────────────────────────────────────── */

static void test_push_null_text(void) {
   TEST_ASSERT_EQUAL_INT(0, input_queue_push(INPUT_SOURCE_TUI, NULL));
   TEST_ASSERT_EQUAL_INT(0, input_queue_get_count());
}

/* ── Invalid Source ─────────────────────────────────────────────────────── */

static void test_push_invalid_source(void) {
   TEST_ASSERT_EQUAL_INT(0, input_queue_push(INPUT_SOURCE_COUNT, "bad"));
   TEST_ASSERT_EQUAL_INT(0, input_queue_push((input_source_t)99, "bad"));
   TEST_ASSERT_EQUAL_INT(0, input_queue_get_count());
}

/* ── Pop Null Output ────────────────────────────────────────────────────── */

static void test_pop_null_output(void) {
   input_queue_push(INPUT_SOURCE_TUI, "something");
   TEST_ASSERT_EQUAL_INT(0, input_queue_pop(NULL));
   TEST_ASSERT_EQUAL_INT(1, input_queue_get_count());
}

/* ── Source Names ───────────────────────────────────────────────────────── */

static void test_source_names(void) {
   TEST_ASSERT_EQUAL_STRING("voice", input_source_name(INPUT_SOURCE_VOICE));
   TEST_ASSERT_EQUAL_STRING("TUI", input_source_name(INPUT_SOURCE_TUI));
   TEST_ASSERT_EQUAL_STRING("MQTT", input_source_name(INPUT_SOURCE_MQTT));
   TEST_ASSERT_EQUAL_STRING("network", input_source_name(INPUT_SOURCE_NETWORK));
   TEST_ASSERT_EQUAL_STRING("REST", input_source_name(INPUT_SOURCE_REST));
   TEST_ASSERT_EQUAL_STRING("WebSocket", input_source_name(INPUT_SOURCE_WEBSOCKET));
}

/* ── Source Name Unknown ────────────────────────────────────────────────── */

static void test_source_name_unknown(void) {
   TEST_ASSERT_EQUAL_STRING("unknown", input_source_name(INPUT_SOURCE_COUNT));
   TEST_ASSERT_EQUAL_STRING("unknown", input_source_name((input_source_t)99));
}

/* ── Text Truncation ────────────────────────────────────────────────────── */

static void test_text_truncation(void) {
   char long_text[INPUT_QUEUE_MAX_TEXT + 100];
   memset(long_text, 'A', sizeof(long_text) - 1);
   long_text[sizeof(long_text) - 1] = '\0';

   TEST_ASSERT_EQUAL_INT(1, input_queue_push(INPUT_SOURCE_TUI, long_text));

   queued_input_t out;
   input_queue_pop(&out);
   TEST_ASSERT_EQUAL_size_t(INPUT_QUEUE_MAX_TEXT, strlen(out.text));
   TEST_ASSERT_EQUAL_CHAR('A', out.text[0]);
   TEST_ASSERT_EQUAL_CHAR('A', out.text[INPUT_QUEUE_MAX_TEXT - 1]);
   TEST_ASSERT_EQUAL_CHAR('\0', out.text[INPUT_QUEUE_MAX_TEXT]);
}

/* ── Main ───────────────────────────────────────────────────────────────── */

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_empty_queue);
   RUN_TEST(test_push_and_pop);
   RUN_TEST(test_fifo_order);
   RUN_TEST(test_multiple_sources);
   RUN_TEST(test_full_queue_eviction);
   RUN_TEST(test_count_after_pop);
   RUN_TEST(test_clear);
   RUN_TEST(test_push_null_text);
   RUN_TEST(test_push_invalid_source);
   RUN_TEST(test_pop_null_output);
   RUN_TEST(test_source_names);
   RUN_TEST(test_source_name_unknown);
   RUN_TEST(test_text_truncation);
   return UNITY_END();
}
