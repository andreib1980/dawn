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
 * Unit tests for LCM Phase 1 — compaction escalation helpers.
 * Tests compact_deterministic, calculate_compaction_target, and
 * estimate_tokens_range via DAWN_TESTING wrappers.
 */

#define DAWN_TESTING

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "llm/llm_context.h"
#include "unity.h"

void setUp(void) {
}
void tearDown(void) {
}

/* =============================================================================
 * Helper: build a simple conversation message
 * ============================================================================= */

static struct json_object *make_msg(const char *role, const char *content) {
   struct json_object *msg = json_object_new_object();
   json_object_object_add(msg, "role", json_object_new_string(role));
   json_object_object_add(msg, "content", json_object_new_string(content));
   return msg;
}

/* =============================================================================
 * compact_deterministic tests
 * ============================================================================= */

static void test_compact_deterministic_basic(void) {
   struct json_object *arr = json_object_new_array();
   for (int i = 0; i < 10; i++) {
      const char *role = (i % 2 == 0) ? "user" : "assistant";
      json_object_array_add(arr, make_msg(role, "This is a test message for compaction."));
   }

   char *result = llm_context_compact_deterministic(arr, 150);
   TEST_ASSERT_NOT_NULL_MESSAGE(result, "deterministic returns non-NULL");
   TEST_ASSERT_NOT_NULL_MESSAGE(strstr(result, "truncated"), "output contains 'truncated' header");
   TEST_ASSERT_NOT_NULL_MESSAGE(strstr(result, "user:"), "output contains user role");
   TEST_ASSERT_NOT_NULL_MESSAGE(strstr(result, "assistant:"), "output contains assistant role");

   free(result);
   json_object_put(arr);
}

static void test_compact_deterministic_budget(void) {
   struct json_object *arr = json_object_new_array();
   char long_content[2000];
   memset(long_content, 'A', sizeof(long_content) - 1);
   long_content[sizeof(long_content) - 1] = '\0';

   for (int i = 0; i < 20; i++) {
      json_object_array_add(arr, make_msg("user", long_content));
   }

   int budget = 150;
   int max_bytes = budget * 4 + 128;
   char *result = llm_context_compact_deterministic(arr, budget);
   TEST_ASSERT_NOT_NULL_MESSAGE(result, "deterministic with long content returns non-NULL");
   TEST_ASSERT_TRUE_MESSAGE((int)strlen(result) < max_bytes, "output stays within budget");

   free(result);
   json_object_put(arr);
}

static void test_compact_deterministic_empty(void) {
   struct json_object *arr = json_object_new_array();
   json_object_array_add(arr, make_msg("user", ""));
   json_object_array_add(arr, make_msg("assistant", ""));

   char *result = llm_context_compact_deterministic(arr, 150);
   TEST_ASSERT_NOT_NULL_MESSAGE(result, "empty content messages don't crash");
   TEST_ASSERT_NOT_NULL_MESSAGE(strstr(result, "truncated"), "still has header");

   free(result);
   json_object_put(arr);
}

static void test_compact_deterministic_single(void) {
   struct json_object *arr = json_object_new_array();
   json_object_array_add(arr, make_msg("user", "Hello, how are you today?"));

   char *result = llm_context_compact_deterministic(arr, 150);
   TEST_ASSERT_NOT_NULL_MESSAGE(result, "single message returns non-NULL");
   TEST_ASSERT_NOT_NULL_MESSAGE(strstr(result, "Hello"), "contains message content");

   free(result);
   json_object_put(arr);
}

static void test_compact_deterministic_many_messages(void) {
   struct json_object *arr = json_object_new_array();
   for (int i = 0; i < 500; i++) {
      json_object_array_add(arr, make_msg("user", "Short msg."));
   }

   int budget = 150;
   int max_bytes = budget * 4 + 128;
   char *result = llm_context_compact_deterministic(arr, budget);
   TEST_ASSERT_NOT_NULL_MESSAGE(result, "500 messages returns non-NULL");
   TEST_ASSERT_TRUE_MESSAGE((int)strlen(result) < max_bytes,
                            "output stays within budget with many messages");

   free(result);
   json_object_put(arr);
}

/* =============================================================================
 * calculate_compaction_target tests
 * ============================================================================= */

