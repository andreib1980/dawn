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
 * source_dedup_set_t helpers (Phase B).
 *
 * Tiny self-contained module — no DB, no logging, no config — so unit tests
 * can link the helpers without pulling in the rest of memory_callback's
 * dependency cone.  Declared in memory/memory_callback_internal.h.
 *
 * INVARIANT: see memory_callback_internal.h for the M1 dedup-set invariant.
 * The helpers do not enforce it on their own; it lives at the call sites.
 */

#include "memory/memory_callback_internal.h"

bool source_dedup_seen(const source_dedup_set_t *set, int64_t conv, int64_t s, int64_t e) {
   if (!set)
      return false;
   for (int i = 0; i < set->count; i++) {
      if (set->conv_ids[i] == conv && set->starts[i] == s && set->ends[i] == e)
         return true;
   }
   return false;
}

void source_dedup_add(source_dedup_set_t *set, int64_t conv, int64_t s, int64_t e) {
   if (!set || set->count >= SOURCE_DEDUP_CAP)
      return;
   set->conv_ids[set->count] = conv;
   set->starts[set->count] = s;
   set->ends[set->count] = e;
   set->count++;
}
