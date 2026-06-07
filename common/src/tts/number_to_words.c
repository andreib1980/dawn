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
 * Number-to-words for TTS — see number_to_words.h.  Reverse of
 * src/word_to_number.c: groups the decimal digits in threes and names each
 * group with a short-scale magnitude word.
 */

/**
 * @file number_to_words.c
 * @brief Convert a decimal number string to spoken English words for TTS.
 */

#include "tts/number_to_words.h"

#include <stdbool.h>
#include <string.h>

/* zero..nineteen — covers the units and teens in one table for direct indexing. */
static const char *const ONES[] = { "zero",    "one",     "two",       "three",    "four",
                                    "five",    "six",     "seven",     "eight",    "nine",
                                    "ten",     "eleven",  "twelve",    "thirteen", "fourteen",
                                    "fifteen", "sixteen", "seventeen", "eighteen", "nineteen" };

/* Tens: index by the tens digit (2 -> "twenty" ... 9 -> "ninety"). */
static const char *const TENS[] = { "",      "",      "twenty",  "thirty", "forty",
                                    "fifty", "sixty", "seventy", "eighty", "ninety" };

/* Short-scale group names; index = group position (group g covers 10^(3g)).
 * Through trigintillion (10^93) — comfortably past a 68-digit factorial. */
static const char *const SCALES[] = {
   "",
   "thousand",
   "million",
   "billion",
   "trillion",
   "quadrillion",
   "quintillion",
   "sextillion",
   "septillion",
   "octillion",
   "nonillion",
   "decillion",
   "undecillion",
   "duodecillion",
   "tredecillion",
   "quattuordecillion",
   "quindecillion",
   "sexdecillion",
   "septendecillion",
   "octodecillion",
   "novemdecillion",
   "vigintillion",
   "unvigintillion",
   "duovigintillion",
   "trevigintillion",
   "quattuorvigintillion",
   "quinvigintillion",
   "sexvigintillion",
   "septenvigintillion",
   "octovigintillion",
   "novemvigintillion",
   "trigintillion",
};
#define NUM_SCALES (sizeof(SCALES) / sizeof(SCALES[0]))

/* Bounded word emitter: joins words with single spaces; flags overflow. */
typedef struct {
   char *buf;
   size_t size;
   size_t pos;
   bool ok;
} word_ctx_t;

static void w_emit(word_ctx_t *c, const char *word) {
   if (!c->ok) {
      return;
   }
   size_t wl = strlen(word);
   size_t need = (c->pos ? 1 : 0) + wl;
   if (c->pos + need + 1 > c->size) {
      c->ok = false;
      return;
   }
   if (c->pos) {
      c->buf[c->pos++] = ' ';
   }
   memcpy(c->buf + c->pos, word, wl);
   c->pos += wl;
   c->buf[c->pos] = '\0';
}

/* Emit a 1..999 group as words ("six hundred fifty eight"). */
static void emit_three(word_ctx_t *c, int v) {
   int hundreds = v / 100;
   int rem = v % 100;
   if (hundreds) {
      w_emit(c, ONES[hundreds]);
      w_emit(c, "hundred");
   }
   if (rem) {
      if (rem < 20) {
         w_emit(c, ONES[rem]);
      } else {
         w_emit(c, TENS[rem / 10]);
         if (rem % 10) {
            w_emit(c, ONES[rem % 10]);
         }
      }
   }
}

num2words_status_t number_to_words(const char *number, char *out, size_t out_size) {
   if (!number || !out || out_size == 0) {
      return NUM2WORDS_INVALID;
   }
   out[0] = '\0';

   const char *p = number;
   while (*p == ' ' || *p == '\t') {
      p++;
   }
   bool negative = false;
   if (*p == '-') {
      negative = true;
      p++;
   } else if (*p == '+') {
      p++;
   }

   /* Extract integer digits (dropping thousands commas) into a fixed buffer.
    * NUM_SCALES groups of 3 is the most the scale table can name; more integer
    * digits than that is rejected up front, so no heap allocation is needed. */
   char idig[NUM_SCALES * 3 + 1];
   size_t nd = 0;
   const char *frac = NULL;
   for (; *p; p++) {
      if (*p >= '0' && *p <= '9') {
         if (nd >= NUM_SCALES * 3) {
            return NUM2WORDS_TOO_LARGE; /* more integer digits than any scale name */
         }
         idig[nd++] = *p;
      } else if (*p == ',') {
         continue;
      } else if (*p == '.') {
         frac = p + 1;
         break;
      } else {
         return NUM2WORDS_INVALID; /* unexpected char (e.g. 'e' notation) */
      }
   }
   idig[nd] = '\0';

   /* Validate the fractional part is pure digits. */
   size_t nfrac = 0;
   if (frac) {
      for (const char *f = frac; *f; f++) {
         if (*f < '0' || *f > '9') {
            return NUM2WORDS_INVALID;
         }
         nfrac++;
      }
   }

   /* Strip leading zeros from the integer part. */
   size_t izero = 0;
   while (izero < nd && idig[izero] == '0') {
      izero++;
   }
   size_t isig = nd - izero;          /* significant integer digits */
   const char *digits = idig + izero; /* points at first significant digit */

   bool frac_nonzero = false;
   for (size_t i = 0; i < nfrac; i++) {
      if (frac[i] != '0') {
         frac_nonzero = true;
         break;
      }
   }

   /* Reject magnitudes the scale table can't name. */
   size_t ngroups = (isig + 2) / 3;
   if (ngroups > NUM_SCALES) {
      return NUM2WORDS_TOO_LARGE;
   }

   word_ctx_t ctx = { .buf = out, .size = out_size, .pos = 0, .ok = true };

   if (negative && (isig > 0 || frac_nonzero)) {
      w_emit(&ctx, "negative");
   }

   if (isig == 0) {
      w_emit(&ctx, "zero");
   } else {
      /* Most-significant group first. */
      for (size_t g = ngroups; g-- > 0;) {
         size_t end = isig - 3 * g; /* exclusive index into digits */
         size_t start = (end >= 3) ? end - 3 : 0;
         int val = 0;
         for (size_t k = start; k < end; k++) {
            val = val * 10 + (digits[k] - '0');
         }
         if (val == 0) {
            continue; /* skip empty group (e.g. the zeros in 1,000,000) */
         }
         emit_three(&ctx, val);
         if (g > 0) {
            w_emit(&ctx, SCALES[g]);
         }
      }
   }

   if (nfrac > 0) {
      w_emit(&ctx, "point");
      for (size_t i = 0; i < nfrac; i++) {
         w_emit(&ctx, ONES[frac[i] - '0']);
      }
   }

   return ctx.ok ? NUM2WORDS_OK : NUM2WORDS_INVALID;
}
