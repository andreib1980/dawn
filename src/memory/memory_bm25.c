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
 * BM25 sigmoid normalization helpers.
 *
 * /-----------------------------------------------------------------------\
 * |  Adapted from mem0ai/mem0 (Apache-2.0).  See NOTICE.                  |
 * |  Originals:                                                            |
 * |    mem0/utils/scoring.py::get_bm25_params                              |
 * |    mem0/utils/scoring.py::normalize_bm25                               |
 * |  Changes from upstream:                                                |
 * |    - Reimplemented in C; identical formula + parameter table.          |
 * |    - Input clamping for non-finite scores (defensive — not in original)|
 * |    - Caller responsible for tokenization + stemming (mem0 ports        |
 * |      lemmatize inline via spaCy; DAWN uses Porter2 in memory_stem.c).  |
 * \-----------------------------------------------------------------------/
 */

#include "memory/memory_bm25.h"

#include <math.h>
#include <string.h>

#include "logging.h"
#include "memory/memory_types.h"

/* Query-length tier breakpoints (inclusive upper bound for each tier).
 * Adapted from mem0/utils/scoring.py::get_bm25_params (Apache-2.0).
 * Above BM25_TIER_VLONG_MAX terms, the "very long query" parameters apply. */
#define BM25_TIER_SHORT_MAX 3
#define BM25_TIER_MEDIUM_MAX 6
#define BM25_TIER_LONG_MAX 9
#define BM25_TIER_VLONG_MAX 15

void memory_bm25_get_params(int num_terms, float *midpoint_out, float *steepness_out) {
   /* Adapted from mem0/utils/scoring.py::get_bm25_params (Apache-2.0).
    * Treat any non-positive count as a single-term query — the resolver
    * is only sensitive at the lower end of the curve and there is no
    * meaningful "zero-term" search. */
   float midpoint;
   float steepness;

   if (num_terms <= BM25_TIER_SHORT_MAX) {
      midpoint = 5.0f;
      steepness = 0.7f;
   } else if (num_terms <= BM25_TIER_MEDIUM_MAX) {
      midpoint = 7.0f;
      steepness = 0.6f;
   } else if (num_terms <= BM25_TIER_LONG_MAX) {
      midpoint = 9.0f;
      steepness = 0.5f;
   } else if (num_terms <= BM25_TIER_VLONG_MAX) {
      midpoint = 10.0f;
      steepness = 0.5f;
   } else {
      midpoint = 12.0f;
      steepness = 0.5f;
   }

   if (midpoint_out)
      *midpoint_out = midpoint;
   if (steepness_out)
      *steepness_out = steepness;
}

float memory_bm25_normalize(float raw_score, float midpoint, float steepness) {
   /* Adapted from mem0/utils/scoring.py::normalize_bm25 (Apache-2.0).
    * Defensive: reject non-finite inputs so a corrupted FTS5 bm25()
    * return (NaN / inf) can't poison the downstream composite. */
   if (!isfinite(raw_score) || !isfinite(midpoint) || !isfinite(steepness)) {
      return 0.0f;
   }
   float arg = -steepness * (raw_score - midpoint);
   /* expf saturates to 0/inf cleanly; result stays in [0, 1] for any
    * finite arg.  Clamp explicitly for paranoia. */
   float s = 1.0f / (1.0f + expf(arg));
   if (s < 0.0f)
      s = 0.0f;
   if (s > 1.0f)
      s = 1.0f;
   return s;
}

/* Promoted from memory_db_facts.c (was a file-static) when document search
 * became the second FTS5 consumer.  Builds `"a" OR "b" OR ...` from a
 * space-separated stem string. */
int memory_bm25_build_match_expr(const char *stemmed, char *out, size_t out_sz) {
   if (!out || out_sz == 0)
      return 0;
   out[0] = '\0';
   if (!stemmed || !stemmed[0])
      return 0;
   /* Working copy for strtok_r since we can't mutate the input.  Sized to
    * MEMORY_FACT_STEMS_MAX (matches the upstream memory_stem_string buffer) so
    * long queries don't silently lose trailing tokens. */
   char buf[MEMORY_FACT_STEMS_MAX];
   size_t in_len = strlen(stemmed);
   if (in_len >= sizeof(buf)) {
      OLOG_WARNING("memory_bm25_build_match_expr: stem string len=%zu >= buf=%zu, truncating",
                   in_len, sizeof(buf));
      in_len = sizeof(buf) - 1;
   }
   memcpy(buf, stemmed, in_len);
   buf[in_len] = '\0';

   size_t off = 0;
   int count = 0;
   char *saveptr = NULL;
   char *tok = strtok_r(buf, " ", &saveptr);
   while (tok != NULL) {
      size_t tlen = strlen(tok);
      if (tlen >= 1) {
         /* Need 4 + tlen for ` OR "x"` or 2 + tlen for `"x"` plus NUL. */
         size_t need = (count > 0 ? 4 : 0) + tlen + 2 + 1;
         if (off + need >= out_sz)
            break;
         if (count > 0) {
            memcpy(out + off, " OR ", 4);
            off += 4;
         }
         out[off++] = '"';
         /* Strip embedded double-quotes from tokens — they'd break FTS5 phrase
          * parsing.  Standard tokenization removes these; the defensive copy
          * keeps the path safe against future tokenizer changes. */
         for (size_t i = 0; i < tlen; i++) {
            if (tok[i] != '"') {
               out[off++] = tok[i];
            }
         }
         out[off++] = '"';
         count++;
      }
      tok = strtok_r(NULL, " ", &saveptr);
   }
   out[off] = '\0';
   return count;
}
