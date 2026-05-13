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
 * Unit tests for memory_search_apply_score_floor() — the "I don't remember"
 * gate helper exposed via memory_callback_internal.h.  Pins the in-place
 * compaction contract used by memory_action_search() to drop marginal-cosine
 * hits before they reach the LLM.
 */

#include <stdint.h>
#include <string.h>

#include "memory/memory_fact_search.h"
#include "memory/memory_types.h"
#include "unity.h"

void setUp(void) {
}
void tearDown(void) {
}

static void seed_fact(memory_fact_t *f, int64_t id, const char *text) {
   memset(f, 0, sizeof(*f));
   f->id = id;
   strncpy(f->fact_text, text, sizeof(f->fact_text) - 1);
   f->confidence = 0.9f;
}

/* Floor disabled (<= 0) is a no-op — every hit passes regardless of score. */
void test_floor_zero_is_noop(void) {
   memory_fact_t facts[3];
   seed_fact(&facts[0], 1, "alpha");
   seed_fact(&facts[1], 2, "bravo");
   seed_fact(&facts[2], 3, "charlie");
   float scores[3] = { 0.05f, 0.01f, 0.001f };

   int kept = memory_search_apply_score_floor(facts, scores, 3, 0.0f);
   TEST_ASSERT_EQUAL_INT(3, kept);
   TEST_ASSERT_EQUAL_INT64(1, facts[0].id);
   TEST_ASSERT_EQUAL_INT64(2, facts[1].id);
   TEST_ASSERT_EQUAL_INT64(3, facts[2].id);
}

/* Floor below all scores keeps every fact. */
void test_floor_below_all_keeps_all(void) {
   memory_fact_t facts[2];
   seed_fact(&facts[0], 1, "alpha");
   seed_fact(&facts[1], 2, "bravo");
   float scores[2] = { 0.50f, 0.30f };

   int kept = memory_search_apply_score_floor(facts, scores, 2, 0.10f);
   TEST_ASSERT_EQUAL_INT(2, kept);
}

/* Floor above all scores drops every fact — count returns 0. */
void test_floor_above_all_drops_all(void) {
   memory_fact_t facts[2];
   seed_fact(&facts[0], 1, "alpha");
   seed_fact(&facts[1], 2, "bravo");
   float scores[2] = { 0.05f, 0.08f };

   int kept = memory_search_apply_score_floor(facts, scores, 2, 0.20f);
   TEST_ASSERT_EQUAL_INT(0, kept);
}

/* Mixed: only above-floor entries are preserved in original order. */
void test_floor_preserves_above_floor_order(void) {
   memory_fact_t facts[5];
   seed_fact(&facts[0], 10, "high");
   seed_fact(&facts[1], 20, "weak");
   seed_fact(&facts[2], 30, "mid");
   seed_fact(&facts[3], 40, "trace");
   seed_fact(&facts[4], 50, "strong");
   float scores[5] = { 0.50f, 0.05f, 0.20f, 0.01f, 0.40f };

   int kept = memory_search_apply_score_floor(facts, scores, 5, 0.15f);
   TEST_ASSERT_EQUAL_INT(3, kept);
   /* Survivors retain their pre-filter ordering. */
   TEST_ASSERT_EQUAL_INT64(10, facts[0].id);
   TEST_ASSERT_EQUAL_INT64(30, facts[1].id);
   TEST_ASSERT_EQUAL_INT64(50, facts[2].id);
   TEST_ASSERT_EQUAL_FLOAT(0.50f, scores[0]);
   TEST_ASSERT_EQUAL_FLOAT(0.20f, scores[1]);
   TEST_ASSERT_EQUAL_FLOAT(0.40f, scores[2]);
}

/* Boundary: a score exactly at the floor passes (>= floor). */
void test_floor_inclusive_lower_bound(void) {
   memory_fact_t facts[3];
   seed_fact(&facts[0], 1, "below");
   seed_fact(&facts[1], 2, "exactly");
   seed_fact(&facts[2], 3, "above");
   float scores[3] = { 0.099f, 0.10f, 0.11f };

   int kept = memory_search_apply_score_floor(facts, scores, 3, 0.10f);
   TEST_ASSERT_EQUAL_INT(2, kept);
   TEST_ASSERT_EQUAL_INT64(2, facts[0].id);
   TEST_ASSERT_EQUAL_INT64(3, facts[1].id);
}

/* Count==0 is safe (no array reads). */
void test_floor_empty_input_safe(void) {
   memory_fact_t facts[1];
   float scores[1] = { 0.5f };
   int kept = memory_search_apply_score_floor(facts, scores, 0, 0.10f);
   TEST_ASSERT_EQUAL_INT(0, kept);
}

/* NULL inputs are safe and return the input count unchanged
 * (defensive — the caller is the production code, but the helper must
 * not crash if a future caller passes garbage). */
void test_floor_null_inputs_safe(void) {
   float scores[1] = { 0.5f };
   memory_fact_t facts[1];
   TEST_ASSERT_EQUAL_INT(1, memory_search_apply_score_floor(NULL, scores, 1, 0.10f));
   TEST_ASSERT_EQUAL_INT(1, memory_search_apply_score_floor(facts, NULL, 1, 0.10f));
   TEST_ASSERT_EQUAL_INT(0, memory_search_apply_score_floor(NULL, NULL, 0, 0.10f));
}

/* Keyword-only fallback scores (integer match counts cast to float >= 1.0)
 * pass any sub-1.0 floor untouched — the floor only affects hybrid hits. */
void test_floor_keyword_only_scores_untouched(void) {
   memory_fact_t facts[3];
   seed_fact(&facts[0], 1, "one-token");
   seed_fact(&facts[1], 2, "two-tokens");
   seed_fact(&facts[2], 3, "three-tokens");
   /* Keyword-only output writes (float)kw_score_int — integer >= 1.0. */
   float scores[3] = { 1.0f, 2.0f, 3.0f };

   /* Even the most aggressive plausible floor (0.20) passes all three. */
   int kept = memory_search_apply_score_floor(facts, scores, 3, 0.20f);
   TEST_ASSERT_EQUAL_INT(3, kept);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_floor_zero_is_noop);
   RUN_TEST(test_floor_below_all_keeps_all);
   RUN_TEST(test_floor_above_all_drops_all);
   RUN_TEST(test_floor_preserves_above_floor_order);
   RUN_TEST(test_floor_inclusive_lower_bound);
   RUN_TEST(test_floor_empty_input_safe);
   RUN_TEST(test_floor_null_inputs_safe);
   RUN_TEST(test_floor_keyword_only_scores_untouched);
   return UNITY_END();
}
