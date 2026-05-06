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
 * Memory provenance reader implementations.
 *
 * One single-record reader (`memory_db_fact_get_source`) and four batch
 * readers — one per record type (facts/relations/summaries/preferences) —
 * that return the source-conversation back-link recorded at extraction time.
 *
 * Privacy: each batch query JOINs `conversations c ON ... = c.id` and filters
 * `c.is_private = 0` in SQL, so private-conversation rows return
 * out_conv_ids[i] = 0 in the output array.  The single-record path checks
 * privacy via `conv_db_is_private()` after the lock is released (auth_db
 * mutex is non-reentrant).
 *
 * Truncation safety: each batch builder caps N at MAX_PROVENANCE_BATCH and
 * returns MEMORY_DB_FAILURE on overflow (fail-closed).  The static_assert
 * below ties the cap to the SQL buffer size — change either and the build
 * stops.
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include "memory/memory_db_provenance.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "logging.h"
#include "memory/memory_db.h"

/* SQL buffer for the batch IN-clause builder.  Sized so that
 * MAX_PROVENANCE_BATCH IDs fit with comfortable headroom for the SELECT/JOIN
 * prefix and the ")" suffix.  Keep the cap aligned with this constant. */
#define PROV_SQL_BUF_SIZE 1024

/* Worst-case per-ID encoding: leading "," + up to 20 digits of int64. */
#define PROV_ID_MAX_CHARS 21

/* The fixed SELECT/JOIN prefix and the closing ")" must fit alongside N IDs.
 * Measured prefix sizes (longest table-name variant = "memory_preferences"):
 *   SELECT/JOIN/WHERE/AND ... ≈ 277 bytes; closing ")" + NUL ≈ 2 bytes.
 * Reserve 320 bytes (= 277 + 43 headroom) so a future variant that grows the
 * format string by a few %s still fits.  If you change build_in_clause's
 * format string, re-measure and bump this if needed. */
#define PROV_FIXED_OVERHEAD 320

_Static_assert(MAX_PROVENANCE_BATCH *PROV_ID_MAX_CHARS + PROV_FIXED_OVERHEAD <= PROV_SQL_BUF_SIZE,
               "MAX_PROVENANCE_BATCH × per-ID width + prefix exceeds SQL buffer; "
               "shrink the cap or grow PROV_SQL_BUF_SIZE.");

/* =============================================================================
 * Single-record fact source reader (moved verbatim from memory_db.c).
 * ============================================================================= */

int memory_db_fact_get_source(int64_t fact_id,
                              int user_id,
                              int64_t *conv_id_out,
                              int64_t *start_out,
                              int64_t *end_out) {
   if (!conv_id_out || !start_out || !end_out)
      return MEMORY_DB_FAILURE;
   *conv_id_out = 0;
   *start_out = 0;
   *end_out = 0;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "SELECT user_id, source_conversation_id, "
                               "       source_msg_id_start, source_msg_id_end "
                               "FROM memory_facts WHERE id = ?",
                               -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int64(stmt, 1, fact_id);
   rc = sqlite3_step(stmt);

   if (rc != SQLITE_ROW) {
      sqlite3_finalize(stmt);
      AUTH_DB_UNLOCK();
      return MEMORY_DB_NOT_FOUND;
   }

   int owner = sqlite3_column_int(stmt, 0);
   int col1_type = sqlite3_column_type(stmt, 1);
   int64_t conv_id = (col1_type != SQLITE_NULL) ? sqlite3_column_int64(stmt, 1) : 0;
   int64_t msg_start = (sqlite3_column_type(stmt, 2) != SQLITE_NULL) ? sqlite3_column_int64(stmt, 2)
                                                                     : 0;
   int64_t msg_end = (sqlite3_column_type(stmt, 3) != SQLITE_NULL) ? sqlite3_column_int64(stmt, 3)
                                                                   : 0;
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();

   /* Ownership check */
   if (owner != user_id)
      return MEMORY_DB_NOT_FOUND;

   /* No provenance recorded */
   if (col1_type == SQLITE_NULL || conv_id <= 0)
      return MEMORY_DB_NOT_FOUND;

   /* Privacy check: auth_db mutex is NOT re-entrant, so we must release it before
    * calling conv_db_is_private() which acquires the same mutex.  The race window
    * between the unlock above and this re-acquire is benign: the ownership check
    * already passed using the stored user_id column, and a concurrent privacy flip
    * would affect at most one stale read — acceptable per the design doc. */
   bool is_private = false;
   conv_db_is_private(conv_id, user_id, &is_private);
   if (is_private)
      return MEMORY_DB_NOT_FOUND;

   *conv_id_out = conv_id;
   *start_out = msg_start;
   *end_out = msg_end;
   return MEMORY_DB_SUCCESS;
}

