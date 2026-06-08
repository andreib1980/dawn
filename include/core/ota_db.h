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
 * OTA device-state DB layer — per-satellite OTA update state, keyed by uuid.
 *
 * Accesses the shared auth_db handle (s_db) directly, same pattern as
 * scheduler_db.c / missed_notifications_db.c.  All functions acquire the
 * auth_db mutex.  See docs/OTA_DESIGN.md §5.
 */

#ifndef OTA_DB_H
#define OTA_DB_H

#include <time.h>

#include "auth/auth_db.h" /* SATELLITE_UUID_MAX, AUTH_DB_* return codes */

#ifdef __cplusplus
extern "C" {
#endif

/* Semantic version string (e.g., "2.0.0").  Generous for pre-release suffixes. */
#define OTA_VERSION_MAX 32
/* State token: idle/offered/downloading/verifying/applying/rebooting/success/failed/
 * offer_pending/unknown. */
#define OTA_STATE_MAX 16
/* Last-error detail (human-readable). */
#define OTA_ERROR_MAX 128

/**
 * @brief In-memory view of an ota_device_state row.
 */
typedef struct {
   char uuid[SATELLITE_UUID_MAX];
   char current_version[OTA_VERSION_MAX]; /**< Device-reported running version */
   char target_version[OTA_VERSION_MAX];  /**< Pushed target ("" when none) */
   char state[OTA_STATE_MAX];             /**< OTA state-machine token */
   char last_error[OTA_ERROR_MAX];        /**< Last failure detail ("" when none) */
   time_t created_at;
   time_t updated_at;
} ota_device_state_t;

/**
 * @brief Record the firmware version a device reported at registration.
 *
 * Upsert keyed by uuid: creates the row (state 'idle') if absent, otherwise
 * updates current_version + updated_at, leaving any in-flight target/state
 * untouched (the register-time finalize check lives in the OTA engine, not
 * here).  Empty/NULL version is ignored (returns SUCCESS, no write).
 *
 * @param uuid     Satellite UUID (must be non-empty)
 * @param version  Reported firmware version string
 * @return AUTH_DB_SUCCESS on success, AUTH_DB_FAILURE on error
 */
int ota_db_report_version(const char *uuid, const char *version);

/**
 * @brief Fetch the OTA state row for a device.
 *
 * @param uuid  Satellite UUID
 * @param out   Output (zeroed then filled on success)
 * @return AUTH_DB_SUCCESS if found, AUTH_DB_NOT_FOUND if no row, AUTH_DB_FAILURE on error
 */
int ota_db_get(const char *uuid, ota_device_state_t *out);

/* OTA state-machine tokens (DB is the single source of truth — §5).  In-flight
 * states are the ones a fresh push must not interrupt. */
#define OTA_STATE_IDLE "idle"
#define OTA_STATE_OFFERED "offered"
#define OTA_STATE_DOWNLOADING "downloading"
#define OTA_STATE_VERIFYING "verifying"
#define OTA_STATE_APPLYING "applying"
#define OTA_STATE_REBOOTING "rebooting"
#define OTA_STATE_SUCCESS "success"
#define OTA_STATE_FAILED "failed"
#define OTA_STATE_UNKNOWN "unknown"

/**
 * @brief Transition a device's OTA state (e.g. from an ota_status report).
 * @param uuid       Satellite UUID (row must exist)
 * @param state      New state token (OTA_STATE_*)
 * @param last_error Optional error detail (NULL/"" clears it)
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int ota_db_set_state(const char *uuid, const char *state, const char *last_error);

/**
 * @brief Begin an update offer for a device — SINGLE-FLIGHT.
 *
 * Records target_version + a one-time download token (with expiry) and moves the
 * device to 'offered', but ONLY if it is not already mid-update.  Rejects with
 * AUTH_DB_LOCKED when an offer/download/apply is already in flight.
 *
 * @return AUTH_DB_SUCCESS if claimed, AUTH_DB_LOCKED if already in-flight,
 *         AUTH_DB_FAILURE on error
 */
int ota_db_begin_offer(const char *uuid,
                       const char *target_version,
                       const char *token,
                       time_t token_expires);

/**
 * @brief Validate + consume the one-time download token (gates the HTTPS pull).
 *
 * Succeeds only if the token matches, has not expired, and matches the device's
 * current target_version; the token is cleared on success (single use).
 *
 * @return AUTH_DB_SUCCESS if valid (and consumed), AUTH_DB_FAILURE otherwise
 */
int ota_db_consume_token(const char *uuid, const char *version, const char *token, time_t now);

/**
 * @brief Clear target/token and return to idle (on success-finalize or abort).
 */
int ota_db_clear_target(const char *uuid, const char *final_state);

/**
 * @brief Reconcile transient states orphaned by a daemon restart.
 *
 * Rows stuck in an in-flight state with updated_at older than @p stale_before
 * are moved to 'unknown' (reconciled when the device reconnects + reports).
 * @param reconciled_out Optional count of rows touched
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int ota_db_reconcile_stale(time_t stale_before, int *reconciled_out);

#ifdef __cplusplus
}
#endif

#endif /* OTA_DB_H */
