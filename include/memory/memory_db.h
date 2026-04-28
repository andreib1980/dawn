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
 * Memory Database API
 *
 * Provides CRUD operations for memory facts, preferences, and summaries.
 * Uses the auth_db module's SQLite database and prepared statements.
 * All functions are thread-safe via auth_db's mutex protection.
 */

#ifndef MEMORY_DB_H
#define MEMORY_DB_H

#include "memory/memory_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Return codes */
#define MEMORY_DB_SUCCESS 0
#define MEMORY_DB_FAILURE 1
#define MEMORY_DB_NOT_FOUND 2
#define MEMORY_DB_DUPLICATE 3

/* =============================================================================
 * Fact Operations
 * ============================================================================= */

/**
 * @brief Create a new memory fact
 *
 * @param user_id User who owns this fact
 * @param fact_text The fact content
 * @param confidence Confidence level (0.0-1.0)
 * @param source Source of fact ("explicit" or "inferred")
 * @param category One of MEMORY_FACT_CATEGORIES; NULL or empty defaults to "general" (v34)
 * @param id_out Output: fact ID on success (may be NULL if caller doesn't need it)
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_fact_create(int user_id,
                          const char *fact_text,
                          float confidence,
                          const char *source,
                          const char *category,
                          int64_t *id_out);

/**
 * @brief Search facts by keyword, restricted to one category (v34).
 *
 * Pre-filters at the SQL level so hybrid scoring downstream only sees facts
 * in the requested category — gives up to 34% R@K lift on category-aligned queries.
 *
 * @param user_id User ID
 * @param keywords Search terms (wrapped in %...%)
 * @param category One of MEMORY_FACT_CATEGORIES (must match exactly)
 * @param out_facts Output array (caller allocates)
 * @param max_facts Maximum facts to return
 * @param count_out Output: number of facts found
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_fact_search_by_category(int user_id,
                                      const char *keywords,
                                      const char *category,
                                      memory_fact_t *out_facts,
                                      int max_facts,
                                      int *count_out);

/**
 * @brief Update a fact's category in place (v34).  Used by the embedding-centroid
 * backfill pass and the future LLM recategorize-all admin command.
 *
 * @param fact_id Fact ID
 * @param category New category (must be one of MEMORY_FACT_CATEGORIES)
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_fact_update_category(int64_t fact_id, const char *category);

/**
 * @brief List facts with category='general' for a user, paginated by id.
 *
 * Used by LLM recategorization to fetch batches of uncategorized facts.
 *
 * @param user_id     User ID
 * @param cursor_id   Return facts with id > cursor_id (0 for first batch)
 * @param out_facts   Output array (caller allocates)
 * @param max_facts   Size of out_facts array
 * @param count_out   Output: number of facts fetched
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_fact_list_general(int user_id,
                                int64_t cursor_id,
                                memory_fact_t *out_facts,
                                int max_facts,
                                int *count_out);

/**
 * @brief Count non-superseded facts with category='general' for a user.
 *
 * @param user_id User ID
 * @param count_out Output: count of general facts
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_fact_count_general(int user_id, int *count_out);

/**
 * @brief Get a fact by ID
 *
 * @param fact_id Fact ID to retrieve
 * @param out_fact Output: populated fact structure
 * @return MEMORY_DB_SUCCESS, MEMORY_DB_NOT_FOUND, or MEMORY_DB_FAILURE
 */
int memory_db_fact_get(int64_t fact_id, memory_fact_t *out_fact);

