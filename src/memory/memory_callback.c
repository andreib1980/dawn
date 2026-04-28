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
 * Memory Callback Implementation
 *
 * Handles the memory tool actions: search, remember, forget.
 */

#define _GNU_SOURCE /* strptime + timegm for as_of date parsing */

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config/dawn_config.h"
#include "core/session_manager.h"
#include "logging.h"
#include "memory/contacts_db.h"
#include "memory/memory_db.h"
#include "memory/memory_embeddings.h"
#include "memory/memory_filter.h"
#include "memory/memory_similarity.h"
#include "memory/memory_types.h"
#include "mosquitto_comms.h"
#include "tools/time_utils.h"
#include "tools/tool_registry.h"

/* =============================================================================
 * Helper: Get user ID from current session
 * ============================================================================= */

static int get_current_user_id(void) {
#ifdef ENABLE_MULTI_CLIENT
   session_t *session = session_get_command_context();
   if (session) {
      /* For authenticated WebSocket sessions, use their user_id */
      if (session->metrics.user_id > 0) {
         return session->metrics.user_id;
      }
      /* For local voice sessions, use configured default user */
      if (session->type == SESSION_TYPE_LOCAL) {
         int user_id = g_config.memory.default_voice_user_id;
         return (user_id > 0) ? user_id : 1; /* Fallback to admin */
      }
   }
#endif
   /* Fallback for non-multi-client builds: use default voice user */
   int user_id = g_config.memory.default_voice_user_id;
   return (user_id > 0) ? user_id : 1;
}

/* =============================================================================
 * Helper: Format time difference
 * ============================================================================= */

static void format_time_ago(time_t timestamp, char *buf, size_t buf_size) {
   if (timestamp == 0) {
      snprintf(buf, buf_size, "unknown");
      return;
   }

   time_t now = time(NULL);
   time_t diff = now - timestamp;

   if (diff < 60) {
      snprintf(buf, buf_size, "just now");
   } else if (diff < 3600) {
      snprintf(buf, buf_size, "%ld min ago", diff / 60);
   } else if (diff < 86400) {
      snprintf(buf, buf_size, "%ld hours ago", diff / 3600);
   } else if (diff < 604800) {
      snprintf(buf, buf_size, "%ld days ago", diff / 86400);
   } else {
      snprintf(buf, buf_size, "%ld weeks ago", diff / 604800);
   }
}

/* =============================================================================
 * Helper: Tokenize search query into individual words
 *
 * Splits keywords on whitespace/punctuation, lowercases each token,
 * and skips single-character tokens (noise). Returns token count.
 * ============================================================================= */

#define MAX_SEARCH_TOKENS 8
#define MAX_DEDUP_RESULTS 30 /* Max intermediate results during multi-token dedup */

/**
 * @brief Multi-token fact search with dedup and relevance ranking
 *
 * Searches for each token independently, deduplicates results by fact ID,
 * scores by number of matching tokens, and returns top results sorted by
 * score (desc) then confidence (desc).
 *
 * @param user_id User to search for
 * @param tokens Tokenized search terms
 * @param token_count Number of tokens
 * @param per_token_limit Max results per individual token search
 * @param results Output array for top results
 * @param max_results Maximum results to return
 * @return Number of results found
 */
static int multi_token_fact_search(int user_id,
                                   char tokens[][64],
                                   int token_count,
                                   int per_token_limit,
                                   memory_fact_t *results,
                                   int max_results,
                                   time_t since_ts,
                                   int *out_scores) {
   int64_t seen_ids[MAX_DEDUP_RESULTS];
   int seen_scores[MAX_DEDUP_RESULTS];
   memory_fact_t seen_facts[MAX_DEDUP_RESULTS];
   int seen_count = 0;

   for (int t = 0; t < token_count; t++) {
      memory_fact_t token_results[10];
      int limit = per_token_limit > 10 ? 10 : per_token_limit;
      int n = 0;
      if (since_ts > 0)
         memory_db_fact_search_since(user_id, tokens[t], since_ts, token_results, limit, &n);
      else
         memory_db_fact_search(user_id, tokens[t], token_results, limit, &n);
      for (int j = 0; j < n; j++) {
         int found = -1;
         for (int k = 0; k < seen_count; k++) {
            if (seen_ids[k] == token_results[j].id) {
               found = k;
               break;
            }
         }
         if (found >= 0) {
            seen_scores[found]++;
         } else if (seen_count < MAX_DEDUP_RESULTS) {
            seen_ids[seen_count] = token_results[j].id;
            seen_scores[seen_count] = 1;
            seen_facts[seen_count] = token_results[j];
            seen_count++;
         }
      }
   }

   /* Insertion sort by score desc, then confidence desc */
   for (int i = 1; i < seen_count; i++) {
      int64_t tmp_id = seen_ids[i];
      int tmp_score = seen_scores[i];
      memory_fact_t tmp_fact = seen_facts[i];
      int j = i - 1;
      while (j >= 0 &&
             (seen_scores[j] < tmp_score ||
              (seen_scores[j] == tmp_score && seen_facts[j].confidence < tmp_fact.confidence))) {
         seen_ids[j + 1] = seen_ids[j];
         seen_scores[j + 1] = seen_scores[j];
         seen_facts[j + 1] = seen_facts[j];
         j--;
      }
      seen_ids[j + 1] = tmp_id;
      seen_scores[j + 1] = tmp_score;
      seen_facts[j + 1] = tmp_fact;
   }

   int count = (seen_count > max_results) ? max_results : seen_count;
   for (int i = 0; i < count; i++) {
      results[i] = seen_facts[i];
      if (out_scores)
         out_scores[i] = seen_scores[i];
   }
   return count;
}

