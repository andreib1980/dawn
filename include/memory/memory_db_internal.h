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
 * Internal header shared by memory_db*.c TUs.
 *
 * Phase 6 source split — these helpers were `static` in memory_db.c before
 * the file was decomposed into per-noun sources (facts / summaries / prefs /
 * entities / relations).  Promoted to non-static so the new TUs can share
 * them without duplicating the code.  NOT a public API — callers outside
 * src/memory/memory_db*.c must not include this header.
 *
 * All helpers in this header assume the caller holds AUTH_DB_LOCK except
 * where noted.  See memory_db.c for the canonical implementations.
 */
#ifndef DAWN_MEMORY_DB_INTERNAL_H
#define DAWN_MEMORY_DB_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "memory/memory_db.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations to avoid pulling in sqlite3.h at every include site. */
struct sqlite3_stmt;

/* Bind provenance columns to sequential params at (base, base+1, base+2).
 * Emits SQLITE_NULL for all three when prov is NULL or prov->conv_id <= 0.
 * Caller must hold AUTH_DB_LOCK (operates on a prepared statement). */
void memory_db_internal_bind_provenance(struct sqlite3_stmt *stmt,
                                        int base,
                                        const memory_provenance_t *prov);

/* Build a LIKE pattern wrapping `keywords` with `%` and escaping the LIKE
 * metacharacters %, _, \\ with backslash.  Output is NUL-terminated even on
 * overflow.  Pure helper — no locks involved. */
void memory_db_internal_build_like_pattern(const char *keywords, char *out_pattern, size_t max_len);

/* Lightweight FK existence check (diagnostic — used by the relation_supersede
 * FK probe to pinpoint which FK fired on SQLITE_CONSTRAINT_FOREIGNKEY).
 * Returns 1 if a row exists, 0 if missing, -1 if the prepare itself failed.
 * The -1 is intentionally not SUCCESS/FAILURE — this is diagnostic-only and
 * the caller logs it verbatim ("-1=check_failed").  Caller must hold
 * AUTH_DB_LOCK.
 *
 * `user_id_scope`: when > 0, the probe also requires user_id = user_id_scope
 * (CWE-209 close — without this, the probe would report "subj_ok=1" for an
 * entity belonging to a DIFFERENT user, misleading operator triage and
 * weakly leaking cross-user row existence into the FK error log).  When 0
 * (legacy unscoped), the probe matches any user — appropriate for the
 * `users` table where the row itself IS the user. */
int memory_db_internal_fk_row_exists(const char *table, int64_t id, int user_id_scope);

#ifdef __cplusplus
}
#endif

#endif /* DAWN_MEMORY_DB_INTERNAL_H */
