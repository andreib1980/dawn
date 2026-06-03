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
 * Candidate allocation / cleanup helpers shared by every focus-source
 * adapter.  Phase 1d extraction from memory_focus_adapters.c — kept
 * framework-scoped (NOT memory-subsystem-scoped) so L3 adapters can
 * consume it without pulling memory headers.
 *
 * Layer: 2.  Depends only on focus_source.h + libc.  L3 adapters
 * (document, calendar) include this directly without crossing a layer
 * boundary.
 */

#ifndef MEMORY_FOCUS_CANDIDATE_HELPERS_H
#define MEMORY_FOCUS_CANDIDATE_HELPERS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "core/focus/focus_source.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Per-candidate text cap (truncate-at, not reject-at).  Sized to fully
 * accommodate a rendered document chunk: DOC_CHUNK_TEXT_MAX (4096) +
 * "[<filename>] " framing (DOC_FILENAME_MAX 256 + 3) ≈ 4355, rounded up
 * to 4608.  The earlier 1024 default made sense when only memory
 * facts/entities/summaries (avg ~200 chars) were candidate sources;
 * document chunks pushed past it — first to 4096, then to 4608 once
 * profiling showed the filename framing pushed full chunks just over
 * 4096 and tripped the truncation warning.
 *
 * NOTE: focus_budget_bytes (default 10240) is the per-call budget, in
 * the SAME unit as this cap — the subsystem standardized on bytes (the
 * old "token" budget was just bytes/4, never a real tokenizer).  A
 * SINGLE large candidate can still consume much of a small budget on
 * document-heavy turns.  This is intentional: the score-ordered ranker
 * keeps the most relevant candidate even if it crowds out others, and
 * `focus_compose` force-keeps the top-ranked candidate when the strict
 * budget would otherwise produce an empty focus block.  Users who want
 * multi-source coexistence should bump focus_budget_bytes.
 *
 * Stack-allocation footprint: per-call rendering buffers in the
 * focus-source adapters use a single `char rendered[]` per call
 * site, sized to fit `FOCUS_TEXT_MAX_BYTES` plus an adapter-specific
 * framing overhead (calendar: +64 = 4672 B; document:
 * +DOC_FILENAME_MAX+16 = 4880 B).  Worker-thread stack default is
 * 8 MB on Linux; impact is negligible.  No adapter uses
 * `FOCUS_TEXT_MAX_BYTES * N` array patterns; before bumping the cap
 * again, re-grep `FOCUS_TEXT_MAX_BYTES` across src/ + include/ to
 * confirm that property still holds. */
#define FOCUS_TEXT_MAX_BYTES 4608

/* "fact:9223372036854775807\0" → 24 chars; entity / relation /
 * document_chunk / calendar_occ variants all fit within 64. */
#define FOCUS_ITEM_ID_BUFLEN 64

/**
 * @brief Free `text` and `item_id` on a single candidate (failure path).
 *
 * NOT used on the SUCCESS path — `focus_result_free()` handles that.
 * Idempotent on already-freed / NULL members.  No-op on NULL `c`.
 */
void focus_candidate_cleanup_on_failure(focus_candidate_t *c);

/**
 * @brief Free `populated` partially-populated candidates plus the array
 *        itself, then zero the caller's out-params per the
 *        `focus_source_query_fn` FAILURE contract.
 *
 * Use from any adapter on its FAILURE return path; preserves the
 * "adapter MUST clean up partial allocation, set out=NULL, count=0"
 * contract documented in `focus_source.h`.
 */
void focus_adapter_failure_cleanup(focus_candidate_t *array,
                                   int populated,
                                   focus_candidate_t **out_candidates,
                                   int *out_count);

/**
 * @brief Allocate-and-strdup `text` + `item_id` into `c`, populating
 *        the scalar score / timestamp fields.
 *
 * Truncates `text` to `FOCUS_TEXT_MAX_BYTES` with a per-call warning
 * (fires only on the FIRST truncation via the caller-supplied
 * `truncated_warned` flag).  `source_id` is stored as-is (must point
 * to a stable static string per the framework contract).
 *
 * On any allocation failure: cleans up partial state on `c` and
 * returns FAILURE.  Provenance is zeroed; the caller fills it in
 * after the helper returns SUCCESS.
 *
 * @return SUCCESS on success; FAILURE on NULL inputs or allocation
 *         failure.
 */
int focus_candidate_init(focus_candidate_t *c,
                         const char *source_id,
                         focus_source_type_t source_type,
                         const char *text,
                         const char *item_id,
                         time_t item_timestamp,
                         float semantic_score,
                         float recency_score,
                         float importance_score,
                         bool *truncated_warned);

/**
 * @brief Format `<prefix>:<id>` into `buf`.
 *
 * Returns FAILURE on snprintf truncation (only possible if the int64
 * representation exceeded the buffer).  `buflen` should be at least
 * `FOCUS_ITEM_ID_BUFLEN`.
 */
int focus_candidate_format_item_id(char *buf, size_t buflen, const char *prefix, int64_t id);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_FOCUS_CANDIDATE_HELPERS_H */
