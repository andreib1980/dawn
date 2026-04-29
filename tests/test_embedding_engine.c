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
 * Unit tests for embedding_engine.c — L2 norm, cosine similarity.
 * Tests only the pure math functions; no model loading needed.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/embedding_engine.h"
#include "unity.h"

void setUp(void) {
}
void tearDown(void) {
}

/* Float comparison tolerance */
#define EPSILON 1e-5f

/* ============================================================================
 * Test: L2 Norm Basics
 * ============================================================================ */

static void test_l2_norm_basic(void) {
   /* Unit vectors */
   float e1[] = { 1.0f, 0.0f, 0.0f };
   TEST_ASSERT_FLOAT_WITHIN(EPSILON, 1.0f, embedding_engine_l2_norm(e1, 3));

   float e2[] = { 0.0f, 1.0f, 0.0f };
   TEST_ASSERT_FLOAT_WITHIN(EPSILON, 1.0f, embedding_engine_l2_norm(e2, 3));

   /* Known value: [3, 4] -> norm = 5 */
   float v34[] = { 3.0f, 4.0f };
   TEST_ASSERT_FLOAT_WITHIN(EPSILON, 5.0f, embedding_engine_l2_norm(v34, 2));

   /* All ones: [1,1,1,1] -> norm = 2 */
   float ones[] = { 1.0f, 1.0f, 1.0f, 1.0f };
   TEST_ASSERT_FLOAT_WITHIN(EPSILON, 2.0f, embedding_engine_l2_norm(ones, 4));
}

/* ============================================================================
 * Test: L2 Norm Edge Cases
 * ============================================================================ */

static void test_l2_norm_edge(void) {
   /* Zero vector */
   float zeros[] = { 0.0f, 0.0f, 0.0f };
   TEST_ASSERT_FLOAT_WITHIN(EPSILON, 0.0f, embedding_engine_l2_norm(zeros, 3));

   /* NULL input */
   TEST_ASSERT_FLOAT_WITHIN(EPSILON, 0.0f, embedding_engine_l2_norm(NULL, 3));

   /* Zero dims */
   float v[] = { 1.0f, 2.0f };
   TEST_ASSERT_FLOAT_WITHIN(EPSILON, 0.0f, embedding_engine_l2_norm(v, 0));

   /* Negative dims */
   TEST_ASSERT_FLOAT_WITHIN(EPSILON, 0.0f, embedding_engine_l2_norm(v, -1));

   /* Single element */
   float single[] = { 7.0f };
   TEST_ASSERT_FLOAT_WITHIN(EPSILON, 7.0f, embedding_engine_l2_norm(single, 1));
}

/* ============================================================================
 * Test: Cosine Similarity — Identical Vectors
 * ============================================================================ */

static void test_cosine_identical(void) {
   float a[] = { 1.0f, 2.0f, 3.0f, 4.0f };
   float sim = embedding_engine_cosine(a, a, 4);
   TEST_ASSERT_FLOAT_WITHIN(EPSILON, 1.0f, sim);
}

/* ============================================================================
 * Test: Cosine Similarity — Orthogonal Vectors
 * ============================================================================ */

static void test_cosine_orthogonal(void) {
   float a[] = { 1.0f, 0.0f, 0.0f };
   float b[] = { 0.0f, 1.0f, 0.0f };
   float sim = embedding_engine_cosine(a, b, 3);
   TEST_ASSERT_FLOAT_WITHIN(EPSILON, 0.0f, sim);
}

/* ============================================================================
 * Test: Cosine Similarity — Opposite Vectors (clamped to 0)
 * ============================================================================ */

static void test_cosine_opposite(void) {
   float a[] = { 1.0f, 0.0f };
   float b[] = { -1.0f, 0.0f };
   float sim = embedding_engine_cosine(a, b, 2);
   /* Implementation clamps negative to 0 */
   TEST_ASSERT_FLOAT_WITHIN(EPSILON, 0.0f, sim);
}

/* ============================================================================
 * Test: Cosine with Pre-computed Norms
 * ============================================================================ */