/**
 * @brief List facts for a user (non-superseded only)
 *
 * @param user_id User ID
 * @param out_facts Output: array of facts (caller allocates)
 * @param max_facts Maximum facts to return
 * @param offset Starting offset for pagination
 * @param count_out Output: number of facts returned
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_fact_list(int user_id,
                        memory_fact_t *out_facts,
                        int max_facts,
                        int offset,
                        int *count_out);

/**
 * @brief Search facts by keyword
 *
 * @param user_id User ID
 * @param keywords Search terms (will be wrapped in %...%)
 * @param out_facts Output: array of matching facts
 * @param max_facts Maximum facts to return
 * @param count_out Output: number of facts found
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_fact_search(int user_id,
                          const char *keywords,
                          memory_fact_t *out_facts,
                          int max_facts,
                          int *count_out);

/**
 * @brief Update fact access time, count, and reinforcement boost
 *
 * Called when a fact is retrieved for context injection.
 * Includes time-gated confidence reinforcement (only boosts if
 * last_accessed > 1 hour ago to prevent confidence pinning).
 *
 * @param fact_id Fact ID to update
 * @param user_id User ID (for ownership isolation)
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_fact_update_access(int64_t fact_id, int user_id);

/**
 * @brief Update fact confidence
 *
 * @param fact_id Fact ID
 * @param confidence New confidence value
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_fact_update_confidence(int64_t fact_id, float confidence);

/**
 * @brief Mark a fact as superseded by another
 *
 * Used when a fact is corrected or updated.
 *
 * @param old_fact_id Fact being superseded
 * @param new_fact_id Fact that supersedes it
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_fact_supersede(int64_t old_fact_id, int64_t new_fact_id);

/**
 * @brief Delete a fact
 *
 * @param fact_id Fact ID
 * @param user_id User ID (for ownership check)
 * @return MEMORY_DB_SUCCESS, MEMORY_DB_NOT_FOUND, or MEMORY_DB_FAILURE
 */
int memory_db_fact_delete(int64_t fact_id, int user_id);

/**
 * @brief Find similar facts (for duplicate detection)
 *
 * Uses LIKE pattern matching on fact text.
 *
 * @param user_id User ID
 * @param fact_text Text to search for
 * @param out_facts Output: array of similar facts
 * @param max_facts Maximum facts to return
 * @param count_out Output: number of similar facts found
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_fact_find_similar(int user_id,
                                const char *fact_text,
                                memory_fact_t *out_facts,
                                int max_facts,
                                int *count_out);

/**
 * @brief Find facts by normalized hash (fast duplicate detection)
 *
 * Looks up facts by their normalized text hash for O(1) exact duplicate
 * detection. Hash collisions are expected; callers should verify with
 * Jaccard similarity.
 *
 * @param user_id User ID
 * @param hash Normalized text hash from memory_normalize_and_hash()
 * @param out_facts Output: array of facts with matching hash
 * @param max_facts Maximum facts to return
 * @param count_out Output: number of facts found
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_fact_find_by_hash(int user_id,
                                uint32_t hash,
                                memory_fact_t *out_facts,
                                int max_facts,
                                int *count_out);

/**
 * @brief Prune old superseded facts
 *
 * Deletes facts that have been superseded by newer facts and are older
 * than the retention period.
 *
 * @param user_id User ID
 * @param retention_days Keep superseded facts for this many days
 * @param count_out Output: number of facts deleted
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_fact_prune_superseded(int user_id, int retention_days, int *count_out);

/**
 * @brief Prune stale low-confidence facts
 *
 * Deletes facts that haven't been accessed in a long time and have
 * low confidence scores.
 *
 * @param user_id User ID
 * @param stale_days Prune facts not accessed in this many days
 * @param min_confidence Only prune facts with confidence below this
 * @param count_out Output: number of facts deleted
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_fact_prune_stale(int user_id, int stale_days, float min_confidence, int *count_out);

/* =============================================================================
 * Decay and Maintenance Operations (Phase 5)
 * ============================================================================= */

