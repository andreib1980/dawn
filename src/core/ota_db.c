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

   int64_t now = (int64_t)time(NULL);
   sqlite3_bind_text(stmt, 1, uuid, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, version, -1, SQLITE_TRANSIENT);
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
   int rc = sqlite3_prepare_v2(s_db.db,
                               "SELECT uuid, current_version, target_version, state, last_error, "
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
      copy_text_col(out->state, sizeof(out->state), stmt, 3);
      copy_text_col(out->last_error, sizeof(out->last_error), stmt, 4);
      out->created_at = (time_t)sqlite3_column_int64(stmt, 5);
      out->updated_at = (time_t)sqlite3_column_int64(stmt, 6);
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
