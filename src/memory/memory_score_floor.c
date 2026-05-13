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
 * "I don't remember" gate — fact-search score floor helper.
 *
 * Tiny self-contained module — no DB, no logging, no config — so unit tests
 * can link the helper without pulling in the rest of memory_callback's
 * dependency cone.  Declared in memory/memory_fact_search.h alongside the
 * pipeline it post-processes.
 */

#include "memory/memory_fact_search.h"

int memory_search_apply_score_floor(memory_fact_t *facts,
                                    float *scores,
                                    int count,
                                    float score_floor) {
   if (facts == NULL || scores == NULL || count <= 0 || score_floor <= 0.0f) {
      return count;
   }
   int kept = 0;
   for (int i = 0; i < count; i++) {
      if (scores[i] >= score_floor) {
         if (kept != i) {
            facts[kept] = facts[i];
            scores[kept] = scores[i];
         }
         kept++;
      }
   }
   return kept;
}
