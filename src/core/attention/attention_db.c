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
 * SAGE attention_db — thin wrapper mapping a processed sage_event_t onto the
 * auth_db attention_log accessor.  Keeps the auth layer free of SAGE event
 * internals.
 */

#include "auth/auth_db.h"
#include "auth/auth_db_attention.h"
#include "core/attention/attention_internal.h"

/* Delivery surface label written to attention_log for each mode. */
static const char *mode_surface(sage_delivery_mode_t mode) {
   switch (mode) {
      case SAGE_MODE_ALERT:
         return "alert";
      case SAGE_MODE_AMBIENT:
         return "ambient";
      case SAGE_MODE_DIGEST:
         return "digest";
      case SAGE_MODE_DROP:
      default:
         return NULL; /* not delivered */
   }
}

int attention_db_log_event(const sage_event_t *ev,
                           sage_delivery_mode_t mode,
                           const char *surface,
                           int64_t delivered_at_ms) {
   if (!ev) {
      return AUTH_DB_INVALID;
   }
   const char *surf = surface ? surface : mode_surface(mode);
   /* category = the watch name (per-category precision groups by watch);
    * event_key = the stable dedup key. */
   return auth_db_attention_log_insert(ev->user_id, ev->event_key, ev->gate_rule, ev->source,
                                       ev->summary, ev->gate_rule, ev->privacy, surf,
                                       delivered_at_ms);
}