static int tokenize_query(const char *keywords, char tokens[][64], int max_tokens) {
   if (!keywords || max_tokens <= 0) {
      return 0;
   }

   char buf[512];
   strncpy(buf, keywords, sizeof(buf) - 1);
   buf[sizeof(buf) - 1] = '\0';

   for (size_t i = 0; buf[i]; i++) {
      buf[i] = tolower((unsigned char)buf[i]);
   }

   int count = 0;
   char *saveptr = NULL;
   char *tok = strtok_r(buf, " \t\n\r,.;:!?\"'()[]{}/-", &saveptr);
   while (tok && count < max_tokens) {
      if (strlen(tok) > 1) {
         snprintf(tokens[count], 64, "%s", tok);
         count++;
      }
      tok = strtok_r(NULL, " \t\n\r,.;:!?\"'()[]{}/-", &saveptr);
   }
   return count;
}

/* =============================================================================
 * Helper: Append entity graph context to search results
 *
 * as_of_ts (v33): timestamp at which to evaluate relation validity.
 *   0 = "currently true" (now()).
 *   non-zero = relations valid at that historical timestamp.
 * include_historical: when true, bypasses validity filter (returns all rows).
 * ============================================================================= */

static size_t append_graph_context(int user_id,
                                   const char *keywords,
                                   int64_t as_of_ts,
                                   bool include_historical,
                                   char *buf,
                                   size_t buf_size,
                                   size_t offset) {
   /* Try semantic entity search first */
   int64_t entity_ids[5];
   char entity_names[5][MEMORY_ENTITY_NAME_MAX];
   char entity_types[5][MEMORY_ENTITY_TYPE_MAX];
   float entity_scores[5];
   int entity_count = 0;

   entity_count = memory_embeddings_entity_search(user_id, keywords, NULL, entity_ids, entity_names,
                                                  entity_types, entity_scores, 5);

   /* Fallback to keyword search if vector search returned nothing */
   if (entity_count == 0) {
      /* Tokenize query and search per-token to avoid full-string LIKE mismatch */
      char tokens[MAX_SEARCH_TOKENS][64];
      int token_count = tokenize_query(keywords, tokens, MAX_SEARCH_TOKENS);
      int64_t seen_ids[5];
      int seen_count = 0;

      for (int t = 0; t < token_count && entity_count < 5; t++) {
         memory_entity_t kw_entities[5];
         int kw_count = 0;
         memory_db_entity_search(user_id, tokens[t], kw_entities, 5, &kw_count);
         for (int i = 0; i < kw_count && entity_count < 5; i++) {
            /* Dedup by entity ID */
            bool dup = false;
            for (int k = 0; k < seen_count; k++) {
               if (seen_ids[k] == kw_entities[i].id) {
                  dup = true;
                  break;
               }
            }
            if (dup)
               continue;
            if (seen_count < 5)
               seen_ids[seen_count++] = kw_entities[i].id;
            entity_ids[entity_count] = kw_entities[i].id;
            strncpy(entity_names[entity_count], kw_entities[i].name, MEMORY_ENTITY_NAME_MAX - 1);
            entity_names[entity_count][MEMORY_ENTITY_NAME_MAX - 1] = '\0';
            strncpy(entity_types[entity_count], kw_entities[i].entity_type,
                    MEMORY_ENTITY_TYPE_MAX - 1);
            entity_types[entity_count][MEMORY_ENTITY_TYPE_MAX - 1] = '\0';
            entity_count++;
         }
      }
   }

   if (entity_count == 0)
      return offset;

   /* Show top 3 entities with their relations (skip entities with no relations) */
   int show_count = entity_count > 3 ? 3 : entity_count;
   size_t header_offset = offset; /* Save position before writing header */
   bool header_written = false;

   for (int i = 0; i < show_count && offset < buf_size - 100; i++) {
      /* Pre-fetch relations to check if entity has any.  When include_historical
       * is true, return all relations regardless of validity.  Otherwise apply
       * temporal filtering: relations valid at as_of_ts (0 = now). */
      memory_relation_t rels[8];
      int rel_count;
      if (include_historical) {
         memory_db_relation_list_by_subject(user_id, entity_ids[i], rels, 8, &rel_count);
      } else {
         memory_db_relation_list_by_subject_at(user_id, entity_ids[i], as_of_ts, rels, 8,
                                               &rel_count);
      }
      /* Incoming relations: no temporal filter helper today; subject-side filter is
       * the high-value case (e.g., "where does Alice work").  Object-side stays
       * unfiltered for v1 — incoming recall is contextual, less likely to confuse. */
      memory_relation_t in_rels[8];
      int in_count = 0;
      memory_db_relation_list_by_object(user_id, entity_ids[i], in_rels, 8, &in_count);

      if (rel_count == 0 && in_count == 0)
         continue;

      /* Write header on first entity that has relations */
      if (!header_written) {
         if (offset > 0 && offset < buf_size - 20) {
            offset += snprintf(buf + offset, buf_size - offset, "\n");
         }
         offset += snprintf(buf + offset, buf_size - offset, "ENTITIES:\n");
         header_written = true;
      }

      offset += snprintf(buf + offset, buf_size - offset, "- %s (%s):\n", entity_names[i],
                         entity_types[i]);

      for (int r = 0; r < rel_count && offset < buf_size - 80; r++) {
         offset += snprintf(buf + offset, buf_size - offset, "  %s: %s\n", rels[r].relation,
                            rels[r].object_name);
      }

      for (int r = 0; r < in_count && offset < buf_size - 80; r++) {
         offset += snprintf(buf + offset, buf_size - offset, "  %s %s %s\n", in_rels[r].object_name,
                            in_rels[r].relation, entity_names[i]);
      }
   }

   return offset;
}

