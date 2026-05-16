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
 * Dominant-token over-inclusion heuristic for focus-injection candidates.
 *
 * Bench-validated structural fix for the pathology probed by cases 16-18
 * in benchmarks/focus_probe_cases.json: queries with a low-IDF dominant
 * token ("favorite restaurant", "doctor blood test") where > threshold
 * of the candidate pool shares that token, and the right-answer summary
 * uses specific vocabulary instead.  Pure cosine + weight tuning cannot
 * rescue these cases — the semantic gap exceeds the maximum recency
 * lift any safe parameter value can produce.  Phase B-i prototype
 * (benchmarks/bench_focus_heuristic_probe.py) demonstrated 18/18 aggregate
 * quorum across all three providers under any reasonable (threshold,
 * base_penalty) pair in {0.50, 0.60, 0.70} x {0.30, 0.40, 0.50}.
 *
 * Pipeline placement: between adapter filter/cap and the ranker in
 * focus_compose().  The heuristic mutates each candidate's semantic_score
 * in the working pool (subtracting the per-candidate penalty), so the
 * existing composite formula propagates the penalty naturally.  The
 * score breakdown will reflect the penalized value as `semantic_contribution`
 * — diagnostic transparency is via the OLOG_DEBUG line per call.
 *
 * Threading: stateless and thread-safe (pure function over the pool +
 * query string).
 */

#ifndef MEMORY_FOCUS_DOMINANT_TOKEN_H
#define MEMORY_FOCUS_DOMINANT_TOKEN_H

#include <stddef.h>

#include "core/focus/focus_source.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Apply the dominant-token over-inclusion penalty to a working pool
 *        of focus-injection candidates.
 *
 * Algorithm:
 *   1. Tokenize @p query into alphanumeric runs ≥ 4 chars, drop stopwords
 *      (articles, pronouns, common verbs, generic "user" agent tokens).
 *   2. For each query token, count how many candidates contain it.
 *   3. Tokens shared by > @p threshold of the pool are "dominant."
 *   4. For each candidate:
 *      - `shared = query_tokens ∩ candidate_tokens`
 *      - `shared_dominant = shared ∩ dominant_tokens`
 *      - `non_dominant = shared - dominant_tokens`
 *      - Penalty = 0 if `shared_dominant` is empty
 *      - Penalty = @p base_penalty if only dominant tokens match
 *      - Penalty = `base_penalty * |shared_dominant| / |shared|` if some
 *        non-dominant tokens also match (diluted)
 *   5. Subtract penalty from `pool[i].semantic_score` (clamped to ≥ 0).
 *
 * No-op cases (function returns early without mutation):
 *   - @p enabled is false
 *   - @p pool_count < 2 (no pool to compute dominance against)
 *   - @p query is NULL or empty
 *   - Query contains no tokens after stopword filtering
 *
 * Threading: pure function; safe to call concurrently from multiple
 * `focus_compose()` invocations.
 *
 * @param enabled        Master toggle (typically @c
 * g_config.memory.focus_injection.dominant_token_heuristic.enabled)
 * @param threshold      Fraction of pool a token must appear in to be
 *                       considered "dominant" (0.0-1.0, typical 0.5-0.7)
 * @param base_penalty   Maximum penalty applied to a candidate's
 *                       semantic_score when only dominant tokens match
 *                       (0.0-1.0, typical 0.3-0.5)
 * @param query          User's raw turn text; NULL or empty is no-op
 * @param pool           Working pool of candidates; semantic_score is
 *                       mutated in place when penalty applies
 * @param pool_count     Number of entries in @p pool
 */
void focus_apply_dominant_token_penalty(bool enabled,
                                        float threshold,
                                        float base_penalty,
                                        const char *query,
                                        focus_candidate_t *pool,
                                        int pool_count);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_FOCUS_DOMINANT_TOKEN_H */
