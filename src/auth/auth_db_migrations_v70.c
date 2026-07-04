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
 * Schema migration v70: case-insensitive uniqueness for code_projects.name.
 *
 * The code-project lookups (get_by_name / get_visible_by_name) and the import
 * duplicate-check match names COLLATE NOCASE, so the application treats "dawn"
 * and "DAWN" as the same project.  The base UNIQUE(name) constraint, however,
 * is BINARY — so a *concurrent* import of two case-variants could slip past the
 * racy fast-path check and land both rows, breaking the case-unique invariant
 * the read path assumes.  This migration adds the authoritative guard: a UNIQUE
 * INDEX over name COLLATE NOCASE.
 *
 * IF NOT EXISTS keeps it idempotent (and covers fresh installs, which run the
 * ladder from 0).  Only a genuine data collision (SQLITE_CONSTRAINT — a legacy
 * DB already holding two case-variant names) is treated as non-fatal: we log it
 * and advance the version anyway, since retrying can never succeed until the
 * operator removes the duplicate, and the NOCASE lookups keep working meanwhile.
 * A transient failure (BUSY / IOERR / disk-full) instead returns FAILURE so the
 * migration ladder leaves the version at 69 and retries on the next boot rather
 * than permanently forfeiting the guard.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <sqlite3.h>
#include <stdbool.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "logging.h"

/* True if @table exists. @table is a fixed literal (not user input). */
static bool table_exists(sqlite3 *db, const char *table) {
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?", -1, &st,
                          NULL) != SQLITE_OK) {
      return false;
   }
   sqlite3_bind_text(st, 1, table, -1, SQLITE_STATIC);
   bool found = (sqlite3_step(st) == SQLITE_ROW);
   sqlite3_finalize(st);
   return found;
}

int auth_db_migrations_v70(sqlite3 *db) {
   if (db == NULL) {
      return AUTH_DB_FAILURE;
   }

   /* code_projects is created by v65, so any DB at >= v65 has it; a missing table
    * cannot happen in production. If it is absent anyway (a corrupt DB, or a
    * minimal test seed that skipped v65), the index is moot — skip and advance
    * rather than fail, since retrying can never create the table. */
   if (!table_exists(db, "code_projects")) {
      OLOG_WARNING("auth_db: v70 skipped — code_projects table absent (nothing to index)");
      return AUTH_DB_SUCCESS;
   }

   char *errmsg = NULL;
   int rc = sqlite3_exec(db,
                         "CREATE UNIQUE INDEX IF NOT EXISTS idx_code_projects_name_nocase "
                         "ON code_projects(name COLLATE NOCASE)",
                         NULL, NULL, &errmsg);
   if (rc == SQLITE_CONSTRAINT) {
      /* Pre-existing case-variant collision — the only non-fatal cause (retrying
       * can't help until the operator removes the duplicate). Advance anyway. */
      OLOG_WARNING("auth_db: v70 NOCASE unique index not created — pre-existing case-variant "
                   "project-name collision: %s. NOCASE lookups still work; remove the "
                   "duplicate to restore the DB-level guard.",
                   errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
   } else if (rc != SQLITE_OK) {
      /* Transient (BUSY / IOERR / disk-full): fail so the ladder keeps the version
       * at 69 and retries next boot rather than permanently losing the guard. */
      OLOG_ERROR("auth_db: v70 NOCASE unique index creation failed: %s",
                 errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      return AUTH_DB_FAILURE;
   }

   return AUTH_DB_SUCCESS;
}
