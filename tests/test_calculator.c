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
 * Unit tests for calculator tool (evaluate, convert, base_convert, random).
 */

#include <stdlib.h>
#include <string.h>

#include "tools/calculator.h"
#include "unity.h"

void setUp(void) {
}

void tearDown(void) {
}

/* ── Evaluate: basic arithmetic ─────────────────────────────────────────── */

static void test_eval_addition(void) {
   calc_result_t r = calculator_evaluate("2 + 3");
   TEST_ASSERT_EQUAL_INT(1, r.success);
   TEST_ASSERT_DOUBLE_WITHIN(0.0001, 5.0, r.result);
}

static void test_eval_order_of_operations(void) {
   calc_result_t r = calculator_evaluate("2 + 3 * 4");
   TEST_ASSERT_EQUAL_INT(1, r.success);
   TEST_ASSERT_DOUBLE_WITHIN(0.0001, 14.0, r.result);
}

static void test_eval_parentheses(void) {
   calc_result_t r = calculator_evaluate("(2 + 3) * 4");
   TEST_ASSERT_EQUAL_INT(1, r.success);
   TEST_ASSERT_DOUBLE_WITHIN(0.0001, 20.0, r.result);
}

static void test_eval_power(void) {
   calc_result_t r = calculator_evaluate("2^10");
   TEST_ASSERT_EQUAL_INT(1, r.success);
   TEST_ASSERT_DOUBLE_WITHIN(0.0001, 1024.0, r.result);
}

/* ── Evaluate: functions ────────────────────────────────────────────────── */

static void test_eval_sqrt(void) {
   calc_result_t r = calculator_evaluate("sqrt(144)");
   TEST_ASSERT_EQUAL_INT(1, r.success);
   TEST_ASSERT_DOUBLE_WITHIN(0.0001, 12.0, r.result);
}

static void test_eval_nested_functions(void) {
   calc_result_t r = calculator_evaluate("abs(-5) + ceil(3.2)");
   TEST_ASSERT_EQUAL_INT(1, r.success);
   TEST_ASSERT_DOUBLE_WITHIN(0.0001, 9.0, r.result);
}

/* ── Evaluate: error cases ──────────────────────────────────────────────── */

static void test_eval_null(void) {
   calc_result_t r = calculator_evaluate(NULL);
   TEST_ASSERT_EQUAL_INT(0, r.success);
}

static void test_eval_empty(void) {
   calc_result_t r = calculator_evaluate("");
   TEST_ASSERT_EQUAL_INT(0, r.success);
}

static void test_eval_parse_error(void) {
   calc_result_t r = calculator_evaluate("2 +");
   TEST_ASSERT_EQUAL_INT(0, r.success);
}

static void test_eval_division_by_zero(void) {
   calc_result_t r = calculator_evaluate("1/0");
   TEST_ASSERT_EQUAL_INT(0, r.success);
}

static void test_eval_nan(void) {
   calc_result_t r = calculator_evaluate("sqrt(-1)");
   TEST_ASSERT_EQUAL_INT(0, r.success);
}

/* ── Format result ──────────────────────────────────────────────────────── */

static void test_format_integer(void) {
   calc_result_t r = { .result = 42.0, .success = 1 };
   char *s = calculator_format_result(&r);
   TEST_ASSERT_NOT_NULL(s);
   TEST_ASSERT_EQUAL_STRING("42", s);
   free(s);
}

static void test_format_float(void) {
   calc_result_t r = { .result = 3.14159, .success = 1 };
   char *s = calculator_format_result(&r);
   TEST_ASSERT_NOT_NULL(s);
   TEST_ASSERT_NOT_NULL(strstr(s, "3.14159"));
   free(s);
}

static void test_format_error(void) {
   calc_result_t r = { .success = 0 };
   strncpy(r.error, "bad expression", sizeof(r.error));
   char *s = calculator_format_result(&r);
   TEST_ASSERT_NOT_NULL(s);
   TEST_ASSERT_NOT_NULL(strstr(s, "Error:"));
   free(s);
}

/* ── Unit conversion ────────────────────────────────────────────────────── */

static void test_convert_length(void) {
   char *s = calculator_convert("5 miles to km");
   TEST_ASSERT_NOT_NULL(s);
   TEST_ASSERT_NOT_NULL(strstr(s, "8.04672"));
   free(s);
}

