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
 * @file memory_graph_retrieval.h
 * @brief Entity-graph candidate retrieval for memory.search (Phase 2.b).
 *
 * Parallel candidate source to `memory_fact_search_hybrid()` for cat-3
 * style entity-specific queries — "What country did Jolene buy snake
 * Seraphim?", "Which meat does Audrey prefer?" — where cosine over-
 * broadens on the noun token ("country", "meat") and the gold answer
 * sits at rank 50+.  Closes the LoCoMo cat-3 retrieval gap that
 * remained after the May 2026 shipped retrieval surface.
 *
 * Phase 1A (this module's initial scope) walks fact-linked relations
 * only — relations with non-NULL `fact_id`.  ~40% of relation rows
 * qualify at the dev's current scale.  Phase 1B will add structured-
 * relation synthesis for the other 60% (relations that exist as graph
 * edges without an associated fact text).
 *
 * Pipeline:
 *   1. Extract proper-noun candidates from the query string.
 *   2. Resolve each candidate against `memory_entities` (canonical names
 *      + aliases).  Discard unresolved candidates.
 *   3. For each resolved seed entity, fan out one hop via
 *      `memory_db_relation_fact_ids_for_entity()`.  Cap fan-out per
 *      query at `[memory.graph_retrieval].max_facts_per_query`.
 *   4. Dedupe fact_ids; fetch the facts; emit as (facts, scores)
 *      parallel arrays.  Initial score = `entity_grounding_bonus`
 *      (configurable, defaults to 0.4 — above the 0.30 search floor,
 *      below typical strong hybrid hits).
 *
 * Layer: 2 (alongside memory_fact_search.h).  Pulls memory_types,
 * memory_db_entities, embedding_engine for query-time entity
 * resolution.  No LLM, no logging beyond OLOG.
 *
 * Thread safety: stateless module functions; each DB call acquires
 * the auth_db mutex internally.  Safe to call from any worker.
 */

#ifndef MEMORY_GRAPH_RETRIEVAL_H
#define MEMORY_GRAPH_RETRIEVAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "memory/memory_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum proper-noun candidates extracted from a single query.  Bounds
 * stack usage in the extractor (each candidate is ~64 bytes of name +
 * scratch).  LoCoMo cat-3 questions average ~1.5 proper nouns; 8 is
 * comfortable headroom. */
#define MEMORY_GRAPH_MAX_SEED_CANDIDATES 8

/**
 * @brief Extract proper-noun candidates from @p query and resolve each
 *        against the user's `memory_entities` table.
 *
 * Heuristic candidate extraction:
 *   - Capitalized 3+ letter tokens (alphanumeric, apostrophe and hyphen
 *     allowed inside).
 *   - First-token question-starter filter (What/Why/How/Who/When/Where/
 *     Which/Would/Could/Should/Is/Are/Was/Were/Did/Does/Do/Has/Have/Had/
 *     Will) — skipped because they're sentence-initial and never proper
 *     nouns in question context.
 *   - 100% coverage on LoCoMo cat-3 questions per the May 2026 profile.
 *
 * Resolution: each candidate goes through `memory_db_entity_get_by_name`
 * first (exact canonical match), then falls back to alias resolution via
 * `memory_db_entity_resolve_alias`.  Candidates that match no real entity
 * are silently dropped (the resolver itself filters question-starters
 * and noise).
 *
 * @param user_id User ID
 * @param query Raw query text (NULL or empty → SUCCESS with count=0)
 * @param out_entity_ids Caller-allocated array of size @p max
 * @param max Cap (typically MEMORY_GRAPH_MAX_SEED_CANDIDATES)
 * @param out_count Receives actual seed count, in [0, @p max]
 * @return SUCCESS or FAILURE.  An empty seed set is SUCCESS, not failure.
 */
int memory_graph_extract_seed_entities(int user_id,
                                       const char *query,
                                       int64_t *out_entity_ids,
                                       int max,
                                       int *out_count);

/**
 * @brief Phase 1A: expand 1 hop from seed entities, fetch fact-linked
 *        candidates, emit as (facts, scores) parallel arrays.
 *
 * For each seed entity, calls `memory_db_relation_fact_ids_for_entity`
 * with a per-seed fan-out budget computed from @p max and the seed
 * count.  Dedupes fact_ids across seeds, fetches each fact via
 * `memory_db_fact_get`, applies a defense-in-depth user_id check,
 * and writes the survivors to @p out_facts / @p out_scores.
 *
 * Scoring: each fact-linked candidate receives a flat
 * `entity_grounding_bonus` score (configurable via
 * `g_config.memory.graph_retrieval.entity_grounding_bonus`).  The
 * merge layer in `memory_action_search` combines these with hybrid-
 * cosine candidates and re-ranks via the existing pipeline.
 *
 * @param user_id User ID (defense-in-depth)
 * @param seed_entity_ids Array of seed entity IDs from
 *                        memory_graph_extract_seed_entities
 * @param seed_count Number of seeds (0 → SUCCESS with count=0)
 * @param out_facts Caller-allocated array of size @p max
 * @param out_scores Caller-allocated parallel array of size @p max
 * @param max Total fan-out cap across all seeds
 * @param out_count Receives actual returned fact count, in [0, @p max]
 * @return SUCCESS or FAILURE
 */
int memory_graph_expand_fact_linked(int user_id,
                                    const int64_t *seed_entity_ids,
                                    int seed_count,
                                    memory_fact_t *out_facts,
                                    float *out_scores,
                                    int max,
                                    int *out_count);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_GRAPH_RETRIEVAL_H */
