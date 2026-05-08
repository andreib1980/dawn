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
 * Memory fact-search pipeline — extracted from memory_callback.c
 * (Phase 1c-i).  Single source of truth for the tokenize → multi-token
 * keyword → hybrid embedding re-rank flow used by both the legacy
 * `memoryCallback` recall path and the new focus-source fact adapter.
 */

#include "memory/memory_fact_search.h"

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dawn_error.h"
#include "logging.h"
#include "memory/memory_db.h"
#include "memory/memory_embeddings.h"

/* Token bookkeeping caps preserved verbatim from the original
 * `multi_token_fact_search` helper — bounded so the pipeline stays
 * stack-allocated. */
#define MAX_SEARCH_TOKENS 8
#define MAX_DEDUP_RESULTS 30
#define KEYWORD_FETCH_LIMIT 10
#define HYBRID_FETCH_LIMIT 10

/* Tokenize the query into lowercase 2+ char strings, splitting on the
 * same delimiter set the production extraction path uses. */
static int tokenize_query(const char *keywords, char tokens[][64], int max_tokens) {
   if (keywords == NULL || max_tokens <= 0)
      return 0;

   char buf[512];
   strncpy(buf, keywords, sizeof(buf) - 1);
   buf[sizeof(buf) - 1] = '\0';

   for (size_t i = 0; buf[i]; i++)
      buf[i] = (char)tolower((unsigned char)buf[i]);

   int count = 0;
   char *saveptr = NULL;
   char *tok = strtok_r(buf, " \t\n\r,.;:!?\"'()[]{}/-", &saveptr);
   while (tok != NULL && count < max_tokens) {
      if (strlen(tok) > 1) {
         snprintf(tokens[count], 64, "%s", tok);
         count++;
      }
      tok = strtok_r(NULL, " \t\n\r,.;:!?\"'()[]{}/-", &saveptr);
   }
   return count;
}

/* Multi-token keyword search with per-fact match-count scoring.
 * Insertion-sorts by score desc, then confidence desc.  Fills
 * `out_scores` with raw match counts (parallel to `results`). */
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
      memory_fact_t token_results[KEYWORD_FETCH_LIMIT];
      int limit = per_token_limit > KEYWORD_FETCH_LIMIT ? KEYWORD_FETCH_LIMIT : per_token_limit;
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
      if (out_scores != NULL)
         out_scores[i] = seen_scores[i];
   }
   return count;
}

int memory_fact_search_hybrid(int user_id,
                              const char *query,
                              const float *query_embedding,
                              size_t embed_dim,
                              time_t since_ts,
                              memory_fact_t *out_facts,
                              float *out_scores,
                              int max,
                              int *out_count) {
   (void)embed_dim; /* hybrid_search re-derives from the engine */
   if (out_count != NULL)
      *out_count = 0;
   if (out_facts == NULL || out_scores == NULL || out_count == NULL || max <= 0)
      return FAILURE;
   if (query == NULL || query[0] == '\0')
      return SUCCESS;

   /* Cap working buffers at HYBRID_FETCH_LIMIT — both keyword and hybrid
    * intermediates use the same bound to keep stack frames compact. */
   const int work_cap = (max > HYBRID_FETCH_LIMIT) ? HYBRID_FETCH_LIMIT : max;

   /* 1. Tokenize. */
   char tokens[MAX_SEARCH_TOKENS][64];
   int token_count = tokenize_query(query, tokens, MAX_SEARCH_TOKENS);

   /* 2. Keyword search. */
   memory_fact_t kw_facts[HYBRID_FETCH_LIMIT];
   int kw_scores[HYBRID_FETCH_LIMIT];
   int kw_count = 0;
   if (token_count <= 1) {
      /* Single-token (or zero — `query` was non-empty but contained
       * only delimiters): single SQL call mirrors the existing path. */
      if (since_ts > 0)
         memory_db_fact_search_since(user_id, query, since_ts, kw_facts, work_cap, &kw_count);
      else
         memory_db_fact_search(user_id, query, kw_facts, work_cap, &kw_count);
      for (int i = 0; i < kw_count; i++)
         kw_scores[i] = 1;
   } else {
      kw_count = multi_token_fact_search(user_id, tokens, token_count, KEYWORD_FETCH_LIMIT,
                                         kw_facts, work_cap, since_ts, kw_scores);
   }

   /* 3. Hybrid re-rank when embeddings are available AND caller provided
    *    a query embedding.  Both gates required: hybrid_search consumes
    *    a query string + cached query embedding; if the engine isn't
    *    ready we must not enter that path. */
   if (query_embedding != NULL && memory_embeddings_available()) {
      int64_t kw_ids[HYBRID_FETCH_LIMIT];
      for (int i = 0; i < kw_count; i++)
         kw_ids[i] = kw_facts[i].id;

      embedding_search_result_t hybrid[HYBRID_FETCH_LIMIT];
      int hybrid_count = memory_embeddings_hybrid_search(user_id, query, kw_ids, kw_scores,
                                                         kw_count,
                                                         (token_count > 0) ? token_count : 1,
                                                         hybrid, work_cap);
      if (hybrid_count > 0) {
         int produced = 0;
         for (int h = 0; h < hybrid_count && produced < max; h++) {
            bool found = false;
            for (int f = 0; f < kw_count; f++) {
               if (kw_facts[f].id == hybrid[h].fact_id) {
                  out_facts[produced] = kw_facts[f];
                  out_scores[produced] = hybrid[h].score;
                  produced++;
                  found = true;
                  break;
               }
            }
            if (!found) {
               /* Vector-only hit — fetch and DEFENSE-IN-DEPTH check that
                * the fetched fact really belongs to `user_id`.
                * `memory_db_fact_get` is NOT user-scoped: a regression
                * in hybrid_search's user scoping would otherwise let a
                * foreign fact through.  Skip on mismatch, log as ERROR. */
               memory_fact_t vec_fact;
               int rc = memory_db_fact_get(hybrid[h].fact_id, &vec_fact);
               if (rc != MEMORY_DB_SUCCESS)
                  continue;
               if (vec_fact.user_id != user_id) {
                  OLOG_ERROR("memory_fact_search: vector-only fact_id=%lld owned by user_id=%d "
                             "(expected %d) — skipping (defense-in-depth)",
                             (long long)hybrid[h].fact_id, vec_fact.user_id, user_id);
                  continue;
               }
               out_facts[produced] = vec_fact;
               out_scores[produced] = hybrid[h].score;
               produced++;
            }
         }
         *out_count = produced;
         return SUCCESS;
      }
      /* hybrid_count == 0 → fall through to keyword-only output. */
   }

   /* 4. Keyword-only fallback. */
   const int produced = (kw_count > max) ? max : kw_count;
   for (int i = 0; i < produced; i++) {
      out_facts[i] = kw_facts[i];
      out_scores[i] = (float)kw_scores[i];
   }
   *out_count = produced;
   return SUCCESS;
}
