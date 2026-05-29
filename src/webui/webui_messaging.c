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
 * WebUI messaging-channels handlers — user-scoped channel management
 * (list / create-link-code / unlink / rename) for the Settings panel.
 *
 * Transport is WebSocket RPC (mirrors the satellite admin handlers), but
 * these are USER-scoped: conn_require_auth + conn->auth_user_id, so a
 * user manages only their own channels.  Operator / cross-user channel
 * management lives on the dawn-admin unix socket instead.  All four wrap
 * the shared messaging_engine_* functions (the single source of truth);
 * mutations key on the stable row id, not the mutable display_name.
 * See docs/MESSAGING_CHANNELS_DESIGN.md §13 Phase 6.
 */

#include <json-c/json.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "logging.h"
#include "messaging/messaging_engine.h"
#include "webui/webui_internal.h"

/* =============================================================================
 * list_channels — { } → { channels: [ {id,name,provider,enabled,last_used_at} ] }
 * ============================================================================= */

void handle_list_channels(ws_connection_t *conn) {
   if (!conn_require_auth(conn)) {
      return;
   }

   char *json = messaging_engine_list_channels_json(conn->auth_user_id);
   /* Parse the engine's JSON array string into an object we can embed.
    * On NULL / parse failure, fall back to an empty array so the panel
    * always renders. */
   struct json_object *channels = json ? json_tokener_parse(json) : NULL;
   if (json) {
      free(json);
   }
   if (!channels || !json_object_is_type(channels, json_type_array)) {
      if (channels) {
         json_object_put(channels);
      }
      channels = json_object_new_array();
   }

   struct json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("list_channels_response"));
   struct json_object *payload = json_object_new_object();
   json_object_object_add(payload, "channels", channels);
   json_object_object_add(response, "payload", payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * create_link_code — { provider? } → { code, ttl_seconds, provider }
 * ============================================================================= */

void handle_create_link_code(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   const char *provider = NULL;
   struct json_object *prov_obj = NULL;
   if (payload && json_object_object_get_ex(payload, "provider", &prov_obj)) {
      const char *p = json_object_get_string(prov_obj);
      if (p && p[0] != '\0') {
         if (!messaging_engine_provider_known(p)) {
            send_error_impl(conn->wsi, "INVALID_PARAM",
                            "Unknown provider (use telegram|discord|slack|sms)");
            return;
         }
         provider = p;
      }
   }

   char code[MESSAGING_LINK_CODE_BUF_SIZE];
   int rc = messaging_engine_generate_link_code(conn->auth_user_id, provider, code, sizeof(code));
   if (rc != MESSAGING_SUCCESS) {
      send_error_impl(conn->wsi, "SERVICE_ERROR", "Failed to generate link code");
      return;
   }

   struct json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("create_link_code_response"));
   struct json_object *payload_out = json_object_new_object();
   json_object_object_add(payload_out, "code", json_object_new_string(code));
   json_object_object_add(payload_out, "ttl_seconds",
                          json_object_new_int(MESSAGING_LINK_TTL_SECONDS));
   json_object_object_add(payload_out, "provider",
                          json_object_new_string(provider ? provider : ""));
   json_object_object_add(response, "payload", payload_out);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * unlink_channel — { id } → { success, id }
 * ============================================================================= */

void handle_unlink_channel(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }
   struct json_object *id_obj = NULL;
   if (!payload || !json_object_object_get_ex(payload, "id", &id_obj)) {
      send_error_impl(conn->wsi, "INVALID_PARAM", "Missing 'id'");
      return;
   }
   int64_t channel_id = json_object_get_int64(id_obj);
   if (channel_id <= 0) {
      send_error_impl(conn->wsi, "INVALID_PARAM", "Invalid 'id'");
      return;
   }

   int rc = messaging_engine_unlink_channel_by_id(conn->auth_user_id, channel_id);
   if (rc == MESSAGING_UNKNOWN_CHANNEL) {
      send_error_impl(conn->wsi, "NOT_FOUND", "No such channel");
      return;
   }
   if (rc != MESSAGING_SUCCESS) {
      send_error_impl(conn->wsi, "SERVICE_ERROR", "Failed to unlink channel");
      return;
   }

   struct json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("unlink_channel_response"));
   struct json_object *payload_out = json_object_new_object();
   json_object_object_add(payload_out, "success", json_object_new_boolean(1));
   json_object_object_add(payload_out, "id", json_object_new_int64(channel_id));
   json_object_object_add(response, "payload", payload_out);
   send_json_response(conn, response);
   json_object_put(response);

   OLOG_INFO("WebUI: user %d unlinked messaging channel id %lld", conn->auth_user_id,
             (long long)channel_id);
}

