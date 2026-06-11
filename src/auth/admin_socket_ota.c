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
 * OTA operator admin opcode handlers (dawn-admin ota list / push).  Sibling of
 * admin_socket_messaging.c.  The list path reads the release store via the
 * Layer-2 ota.c engine; the push path delegates to webui_ota_push() (the same
 * resolve→mint→deliver spine the WebUI admin panel uses) so the two operator
 * surfaces never diverge.  See docs/OTA_DESIGN.md §3, §6.
 */

#define ADMIN_SOCKET_INTERNAL_ALLOWED

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "auth/admin_socket_internal.h"
#include "auth/auth_db.h"
#include "core/ota.h"
#include "core/ota_manifest.h" /* ota_platform_t */
#include "core/ota_rollout.h"
#include "dawn_error.h"
#include "logging.h"

/* Push delivery lives in the WebUI layer (it needs the session response queue).
 * Forward-declared here to avoid pulling webui_internal.h into src/auth/ — the
 * symbol is always linked because ENABLE_AUTH requires ENABLE_WEBUI.  Return:
 * AUTH_DB_SUCCESS | AUTH_DB_LOCKED | AUTH_DB_NOT_FOUND | AUTH_DB_FAILURE.
 * KEEP IN SYNC with the canonical prototype in include/webui/webui_ota.h. */
int webui_ota_push(const char *uuid, const char *version, bool allow_downgrade);

/* =============================================================================
 * OTA: list — available releases as an aligned text table.
 * ============================================================================= */

int handle_ota_list_cmd(int client_fd) {
   if (!ota_enabled()) {
      return send_text_response(client_fd, ADMIN_RESP_SUCCESS,
                                "OTA is disabled ([ota].enabled = false).");
   }

   char buf[ADMIN_MSG_CONTENT_MAX + 1];
   size_t off = 0;
   int n = ota_release_count();

/* snprintf returns the would-have-written length, which can exceed the
 * remaining space; clamp so 'off' never advances past the buffer (otherwise
 * 'sizeof(buf) - off' underflows into a huge size_t on the next call). */
#define OTA_LIST_APPEND(...)                                                       \
   do {                                                                            \
      if (off >= sizeof(buf)) {                                                    \
         break;                                                                    \
      }                                                                            \
      int w_ = snprintf(buf + off, sizeof(buf) - off, __VA_ARGS__);                \
      if (w_ < 0) {                                                                \
         break;                                                                    \
      }                                                                            \
      off = (off + (size_t)w_ < sizeof(buf)) ? off + (size_t)w_ : sizeof(buf) - 1; \
   } while (0)

   OTA_LIST_APPEND("OTA enabled — %d release%s\n\n", n, (n == 1) ? "" : "s");
   if (n > 0) {
      OTA_LIST_APPEND("%-8s %-5s %s\n", "PLATFORM", "TIER", "VERSION");
   }
   for (int i = 0; i < n; i++) {
      ota_platform_t plat = OTA_PLATFORM_UNKNOWN;
      int tier = 0;
      char ver[OTA_VERSION_MAX] = "";
      if (ota_release_info(i, &plat, &tier, ver, sizeof(ver)) != SUCCESS) {
         continue;
      }
      OTA_LIST_APPEND("%-8s %-5d %s\n", (plat == OTA_PLATFORM_ESP32) ? "esp32" : "rpi", tier, ver);
   }
   if (n == 0) {
      OTA_LIST_APPEND("No releases in the store.");
   }
#undef OTA_LIST_APPEND
   return send_text_response(client_fd, ADMIN_RESP_SUCCESS, buf);
}

/* =============================================================================
 * OTA: rescan — re-load the release dir into the in-memory store at runtime so a
 * freshly-staged release is pushable without a daemon restart.  No payload.
 * ============================================================================= */

int handle_ota_rescan_cmd(int client_fd) {
   if (!ota_enabled()) {
      return send_text_response(client_fd, ADMIN_RESP_SUCCESS,
                                "OTA is disabled ([ota].enabled = false).");
   }
   int count = 0;
   if (ota_rescan(&count) != SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "OTA rescan failed.");
   }
   char msg[64];
   snprintf(msg, sizeof(msg), "Rescanned release store: %d release%s.", count,
            (count == 1) ? "" : "s");
   OLOG_INFO("OTA: admin rescanned release store (%d release(s))", count);
   return send_text_response(client_fd, ADMIN_RESP_SUCCESS, msg);
}

/* =============================================================================
 * OTA: push — offer an update to one device by uuid.
 *
 * Wire format (see admin_socket.h "OTA payloads"):
 *   Byte 0:        flags (bit 0 = allow_downgrade)
 *   Byte 1:        uuid_len (1..ADMIN_OTA_UUID_MAX)
 *   Bytes 2..1+U:  uuid
 *   Bytes 2+U..:   version (length = payload_len - 2 - uuid_len)
 * ============================================================================= */

