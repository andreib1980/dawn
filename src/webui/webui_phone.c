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
 * WebUI phone notifications — server->browser incoming-call banner broadcast
 * (outbound) and the phone_action answer/reject handler (inbound).
 *
 * Mirrors the scheduler notification pattern in webui_broadcasts.c:
 *   - webui_broadcast_phone_call() is the strong override of the weak symbol
 *     declared in tools/phone_service.h; it fans a phone_call_notification
 *     frame out to the modem owner's authenticated browser sessions.
 *   - webui_phone_handle_action() runs the (blocking) answer/hang-up off the
 *     lws service thread via a short-lived detached thread.
 */

#include <json-c/json.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "logging.h"
#include "tools/phone_service.h"
#include "utils/string_utils.h"
#include "webui/webui_internal.h"
#include "webui/webui_send.h"

/* =============================================================================
 * Outbound: incoming-call banner / in-call panel broadcast
 * ============================================================================= */

/* Serialize a phone_call_notification frame to a heap string (caller frees).
 * Shared by the broadcast and the per-connection snapshot reply. */
static char *phone_call_json(const char *status_str,
                             const char *number,
                             const char *contact_name,
                             int64_t call_id,
                             int elapsed_sec,
                             struct json_object *photo) {
   /* Scrub caller-provided free text before it enters a WS text frame: a stray
    * invalid UTF-8 byte fails the frame (RFC 6455 §5.6) and wedges the client. */
   char name_safe[64];
   char num_safe[24];
   snprintf(name_safe, sizeof(name_safe), "%s", contact_name ? contact_name : "");
   snprintf(num_safe, sizeof(num_safe), "%s", number ? number : "");
   sanitize_utf8_for_json(name_safe);
   sanitize_utf8_for_json(num_safe);

   json_object *root = json_object_new_object();
   json_object_object_add(root, "type", json_object_new_string("phone_call_notification"));
   json_object *payload = json_object_new_object();
   json_object_object_add(payload, "status", json_object_new_string(status_str));
   json_object_object_add(payload, "name", json_object_new_string(name_safe));
   json_object_object_add(payload, "number", json_object_new_string(num_safe));
   json_object_object_add(payload, "call_id", json_object_new_int64(call_id));
   json_object_object_add(payload, "elapsed_sec", json_object_new_int(elapsed_sec));
   if (photo) {
      /* Borrowed — incref so the payload owns its own reference; the caller's
       * reference (freed when its HUD object is put) is unaffected. */
      json_object_object_add(payload, "photo", json_object_get(photo));
   }
   json_object_object_add(root, "payload", payload);

   const char *s = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN);
   char *copy = s ? strdup(s) : NULL;
   json_object_put(root);
   return copy;
}

void webui_broadcast_phone_call(int user_id,
                                phone_call_notif_status_t status,
                                const char *number,
                                const char *contact_name,
                                int64_t call_id,
                                int elapsed_sec,
                                struct json_object *photo) {
   const char *status_str = (status == PHONE_CALL_NOTIF_RINGING)  ? "ringing"
                            : (status == PHONE_CALL_NOTIF_ACTIVE) ? "active"
                                                                  : "ended";

   char *json_str = phone_call_json(status_str, number, contact_name, call_id, elapsed_sec, photo);
   if (!json_str)
      return;

   int sent = 0;
   pthread_mutex_lock(&s_conn_registry_mutex);
   for (int i = 0; i < MAX_ACTIVE_CONNECTIONS; i++) {
      ws_connection_t *conn = s_active_connections[i];
      if (!conn || !conn->session)
         continue;
      /* Browser sessions only — the banner is a WebUI surface, not a satellite
       * or unauthenticated login screen. */
      if (!conn->authenticated || conn->is_satellite)
         continue;
      /* Owner-only.  A non-positive owner id (misconfig) sends to nobody rather
       * than leaking caller PII to every user's sessions. */
      if (user_id <= 0 || conn->auth_user_id != user_id)
         continue;

      char *json_copy = strdup(json_str);
      if (!json_copy)
         continue;
      ws_response_t resp = { .session = conn->session,
                             .type = WS_RESP_JSON,
                             .generic_json = { .json = json_copy } };
      queue_response(&resp);
      sent++;
   }
   pthread_mutex_unlock(&s_conn_registry_mutex);

   free(json_str);

   if (sent > 0) {
      OLOG_INFO("WebUI: phone call notification (%s) to %d client(s)", status_str, sent);
   }
}

