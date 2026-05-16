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
 * BM25 sigmoid normalization for memory_facts keyword scoring.
 *
 * The sigmoid params table and logistic-normalization formula are
 * adapted from Mem0 (Apache 2.0).  See NOTICE for full attribution.
 */

/**
 * @file memory_bm25.h
 * @brief BM25 raw-score normalization helpers.
 *
 * SQLite FTS5's `bm25()` returns unbounded scores where smaller values
 * are MORE relevant (typical range 0..-20 for matched rows).  DAWN's
 * hybrid composite expects a keyword score in [0, 1] so it can be
 * linearly combined with vector cosine.  These helpers convert the raw
 * negated-BM25 score (positive, larger = more relevant) into a [0, 1]
 * confidence using a logistic sigmoid with query-length-adaptive
 * (midpoint, steepness) parameters.
 *
 * The adaptive parameter table accounts for the fact that longer
 * queries naturally accumulate larger raw BM25 scores (more term
 * matches), so the sigmoid midpoint must shift right to avoid
 * saturating at 1.0 on routine multi-term queries.
 */

#ifndef MEMORY_BM25_H
#define MEMORY_BM25_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Pick BM25 sigmoid parameters for a query of N stemmed terms.
 *
 * Adapted verbatim from `mem0/utils/scoring.py::get_bm25_params`
 * (Apache 2.0, Copyright 2023 Taranjeet Singh).  See NOTICE.
 *
 * @param num_terms  Number of stemmed tokens in the query (>= 1).
 *                   Caller is responsible for stemming + tokenizing
 *                   before counting.  If 0 or negative is passed, the
 *                   1-term row is used.
 * @param midpoint_out  [out] Raw BM25 score at which sigmoid returns 0.5.
 * @param steepness_out [out] Sigmoid steepness (k in 1/(1+e^-k(x-m))).
 *
 * Curve table (num_terms -> midpoint, steepness):
 *   1..3     ->  5.0, 0.7
 *   4..6     ->  7.0, 0.6
 *   7..9     ->  9.0, 0.5
 *   10..15   -> 10.0, 0.5
 *   16+      -> 12.0, 0.5
 */
void memory_bm25_get_params(int num_terms, float *midpoint_out, float *steepness_out);

/**
 * @brief Map a raw BM25 score (positive, larger = more relevant) to [0, 1]
 *        via logistic sigmoid.
 *
 * Adapted verbatim from `mem0/utils/scoring.py::normalize_bm25`
 * (Apache 2.0, Copyright 2023 Taranjeet Singh).  See NOTICE.
 *
 * Formula: `1 / (1 + exp(-steepness * (raw_score - midpoint)))`.
 *
 * Caller is responsible for negating SQLite FTS5's `bm25()` output —
 * FTS5 returns negative-for-relevant by convention; this helper expects
 * positive-for-relevant.
 *
 * @param raw_score  Positive BM25 score; 0 means "no signal".
 * @param midpoint   From memory_bm25_get_params().
 * @param steepness  From memory_bm25_get_params().
 * @return Normalized score in [0, 1].  Returns 0 for non-finite inputs.
 */
float memory_bm25_normalize(float raw_score, float midpoint, float steepness);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_BM25_H */
