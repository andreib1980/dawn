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
 * the project author(s). Contributions include any modifications,
 * enhancements, or additions to the project. These contributions become
 * part of the project and are adopted by the project author(s).
 *
 * WebUI Settings Handlers - Personal user settings management
 *
 * This module handles WebSocket messages for:
 * - get_my_settings (get user's personal settings)
 * - set_my_settings (update user's personal settings)
 */

#include <string.h>

#include "auth/auth_db.h"
#include "config/dawn_config.h"
#include "core/session_manager.h"
#include "dawn.h"
#include "logging.h"
#include "webui/webui_internal.h"

/**
 * @brief Get current user's personal settings
 */
void handle_get_my_settings(ws_connection_t *conn) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("get_my_settings_response"));
   json_object *resp_payload = json_object_new_object();

   auth_user_settings_t settings;
   int result = auth_db_get_user_settings(conn->auth_user_id, &settings);

   if (result == AUTH_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));

      /* Include base persona (from config or dynamic default) for UI display */
      char base_persona_buf[2048];
      const char *base_persona;
      if (g_config.persona.description[0] != '\0') {
         base_persona = g_config.persona.description;
      } else {
         /* Build dynamic persona with configured AI name */
         const char *ai_name = g_config.general.ai_name[0] != '\0' ? g_config.general.ai_name
                                                                   : AI_NAME;

         /* Capitalize first letter for proper noun */
         char capitalized_name[64];
         snprintf(capitalized_name, sizeof(capitalized_name), "%s", ai_name);
         if (capitalized_name[0] >= 'a' && capitalized_name[0] <= 'z') {
            capitalized_name[0] -= 32;
         }

         snprintf(base_persona_buf, sizeof(base_persona_buf),
                  AI_PERSONA_NAME_TEMPLATE " " AI_PERSONA_TRAITS, capitalized_name);
         base_persona = base_persona_buf;
      }
      json_object_object_add(resp_payload, "base_persona", json_object_new_string(base_persona));

      /* User's custom settings */
      json_object_object_add(resp_payload, "persona_description",
                             json_object_new_string(settings.persona_description));
      json_object_object_add(resp_payload, "persona_mode",
                             json_object_new_string(settings.persona_mode));
      json_object_object_add(resp_payload, "location", json_object_new_string(settings.location));
      json_object_object_add(resp_payload, "timezone", json_object_new_string(settings.timezone));
      json_object_object_add(resp_payload, "units", json_object_new_string(settings.units));
      json_object_object_add(resp_payload, "theme", json_object_new_string(settings.theme));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to load settings"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/**
 * @brief Update current user's personal settings
 */
void handle_set_my_settings(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("set_my_settings_response"));
   json_object *resp_payload = json_object_new_object();

   /* Get current settings as defaults */
   auth_user_settings_t settings;
   auth_db_get_user_settings(conn->auth_user_id, &settings);

   /* Update with any provided fields */
   json_object *field_obj;

   if (json_object_object_get_ex(payload, "persona_description", &field_obj)) {
      strncpy(settings.persona_description, json_object_get_string(field_obj),
              AUTH_PERSONA_DESC_MAX - 1);
      settings.persona_description[AUTH_PERSONA_DESC_MAX - 1] = '\0';
   }

   if (json_object_object_get_ex(payload, "persona_mode", &field_obj)) {
      const char *mode = json_object_get_string(field_obj);
      /* Validate mode value */
      if (strcmp(mode, "append") == 0 || strcmp(mode, "replace") == 0) {
         strncpy(settings.persona_mode, mode, AUTH_PERSONA_MODE_MAX - 1);
         settings.persona_mode[AUTH_PERSONA_MODE_MAX - 1] = '\0';
      }
   }

   if (json_object_object_get_ex(payload, "location", &field_obj)) {
      strncpy(settings.location, json_object_get_string(field_obj), AUTH_LOCATION_MAX - 1);
      settings.location[AUTH_LOCATION_MAX - 1] = '\0';
   }

   if (json_object_object_get_ex(payload, "timezone", &field_obj)) {
      strncpy(settings.timezone, json_object_get_string(field_obj), AUTH_TIMEZONE_MAX - 1);
      settings.timezone[AUTH_TIMEZONE_MAX - 1] = '\0';
   }

   if (json_object_object_get_ex(payload, "units", &field_obj)) {
      const char *units = json_object_get_string(field_obj);
      /* Validate units value */
      if (strcmp(units, "metric") == 0 || strcmp(units, "imperial") == 0) {
         strncpy(settings.units, units, AUTH_UNITS_MAX - 1);
         settings.units[AUTH_UNITS_MAX - 1] = '\0';
      }
   }

   if (json_object_object_get_ex(payload, "theme", &field_obj)) {
      const char *theme = json_object_get_string(field_obj);
      /* Validate theme value - check for NULL first */
      if (theme && (strcmp(theme, "cyan") == 0 || strcmp(theme, "purple") == 0 ||
                    strcmp(theme, "green") == 0 || strcmp(theme, "orange") == 0 ||
                    strcmp(theme, "red") == 0 || strcmp(theme, "blue") == 0 ||
                    strcmp(theme, "terminal") == 0)) {
         strncpy(settings.theme, theme, AUTH_THEME_MAX - 1);
         settings.theme[AUTH_THEME_MAX - 1] = '\0';
      }
   }

   /* Save settings */
   int result = auth_db_set_user_settings(conn->auth_user_id, &settings);

   if (result == AUTH_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "message", json_object_new_string("Settings saved"));

      /* Refresh active session's system prompt immediately (preserves conversation) */
      if (conn->session) {
         /* Phase 1f: SESSION_START builder boundary — clear dedup state. */
         session_injected_set_clear(conn->session);
         char *new_prompt = session_manager_build_system_prompt_string(conn->auth_user_id);
         if (new_prompt) {
            session_update_system_prompt(conn->session, new_prompt);
            OLOG_INFO("WebUI: Refreshed system prompt for user %s", conn->username);

            /* Queue updated prompt to client so debug view refreshes.
             * Must go through queue — the settings response below is already
             * a direct write in this callback, so a second write would corrupt
             * WebSocket framing. */
            json_object *prompt_msg = json_object_new_object();
            json_object_object_add(prompt_msg, "type",
                                   json_object_new_string("system_prompt_response"));
            json_object *prompt_payload = json_object_new_object();
            json_object_object_add(prompt_payload, "success", json_object_new_boolean(1));
            json_object_object_add(prompt_payload, "prompt", json_object_new_string(new_prompt));
            json_object_object_add(prompt_payload, "length",
                                   json_object_new_int((int)strlen(new_prompt)));
            json_object_object_add(prompt_msg, "payload", prompt_payload);
            const char *json_str = json_object_to_json_string(prompt_msg);
            ws_response_t resp_q = { 0 };
            resp_q.session = conn->session;
            resp_q.type = WS_RESP_JSON;
            resp_q.generic_json.json = strdup(json_str);
            queue_response(&resp_q);
            json_object_put(prompt_msg);

            free(new_prompt);
         }
      }

      /* Log event */
      auth_db_log_event("SETTINGS_UPDATED", conn->username, conn->client_ip, "Personal settings");
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to save settings"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}
