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
 * WebUI handlers for the phone call-audio settings panel (GET + apply).
 */

#ifndef WEBUI_PHONE_CONFIG_H
#define WEBUI_PHONE_CONFIG_H

#include "webui/webui_internal.h"

struct json_object;

/** @brief GET handler: send the current call-audio config (+ webrtc_available,
 *         bridge_active) as a phone_audio_config_response.  Admin-gated. */
void handle_phone_audio_config_get(ws_connection_t *conn);

/** @brief Apply a set_config `phone` section: overlay onto the current config,
 *         push it to the tool (clamp + live-apply), and echo the result back.
 *         The caller persists via config_write_toml.  Admin-gated by handle_set_config. */
void webui_phone_apply_config(ws_connection_t *conn, struct json_object *phone_section);

#endif /* WEBUI_PHONE_CONFIG_H */
