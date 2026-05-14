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
 * the project author(s). Contributions include any modifications,
 * enhancements, or additions to the project. These contributions become
 * part of the project and are adopted by the project author(s).
 *
 * Memory Embeddings API
 *
 * Provider-agnostic embedding generation and semantic search for the
 * memory system. Supports ONNX local inference (default), Ollama, and
 * OpenAI-compatible HTTP endpoints.
 */

#ifndef MEMORY_EMBEDDINGS_H
#define MEMORY_EMBEDDINGS_H

#include <stdbool.h>
#include <stdint.h>

#include "core/embedding_engine.h"
#include "memory/memory_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum embedding dimensions (all-MiniLM = 384, OpenAI ada = 1536) */
#define MAX_EMBEDDING_DIMS 2048

/* Maximum facts to cache for vector search */
#define EMBEDDING_SEARCH_CAP 2000

_Static_assert(MAX_EMBEDDING_DIMS * sizeof(float) <= 8192, "Embedding stack buffer exceeds 8KB");

/* Hybrid search result */
typedef struct {
   int64_t fact_id;
   float score; /* Combined hybrid score */
} embedding_search_result_t;

/**
 * @brief Initialize the embedding system
 *
 * Selects and initializes the configured provider. Call once at startup.
 *
 * @return 0 on success, non-zero on failure (non-fatal — search falls back to keyword)
 */
int memory_embeddings_init(void);

/**
 * @brief Shut down the embedding system
 *
 * Joins the backfill thread and releases provider resources.
 */
void memory_embeddings_cleanup(void);

/**
 * @brief Check if embeddings are available
 *
 * @return true if a provider is initialized and ready
 */
bool memory_embeddings_available(void);

/**
 * @brief Get the embedding dimension for the current provider
 *
 * @return Number of dimensions, or 0 if not initialized
 */
int memory_embeddings_dims(void);

/**
 * @brief Generate an embedding for text
 *
 * @param text Input text to embed
 * @param out Output float array (must hold at least MAX_EMBEDDING_DIMS)
 * @param out_dims Output: actual dimensions written
 * @return 0 on success, non-zero on failure
 */
int memory_embeddings_embed(const char *text, float *out, int *out_dims);

/**
 * @brief Generate embedding and store it for a fact
 *
 * Convenience function: embeds text and writes to DB in one call.
 *
 * @param user_id User who owns the fact (for ownership check)
 * @param fact_id Fact ID to update
 * @param text Fact text to embed
 * @return 0 on success, non-zero on failure
 */
int memory_embeddings_embed_and_store(int user_id, int64_t fact_id, const char *text);

/**
 * @brief Store a pre-embedded vector for a fact, then update the in-memory
 * cache directly (instead of invalidating it).
 *
 * Companion to memory_embeddings_nearest_fact() in the extraction-time
 * paraphrase-dedup loop.  The caller embeds the fact text once for both
 * dedup scoring and storage, avoiding the second embed call that
 * memory_embeddings_embed_and_store() would otherwise pay.  This variant
 * also appends to the per-user fact cache instead of invalidating it, so
 * an N-fact extraction loop does not pay N cache-reload cycles against
 * SQLite.
 *
 * If cache append fails (e.g. EMBEDDING_SEARCH_CAP reached or user
 * mismatch), the function falls back to invalidating the cache so the
 * next access reloads fresh.
 *
 * @param user_id User who owns the fact (for ownership check on the DB write)
 * @param fact_id Fact ID to update — the row must already exist
 * @param vec Pre-computed embedding vector
 * @param dims Vector dimensions (must match memory_embeddings_dims())
 * @return 0 on success, non-zero on failure
 */
int memory_embeddings_store_precomputed(int user_id, int64_t fact_id, const float *vec, int dims);

/**
 * @brief Pre-warm the per-user fact embedding cache.
 *
 * Loads (or refreshes if dirty) the cache for @p user_id.  The dedup loop
 * in memory_extraction.c calls this once at the top of
 * process_extraction_response(), so the first nearest_fact() lookup hits
 * a warm cache rather than paying one-time DB-load latency on the
 * extraction worker thread.  Idempotent — already-valid same-user cache
 * is a no-op.
 *
 * @param user_id User ID whose fact cache to load
 * @return 0 on success (cache is now warm), non-zero on failure
 */
int memory_embeddings_warm_cache(int user_id);

