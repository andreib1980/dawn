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

void memory_bm25_get_params(int num_terms, float *midpoint_out, float *steepness_out) {
   /* Adapted from mem0/utils/scoring.py::get_bm25_params (Apache-2.0).
    * Treat any non-positive count as a single-term query — the resolver
    * is only sensitive at the lower end of the curve and there is no
    * meaningful "zero-term" search. */
   float midpoint;
   float steepness;

   if (num_terms <= 3) {
      midpoint = 5.0f;
      steepness = 0.7f;
   } else if (num_terms <= 6) {
      midpoint = 7.0f;
      steepness = 0.6f;
   } else if (num_terms <= 9) {
      midpoint = 9.0f;
      steepness = 0.5f;
   } else if (num_terms <= 15) {
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