/* =============================================================================
 * Action: Search
 * ============================================================================= */

/* category (v34): when non-NULL/non-empty, pre-filters fact-ID set by exact category
 *   match before hybrid scoring.  Bypasses time_range path (categories layer above
 *   recency for now — combinable in a follow-up if useful).
 * as_of_ts (v33): timestamp for relation validity in entity recall (0 = now).
 * include_historical (v33): bypass relation validity filter entirely. */
static char *memory_action_search(int user_id,
                                  const char *keywords,
                                  time_t since_ts,
                                  const char *category,
                                  int64_t as_of_ts,
                                  bool include_historical) {
   if (!keywords || strlen(keywords) == 0) {
      return strdup("Please provide search keywords.");
   }

   /* Allocate result buffer (8KB for entity graph output) */
   size_t buf_size = 8192;
   char *result = malloc(buf_size);
   if (!result) {
      return strdup("Memory search failed: out of memory.");
   }
   result[0] = '\0';
   size_t offset = 0;

   /* Tokenize query for per-word matching */
   char tokens[MAX_SEARCH_TOKENS][64];
   int token_count = tokenize_query(keywords, tokens, MAX_SEARCH_TOKENS);

   /* Search facts — keyword search first, then hybrid if embeddings available */
   memory_fact_t facts[10];
   int kw_scores[10];
   int fact_count = 0;
   bool category_filter_active = (category && *category);

   if (category_filter_active) {
      /* Category pre-filter is independent of token count — single SQL per query.
       * Score-vs-time combinability deferred (see decision #7). */
      memory_db_fact_search_by_category(user_id, keywords, category, facts, 10, &fact_count);
      for (int i = 0; i < fact_count; i++)
         kw_scores[i] = 1;
   } else if (token_count <= 1) {
      /* Single word or empty: use original single-call path */
      if (since_ts > 0)
         memory_db_fact_search_since(user_id, keywords, since_ts, facts, 10, &fact_count);
      else
         memory_db_fact_search(user_id, keywords, facts, 10, &fact_count);
      /* Single-token: all scores = 1 */
      for (int i = 0; i < fact_count; i++)
         kw_scores[i] = 1;
   } else {
      /* Multi-word: search per token, dedup by ID, rank by match count */
      fact_count = multi_token_fact_search(user_id, tokens, token_count, 10, facts, 10, since_ts,
                                           kw_scores);
   }

   /* Hybrid search: combine keyword results with vector similarity */
   if (memory_embeddings_available() && fact_count >= 0) {
      int64_t kw_ids[10];
      for (int i = 0; i < fact_count; i++)
         kw_ids[i] = facts[i].id;

      embedding_search_result_t hybrid_results[10];
      int hybrid_count = memory_embeddings_hybrid_search(user_id, keywords, kw_ids, kw_scores,
                                                         fact_count,
                                                         (token_count > 0) ? token_count : 1,
                                                         hybrid_results, 10);

      if (hybrid_count > 0) {
         /* Re-order facts by hybrid score */
         memory_fact_t reordered[10];
         int reordered_count = 0;

         for (int h = 0; h < hybrid_count && reordered_count < 10; h++) {
            /* Find fact by ID in original results */
            bool found = false;
            for (int f = 0; f < fact_count; f++) {
               if (facts[f].id == hybrid_results[h].fact_id) {
                  reordered[reordered_count++] = facts[f];
                  found = true;
                  break;
               }
            }
            /* Vector-only result: fetch from DB */
            if (!found) {
               memory_fact_t vec_fact;
               if (memory_db_fact_get(hybrid_results[h].fact_id, &vec_fact) == MEMORY_DB_SUCCESS) {
                  reordered[reordered_count++] = vec_fact;
               }
            }
         }
         memcpy(facts, reordered, reordered_count * sizeof(memory_fact_t));
         fact_count = reordered_count;
      }
   }

   if (fact_count > 0) {
      offset += snprintf(result + offset, buf_size - offset, "FACTS (%d):\n", fact_count);
      for (int i = 0; i < fact_count && offset < buf_size - 100; i++) {
         char time_str[32];
         format_time_ago(facts[i].created_at, time_str, sizeof(time_str));
         offset += snprintf(result + offset, buf_size - offset,
                            "- [ID:%lld] %s (confidence: %.0f%%, %s)\n", (long long)facts[i].id,
                            facts[i].fact_text, facts[i].confidence * 100, time_str);

         /* Update access time */
         memory_db_fact_update_access(facts[i].id, user_id);
      }
   }

   /* Search preferences — SQL-filtered by LIKE on category and value */
   memory_preference_t prefs[10];
   int pref_count = 0;
   memory_db_pref_search(user_id, keywords, prefs, 10, &pref_count);

   if (pref_count > 0) {
      if (offset > 0 && offset < buf_size - 20) {
         offset += snprintf(result + offset, buf_size - offset, "\n");
      }
      offset += snprintf(result + offset, buf_size - offset, "PREFERENCES:\n");
      for (int i = 0; i < pref_count && offset < buf_size - 100; i++) {
         offset += snprintf(result + offset, buf_size - offset, "- %s: %s (reinforced %d times)\n",
                            prefs[i].category, prefs[i].value, prefs[i].reinforcement_count);
      }
   }

   /* Search summaries */
   memory_summary_t summaries[5];
   int summary_count = 0;

   if (token_count <= 1) {
      if (since_ts > 0)
         memory_db_summary_search_since(user_id, keywords, since_ts, summaries, 5, &summary_count);
      else
         memory_db_summary_search(user_id, keywords, summaries, 5, &summary_count);
   } else {
      /* Multi-word: search per token, dedup by ID */
      int64_t seen_sum_ids[20];
      int seen_sum_count = 0;

      for (int t = 0; t < token_count && summary_count < 5; t++) {
         memory_summary_t token_results[5];
         int n = 0;
         if (since_ts > 0)
            memory_db_summary_search_since(user_id, tokens[t], since_ts, token_results, 5, &n);
         else
            memory_db_summary_search(user_id, tokens[t], token_results, 5, &n);
         for (int j = 0; j < n && summary_count < 5; j++) {
            bool dup = false;
            for (int k = 0; k < seen_sum_count; k++) {
               if (seen_sum_ids[k] == token_results[j].id) {
                  dup = true;
                  break;
               }
            }
            if (!dup) {
               if (seen_sum_count < 20) {
                  seen_sum_ids[seen_sum_count++] = token_results[j].id;
               }
               summaries[summary_count++] = token_results[j];
            }
         }
      }
   }

   if (summary_count > 0 && offset < buf_size - 100) {
      if (offset > 0) {
         offset += snprintf(result + offset, buf_size - offset, "\n");
      }
      offset += snprintf(result + offset, buf_size - offset, "CONVERSATION SUMMARIES (%d):\n",
                         summary_count);
      for (int i = 0; i < summary_count && offset < buf_size - 200; i++) {
         char time_str[32];
         format_time_ago(summaries[i].created_at, time_str, sizeof(time_str));
         offset += snprintf(result + offset, buf_size - offset, "- [%s] %s\n  Topics: %s\n",
                            time_str, summaries[i].summary, summaries[i].topics);
      }
   }

   /* Append entity graph context */
   offset = append_graph_context(user_id, keywords, as_of_ts, include_historical, result, buf_size,
                                 offset);

   if (offset == 0) {
      snprintf(result, buf_size, "No memories found matching '%s'.", keywords);
   }

   return result;
}