static void test_convert_temperature(void) {
   char *s = calculator_convert("32 f to c");
   TEST_ASSERT_NOT_NULL(s);
   /* 32°F = 0°C; output should be "0 c" or close to it */
   TEST_ASSERT_NOT_NULL(strstr(s, "0"));
   free(s);
}

static void test_convert_same_unit(void) {
   char *s = calculator_convert("1 meter to m");
   TEST_ASSERT_NOT_NULL(s);
   TEST_ASSERT_NOT_NULL(strstr(s, "1"));
   free(s);
}

static void test_convert_incompatible(void) {
   char *s = calculator_convert("5 miles to kg");
   TEST_ASSERT_NOT_NULL(s);
   TEST_ASSERT_NOT_NULL(strstr(s, "Cannot convert"));
   free(s);
}

static void test_convert_unknown_unit(void) {
   char *s = calculator_convert("5 flurbs to km");
   TEST_ASSERT_NOT_NULL(s);
   TEST_ASSERT_NOT_NULL(strstr(s, "Error"));
   free(s);
}

/* ── Base conversion ────────────────────────────────────────────────────── */

static void test_base_dec_to_hex(void) {
   char *s = calculator_base_convert("255 to hex");
   TEST_ASSERT_NOT_NULL(s);
   TEST_ASSERT_EQUAL_STRING("0xFF", s);
   free(s);
}

static void test_base_hex_to_dec(void) {
   char *s = calculator_base_convert("0xFF to decimal");
   TEST_ASSERT_NOT_NULL(s);
   TEST_ASSERT_EQUAL_STRING("255", s);
   free(s);
}

static void test_base_dec_to_bin(void) {
   char *s = calculator_base_convert("10 to bin");
   TEST_ASSERT_NOT_NULL(s);
   TEST_ASSERT_NOT_NULL(strstr(s, "1010"));
   free(s);
}

static void test_base_bin_to_dec(void) {
   char *s = calculator_base_convert("0b1010 to decimal");
   TEST_ASSERT_NOT_NULL(s);
   TEST_ASSERT_EQUAL_STRING("10", s);
   free(s);
}

/* ── Random ─────────────────────────────────────────────────────────────── */

static void test_random_range(void) {
   char *s = calculator_random("1 to 100");
   TEST_ASSERT_NOT_NULL(s);
   long val = strtol(s, NULL, 10);
   TEST_ASSERT_TRUE(val >= 1 && val <= 100);
   free(s);
}

static void test_random_single(void) {
   char *s = calculator_random("50");
   TEST_ASSERT_NOT_NULL(s);
   long val = strtol(s, NULL, 10);
   TEST_ASSERT_TRUE(val >= 1 && val <= 50);
   free(s);
}

/* ── Main ───────────────────────────────────────────────────────────────── */

int main(void) {
   UNITY_BEGIN();

   /* evaluate */
   RUN_TEST(test_eval_addition);
   RUN_TEST(test_eval_order_of_operations);
   RUN_TEST(test_eval_parentheses);
   RUN_TEST(test_eval_power);
   RUN_TEST(test_eval_sqrt);
   RUN_TEST(test_eval_nested_functions);
   RUN_TEST(test_eval_null);
   RUN_TEST(test_eval_empty);
   RUN_TEST(test_eval_parse_error);
   RUN_TEST(test_eval_division_by_zero);
   RUN_TEST(test_eval_nan);

   /* format */
   RUN_TEST(test_format_integer);
   RUN_TEST(test_format_float);
   RUN_TEST(test_format_error);

   /* unit conversion */
   RUN_TEST(test_convert_length);
   RUN_TEST(test_convert_temperature);
   RUN_TEST(test_convert_same_unit);
   RUN_TEST(test_convert_incompatible);
   RUN_TEST(test_convert_unknown_unit);

   /* base conversion */
   RUN_TEST(test_base_dec_to_hex);
   RUN_TEST(test_base_hex_to_dec);
   RUN_TEST(test_base_dec_to_bin);
   RUN_TEST(test_base_bin_to_dec);

   /* random */
   RUN_TEST(test_random_range);
   RUN_TEST(test_random_single);

   return UNITY_END();
}
