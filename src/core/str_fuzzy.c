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
 * Lightweight fuzzy name matcher implementation.  Extracted verbatim from
 * the static helpers in src/tools/homeassistant_service.c so a second
 * consumer (Discord channel-name resolution) can share one scorer.
 */

#include "core/str_fuzzy.h"

#include <ctype.h>
#include <string.h>

void str_fuzzy_tolower(char *dst, const char *src, size_t max_len) {
   if (!dst || max_len == 0) {
      return;
   }
   if (!src) {
      dst[0] = '\0';
      return;
   }
   size_t i;
   for (i = 0; i < max_len - 1 && src[i]; i++) {
      dst[i] = (char)tolower((unsigned char)src[i]);
   }
   dst[i] = '\0';
}

int str_fuzzy_score(const char *haystack_lower, const char *needle_lower) {
   if (!haystack_lower || !needle_lower) {
      return 0;
   }

   /* Exact match */
   if (strcmp(haystack_lower, needle_lower) == 0) {
      return STR_FUZZY_SCORE_EXACT;
   }

   /* Contains match */
   if (needle_lower[0] && strstr(haystack_lower, needle_lower)) {
      return STR_FUZZY_SCORE_CONTAINS;
   }

   /* Word-by-word match */
   int score = 0;
   char needle_copy[256];
   strncpy(needle_copy, needle_lower, sizeof(needle_copy) - 1);
   needle_copy[sizeof(needle_copy) - 1] = '\0';

   char *saveptr;
   char *token = strtok_r(needle_copy, " ", &saveptr);
   while (token) {
      if (strstr(haystack_lower, token)) {
         score += STR_FUZZY_SCORE_TOKEN_BONUS;
      }
      token = strtok_r(NULL, " ", &saveptr);
   }

   return score;
}
