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
 * SAGE ingest — samples every available source ONCE per tick into a shared
 * ctx.  Poll-based (avoids the single-consumer on_message dispatch): telemetry
 * lives in the STAT/suit services' live caches, and this reuses their existing
 * copy-out snapshot APIs.  The get_snapshot() CALLS are the only #ifdef-guarded
 * bits — a compiled-out service leaves its snapshot struct zeroed (valid=false),
 * so catalog readers see "not present" and watches on it never fire.
 */

#include <string.h>

#include "core/attention/attention_internal.h"
#include "core/component_status.h"

void attention_ingest_sample(attention_sample_ctx_t *ctx, int64_t now_ms) {
   if (!ctx) {
      return;
   }
   memset(ctx, 0, sizeof(*ctx));
   ctx->now_ms = now_ms;

#ifdef DAWN_ENABLE_STAT_TOOL
   if (stat_service_is_active()) {
      stat_service_get_snapshot(&ctx->stat);
      ctx->stat_valid = true;
   }
#endif

#ifdef DAWN_ENABLE_SUIT_TOOL
   if (suit_service_is_active()) {
      suit_service_get_snapshot(&ctx->suit);
      ctx->suit_valid = true;
   }
#endif

   /* Component status (HUD/MIRAGE) is always compiled. */
   int hud_age = component_status_get_hud_age();
   ctx->hud_ever = (hud_age >= 0);
   ctx->hud_online = component_status_is_hud_online();
   ctx->hud_age_sec = (hud_age >= 0) ? hud_age : 0;
}
