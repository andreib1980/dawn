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
 * Memory Database — fact-embedding sub-API.
 *
 * Storage and bulk-load primitives for per-fact embedding vectors.  Entity
 * embeddings live in memory_db_entities.h alongside the rest of the entity
 * surface (per-record-type cohesion: callers operate on facts-with-embeddings
 * or entities-with-embeddings, never both).
 *
 * Included transitively via memory_db.h — callers that already
 * `#include "memory/memory_db.h"` see these declarations unchanged.
 */

#ifndef MEMORY_DB_EMBEDDINGS_H
#define MEMORY_DB_EMBEDDINGS_H

#include <stdint.h>

#include "memory/memory_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Embedding Operations (Semantic Search)
 * ============================================================================= */

/**
 * @brief Store an embedding vector for a fact
 *
 * @param user_id User ID (ownership filter — fact must belong to this user)
 * @param fact_id Fact ID
 * @param embedding Float array of embedding values
 * @param dims Number of dimensions
 * @param norm Pre-computed L2 norm of the embedding
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_fact_update_embedding(int user_id,
                                    int64_t fact_id,
                                    const float *embedding,
                                    int dims,
                                    float norm);

/**
 * @brief Load all embeddings for a user (for in-memory cache)
 *
 * Returns fact IDs, embedding BLOBs, and pre-computed norms.
 * Skips rows where embedding dimensions don't match expected_dims.
 *
 * @param user_id User ID
 * @param expected_dims Expected embedding dimensions (for validation)
 * @param out_ids Output: array of fact IDs (caller allocates)
 * @param out_embeddings Output: flat float array (caller allocates, count * dims)
 * @param out_norms Output: array of norms (caller allocates)
 * @param out_created_ats Optional output: per-fact creation timestamps; pass NULL
 *                       when not needed (temporal-query scoring is the only
 *                       consumer today)
 * @param out_note_doc_ids Optional output: per-fact note_doc_id (v61); >0 marks a
 *                       memory→note bridge gloss.  Pass NULL when gloss-awareness
 *                       isn't needed.
 * @param max_count Maximum entries to return
 * @param count_out Output: number of embeddings loaded
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_fact_get_embeddings(int user_id,
                                  int expected_dims,
                                  int64_t *out_ids,
                                  float *out_embeddings,
                                  float *out_norms,
                                  int64_t *out_created_ats,
                                  int64_t *out_note_doc_ids,
                                  int max_count,
                                  int *count_out);

/**
 * @brief List facts that need embedding (backfill)
 *
 * Returns facts with NULL embedding or mismatched dimensions.
 *
 * @param user_id User ID
 * @param expected_dims Expected embedding dimensions
 * @param out_ids Output: array of fact IDs
 * @param out_texts Output: array of fact text strings (caller allocates char[][512])
 * @param max_count Maximum entries to return
 * @param count_out Output: number of facts found
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_fact_list_without_embedding(int user_id,
                                          int expected_dims,
                                          int64_t *out_ids,
                                          char out_texts[][512],
                                          int max_count,
                                          int *count_out);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_DB_EMBEDDINGS_H */
