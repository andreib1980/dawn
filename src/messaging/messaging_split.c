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
 * Outbound-message splitter — pure function, no DAWN deps.  See
 * include/messaging/messaging_split.h for the contract.
 *
 * Algorithm (one cycle per chunk):
 *   1. If remaining text fits, emit final chunk and finish.
 *   2. Scan acceptance window [pos + 70% of max_chars, pos + max_chars]
 *      for paragraph runs (\n+) and sentence terminators (. ! ? with
 *      optional closing-punct + required whitespace).
 *   3. Pick the latest match in tier order: \n\n > \n > sentence.
 *   4. If no tier matched, reject with err_msg (caller surfaces it to
 *      the user as a single short message).
 *
 * The 70% acceptance floor keeps chunks as full as possible; cuts that
 * leave the chunk less than 70% utilized usually indicate dense
 * content the splitter can't carve cleanly.  Promote this module to
 * src/core/text_break.c when a non-messaging consumer adopts it.
 */
#include "messaging/messaging_split.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dawn_error.h"

/* Acceptance window starts at 70% of max_chars from pos.  Stored as
 * the high water mark (70 of 100) for integer-math computation. */
#define SPLIT_TAIL_FLOOR_NUM 70
#define SPLIT_TAIL_FLOOR_DEN 100

/* Initial parts-array capacity.  Discord briefings empirically split
 * into 2-4 parts; 8 covers the common case with one realloc cycle for
 * Telegram/Slack worst cases up to 16 parts. */
#define SPLIT_PARTS_INITIAL_CAP 8

static int is_closing_punct(char c) {
   return c == '"' || c == ')' || c == ']' || c == '*' || c == '_';
}

