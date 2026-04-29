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
 * Unit tests for memory_similarity.c — normalization, FNV-1a hashing,
 * Jaccard similarity, and duplicate detection.
 */

#include <string.h>

#include "memory/memory_similarity.h"
#include "unity.h"

void setUp(void) {
}

void tearDown(void) {
}

/* =============================================================================
 * memory_normalize_text
 * ============================================================================= */

static void test_normalize_basic(void) {
   char out[256];
   int len = 0;
   int rc = memory_normalize_text("The cat is on the mat", out, sizeof(out), &len);
   TEST_ASSERT_EQUAL_INT(0, rc);
   TEST_ASSERT_EQUAL_STRING("cat mat", out);
   TEST_ASSERT_EQUAL_INT(7, len);
}

static void test_normalize_preserves_content_words(void) {
   char out[256];
   int rc = memory_normalize_text("Alice works at Google", out, sizeof(out), NULL);
   TEST_ASSERT_EQUAL_INT(0, rc);
   TEST_ASSERT_EQUAL_STRING("alice google works", out);
}

static void test_normalize_strips_punctuation(void) {
   char out[256];
   int rc = memory_normalize_text("Hello, world! Great day.", out, sizeof(out), NULL);
   TEST_ASSERT_EQUAL_INT(0, rc);
   /* "hello" "world" "great" "day" — no stopwords, sorted alphabetically */
   TEST_ASSERT_EQUAL_STRING("day great hello world", out);
}

static void test_normalize_null_returns_error(void) {
   char out[64];
   TEST_ASSERT_EQUAL_INT(1, memory_normalize_text(NULL, out, sizeof(out), NULL));
}

static void test_normalize_null_output_returns_error(void) {
   TEST_ASSERT_EQUAL_INT(1, memory_normalize_text("hello", NULL, 64, NULL));
}

static void test_normalize_tiny_buffer_returns_error(void) {
   char out[1];
   TEST_ASSERT_EQUAL_INT(1, memory_normalize_text("hello", out, 1, NULL));
}

static void test_normalize_empty_string(void) {
   char out[64];
   int len = -1;
   int rc = memory_normalize_text("", out, sizeof(out), &len);
   TEST_ASSERT_EQUAL_INT(0, rc);
   TEST_ASSERT_EQUAL_STRING("", out);
   TEST_ASSERT_EQUAL_INT(0, len);
}

static void test_normalize_only_stopwords(void) {
   char out[64];
   int len = -1;
   int rc = memory_normalize_text("the a is an and", out, sizeof(out), &len);
   TEST_ASSERT_EQUAL_INT(0, rc);
   TEST_ASSERT_EQUAL_STRING("", out);
   TEST_ASSERT_EQUAL_INT(0, len);
}

static void test_normalize_buffer_truncation(void) {
   char out[6];
   int len = 0;
   int rc = memory_normalize_text("Alice works at Google", out, sizeof(out), &len);
   TEST_ASSERT_EQUAL_INT(0, rc);
   /* "alice google works" should be truncated to fit in 6 bytes (5 chars + NUL) */
   TEST_ASSERT_TRUE(len <= 5);
   TEST_ASSERT_EQUAL_INT((int)strlen(out), len);
}

/* =============================================================================
 * memory_hash_text
 * ============================================================================= */

static void test_hash_deterministic(void) {
   uint32_t h1 = memory_hash_text("cat mat");
   uint32_t h2 = memory_hash_text("cat mat");
   TEST_ASSERT_EQUAL_UINT32(h1, h2);
}

static void test_hash_different_inputs(void) {
   uint32_t h1 = memory_hash_text("cat mat");
   uint32_t h2 = memory_hash_text("dog house");
   TEST_ASSERT_TRUE(h1 != h2);
}

static void test_hash_null_returns_zero(void) {
   TEST_ASSERT_EQUAL_UINT32(0, memory_hash_text(NULL));
}

static void test_hash_empty_string(void) {
   uint32_t h = memory_hash_text("");
   /* FNV-1a of empty string = offset basis 2166136261 */
   TEST_ASSERT_EQUAL_UINT32(2166136261u, h);
}

/* =============================================================================
 * memory_normalize_and_hash
 * ============================================================================= */

static void test_normalize_and_hash_basic(void) {
   uint32_t h = memory_normalize_and_hash("Alice works at Google");
   /* Should match hashing the normalized form */
   uint32_t expected = memory_hash_text("alice google works");
   TEST_ASSERT_EQUAL_UINT32(expected, h);
}

static void test_normalize_and_hash_case_insensitive(void) {
   uint32_t h1 = memory_normalize_and_hash("Alice Works At Google");
   uint32_t h2 = memory_normalize_and_hash("alice works at google");
   TEST_ASSERT_EQUAL_UINT32(h1, h2);
}

static void test_normalize_and_hash_null(void) {
   TEST_ASSERT_EQUAL_UINT32(0, memory_normalize_and_hash(NULL));
}