/**
 * @brief Find the highest-scoring fact-cache match against a precomputed
 * embedding, returning early on the first match >= @p threshold.
 *
 * Designed for the extraction-time paraphrase-dedup gate: the gate just
 * needs ANY match above threshold (not the absolute best), so we exit on
 * first hit instead of walking the entire cache.  The caller embeds the
 * fact text OUTSIDE any lock; this function acquires the cache mutex
 * internally for the cosine scan only, keeping the critical section
 * sub-millisecond at the dev's ~1200-fact scale.
 *
 * Treats facts without embeddings (norm == 0) as no-match — silently
 * skipped rather than failing the call.
 *
 * @param user_id User ID whose fact cache to scan
 * @param query_vec Pre-computed query embedding
 * @param query_dims Query embedding dimensions (must match cache dims)
 * @param threshold Minimum cosine score to count as a match (e.g. 0.92)
 * @param matched_id_out Output: fact_id of the match, or 0 if no match
 * @param score_out Output: cosine score of the match, or 0.0f if no match
 * @return MEMORY_DB_SUCCESS (with @p matched_id_out=0 if no match), or
 *         MEMORY_DB_FAILURE on cache load error / dim mismatch
 */
int memory_embeddings_nearest_fact(int user_id,
                                   const float *query_vec,
                                   int query_dims,
                                   float threshold,
                                   int64_t *matched_id_out,
                                   float *score_out);

/**
 * @brief Perform hybrid keyword + vector search
 *
 * Combines keyword search scores with vector cosine similarity.
 * Falls back to keyword-only if embeddings are unavailable.
 *
 * @param user_id User ID
 * @param query Search query text
 * @param keyword_facts Pre-searched keyword results (fact IDs)
 * @param keyword_scores Keyword scores per fact (from multi_token_fact_search)
 * @param keyword_count Number of keyword results
 * @param token_count Number of search tokens (for score normalization)
 * @param out_results Output: sorted hybrid results
 * @param max_results Maximum results to return
 * @return Number of results
 */
int memory_embeddings_hybrid_search(int user_id,
                                    const char *query,
                                    const int64_t *keyword_facts,
                                    const int *keyword_scores,
                                    int keyword_count,
                                    int token_count,
                                    embedding_search_result_t *out_results,
                                    int max_results);

/** Sentinel score returned by `memory_embeddings_rescore_against_query`
 * for facts that could not be scored (no embedding in the per-user cache,
 * embedding engine unavailable, query embedding failed).  Callers MUST
 * treat any score equal to this sentinel as "skip this fact" — do NOT
 * merge into the LLM-facing pool.  Negative-infinity (per IEEE 754
 * behavior with insertion sort) is sufficient and distinct from any
 * legitimate score value, but `-1.0f` is human-readable in logs and
 * works with `score < 0.0f` checks. */
#define MEMORY_EMBEDDINGS_RESCORE_SENTINEL (-1.0f)

/**
 * @brief Phase 2 Step 1: re-score a caller-supplied list of facts against a
 *        pre-computed query embedding using cosine similarity + an optional
 *        additive bonus.
 *
 * Used by graph retrieval to apply query-relevance scoring to entity-bounded
 * candidate facts, replacing the legacy flat `entity_grounding_bonus` that
 * was disconnected from query content.
 *
 * Scoring formula (per fact):
 *
 *   if fact_id is in the per-user embedding cache:
 *     score = vec_weight * cosine(query_emb, fact_emb) + entity_bonus
 *   else:
 *     score = MEMORY_EMBEDDINGS_RESCORE_SENTINEL  // caller MUST drop
 *
 * `vec_weight` is read from `g_config.memory.embedding_vector_weight` so the
 * scale stays consistent with `memory_embeddings_hybrid_search`.
 *
 * Taking a pre-computed embedding (rather than re-embedding the query)
 * avoids a duplicate ONNX inference per `memory_action_search` call —
 * `memory_fact_search_hybrid` already embedded the same query string
 * upstream; passing the embedding through saves 5-30 ms on bge-small
 * INT8 / Jetson.
 *
 * The keyword term from the hybrid formula is deliberately omitted: graph
 * candidates are entity-grounded by construction, so the kw signal is
 * largely redundant with the entity-graph membership.  Cosine carries the
 * topical-relevance signal cleanly.
 *
 * **Caller contract**: facts whose returned score equals
 * `MEMORY_EMBEDDINGS_RESCORE_SENTINEL` MUST be dropped from the merged
 * pool.  Earlier versions silently substituted `entity_bonus` for such
 * facts, which produced low-quality LLM-facing candidates and caused a
 * -9pp Phase 2 Step 1 regression (May 14, 2026 architecture audit).
 *
 * If @p query_emb is NULL or the embedding engine is unavailable, EVERY
 * fact gets the sentinel — the caller should treat this as "graph
 * retrieval is silently disabled this turn" and rely on hybrid alone.
 *
 * Thread-safe: takes the embedding cache lock internally.
 *
 * @param user_id Owner user_id (for cache scoping)
 * @param query_emb Caller-supplied query embedding (size must be
 *                  `embedding_engine_dims()`).  NULL → all sentinels.
 * @param query_norm Pre-computed L2 norm of @p query_emb (saves recomputation).
 * @param entity_bonus Caller-supplied additive bonus (typically 0.0)
 * @param fact_ids Caller-allocated array of fact IDs to score (in)
 * @param fact_count Number of facts
 * @param out_scores Caller-allocated parallel array (out) — overwritten;
 *                   sentinel means "drop this fact"
 * @return SUCCESS or FAILURE (FAILURE only on NULL out_scores / NULL fact_ids).
 */
