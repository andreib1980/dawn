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
 * Unit tests for src/core/focus/focus_dominant_token.c — the Phase B-ii
 * dominant-token over-inclusion heuristic.  Each test constructs a
 * working pool, calls the public penalty function, and asserts on the
 * mutated semantic_score values.  No DB, no embedding engine, no LLM.
 *
 * Coverage mirrors the design contract in include/core/focus/focus_dominant_token.h:
 *   - no-op cases (disabled, empty query, single-candidate pool)
 *   - dominance detection at the threshold boundary
 *   - full penalty when only dominant tokens match
 *   - diluted penalty when non-dominant tokens also match
 *   - regression guard: candidates with no shared query tokens untouched
 *   - clamp to non-negative semantic_score
 *   - FOCUS_SCORE_NA (negative sentinel) protection
 *   - stopword filtering (no penalty on stopword-only overlap)
 */

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "core/focus/focus_dominant_token.h"
#include "core/focus/focus_source.h"
#include "unity.h"

void setUp(void) {
}
void tearDown(void) {
}

/* =============================================================================
 * Helper: build a working pool from inline arrays.  Each entry's
 * semantic_score / item_id / text are wired up; recency / importance /
 * timestamp left at 0 — the heuristic only touches semantic_score.
 * ============================================================================= */

#define POOL_MAX 16

typedef struct {
   const char *text;
   float semantic;
   const char *item_id;
} test_seed_t;

static focus_candidate_t s_pool[POOL_MAX];

static int build_pool(const test_seed_t *seeds, int n) {
   TEST_ASSERT_LESS_OR_EQUAL_INT(POOL_MAX, n);
   memset(s_pool, 0, sizeof(s_pool));
   for (int i = 0; i < n; i++) {
      s_pool[i].source_id = "memory_fact";
      s_pool[i].source_type = FOCUS_SOURCE_INTERNAL;
      s_pool[i].semantic_score = seeds[i].semantic;
      s_pool[i].recency_score = 0.5f;
      s_pool[i].importance_score = 0.5f;
      s_pool[i].item_timestamp = 0;
      s_pool[i].text = strdup(seeds[i].text);
      s_pool[i].item_id = strdup(seeds[i].item_id);
   }
   return n;
}

static void free_pool(int n) {
   for (int i = 0; i < n; i++) {
      free(s_pool[i].text);
      free(s_pool[i].item_id);
   }
}

/* =============================================================================
 * No-op contract tests
 * ============================================================================= */

static void test_disabled_is_noop(void) {
   const test_seed_t seeds[] = {
      { "Restaurant favorite is Italian", 0.90f, "a" },
      { "Restaurant favorite varies seasonally", 0.90f, "b" },
      { "Special dining at Le Coucou", 0.50f, "c" },
   };
   const int n = build_pool(seeds, 3);
   focus_apply_dominant_token_penalty(false, 0.60f, 0.40f, "favorite restaurant?", s_pool, n);
   for (int i = 0; i < n; i++) {
      TEST_ASSERT_EQUAL_FLOAT(seeds[i].semantic, s_pool[i].semantic_score);
   }
   free_pool(n);
}

static void test_null_query_is_noop(void) {
   const test_seed_t seeds[] = {
      { "Restaurant favorite Italian dish", 0.90f, "a" },
      { "Restaurant favorite varies", 0.90f, "b" },
   };
   const int n = build_pool(seeds, 2);
   focus_apply_dominant_token_penalty(true, 0.60f, 0.40f, NULL, s_pool, n);
   for (int i = 0; i < n; i++) {
      TEST_ASSERT_EQUAL_FLOAT(seeds[i].semantic, s_pool[i].semantic_score);
   }
   free_pool(n);
}

static void test_empty_query_is_noop(void) {
   const test_seed_t seeds[] = {
      { "Restaurant favorite Italian", 0.90f, "a" },
      { "Restaurant favorite varies", 0.90f, "b" },
   };
   const int n = build_pool(seeds, 2);
   focus_apply_dominant_token_penalty(true, 0.60f, 0.40f, "", s_pool, n);
   for (int i = 0; i < n; i++) {
      TEST_ASSERT_EQUAL_FLOAT(seeds[i].semantic, s_pool[i].semantic_score);
   }
   free_pool(n);
}

static void test_single_candidate_pool_is_noop(void) {
   /* No dominance signal computable from a pool of 1. */
   const test_seed_t seeds[] = {
      { "Restaurant favorite Italian", 0.90f, "a" },
   };
   const int n = build_pool(seeds, 1);
   focus_apply_dominant_token_penalty(true, 0.60f, 0.40f, "favorite restaurant?", s_pool, n);
   TEST_ASSERT_EQUAL_FLOAT(seeds[0].semantic, s_pool[0].semantic_score);
   free_pool(n);
}

