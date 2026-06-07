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
 * Number-to-words for TTS: converts an arbitrarily long decimal number string
 * into spoken English words ("80658175170943878571..." -> "eighty
 * unvigintillion six hundred fifty eight ...").  The forward complement of
 * src/word_to_number.c, sharing the same ones/tens/teens/magnitude vocabulary
 * but extended through trigintillion (10^93) so very large results — e.g. a
 * factorial — read precisely instead of digit-by-digit.
 */

#ifndef NUMBER_TO_WORDS_H
#define NUMBER_TO_WORDS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Outcome of a number_to_words() conversion. */
typedef enum {
   NUM2WORDS_OK = 0,    /**< Words written to out. */
   NUM2WORDS_TOO_LARGE, /**< Integer magnitude exceeds the scale-name table. */
   NUM2WORDS_INVALID,   /**< Not a parseable decimal number, or out buffer too small. */
} num2words_status_t;

/** Recommended minimum out-buffer size: any NUM2WORDS_OK result fits in this.
 *  Worst case is the largest nameable integer (NUM_SCALES groups) plus a long
 *  fractional tail read digit-by-digit; 3072 covers it with margin.  (If the
 *  buffer is too small, w_emit stops safely and the call returns INVALID — never
 *  an overflow.) */
#define NUM2WORDS_MIN_BUFFER 3072

/**
 * @brief Convert a decimal number string to spoken English words.
 *
 * Accepts an optional leading sign, digit groups with or without thousands
 * commas, and an optional fractional part after '.': e.g. "1,000", "-42",
 * "3.14", or a 68-digit factorial.  The integer part is read with scale names
 * (thousand, million, … through trigintillion); the fractional part is read
 * digit-by-digit after "point".  Scientific notation ("8e67") is NOT handled
 * here — the caller detects and renders that separately.
 *
 * @param number   The number string.
 * @param out      Destination buffer (recommend >= NUM2WORDS_MIN_BUFFER).
 * @param out_size Size of @p out.
 * @return NUM2WORDS_OK on success; NUM2WORDS_TOO_LARGE if the integer part is
 *         bigger than the scale table can name; NUM2WORDS_INVALID on a malformed
 *         number or insufficient buffer.
 */
num2words_status_t number_to_words(const char *number, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* NUMBER_TO_WORDS_H */
