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
 * OTA daemon glue (Layer 2) — release store + device update orchestration.
 * Builds on the pure ota_manifest core and the ota_db state machine.  Has NO
 * webui dependency: the WS/HTTP/admin transports (Layer 4) call down into here.
 * See docs/OTA_DESIGN.md §2, §5, §6.
 */

#ifndef OTA_H
#define OTA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/ota_manifest.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OTA_TOKEN_BYTES 32
#define OTA_TOKEN_HEX (OTA_TOKEN_BYTES * 2)

/* What a connected satellite needs to fetch + verify an update.  The manifest +
 * signature are inlined (hex) because they're tiny (164 + 64 bytes) — only the
 * image is pulled over HTTPS, so the download token gates a single GET. */
typedef struct {
   char platform_str[8]; /* "rpi" | "esp32" */
   char version[OTA_VERSION_MAX];
   char url_path[256];            /* "/api/ota/<platform>/<version>/image" */
   char token[OTA_TOKEN_HEX + 1]; /* one-time download token (hex) */
   char sha256_hex[OTA_SHA256_BYTES * 2 + 1];
   uint64_t image_size;
   char manifest_hex[OTA_MANIFEST_WIRE_SIZE * 2 + 1];
   char sig_hex[OTA_SIG_BYTES * 2 + 1];
   bool allow_downgrade;
} ota_offer_t;

/**
 * @brief Initialize the OTA subsystem.  Reads g_config.ota; if enabled, inits
 *        crypto, scans the release store, and reconciles restart-orphaned state.
 *        Safe to call when disabled (no-op).
 * @return SUCCESS or FAILURE
 */
int ota_init(void);

/** @brief True when [ota].enabled and the release store loaded. */
bool ota_enabled(void);

/**
 * @brief Re-scan the release directory into the in-memory store at runtime.
 *
 * Lets a freshly-staged release (e.g. just written by ota-release.sh) become
 * pushable without restarting the daemon.  Thread-safe against concurrent
 * readers (push/list).  Fails (no-op) if OTA is disabled.
 *
 * @param count_out Optional; receives the release count after the rescan.
 * @return SUCCESS, or FAILURE if OTA is disabled.
 */
int ota_rescan(int *count_out);

/**
 * @brief Latest available version for a platform/tier, or FAILURE if none.
 */
int ota_resolve_latest(ota_platform_t platform, int tier, char *out_version, size_t out_size);

/** @brief Number of releases currently in the store (for admin listing). */
int ota_release_count(void);

/**
 * @brief Read metadata for release @p index (0..count-1).  Any out param NULL-able.
 * @return SUCCESS or FAILURE (index out of range)
 */
int ota_release_info(int index,
                     ota_platform_t *platform_out,
                     int *tier_out,
                     char *version_out,
                     size_t version_size);

/**
 * @brief Prepare an update offer for a device (single-flight via ota_db).
 *
 * Resolves the release, mints a one-time download token (TTL from config),
 * records the offer in ota_device_state, and fills @p out with everything the
 * device needs.  Does NOT push the message — the caller (Layer 4) delivers it.
 *
 * @return SUCCESS, AUTH_DB_LOCKED if the device is already mid-update, or FAILURE
 */
int ota_begin_push(const char *uuid,
                   ota_platform_t platform,
                   const char *version,
                   bool allow_downgrade,
                   ota_offer_t *out);

/**
 * @brief Authorize + resolve the image path for a download request.
 *
 * Validates the platform/version path components (enum + semver, anti-traversal)
 * and consumes the one-time token (binding to uuid + version).  On success
 * writes the on-disk image path to @p out_path.
 *
 * @return SUCCESS or FAILURE
 */
int ota_authorize_image_download(const char *uuid,
                                 const char *platform_str,
                                 const char *version,
                                 const char *token,
                                 char *out_path,
                                 size_t out_size);

/**
 * @brief Record a device-reported status transition (visibility only).
 */
int ota_report_status(const char *uuid, const char *state, const char *error);

/**
 * @brief Validate a device-reported OTA state token against the known set.
 *
 * Satellite-supplied state strings cross a trust boundary; callers should reject
 * anything not in the OTA_STATE_* set before persisting it (an unknown token also
 * dodges the in-flight predicate logic).
 *
 * @param state NUL-terminated token (may be NULL).
 * @return true if @p state is one of the recognized OTA_STATE_* values.
 */
bool ota_state_is_valid(const char *state);

/**
 * @brief Server-owned commit: called at registration with the device's reported
 *        version.  If it matches the in-flight target, marks the update success.
 */
void ota_finalize_on_register(const char *uuid, const char *reported_version);

#ifdef __cplusplus
}
#endif

#endif /* OTA_H */
