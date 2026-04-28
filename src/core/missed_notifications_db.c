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
 * Missed Notifications DB Layer implementation.
 *
 * Accesses s_db directly (same pattern as scheduler_db.c).
 * All functions acquire the auth_db mutex.
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include "core/missed_notifications_db.h"

#include <string.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "logging.h"

#define MISSED_SELECT_COLS                                                                \
   "id, user_id, event_id, event_type, status, name, message, fire_at, conversation_id, " \
   "created_at"

/* Batch size for expiration deletes (keeps the DB lock short under large backlogs). */
#define MISSED_NOTIF_EXPIRE_BATCH 500

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

static void extract_row(sqlite3_stmt *stmt, missed_notif_t *m) {
   m->id = sqlite3_column_int64(stmt, 0);
   m->user_id = sqlite3_column_int(stmt, 1);
   m->event_id = sqlite3_column_int64(stmt, 2);
   copy_text_col(m->event_type, sizeof(m->event_type), stmt, 3);
   copy_text_col(m->status, sizeof(m->status), stmt, 4);
   copy_text_col(m->name, sizeof(m->name), stmt, 5);
   copy_text_col(m->message, sizeof(m->message), stmt, 6);
   m->fire_at = (time_t)sqlite3_column_int64(stmt, 7);
   m->conversation_id = sqlite3_column_int64(stmt, 8);
   m->created_at = (time_t)sqlite3_column_int64(stmt, 9);
}