static void test_calculate_target_normal(void) {
   int target = llm_context_calculate_compaction_target(8192, 0.80f);
   TEST_ASSERT_TRUE_MESSAGE(target > 4000, "8192 * 0.60 > 4000");
   TEST_ASSERT_TRUE_MESSAGE(target < 5500, "8192 * 0.60 < 5500");
   TEST_ASSERT_EQUAL_INT_MESSAGE((int)(8192 * 0.60f), target, "exact value matches 8192 * 0.60");
}

static void test_calculate_target_low_threshold(void) {
   int target = llm_context_calculate_compaction_target(4096, 0.40f);
   int floor_val = (int)(4096 * 0.30f);
   TEST_ASSERT_EQUAL_INT_MESSAGE(floor_val, target, "low threshold hits floor at 0.30");
}

static void test_calculate_target_high_threshold(void) {
   int target = llm_context_calculate_compaction_target(128000, 0.85f);
   int expected = (int)(128000 * 0.65f);
   TEST_ASSERT_EQUAL_INT_MESSAGE(expected, target, "128K context * 0.65 = ~83200");
}

static void test_calculate_target_below_clamp(void) {
   int target = llm_context_calculate_compaction_target(4096, 0.15f);
   int floor_val = (int)(4096 * 0.30f);
   TEST_ASSERT_EQUAL_INT_MESSAGE(floor_val, target, "threshold < 0.25 clamped, hits floor at 0.30");
}

/* =============================================================================
 * estimate_tokens_range tests
 * ============================================================================= */

static void test_estimate_tokens_range(void) {
   struct json_object *arr = json_object_new_array();
   json_object_array_add(arr, make_msg("system", "You are a helpful assistant."));
   json_object_array_add(arr, make_msg("user", "Hello there, how are you doing today?"));
   json_object_array_add(arr, make_msg("assistant", "I am doing well, thank you for asking!"));

   int full = llm_context_estimate_tokens_range(arr, 0, 3);
   TEST_ASSERT_TRUE_MESSAGE(full > 0, "full range estimate is positive");

   int partial = llm_context_estimate_tokens_range(arr, 1, 3);
   TEST_ASSERT_TRUE_MESSAGE(partial > 0, "partial range estimate is positive");
   TEST_ASSERT_TRUE_MESSAGE(partial < full, "partial range is less than full");

   int single = llm_context_estimate_tokens_range(arr, 0, 1);
   TEST_ASSERT_TRUE_MESSAGE(single > 0, "single message estimate is positive");

   int empty = llm_context_estimate_tokens_range(arr, 2, 2);
   TEST_ASSERT_EQUAL_INT_MESSAGE(0, empty, "empty range returns 0");

   json_object_put(arr);
}

/* =============================================================================
 * Level ordering test
 * ============================================================================= */

static void test_level_ordering(void) {
   TEST_ASSERT_TRUE_MESSAGE(LLM_COMPACT_NORMAL < LLM_COMPACT_AGGRESSIVE, "NORMAL < AGGRESSIVE");
   TEST_ASSERT_TRUE_MESSAGE(LLM_COMPACT_AGGRESSIVE < LLM_COMPACT_DETERMINISTIC,
                            "AGGRESSIVE < DETERMINISTIC");
   TEST_ASSERT_EQUAL_INT_MESSAGE(LLM_COMPACT_MAX_LEVEL, LLM_COMPACT_DETERMINISTIC,
                                 "DETERMINISTIC == MAX_LEVEL");
}

/* =============================================================================
 * Main
 * ============================================================================= */

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_compact_deterministic_basic);
   RUN_TEST(test_compact_deterministic_budget);
   RUN_TEST(test_compact_deterministic_empty);
   RUN_TEST(test_compact_deterministic_single);
   RUN_TEST(test_compact_deterministic_many_messages);
   RUN_TEST(test_calculate_target_normal);
   RUN_TEST(test_calculate_target_low_threshold);
   RUN_TEST(test_calculate_target_high_threshold);
   RUN_TEST(test_calculate_target_below_clamp);
   RUN_TEST(test_estimate_tokens_range);
   RUN_TEST(test_level_ordering);
   return UNITY_END();
}
