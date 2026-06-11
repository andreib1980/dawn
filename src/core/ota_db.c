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
 * OTA device-state DB layer implementation.
 *
 * Accesses s_db directly (same pattern as missed_notifications_db.c).
 * All functions acquire the auth_db mutex.
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include "core/ota_db.h"

#include <string.h>
#include <time.h>

#include "auth/auth_db_internal.h"
#include "logging.h"

static void copy_text_col(char *dst, size_t dst_size, sqlite3_stmt *stmt, int col) {
   if (dst_size == 0)
      return;
   const unsigned char *text = sqlite3_column_text(stmt, col);
   if (!text) {
      dst[0] = '\0';
      return;
   }
   int n = sqlite3_column_bytes(stmt, col);
   size_t copy = ((size_t)n < dst_size - 1) ? (size_t)n : dst_size - 1;
   memcpy(dst, text, copy);
   dst[copy] = '\0';
}

int ota_db_report_version(const char *uuid, const char *version) {
   if (!uuid || uuid[0] == '\0')
      return AUTH_DB_FAILURE;
   /* No version reported (e.g., legacy firmware) — nothing to record. */
   if (!version || version[0] == '\0')
      return AUTH_DB_SUCCESS;

   AUTH_DB_LOCK_OR_FAIL();

   /* Upsert: create the row on first sighting, otherwise refresh the reported
    * version + timestamp.  ON CONFLICT leaves target_version/state/last_error
    * alone so an in-flight update is not clobbered by a routine re-registration. */
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO ota_device_state (uuid, current_version, state, created_at, updated_at) "
       "VALUES (?, ?, 'idle', ?, ?) "
       "ON CONFLICT(uuid) DO UPDATE SET current_version = excluded.current_version, "
       "updated_at = excluded.updated_at",
       -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("ota_db: prepare report_version failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   /* Clamp the (satellite-supplied at registration) version to the column width
    * at the DB chokepoint — same rationale as ota_db_set_state's error clamp. */
   char ver_clamped[OTA_VERSION_MAX];
   snprintf(ver_clamped, sizeof(ver_clamped), "%s", version);
   int64_t now = (int64_t)time(NULL);
   sqlite3_bind_text(stmt, 1, uuid, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, ver_clamped, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int64(stmt, 3, now);
   sqlite3_bind_int64(stmt, 4, now);

   rc = sqlite3_step(stmt);
   char errbuf[128];
   if (rc != SQLITE_DONE)
      snprintf(errbuf, sizeof(errbuf), "%s", sqlite3_errmsg(s_db.db));
   sqlite3_finalize(stmt);

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("ota_db: report_version step failed: %s", errbuf);
      return AUTH_DB_FAILURE;
   }
   return AUTH_DB_SUCCESS;
}

int ota_db_get(const char *uuid, ota_device_state_t *out) {
   if (!uuid || uuid[0] == '\0' || !out)
      return AUTH_DB_FAILURE;

   memset(out, 0, sizeof(*out));

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT uuid, current_version, target_version, target_platform, state, last_error, "
       "created_at, updated_at FROM ota_device_state WHERE uuid = ?",
       -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("ota_db: prepare get failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   sqlite3_bind_text(stmt, 1, uuid, -1, SQLITE_TRANSIENT);

   int result;
   rc = sqlite3_step(stmt);
   if (rc == SQLITE_ROW) {
      copy_text_col(out->uuid, sizeof(out->uuid), stmt, 0);
      copy_text_col(out->current_version, sizeof(out->current_version), stmt, 1);
      copy_text_col(out->target_version, sizeof(out->target_version), stmt, 2);
      copy_text_col(out->target_platform, sizeof(out->target_platform), stmt, 3);
      copy_text_col(out->state, sizeof(out->state), stmt, 4);
      copy_text_col(out->last_error, sizeof(out->last_error), stmt, 5);
      out->created_at = (time_t)sqlite3_column_int64(stmt, 6);
      out->updated_at = (time_t)sqlite3_column_int64(stmt, 7);
      result = AUTH_DB_SUCCESS;
   } else if (rc == SQLITE_DONE) {
      result = AUTH_DB_NOT_FOUND;
   } else {
      OLOG_ERROR("ota_db: get step failed: %s", sqlite3_errmsg(s_db.db));
      result = AUTH_DB_FAILURE;
   }

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return result;
}

int ota_db_set_state(const char *uuid, const char *state, const char *last_error) {
   if (!uuid || uuid[0] == '\0' || !state || state[0] == '\0') {
      return AUTH_DB_FAILURE;
   }

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(
       s_db.db,
       "UPDATE ota_device_state SET state = ?, last_error = ?, updated_at = ? WHERE uuid = ?", -1,
       &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("ota_db: prepare set_state failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   /* Clamp the (possibly satellite-supplied) free-text error to the column width
    * here at the DB chokepoint, so every caller — the webui status/reject paths
    * and the internal failure path in ota.c — gets one enforced bound rather than
    * each transport seam re-clamping.  (The parallel state-token *validity* check
    * stays at the trust boundary: ota_state_is_valid in webui_ota.c.) */
   sqlite3_bind_text(stmt, 1, state, -1, SQLITE_TRANSIENT);
   if (last_error && last_error[0]) {
      char err_clamped[OTA_ERROR_MAX];
      snprintf(err_clamped, sizeof(err_clamped), "%s", last_error);
      sqlite3_bind_text(stmt, 2, err_clamped, -1, SQLITE_TRANSIENT);
   } else {
      sqlite3_bind_null(stmt, 2);
   }
   sqlite3_bind_int64(stmt, 3, (int64_t)time(NULL));
   sqlite3_bind_text(stmt, 4, uuid, -1, SQLITE_TRANSIENT);

   rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return (rc == SQLITE_DONE) ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
}

/* In-flight states a fresh offer must not interrupt (single-flight guard). */
#define OTA_INFLIGHT_PREDICATE \
   "state IN ('offered', 'downloading', 'verifying', 'applying', 'rebooting')"

int ota_db_begin_offer(const char *uuid,
                       const char *target_version,
                       const char *target_platform,
                       const char *token,
                       time_t token_expires) {
   if (!uuid || uuid[0] == '\0' || !target_version || !target_platform || !token) {
      return AUTH_DB_FAILURE;
   }

   AUTH_DB_LOCK_OR_FAIL();
   int64_t now = (int64_t)time(NULL);

   /* Ensure a row exists (idle) without disturbing an existing one. */
   sqlite3_stmt *ins = NULL;
   int rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO ota_device_state (uuid, current_version, state, created_at, updated_at) "
       "VALUES (?, '', 'idle', ?, ?) ON CONFLICT(uuid) DO NOTHING",
       -1, &ins, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("ota_db: prepare begin_offer(insert) failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_text(ins, 1, uuid, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int64(ins, 2, now);
   sqlite3_bind_int64(ins, 3, now);
   rc = sqlite3_step(ins);
   sqlite3_finalize(ins);
   if (rc != SQLITE_DONE) {
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   /* Conditional claim: only if not already mid-update. */
   sqlite3_stmt *upd = NULL;
   rc = sqlite3_prepare_v2(
       s_db.db,
       "UPDATE ota_device_state SET target_version = ?, target_platform = ?, state = 'offered', "
       "token = ?, token_expires = ?, last_error = NULL, updated_at = ? "
       "WHERE uuid = ? AND NOT (" OTA_INFLIGHT_PREDICATE ")",
       -1, &upd, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("ota_db: prepare begin_offer(update) failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_text(upd, 1, target_version, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(upd, 2, target_platform, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(upd, 3, token, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int64(upd, 4, (int64_t)token_expires);
   sqlite3_bind_int64(upd, 5, now);
   sqlite3_bind_text(upd, 6, uuid, -1, SQLITE_TRANSIENT);
   rc = sqlite3_step(upd);
   int changes = (rc == SQLITE_DONE) ? sqlite3_changes(s_db.db) : -1;
   sqlite3_finalize(upd);
   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      return AUTH_DB_FAILURE;
   }
   return (changes == 1) ? AUTH_DB_SUCCESS : AUTH_DB_LOCKED;
}

int ota_db_consume_token(const char *uuid,
                         const char *platform,
                         const char *version,
                         const char *token,
                         time_t now) {
   if (!uuid || uuid[0] == '\0' || !platform || platform[0] == '\0' || !version || !token ||
       token[0] == '\0') {
      return AUTH_DB_FAILURE;
   }

   AUTH_DB_LOCK_OR_FAIL();

   /* Single-use: clear the token and advance to downloading, but only if it
    * matches, is unexpired, and matches the current target_version AND
    * target_platform (so a token issued for rpi/X can't fetch esp32/X).  The
    * token is high-entropy + single-use + TTL-bound, so SQL equality is adequate. */
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(
       s_db.db,
       "UPDATE ota_device_state SET token = NULL, state = 'downloading', updated_at = ? "
       "WHERE uuid = ? AND token IS NOT NULL AND token = ? AND target_version = ? "
       "AND target_platform = ? AND token_expires >= ?",
       -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("ota_db: prepare consume_token failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int64(stmt, 1, (int64_t)now);
   sqlite3_bind_text(stmt, 2, uuid, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, token, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 4, version, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 5, platform, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int64(stmt, 6, (int64_t)now);
   rc = sqlite3_step(stmt);
   int changes = (rc == SQLITE_DONE) ? sqlite3_changes(s_db.db) : -1;
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      return AUTH_DB_FAILURE;
   }
   return (changes == 1) ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
}

int ota_db_clear_target(const char *uuid, const char *final_state) {
   if (!uuid || uuid[0] == '\0') {
      return AUTH_DB_FAILURE;
   }
   const char *state = (final_state && final_state[0]) ? final_state : OTA_STATE_IDLE;

   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(
       s_db.db,
       "UPDATE ota_device_state SET target_version = NULL, target_platform = NULL, token = NULL, "
       "token_expires = NULL, state = ?, updated_at = ? WHERE uuid = ?",
       -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("ota_db: prepare clear_target failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_text(stmt, 1, state, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int64(stmt, 2, (int64_t)time(NULL));
   sqlite3_bind_text(stmt, 3, uuid, -1, SQLITE_TRANSIENT);
   rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return (rc == SQLITE_DONE) ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
}

int ota_db_reconcile_stale(time_t stale_before, int *reconciled_out) {
   if (reconciled_out) {
      *reconciled_out = 0;
   }

   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "UPDATE ota_device_state SET state = 'unknown', updated_at = ? "
                               "WHERE (" OTA_INFLIGHT_PREDICATE ") AND updated_at < ?",
                               -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("ota_db: prepare reconcile failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int64(stmt, 1, (int64_t)time(NULL));
   sqlite3_bind_int64(stmt, 2, (int64_t)stale_before);
   rc = sqlite3_step(stmt);
   int changes = (rc == SQLITE_DONE) ? sqlite3_changes(s_db.db) : 0;
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      return AUTH_DB_FAILURE;
   }
   if (reconciled_out) {
      *reconciled_out = changes;
   }
   return AUTH_DB_SUCCESS;
}
