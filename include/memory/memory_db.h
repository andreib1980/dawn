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
 *
 * Phase 1a (May 2026) split this header for cohesion: the entity graph
 * surface lives in memory_db_entities.h and fact-embedding storage lives
 * in memory_db_embeddings.h.  Both are included transitively below so
 * existing callers that `#include "memory/memory_db.h"` continue to
 * compile unchanged.
 */

#ifndef MEMORY_DB_H
#define MEMORY_DB_H

#include "memory/memory_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Return codes — defined in memory_types.h so the split sub-headers can
 * reference them without pulling in the memory_db.h umbrella.  Names
 * preserved exactly:
 *   MEMORY_DB_SUCCESS
 *   MEMORY_DB_FAILURE
 *   MEMORY_DB_NOT_FOUND
 *   MEMORY_DB_DUPLICATE
 *
 * Provenance typedef (memory_provenance_t) likewise lives in memory_types.h.
 */

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
 * @param prov Provenance (source range in conversation); NULL or conv_id==0 = no provenance
 * @param id_out Output: fact ID on success (may be NULL if caller doesn't need it)
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_fact_create(int user_id,
                          const char *fact_text,
                          float confidence,
                          const char *source,
                          const char *category,
                          const memory_provenance_t *prov,
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
 * will be incremented.  Latest-source-wins on upsert.
 *
 * @param user_id User ID
 * @param category Preference category
 * @param value Preference value
 * @param confidence Confidence level
 * @param source Source ("explicit" or "inferred")
 * @param prov Provenance; NULL or conv_id==0 = no provenance
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_pref_upsert(int user_id,
                          const char *category,
                          const char *value,
                          float confidence,
                          const char *source,
                          const memory_provenance_t *prov);

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
 * @param prov Provenance; NULL or conv_id==0 = no provenance
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
                             const memory_provenance_t *prov,
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
 * @brief Set last extracted message count and message-ID cursor for a conversation
 *
 * Atomically clears the recovery counters (`extraction_attempts = 0`,
 * `extraction_last_attempt_at = 0`) and advances `last_extracted_msg_id`
 * to the caller-supplied value (0 = leave unchanged, used on early-skip paths).
 * Passing the cursor value captured *before* LLM inference avoids the TOCTOU
 * race of re-querying MAX(id) at commit time.
 *
 * @param conversation_id Conversation ID
 * @param message_count Message count to record (legacy count-based cursor)
 * @param last_msg_id Highest message ID processed; 0 = do not advance cursor
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_set_last_extracted(int64_t conversation_id, int message_count, int64_t last_msg_id);

/**
 * @brief Get the last-extracted message ID cursor for a conversation (v40).
 *
 * Returns the highest message.id that was included in the most recent successful
 * extraction.  0 = never extracted.  Role-agnostic (counts all message types).
 *
 * @param conversation_id Conversation ID
 * @param msg_id_out Output: last extracted message ID (0 if never extracted)
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_get_last_extracted_msg_id(int64_t conversation_id, int64_t *msg_id_out);

/* memory_db_facts_get_sources / memory_db_fact_get_source — see
 * memory/memory_db_provenance.h.  Phase B (May 2026) split provenance readers
 * into their own module so future relation/summary/preference variants live
 * alongside the fact variants. */

#ifdef __cplusplus
}
#endif

/* Phase 1a (May 2026) sub-headers — pulled in transitively so existing
 * callers that `#include "memory/memory_db.h"` see the full surface
 * unchanged.  Sub-headers may also be included directly when a file
 * uses only one slice (entity-graph or fact-embedding storage).
 *
 * Note: the includes deliberately sit OUTSIDE the `extern "C" { ... }` block
 * above — each sub-header carries its own `extern "C"` linkage guard, so
 * nesting them here would produce redundant (legal but noisy) extern blocks.
 *
 * `memory_db_provenance.h` is intentionally NOT in this umbrella — provenance
 * readers are an opt-in module, not part of the core memory_db surface. */
#include "memory/memory_db_embeddings.h"
#include "memory/memory_db_entities.h"

#endif /* MEMORY_DB_H */
