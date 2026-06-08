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
 * OTA WebSocket transport (Layer 4) — device-reported status handlers + the
 * server→device offer push.  Bridges the satellite WS to the core ota.c engine.
 * See docs/OTA_DESIGN.md §6.
 */

#ifndef WEBUI_OTA_H
#define WEBUI_OTA_H

#include "webui/webui_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Device→server WS control messages (dispatched from webui_message_dispatch). */
void handle_ota_status(ws_connection_t *conn, struct json_object *payload);
void handle_ota_ack(ws_connection_t *conn, struct json_object *payload);
void handle_ota_reject(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Push an update offer to one satellite by uuid (admin-triggered).
 *
 * Looks up the device's tier→platform, mints the offer via ota_begin_push(),
 * and delivers the `ota_offer` over the device's WS session.  Usable from any
 * thread (admin socket or WebUI handler) — delivery goes through the response
 * queue.
 *
 * @return AUTH_DB_SUCCESS on success, AUTH_DB_LOCKED if mid-update,
 *         AUTH_DB_NOT_FOUND if the device is offline, AUTH_DB_FAILURE otherwise
 */
int webui_ota_push(const char *uuid, const char *version, bool allow_downgrade);

/* Admin (WebUI) message handlers — admin-gated inside. */
void handle_ota_push(ws_connection_t *conn, struct json_object *payload);
void handle_ota_list(ws_connection_t *conn);

#ifdef __cplusplus
}
#endif

#endif /* WEBUI_OTA_H */
