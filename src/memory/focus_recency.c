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
 * Recency-decay implementation — see focus_recency.h.
 */

#include "memory/focus_recency.h"

#include <math.h>

float focus_recency_decay_uniform(time_t item_ts, time_t now) {
   if (item_ts <= 0)
      return 0.0f;
   if (item_ts >= now)
      return 1.0f;
   const double age = (double)(now - item_ts);
   const double half = (double)FOCUS_RECENCY_HALF_LIFE_SECONDS;
   const double decay = pow(0.5, age / half);
   if (decay <= 0.0)
      return 0.0f;
   if (decay >= 1.0)
      return 1.0f;
   return (float)decay;
}
