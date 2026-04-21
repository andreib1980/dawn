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
 * WebUI Admin Satellite Handlers - Satellite management endpoints (admin-only)
 *
 * Provides list/update/delete operations for satellite-to-user mappings.
 * Changes are DB-only and take effect on satellite's next reconnect.
 */

#include <string.h>
#include <time.h>

#include "auth/auth_db.h"
#include "logging.h"
#include "webui/webui_internal.h"

#ifdef DAWN_ENABLE_HOMEASSISTANT_TOOL
#include "tools/homeassistant_service.h"
#endif

/* Maximum satellites we expect (for stack-allocated collection buffer) */
#define MAX_SATELLITE_MAPPINGS 64

/* =============================================================================
 * Shared: Serialize satellite_mapping_t to json_object
 * ============================================================================= */

static json_object *satellite_mapping_to_json(const satellite_mapping_t *m, bool online) {
   json_object *sat = json_object_new_object();
   json_object_object_add(sat, "uuid", json_object_new_string(m->uuid));
   json_object_object_add(sat, "name", json_object_new_string(m->name));
   json_object_object_add(sat, "location", json_object_new_string(m->location));
   json_object_object_add(sat, "ha_area", json_object_new_string(m->ha_area));
   json_object_object_add(sat, "user_id", json_object_new_int(m->user_id));
   json_object_object_add(sat, "tier", json_object_new_int(m->tier));
   json_object_object_add(sat, "enabled", json_object_new_boolean(m->enabled));
   json_object_object_add(sat, "last_seen", json_object_new_int64((int64_t)m->last_seen));
   json_object_object_add(sat, "created_at", json_object_new_int64((int64_t)m->created_at));
   json_object_object_add(sat, "online", json_object_new_boolean(online));
   return sat;
}

/* =============================================================================
 * Callback: Collect satellite mappings into array (no online-status lookup)
 *
 * This avoids calling webui_is_satellite_online() while the DB mutex is held,
 * preventing a db_mutex -> conn_registry_mutex lock ordering dependency.
 * ============================================================================= */

typedef struct {
   satellite_mapping_t *mappings;
   int count;
   int capacity;
} satellite_collect_ctx_t;

static int collect_satellites_callback(const satellite_mapping_t *mapping, void *ctx) {
   satellite_collect_ctx_t *collect = (satellite_collect_ctx_t *)ctx;
   if (collect->count >= collect->capacity) {
      OLOG_WARNING("Satellite list truncated at %d entries", collect->capacity);
      return 1; /* Stop iteration */
   }
   collect->mappings[collect->count] = *mapping;
   collect->count++;
   return 0;
}

/* =============================================================================
 * Callback: Build minimal user list for dropdown
 * ============================================================================= */

static int list_users_minimal_callback(const auth_user_summary_t *user, void *ctx) {
   json_object *array = (json_object *)ctx;
   json_object *u = json_object_new_object();
   json_object_object_add(u, "id", json_object_new_int(user->id));
   json_object_object_add(u, "display_name", json_object_new_string(user->username));
   json_object_array_add(array, u);
   return 0;
}

/* =============================================================================
 * Handlers
 * ============================================================================= */

void handle_list_satellites(ws_connection_t *conn) {
   if (!conn_require_admin(conn))
      return;

   /* Collect mappings from DB (releases DB lock before online-status check) */
   satellite_mapping_t mappings[MAX_SATELLITE_MAPPINGS];
   satellite_collect_ctx_t ctx = { .mappings = mappings,
                                   .count = 0,
                                   .capacity = MAX_SATELLITE_MAPPINGS };
   satellite_db_list(collect_satellites_callback, &ctx);

   /* Build JSON array with online status (now outside DB lock) */
   json_object *sat_array = json_object_new_array();
   for (int i = 0; i < ctx.count; i++) {
      bool online = webui_is_satellite_online(mappings[i].uuid);
      json_object_array_add(sat_array, satellite_mapping_to_json(&mappings[i], online));
   }

   /* Build minimal user list for dropdown */
   json_object *users_array = json_object_new_array();
   auth_db_list_users(list_users_minimal_callback, users_array);

   /* Build response */
   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("list_satellites_response"));

   json_object *payload = json_object_new_object();
   json_object_object_add(payload, "satellites", sat_array);
   json_object_object_add(payload, "users", users_array);

   /* Include HA area names for dropdown if available */