/* =============================================================================
 * Action: Remember
 * ============================================================================= */

static char *memory_action_remember(int user_id, const char *fact_text) {
   if (!fact_text || strlen(fact_text) == 0) {
      return strdup("Please provide the fact to remember.");
   }

   if (strlen(fact_text) > MEMORY_FACT_TEXT_MAX - 1) {
      return strdup("The fact is too long. Please keep it under 500 characters.");
   }

   /* Check for injection patterns */
   if (memory_filter_check(fact_text)) {
      return strdup("I cannot store that as a fact. It contains patterns that could affect my "
                    "behavior in unintended ways.");
   }

   /* Stage 1: Fast hash-based duplicate check */
   uint32_t fact_hash = memory_normalize_and_hash(fact_text);

   if (fact_hash != 0) {
      memory_fact_t hash_matches[5];
      int hash_count = 0;
      memory_db_fact_find_by_hash(user_id, fact_hash, hash_matches, 5, &hash_count);

      if (hash_count > 0) {
         /* Verify with Jaccard similarity (handles hash collisions) */
         for (int i = 0; i < hash_count; i++) {
            if (memory_is_duplicate(fact_text, hash_matches[i].fact_text,
                                    MEMORY_SIMILARITY_THRESHOLD)) {
               /* Exact or near-exact duplicate - reinforce confidence */
               float new_conf = hash_matches[i].confidence + 0.1f;
               if (new_conf > 1.0f)
                  new_conf = 1.0f;
               memory_db_fact_update_confidence(hash_matches[i].id, new_conf);
               OLOG_INFO("memory_callback: duplicate detected (hash match), reinforced fact %ld",
                         (long)hash_matches[i].id);
               return strdup("I already know that. Increased my confidence in this fact.");
            }
         }
      }
   }

   /* Stage 2: SQL LIKE search for potential fuzzy duplicates */
   memory_fact_t similar[5];
   int similar_count = 0;
   memory_db_fact_find_similar(user_id, fact_text, similar, 5, &similar_count);

   if (similar_count > 0) {
      /* Check Jaccard similarity on candidates */
      for (int i = 0; i < similar_count; i++) {
         float similarity = memory_jaccard_similarity(fact_text, similar[i].fact_text);
         if (similarity >= MEMORY_SIMILARITY_THRESHOLD) {
            /* Similar enough to be considered a duplicate */
            float new_conf = similar[i].confidence + 0.1f;
            if (new_conf > 1.0f)
               new_conf = 1.0f;
            memory_db_fact_update_confidence(similar[i].id, new_conf);
            OLOG_INFO("memory_callback: duplicate detected (Jaccard=%.2f), reinforced fact %ld",
                      similarity, (long)similar[i].id);
            return strdup(
                "I already know something similar. Increased my confidence in that fact.");
         }
      }
   }

   /* No duplicates found - store the new fact */
   int64_t fact_id = 0;
   int create_rc = memory_db_fact_create(user_id, fact_text, 1.0f, "explicit", NULL, &fact_id);

   if (create_rc != MEMORY_DB_SUCCESS) {
      return strdup("Failed to store the fact. Please try again.");
   }

   /* Generate and store embedding (non-blocking, ~5-10ms for ONNX) */
   if (memory_embeddings_available()) {
      memory_embeddings_embed_and_store(user_id, fact_id, fact_text);
   }

   char *result = malloc(256);
   if (result) {
      snprintf(result, 256, "Remembered: \"%s\"", fact_text);
   }
   return result ? result : strdup("Fact stored successfully.");
}

