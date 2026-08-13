/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * By contributing to this project, you agree to license your contributions
 * under the GPLv3 (or any later version) or any future licenses chosen by
 * the project author(s).
 *
 * Regression tests for suppressing streamed reasoning when thinking is disabled.
 */

#include <string.h>

#include "llm/llm_streaming.h"
#include "unity.h"

void test_set_thinking_mode(const char *mode);
void test_reset_usage(void);
int test_get_usage_calls(void);
uint32_t test_get_usage_session_id(void);
int test_get_usage_prompt_tokens(void);
int test_get_usage_completion_tokens(void);
int test_get_usage_cached_tokens(void);

static char response_text[256];
static char thinking_text[256];
static int thinking_chunks;

static void append_text(char *destination, size_t capacity, const char *text) {
   size_t used = strlen(destination);
   if (used < capacity - 1) {
      strncat(destination, text, capacity - used - 1);
   }
}

static void response_callback(const char *text, void *userdata) {
   (void)userdata;
   append_text(response_text, sizeof(response_text), text);
}

static void chunk_callback(llm_chunk_type_t type, const char *text, void *userdata) {
   (void)userdata;
   if (type == LLM_CHUNK_THINKING) {
      thinking_chunks++;
      append_text(thinking_text, sizeof(thinking_text), text);
   }
}

static llm_stream_context_t *create_stream(const char *mode) {
   test_set_thinking_mode(mode);
   return llm_stream_create_extended(LLM_LOCAL, (cloud_provider_t)0, response_callback,
                                     chunk_callback, NULL);
}

void setUp(void) {
   response_text[0] = '\0';
   thinking_text[0] = '\0';
   thinking_chunks = 0;
   test_reset_usage();
}

void tearDown(void) {
}

static void test_disabled_mode_discards_structured_reasoning(void) {
   llm_stream_context_t *stream = create_stream("disabled");
   TEST_ASSERT_NOT_NULL(stream);

   llm_stream_handle_event(stream, "{\"choices\":[{\"delta\":{\"reasoning_content\":\"secret\","
                                   "\"content\":\"answer\"}}]}");

   TEST_ASSERT_EQUAL_STRING("answer", response_text);
   TEST_ASSERT_EQUAL_STRING("", thinking_text);
   TEST_ASSERT_EQUAL_INT(0, thinking_chunks);
   TEST_ASSERT_FALSE(llm_stream_has_thinking(stream));
   TEST_ASSERT_NULL(llm_stream_get_thinking_ref(stream));

   llm_stream_free(stream);
}

static void test_disabled_mode_discards_split_inline_thinking(void) {
   llm_stream_context_t *stream = create_stream("disabled");
   TEST_ASSERT_NOT_NULL(stream);

   llm_stream_handle_event(stream, "{\"choices\":[{\"delta\":{\"content\":\"<thi\"}}]}");
   llm_stream_handle_event(stream,
                           "{\"choices\":[{\"delta\":{\"content\":\"nk>secret</think>answer\"}}]}");

   TEST_ASSERT_EQUAL_STRING("answer", response_text);
   TEST_ASSERT_EQUAL_STRING("", thinking_text);
   TEST_ASSERT_EQUAL_INT(0, thinking_chunks);
   TEST_ASSERT_FALSE(llm_stream_has_thinking(stream));

   llm_stream_free(stream);
}

static void test_disabled_mode_discards_openrouter_reasoning_formats(void) {
   llm_stream_context_t *stream = create_stream("disabled");
   TEST_ASSERT_NOT_NULL(stream);

   llm_stream_handle_event(stream,
                           "{\"choices\":[{\"delta\":{\"reasoning_details\":["
                           "{\"type\":\"reasoning.text\",\"text\":\"secret text\"},"
                           "{\"type\":\"reasoning.summary\",\"summary\":\"secret summary\"}],"
                           "\"content\":\"first\"}}]}");

   llm_stream_handle_event(stream, "{\"choices\":[{\"delta\":{\"reasoning\":\"flat secret\","
                                   "\"content\":\" second\"}}]}");

   TEST_ASSERT_EQUAL_STRING("first second", response_text);
   TEST_ASSERT_EQUAL_STRING("", thinking_text);
   TEST_ASSERT_EQUAL_INT(0, thinking_chunks);
   TEST_ASSERT_FALSE(llm_stream_has_thinking(stream));

   llm_stream_free(stream);
}

static void test_openai_usage_is_forwarded_to_context_accounting(void) {
   llm_stream_context_t *stream = create_stream("disabled");
   TEST_ASSERT_NOT_NULL(stream);

   llm_stream_handle_event(stream, "{\"choices\":[],\"usage\":{\"prompt_tokens\":321,"
                                   "\"completion_tokens\":45,\"prompt_tokens_details\":"
                                   "{\"cached_tokens\":123}}}");

   TEST_ASSERT_EQUAL_INT(1, test_get_usage_calls());
   TEST_ASSERT_EQUAL_UINT32(0, test_get_usage_session_id());
   TEST_ASSERT_EQUAL_INT(321, test_get_usage_prompt_tokens());
   TEST_ASSERT_EQUAL_INT(45, test_get_usage_completion_tokens());
   TEST_ASSERT_EQUAL_INT(123, test_get_usage_cached_tokens());

   llm_stream_free(stream);
}

static void test_enabled_mode_preserves_structured_reasoning(void) {
   llm_stream_context_t *stream = create_stream("enabled");
   TEST_ASSERT_NOT_NULL(stream);

   llm_stream_handle_event(stream, "{\"choices\":[{\"delta\":{\"reasoning_content\":\"visible\","
                                   "\"content\":\"answer\"}}]}");

   TEST_ASSERT_EQUAL_STRING("answer", response_text);
   TEST_ASSERT_EQUAL_STRING("visible", thinking_text);
   TEST_ASSERT_EQUAL_INT(1, thinking_chunks);
   TEST_ASSERT_TRUE(llm_stream_has_thinking(stream));

   llm_stream_free(stream);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_disabled_mode_discards_structured_reasoning);
   RUN_TEST(test_disabled_mode_discards_split_inline_thinking);
   RUN_TEST(test_disabled_mode_discards_openrouter_reasoning_formats);
   RUN_TEST(test_openai_usage_is_forwarded_to_context_accounting);
   RUN_TEST(test_enabled_mode_preserves_structured_reasoning);
   return UNITY_END();
}