#ifdef DAWN_ENABLE_HOMEASSISTANT_TOOL
   if (homeassistant_is_configured()) {
      char area_names[128][64];
      int area_count = homeassistant_list_areas(area_names, 128);
      if (area_count > 0) {
         json_object *areas_array = json_object_new_array();
         for (int i = 0; i < area_count; i++) {
            json_object_array_add(areas_array, json_object_new_string(area_names[i]));
         }
         json_object_object_add(payload, "ha_areas", areas_array);
      }
   }
#endif

   json_object_object_add(response, "payload", payload);
   send_json_response(conn, response);
   json_object_put(response);
}

void handle_update_satellite(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_admin(conn))
      return;

   struct json_object *uuid_obj;
   if (!json_object_object_get_ex(payload, "uuid", &uuid_obj)) {
      send_error_impl(conn->wsi, "INVALID_PARAM", "Missing 'uuid'");
      return;
   }

   const char *uuid = json_object_get_string(uuid_obj);
   if (!uuid || strlen(uuid) != 36) {
      send_error_impl(conn->wsi, "INVALID_PARAM", "Invalid UUID");
      return;
   }

   /* Get current mapping */
   satellite_mapping_t mapping;
   int rc = satellite_db_get(uuid, &mapping);
   if (rc == AUTH_DB_NOT_FOUND) {
      send_error_impl(conn->wsi, "NOT_FOUND", "Satellite not found");
      return;
   } else if (rc != AUTH_DB_SUCCESS) {
      send_error_impl(conn->wsi, "DB_ERROR", "Database error");
      return;
   }

   /* Apply requested updates */
   struct json_object *user_id_obj, *ha_area_obj;
   if (json_object_object_get_ex(payload, "user_id", &user_id_obj)) {
      int new_user_id = json_object_get_int(user_id_obj);
      satellite_db_update_user(uuid, new_user_id);
      mapping.user_id = new_user_id;
   }

   if (json_object_object_get_ex(payload, "ha_area", &ha_area_obj)) {
      const char *ha_area = json_object_get_string(ha_area_obj);
      if (ha_area) {
         /* Sanitize: allowlist alphanumeric, spaces, hyphens, underscores */
         char safe[SATELLITE_LOCATION_MAX];
         strncpy(safe, ha_area, sizeof(safe) - 1);
         safe[sizeof(safe) - 1] = '\0';
         for (char *p = safe; *p; p++) {
            if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                  (*p >= '0' && *p <= '9') || *p == ' ' || *p == '-' || *p == '_'))
               *p = '_';
         }
         satellite_db_update_location(uuid, mapping.location, safe);
         strncpy(mapping.ha_area, safe, sizeof(mapping.ha_area) - 1);
      }
   }

   /* Force-reconnect so satellite picks up new config immediately */
   webui_force_disconnect_satellite(uuid);

   /* Build response — satellite will show offline briefly until it reconnects */
   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("update_satellite_response"));

   json_object *resp_payload = json_object_new_object();
   json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
   json_object_object_add(resp_payload, "satellite", satellite_mapping_to_json(&mapping, false));

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);

   OLOG_INFO("Admin: Updated satellite %s (%s) user_id=%d ha_area='%s'", mapping.name, uuid,
             mapping.user_id, mapping.ha_area);
}

void handle_delete_satellite(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_admin(conn))
      return;

   struct json_object *uuid_obj;
   if (!json_object_object_get_ex(payload, "uuid", &uuid_obj)) {
      send_error_impl(conn->wsi, "INVALID_PARAM", "Missing 'uuid'");
      return;
   }

   const char *uuid = json_object_get_string(uuid_obj);
   if (!uuid || strlen(uuid) != 36) {
      send_error_impl(conn->wsi, "INVALID_PARAM", "Invalid UUID");
      return;
   }

   /* The local pseudo-satellite represents the daemon's own speaker — deleting
    * it doesn't make sense. Admin can still disable or reassign it. */
   if (satellite_is_local_pseudo(uuid)) {
      send_error_impl(conn->wsi, "FORBIDDEN",
                      "Local device cannot be deleted; reassign to another user instead");
      return;
   }

   /* Force-disconnect if online before removing mapping */
   webui_force_disconnect_satellite(uuid);

   int rc = satellite_db_delete(uuid);
   if (rc == AUTH_DB_NOT_FOUND) {
      send_error_impl(conn->wsi, "NOT_FOUND", "Satellite not found");
      return;
   } else if (rc != AUTH_DB_SUCCESS) {
      send_error_impl(conn->wsi, "DB_ERROR", "Failed to delete satellite");
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("delete_satellite_response"));

   json_object *resp_payload = json_object_new_object();
   json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
   json_object_object_add(resp_payload, "uuid", json_object_new_string(uuid));

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);

   OLOG_INFO("Admin: Deleted satellite mapping %s", uuid);
}
