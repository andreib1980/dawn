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
 * @file memory_predicate_dedup.h
 * @brief Relation-predicate canonicalization for two-tier vocabulary.
 *
 * Phase 0 design.  Mirrors the entity-resolver pattern (cascade →
 * composite-score-band) but applied to relation predicates, so the LLM
 * can extend the closed standard vocabulary with snake_case custom
 * predicates AND we still collapse trivial duplicates (has_child /
 * has_children, is_friend_with / is_friends_with / is_friend_of,
 * visited / has_visited / recently_visited) without per-pair rules.
 *
 * Resolver cascade (cheap → expensive):
 *   1. Lowercase + trim normalization.
 *   2. Standard-vocab exact match against the closed ~26-type list
 *      (Schema.org Person properties + ConceptNet commonsense relations).
 *   3. User-vocabulary token-Jaccard match against this user's distinct
 *      predicates already stored in memory_relations.  Threshold gates
 *      false-positive collapse.
 *   4. Fallback: return the lowercased candidate as a new custom
 *      predicate.
 *
 * Layer: 2 (alongside memory_score_floor.c).  Pulls only memory_db_*
 * for the user-vocabulary SQL; no LLM, no embeddings (v1 keeps it
 * cheap — embeddings can be added later if Jaccard misses on semantic
 * synonyms like 'enjoys' / 'likes' that should NOT be collapsed).
 *
 * Thread safety: stateless module functions; each DB call acquires the
 * auth_db mutex internally.  Safe to call from any worker.
 */

#ifndef MEMORY_PREDICATE_DEDUP_H
#define MEMORY_PREDICATE_DEDUP_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Token-Jaccard threshold for collapsing a candidate predicate onto an
 * existing one.  0.66 ≈ 2/3 token overlap.
 *
 * SCOPE — what this catches and what it doesn't:
 *
 *   The resolver does NOT stem-strip tokens (strict equality on whole
 *   tokens after underscore-split).  At the 0.66 floor it catches:
 *     - Token-permutation duplicates  ("with_is_friends" vs
 *       "is_friends_with" → 3/3 = 1.0 → snap)
 *     - High-overlap multi-token cases where one token differs and the
 *       others outnumber it 2+:1 (rare in practice)
 *
 *   It does NOT catch morphological duplicates:
 *     - has_child vs has_children     → 1/3 = 0.33 (below floor)
 *     - has_visited vs have_visited   → 1/3 = 0.33 (below floor)
 *     - is_friends_with vs is_friend_with → 2/4 = 0.50 (below floor)
 *
 *   Morphological collapse is carried by the prompt-side "Previously
 *   used relation types" hint (see build_existing_profile in
 *   memory_extraction.c): the LLM sees the user's accumulated predicate
 *   vocabulary and is told to reuse exact names rather than invent
 *   parallel variants.  That hint, not this resolver, is what keeps
 *   has_child / has_children from coexisting.  This resolver is a
 *   defense-in-depth backstop for the cases where the prompt hint
 *   misses (e.g. token-permutation, which the hint won't flag because
 *   the tokens themselves are present, just reordered).
 *
 *   Tune the floor via bench if a different morphological coverage is
 *   needed; or add a stem-strip pass before tokenization to widen the
 *   coverage envelope. */
#define MEMORY_PREDICATE_DEDUP_JACCARD_FLOOR 0.66f

/** Max candidate length we'll consider.  Matches the SQL TEXT column's
 * practical size — predicates longer than this are likely LLM
 * extraction artifacts, not real relation types. */
#define MEMORY_PREDICATE_NAME_MAX 64

/**
 * @brief Canonicalize a relation predicate emitted by the extraction LLM.
 *
 * Two-tier resolver:
 *   - Standard vocabulary (closed ~26-type list) ALWAYS wins when matched.
 *   - User custom vocabulary collapses duplicates above the Jaccard floor.
 *   - Otherwise the candidate becomes a new custom predicate.
 *
 * Called once per relation at insert time.  Cost is one DB query
 * (SELECT DISTINCT relation for this user) plus in-memory token
 * matching — typically <1ms at LoCoMo scale.
 *
 * @param user_id User ID — bounds the user-vocabulary lookup
 * @param candidate Raw predicate from the LLM (NULL or empty → returns
 *                  empty string in @p out and FAILURE)
 * @param out Caller-allocated output buffer
 * @param out_cap Capacity of @p out (must be > 0)
 * @return SUCCESS or FAILURE.  On SUCCESS, @p out is null-terminated.
 */
int memory_predicate_canonicalize(int user_id, const char *candidate, char *out, size_t out_cap);

/**
 * @brief Test if @p predicate matches one of the standard vocabulary types.
 *
 * Exact (case-sensitive) match against the closed Schema.org / ConceptNet
 * aligned list.  Used by extraction to decide whether a predicate should
 * participate in exclusive-relation / contradiction-pair semantics.
 * Standard types do NOT need user-vocabulary dedup (they're already
 * canonical by definition).
 *
 * @param predicate Predicate string (NULL → false)
 * @return true if @p predicate is in the standard vocabulary
 */
bool memory_predicate_is_standard(const char *predicate);

/**
 * @brief Token-Jaccard similarity between two snake_case predicates.
 *
 * Splits both predicates on underscore, computes |A ∩ B| / |A ∪ B|.
 * Returns 0.0 for NULL / empty inputs.  Exposed for unit testing the
 * resolver's similarity primitive.
 *
 * @param a First predicate (snake_case)
 * @param b Second predicate (snake_case)
 * @return Jaccard similarity in [0.0, 1.0]
 */
float memory_predicate_jaccard(const char *a, const char *b);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_PREDICATE_DEDUP_H */
