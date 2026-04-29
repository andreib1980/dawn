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
 * Unit tests for text_filter command tag stripping.
 */

#include <string.h>

#include "core/text_filter.h"
#include "unity.h"

static cmd_tag_filter_state_t s_state;

void setUp(void) {
   text_filter_reset(&s_state);
}

void tearDown(void) {
}

/* ── helper for buffer tests ────────────────────────────────────────────── */

static void filter_to_buf(const char *input, char *out, size_t out_size) {
   text_filter_command_tags_to_buffer(&s_state, input, out, out_size);
}

/* ── tests ──────────────────────────────────────────────────────────────── */

static void test_no_tags(void) {
   char buf[128];
   filter_to_buf("Hello world", buf, sizeof(buf));
   TEST_ASSERT_EQUAL_STRING("Hello world", buf);
}

static void test_simple_tag_removal(void) {
   char buf[128];
   filter_to_buf("Hello <command>do stuff</command> world", buf, sizeof(buf));
   TEST_ASSERT_EQUAL_STRING("Hello  world", buf);
}

static void test_tag_at_start(void) {
   char buf[128];
   filter_to_buf("<command>hidden</command>visible", buf, sizeof(buf));
   TEST_ASSERT_EQUAL_STRING("visible", buf);
}

static void test_tag_at_end(void) {
   char buf[128];
   filter_to_buf("visible<command>hidden</command>", buf, sizeof(buf));
   TEST_ASSERT_EQUAL_STRING("visible", buf);
}

static void test_only_tag_content(void) {
   char buf[128];
   filter_to_buf("<command>all hidden</command>", buf, sizeof(buf));
   TEST_ASSERT_EQUAL_STRING("", buf);
}

static void test_nested_tags(void) {
   char buf[128];
   filter_to_buf("<command>outer<command>inner</command></command>", buf, sizeof(buf));
   TEST_ASSERT_EQUAL_STRING("", buf);
}

static void test_multiple_tags(void) {
   char buf[128];
   filter_to_buf("a<command>x</command>b<command>y</command>c", buf, sizeof(buf));
   TEST_ASSERT_EQUAL_STRING("abc", buf);
}

static void test_streaming_across_chunks(void) {
   char buf[128] = "";
   int len = 0;

   len = text_filter_command_tags_to_buffer(&s_state, "<com", buf, sizeof(buf));

   char buf2[128] = "";
   int len2 = text_filter_command_tags_to_buffer(&s_state, "mand>hidden</command>after", buf2,
                                                 sizeof(buf2));

   TEST_ASSERT_EQUAL_STRING("after", buf2);
   TEST_ASSERT_EQUAL_INT(5, len2);
   (void)len;
}

static void test_partial_tag_not_matching(void) {
   char buf[128];
   filter_to_buf("<comm!and>", buf, sizeof(buf));
   TEST_ASSERT_EQUAL_STRING("<comm!and>", buf);
}

static void test_empty_input(void) {
   char buf[128];
   filter_to_buf("", buf, sizeof(buf));
   TEST_ASSERT_EQUAL_STRING("", buf);
}

static void test_buffer_overflow_protection(void) {
   char buf[5];
   int len = text_filter_command_tags_to_buffer(&s_state, "Hello world", buf, sizeof(buf));
   buf[sizeof(buf) - 1] = '\0';
   TEST_ASSERT_EQUAL_INT(4, len);
   TEST_ASSERT_EQUAL_STRING("Hell", buf);
}

static void test_reset_mid_tag(void) {
   char buf[128];

   /* Start a tag but don't finish it */
   filter_to_buf("<command>partial", buf, sizeof(buf));
   TEST_ASSERT_EQUAL_STRING("", buf);
   TEST_ASSERT_TRUE(s_state.nesting_depth > 0);

   /* Reset state */
   text_filter_reset(&s_state);
   TEST_ASSERT_EQUAL_INT(0, s_state.nesting_depth);
   TEST_ASSERT_EQUAL_INT(0, s_state.len);

   /* After reset, text passes through normally */
   filter_to_buf("clean text", buf, sizeof(buf));
   TEST_ASSERT_EQUAL_STRING("clean text", buf);
}

/* ── callback test ──────────────────────────────────────────────────────── */

typedef struct {
   char chunks[8][64];
   int count;
} capture_ctx_t;

static void capture_callback(const char *text, size_t len, void *ctx) {
   capture_ctx_t *cap = (capture_ctx_t *)ctx;
   if (cap->count < 8 && len < 64) {
      memcpy(cap->chunks[cap->count], text, len);
      cap->chunks[cap->count][len] = '\0';
      cap->count++;
   }
}

static void test_callback_receives_chunks(void) {
   capture_ctx_t cap = { .count = 0 };

   text_filter_command_tags(&s_state, "before<command>hidden</command>after", capture_callback,
                            &cap);

   TEST_ASSERT_EQUAL_INT(2, cap.count);
   TEST_ASSERT_EQUAL_STRING("before", cap.chunks[0]);
   TEST_ASSERT_EQUAL_STRING("after", cap.chunks[1]);
}

static void test_deeply_nested(void) {
   char buf[128];
   filter_to_buf("<command><command>deep</command></command>", buf, sizeof(buf));
   TEST_ASSERT_EQUAL_STRING("", buf);
}

/* ── main ───────────────────────────────────────────────────────────────── */

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_no_tags);
   RUN_TEST(test_simple_tag_removal);
   RUN_TEST(test_tag_at_start);
   RUN_TEST(test_tag_at_end);
   RUN_TEST(test_only_tag_content);
   RUN_TEST(test_nested_tags);
   RUN_TEST(test_multiple_tags);
   RUN_TEST(test_streaming_across_chunks);
   RUN_TEST(test_partial_tag_not_matching);
   RUN_TEST(test_empty_input);
   RUN_TEST(test_buffer_overflow_protection);
   RUN_TEST(test_reset_mid_tag);
   RUN_TEST(test_callback_receives_chunks);
   RUN_TEST(test_deeply_nested);
   return UNITY_END();
}