int handle_ota_push_cmd(int client_fd, const char *payload, uint16_t payload_len) {
   if (!ota_enabled()) {
      return send_text_response(client_fd, ADMIN_RESP_SUCCESS,
                                "OTA is disabled ([ota].enabled = false).");
   }
   /* Minimum: flags + uuid_len + 1 uuid byte + 1 version byte = 4. */
   if (!payload || payload_len < 4) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "Invalid ota push payload");
   }
   uint8_t flags = (uint8_t)payload[0];
   uint8_t uuid_len = (uint8_t)payload[1];
   if (uuid_len == 0 || uuid_len > ADMIN_OTA_UUID_MAX || (size_t)2 + uuid_len >= payload_len) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "Invalid device uuid");
   }
   char uuid[ADMIN_OTA_UUID_MAX + 1];
   memcpy(uuid, payload + 2, uuid_len);
   uuid[uuid_len] = '\0';

   uint16_t ver_len = (uint16_t)(payload_len - 2 - uuid_len);
   if (ver_len == 0 || ver_len > ADMIN_OTA_VERSION_MAX) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "Invalid version");
   }
   char version[ADMIN_OTA_VERSION_MAX + 1];
   memcpy(version, payload + 2 + uuid_len, ver_len);
   version[ver_len] = '\0';

   bool allow_downgrade = (flags & ADMIN_OTA_FLAG_ALLOW_DOWNGRADE) != 0;

   int rc = webui_ota_push(uuid, version, allow_downgrade);
   switch (rc) {
      case AUTH_DB_SUCCESS: {
         char msg[128];
         snprintf(msg, sizeof(msg), "Pushed %s to %s.", version, uuid);
         OLOG_INFO("OTA: admin pushed %s to %s%s", version, uuid,
                   allow_downgrade ? " (allow-downgrade)" : "");
         return send_text_response(client_fd, ADMIN_RESP_SUCCESS, msg);
      }
      case AUTH_DB_LOCKED:
         return send_text_response(client_fd, ADMIN_RESP_FAILURE,
                                   "Device is already mid-update (use a re-push after it "
                                   "reports success or failure)");
      case AUTH_DB_NOT_FOUND:
         return send_text_response(client_fd, ADMIN_RESP_NOT_FOUND, "Device is offline");
      default:
         return send_text_response(client_fd, ADMIN_RESP_FAILURE,
                                   "No matching release for the device, or push failed");
   }
}

/* =============================================================================
 * OTA: push-all — canary-then-rollout to a platform/tier.
 *
 * Wire format:
 *   Byte 0:     flags (bit 0 = allow_downgrade)
 *   Byte 1:     tier  (1 = RPi, 2 = ESP32; platform derived from tier)
 *   Bytes 2..:  version (length = payload_len - 2)
 * ============================================================================= */

int handle_ota_push_all_cmd(int client_fd, const char *payload, uint16_t payload_len) {
   /* Minimum: flags + tier + 1 version byte = 3. */
   if (!payload || payload_len < 3) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "Invalid ota push-all payload");
   }
   if (!ota_enabled()) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE,
                                "OTA is disabled ([ota].enabled = false).");
   }
   uint8_t flags = (uint8_t)payload[0];
   uint8_t tier = (uint8_t)payload[1];
   if (tier != 1 && tier != 2) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "Invalid tier (must be 1 or 2)");
   }
   uint16_t ver_len = (uint16_t)(payload_len - 2);
   if (ver_len == 0 || ver_len > ADMIN_OTA_VERSION_MAX) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "Invalid version");
   }
   char version[ADMIN_OTA_VERSION_MAX + 1];
   memcpy(version, payload + 2, ver_len);
   version[ver_len] = '\0';

   bool allow_downgrade = (flags & ADMIN_OTA_FLAG_ALLOW_DOWNGRADE) != 0;
   ota_platform_t platform = (tier == 2) ? OTA_PLATFORM_ESP32 : OTA_PLATFORM_RPI;

   char summary[256];
   int rc = ota_rollout_start(platform, tier, version, allow_downgrade, summary, sizeof(summary));
   if (rc == SUCCESS) {
      OLOG_INFO("OTA: admin started rollout of %s (tier %d)%s", version, tier,
                allow_downgrade ? " (allow-downgrade)" : "");
      return send_text_response(client_fd, ADMIN_RESP_SUCCESS, summary);
   }
   return send_text_response(client_fd, ADMIN_RESP_FAILURE,
                             summary[0] ? summary : "Rollout could not be started");
}

int handle_ota_rollout_status_cmd(int client_fd) {
   char buf[512];
   if (ota_rollout_status(buf, sizeof(buf)) != SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "Could not read rollout status");
   }
   return send_text_response(client_fd, ADMIN_RESP_SUCCESS, buf);
}

int handle_ota_rollout_abort_cmd(int client_fd) {
   if (ota_rollout_abort() != SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_SUCCESS, "No rollout in progress.");
   }
   OLOG_INFO("OTA: admin aborted in-progress rollout");
   return send_text_response(client_fd, ADMIN_RESP_SUCCESS, "Rollout aborted.");
}
