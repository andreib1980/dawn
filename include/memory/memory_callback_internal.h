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
 * Internal interface for memory_callback.c.  Exposes the per-action search
 * and recent helpers so the bench harness can drive the production memory
 * retrieval path with explicit user_id and source_budget overrides — without
 * routing through memoryCallback's value-string parsing.  Not part of the
 * public memory API; do not include outside memory_callback.c and the bench.
 */

#ifndef MEMORY_CALLBACK_INTERNAL_H
#define MEMORY_CALLBACK_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/**
 * @brief Search memory facts/preferences/summaries/relations.
 *
 * @param user_id Owner of the memory store
 * @param keywords Search query (NULL or empty returns an error string)
 * @param since_ts If > 0, restricts to items created on/after this timestamp
 * @param category Optional fact category filter (NULL = all)
 * @param as_of_ts For relation validity (0 = now)
 * @param include_historical Bypass relation validity filter
 * @param with_source Append verbatim source excerpts to each fact
 * @param source_budget Char budget for verbatim excerpts; 0 = use default
 *                      (MEMORY_SOURCE_BUDGET_CHARS = 3072).  Bench harness
 *                      passes a non-zero override to sweep budget values
 *                      without mutating shared global state.
 * @return Heap-allocated formatted string (caller frees), or NULL on alloc fail
 */
char *memory_action_search(int user_id,
                           const char *keywords,
                           time_t since_ts,
                           const char *category,
                           int64_t as_of_ts,
                           bool include_historical,
                           bool with_source,
                           int source_budget);

/**
 * @brief List facts/summaries created within a time period.
 *
 * @param user_id Owner of the memory store
 * @param period Time period string (e.g., "24h", "7d", "1w")
 * @param with_source Append verbatim source excerpts to each fact
 * @param source_budget Char budget for verbatim excerpts; 0 = use default
 * @return Heap-allocated formatted string (caller frees), or NULL on alloc fail
 */
char *memory_action_recent(int user_id, const char *period, bool with_source, int source_budget);

/* =============================================================================
 * Source-fetch dedup set (Phase B).
 *
 * Spans a single memory_action_search / memory_action_recent call so that the
 * same `(conversation_id, msg_start, msg_end)` cited by multiple records
 * (fact + summary + relation) emits a verbatim excerpt only on first mention
 * and a one-line back-ref on subsequent mentions.  Saves both LLM context and
 * SQLite round-trips.
 *
 * INVARIANT (Sec M1 in PROVENANCE.md): callers must add ONLY post-batch-filter
 * triples — the `_get_sources` batch APIs return `out_conv_ids[i] = 0` for
 * private-conversation rows, so a triple where conv_id > 0 is provably public.
 * Adding a private triple here would cause a later public record sharing the
 * same range to emit "[source: same as above]" which betrays existence of the
 * upstream private record.  Helper functions enforce nothing on their own —
 * the discipline lives at the call sites in memory_callback.c.
 *
 * Exposed in this internal header so unit tests can exercise the helpers
 * directly without compiling the whole callback module.
 * ============================================================================= */

#define SOURCE_DEDUP_CAP 24

typedef struct {
   int64_t conv_ids[SOURCE_DEDUP_CAP];
   int64_t starts[SOURCE_DEDUP_CAP];
   int64_t ends[SOURCE_DEDUP_CAP];
   int count;
} source_dedup_set_t;

bool source_dedup_seen(const source_dedup_set_t *set, int64_t conv, int64_t s, int64_t e);
void source_dedup_add(source_dedup_set_t *set, int64_t conv, int64_t s, int64_t e);

#endif /* MEMORY_CALLBACK_INTERNAL_H */