static int is_inline_whitespace(char c) {
   return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static void free_parts(char **parts, size_t n) {
   if (!parts) {
      return;
   }
   for (size_t i = 0; i < n; i++) {
      free(parts[i]);
   }
   free(parts);
}

/* Search the window [window_lo, window_hi] for paragraph runs (\n+)
 * and sentence terminators.  On match, set *cut_at / *next_pos and
 * return SUCCESS.  Tier priority: \n\n > \n > sentence.
 *
 * `cut_at` is the exclusive end of the chunk content (chunk =
 * text[pos..cut_at)).  `next_pos` is where the next chunk starts;
 * bytes [cut_at, next_pos) are discarded break characters. */
static int find_best_break(const char *text,
                           size_t text_len,
                           size_t window_lo,
                           size_t window_hi,
                           size_t *cut_at,
                           size_t *next_pos) {
   size_t para2_cut = 0, para2_next = 0;
   int para2_found = 0;
   size_t para1_cut = 0, para1_next = 0;
   int para1_found = 0;
   size_t sent_cut = 0, sent_next = 0;
   int sent_found = 0;

   /* Scan for newline runs.  Forward scan tracks the LATEST run that
    * begins inside the window so the chunk is as full as possible. */
   size_t i = window_lo;
   while (i <= window_hi && i < text_len) {
      if (text[i] == '\n') {
         size_t run_start = i;
         size_t newlines = 0;
         while (i < text_len && text[i] == '\n') {
            i++;
            newlines++;
         }
         /* run_start must be in [window_lo, window_hi] for the chunk
          * (text[pos..run_start)) to fit. */
         if (run_start <= window_hi) {
            if (newlines >= 2) {
               para2_cut = run_start;
               para2_next = i;
               para2_found = 1;
            } else {
               para1_cut = run_start;
               para1_next = i;
               para1_found = 1;
            }
         }
      } else {
         i++;
      }
   }

   /* Scan for sentence terminators.  Pattern: [.!?] then optional
    * closing-punct chain (" ) ] asterisk underscore) then required
    * whitespace. */
   i = window_lo;
   while (i <= window_hi && i < text_len) {
      char c = text[i];
      if (c == '.' || c == '!' || c == '?') {
         size_t j = i + 1;
         while (j < text_len && is_closing_punct(text[j])) {
            j++;
         }
         size_t closing_end = j;
         while (j < text_len && is_inline_whitespace(text[j])) {
            j++;
         }
         size_t ws_end = j;
         /* Real break requires at least one whitespace char following
          * the terminator (and any closing punctuation).  Without
          * trailing whitespace the period is likely a decimal point,
          * abbreviation, or end-of-text mark. */
         if (ws_end > closing_end && closing_end <= window_hi) {
            sent_cut = closing_end;
            sent_next = ws_end;
            sent_found = 1;
         }
         /* Advance past the closing-punct chain so we don't re-scan
          * those positions as fresh terminators. */
         i = closing_end > i ? closing_end : i + 1;
      } else {
         i++;
      }
   }

   if (para2_found) {
      *cut_at = para2_cut;
      *next_pos = para2_next;
      return SUCCESS;
   }
   if (para1_found) {
      *cut_at = para1_cut;
      *next_pos = para1_next;
      return SUCCESS;
   }
   if (sent_found) {
      *cut_at = sent_cut;
      *next_pos = sent_next;
      return SUCCESS;
   }
   return FAILURE;
}

static char *strndup_chunk(const char *src, size_t len) {
   char *dst = (char *)malloc(len + 1);
   if (!dst) {
      return NULL;
   }
   if (len > 0) {
      memcpy(dst, src, len);
   }
   dst[len] = '\0';
   return dst;
}

int messaging_split_at_breaks(const char *text,
                              size_t max_chars,
                              char ***parts_out,
                              size_t *count_out,
                              char *err_msg,
                              size_t err_size) {
   if (!text || !parts_out || !count_out || max_chars == 0) {
      if (err_msg && err_size > 0) {
         snprintf(err_msg, err_size, "messaging_split: invalid arguments");
      }
      if (parts_out) {
         *parts_out = NULL;
      }
      if (count_out) {
         *count_out = 0;
      }
      return FAILURE;
   }

   *parts_out = NULL;
   *count_out = 0;

   size_t text_len = strlen(text);
   size_t parts_cap = SPLIT_PARTS_INITIAL_CAP;
   char **parts = (char **)malloc(parts_cap * sizeof(char *));
   if (!parts) {
      if (err_msg && err_size > 0) {
         snprintf(err_msg, err_size, "messaging_split: out of memory");
      }
      return FAILURE;
   }
   size_t parts_n = 0;

   /* Empty input: emit a single zero-length chunk so callers can rely
    * on "SUCCESS implies at least one chunk." */
   if (text_len == 0) {
      char *chunk = strndup_chunk("", 0);
      if (!chunk) {
         free(parts);
         if (err_msg && err_size > 0) {
            snprintf(err_msg, err_size, "messaging_split: out of memory");
         }
         return FAILURE;
      }
      parts[0] = chunk;
      *parts_out = parts;
      *count_out = 1;
      return SUCCESS;
   }

   size_t pos = 0;
   while (pos < text_len) {
      size_t remaining = text_len - pos;
      size_t chunk_len = 0;
      size_t next_pos = 0;

      if (remaining <= max_chars) {
         chunk_len = remaining;
         next_pos = text_len;
      } else {
         size_t window_lo = pos + (max_chars * SPLIT_TAIL_FLOOR_NUM) / SPLIT_TAIL_FLOOR_DEN;
         size_t window_hi = pos + max_chars;
         /* window_hi is clamped to text_len above by the
          * remaining > max_chars branch (text_len - pos > max_chars
          * implies pos + max_chars < text_len). */
         size_t cut_at = 0;
         if (find_best_break(text, text_len, window_lo, window_hi, &cut_at, &next_pos) != SUCCESS) {
            if (err_msg && err_size > 0) {
               snprintf(err_msg, err_size,
                        "This reply is too long and couldn't be broken into multiple "
                        "messages.  Open the WebUI to see the full response.");
            }
            free_parts(parts, parts_n);
            return FAILURE;
         }
         chunk_len = cut_at - pos;
      }

      /* Grow parts buffer if exhausted. */
      if (parts_n == parts_cap) {
         size_t new_cap = parts_cap * 2;
         char **new_parts = (char **)realloc(parts, new_cap * sizeof(char *));
         if (!new_parts) {
            free_parts(parts, parts_n);
            if (err_msg && err_size > 0) {
               snprintf(err_msg, err_size, "messaging_split: out of memory");
            }
            return FAILURE;
         }
         parts = new_parts;
         parts_cap = new_cap;
      }

      char *chunk = strndup_chunk(text + pos, chunk_len);
      if (!chunk) {
         free_parts(parts, parts_n);
         if (err_msg && err_size > 0) {
            snprintf(err_msg, err_size, "messaging_split: out of memory");
         }
         return FAILURE;
      }
      parts[parts_n++] = chunk;
      pos = next_pos;
   }

   *parts_out = parts;
   *count_out = parts_n;
   return SUCCESS;
}