/* =============================================================================
 * Action: Forget
 * ============================================================================= */

static char *memory_action_forget(int user_id, const char *fact_text) {
   if (!fact_text || strlen(fact_text) == 0) {
      return strdup("Please specify the fact ID to forget. Use 'search' or 'recent' first to find "
                    "the ID.");
   }

   /* Require numeric DB ID — prevents accidental deletion from fuzzy search */
   char *endptr = NULL;
   errno = 0;
   long long id_val = strtoll(fact_text, &endptr, 10);
   if (!endptr || *endptr != '\0' || id_val <= 0 || errno == ERANGE) {
      return strdup("Please provide a numeric fact ID (e.g., '42'). Use 'search' or 'recent' first "
                    "to find the ID of the memory you want to forget.");
   }

   /* Look up fact by ID and verify ownership */
   memory_fact_t fact;
   int get_result = memory_db_fact_get((int64_t)id_val, &fact);
   if (get_result != MEMORY_DB_SUCCESS) {
      char *msg = malloc(128);
      if (msg)
         snprintf(msg, 128, "No fact found with ID %lld.", id_val);
      return msg ? msg : strdup("Fact not found.");
   }

   if (fact.user_id != user_id) {
      return strdup("Fact not found."); /* Don't reveal other users' facts */
   }

   int result = memory_db_fact_delete((int64_t)id_val, user_id);

   if (result == MEMORY_DB_SUCCESS) {
      char *msg = malloc(384);
      if (msg) {
         snprintf(msg, 384, "Forgotten (ID %lld): \"%.200s%s\"", id_val, fact.fact_text,
                  strlen(fact.fact_text) > 200 ? "..." : "");
         return msg;
      }
      return strdup("Fact forgotten successfully.");
   } else {
      return strdup("Failed to forget the fact. Please try again.");
   }
}

/* =============================================================================
 * Action: Recent
 *
 * Returns facts and summaries created within a specified time period.
 * ============================================================================= */