static void test_query_with_only_stopwords_is_noop(void) {
   const test_seed_t seeds[] = {
      { "Restaurant favorite Italian dish", 0.90f, "a" },
      { "Restaurant favorite varies seasonally", 0.90f, "b" },
      { "Special dining at venue", 0.50f, "c" },
   };
   const int n = build_pool(seeds, 3);
   /* "What is the?" tokenizes to {is} after MIN_TOKEN_LEN=4 filter +
    * "what"/"that"/"the" stopword filter — empty query tokens. */
   focus_apply_dominant_token_penalty(true, 0.60f, 0.40f, "What is the?", s_pool, n);
   for (int i = 0; i < n; i++) {
      TEST_ASSERT_EQUAL_FLOAT(seeds[i].semantic, s_pool[i].semantic_score);
   }
   free_pool(n);
}

/* =============================================================================
 * Dominance detection
 * ============================================================================= */

static void test_no_dominant_tokens_is_noop(void) {
   /* Each candidate shares at most 1/3 of the pool — below 0.60 threshold. */
   const test_seed_t seeds[] = {
      { "Restaurant menu has tomato salad", 0.80f, "a" },
      { "Doctor visited the kitchen yesterday", 0.80f, "b" },
      { "Mountain biking trail review posted", 0.80f, "c" },
   };
   const int n = build_pool(seeds, 3);
   focus_apply_dominant_token_penalty(true, 0.60f, 0.40f, "restaurant doctor mountain", s_pool, n);
   for (int i = 0; i < n; i++) {
      TEST_ASSERT_EQUAL_FLOAT(seeds[i].semantic, s_pool[i].semantic_score);
   }
   free_pool(n);
}

static void test_full_penalty_only_dominant_match(void) {
   /* 4/5 candidates contain "favorite" + "restaurant" — both dominant
    * at threshold 0.60.  Right answer (candidate index 4) contains
    * neither, so no penalty.  The four decoys should each get full
    * base_penalty=0.40 since only dominant tokens match. */
   const test_seed_t seeds[] = {
      { "Favorite restaurant cuisine is Italian", 0.90f, "decoy1" },
      { "Favorite restaurant atmosphere preference is intimate", 0.90f, "decoy2" },
      { "Favorite restaurant cuisine genres rotate seasonally", 0.90f, "decoy3" },
      { "Favorite restaurant atmosphere preference is intimate setting", 0.90f, "decoy4" },
      { "Tried Le Coucou for dinner; duck confit standout", 0.50f, "right" },
   };
   const int n = build_pool(seeds, 5);
   focus_apply_dominant_token_penalty(true, 0.60f, 0.40f, "favorite restaurant city?", s_pool, n);
   /* Four decoys at 0.90 - 0.40 = 0.50. */
   TEST_ASSERT_EQUAL_FLOAT(0.50f, s_pool[0].semantic_score);
   TEST_ASSERT_EQUAL_FLOAT(0.50f, s_pool[1].semantic_score);
   TEST_ASSERT_EQUAL_FLOAT(0.50f, s_pool[2].semantic_score);
   TEST_ASSERT_EQUAL_FLOAT(0.50f, s_pool[3].semantic_score);
   /* Right answer untouched at 0.50. */
   TEST_ASSERT_EQUAL_FLOAT(0.50f, s_pool[4].semantic_score);
   free_pool(n);
}

static void test_diluted_penalty_when_non_dominant_matches(void) {
   /* Candidate matches BOTH the dominant token ("favorite") AND a
    * non-dominant token ("Italian", appearing in only one candidate).
    * Penalty should dilute to base_penalty * 1/2 = 0.20. */
   const test_seed_t seeds[] = {
      { "Favorite restaurant cuisine is Italian preference", 0.90f, "shared_dom_plus_italian" },
      { "Favorite restaurant atmosphere preference", 0.90f, "dom_only_1" },
      { "Favorite restaurant seasonal variety", 0.90f, "dom_only_2" },
      { "Favorite restaurant proximity matters", 0.90f, "dom_only_3" },
      { "Tried Le Coucou for dinner; duck confit standout", 0.50f, "right" },
   };
   const int n = build_pool(seeds, 5);
   /* Query has BOTH dominant ("favorite", "restaurant") and a non-
    * dominant token ("Italian", in only 1 candidate). */
   focus_apply_dominant_token_penalty(true, 0.60f, 0.40f, "favorite restaurant Italian", s_pool, n);
   /* Candidate 0 matches 2 dominant (favorite, restaurant) + 1 non-dominant
    * (italian) = 3 shared total.  Penalty = 0.40 * 2/3 = 0.2667. */
   TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.90f - 0.2667f, s_pool[0].semantic_score);
   /* Candidates 1-3 match only dominants → full penalty. */
   TEST_ASSERT_EQUAL_FLOAT(0.50f, s_pool[1].semantic_score);
   TEST_ASSERT_EQUAL_FLOAT(0.50f, s_pool[2].semantic_score);
   TEST_ASSERT_EQUAL_FLOAT(0.50f, s_pool[3].semantic_score);
   /* Right answer matches no query tokens → no penalty. */
   TEST_ASSERT_EQUAL_FLOAT(0.50f, s_pool[4].semantic_score);
   free_pool(n);
}

