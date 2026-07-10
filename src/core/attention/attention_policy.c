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
 * SAGE policy — the P1/P2 insertion seam.  P0 maps the watch's requested notify
 * level onto a delivery mode and enforces a per-hour ALERT budget.  Distinct
 * "mode returned" vs "notify requested" is deliberate: P2 will insert the LLM
 * judge BEFORE this function, and P1 will add quiet-hours / richer demotion
 * INSIDE it, without changing the ingest or gate.  Called only from the tick
 * (main thread) — no locking needed.
 */

#include "core/attention/attention_internal.h"

#define SAGE_ALERT_RING 128 /* MUST be >= the config max for max_alerts_per_hour (100) */

static int s_max_per_hour = SAGE_MAX_ALERTS_PER_HOUR; /* replaced at configure */
static int64_t s_alert_ts[SAGE_ALERT_RING];
static int s_alert_head;
static int s_alert_count;

void attention_policy_configure(int max_alerts_per_hour) {
   s_max_per_hour = (max_alerts_per_hour > 0) ? max_alerts_per_hour : 0;
}

void attention_policy_reset_budget(void) {
   s_alert_head = 0;
   s_alert_count = 0;
}

void attention_policy_note_alert(int64_t now_ms) {
   s_alert_ts[s_alert_head] = now_ms;
   s_alert_head = (s_alert_head + 1) % SAGE_ALERT_RING;
   if (s_alert_count < SAGE_ALERT_RING) {
      s_alert_count++;
   }
}

/* Count ALERTs emitted within the last hour. */
static int alerts_last_hour(int64_t now_ms) {
   const int64_t hour_ms = 60 * 60 * 1000;
   int n = 0;
   for (int i = 0; i < s_alert_count; i++) {
      if (now_ms - s_alert_ts[i] < hour_ms) {
         n++;
      }
   }
   return n;
}

sage_delivery_mode_t attention_policy_decide(const sage_event_t *ev, int64_t now_ms) {
   if (!ev) {
      return SAGE_MODE_DROP;
   }
   switch (ev->notify) {
      case SAGE_NOTIFY_ALERT:
         /* Over budget => demote to AMBIENT (still shown + logged, not spoken). */
         if (s_max_per_hour > 0 && alerts_last_hour(now_ms) >= s_max_per_hour) {
            return SAGE_MODE_AMBIENT;
         }
         return SAGE_MODE_ALERT;
      case SAGE_NOTIFY_AMBIENT:
         return SAGE_MODE_AMBIENT;
      case SAGE_NOTIFY_DIGEST:
      default:
         /* No briefing in P0 — digest is log-only (P1 folds it into briefings). */
         return SAGE_MODE_DIGEST;
   }
}
