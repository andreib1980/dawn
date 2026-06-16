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
 * Schema migration v67: add conversations.context_watermark_msg_id and unlock
 * legacy split-archived conversations.
 *
 * The compaction-watermark model replaces fork-on-compaction: instead of
 * archiving a conversation and creating a continuation row, compaction now
 * records a watermark (the last compacted message id) on the SAME row, and
 * reload bounds context to messages after the watermark.  This migration:
 *   1. Adds `context_watermark_msg_id INTEGER NOT NULL DEFAULT 0` (0 = never
 *      compacted -> load all, the pre-existing behavior; the zero-risk gate).
 *   2. One-time unlocks every conversation that the old split path archived.
 *      `is_archived = 1` was only ever written by conv_db_create_continuation
 *      (the split), so clearing it makes those (now read-only) conversations
 *      writable again.  `continued_from` is left intact as a historical
 *      breadcrumb for sidebar chain rendering.
 *
 * ALTER TABLE ADD COLUMN is NOT idempotent (it errors on a duplicate column),
 * and the migration ladder hard-gates the schema_version bump on this function
 * returning AUTH_DB_SUCCESS, so we probe PRAGMA table_info first and skip the
 * ALTER when the column already exists.  A literal DEFAULT keeps SQLite on the
 * fast ALTER path (no table rewrite).
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <sqlite3.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "logging.h"

/* True if @col exists on @table. @table is a fixed literal (not user input);
 * PRAGMA cannot be parameterized, so it is interpolated directly. */
static bool conv_column_exists(sqlite3 *db, const char *table, const char *col) {
   char sql[128];
   snprintf(sql, sizeof(sql), "PRAGMA table_info(%s)", table);
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
      return false;
   }
   bool found = false;
   while (sqlite3_step(st) == SQLITE_ROW) {
      const unsigned char *name = sqlite3_column_text(st, 1); /* col 1 = column name */
      if (name != NULL && strcmp((const char *)name, col) == 0) {
         found = true;
         break;
      }
   }
   sqlite3_finalize(st);
   return found;
}

int auth_db_migrations_v67(sqlite3 *db) {
   if (db == NULL) {
      return AUTH_DB_FAILURE;
   }

   if (!conv_column_exists(db, "conversations", "context_watermark_msg_id")) {
      char *errmsg = NULL;
      if (sqlite3_exec(db,
                       "ALTER TABLE conversations ADD COLUMN "
                       "context_watermark_msg_id INTEGER NOT NULL DEFAULT 0",
                       NULL, NULL, &errmsg) != SQLITE_OK) {
         OLOG_ERROR("auth_db: v67 ALTER (context_watermark_msg_id) failed: %s",
                    errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         return AUTH_DB_FAILURE;
      }
   }

   /* One-time unlock of legacy split-archived conversations.  Harmless to
    * re-run (no rows match once cleared); runs only in the < v67 block. */
   char *errmsg = NULL;
   if (sqlite3_exec(db, "UPDATE conversations SET is_archived = 0 WHERE is_archived = 1", NULL,
                    NULL, &errmsg) != SQLITE_OK) {
      OLOG_ERROR("auth_db: v67 unlock (is_archived) failed: %s", errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      return AUTH_DB_FAILURE;
   }

   return AUTH_DB_SUCCESS;
}
