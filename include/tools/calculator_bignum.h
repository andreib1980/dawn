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
 * Arbitrary-precision integer expression evaluator for the calculator's
 * digit-perfect "exact" mode.  Supports +, -, *, exact /, ^ (non-negative
 * integer exponent), ! (factorial), and parentheses over big integers.  Any
 * expression that leaves the integer domain (irrational functions, a divide
 * with a non-zero remainder, a negative exponent) is reported as INEXACT so the
 * caller can fall back to the fast floating-point path.
 */

#ifndef CALCULATOR_BIGNUM_H
#define CALCULATOR_BIGNUM_H

#include <stddef.h>

/** Outcome of an exact (arbitrary-precision integer) evaluation. */
typedef enum {
   CALC_EXACT_OK = 0,    /**< Exact integer result written to *out_decimal. */
   CALC_EXACT_INEXACT,   /**< Not representable as an exact integer — caller should fall back. */
   CALC_EXACT_TOO_LARGE, /**< Result would exceed the digit cap. */
   CALC_EXACT_ERROR,     /**< Malformed expression. */
} calc_exact_status_t;

/** Maximum number of decimal digits an exact result may have before it is
 *  rejected as CALC_EXACT_TOO_LARGE (bounds memory + the spoken/chat output). */
#define CALC_EXACT_MAX_DIGITS 10000

/**
 * @brief Evaluate @p expr as an exact arbitrary-precision integer expression.
 *
 * On CALC_EXACT_OK, *out_decimal is a heap-allocated decimal string (caller
 * frees).  On any other status, *out_decimal is set to NULL and @p err (if
 * non-NULL) receives a short human-readable reason.
 *
 * @param expr        Expression text (e.g. "52!", "2^256", "(10+3)*4").
 * @param out_decimal Out: allocated decimal string on success, else NULL.
 * @param err         Optional buffer for an error/explanation message.
 * @param err_size    Size of @p err.
 * @return calc_exact_status_t outcome.
 */
calc_exact_status_t calculator_evaluate_exact(const char *expr,
                                              char **out_decimal,
                                              char *err,
                                              size_t err_size);

#endif /* CALCULATOR_BIGNUM_H */
