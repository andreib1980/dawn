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
 * Memory provenance reader APIs (Phase B, schema v40).
 *
 * Single-record and batch readers that return the source-conversation back-link
 * `(conversation_id, msg_id_start, msg_id_end)` for memory facts, relations,
 * summaries, and preferences.  Privacy filtering (`conversations.is_private = 0`)
 * is applied in SQL via JOIN; rows owned by the caller but stored in private
 * conversations return `out_conv_ids[i] = 0` for batch APIs or
 * `MEMORY_DB_NOT_FOUND` for single-record APIs.
 *
 * Batch readers cap N at MAX_PROVENANCE_BATCH and fail closed
 * (MEMORY_DB_FAILURE) on overflow.  Callers (top-K result lists) are bounded
 * well below the cap.
 */

#ifndef MEMORY_DB_PROVENANCE_H
#define MEMORY_DB_PROVENANCE_H

#include <stdint.h>

#include "memory/memory_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum number of IDs the IN-clause SQL builder will fit into a single
 * statement.  The 1024-byte SQL buffer + 21-char-per-ID encoding leaves room
 * for ~32 IDs after the SELECT/JOIN prefix; a static_assert in the .c file
 * ties this cap to the buffer size.
 *
 * The PUBLIC `_get_sources` APIs accept ANY positive N — they slice into
 * MAX_PROVENANCE_BATCH-sized chunks internally and run one statement per
 * chunk.  Callers therefore do not need to chunk on their own; the WebUI list
 * path (50 facts) and bench harness (up to 500) hit multiple chunks
 * transparently.  Defense-in-depth fail-closed semantics are retained at the
 * inner SQL builder so a snprintf-truncation bug can never produce a
 * partial-IN-clause query. */
#define MAX_PROVENANCE_BATCH 32

/**
 * @brief Return the source range for a fact (v40).
 *
 * Leaf function — must NOT be called while holding the auth_db mutex.
 * Returns MEMORY_DB_NOT_FOUND if: source columns are NULL, the fact does not
 * exist, the fact belongs to a different user, the source conversation is
 * private, or the source conversation has been deleted.
 *
 * @param fact_id Fact ID to look up
 * @param user_id Requesting user (ownership check)
 * @param conv_id_out Output: source conversation ID
 * @param start_out Output: first message ID in source range
 * @param end_out Output: last message ID in source range
 * @return MEMORY_DB_SUCCESS, MEMORY_DB_NOT_FOUND, or MEMORY_DB_FAILURE
 */
int memory_db_fact_get_source(int64_t fact_id,
                              int user_id,
                              int64_t *conv_id_out,
                              int64_t *start_out,
                              int64_t *end_out);

/**
 * @brief Batch-fetch provenance for up to `n` facts in a single lock cycle (v40).
 *
 * Replaces N sequential calls to `memory_db_fact_get_source()` in list paths.
 * Outputs are indexed to match the input `fact_ids[]` array; entries with no
 * provenance (legacy rows, NULL source, or private conversation) have
 * out_conv_ids[i] = 0.
 *
 * Accepts any positive N; chunks internally in MAX_PROVENANCE_BATCH-sized
 * passes (one DB lock cycle per chunk).
 *
 * @param user_id Requesting user (ownership + privacy filter applied in SQL)
 * @param fact_ids Array of fact IDs to look up
 * @param n Length of fact_ids (and output arrays); must be > 0
 * @param out_conv_ids Output: source conversation ID per fact (0 = none)
 * @param out_starts Output: source_msg_id_start per fact
 * @param out_ends Output: source_msg_id_end per fact
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_facts_get_sources(int user_id,
                                const int64_t *fact_ids,
                                int n,
                                int64_t *out_conv_ids,
                                int64_t *out_starts,
                                int64_t *out_ends);

/**
 * @brief Batch-fetch provenance for up to `n` relations.
 *
 * Same semantics as memory_db_facts_get_sources but for `memory_relations`.
 * Accepts any positive N (chunks internally).
 */
int memory_db_relations_get_sources(int user_id,
                                    const int64_t *relation_ids,
                                    int n,
                                    int64_t *out_conv_ids,
                                    int64_t *out_starts,
                                    int64_t *out_ends);

/**
 * @brief Batch-fetch provenance for up to `n` summaries.
 *
 * Same semantics as memory_db_facts_get_sources but for `memory_summaries`.
 * Accepts any positive N (chunks internally).
 */
int memory_db_summaries_get_sources(int user_id,
                                    const int64_t *summary_ids,
                                    int n,
                                    int64_t *out_conv_ids,
                                    int64_t *out_starts,
                                    int64_t *out_ends);

/**
 * @brief Batch-fetch provenance for up to `n` preferences.
 *
 * Same semantics as memory_db_facts_get_sources but for `memory_preferences`.
 * Accepts any positive N (chunks internally).
 */
int memory_db_prefs_get_sources(int user_id,
                                const int64_t *pref_ids,
                                int n,
                                int64_t *out_conv_ids,
                                int64_t *out_starts,
                                int64_t *out_ends);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_DB_PROVENANCE_H */
