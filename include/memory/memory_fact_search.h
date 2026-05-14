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
 * @file memory_fact_search.h
 * @brief Memory fact-search pipeline — extracted from memory_callback.c
 *        so the Phase 1c focus-source fact adapter and the legacy
 *        `process_recall_action` call site share identical retrieval
 *        semantics.
 *
 * Pipeline:
 *   1. Tokenize the query string.
 *   2. Multi-token keyword search (with optional `since_ts` time filter)
 *      with per-fact match-count scoring.
 *   3. If embeddings are available AND a `query_embedding` was supplied,
 *      hand the keyword fact_ids + scores to
 *      `memory_embeddings_hybrid_search()` and emit the re-ranked output.
 *      Vector-only hits (those NOT in the keyword set) are fetched via
 *      `memory_db_fact_get()` with a defense-in-depth `user_id`
 *      post-check — `memory_db_fact_get` is NOT user-scoped, so the
 *      caller must verify ownership.
 *   4. Otherwise emit the keyword-only ranking with match counts as the
 *      score.
 *
 * Layer: 2.  Header pulls only `memory_types.h` + libc.
 *
 * Thread safety: stateless; safe to call concurrently.  Each
 * `memory_db_*` callee acquires the auth_db mutex internally; this
 * helper holds no locks of its own.
 */

#ifndef MEMORY_FACT_SEARCH_H
#define MEMORY_FACT_SEARCH_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "memory/memory_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Run DAWN's full fact-retrieval pipeline.
 *
 * Tokenize the query → multi-token keyword search (with optional
 * `since_ts` time filter) → hybrid embedding re-rank when
 * `query_embedding` is provided AND `memory_embeddings_available()` is
 * true.  Otherwise returns keyword-only results.
 *
 * Memory ownership: caller allocates `out_facts` and `out_scores`
 * arrays of size `max`.  No heap allocation inside the helper.
 *
 * @param user_id      Authenticated user — every internal `memory_db_*`
 *                     call is scoped by this value.  The vector-only
 *                     fetch path additionally post-checks the returned
 *                     fact's `user_id` against this value to defend
 *                     against any future regression in
 *                     `memory_embeddings_hybrid_search`'s user scoping.
 * @param query        Raw query string; tokenized internally.  NULL or
 *                     empty returns SUCCESS with `*out_count = 0`.
 * @param query_embedding  Acts as a GATE only — non-NULL opts into the
 *                     hybrid re-rank path (when embeddings are
 *                     available); NULL forces keyword-only.  The helper
 *                     MUST NOT dereference this pointer; the underlying
 *                     `memory_embeddings_hybrid_search()` re-embeds the
 *                     query string internally.  Callers that don't have
 *                     a pre-computed embedding may pass any non-NULL
 *                     stack address as a sentinel.  If a future variant
 *                     of this helper actually consumes the vector, this
 *                     contract must be revisited (consider switching to
 *                     a `bool use_hybrid` parameter).
 * @param embed_dim    Embedding dimensionality; ignored when
 *                     `query_embedding` is NULL.
 * @param since_ts     0 = no time filter; otherwise restrict keyword
 *                     hits to facts with `created_at >= since_ts`.
 * @param out_facts    Caller-allocated output array.
 * @param out_scores   Caller-allocated parallel score array.  Hybrid
 *                     score (cosine + keyword combination) when
 *                     re-ranked; raw keyword match count otherwise.
 * @param max          Capacity of `out_facts` / `out_scores`.
 * @param out_count    Receives actual returned count, in `[0, max]`.
 *
 * @return SUCCESS or FAILURE.  Per-fact errors (e.g., a vector-only
 *         hit owned by a different user) cause that hit to be skipped
 *         silently with a `OLOG_WARNING`; the call still returns
 *         SUCCESS so surviving hits make it back to the caller.
 */
int memory_fact_search_hybrid(int user_id,
                              const char *query,
                              const float *query_embedding,
                              size_t embed_dim,
                              time_t since_ts,
                              memory_fact_t *out_facts,
                              float *out_scores,
                              int max,
                              int *out_count);