/**
 * @brief Apply confidence decay to all active facts for a user
 *
 * Uses atomic SQL UPDATE with powf() — no C-side row iteration needed.
 * Decay is proportional to time since last_accessed.
 *
 * @param user_id User ID
 * @param inferred_rate Weekly decay multiplier for inferred facts
 * @param explicit_rate Weekly decay multiplier for explicit facts
 * @param inferred_floor Minimum confidence for inferred facts
 * @param explicit_floor Minimum confidence for explicit facts
 * @param count_out Output: number of rows affected
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_apply_fact_decay(int user_id,
                               float inferred_rate,
                               float explicit_rate,
                               float inferred_floor,
                               float explicit_floor,
                               int *count_out);

/**
 * @brief Apply confidence decay to all preferences for a user
 *
 * @param user_id User ID
 * @param pref_rate Weekly decay multiplier for preferences
 * @param pref_floor Minimum confidence for preferences
 * @param count_out Output: number of rows affected
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_apply_pref_decay(int user_id, float pref_rate, float pref_floor, int *count_out);

/**
 * @brief Delete facts with confidence below threshold
 *
 * Logs pruned facts before deletion for audit trail.
 *
 * @param user_id User ID
 * @param threshold Delete facts below this confidence
 * @param count_out Output: number of facts deleted
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_prune_low_confidence(int user_id, float threshold, int *count_out);

/**
 * @brief Delete summaries older than retention period
 *
 * @param user_id User ID
 * @param retention_days Delete summaries older than this
 * @param count_out Output: number of summaries deleted
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_prune_old_summaries(int user_id, int retention_days, int *count_out);

/**
 * @brief Get all user IDs that have memory data
 *
 * @param out_ids Output: array of user IDs (caller allocates)
 * @param max_ids Maximum IDs to return
 * @param count_out Output: number of user IDs found
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_get_all_user_ids(int *out_ids, int max_ids, int *count_out);

/* =============================================================================
 * Date-Filtered Queries
 *
 * Variants of search/list that only return results created at or after
 * a given timestamp. Used for time_range search and fixed recent action.
 * ============================================================================= */

/**
 * @brief Search facts by keyword with time filter
 *
 * @param user_id User ID
 * @param keywords Search terms (will be wrapped in %...%)
 * @param since_ts Only return facts created at or after this timestamp
 * @param out_facts Output: array of matching facts
 * @param max_facts Maximum facts to return
 * @param count_out Output: number of facts found
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_fact_search_since(int user_id,
                                const char *keywords,
                                time_t since_ts,
                                memory_fact_t *out_facts,
                                int max_facts,
                                int *count_out);

/**
 * @brief Search summaries by keyword with time filter
 *
 * @param user_id User ID
 * @param keywords Search terms
 * @param since_ts Only return summaries created at or after this timestamp
 * @param out_summaries Output: array of matching summaries
 * @param max_summaries Maximum to return
 * @param count_out Output: number of matches
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_summary_search_since(int user_id,
                                   const char *keywords,
                                   time_t since_ts,
                                   memory_summary_t *out_summaries,
                                   int max_summaries,
                                   int *count_out);

/**
 * @brief List facts created since a timestamp (ordered by recency)
 *
 * @param user_id User ID
 * @param since_ts Only return facts created at or after this timestamp
 * @param out_facts Output: array of facts
 * @param max_facts Maximum facts to return
 * @param count_out Output: number of facts returned
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_fact_list_since(int user_id,
                              time_t since_ts,
                              memory_fact_t *out_facts,
                              int max_facts,
                              int *count_out);

/**
 * @brief List summaries created since a timestamp
 *
 * @param user_id User ID
 * @param since_ts Only return summaries created at or after this timestamp
 * @param out_summaries Output: array of summaries
 * @param max_summaries Maximum to return
 * @param count_out Output: number of summaries
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_summary_list_since(int user_id,
                                 time_t since_ts,
                                 memory_summary_t *out_summaries,
                                 int max_summaries,
                                 int *count_out);

/* =============================================================================
 * Preference Operations
 * ============================================================================= */