/* =============================================================================
 * Edge cases
 * ============================================================================= */

static void test_clamp_to_zero_when_penalty_exceeds_semantic(void) {
   /* Candidate with low semantic (0.20) + only dominant match.  Penalty
    * 0.40 would push semantic to -0.20; clamp to 0.0. */
   const test_seed_t seeds[] = {
      { "Restaurant favorite Italian", 0.20f, "low_sem_decoy" },
      { "Restaurant favorite varies seasonally", 0.20f, "low_sem_decoy2" },
      { "Restaurant favorite atmosphere preference", 0.20f, "low_sem_decoy3" },
      { "Mountain biking trail review", 0.50f, "right" },
   };
   const int n = build_pool(seeds, 4);
   focus_apply_dominant_token_penalty(true, 0.60f, 0.40f, "favorite restaurant city?", s_pool, n);
   TEST_ASSERT_EQUAL_FLOAT(0.0f, s_pool[0].semantic_score);
   TEST_ASSERT_EQUAL_FLOAT(0.0f, s_pool[1].semantic_score);
   TEST_ASSERT_EQUAL_FLOAT(0.0f, s_pool[2].semantic_score);
   TEST_ASSERT_EQUAL_FLOAT(0.50f, s_pool[3].semantic_score);
   free_pool(n);
}

static void test_focus_score_na_sentinel_protected(void) {
   /* Candidate with FOCUS_SCORE_NA (negative sentinel) semantic_score
    * must NOT have penalty subtracted — would push further negative
    * and break the score-aggregator's "NA means not applicable" semantics. */
   const test_seed_t seeds[] = {
      { "Restaurant favorite Italian", 0.90f, "decoy1" },
      { "Restaurant favorite seasonal", 0.90f, "decoy2" },
      { "Restaurant favorite proximity", 0.90f, "decoy3" },
   };
   const int n = build_pool(seeds, 3);
   /* Override one candidate's semantic_score to the NA sentinel
    * (FOCUS_SCORE_NA is documented as negative; -1.0 is the canonical
    * sentinel value used elsewhere in the codebase). */
   s_pool[0].semantic_score = FOCUS_SCORE_NA;
   focus_apply_dominant_token_penalty(true, 0.60f, 0.40f, "favorite restaurant", s_pool, n);
   /* NA sentinel untouched. */
   TEST_ASSERT_EQUAL_FLOAT(FOCUS_SCORE_NA, s_pool[0].semantic_score);
   /* Others get full penalty. */
   TEST_ASSERT_EQUAL_FLOAT(0.50f, s_pool[1].semantic_score);
   TEST_ASSERT_EQUAL_FLOAT(0.50f, s_pool[2].semantic_score);
   free_pool(n);
}

static void test_threshold_boundary_strict_greater_than(void) {
   /* 3 of 5 candidates contain "favorite" — that's 60% exactly.  Per
    * the strict-greater-than-threshold rule (matching the Python
    * prototype), this should NOT be flagged as dominant.  Penalty
    * should be zero across all candidates. */
   const test_seed_t seeds[] = {
      { "Favorite color is teal", 0.90f, "a" },
      { "Favorite cuisine is Thai", 0.90f, "b" },
      { "Favorite chess opening is Sicilian", 0.90f, "c" },
      { "Volunteer schedule changes weekly", 0.90f, "d" },
      { "Special dining at Le Coucou", 0.50f, "e" },
   };
   const int n = build_pool(seeds, 5);
   focus_apply_dominant_token_penalty(true, 0.60f, 0.40f, "favorite hobby preferences", s_pool, n);
   for (int i = 0; i < n; i++) {
      TEST_ASSERT_EQUAL_FLOAT(seeds[i].semantic, s_pool[i].semantic_score);
   }
   free_pool(n);
}