/* =============================================================================
 * rename_channel — { id, name } → { success, id, name }
 * ============================================================================= */

void handle_rename_channel(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }
   struct json_object *id_obj = NULL;
   struct json_object *name_obj = NULL;
   if (!payload || !json_object_object_get_ex(payload, "id", &id_obj) ||
       !json_object_object_get_ex(payload, "name", &name_obj)) {
      send_error_impl(conn->wsi, "INVALID_PARAM", "Missing 'id' or 'name'");
      return;
   }
   int64_t channel_id = json_object_get_int64(id_obj);
   const char *new_name = json_object_get_string(name_obj);
   if (channel_id <= 0 || !new_name || new_name[0] == '\0') {
      send_error_impl(conn->wsi, "INVALID_PARAM", "Invalid 'id' or empty 'name'");
      return;
   }
   if (strlen(new_name) >= MESSAGING_DISPLAY_NAME_MAX) {
      send_error_impl(conn->wsi, "INVALID_PARAM", "Name must be under 64 characters");
      return;
   }

   int rc = messaging_engine_rename_channel_by_id(conn->auth_user_id, channel_id, new_name);
   if (rc == MESSAGING_UNKNOWN_CHANNEL) {
      send_error_impl(conn->wsi, "NOT_FOUND", "No such channel");
      return;
   }
   if (rc == MESSAGING_NAME_TAKEN) {
      send_error_impl(conn->wsi, "NAME_TAKEN", "A channel with that name already exists");
      return;
   }
   if (rc != MESSAGING_SUCCESS) {
      send_error_impl(conn->wsi, "SERVICE_ERROR", "Failed to rename channel");
      return;
   }

   struct json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("rename_channel_response"));
   struct json_object *payload_out = json_object_new_object();
   json_object_object_add(payload_out, "success", json_object_new_boolean(1));
   json_object_object_add(payload_out, "id", json_object_new_int64(channel_id));
   json_object_object_add(payload_out, "name", json_object_new_string(new_name));
   json_object_object_add(response, "payload", payload_out);
   send_json_response(conn, response);
   json_object_put(response);

   OLOG_INFO("WebUI: user %d renamed messaging channel id %lld", conn->auth_user_id,
             (long long)channel_id);
}

/* =============================================================================
 * reenable_channel — { id } → { success, id }  (inverse of unlink)
 * ============================================================================= */

void handle_reenable_channel(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }
   struct json_object *id_obj = NULL;
   if (!payload || !json_object_object_get_ex(payload, "id", &id_obj)) {
      send_error_impl(conn->wsi, "INVALID_PARAM", "Missing 'id'");
      return;
   }
   int64_t channel_id = json_object_get_int64(id_obj);
   if (channel_id <= 0) {
      send_error_impl(conn->wsi, "INVALID_PARAM", "Invalid 'id'");
      return;
   }

   int rc = messaging_engine_reenable_channel_by_id(conn->auth_user_id, channel_id);
   if (rc == MESSAGING_UNKNOWN_CHANNEL) {
      send_error_impl(conn->wsi, "NOT_FOUND", "No such unlinked channel");
      return;
   }
   if (rc == MESSAGING_NAME_TAKEN) {
      send_error_impl(conn->wsi, "NAME_TAKEN", "An enabled channel with that name already exists");
      return;
   }
   if (rc != MESSAGING_SUCCESS) {
      send_error_impl(conn->wsi, "SERVICE_ERROR", "Failed to re-enable channel");
      return;
   }

   struct json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("reenable_channel_response"));
   struct json_object *payload_out = json_object_new_object();
   json_object_object_add(payload_out, "success", json_object_new_boolean(1));
   json_object_object_add(payload_out, "id", json_object_new_int64(channel_id));
   json_object_object_add(response, "payload", payload_out);
   send_json_response(conn, response);
   json_object_put(response);

   OLOG_INFO("WebUI: user %d re-enabled messaging channel id %lld", conn->auth_user_id,
             (long long)channel_id);
}