/**
 * @brief Upsert a preference (insert or update if exists)
 *
 * If a preference with the same category exists for this user,
 * it will be updated with the new value and its reinforcement_count
 * will be incremented.
 *
 * @param user_id User ID
 * @param category Preference category
 * @param value Preference value
 * @param confidence Confidence level
 * @param source Source ("explicit" or "inferred")
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_pref_upsert(int user_id,
                          const char *category,
                          const char *value,
                          float confidence,
                          const char *source);

/**
 * @brief Get a preference by category
 *
 * @param user_id User ID
 * @param category Category to look up
 * @param out_pref Output: populated preference
 * @return MEMORY_DB_SUCCESS, MEMORY_DB_NOT_FOUND, or MEMORY_DB_FAILURE
 */
int memory_db_pref_get(int user_id, const char *category, memory_preference_t *out_pref);

/**
 * @brief List all preferences for a user
 *
 * @param user_id User ID
 * @param out_prefs Output: array of preferences
 * @param max_prefs Maximum to return
 * @param offset Starting offset for pagination
 * @param count_out Output: number of preferences
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_pref_list(int user_id,
                        memory_preference_t *out_prefs,
                        int max_prefs,
                        int offset,
                        int *count_out);

/**
 * @brief Search preferences by keyword (LIKE on category and value)
 *
 * @param user_id User ID
 * @param keywords Search terms (will be wrapped in %...%)
 * @param out_prefs Output: array of matching preferences
 * @param max_prefs Maximum to return
 * @param count_out Output: number of matches
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_pref_search(int user_id,
                          const char *keywords,
                          memory_preference_t *out_prefs,
                          int max_prefs,
                          int *count_out);

/**
 * @brief Delete a preference
 *
 * @param user_id User ID
 * @param category Category to delete
 * @return MEMORY_DB_SUCCESS, MEMORY_DB_NOT_FOUND, or MEMORY_DB_FAILURE
 */
int memory_db_pref_delete(int user_id, const char *category);

/* =============================================================================
 * Summary Operations
 * ============================================================================= */

/**
 * @brief Create a conversation summary
 *
 * @param user_id User ID
 * @param session_id Session identifier
 * @param summary Summary text
 * @param topics Comma-separated topics
 * @param sentiment Overall sentiment
 * @param message_count Number of messages in conversation
 * @param duration_seconds Session duration
 * @param id_out Output: summary ID on success (may be NULL)
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_summary_create(int user_id,
                             const char *session_id,
                             const char *summary,
                             const char *topics,
                             const char *sentiment,
                             int message_count,
                             int duration_seconds,
                             int64_t *id_out);

/**
 * @brief List recent summaries for a user
 *
 * Only returns non-consolidated summaries.
 *
 * @param user_id User ID
 * @param out_summaries Output: array of summaries
 * @param max_summaries Maximum to return
 * @param offset Starting offset for pagination
 * @param count_out Output: number of summaries
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_summary_list(int user_id,
                           memory_summary_t *out_summaries,
                           int max_summaries,
                           int offset,
                           int *count_out);

/**
 * @brief Mark a summary as consolidated
 *
 * @param summary_id Summary ID
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_summary_mark_consolidated(int64_t summary_id);

/**
 * @brief Search summaries by keyword
 *
 * Searches both summary text and topics.
 *
 * @param user_id User ID
 * @param keywords Search terms
 * @param out_summaries Output: array of matching summaries
 * @param max_summaries Maximum to return
 * @param count_out Output: number of matches
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_summary_search(int user_id,
                             const char *keywords,
                             memory_summary_t *out_summaries,
                             int max_summaries,
                             int *count_out);

/**
 * @brief Delete a summary
 *
 * @param summary_id Summary ID
 * @param user_id User ID (for ownership check)
 * @return MEMORY_DB_SUCCESS, MEMORY_DB_NOT_FOUND, or MEMORY_DB_FAILURE
 */
int memory_db_summary_delete(int64_t summary_id, int user_id);

