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
 * Unit tests for memory embeddings math utilities and hybrid search logic.
 * Tests pure math functions without requiring ONNX model or database.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "memory/memory_embeddings.h"
#include "unity.h"

#define FLOAT_EQ(a, b) (fabsf((a) - (b)) < 1e-5f)

void setUp(void) {
}
void tearDown(void) {
}

/* ============================================================================
 * Test: L2 Norm
 * ============================================================================ */

static void test_l2_norm(void) {
   /* Unit vector */
   float unit[] = { 1.0f, 0.0f, 0.0f };
   float norm = memory_embeddings_l2_norm(unit, 3);
   TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.0f, norm);

   /* Simple 3-4-5 triangle */
   float vec345[] = { 3.0f, 4.0f };
   norm = memory_embeddings_l2_norm(vec345, 2);
   TEST_ASSERT_FLOAT_WITHIN(1e-5f, 5.0f, norm);

   /* Zero vector */
   float zero[] = { 0.0f, 0.0f, 0.0f };
   norm = memory_embeddings_l2_norm(zero, 3);
   TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.0f, norm);

   /* NULL input */
   norm = memory_embeddings_l2_norm(NULL, 3);
   TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.0f, norm);

   /* Zero dims */
   float any[] = { 1.0f };
   norm = memory_embeddings_l2_norm(any, 0);
   TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.0f, norm);
}

/* ============================================================================
 * Test: Cosine Similarity
 * ============================================================================ */

static void test_cosine_similarity(void) {
   /* Identical vectors */
   float a[] = { 1.0f, 2.0f, 3.0f };
   float b[] = { 1.0f, 2.0f, 3.0f };
   float sim = memory_embeddings_cosine(a, b, 3);
   TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.0f, sim);

   /* Orthogonal vectors */
   float x[] = { 1.0f, 0.0f };
   float y[] = { 0.0f, 1.0f };
   sim = memory_embeddings_cosine(x, y, 2);
   TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.0f, sim);

   /* Opposite vectors — clamped to 0.0 */
   float pos[] = { 1.0f, 0.0f };
   float neg[] = { -1.0f, 0.0f };
   sim = memory_embeddings_cosine(pos, neg, 2);
   TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.0f, sim);

   /* Zero vector */
   float zero[] = { 0.0f, 0.0f };
   sim = memory_embeddings_cosine(a, zero, 2);
   TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.0f, sim);
}

/* ============================================================================
 * Test: Cosine with Precomputed Norms
 * ============================================================================ */

static void test_cosine_with_norms(void) {
   float a[] = { 1.0f, 2.0f, 3.0f };
   float b[] = { 4.0f, 5.0f, 6.0f };
   float norm_a = memory_embeddings_l2_norm(a, 3);
   float norm_b = memory_embeddings_l2_norm(b, 3);

   float sim_precomputed = memory_embeddings_cosine_with_norms(a, b, 3, norm_a, norm_b);
   float sim_computed = memory_embeddings_cosine(a, b, 3);
   TEST_ASSERT_FLOAT_WITHIN(1e-5f, sim_computed, sim_precomputed);

   /* Zero norm */
   float sim_zero = memory_embeddings_cosine_with_norms(a, b, 3, 0.0f, norm_b);
   TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.0f, sim_zero);
}

/* ============================================================================
 * Test: Cosine Clamping (negative to 0)
 * ============================================================================ */

static void test_cosine_clamping(void) {
   /* Anti-correlated vectors should clamp to 0 */
   float a[] = { 1.0f, 1.0f, 1.0f };
   float b[] = { -1.0f, -1.0f, -1.0f };
   float sim = memory_embeddings_cosine(a, b, 3);
   TEST_ASSERT_TRUE_MESSAGE(sim >= 0.0f, "negative cosine clamped to >= 0");
   TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.0f, sim);

   /* Partially opposite */
   float c[] = { 1.0f, 0.0f };
   float d[] = { -0.5f, 0.1f };
   sim = memory_embeddings_cosine(c, d, 2);
   TEST_ASSERT_TRUE_MESSAGE(sim >= 0.0f, "partial opposite clamped to >= 0");
}

/* ============================================================================
 * Test: Hybrid Score Merge Logic
 * ============================================================================ */

static void test_hybrid_score_merge(void) {
   float kw_weight = 0.3f;
   float vec_weight = 0.7f;

   /* Fact with 2/3 keyword match tokens and 0.8 cosine */
   float kw_score = 2.0f / 3.0f;
   float cosine = 0.8f;
   float hybrid = kw_weight * kw_score + vec_weight * cosine;
   float expected = 0.3f * (2.0f / 3.0f) + 0.7f * 0.8f;
   TEST_ASSERT_FLOAT_WITHIN(1e-5f, expected, hybrid);

   /* Un-embedded fact (keyword only, no vector penalty) */
   float kw_only = kw_weight * 1.0f;
   TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.3f, kw_only);

   /* Vector-only result (no keyword match) */
   float vec_only = vec_weight * 0.9f;
   TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.63f, vec_only);
}

/* ============================================================================
 * Test: Dimension Mismatch Skip
 * ============================================================================ */

static void test_dimension_validation(void) {
   /* Verify L2 norm handles different dimensions correctly */
   float vec4[] = { 1.0f, 1.0f, 1.0f, 1.0f };
   float norm4 = memory_embeddings_l2_norm(vec4, 4);
   TEST_ASSERT_FLOAT_WITHIN(1e-5f, 2.0f, norm4);

   float norm2 = memory_embeddings_l2_norm(vec4, 2);
   TEST_ASSERT_FLOAT_WITHIN(1e-5f, sqrtf(2.0f), norm2);

   /* Cosine with different dim interpretations */
   float a[] = { 1.0f, 0.0f, 0.0f, 0.0f };
   float b[] = { 0.0f, 1.0f, 0.0f, 0.0f };
   float sim4 = memory_embeddings_cosine(a, b, 4);
   TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.0f, sim4);

   float sim2 = memory_embeddings_cosine(a, b, 2);
   TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.0f, sim2);
}

/* ============================================================================
 * Test: Edge Cases
 * ============================================================================ */

static void test_edge_cases(void) {
   /* Single dimension */
   float a1[] = { 5.0f };
   float b1[] = { 3.0f };
   float sim = memory_embeddings_cosine(a1, b1, 1);
   TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.0f, sim);

   float c1[] = { -3.0f };
   sim = memory_embeddings_cosine(a1, c1, 1);
   TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.0f, sim);

   /* Large dimension */
   float large_a[384];
   float large_b[384];
   for (int i = 0; i < 384; i++) {
      large_a[i] = (float)(i % 10) / 10.0f;
      large_b[i] = (float)(i % 10) / 10.0f;
   }
   sim = memory_embeddings_cosine(large_a, large_b, 384);
   TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.0f, sim);

   /* Slightly different */
   large_b[0] += 0.01f;
   sim = memory_embeddings_cosine(large_a, large_b, 384);
   TEST_ASSERT_TRUE_MESSAGE(sim > 0.99f && sim <= 1.0f, "nearly identical 384D > 0.99");
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_l2_norm);
   RUN_TEST(test_cosine_similarity);
   RUN_TEST(test_cosine_with_norms);
   RUN_TEST(test_cosine_clamping);
   RUN_TEST(test_hybrid_score_merge);
   RUN_TEST(test_dimension_validation);
   RUN_TEST(test_edge_cases);
   return UNITY_END();
}
