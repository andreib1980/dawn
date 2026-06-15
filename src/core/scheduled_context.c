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
 * Scheduled-origin context implementation — one thread-local int.  0 means
 * "not in a scheduled scope"; a positive value is the owning user_id.
 */

#include "core/scheduled_context.h"

static __thread int tl_scheduled_user = 0;

void scheduled_context_set(int user_id) {
   tl_scheduled_user = (user_id > 0) ? user_id : 0;
}

void scheduled_context_clear(void) {
   tl_scheduled_user = 0;
}

bool scheduled_context_get(int *user_id_out) {
   if (user_id_out) {
      *user_id_out = tl_scheduled_user;
   }
   return tl_scheduled_user > 0;
}
