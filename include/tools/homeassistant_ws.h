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
 * Home Assistant realtime WebSocket listener — subscribes to HA's own
 * /api/websocket state_changed event stream to keep the entity cache live.
 * This is the READ/EVENT plane; commands stay on the REST path
 * (homeassistant_service.c). When built without DAWN_ENABLE_HA_REALTIME
 * (no libwebsockets), all functions are no-op stubs so every preset links.
 */

#ifndef HOMEASSISTANT_WS_H
#define HOMEASSISTANT_WS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct json_object;

/**
 * @brief Layer-3 -> Layer-4 hooks for a coalesced batch of state_changed events
 *        (weak no-op default in homeassistant_ws.c; strong overrides elsewhere).
 *
 * ha_broadcast_state_changed: push a delta frame to the admin board (strong
 * override in src/webui/webui_homeassistant.c). @param dirty_map is a json
 * object {entity_id -> new_state|null}; the callee reads it, does not free it.
 *
 * ha_observe_state_changed: the SAGE proactive-attention seam (no strong
 * override yet — design-only). Called once per changed entity at flush time.
 * NOTE: this is a coalesced, new-state-oriented signal — events are deduped
 * last-wins per flush window, so intermediate transitions are lost and
 * @p old_state is NOT tracked (always NULL today). A future SAGE adapter that
 * needs transition predicates ("home -> away") must extend the dirty value to
 * carry old_state; don't design against a before/after promise this seam can't
 * currently keep.
 */
void ha_broadcast_state_changed(struct json_object *dirty_map);
void ha_observe_state_changed(const char *entity_id, const char *old_state, const char *new_state);

/**
 * @brief Start the realtime WebSocket listener (idempotent).
 *
 * Spawns a single background thread that connects to HA's /api/websocket,
 * authenticates, subscribes to state_changed, seeds and then maintains the
 * shared entity cache live. Credentials (URL + token) are read from the HA
 * service layer at each connect (survives rotation), so none are passed here.
 *
 * @param insecure_tls For wss:// only — accept a self-signed cert / skip
 *                     hostname verification (self-hosted HA). Ignored for ws://.
 * @return SUCCESS if the listener started (or is a build-disabled no-op),
 *         FAILURE on thread/context creation error.
 */
int homeassistant_ws_start(bool insecure_tls);

/**
 * @brief Stop the listener thread and tear down the WS context (idempotent).
 */
void homeassistant_ws_stop(void);

/**
 * @brief True if the WS listener is authed AND liveness-fresh (pong within the
 *        keepalive window). This is the READ-plane liveness signal — it is
 *        deliberately SEPARATE from homeassistant_is_connected() (the REST
 *        command-plane reachability), which the WS plane must never write.
 */
bool homeassistant_ws_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif /* HOMEASSISTANT_WS_H */