static char *memory_action_recent(int user_id, const char *period) {
   if (!period || strlen(period) == 0) {
      return strdup("Please specify a time period (e.g., '24h', '7d', '1w').");
   }

   time_t seconds = parse_time_period(period);
   if (seconds <= 0) {
      return strdup("Invalid time period. Use format like '24h', '7d', '1w', or '30m'.");
   }

   time_t since = time(NULL) - seconds;
   if (since < 0)
      since = 0;

   /* Allocate result buffer */
   size_t buf_size = 4096;
   char *result = malloc(buf_size);
   if (!result) {
      return strdup("Memory query failed: out of memory.");
   }
   result[0] = '\0';
   size_t offset = 0;

   /* Get recent facts — SQL-filtered by created_at, ordered by recency */
   memory_fact_t facts[20];
   int fact_count = 0;
   memory_db_fact_list_since(user_id, since, facts, 20, &fact_count);

   if (fact_count > 0) {
      offset += snprintf(result + offset, buf_size - offset, "RECENT FACTS:\n");
      for (int i = 0; i < fact_count && offset < buf_size - 100; i++) {
         char time_str[32];
         format_time_ago(facts[i].created_at, time_str, sizeof(time_str));
         offset += snprintf(result + offset, buf_size - offset, "- [ID:%lld] %s (%s, %s)\n",
                            (long long)facts[i].id, facts[i].fact_text, facts[i].source, time_str);
      }
   }

   /* Get recent summaries — SQL-filtered by created_at */
   memory_summary_t summaries[10];
   int summary_count = 0;
   memory_db_summary_list_since(user_id, since, summaries, 10, &summary_count);

   if (summary_count > 0 && offset < buf_size - 200) {
      if (offset > 0) {
         offset += snprintf(result + offset, buf_size - offset, "\n");
      }
      offset += snprintf(result + offset, buf_size - offset, "RECENT CONVERSATIONS:\n");
      for (int i = 0; i < summary_count && offset < buf_size - 200; i++) {
         char time_str[32];
         format_time_ago(summaries[i].created_at, time_str, sizeof(time_str));
         offset += snprintf(result + offset, buf_size - offset, "- [%s] %s\n  Topics: %s\n",
                            time_str, summaries[i].summary, summaries[i].topics);
      }
   }

   if (fact_count == 0 && summary_count == 0) {
      snprintf(result, buf_size, "No memories found in the past %s.", period);
   } else {
      if (offset < buf_size - 50) {
         offset += snprintf(result + offset, buf_size - offset,
                            "\nTotal: %d facts, %d conversations", fact_count, summary_count);
      }
   }

   return result;
}

/* =============================================================================
 * Main Callback
 * ============================================================================= */

