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
 * Embedding Recomputation Worker
 *
 * Detects when the configured embedding model has changed and re-indexes all
 * stored embeddings (memory_facts, memory_entities, document_chunks) in the
 * background so cosine similarity remains valid across model swaps.
 */

#ifndef MEMORY_EMBED_RECOMPUTE_H
#define MEMORY_EMBED_RECOMPUTE_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Start the embedding recomputation worker.
 *
 * Compares system_metadata.embedding_model_id against
 * g_config.memory.model_id. If they differ, launches a background thread
 * (nice 10) that re-embeds all facts and entities for each user whose
 * users.embeddings_model_id does not match the current model, then updates
 * users.embeddings_model_id on completion. A second lower-priority pass
 * re-embeds document_chunks after the facts/entities pass.
 *
 * No-op if g_config.memory.recompute_on_model_change is false or the
 * embedding engine is unavailable.
 *
 * @return SUCCESS or FAILURE (thread creation failure).
 */
int memory_embed_recompute_start(void);

/**
 * @brief Signal the recomputation worker to stop and wait for it to exit.
 *
 * Must be called before auth_db_close(). Safe to call even if the worker
 * was never started or has already finished.
 */
void memory_embed_recompute_stop(void);

/**
 * @brief Return true if the recomputation worker thread is running.
 */
bool memory_embed_recompute_in_progress(void);

/**
 * @brief Query recomputation progress.
 *
 * @param done_out   Number of items recomputed so far (may be NULL).
 * @param total_out  Total items to recompute (may be NULL; 0 = unknown).
 * @return SUCCESS if worker was started, FAILURE if never started.
 */
int memory_embed_recompute_status(int64_t *done_out, int64_t *total_out);

#endif /* MEMORY_EMBED_RECOMPUTE_H */