/* =============================================================================
 * Batch source readers — one per record type.
 *
 * All four share the IN-clause SQL build pattern.  `build_in_clause` writes the
 * SELECT into `sql` for table `tbl` (must be a literal string, not user input)
 * with privacy JOIN.  Returns SUCCESS on a successful build, FAILURE on
 * truncation (which is now impossible given the static_assert, but the runtime
 * check is retained as a defense-in-depth fence).
 * ============================================================================= */

static int build_in_clause(char *sql,
                           size_t sql_size,
                           const char *tbl_alias, /* "f", "r", "s", or "p" */
                           const char *tbl_name,  /* table name (string literal) */
                           const int64_t *ids,
                           int n) {
   int off = snprintf(sql, sql_size,
                      "SELECT %s.id, %s.source_conversation_id, "
                      "%s.source_msg_id_start, %s.source_msg_id_end "
                      "FROM %s %s "
                      "JOIN conversations c ON %s.source_conversation_id = c.id "
                      "WHERE %s.user_id = ? AND c.is_private = 0 "
                      "AND %s.source_conversation_id IS NOT NULL "
                      "AND %s.id IN (%lld",
                      tbl_alias, tbl_alias, tbl_alias, tbl_alias, tbl_name, tbl_alias, tbl_alias,
                      tbl_alias, tbl_alias, tbl_alias, (long long)ids[0]);
   if (off < 0 || (size_t)off >= sql_size)
      return MEMORY_DB_FAILURE;
   for (int i = 1; i < n; i++) {
      int written = snprintf(sql + off, sql_size - (size_t)off, ",%lld", (long long)ids[i]);
      if (written < 0 || (size_t)(off + written) >= sql_size - 2)
         return MEMORY_DB_FAILURE;
      off += written;
   }
   if ((size_t)(off + 2) >= sql_size)
      return MEMORY_DB_FAILURE;
   sql[off++] = ')';
   sql[off] = '\0';
   return MEMORY_DB_SUCCESS;
}

/* Process one chunk (n ≤ MAX_PROVENANCE_BATCH).  Output indices are relative
 * to the chunk slice — caller advances output pointers between chunks.
 * Fail-closed if n > cap (the static_assert + this runtime guard form the
 * SQL-truncation defense-in-depth). */
