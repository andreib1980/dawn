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
 * Schema migration v68: generic `blobs` table (original-file storage) +
 * `documents.original_blob_id` link.
 *
 * The blob store keeps original uploaded files on disk with metadata here, so a
 * document's source PDF/DOCX survives text extraction (re-extraction, OCR, the
 * media gallery).  `blobs` dedups per user on content_hash.  `documents` gains
 * an `original_blob_id` pointer to the source blob.
 *
 * SQLite cannot ALTER-add a FOREIGN KEY without a full table rebuild, so on a
 * migrated DB `original_blob_id` is a plain TEXT column and the link is enforced
 * in C; fresh installs declare the real FK in the base schema.  CREATE TABLE/
 * INDEX are IF NOT EXISTS (idempotent); the ALTER is probe-guarded like v67.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <sqlite3.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "logging.h"

/* True if @col exists on @table. @table is a fixed literal (not user input). */
static bool v68_column_exists(sqlite3 *db, const char *table, const char *col) {
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

int auth_db_migrations_v68(sqlite3 *db) {
   if (db == NULL) {
      return AUTH_DB_FAILURE;
   }

   char *errmsg = NULL;

   /* 1. Generic blobs table + indexes (idempotent). */
   if (sqlite3_exec(db,
                    "CREATE TABLE IF NOT EXISTS blobs ("
                    "  id TEXT PRIMARY KEY,"
                    "  user_id INTEGER NOT NULL,"
                    "  kind INTEGER NOT NULL DEFAULT 0,"
                    "  retention_policy INTEGER NOT NULL DEFAULT 0,"
                    "  content_hash TEXT NOT NULL,"
                    "  mime_type TEXT NOT NULL,"
                    "  size INTEGER NOT NULL,"
                    "  filename TEXT NOT NULL,"
                    "  filename_original TEXT,"
                    "  created_at INTEGER NOT NULL,"
                    "  last_accessed INTEGER,"
                    "  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE"
                    ");"
                    "CREATE INDEX IF NOT EXISTS idx_blobs_user ON blobs(user_id);"
                    "CREATE INDEX IF NOT EXISTS idx_blobs_created ON blobs(created_at);"
                    "CREATE UNIQUE INDEX IF NOT EXISTS idx_blobs_user_hash "
                    "ON blobs(user_id, content_hash);",
                    NULL, NULL, &errmsg) != SQLITE_OK) {
      OLOG_ERROR("auth_db: v68 blobs table failed: %s", errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      return AUTH_DB_FAILURE;
   }

   /* 2. documents.original_blob_id (probe-guarded ALTER; FK enforced in C on
    *    migrated DBs — fresh installs get the real FK in the base schema). */
   if (!v68_column_exists(db, "documents", "original_blob_id")) {
      if (sqlite3_exec(db, "ALTER TABLE documents ADD COLUMN original_blob_id TEXT DEFAULT NULL",
                       NULL, NULL, &errmsg) != SQLITE_OK) {
         OLOG_ERROR("auth_db: v68 ALTER (original_blob_id) failed: %s",
                    errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         return AUTH_DB_FAILURE;
      }
   }

   /* 3. Index for the orphan sweep's reverse lookup (documents referencing a blob). */
   if (sqlite3_exec(db,
                    "CREATE INDEX IF NOT EXISTS idx_documents_original_blob "
                    "ON documents(original_blob_id);",
                    NULL, NULL, &errmsg) != SQLITE_OK) {
      OLOG_ERROR("auth_db: v68 index (idx_documents_original_blob) failed: %s",
                 errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      return AUTH_DB_FAILURE;
   }

   return AUTH_DB_SUCCESS;
}
