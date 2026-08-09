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
 * Shared reconnect state for persistent-WS messaging drivers.
 */
#include "core/ws_reconnect.h"

#include <stdio.h>
#include <string.h>

void ws_reconnect_init(ws_reconnect_state_t *state) {
   if (!state) {
      return;
   }
   state->session_id[0] = '\0';
   state->resume_url[0] = '\0';
   state->last_sequence = -1;
   state->backoff_seconds = WS_RECONNECT_BACKOFF_FLOOR;
}

void ws_reconnect_record_success(ws_reconnect_state_t *state,
                                 const char *session_id,
                                 const char *resume_url) {
   if (!state) {
      return;
   }
   state->backoff_seconds = WS_RECONNECT_BACKOFF_FLOOR;
   if (session_id) {
      snprintf(state->session_id, sizeof(state->session_id), "%s", session_id);
   }
   if (resume_url) {
      snprintf(state->resume_url, sizeof(state->resume_url), "%s", resume_url);
   }
}

void ws_reconnect_invalidate_session(ws_reconnect_state_t *state) {
   if (!state) {
      return;
   }
   state->session_id[0] = '\0';
   state->resume_url[0] = '\0';
   state->last_sequence = -1;
}

int ws_reconnect_next_delay_seconds(ws_reconnect_state_t *state) {
   if (!state) {
      return WS_RECONNECT_BACKOFF_FLOOR;
   }
   int d = state->backoff_seconds;
   if (d < WS_RECONNECT_BACKOFF_FLOOR) {
      d = WS_RECONNECT_BACKOFF_FLOOR;
   }
   if (d > WS_RECONNECT_BACKOFF_CEILING) {
      d = WS_RECONNECT_BACKOFF_CEILING;
   }
   int next = d * 2;
   if (next > WS_RECONNECT_BACKOFF_CEILING) {
      next = WS_RECONNECT_BACKOFF_CEILING;
   }
   state->backoff_seconds = next;
   return d;
}

bool ws_reconnect_has_resumable_session(const ws_reconnect_state_t *state) {
   if (!state) {
      return false;
   }
   return state->session_id[0] != '\0' && state->last_sequence >= 0;
}