int missed_notif_insert(int user_id,
                        int64_t event_id,
                        const char *event_type,
                        const char *status,
                        const char *name,
                        const char *message,
                        time_t fire_at,
                        int64_t conversation_id) {
   if (user_id <= 0 || !event_type || !status || !name)
      return AUTH_DB_FAILURE;

   AUTH_DB_LOCK_OR_FAIL();

   /* Atomic cap-enforced insert: the row is only inserted if the user's queue
    * is below MISSED_NOTIF_MAX_PER_USER. Using INSERT ... SELECT ... WHERE
    * keeps count and insert in a single statement — no race window between
    * them and one less round-trip than COUNT + INSERT. */
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO missed_notifications "
       "(user_id, event_id, event_type, status, name, message, "
       " fire_at, conversation_id, created_at) "
       "SELECT ?, ?, ?, ?, ?, ?, ?, ?, ? "
       "WHERE (SELECT COUNT(*) FROM missed_notifications WHERE user_id = ?) < ?",
       -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("missed_notif: prepare insert failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, event_id);
   sqlite3_bind_text(stmt, 3, event_type, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 4, status, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 5, name, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 6, message ? message : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_int64(stmt, 7, (int64_t)fire_at);
   sqlite3_bind_int64(stmt, 8, conversation_id);
   sqlite3_bind_int64(stmt, 9, (int64_t)time(NULL));
   sqlite3_bind_int(stmt, 10, user_id);
   sqlite3_bind_int(stmt, 11, MISSED_NOTIF_MAX_PER_USER);

   rc = sqlite3_step(stmt);
   int changes = (rc == SQLITE_DONE) ? sqlite3_changes(s_db.db) : -1;
   /* Capture error message while the lock is still held (prevents a concurrent
    * writer from overwriting the per-connection error state). */
   char errbuf[128];
   if (rc != SQLITE_DONE) {
      snprintf(errbuf, sizeof(errbuf), "%s", sqlite3_errmsg(s_db.db));
   }
   sqlite3_finalize(stmt);

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("missed_notif: insert step failed: %s", errbuf);
      return AUTH_DB_FAILURE;
   }
   if (changes == 0) {
      OLOG_WARNING("missed_notif: user %d at cap (%d), dropping event %lld", user_id,
                   MISSED_NOTIF_MAX_PER_USER, (long long)event_id);
      return AUTH_DB_FAILURE;
   }
   return AUTH_DB_SUCCESS;
}

int missed_notif_get_for_user(int user_id, int max_count, missed_notif_t *out, int *count_out) {
   if (count_out)
      *count_out = 0;

   if (!out || max_count <= 0 || user_id <= 0)
      return AUTH_DB_FAILURE;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "SELECT " MISSED_SELECT_COLS " FROM missed_notifications "
                               "WHERE user_id = ? ORDER BY created_at ASC LIMIT ?",
                               -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("missed_notif: prepare get failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int(stmt, 2, max_count);

   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && count < max_count) {
      extract_row(stmt, &out[count]);
      count++;
   }
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = count;
   return AUTH_DB_SUCCESS;
}

int missed_notif_delete_by_user(int64_t id, int user_id) {
   if (id <= 0 || user_id <= 0)
      return AUTH_DB_FAILURE;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "DELETE FROM missed_notifications WHERE id = ? AND user_id = ?", -1,
                               &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("missed_notif: prepare delete failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   sqlite3_bind_int64(stmt, 1, id);
   sqlite3_bind_int(stmt, 2, user_id);
   rc = sqlite3_step(stmt);
   int changes = (rc == SQLITE_DONE) ? sqlite3_changes(s_db.db) : -1;
   sqlite3_finalize(stmt);

   AUTH_DB_UNLOCK();

   /* Log zero-row deletes for server-side audit visibility (ID guessing probes,
    * stale clients). The caller's response is unchanged so the client cannot
    * distinguish "not owner" from "already deleted". */
   if (rc == SQLITE_DONE && changes == 0) {
      OLOG_INFO("missed_notif: dismiss by user %d for id %lld matched no row", user_id,
                (long long)id);
   }
   return (rc == SQLITE_DONE) ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
}

int missed_notif_delete_all_for_user(int user_id, int *deleted_out) {
   if (deleted_out)
      *deleted_out = 0;

   if (user_id <= 0)
      return AUTH_DB_FAILURE;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM missed_notifications WHERE user_id = ?", -1,
                               &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   rc = sqlite3_step(stmt);
   int deleted = (rc == SQLITE_DONE) ? sqlite3_changes(s_db.db) : 0;
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE)
      return AUTH_DB_FAILURE;

   if (deleted_out)
      *deleted_out = deleted;
   return AUTH_DB_SUCCESS;
}

int missed_notif_expire(int max_age_sec, int *deleted_out) {
   if (deleted_out)
      *deleted_out = 0;

   if (max_age_sec <= 0)
      return AUTH_DB_FAILURE;

   AUTH_DB_LOCK_OR_FAIL();

   time_t cutoff = time(NULL) - (time_t)max_age_sec;
   int total_deleted = 0;

   /* Batched deletes to avoid long DB locks.
    * Loop until a batch returns fewer rows than the batch size. */
   for (;;) {
      sqlite3_stmt *stmt = NULL;
      int rc = sqlite3_prepare_v2(s_db.db,
                                  "DELETE FROM missed_notifications WHERE id IN "
                                  "(SELECT id FROM missed_notifications WHERE created_at < ? "
                                  " LIMIT ?)",
                                  -1, &stmt, NULL);
      if (rc != SQLITE_OK) {
         OLOG_ERROR("missed_notif: prepare expire failed: %s", sqlite3_errmsg(s_db.db));
         AUTH_DB_UNLOCK();
         return AUTH_DB_FAILURE;
      }
      sqlite3_bind_int64(stmt, 1, (int64_t)cutoff);
      sqlite3_bind_int(stmt, 2, MISSED_NOTIF_EXPIRE_BATCH);

      rc = sqlite3_step(stmt);
      int batch_deleted = (rc == SQLITE_DONE) ? sqlite3_changes(s_db.db) : 0;
      sqlite3_finalize(stmt);

      if (rc != SQLITE_DONE) {
         AUTH_DB_UNLOCK();
         return AUTH_DB_FAILURE;
      }
      total_deleted += batch_deleted;
      if (batch_deleted < MISSED_NOTIF_EXPIRE_BATCH)
         break;
   }

   AUTH_DB_UNLOCK();
   if (deleted_out)
      *deleted_out = total_deleted;
   return AUTH_DB_SUCCESS;
}