int memory_embeddings_rescore_against_query(int user_id,
                                            const float *query_emb,
                                            float query_norm,
                                            float entity_bonus,
                                            const int64_t *fact_ids,
                                            int fact_count,
                                            float *out_scores);

/**
 * @brief Invalidate the fact embedding cache (e.g., after store/delete)
 */
void memory_embeddings_invalidate_cache(void);

/**
 * @brief Generate embedding and store it for an entity
 *
 * @param entity_id Entity ID to update
 * @param user_id User ID (for ownership check)
 * @param text Entity name text to embed
 * @return 0 on success, non-zero on failure
 */
int memory_embeddings_embed_and_store_entity(int64_t entity_id, int user_id, const char *text);

/**
 * @brief Generate embedding and store it for a summary row.
 *
 * Convenience wrapper paralleling memory_embeddings_embed_and_store for
 * facts.  Used by the live extractor (immediately after a successful
 * memory_db_summary_create) and the summarize-missing backfill worker so
 * every summary row lands with an embedding without a deferred recompute
 * pass.  No-op if the embedding engine is unavailable — the recompute
 * worker will pick it up on next boot.
 *
 * @param user_id     owning user ID
 * @param summary_id  summary row ID
 * @param text        summary text to embed
 * @return SUCCESS on store, FAILURE on embed or DB error
 */
int memory_embeddings_embed_and_store_summary(int user_id, int64_t summary_id, const char *text);

/**
 * @brief Invalidate the entity embedding cache
 */
void memory_embeddings_invalidate_entity_cache(void);

/**
 * @brief Invalidate both fact and entity embedding caches in one call.
 *
 * Helper for paths that drop or replace memory state in bulk
 * (delete_user_memories, snapshot reload, bench reset).  Safer than calling
 * the two cache-specific invalidators separately — adding a third cache later
 * only requires updating this helper, not every call site.
 */
void memory_embeddings_invalidate_all(void);

/**
 * @brief Search entities by semantic similarity
 *
 * @param user_id User ID
 * @param query Search query
 * @param type_filter Optional entity type filter (NULL for all)
 * @param out_ids Output: entity IDs
 * @param out_names Output: entity names
 * @param out_types Output: entity types
 * @param out_scores Output: cosine similarity scores
 * @param max_results Maximum results
 * @return Number of results
 */
int memory_embeddings_entity_search(int user_id,
                                    const char *query,
                                    const char *type_filter,
                                    int64_t *out_ids,
                                    char out_names[][MEMORY_ENTITY_NAME_MAX],
                                    char out_types[][MEMORY_ENTITY_TYPE_MAX],
                                    float *out_scores,
                                    int max_results);

/**
 * @brief Score a precomputed query embedding against a single entity's cached
 * embedding.
 *
 * Loads the per-user entity cache on demand (same RAM-resident pool used by
 * memory_embeddings_entity_search) and returns the cosine in @p out_cosine
 * if the entity is found.  Returns FAILURE if the entity is not in the cache
 * (no embedding stored, or — once Ckpt 3's loader filter ships — an alias
 * row with canonical_id IS NOT NULL).  Caller treats FAILURE as "no signal"
 * and contributes 0 to the embedding-cosine term in the alias-merge composite.
 *
 * Used by the v43 alias resolver Stage 4 (see memory_db_alias.c).  Operates
 * on the same cache as memory_embeddings_entity_search() so the resolver does
 * not pay a DB hit per candidate.
 *
 * @param user_id User ID (selects which cache to use)
 * @param entity_id Entity to score
 * @param query_embedding Pre-computed query embedding
 * @param query_dims Dimensions of query embedding (must match cache dims)
 * @param query_norm Pre-computed L2 norm of query embedding
 * @param out_cosine Output: cosine similarity in [-1, 1]; populated only on SUCCESS
 * @return SUCCESS, or FAILURE if entity_id is not in the cache or dims mismatch
 */
int memory_embeddings_entity_cosine(int user_id,
                                    int64_t entity_id,
                                    const float *query_embedding,
                                    int query_dims,
                                    float query_norm,
                                    float *out_cosine);

/**
 * @brief Start background backfill of un-embedded facts
 *
 * @param user_id User ID to backfill
 */
void memory_embeddings_start_backfill(int user_id);

/* Compute L2 norm of a float vector */
float memory_embeddings_l2_norm(const float *vec, int dims);

/* Compute cosine similarity using pre-computed norms */
float memory_embeddings_cosine_with_norms(const float *a,
                                          const float *b,
                                          int dims,
                                          float norm_a,
                                          float norm_b);

/* Compute cosine similarity (computes norms internally) */
float memory_embeddings_cosine(const float *a, const float *b, int dims);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_EMBEDDINGS_H */
