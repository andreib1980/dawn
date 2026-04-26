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

static int passed = 0;
static int failed = 0;

#define ASSERT(cond, msg)                                  \
   do {                                                    \
      if (cond) {                                          \
         printf("  [PASS] %s\n", msg);                     \
         passed++;                                         \
      } else {                                             \
         printf("  [FAIL] %s (line %d)\n", msg, __LINE__); \
         failed++;                                         \
      }                                                    \
   } while (0)

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
   printf("\n--- test_compact_deterministic_basic ---\n");

   struct json_object *arr = json_object_new_array();
   for (int i = 0; i < 10; i++) {
      const char *role = (i % 2 == 0) ? "user" : "assistant";
      json_object_array_add(arr, make_msg(role, "This is a test message for compaction."));
   }

   char *result = llm_context_compact_deterministic(arr, 150);
   ASSERT(result != NULL, "deterministic returns non-NULL");
   ASSERT(strstr(result, "truncated") != NULL, "output contains 'truncated' header");
   ASSERT(strstr(result, "user:") != NULL, "output contains user role");
   ASSERT(strstr(result, "assistant:") != NULL, "output contains assistant role");

   free(result);
   json_object_put(arr);
}

static void test_compact_deterministic_budget(void) {
   printf("\n--- test_compact_deterministic_budget ---\n");

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
   ASSERT(result != NULL, "deterministic with long content returns non-NULL");
   ASSERT((int)strlen(result) < max_bytes, "output stays within budget");

   free(result);
   json_object_put(arr);
}

static void test_compact_deterministic_empty(void) {
   printf("\n--- test_compact_deterministic_empty ---\n");

   struct json_object *arr = json_object_new_array();
   json_object_array_add(arr, make_msg("user", ""));
   json_object_array_add(arr, make_msg("assistant", ""));

   char *result = llm_context_compact_deterministic(arr, 150);
   ASSERT(result != NULL, "empty content messages don't crash");
   ASSERT(strstr(result, "truncated") != NULL, "still has header");

   free(result);
   json_object_put(arr);
}

static void test_compact_deterministic_single(void) {
   printf("\n--- test_compact_deterministic_single ---\n");

   struct json_object *arr = json_object_new_array();
   json_object_array_add(arr, make_msg("user", "Hello, how are you today?"));

   char *result = llm_context_compact_deterministic(arr, 150);
   ASSERT(result != NULL, "single message returns non-NULL");
   ASSERT(strstr(result, "Hello") != NULL, "contains message content");

   free(result);
   json_object_put(arr);
}

static void test_compact_deterministic_many_messages(void) {
   printf("\n--- test_compact_deterministic_many_messages ---\n");

   struct json_object *arr = json_object_new_array();
   for (int i = 0; i < 500; i++) {
      json_object_array_add(arr, make_msg("user", "Short msg."));
   }

   int budget = 150;
   int max_bytes = budget * 4 + 128;
   char *result = llm_context_compact_deterministic(arr, budget);
   ASSERT(result != NULL, "500 messages returns non-NULL");
   ASSERT((int)strlen(result) < max_bytes, "output stays within budget with many messages");

   free(result);
   json_object_put(arr);
}

/* =============================================================================
 * calculate_compaction_target tests
 * ============================================================================= */

static void test_calculate_target_normal(void) {
   printf("\n--- test_calculate_target_normal ---\n");

   int target = llm_context_calculate_compaction_target(8192, 0.80f);
   ASSERT(target > 4000, "8192 * 0.60 > 4000");
   ASSERT(target < 5500, "8192 * 0.60 < 5500");
   ASSERT(target == (int)(8192 * 0.60f), "exact value matches 8192 * 0.60");
}

static void test_calculate_target_low_threshold(void) {
   printf("\n--- test_calculate_target_low_threshold ---\n");

   int target = llm_context_calculate_compaction_target(4096, 0.40f);
   int floor_val = (int)(4096 * 0.30f);
   ASSERT(target == floor_val, "low threshold hits floor at 0.30");
}

static void test_calculate_target_high_threshold(void) {
   printf("\n--- test_calculate_target_high_threshold ---\n");

   int target = llm_context_calculate_compaction_target(128000, 0.85f);
   int expected = (int)(128000 * 0.65f);
   ASSERT(target == expected, "128K context * 0.65 = ~83200");
}

static void test_calculate_target_below_clamp(void) {
   printf("\n--- test_calculate_target_below_clamp ---\n");

   int target = llm_context_calculate_compaction_target(4096, 0.15f);
   int floor_val = (int)(4096 * 0.30f);
   ASSERT(target == floor_val, "threshold < 0.25 clamped, hits floor at 0.30");
}

/* =============================================================================
 * estimate_tokens_range tests
 * ============================================================================= */

static void test_estimate_tokens_range(void) {
   printf("\n--- test_estimate_tokens_range ---\n");

   struct json_object *arr = json_object_new_array();
   json_object_array_add(arr, make_msg("system", "You are a helpful assistant."));
   json_object_array_add(arr, make_msg("user", "Hello there, how are you doing today?"));
   json_object_array_add(arr, make_msg("assistant", "I am doing well, thank you for asking!"));

   int full = llm_context_estimate_tokens_range(arr, 0, 3);
   ASSERT(full > 0, "full range estimate is positive");

   int partial = llm_context_estimate_tokens_range(arr, 1, 3);
   ASSERT(partial > 0, "partial range estimate is positive");
   ASSERT(partial < full, "partial range is less than full");

   int single = llm_context_estimate_tokens_range(arr, 0, 1);
   ASSERT(single > 0, "single message estimate is positive");

   int empty = llm_context_estimate_tokens_range(arr, 2, 2);
   ASSERT(empty == 0, "empty range returns 0");

   json_object_put(arr);
}

/* =============================================================================
 * Level ordering test
 * ============================================================================= */

static void test_level_ordering(void) {
   printf("\n--- test_level_ordering ---\n");

   ASSERT(LLM_COMPACT_NORMAL < LLM_COMPACT_AGGRESSIVE, "NORMAL < AGGRESSIVE");
   ASSERT(LLM_COMPACT_AGGRESSIVE < LLM_COMPACT_DETERMINISTIC, "AGGRESSIVE < DETERMINISTIC");
   ASSERT(LLM_COMPACT_DETERMINISTIC == LLM_COMPACT_MAX_LEVEL, "DETERMINISTIC == MAX_LEVEL");
}

/* =============================================================================
 * Main
 * ============================================================================= */

int main(void) {
   printf("=== LCM Phase 1: Context Compaction Unit Tests ===\n");

   test_compact_deterministic_basic();
   test_compact_deterministic_budget();
   test_compact_deterministic_empty();
   test_compact_deterministic_single();
   test_compact_deterministic_many_messages();

   test_calculate_target_normal();
   test_calculate_target_low_threshold();
   test_calculate_target_high_threshold();
   test_calculate_target_below_clamp();

   test_estimate_tokens_range();

   test_level_ordering();

   printf("\n=== Results: %d passed, %d failed ===\n", passed, failed);
   return failed > 0 ? 1 : 0;
}