/* =============================================================================
 * Utility Operations
 * ============================================================================= */

/**
 * @brief Delete all memories for a user
 *
 * Used when a user requests to be forgotten.
 *
 * @param user_id User ID
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_delete_user_memories(int user_id);

/**
 * @brief Get memory statistics for a user
 *
 * @param user_id User ID
 * @param out_stats Output: statistics
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_get_stats(int user_id, memory_stats_t *out_stats);

/* =============================================================================
 * Embedding Operations (Semantic Search)
 * ============================================================================= */

/**
 * @brief Store an embedding vector for a fact
 *
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
 * @param max_count Maximum entries to return
 * @param count_out Output: number of embeddings loaded
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
/* out_created_ats: optional. Pass NULL when caller doesn't need per-fact
 * timestamps (temporal-query scoring is the only consumer). */
int memory_db_fact_get_embeddings(int user_id,
                                  int expected_dims,
                                  int64_t *out_ids,
                                  float *out_embeddings,
                                  float *out_norms,
                                  int64_t *out_created_ats,
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

/* =============================================================================
 * Entity Graph Operations (Phase S4)
 * ============================================================================= */

/**
 * @brief Build a canonical (lowercase ASCII) version of an entity name
 *
 * Lowercases ASCII characters, preserves multibyte UTF-8 as-is,
 * and trims trailing spaces.
 *
 * @param name Input entity name
 * @param out Output buffer
 * @param size Size of output buffer
 */
void memory_make_canonical_name(const char *name, char *out, size_t size);

/**
 * @brief Upsert an entity (insert or increment mention_count)
 *
 * Uses INSERT ... ON CONFLICT ... RETURNING id, mention_count.
 *
 * @param user_id User who owns this entity
 * @param name Display name
 * @param entity_type Entity type (person, pet, place, org, thing)
 * @param canonical_name Lowercased canonical name
 * @param out_created Output: true if this was a new insert (mention_count == 1)
 * @param id_out Output: entity ID on success (may be NULL)
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_entity_upsert(int user_id,
                            const char *name,
                            const char *entity_type,
                            const char *canonical_name,
                            bool *out_created,
                            int64_t *id_out);

/**
 * @brief Get an entity by exact canonical name
 *
 * @param user_id User ID
 * @param canonical_name Exact canonical name to look up
 * @param out_entity Output: populated entity structure
 * @return MEMORY_DB_SUCCESS, MEMORY_DB_NOT_FOUND, or MEMORY_DB_FAILURE
 */
int memory_db_entity_get_by_name(int user_id,
                                 const char *canonical_name,
                                 memory_entity_t *out_entity);

/**
 * @brief Update entity embedding vector
 *
 * @param entity_id Entity ID
 * @param user_id User ID (for ownership check)
 * @param embedding Float array of embedding values
 * @param dims Number of dimensions
 * @param norm Pre-computed L2 norm
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_entity_update_embedding(int64_t entity_id,
                                      int user_id,
                                      const float *embedding,
                                      int dims,
                                      float norm);

/**
 * @brief Create a relation between entities
 *
 * @param user_id User ID
 * @param subject_entity_id Subject entity ID
 * @param relation Relation type (e.g., "is_a", "lives_in")
 * @param object_entity_id Object entity ID (0 for literal)
 * @param object_value Literal value if no object entity
 * @param fact_id Associated fact ID (0 for none)
 * @param confidence Confidence score (0.0-1.0)
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_relation_create(int user_id,
                              int64_t subject_entity_id,
                              const char *relation,
                              int64_t object_entity_id,
                              const char *object_value,
                              int64_t fact_id,
                              float confidence,
                              int64_t valid_from,
                              int64_t valid_to);

/**
 * @brief Transactional close-and-create: auto-closes any existing open exclusive
 * relation with a different object before inserting the new row.  All work happens
 * under a single BEGIN IMMEDIATE so other workers cannot observe an inconsistent
 * state.  Non-exclusive relations skip the close branch (multiple open rows valid).
 *
 * See EXCLUSIVE_RELATIONS[] and CONTRADICTORY_PAIRS[] in memory_db.c for the
 * full compile-time lists of auto-close relation types.
 *
 * Use this from extraction instead of memory_db_relation_create directly.
 *
 * @param user_id User ID
 * @param subject_entity_id Subject entity ID
 * @param relation Relation type (auto-close enabled if exclusive)
 * @param object_entity_id Object entity ID (0 for literal)
 * @param object_value Literal value if no object entity
 * @param fact_id Associated fact ID (0 for none)
 * @param confidence Confidence (0.0-1.0)
 * @param valid_from Start of validity period (0 = open-ended/NULL)
 * @param valid_to End of validity period (0 = open-ended/NULL = currently true)
 * @param out_old_fact_id If non-NULL and an existing open relation was closed
 *        (exclusive supersede or contradictory-pair close), receives that old
 *        relation's fact_id (0 if none was linked)
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_relation_supersede(int user_id,
                                 int64_t subject_entity_id,
                                 const char *relation,
                                 int64_t object_entity_id,
                                 const char *object_value,
                                 int64_t fact_id,
                                 float confidence,
                                 int64_t valid_from,
                                 int64_t valid_to,
                                 int64_t *out_old_fact_id);

/**
 * @brief List relations where entity is subject (outgoing).  Returns ALL
 * relations regardless of validity period — use _list_by_subject_at for
 * temporal filtering.
 *
 * @param count_out Output: number of relations
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_relation_list_by_subject(int user_id,
                                       int64_t subject_entity_id,
                                       memory_relation_t *out,
                                       int max,
                                       int *count_out);

/**
 * @brief List relations valid at a given timestamp (v33).
 *
 * Returns rows where (valid_from IS NULL OR valid_from <= as_of_ts)
 *                AND (valid_to IS NULL OR valid_to > as_of_ts).
 *
 * Pass as_of_ts = 0 for "currently valid" (now()).  Used by the entity-recall
 * block when building the LLM context.
 *
 * @param user_id User ID
 * @param subject_entity_id Subject entity ID
 * @param as_of_ts Timestamp to evaluate validity at (0 = now)
 * @param out Output array
 * @param max Maximum results
 * @param count_out Output: number of relations
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_relation_list_by_subject_at(int user_id,
                                          int64_t subject_entity_id,
                                          int64_t as_of_ts,
                                          memory_relation_t *out,
                                          int max,
                                          int *count_out);

/**
 * @brief List incoming relations where entity is the object
 *
 * Returns relations where the given entity is the target/object.
 * The object_name field contains the subject entity's resolved name.
 *
 * @param user_id User ID
 * @param object_entity_id Entity ID to find incoming relations for
 * @param out Output array
 * @param max Maximum results
 * @param count_out Output: number of relations
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_relation_list_by_object(int user_id,
                                      int64_t object_entity_id,
                                      memory_relation_t *out,
                                      int max,
                                      int *count_out);

/**
 * @brief Bulk-load all relations for a user in a single query
 *
 * Returns outgoing relations sorted by subject_entity_id. Used by WebUI
 * to avoid N+1 queries when loading entities with their relations.
 *
 * @param user_id User ID
 * @param out Output array of relations
 * @param max Maximum relations to return
 * @param count_out Output: number of relations
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_relation_list_all_by_user(int user_id,
                                        memory_relation_t *out,
                                        int max,
                                        int *count_out);

/**
 * @brief List all entities for a user, ordered by mention count
 *
 * Used to feed existing entities into the extraction prompt so the
 * LLM reuses canonical names instead of creating variants.
 *
 * @param user_id User ID
 * @param out Output array of entities
 * @param max Maximum entities to return
 * @param offset Starting offset for pagination
 * @param count_out Output: number of entities found
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_entity_list(int user_id, memory_entity_t *out, int max, int offset, int *count_out);

/**
 * @brief Search entities by keyword (LIKE on canonical_name)
 *
 * @param user_id User ID
 * @param keywords Search terms
 * @param out Output array of entities
 * @param max Maximum entities to return
 * @param count_out Output: number of entities found
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_entity_search(int user_id,
                            const char *keywords,
                            memory_entity_t *out,
                            int max,
                            int *count_out);

/**
 * @brief Delete an entity and its relations
 *
 * @param entity_id Entity ID
 * @param user_id User ID (ownership check)
 * @return MEMORY_DB_SUCCESS, MEMORY_DB_NOT_FOUND, or MEMORY_DB_FAILURE
 */
int memory_db_entity_delete(int64_t entity_id, int user_id);

/**
 * @brief Set an entity's photo (image store reference).
 *
 * @param user_id User ID (ownership check)
 * @param entity_id Entity ID
 * @param photo_id Image store ID, or NULL to clear
 * @return MEMORY_DB_SUCCESS, MEMORY_DB_NOT_FOUND, or MEMORY_DB_FAILURE
 */
int memory_db_entity_set_photo(int user_id, int64_t entity_id, const char *photo_id);

/**
 * @brief Get an entity's photo ID.
 *
 * @param user_id User ID (ownership check)
 * @param entity_id Entity ID
 * @param out_photo_id Output buffer for photo ID
 * @param photo_id_size Size of output buffer
 * @return MEMORY_DB_SUCCESS (photo_id may be empty if none set),
 *         MEMORY_DB_NOT_FOUND, or MEMORY_DB_FAILURE
 */
int memory_db_entity_get_photo(int user_id,
                               int64_t entity_id,
                               char *out_photo_id,
                               size_t photo_id_size);

/**
 * @brief Merge source entity into target entity
 *
 * Reassigns all relations and contacts from source to target,
 * adds source mention_count to target, deduplicates self-referencing
 * relations, then deletes the source entity. All within a transaction.
 *
 * @param user_id User ID (ownership check on both entities)
 * @param source_id Entity to merge FROM (will be deleted)
 * @param target_id Entity to merge INTO (will absorb data)
 * @return MEMORY_DB_SUCCESS, MEMORY_DB_NOT_FOUND, or MEMORY_DB_FAILURE
 */
int memory_db_entity_merge(int user_id, int64_t source_id, int64_t target_id);

/**
 * @brief Load all entity embeddings for a user (for cache)
 *
 * @param user_id User ID
 * @param expected_dims Expected embedding dimensions
 * @param out_ids Output: entity IDs
 * @param out_names Output: canonical names
 * @param out_types Output: entity types
 * @param out_embeddings Output: flat float array
 * @param out_norms Output: norms
 * @param max Maximum entries
 * @param count_out Output: number loaded
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_entity_get_embeddings(int user_id,
                                    int expected_dims,
                                    int64_t *out_ids,
                                    char out_names[][MEMORY_ENTITY_NAME_MAX],
                                    char out_types[][MEMORY_ENTITY_TYPE_MAX],
                                    float *out_embeddings,
                                    float *out_norms,
                                    int max,
                                    int *count_out);

/* =============================================================================
 * Extraction Tracking
 * ============================================================================= */

/**
 * @brief Get last extracted message count for a conversation
 *
 * Used to track which messages have already been processed for
 * memory extraction, enabling incremental extraction.
 *
 * @param conversation_id Conversation ID
 * @param count_out Output: last extracted message count (0 if never extracted)
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_get_last_extracted(int64_t conversation_id, int *count_out);

/**
 * @brief Set last extracted message count for a conversation
 *
 * @param conversation_id Conversation ID
 * @param message_count Message count to record
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_set_last_extracted(int64_t conversation_id, int message_count);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_DB_H */