char *memoryCallback(const char *actionName, char *value, int *should_respond) {
   if (should_respond) {
      *should_respond = 1;
   }

   /* Check if memory system is enabled */
   if (!g_config.memory.enabled) {
      return strdup("Memory system is disabled.");
   }

   /* Get current user ID */
   int user_id = get_current_user_id();
   if (user_id <= 0) {
      return strdup("Memory system requires authentication. Please log in.");
   }

   if (!actionName) {
      return strdup("Invalid memory action.");
   }

   OLOG_INFO("memory_callback: action='%s', value='%s', user_id=%d", actionName,
             value ? value : "(null)", user_id);

   if (strcmp(actionName, "search") == 0) {
      /* Extract base keywords and optional time_range / category / as_of /
       * include_historical from encoded value. */
      char keywords[512] = "";
      time_t since_ts = 0;
      char category[MEMORY_CATEGORY_MAX] = "";
      int64_t as_of_ts = 0;
      bool include_historical = false;

      if (value) {
         tool_param_extract_base(value, keywords, sizeof(keywords));

         char time_range[32] = "";
         if (tool_param_extract_custom(value, "time_range", time_range, sizeof(time_range))) {
            time_t seconds = parse_time_period(time_range);
            if (seconds > 0) {
               since_ts = time(NULL) - seconds;
               if (since_ts < 0)
                  since_ts = 0;
            }
         }

         /* category param: validate against canonical taxonomy; ignore unknowns. */
         char raw_cat[MEMORY_CATEGORY_MAX] = "";
         if (tool_param_extract_custom(value, "category", raw_cat, sizeof(raw_cat)) && raw_cat[0]) {
            for (int i = 0; i < MEMORY_FACT_CATEGORY_COUNT; i++) {
               if (strcmp(raw_cat, MEMORY_FACT_CATEGORIES[i]) == 0) {
                  strncpy(category, raw_cat, sizeof(category) - 1);
                  category[sizeof(category) - 1] = '\0';
                  break;
               }
            }
            if (!category[0]) {
               OLOG_INFO("memory_callback: ignoring unknown category '%s'", raw_cat);
            }
         }

         /* as_of: ISO-8601 date for relation validity at a historical point. */
         char as_of_str[32] = "";
         if (tool_param_extract_custom(value, "as_of", as_of_str, sizeof(as_of_str)) &&
             as_of_str[0]) {
            struct tm tm = { 0 };
            char *end = strptime(as_of_str, "%Y-%m-%d", &tm);
            if (!end) {
               memset(&tm, 0, sizeof(tm));
               end = strptime(as_of_str, "%Y", &tm);
               if (end) {
                  tm.tm_mday = 1;
                  tm.tm_mon = 0;
               }
            }
            if (end) {
               time_t t = timegm(&tm);
               if (t != (time_t)-1)
                  as_of_ts = (int64_t)t;
            }
         }

         /* include_historical: "true" / "false" string. */
         char hist[8] = "";
         if (tool_param_extract_custom(value, "include_historical", hist, sizeof(hist))) {
            include_historical = (strcmp(hist, "true") == 0);
         }
      }

      return memory_action_search(user_id, keywords[0] ? keywords : NULL, since_ts,
                                  category[0] ? category : NULL, as_of_ts, include_historical);
   } else if (strcmp(actionName, "remember") == 0) {
      return memory_action_remember(user_id, value);
   } else if (strcmp(actionName, "forget") == 0) {
      return memory_action_forget(user_id, value);
   } else if (strcmp(actionName, "recent") == 0) {
      return memory_action_recent(user_id, value);
   } else if (strcmp(actionName, "save_contact") == 0) {
      /* value format: "entity_name::field_type::type::value::val::label::lbl" */
      if (!value || !value[0])
         return strdup("Error: save_contact requires entity name, field_type, and value");

      char entity_name[128] = "";
      tool_param_extract_base(value, entity_name, sizeof(entity_name));

      char field_type[32] = "";
      tool_param_extract_custom(value, "field_type", field_type, sizeof(field_type));

      char contact_value[256] = "";
      tool_param_extract_custom(value, "value", contact_value, sizeof(contact_value));

      char label[32] = "";
      tool_param_extract_custom(value, "label", label, sizeof(label));

      /* Optional entity_id for disambiguation (from a prior fuzzy match prompt) */
      char entity_id_str[32] = "";
      tool_param_extract_custom(value, "entity_id", entity_id_str, sizeof(entity_id_str));

      if (!entity_name[0] || !field_type[0] || !contact_value[0])
         return strdup("Error: save_contact requires entity name, field_type, and value");

      int64_t entity_id;

      if (entity_id_str[0]) {
         /* Explicit entity_id provided — use it directly (disambiguation resolved) */
         entity_id = atoll(entity_id_str);
         if (entity_id <= 0)
            return strdup("Error: invalid entity_id");
      } else {
         /* Check for exact canonical match first */
         char canonical[64];
         memory_make_canonical_name(entity_name, canonical, sizeof(canonical));

         memory_entity_t exact;
         int exact_rc = memory_db_entity_get_by_name(user_id, canonical, &exact);

         if (exact_rc == MEMORY_DB_SUCCESS) {
            /* Exact match — use existing entity */
            entity_id = exact.id;
         } else {
            /* No exact match — search for similar entities */
            memory_entity_t similar[5];
            int sim_count = 0;
            memory_db_entity_search(user_id, entity_name, similar, 5, &sim_count);

            /* Filter to person entities only */
            int person_count = 0;
            memory_entity_t persons[5];
            for (int i = 0; i < sim_count; i++) {
               if (strcmp(similar[i].entity_type, "person") == 0) {
                  persons[person_count++] = similar[i];
               }
            }

            if (person_count > 0) {
               /* Similar people found — return disambiguation prompt */
               char *buf = malloc(2048);
               if (!buf)
                  return strdup("Error: memory allocation failed");
               int pos = snprintf(buf, 2048,
                                  "Similar people already exist. Which person should receive "
                                  "this contact info?\n");
               for (int i = 0; i < person_count && pos < 1800; i++) {
                  pos += snprintf(buf + pos, 2048 - pos, "[%d] %s (%d mentions, id:%lld)\n", i + 1,
                                  persons[i].name, persons[i].mention_count,
                                  (long long)persons[i].id);
               }
               pos += snprintf(buf + pos, 2048 - pos,
                               "[%d] Create new person '%s'\n\n"
                               "Call save_contact again with entity_id parameter set to the "
                               "chosen person's id, or set entity_id to 0 to create new.",
                               person_count + 1, entity_name);
               return buf;
            }

            /* No similar people — create new entity */
            bool created = false;
            if (memory_db_entity_upsert(user_id, entity_name, "person", canonical, &created,
                                        &entity_id) != MEMORY_DB_SUCCESS)
               return strdup("Error: failed to create entity");
         }
      }

      /* entity_id == 0 means "create new" from disambiguation */
      if (entity_id == 0) {
         char canonical[64];
         memory_make_canonical_name(entity_name, canonical, sizeof(canonical));
         bool created = false;
         if (memory_db_entity_upsert(user_id, entity_name, "person", canonical, &created,
                                     &entity_id) != MEMORY_DB_SUCCESS)
            return strdup("Error: failed to create entity");
      }

      if (contacts_add(user_id, entity_id, field_type, contact_value, label) != 0)
         return strdup("Error: failed to save contact information");

      char *result = malloc(512);
      if (result)
         snprintf(result, 512, "Saved %s %s for %s%s%s%s", field_type, contact_value, entity_name,
                  label[0] ? " (" : "", label, label[0] ? ")" : "");
      return result ? result : strdup("Contact saved.");
   } else if (strcmp(actionName, "find_contact") == 0) {
      if (!value || !value[0])
         return strdup("Error: find_contact requires a name to search for");

      char name[128] = "";
      tool_param_extract_base(value, name, sizeof(name));

      char field_type[32] = "";
      tool_param_extract_custom(value, "field_type", field_type, sizeof(field_type));

      contact_result_t results[10];
      int count = 0;
      contacts_find(user_id, name[0] ? name : value, field_type[0] ? field_type : NULL, results, 10,
                    &count);

      if (count <= 0)
         return strdup("No contacts found matching that name.");

      char *buf = malloc(2048);
      if (!buf)
         return strdup("Error: memory allocation failed");
      int pos = snprintf(buf, 2048, "Contact results (%d):\n", count);
      for (int i = 0; i < count && pos < 1900; i++) {
         pos += snprintf(buf + pos, 2048 - pos, "- %s: %s = %s%s%s%s [id:%lld]\n",
                         results[i].entity_name, results[i].field_type, results[i].value,
                         results[i].label[0] ? " (" : "", results[i].label,
                         results[i].label[0] ? ")" : "", (long long)results[i].contact_id);
      }
      return buf;
   } else if (strcmp(actionName, "list_contacts") == 0) {
      char field_type[32] = "";
      if (value)
         tool_param_extract_base(value, field_type, sizeof(field_type));

      contact_result_t results[20];
      int count = 0;
      contacts_list(user_id, field_type[0] ? field_type : NULL, results, 20, 0, &count);

      if (count <= 0)
         return strdup("No contacts stored.");

      char *buf = malloc(4096);
      if (!buf)
         return strdup("Error: memory allocation failed");
      int pos = snprintf(buf, 4096, "All contacts (%d):\n", count);
      for (int i = 0; i < count && pos < 3900; i++) {
         pos += snprintf(buf + pos, 4096 - pos, "- %s: %s = %s%s%s%s\n", results[i].entity_name,
                         results[i].field_type, results[i].value, results[i].label[0] ? " (" : "",
                         results[i].label, results[i].label[0] ? ")" : "");
      }
      return buf;
   } else if (strcmp(actionName, "merge_entities") == 0) {
      if (!value || !value[0])
         return strdup("Error: merge_entities requires source_name (query) and target_name");

      char source_name[128] = "";
      tool_param_extract_base(value, source_name, sizeof(source_name));

      char target_name[128] = "";
      tool_param_extract_custom(value, "target_name", target_name, sizeof(target_name));

      if (!source_name[0] || !target_name[0])
         return strdup("Error: merge_entities requires both source_name (query) and target_name");

      /* Look up both entities by canonical name */
      char src_canonical[64], tgt_canonical[64];
      memory_make_canonical_name(source_name, src_canonical, sizeof(src_canonical));
      memory_make_canonical_name(target_name, tgt_canonical, sizeof(tgt_canonical));

      memory_entity_t src_entity, tgt_entity;
      if (memory_db_entity_get_by_name(user_id, src_canonical, &src_entity) != MEMORY_DB_SUCCESS)
         return strdup("Error: source entity not found");
      if (memory_db_entity_get_by_name(user_id, tgt_canonical, &tgt_entity) != MEMORY_DB_SUCCESS)
         return strdup("Error: target entity not found");

      int rc = memory_db_entity_merge(user_id, src_entity.id, tgt_entity.id);
      if (rc == MEMORY_DB_SUCCESS) {
         char *result = malloc(768);
         if (result)
            snprintf(result, 768,
                     "Merged '%s' into '%s'. The '%s' entity has been deleted; "
                     "all its relations and contacts were transferred to '%s'. "
                     "Use '%s' for future queries.",
                     source_name, target_name, source_name, target_name, target_name);
         return result ? result : strdup("Entities merged successfully.");
      } else if (rc == MEMORY_DB_NOT_FOUND) {
         /* Race: entity deleted between lookup and merge */
         OLOG_WARNING("memory_callback: merge_entities race — entity vanished between lookup and "
                      "merge (source='%s', target='%s')",
                      source_name, target_name);
         return strdup("Error: one or both entities no longer exist (may have been deleted)");
      } else {
         return strdup("Error: merge operation failed");
      }
   } else if (strcmp(actionName, "delete_contact") == 0) {
      if (!value || !value[0])
         return strdup("Error: delete_contact requires a contact ID");

      char id_str[32] = "";
      tool_param_extract_base(value, id_str, sizeof(id_str));
      int64_t contact_id = strtoll(id_str[0] ? id_str : value, NULL, 10);
      if (contact_id <= 0)
         return strdup("Error: invalid contact ID");

      if (contacts_delete(user_id, contact_id) != 0)
         return strdup("Error: contact not found or already deleted");

      return strdup("Contact deleted.");
   } else {
      char *msg = malloc(128);
      if (msg) {
         snprintf(msg, 128, "Unknown memory action: '%s'", actionName);
         return msg;
      }
      return strdup("Unknown memory action.");
   }
}
