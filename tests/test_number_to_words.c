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
 * Unit tests for number_to_words (TTS number reader).
 */

/**
 * @file test_number_to_words.c
 * @brief Unit tests for the TTS number-to-words reader.
 */

#include <string.h>

#include "tts/number_to_words.h"
#include "unity.h"

void setUp(void) {
}

void tearDown(void) {
}

/* Helper: convert and assert success + exact word string. */
static void assert_words(const char *number, const char *expected) {
   char out[NUM2WORDS_MIN_BUFFER];
   num2words_status_t st = number_to_words(number, out, sizeof(out));
   TEST_ASSERT_EQUAL_INT(NUM2WORDS_OK, st);
   TEST_ASSERT_EQUAL_STRING(expected, out);
}

/* ── Basics ─────────────────────────────────────────────────────────────── */

static void test_zero(void) {
   assert_words("0", "zero");
}

static void test_single_digit(void) {
   assert_words("7", "seven");
}

static void test_teen(void) {
   assert_words("13", "thirteen");
}

static void test_tens(void) {
   assert_words("40", "forty");
}

static void test_two_digit_compound(void) {
   assert_words("52", "fifty two");
}

static void test_hundreds(void) {
   assert_words("658", "six hundred fifty eight");
}

static void test_thousands(void) {
   assert_words("4025", "four thousand twenty five");
}

static void test_million_with_gap(void) {
   /* Interior zero group must be skipped, not voiced. */
   assert_words("1000000", "one million");
}

static void test_full_scale_chain(void) {
   assert_words("1234567", "one million two hundred thirty four thousand five hundred sixty seven");
}

/* ── Sign / decimals / commas ───────────────────────────────────────────── */

static void test_negative(void) {
   assert_words("-7", "negative seven");
}

static void test_negative_zero_is_zero(void) {
   assert_words("-0", "zero");
}

static void test_decimal(void) {
   assert_words("3.14", "three point one four");
}

static void test_zero_point(void) {
   assert_words("0.5", "zero point five");
}

static void test_commas_stripped(void) {
   assert_words("1,234,567",
                "one million two hundred thirty four thousand five hundred sixty seven");
}

/* ── The headline: a 68-digit factorial reads precisely ─────────────────── */

static void test_factorial_52(void) {
   /* 52! — 68 digits, many non-zero groups, so the full reading is long. Assert
    * it succeeds and that the leading group is named "eighty unvigintillion"
    * (the 10^66 group), which is the whole point of the extended scale table. */
   char out[NUM2WORDS_MIN_BUFFER];
   num2words_status_t st = number_to_words(
       "80658175170943878571660636856403766975289505440883277824000000000000", out, sizeof(out));
   TEST_ASSERT_EQUAL_INT(NUM2WORDS_OK, st);
   TEST_ASSERT_EQUAL_INT(0, strncmp(out, "eighty unvigintillion ", 22));
   /* Trailing zero groups must be skipped — it must not end in a bare scale word. */
   TEST_ASSERT_NOT_NULL(strstr(out, "vigintillion"));
}

static void test_trailing_zero_groups_skipped(void) {
   /* 2 followed by 9 zeros = two billion exactly, no trailing words. */
   assert_words("2000000000", "two billion");
}

/* ── Bounds ─────────────────────────────────────────────────────────────── */

static void test_too_large(void) {
   /* 100 digits exceeds the trigintillion (10^93) table → TOO_LARGE. */
   char big[120];
   memset(big, '9', 100);
   big[100] = '\0';
   char out[NUM2WORDS_MIN_BUFFER];
   TEST_ASSERT_EQUAL_INT(NUM2WORDS_TOO_LARGE, number_to_words(big, out, sizeof(out)));
}

static void test_invalid_scientific(void) {
   char out[NUM2WORDS_MIN_BUFFER];
   TEST_ASSERT_EQUAL_INT(NUM2WORDS_INVALID, number_to_words("8e67", out, sizeof(out)));
}

static void test_invalid_garbage(void) {
   char out[NUM2WORDS_MIN_BUFFER];
   TEST_ASSERT_EQUAL_INT(NUM2WORDS_INVALID, number_to_words("12x3", out, sizeof(out)));
}

int main(void) {
   UNITY_BEGIN();

   RUN_TEST(test_zero);
   RUN_TEST(test_single_digit);
   RUN_TEST(test_teen);
   RUN_TEST(test_tens);
   RUN_TEST(test_two_digit_compound);
   RUN_TEST(test_hundreds);
   RUN_TEST(test_thousands);
   RUN_TEST(test_million_with_gap);
   RUN_TEST(test_full_scale_chain);

   RUN_TEST(test_negative);
   RUN_TEST(test_negative_zero_is_zero);
   RUN_TEST(test_decimal);
   RUN_TEST(test_zero_point);
   RUN_TEST(test_commas_stripped);

   RUN_TEST(test_factorial_52);
   RUN_TEST(test_trailing_zero_groups_skipped);

   RUN_TEST(test_too_large);
   RUN_TEST(test_invalid_scientific);
   RUN_TEST(test_invalid_garbage);

   return UNITY_END();
}