static int batch_get_sources_one(int user_id,
                                 const char *tbl_alias,
                                 const char *tbl_name,
                                 const int64_t *ids,
                                 int n,
                                 int64_t *out_conv_ids,
                                 int64_t *out_starts,
                                 int64_t *out_ends) {
   if (n <= 0 || n > MAX_PROVENANCE_BATCH) {
      OLOG_WARNING("provenance batch oversize: n=%d cap=%d (table=%s); failing closed", n,
                   MAX_PROVENANCE_BATCH, tbl_name);
      return MEMORY_DB_FAILURE;
   }

   for (int i = 0; i < n; i++) {
      out_conv_ids[i] = 0;
      out_starts[i] = 0;
      out_ends[i] = 0;
   }

   /* IDs are int64 values from each table's AUTOINCREMENT column — no injection
    * risk.  Privacy filtered in SQL via JOIN on conversations.is_private. */
   char sql[PROV_SQL_BUF_SIZE];
   if (build_in_clause(sql, sizeof(sql), tbl_alias, tbl_name, ids, n) != MEMORY_DB_SUCCESS)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);

   while (sqlite3_step(stmt) == SQLITE_ROW) {
      int64_t row_id = sqlite3_column_int64(stmt, 0);
      for (int i = 0; i < n; i++) {
         if (ids[i] == row_id) {
            out_conv_ids[i] = sqlite3_column_int64(stmt, 1);
            out_starts[i] = sqlite3_column_int64(stmt, 2);
            out_ends[i] = sqlite3_column_int64(stmt, 3);
            break;
         }
      }
   }
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return MEMORY_DB_SUCCESS;
}

/* Public-facing batch reader — accepts any positive N.  Slices the input into
 * MAX_PROVENANCE_BATCH-sized chunks and runs `batch_get_sources_one` per chunk.
 * Each chunk acquires its own AUTH_DB_LOCK cycle, so an N=500 bench query is
 * ~16 lock cycles — acceptable for a memory-search-cadence operation, well
 * above the typical LLM-tool top-K (≤ 10).
 *
 * The cap on a single SQL statement remains constant (defense-in-depth against
 * snprintf truncation); only the public surface chunks. */
static int batch_get_sources(int user_id,
                             const char *tbl_alias,
                             const char *tbl_name,
                             const int64_t *ids,
                             int n,
                             int64_t *out_conv_ids,
                             int64_t *out_starts,
                             int64_t *out_ends) {
   if (!ids || !out_conv_ids || !out_starts || !out_ends || n <= 0)
      return MEMORY_DB_FAILURE;

   int offset = 0;
   while (offset < n) {
      int chunk = n - offset;
      if (chunk > MAX_PROVENANCE_BATCH)
         chunk = MAX_PROVENANCE_BATCH;
      int rc = batch_get_sources_one(user_id, tbl_alias, tbl_name, ids + offset, chunk,
                                     out_conv_ids + offset, out_starts + offset, out_ends + offset);
      if (rc != MEMORY_DB_SUCCESS)
         return rc;
      offset += chunk;
   }
   return MEMORY_DB_SUCCESS;
}

int memory_db_facts_get_sources(int user_id,
                                const int64_t *fact_ids,
                                int n,
                                int64_t *out_conv_ids,
                                int64_t *out_starts,
                                int64_t *out_ends) {
   return batch_get_sources(user_id, "f", "memory_facts", fact_ids, n, out_conv_ids, out_starts,
                            out_ends);
}

int memory_db_relations_get_sources(int user_id,
                                    const int64_t *relation_ids,
                                    int n,
                                    int64_t *out_conv_ids,
                                    int64_t *out_starts,
                                    int64_t *out_ends) {
   return batch_get_sources(user_id, "r", "memory_relations", relation_ids, n, out_conv_ids,
                            out_starts, out_ends);
}

int memory_db_summaries_get_sources(int user_id,
                                    const int64_t *summary_ids,
                                    int n,
                                    int64_t *out_conv_ids,
                                    int64_t *out_starts,
                                    int64_t *out_ends) {
   return batch_get_sources(user_id, "s", "memory_summaries", summary_ids, n, out_conv_ids,
                            out_starts, out_ends);
}

int memory_db_prefs_get_sources(int user_id,
                                const int64_t *pref_ids,
                                int n,
                                int64_t *out_conv_ids,
                                int64_t *out_starts,
                                int64_t *out_ends) {
   return batch_get_sources(user_id, "p", "memory_preferences", pref_ids, n, out_conv_ids,
                            out_starts, out_ends);
}