/* =============================================================================
 * memory_jaccard_similarity
 * ============================================================================= */

static void test_jaccard_identical(void) {
   float sim = memory_jaccard_similarity("Alice works at Google", "Alice works at Google");
   TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, sim);
}

static void test_jaccard_completely_different(void) {
   float sim = memory_jaccard_similarity("Alice works at Google", "Bob lives near Paris");
   TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, sim);
}

static void test_jaccard_partial_overlap(void) {
   float sim = memory_jaccard_similarity("Alice works at Google", "Alice works at Microsoft");
   /* Normalized: {"alice","google","works"} vs {"alice","microsoft","works"} */
   /* intersection=2 (alice,works), union=3 (alice,google,microsoft,works)=4 wait...
    * Actually union = |A|+|B|-|intersection| = 3+3-2 = 4, so 2/4 = 0.5 */
   TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.5f, sim);
}

static void test_jaccard_both_only_stopwords(void) {
   float sim = memory_jaccard_similarity("the a is", "an the and");
   /* Both normalize to empty → treated as identical */
   TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, sim);
}

static void test_jaccard_one_only_stopwords(void) {
   float sim = memory_jaccard_similarity("the a is", "Alice works");
   /* First normalizes to empty, second has words → 0.0 */
   TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, sim);
}

static void test_jaccard_null(void) {
   TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, memory_jaccard_similarity(NULL, "hello"));
   TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, memory_jaccard_similarity("hello", NULL));
   TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, memory_jaccard_similarity(NULL, NULL));
}

/* =============================================================================
 * memory_is_duplicate
 * ============================================================================= */

static void test_is_duplicate_exact_match(void) {
   TEST_ASSERT_TRUE(memory_is_duplicate("Alice works at Google", "Alice works at Google", 0.7f));
}

static void test_is_duplicate_case_variation(void) {
   TEST_ASSERT_TRUE(memory_is_duplicate("Alice works at Google", "alice works at google", 0.7f));
}

static void test_is_duplicate_completely_different(void) {
   TEST_ASSERT_FALSE(memory_is_duplicate("Alice works at Google", "Bob lives near Paris", 0.7f));
}

static void test_is_duplicate_below_threshold(void) {
   /* Jaccard 0.5 with threshold 0.7 → not duplicate */
   TEST_ASSERT_FALSE(
       memory_is_duplicate("Alice works at Google", "Alice works at Microsoft", 0.7f));
}

static void test_is_duplicate_above_threshold(void) {
   /* Jaccard 0.5 with threshold 0.4 → duplicate */
   TEST_ASSERT_TRUE(memory_is_duplicate("Alice works at Google", "Alice works at Microsoft", 0.4f));
}

static void test_is_duplicate_null(void) {
   TEST_ASSERT_FALSE(memory_is_duplicate(NULL, "hello", 0.7f));
   TEST_ASSERT_FALSE(memory_is_duplicate("hello", NULL, 0.7f));
   TEST_ASSERT_FALSE(memory_is_duplicate(NULL, NULL, 0.7f));
}

/* =============================================================================
 * Main
 * ============================================================================= */

int main(void) {
   UNITY_BEGIN();

   /* Normalization */
   RUN_TEST(test_normalize_basic);
   RUN_TEST(test_normalize_preserves_content_words);
   RUN_TEST(test_normalize_strips_punctuation);
   RUN_TEST(test_normalize_null_returns_error);
   RUN_TEST(test_normalize_null_output_returns_error);
   RUN_TEST(test_normalize_tiny_buffer_returns_error);
   RUN_TEST(test_normalize_empty_string);
   RUN_TEST(test_normalize_only_stopwords);
   RUN_TEST(test_normalize_buffer_truncation);

   /* Hashing */
   RUN_TEST(test_hash_deterministic);
   RUN_TEST(test_hash_different_inputs);
   RUN_TEST(test_hash_null_returns_zero);
   RUN_TEST(test_hash_empty_string);

   /* Normalize + hash combo */
   RUN_TEST(test_normalize_and_hash_basic);
   RUN_TEST(test_normalize_and_hash_case_insensitive);
   RUN_TEST(test_normalize_and_hash_null);

   /* Jaccard similarity */
   RUN_TEST(test_jaccard_identical);
   RUN_TEST(test_jaccard_completely_different);
   RUN_TEST(test_jaccard_partial_overlap);
   RUN_TEST(test_jaccard_both_only_stopwords);
   RUN_TEST(test_jaccard_one_only_stopwords);
   RUN_TEST(test_jaccard_null);

   /* Duplicate detection */
   RUN_TEST(test_is_duplicate_exact_match);
   RUN_TEST(test_is_duplicate_case_variation);
   RUN_TEST(test_is_duplicate_completely_different);
   RUN_TEST(test_is_duplicate_below_threshold);
   RUN_TEST(test_is_duplicate_above_threshold);
   RUN_TEST(test_is_duplicate_null);

   return UNITY_END();
}