/**
 * @brief "I don't remember" gate — compact a fact/score parallel array by
 *        dropping entries whose score falls below @p score_floor.
 *
 * Designed to be applied to the output of `memory_fact_search_hybrid()` at
 * any call site where the resulting facts will be surfaced to the LLM —
 * the memory tool's `search` action and the per-turn focus-injection fact
 * adapter both qualify.  Closes the floor against fabrication from
 * marginal-cosine hits.
 *
 * - `score_floor <= 0.0f` → no-op (legacy behavior — every above-near-zero
 *   hit reaches the LLM).
 * - Keyword-only fallback scores from `memory_fact_search_hybrid()` are
 *   integer match counts cast to float (always >= 1.0), so any sub-1.0
 *   floor passes them untouched — only hybrid-path marginal-cosine hits
 *   are filtered.
 *
 * In-place compaction preserves the descending-score order from the
 * upstream ranker.  Pure function — no allocations, no logging.
 *
 * @param facts In/out parallel array; entries with score >= floor stay,
 *              others are compacted out.
 * @param scores In/out parallel array (must be same length as @p facts).
 * @param count Current valid entry count.
 * @param score_floor Score threshold (inclusive lower bound).
 * @return New entry count after filtering, in [0, @p count].
 */
int memory_search_apply_score_floor(memory_fact_t *facts,
                                    float *scores,
                                    int count,
                                    float score_floor);

/** Maximum `max` parameter the unified retrieval primitive
 * `memory_search_execute()` will accept.  Sizes its stack-allocated merge
 * pool (`max + MEMORY_GRAPH_INTERMEDIATE_CAP` entries).  Callers passing
 * larger values get clamped silently.  10 matches the LLM-facing slot
 * count used by `memory_action_search`; bench callers can pass 1..10. */
#define MEMORY_SEARCH_HYBRID_MAX 10

/**
 * @brief Unified retrieval primitive: hybrid + entity-graph + score floor.
 *
 * Single source of truth for "given a user query, return top-K relevant
 * facts."  Called by both production (`memory_action_search`, non-category
 * branch) and the bench harness (`bench_mp_handle_query_memory`) so the
 * two paths can't drift.
 *
 * Pipeline:
 *   1. `memory_fact_search_hybrid()` → hybrid keyword + cosine retrieval
 *      against the full corpus, top-`max` by hybrid score.
 *   2. Entity-graph candidate expansion (Phase 1A) if enabled in config:
 *      extract proper-noun seeds from @p query, walk fact-linked relations,
 *      return up to `MEMORY_GRAPH_INTERMEDIATE_CAP` entity-bounded
 *      candidates.
 *   3. Phase 2 Step 1 query-aware scoring if enabled: re-score graph
 *      candidates as `vec_weight * cosine + entity_bonus` so topically-
 *      irrelevant entity-grounded facts compete fairly with hybrid hits.
 *      Sentinel-scored facts (no embedding in cache) are dropped.
 *   4. Score-based merge: union(hybrid, graph) pool → sort by score
 *      descending → truncate to @p max.  Lets a high-scoring graph
 *      candidate displace a low-scoring hybrid tail entry.
 *   5. Apply `search_score_floor` to drop marginal-cosine hits.
 *
 * All scoring knobs read from `g_config.memory.*`.  Only the per-call
 * inputs (user_id, query, since_ts, max) are caller-supplied.
 *
 * Thread-safe: each underlying primitive acquires its own mutex as needed
 * (s_cache.mutex inside hybrid + rescore, auth_db_mutex inside DB calls).
 *
 * @param user_id Owner user_id (for defense-in-depth scoping)
 * @param query Query text
 * @param since_ts Optional time filter (0 = no filter)
 * @param out_facts Caller-allocated array of size @p max
 * @param out_scores Caller-allocated parallel array of size @p max
 * @param max Output cap (typically 10 for LLM-facing retrieval)
 * @param out_count Receives actual returned fact count, in [0, @p max]
 * @return SUCCESS or FAILURE
 */
int memory_search_execute(int user_id,
                          const char *query,
                          time_t since_ts,
                          memory_fact_t *out_facts,
                          float *out_scores,
                          int max,
                          int *out_count);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_FACT_SEARCH_H */