void webui_phone_send_status(ws_connection_t *conn) {
   if (!conn || !conn->session || !conn->authenticated || conn->is_satellite)
      return;

   /* Owner-only, same gate as the broadcast. */
   const phone_service_config_t *pcfg = phone_service_get_config();
   int owner = pcfg ? pcfg->user_id : 0;
   if (owner <= 0 || conn->auth_user_id != owner)
      return;

   phone_call_notif_status_t st = PHONE_CALL_NOTIF_ACTIVE;
   char number[24] = "";
   char name[64] = "";
   int64_t call_id = -1;
   int elapsed = 0;
   if (!phone_service_get_call_snapshot(&st, number, sizeof(number), name, sizeof(name), &call_id,
                                        &elapsed)) {
      return; /* idle — nothing to rehydrate */
   }

   const char *status_str = (st == PHONE_CALL_NOTIF_RINGING) ? "ringing" : "active";
   char *json_str = phone_call_json(status_str, number, name, call_id, elapsed, NULL);
   if (!json_str)
      return;
   ws_response_t resp = { .session = conn->session,
                          .type = WS_RESP_JSON,
                          .generic_json = { .json = json_str } };
   queue_response(&resp);
}

/* =============================================================================
 * Inbound: answer / reject from a browser banner button
 * ============================================================================= */

typedef struct {
   int user_id;
   char action[16];
} phone_action_arg_t;

/* Single-slot guard: only one phone_action round-trip may be in flight at a
 * time, so a spammed/duplicated button can't pile up blocked threads. */
static atomic_bool s_phone_action_inflight = false;

/* phone_service_answer()/hangup() publish to ECHO and block on the response
 * round-trip, so they must not run on the lws service thread. */
static void *phone_action_thread(void *arg) {
   phone_action_arg_t *a = (phone_action_arg_t *)arg;
   char result[128] = "";

   if (strcmp(a->action, "answer") == 0) {
      phone_service_answer(a->user_id, result, sizeof(result));
   } else {
      /* "reject" (decline a ring) and "hangup" (end an active call) both end
       * the call via the same ECHO path. */
      phone_service_hangup(a->user_id, result, sizeof(result));
   }

   OLOG_INFO("WebUI: phone_action '%s' (user %d) -> %s", a->action, a->user_id, result);
   free(a);
   atomic_store(&s_phone_action_inflight, false);
   return NULL;
}

void webui_phone_handle_action(ws_connection_t *conn, const char *action) {
   if (!conn || !action)
      return;

   /* Owner-gate: only the modem owner's (non-satellite) browser session may
    * answer/reject.  The outbound banner is already owner-filtered; enforce the
    * same on the inbound action so a second authenticated user can't seize the
    * shared modem (answer to eavesdrop, or reject to drop the owner's calls). */
   const phone_service_config_t *pcfg = phone_service_get_config();
   int owner = pcfg ? pcfg->user_id : 0;
   if (conn->is_satellite || owner <= 0 || conn->auth_user_id != owner) {
      send_error_impl(conn->wsi, "FORBIDDEN", "Not the modem owner");
      return;
   }

   /* Reject a duplicate while one is already running (claim the slot). */
   bool expected = false;
   if (!atomic_compare_exchange_strong(&s_phone_action_inflight, &expected, true)) {
      return;
   }

   phone_action_arg_t *a = calloc(1, sizeof(*a));
   if (!a) {
      atomic_store(&s_phone_action_inflight, false);
      send_error_impl(conn->wsi, "INTERNAL", "Out of memory");
      return;
   }
   a->user_id = conn->auth_user_id;
   snprintf(a->action, sizeof(a->action), "%s", action);

   pthread_t t;
   if (pthread_create(&t, NULL, phone_action_thread, a) != 0) {
      free(a);
      atomic_store(&s_phone_action_inflight, false);
      send_error_impl(conn->wsi, "INTERNAL", "Failed to dispatch phone action");
      return;
   }
   pthread_detach(t);
}
