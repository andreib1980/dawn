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
 * Schema migration v69: add conversations.is_pinned.
 *
 * Pinned conversations float to a dedicated section at the top of the WebUI
 * conversation list, separate from the date-grouped list.  This migration:
 *   1. Adds `is_pinned INTEGER NOT NULL DEFAULT 0` (0 = not pinned, the
 *      pre-existing behavior; the zero-risk default).
 *   2. Creates idx_conversations_pinned(user_id, is_pinned DESC, updated_at DESC)
 *      so the pinned-first list ordering is served by an index on existing DBs
 *      (the base schema creates the same index for fresh installs).
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

int auth_db_migrations_v69(sqlite3 *db) {
   if (db == NULL) {
      return AUTH_DB_FAILURE;
   }

   if (!conv_column_exists(db, "conversations", "is_pinned")) {
      char *errmsg = NULL;
      if (sqlite3_exec(db,
                       "ALTER TABLE conversations ADD COLUMN "
                       "is_pinned INTEGER NOT NULL DEFAULT 0",
                       NULL, NULL, &errmsg) != SQLITE_OK) {
         OLOG_ERROR("auth_db: v69 ALTER (is_pinned) failed: %s", errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         return AUTH_DB_FAILURE;
      }
   }

   /* Index for the pinned-first list ordering.  IF NOT EXISTS makes it safe to
    * re-run and idempotent against fresh DBs (base schema creates it too). */
   char *errmsg = NULL;
   if (sqlite3_exec(db,
                    "CREATE INDEX IF NOT EXISTS idx_conversations_pinned "
                    "ON conversations(user_id, is_pinned DESC, updated_at DESC)",
                    NULL, NULL, &errmsg) != SQLITE_OK) {
      OLOG_ERROR("auth_db: v69 CREATE INDEX (idx_conversations_pinned) failed: %s",
                 errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      return AUTH_DB_FAILURE;
   }

   return AUTH_DB_SUCCESS;
}