static void test_invalid_params_are_noop(void) {
   /* threshold ≤ 0 OR threshold > 1 OR base_penalty ≤ 0 → no-op
    * (defensive contract — wraps malformed config values). */
   const test_seed_t seeds[] = {
      { "Restaurant favorite Italian", 0.90f, "a" },
      { "Restaurant favorite varies", 0.90f, "b" },
      { "Special dining at Le Coucou", 0.50f, "c" },
   };

   int n = build_pool(seeds, 3);
   focus_apply_dominant_token_penalty(true, 0.0f, 0.40f, "favorite restaurant", s_pool, n);
   for (int i = 0; i < n; i++) {
      TEST_ASSERT_EQUAL_FLOAT(seeds[i].semantic, s_pool[i].semantic_score);
   }
   free_pool(n);

   n = build_pool(seeds, 3);
   focus_apply_dominant_token_penalty(true, 1.5f, 0.40f, "favorite restaurant", s_pool, n);
   for (int i = 0; i < n; i++) {
      TEST_ASSERT_EQUAL_FLOAT(seeds[i].semantic, s_pool[i].semantic_score);
   }
   free_pool(n);

   n = build_pool(seeds, 3);
   focus_apply_dominant_token_penalty(true, 0.60f, 0.0f, "favorite restaurant", s_pool, n);
   for (int i = 0; i < n; i++) {
      TEST_ASSERT_EQUAL_FLOAT(seeds[i].semantic, s_pool[i].semantic_score);
   }
   free_pool(n);
}

/* =============================================================================
 * Phase A integration smoke — the exact pathology shape from
 * benchmarks/focus_probe_cases.json case 16.
 * ============================================================================= */

static void test_phase_a_severe_pathology_penalties_correct(void) {
   /* Reproduces the penalty distribution from focus_probe_cases.json
    * case 16 (severe_dominant_token_restaurant).  Unit-level scope:
    * verify the heuristic's mechanical contract — the right answer
    * (which shares no query tokens) is untouched; the five decoys
    * (which share only the dominant tokens "favorite" + "restaurant")
    * each receive the full base_penalty.
    *
    * The full rank-flip is a COMPOSITE-level property — the right
    * answer wins via recency (0.95) + importance + source contribution
    * even though its semantic (0.50) sits between the penalized decoy
    * semantic values (0.53-0.55).  End-to-end ranking is validated by
    * benchmarks/bench_focus_fix_rate.py (18/18 across providers); this
    * test stays at the unit boundary. */
   const test_seed_t seeds[] = {
      { "Tried Le Coucou for the anniversary dinner; duck confit standout dish", 0.50f, "right" },
      { "Favorite restaurant cuisine genre is Italian preferences", 0.93f, "decoy1" },
      { "Considers neighborhood proximity favorite restaurant factor", 0.93f, "decoy2" },
      { "Favorite restaurant atmosphere preference dim lighting", 0.94f, "decoy3" },
      { "Frequently rotates favorite restaurant seasonal selections", 0.94f, "decoy4" },
      { "Favorite restaurant tends to be small bistro over chains", 0.95f, "decoy5" },
   };
   const int n = build_pool(seeds, 6);
   focus_apply_dominant_token_penalty(true, 0.60f, 0.40f,
                                      "What's my favorite restaurant in the city?", s_pool, n);
   /* Right answer (no query tokens shared) untouched. */
   TEST_ASSERT_EQUAL_FLOAT(0.50f, s_pool[0].semantic_score);
   /* All 5 decoys penalized by 0.40 (only dominant tokens match). */
   for (int i = 1; i < n; i++) {
      TEST_ASSERT_FLOAT_WITHIN(0.001f, seeds[i].semantic - 0.40f, s_pool[i].semantic_score);
   }
   free_pool(n);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_disabled_is_noop);
   RUN_TEST(test_null_query_is_noop);
   RUN_TEST(test_empty_query_is_noop);
   RUN_TEST(test_single_candidate_pool_is_noop);
   RUN_TEST(test_query_with_only_stopwords_is_noop);
   RUN_TEST(test_no_dominant_tokens_is_noop);
   RUN_TEST(test_full_penalty_only_dominant_match);
   RUN_TEST(test_diluted_penalty_when_non_dominant_matches);
   RUN_TEST(test_clamp_to_zero_when_penalty_exceeds_semantic);
   RUN_TEST(test_focus_score_na_sentinel_protected);
   RUN_TEST(test_threshold_boundary_strict_greater_than);
   RUN_TEST(test_invalid_params_are_noop);
   RUN_TEST(test_phase_a_severe_pathology_penalties_correct);
   return UNITY_END();
}
