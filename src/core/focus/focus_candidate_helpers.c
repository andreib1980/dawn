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
 * Candidate allocation / cleanup helpers — see focus_candidate_helpers.h.
 */

#include "core/focus/focus_candidate_helpers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dawn_error.h"
#include "logging.h"

size_t focus_utf8_safe_cap(const char *text, size_t max_bytes) {
   if (text == NULL)
      return 0;
   const size_t n = strlen(text);
   if (n <= max_bytes)
      return n;
   /* Cutting at byte `max_bytes` would split a character if that byte is a
    * UTF-8 continuation byte (0b10xxxxxx). Back up to the start of that
    * character so the returned prefix never ends mid-sequence — otherwise the
    * truncated text is invalid UTF-8 and breaks a WebSocket text frame. Bounded
    * by the max UTF-8 sequence length, so at most 3 steps. */
   size_t cut = max_bytes;
   while (cut > 0 && ((unsigned char)text[cut] & 0xC0) == 0x80)
      cut--;
   return cut;
}

void focus_candidate_cleanup_on_failure(focus_candidate_t *c) {
   if (c == NULL)
      return;
   free(c->text);
   c->text = NULL;
   free(c->item_id);
   c->item_id = NULL;
}

void focus_adapter_failure_cleanup(focus_candidate_t *array,
                                   int populated,
                                   focus_candidate_t **out_candidates,
                                   int *out_count) {
   if (array != NULL) {
      for (int i = 0; i < populated; i++)
         focus_candidate_cleanup_on_failure(&array[i]);
      free(array);
   }
   if (out_candidates != NULL)
      *out_candidates = NULL;
   if (out_count != NULL)
      *out_count = 0;
}

int focus_candidate_init(focus_candidate_t *c,
                         const char *source_id,
                         focus_source_type_t source_type,
                         const char *text,
                         const char *item_id,
                         time_t item_timestamp,
                         float semantic_score,
                         float recency_score,
                         float importance_score,
                         bool *truncated_warned) {
   if (c == NULL || source_id == NULL || text == NULL || item_id == NULL)
      return FAILURE;

   memset(c, 0, sizeof(*c));
   c->source_id = source_id;
   c->source_type = source_type;
   c->semantic_score = semantic_score;
   c->recency_score = recency_score;
   c->importance_score = importance_score;
   c->item_timestamp = item_timestamp;

   const size_t text_len = strlen(text);
   /* UTF-8-safe truncation: a raw byte cut at FOCUS_TEXT_MAX_BYTES could split a
    * multi-byte character, yielding invalid UTF-8 that later breaks the
    * context_injection WebSocket text frame (browser rejects it, drops the
    * connection). Back the cut up to a character boundary. */
   const size_t copy_len = focus_utf8_safe_cap(text, FOCUS_TEXT_MAX_BYTES);
   c->text = malloc(copy_len + 1);
   if (c->text == NULL) {
      focus_candidate_cleanup_on_failure(c);
      return FAILURE;
   }
   memcpy(c->text, text, copy_len);
   c->text[copy_len] = '\0';

   if (text_len > FOCUS_TEXT_MAX_BYTES && truncated_warned != NULL && !*truncated_warned) {
      *truncated_warned = true;
      OLOG_WARNING("focus_adapter[%s]: candidate text truncated %zu→%d bytes (further truncations "
                   "this call suppressed)",
                   source_id, text_len, FOCUS_TEXT_MAX_BYTES);
   }

   c->item_id = strdup(item_id);
   if (c->item_id == NULL) {
      focus_candidate_cleanup_on_failure(c);
      return FAILURE;
   }

   memset(&c->provenance, 0, sizeof(c->provenance));
   return SUCCESS;
}

int focus_candidate_format_item_id(char *buf, size_t buflen, const char *prefix, int64_t id) {
   const int n = snprintf(buf, buflen, "%s:%lld", prefix, (long long)id);
   if (n < 0 || (size_t)n >= buflen)
      return FAILURE;
   return SUCCESS;
}
