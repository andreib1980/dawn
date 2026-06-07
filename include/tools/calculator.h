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
 * Calculator tool for DAWN - math evaluation, unit conversion, base conversion, random numbers.
 */

#ifndef CALCULATOR_H
#define CALCULATOR_H

/**
 * @file calculator.h
 * @brief Calculator tool for DAWN voice assistant.
 *
 * Provides math evaluation, unit conversion, base conversion, and random
 * number generation capabilities accessible via voice commands and LLM integration.
 *
 * @section thread_safety Thread Safety
 * All functions are thread-safe:
 * - Uses pthread_once for one-time initialization
 * - No shared mutable state between calls
 * - Returns heap-allocated strings (caller must free)
 *
 * @section memory Memory Management
 * All functions returning char* return heap-allocated strings.
 * The caller is responsible for freeing the returned memory.
 *
 * @note Random number generation uses standard rand() which is not
 * cryptographically secure. Do not use for security-critical operations.
 */

/**
 * @brief Maximum accepted length of an expression to evaluate.
 *
 * Bounds untrusted (LLM-supplied) input before it reaches the recursive-descent
 * parsers (tinyexpr for 'evaluate', the bignum parser for 'exact'), so a deeply
 * nested expression can't exhaust the (reduced) worker-thread stack.  Generous
 * for any real expression — large exact RESULTS come from compact inputs
 * (e.g. "2^10000", "52!"), not from huge literals.
 */
#define CALC_MAX_EXPR_LEN 1024

/**
 * @brief Result structure for calculator evaluations.
 */
typedef struct {
   double result;   /**< The computed result (valid only if success is true) */
   int success;     /**< 1 if evaluation succeeded, 0 otherwise */
   char error[128]; /**< Error message if success is false */
} calc_result_t;

/**
 * @brief Evaluate a mathematical expression.
 *
 * Supports standard operators (+, -, *, /, ^) and functions:
 * abs, ceil, floor, sqrt, ln, log, exp, sin, cos, tan,
 * asin, acos, atan, sinh, cosh, tanh, pow, fac (factorial)
 *
 * @param expression The expression string to evaluate (e.g., "2 + 3 * sqrt(16)")
 * @return calc_result_t containing result or error information
 */
calc_result_t calculator_evaluate(const char *expression);

/**
 * @brief Evaluate an expression in digit-perfect arbitrary-precision integer mode.
 *
 * Computes the exact integer value of @p expression (supports +, -, *, exact /,
 * ^ with non-negative integer exponent, ! factorial, and parentheses) and
 * returns its full decimal digit string.  When the expression is not an exact
 * integer (irrational functions, non-terminating division, negative exponent),
 * it transparently falls back to the floating-point evaluator.  When the exact
 * result would exceed the digit cap, it returns the float approximation with a
 * note.
 *
 * @param expression The expression string (e.g., "52!", "2^256").
 * @return Heap-allocated result string (caller must free), or NULL on failure.
 */
char *calculator_evaluate_exact_str(const char *expression);

/**
 * @brief Format a calculation result as a string.
 *
 * Returns a heap-allocated string suitable for LLM response.
 * Uses smart formatting: integers display without decimals,
 * floats display with appropriate precision.
 *
 * @param result Pointer to the calculation result
 * @return Heap-allocated string (caller must free), or NULL on allocation failure
 */
char *calculator_format_result(calc_result_t *result);

/**
 * @brief Convert between units.
 *
 * Supports length (m, km, mi, ft, in, etc.), mass (g, kg, lb, oz),
 * volume (l, ml, gal, cup), temperature (c, f, k), and time (s, min, hr, day).
 *
 * @param value_str Format: "5 miles to km" or "100 f to c"
 * @return Heap-allocated string with result (caller must free)
 */
char *calculator_convert(const char *value_str);

/**
 * @brief Convert between number bases.
 *
 * Supports hex (0x prefix), binary (0b prefix), octal (0 prefix), decimal.
 * Target bases: hex, dec, oct, bin.
 *
 * @param value_str Format: "255 to hex" or "0xFF to binary"
 * @return Heap-allocated string with result (caller must free)
 */
char *calculator_base_convert(const char *value_str);

/**
 * @brief Generate a random number.
 *
 * @param value_str Format: "1 to 100" or just "100" (implies 1 to 100)
 * @return Heap-allocated string with result (caller must free)
 */
char *calculator_random(const char *value_str);

#endif /* CALCULATOR_H */
