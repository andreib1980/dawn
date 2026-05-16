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
 * Unit tests for memory_bm25 sigmoid normalization helpers.
 * Algorithm adapted from Mem0 (Apache-2.0); see NOTICE.
 */

#include <math.h>

#include "memory/memory_bm25.h"
#include "unity.h"

void setUp(void) {
}
void tearDown(void) {
}

/* The sigmoid params table — anchor the documented Mem0 curve. */
void test_params_short_query(void) {
   float mid = 0.0f, steep = 0.0f;
   memory_bm25_get_params(1, &mid, &steep);
   TEST_ASSERT_EQUAL_FLOAT(5.0f, mid);
   TEST_ASSERT_EQUAL_FLOAT(0.7f, steep);

   memory_bm25_get_params(3, &mid, &steep);
   TEST_ASSERT_EQUAL_FLOAT(5.0f, mid);
   TEST_ASSERT_EQUAL_FLOAT(0.7f, steep);
}

void test_params_medium_query(void) {
   float mid = 0.0f, steep = 0.0f;
   memory_bm25_get_params(4, &mid, &steep);
   TEST_ASSERT_EQUAL_FLOAT(7.0f, mid);
   TEST_ASSERT_EQUAL_FLOAT(0.6f, steep);

   memory_bm25_get_params(6, &mid, &steep);
   TEST_ASSERT_EQUAL_FLOAT(7.0f, mid);
   TEST_ASSERT_EQUAL_FLOAT(0.6f, steep);
}

void test_params_long_query(void) {
   float mid = 0.0f, steep = 0.0f;
   memory_bm25_get_params(9, &mid, &steep);
   TEST_ASSERT_EQUAL_FLOAT(9.0f, mid);
   TEST_ASSERT_EQUAL_FLOAT(0.5f, steep);

   memory_bm25_get_params(15, &mid, &steep);
   TEST_ASSERT_EQUAL_FLOAT(10.0f, mid);
   TEST_ASSERT_EQUAL_FLOAT(0.5f, steep);

   memory_bm25_get_params(20, &mid, &steep);
   TEST_ASSERT_EQUAL_FLOAT(12.0f, mid);
   TEST_ASSERT_EQUAL_FLOAT(0.5f, steep);
}

/* Edge case: zero / negative term counts fall to the 1-term row. */
void test_params_zero_and_negative_clamp_to_short(void) {
   float mid = 0.0f, steep = 0.0f;
   memory_bm25_get_params(0, &mid, &steep);
   TEST_ASSERT_EQUAL_FLOAT(5.0f, mid);
   TEST_ASSERT_EQUAL_FLOAT(0.7f, steep);

   memory_bm25_get_params(-5, &mid, &steep);
   TEST_ASSERT_EQUAL_FLOAT(5.0f, mid);
   TEST_ASSERT_EQUAL_FLOAT(0.7f, steep);
}

/* NULL out-pointers are tolerated. */
void test_params_null_pointers_ok(void) {
   memory_bm25_get_params(5, NULL, NULL);
   /* No crash = pass. */
   TEST_PASS();
}

/* Sigmoid at midpoint == 0.5 exactly. */
void test_normalize_at_midpoint_returns_half(void) {
   float s = memory_bm25_normalize(5.0f, 5.0f, 0.7f);
   TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.5f, s);
}

/* Far above midpoint approaches 1.0; far below approaches 0.0. */
void test_normalize_saturation(void) {
   float high = memory_bm25_normalize(100.0f, 5.0f, 0.7f);
   TEST_ASSERT_TRUE(high > 0.99f);
   TEST_ASSERT_TRUE(high <= 1.0f);

   float low = memory_bm25_normalize(-100.0f, 5.0f, 0.7f);
   TEST_ASSERT_TRUE(low < 0.01f);
   TEST_ASSERT_TRUE(low >= 0.0f);
}

/* Monotonic: bigger raw score → bigger normalized output. */
void test_normalize_is_monotonic(void) {
   float a = memory_bm25_normalize(3.0f, 5.0f, 0.7f);
   float b = memory_bm25_normalize(5.0f, 5.0f, 0.7f);
   float c = memory_bm25_normalize(7.0f, 5.0f, 0.7f);
   float d = memory_bm25_normalize(10.0f, 5.0f, 0.7f);
   TEST_ASSERT_TRUE(a < b);
   TEST_ASSERT_TRUE(b < c);
   TEST_ASSERT_TRUE(c < d);
}

/* Non-finite inputs yield 0 (defensive against corrupted bm25() return). */
void test_normalize_rejects_non_finite(void) {
   float n = NAN;
   TEST_ASSERT_EQUAL_FLOAT(0.0f, memory_bm25_normalize(n, 5.0f, 0.7f));
   TEST_ASSERT_EQUAL_FLOAT(0.0f, memory_bm25_normalize(INFINITY, 5.0f, 0.7f));
   TEST_ASSERT_EQUAL_FLOAT(0.0f, memory_bm25_normalize(-INFINITY, 5.0f, 0.7f));
   TEST_ASSERT_EQUAL_FLOAT(0.0f, memory_bm25_normalize(5.0f, n, 0.7f));
   TEST_ASSERT_EQUAL_FLOAT(0.0f, memory_bm25_normalize(5.0f, 5.0f, n));
}

/* Output is always in [0, 1]. */
void test_normalize_output_range(void) {
   float scores[] = { -50.0f, -5.0f, 0.0f, 1.0f, 5.0f, 10.0f, 50.0f };
   for (size_t i = 0; i < sizeof(scores) / sizeof(scores[0]); i++) {
      float s = memory_bm25_normalize(scores[i], 5.0f, 0.7f);
      TEST_ASSERT_TRUE(s >= 0.0f);
      TEST_ASSERT_TRUE(s <= 1.0f);
   }
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_params_short_query);
   RUN_TEST(test_params_medium_query);
   RUN_TEST(test_params_long_query);
   RUN_TEST(test_params_zero_and_negative_clamp_to_short);
   RUN_TEST(test_params_null_pointers_ok);
   RUN_TEST(test_normalize_at_midpoint_returns_half);
   RUN_TEST(test_normalize_saturation);
   RUN_TEST(test_normalize_is_monotonic);
   RUN_TEST(test_normalize_rejects_non_finite);
   RUN_TEST(test_normalize_output_range);
   return UNITY_END();
}