static void test_cosine_with_norms(void) {
   float a[] = { 3.0f, 4.0f };
   float b[] = { 4.0f, 3.0f };
   float norm_a = embedding_engine_l2_norm(a, 2); /* 5.0 */
   float norm_b = embedding_engine_l2_norm(b, 2); /* 5.0 */

   float sim = embedding_engine_cosine_with_norms(a, b, 2, norm_a, norm_b);
   /* dot = 12+12 = 24, cos = 24/25 = 0.96 */
   TEST_ASSERT_FLOAT_WITHIN(EPSILON, 0.96f, sim);
}

/* ============================================================================
 * Test: Cosine Edge Cases
 * ============================================================================ */

static void test_cosine_edge(void) {
   float a[] = { 1.0f, 2.0f };
   float zeros[] = { 0.0f, 0.0f };

   /* Zero vector -> zero norm -> should return 0 (no division by zero) */
   float sim = embedding_engine_cosine(a, zeros, 2);
   TEST_ASSERT_FLOAT_WITHIN(EPSILON, 0.0f, sim);

   sim = embedding_engine_cosine_with_norms(a, zeros, 2, 2.236f, 0.0f);
   TEST_ASSERT_FLOAT_WITHIN(EPSILON, 0.0f, sim);

   /* NULL inputs */
   sim = embedding_engine_cosine(NULL, a, 2);
   TEST_ASSERT_FLOAT_WITHIN(EPSILON, 0.0f, sim);

   sim = embedding_engine_cosine(a, NULL, 2);
   TEST_ASSERT_FLOAT_WITHIN(EPSILON, 0.0f, sim);

   /* dims = 0 */
   sim = embedding_engine_cosine(a, a, 0);
   TEST_ASSERT_FLOAT_WITHIN(EPSILON, 0.0f, sim);
}

/* ============================================================================
 * Test: Large Vector (exercises NEON path on ARM)
 * ============================================================================ */

static void test_large_vector(void) {
   /* 384-dim vector (typical embedding size) */
   const int dims = 384;
   float *a = malloc((size_t)dims * sizeof(float));
   float *b = malloc((size_t)dims * sizeof(float));

   if (!a || !b) {
      free(a);
      free(b);
      TEST_FAIL_MESSAGE("malloc failed");
      return;
   }

   /* a = [1, 0, 0, ...], b = [1, 0, 0, ...] -> cosine = 1.0 */
   memset(a, 0, (size_t)dims * sizeof(float));
   memset(b, 0, (size_t)dims * sizeof(float));
   a[0] = 1.0f;
   b[0] = 1.0f;
   TEST_ASSERT_FLOAT_WITHIN_MESSAGE(EPSILON, 1.0f, embedding_engine_cosine(a, b, dims),
                                    "384-dim identical unit vectors = 1.0");

   /* Fill with same values -> cosine = 1.0 */
   for (int i = 0; i < dims; i++) {
      a[i] = (float)(i + 1) * 0.01f;
      b[i] = (float)(i + 1) * 0.01f;
   }
   TEST_ASSERT_FLOAT_WITHIN_MESSAGE(EPSILON, 1.0f, embedding_engine_cosine(a, b, dims),
                                    "384-dim identical vectors = 1.0");

   /* Slightly different -> close to 1.0 */
   b[0] += 0.001f;
   float sim = embedding_engine_cosine(a, b, dims);
   TEST_ASSERT_TRUE_MESSAGE(sim > 0.99f && sim < 1.001f, "384-dim nearly identical > 0.99");

   /* L2 norm of 384-dim vector */
   float norm = embedding_engine_l2_norm(a, dims);
   TEST_ASSERT_TRUE_MESSAGE(norm > 0.0f, "384-dim norm is positive");

   free(a);
   free(b);
}

/* ============================================================================
 * Test: Non-initialized State
 * ============================================================================ */

static void test_not_initialized(void) {
   /* Without init, these should return safe defaults */
   TEST_ASSERT_FALSE_MESSAGE(embedding_engine_available(), "not available without init");
   TEST_ASSERT_EQUAL_INT_MESSAGE(0, embedding_engine_dims(), "dims=0 without init");
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_l2_norm_basic);
   RUN_TEST(test_l2_norm_edge);
   RUN_TEST(test_cosine_identical);
   RUN_TEST(test_cosine_orthogonal);
   RUN_TEST(test_cosine_opposite);
   RUN_TEST(test_cosine_with_norms);
   RUN_TEST(test_cosine_edge);
   RUN_TEST(test_large_vector);
   RUN_TEST(test_not_initialized);
   return UNITY_END();
}
