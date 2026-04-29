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
 * Unit tests for word_to_number.c (parseNumericalWord + wordToNumber).
 */

#include "unity.h"
#include "word_to_number.h"

/* parseNumericalWord is internal to word_to_number.c (not in the public header).
 * Declare it here so the test can exercise it directly without warnings under
 * -Werror=implicit-function-declaration. */
extern int parseNumericalWord(const char *token);

void setUp(void) {
}

void tearDown(void) {
}

/* =========================================================================
 * parseNumericalWord tests
 * ========================================================================= */

static void test_parse_units(void) {
   TEST_ASSERT_EQUAL_INT(0, parseNumericalWord("zero"));
   TEST_ASSERT_EQUAL_INT(1, parseNumericalWord("one"));
   TEST_ASSERT_EQUAL_INT(5, parseNumericalWord("five"));
   TEST_ASSERT_EQUAL_INT(9, parseNumericalWord("nine"));
}

static void test_parse_teens(void) {
   TEST_ASSERT_EQUAL_INT(10, parseNumericalWord("ten"));
   TEST_ASSERT_EQUAL_INT(13, parseNumericalWord("thirteen"));
   TEST_ASSERT_EQUAL_INT(15, parseNumericalWord("fifteen"));
   TEST_ASSERT_EQUAL_INT(19, parseNumericalWord("nineteen"));
}

static void test_parse_tens(void) {
   TEST_ASSERT_EQUAL_INT(20, parseNumericalWord("twenty"));
   TEST_ASSERT_EQUAL_INT(50, parseNumericalWord("fifty"));
   TEST_ASSERT_EQUAL_INT(90, parseNumericalWord("ninety"));
}

static void test_parse_unrecognized(void) {
   TEST_ASSERT_EQUAL_INT(0, parseNumericalWord("hello"));
   TEST_ASSERT_EQUAL_INT(0, parseNumericalWord("hundred"));
   TEST_ASSERT_EQUAL_INT(0, parseNumericalWord("thousand"));
   TEST_ASSERT_EQUAL_INT(0, parseNumericalWord(""));
}

/* =========================================================================
 * wordToNumber tests — integer results
 * ========================================================================= */

static void test_single_word(void) {
   char input[] = "eighteen";
   TEST_ASSERT_DOUBLE_WITHIN(0.0, 18.0, wordToNumber(input));
}

static void test_zero(void) {
   char input[] = "zero";
   TEST_ASSERT_DOUBLE_WITHIN(0.0, 0.0, wordToNumber(input));
}

static void test_hundreds(void) {
   char input[] = "seven hundred fifty six";
   TEST_ASSERT_DOUBLE_WITHIN(0.0, 756.0, wordToNumber(input));
}

static void test_hundreds_exact(void) {
   char input[] = "six hundred fifty";
   TEST_ASSERT_DOUBLE_WITHIN(0.0, 650.0, wordToNumber(input));
}

static void test_thousands(void) {
   char input[] = "four thousand twenty five";
   TEST_ASSERT_DOUBLE_WITHIN(0.0, 4025.0, wordToNumber(input));
}

static void test_compound_thousands(void) {
   char input[] = "sixty nine thousand three hundred twenty seven";
   TEST_ASSERT_DOUBLE_WITHIN(0.0, 69327.0, wordToNumber(input));
}

static void test_large_thousands(void) {
   char input[] = "five hundred twelve thousand three hundred forty six";
   TEST_ASSERT_DOUBLE_WITHIN(0.0, 512346.0, wordToNumber(input));
}

static void test_million(void) {
   char input[] = "one million eighteen";
   TEST_ASSERT_DOUBLE_WITHIN(0.0, 1000018.0, wordToNumber(input));
}

static void test_million_large(void) {
   char input[] = "five hundred million";
   TEST_ASSERT_DOUBLE_WITHIN(0.0, 500000000.0, wordToNumber(input));
}

static void test_max_below_billion(void) {
   char input[] = "nine hundred ninety nine million nine hundred ninety nine thousand"
                  " nine hundred ninety nine";
   TEST_ASSERT_DOUBLE_WITHIN(0.0, 999999999.0, wordToNumber(input));
}

static void test_billion(void) {
   char input[] = "nine hundred ninety nine billion nine hundred ninety nine million"
                  " nine hundred ninety nine thousand nine hundred ninety nine";
   TEST_ASSERT_DOUBLE_WITHIN(0.0, 999999999999.0, wordToNumber(input));
}

static void test_trillion(void) {
   char input[] = "one trillion two hundred thirty four billion five hundred sixty seven"
                  " million eight hundred ninety thousand one hundred twenty three";
   TEST_ASSERT_DOUBLE_WITHIN(0.0, 1234567890123.0, wordToNumber(input));
}

static void test_four_million_flat(void) {
   char input[] = "four million";
   TEST_ASSERT_DOUBLE_WITHIN(0.0, 4000000.0, wordToNumber(input));
}

/* =========================================================================
 * wordToNumber tests — decimal results
 * ========================================================================= */

static void test_decimal_pi(void) {
   char input[] = "three point one four one five nine";
   TEST_ASSERT_DOUBLE_WITHIN(0.001, 3.14159, wordToNumber(input));
}

static void test_decimal_with_integer_part(void) {
   char input[] = "one hundred twenty three point four five six";
   TEST_ASSERT_DOUBLE_WITHIN(0.001, 123.456, wordToNumber(input));
}

static void test_decimal_zero_prefix(void) {
   char input[] = "zero point one eight nine";
   TEST_ASSERT_DOUBLE_WITHIN(0.001, 0.189, wordToNumber(input));
}

static void test_decimal_leading_zeros(void) {
   char input[] = "zero point zero zero one four five";
   TEST_ASSERT_DOUBLE_WITHIN(0.0001, 0.00145, wordToNumber(input));
}

static void test_decimal_large_integer_part(void) {
   char input[] = "four million five hundred sixty seven point eight nine one";
   TEST_ASSERT_DOUBLE_WITHIN(0.001, 4000567.891, wordToNumber(input));
}

/* =========================================================================
 * Edge cases
 * ========================================================================= */

static void test_hundred_alone(void) {
   char input[] = "hundred";
   TEST_ASSERT_DOUBLE_WITHIN(0.0, 0.0, wordToNumber(input));
}

int main(void) {
   UNITY_BEGIN();

   RUN_TEST(test_parse_units);
   RUN_TEST(test_parse_teens);
   RUN_TEST(test_parse_tens);
   RUN_TEST(test_parse_unrecognized);

   RUN_TEST(test_single_word);
   RUN_TEST(test_zero);
   RUN_TEST(test_hundreds);
   RUN_TEST(test_hundreds_exact);
   RUN_TEST(test_thousands);
   RUN_TEST(test_compound_thousands);
   RUN_TEST(test_large_thousands);
   RUN_TEST(test_million);
   RUN_TEST(test_million_large);
   RUN_TEST(test_max_below_billion);
   RUN_TEST(test_billion);
   RUN_TEST(test_trillion);
   RUN_TEST(test_four_million_flat);

   RUN_TEST(test_decimal_pi);
   RUN_TEST(test_decimal_with_integer_part);
   RUN_TEST(test_decimal_zero_prefix);
   RUN_TEST(test_decimal_leading_zeros);
   RUN_TEST(test_decimal_large_integer_part);

   RUN_TEST(test_hundred_alone);

   return UNITY_END();
}
