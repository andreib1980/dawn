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
 * Unit tests for the SSE (Server-Sent Events) parser.
 */

#include <string.h>

#include "llm/sse_parser.h"
#include "unity.h"

static int event_count;

static void counting_callback(const char *event_type, const char *event_data, void *userdata) {
   (void)event_type;
   (void)event_data;
   (void)userdata;
   event_count++;
}

void setUp(void) {
   event_count = 0;
}

void tearDown(void) {
}

void test_simple_event(void) {
   sse_parser_t *parser = sse_parser_create(counting_callback, NULL);
   TEST_ASSERT_NOT_NULL(parser);

   const char *data = "data: Hello world\n\n";
   sse_parser_feed(parser, data, strlen(data));
   TEST_ASSERT_EQUAL_INT(1, event_count);

   sse_parser_free(parser);
}

void test_event_with_type(void) {
   sse_parser_t *parser = sse_parser_create(counting_callback, NULL);
   TEST_ASSERT_NOT_NULL(parser);

   const char *data = "event: message\ndata: Test message\n\n";
   sse_parser_feed(parser, data, strlen(data));
   TEST_ASSERT_EQUAL_INT(1, event_count);

   sse_parser_free(parser);
}

void test_multiline_data(void) {
   sse_parser_t *parser = sse_parser_create(counting_callback, NULL);
   TEST_ASSERT_NOT_NULL(parser);

   const char *data = "data: Line 1\ndata: Line 2\ndata: Line 3\n\n";
   sse_parser_feed(parser, data, strlen(data));
   TEST_ASSERT_EQUAL_INT(1, event_count);

   sse_parser_free(parser);
}

void test_multiple_events_one_chunk(void) {
   sse_parser_t *parser = sse_parser_create(counting_callback, NULL);
   TEST_ASSERT_NOT_NULL(parser);

   const char *data = "data: Event 1\n\ndata: Event 2\n\ndata: Event 3\n\n";
   sse_parser_feed(parser, data, strlen(data));
   TEST_ASSERT_EQUAL_INT(3, event_count);

   sse_parser_free(parser);
}

void test_split_across_chunks(void) {
   sse_parser_t *parser = sse_parser_create(counting_callback, NULL);
   TEST_ASSERT_NOT_NULL(parser);

   const char *part1 = "data: This is ";
   const char *part2 = "a split event\n\n";
   sse_parser_feed(parser, part1, strlen(part1));
   TEST_ASSERT_EQUAL_INT(0, event_count);
   sse_parser_feed(parser, part2, strlen(part2));
   TEST_ASSERT_EQUAL_INT(1, event_count);

   sse_parser_free(parser);
}

void test_comment_ignored(void) {
   sse_parser_t *parser = sse_parser_create(counting_callback, NULL);
   TEST_ASSERT_NOT_NULL(parser);

   const char *data = ": This is a comment\ndata: Real data\n\n";
   sse_parser_feed(parser, data, strlen(data));
   TEST_ASSERT_EQUAL_INT(1, event_count);

   sse_parser_free(parser);
}

void test_openai_json(void) {
   sse_parser_t *parser = sse_parser_create(counting_callback, NULL);
   TEST_ASSERT_NOT_NULL(parser);

   const char *data =
       "data: "
       "{\"id\":\"chatcmpl-123\",\"object\":\"chat.completion.chunk\",\"choices\":[{\"delta\":{"
       "\"content\":\"Hello\"}}]}\n\n";
   sse_parser_feed(parser, data, strlen(data));
   TEST_ASSERT_EQUAL_INT(1, event_count);

   sse_parser_free(parser);
}

void test_openai_done(void) {
   sse_parser_t *parser = sse_parser_create(counting_callback, NULL);
   TEST_ASSERT_NOT_NULL(parser);

   const char *data = "data: [DONE]\n\n";
   sse_parser_feed(parser, data, strlen(data));
   TEST_ASSERT_EQUAL_INT(1, event_count);

   sse_parser_free(parser);
}

void test_claude_json(void) {
   sse_parser_t *parser = sse_parser_create(counting_callback, NULL);
   TEST_ASSERT_NOT_NULL(parser);

   const char *data =
       "data: "
       "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\","
       "\"text\":\"Hello\"}}\n\n";
   sse_parser_feed(parser, data, strlen(data));
   TEST_ASSERT_EQUAL_INT(1, event_count);

   sse_parser_free(parser);
}

void test_all_events_combined(void) {
   sse_parser_t *parser = sse_parser_create(counting_callback, NULL);
   TEST_ASSERT_NOT_NULL(parser);

   /* Feed all test data sequentially — total should be 11 events */
   const char *d1 = "data: Hello world\n\n";
   sse_parser_feed(parser, d1, strlen(d1));

   const char *d2 = "event: message\ndata: Test message\n\n";
   sse_parser_feed(parser, d2, strlen(d2));

   const char *d3 = "data: Line 1\ndata: Line 2\ndata: Line 3\n\n";
   sse_parser_feed(parser, d3, strlen(d3));

   const char *d4 = "data: Event 1\n\ndata: Event 2\n\ndata: Event 3\n\n";
   sse_parser_feed(parser, d4, strlen(d4));

   const char *d5a = "data: This is ";
   const char *d5b = "a split event\n\n";
   sse_parser_feed(parser, d5a, strlen(d5a));
   sse_parser_feed(parser, d5b, strlen(d5b));

   const char *d6 = ": This is a comment\ndata: Real data\n\n";
   sse_parser_feed(parser, d6, strlen(d6));

   const char *d7 =
       "data: "
       "{\"id\":\"chatcmpl-123\",\"object\":\"chat.completion.chunk\",\"choices\":[{\"delta\":{"
       "\"content\":\"Hello\"}}]}\n\n";
   sse_parser_feed(parser, d7, strlen(d7));

   const char *d8 = "data: [DONE]\n\n";
   sse_parser_feed(parser, d8, strlen(d8));

   const char *d9 =
       "data: "
       "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\","
       "\"text\":\"Hello\"}}\n\n";
   sse_parser_feed(parser, d9, strlen(d9));

   TEST_ASSERT_EQUAL_INT(11, event_count);

   sse_parser_free(parser);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_simple_event);
   RUN_TEST(test_event_with_type);
   RUN_TEST(test_multiline_data);
   RUN_TEST(test_multiple_events_one_chunk);
   RUN_TEST(test_split_across_chunks);
   RUN_TEST(test_comment_ignored);
   RUN_TEST(test_openai_json);
   RUN_TEST(test_openai_done);
   RUN_TEST(test_claude_json);
   RUN_TEST(test_all_events_combined);
   return UNITY_END();
}
