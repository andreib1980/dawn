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

#include "config/dawn_config.h"
#include "core/embedding_engine.h"
#include "dawn_error.h"
#include "logging.h"
#include "memory/memory_db.h"
#include "memory/memory_embeddings.h"
#include "memory/memory_graph_retrieval.h"

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

   /* 2. Keyword search.
    *
    * Two paths, gated by `[memory] bm25_enabled`:
    *   - On (Phase 1 of the Mem0 parity program): one FTS5 MATCH call,
    *     BM25-ranked, sigmoid-normalized to [0, 1].  No fan-out per
    *     token, no LIKE pattern building, no match-count integer step.
    *   - Off (legacy): tokenize → multi-LIKE → per-token result merge,
    *     scoring by token-coverage match count.
    *
    * Both paths feed the same `hybrid_search` downstream — we keep its
    * int*+token_count signature by mapping BM25 [0, 1] floats onto the
    * (match_count / token_count) shape (multiply by 100, fix
    * token_count=100).  kw_score downstream comes out identical to the
    * normalized BM25 value. */
   memory_fact_t kw_facts[HYBRID_FETCH_LIMIT];
   int kw_scores[HYBRID_FETCH_LIMIT];
   int kw_count = 0;
   int kw_score_denom = (token_count > 0) ? token_count : 1;

   if (g_config.memory.bm25_enabled) {
      /* BM25 path — single FTS5 MATCH replaces the legacy tokenize →
       * multi-LIKE fan-out → match-count merge.  Time-windowed callers
       * dispatch to the `_since` variant (since_ts > 0) so focus-adapter
       * recent windows also get BM25 ranking instead of silently falling
       * back to LIKE. */
      float bm25_scores[HYBRID_FETCH_LIMIT];
      memory_db_fact_search_bm25_since(user_id, query, since_ts, kw_facts, bm25_scores, work_cap,
                                       &kw_count);
      /* Map [0,1] BM25 score → (int, denom=100) so hybrid_search's kw
       * normalization (`match/denom`) emits the same float. */
      kw_score_denom = 100;
      for (int i = 0; i < kw_count; i++) {
         int s = (int)(bm25_scores[i] * 100.0f + 0.5f);
         if (s < 1)
            s = 1;
         if (s > 100)
            s = 100;
         kw_scores[i] = s;
      }
   } else if (token_count <= 1) {
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
      int hybrid_count;
      if (g_config.memory.rrf_enabled) {
         hybrid_count = memory_embeddings_rrf_search(user_id, query, kw_ids, kw_scores, kw_count,
                                                     kw_score_denom, hybrid, work_cap);
      } else {
         hybrid_count = memory_embeddings_hybrid_search(user_id, query, kw_ids, kw_scores, kw_count,
                                                        kw_score_denom, hybrid, work_cap);
      }
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

int memory_search_execute(int user_id,
                          const char *query,
                          time_t since_ts,
                          memory_fact_t *out_facts,
                          float *out_scores,
                          int max,
                          int *out_count) {
   if (out_count != NULL)
      *out_count = 0;
   if (out_facts == NULL || out_scores == NULL || max <= 0)
      return FAILURE;
   if (query == NULL || query[0] == '\0')
      return SUCCESS;
   /* Clamp to sized stack-merge pool. */
   if (max > MEMORY_SEARCH_HYBRID_MAX)
      max = MEMORY_SEARCH_HYBRID_MAX;

   /* Step 1: hybrid keyword + cosine retrieval against the full corpus.
    * The non-NULL `embed_gate` opts into hybrid re-rank when embeddings
    * are available; the pointer is never dereferenced (hybrid_search
    * re-embeds the query internally). */
   const float embed_gate = 0.0f;
   const float *gate_ptr = memory_embeddings_available() ? &embed_gate : NULL;
   int fact_count = 0;
   if (memory_fact_search_hybrid(user_id, query, gate_ptr, 0, since_ts, out_facts, out_scores, max,
                                 &fact_count) != SUCCESS) {
      fact_count = 0;
   }

   /* Step 2: entity-graph candidate expansion (Phase 1A).  Adds candidates
    * the cosine pool may have ranked too low for top-K. */
   if (g_config.memory.graph_retrieval.enabled) {
      int64_t seeds[MEMORY_GRAPH_MAX_SEED_CANDIDATES];
      int seed_count = 0;
      memory_graph_extract_seed_entities(user_id, query, seeds, MEMORY_GRAPH_MAX_SEED_CANDIDATES,
                                         &seed_count);
      if (seed_count > 0) {
         /* Widen the intermediate candidate pool past the LLM-facing cap
          * so query-scoring sees the full entity-bounded set.  Final
          * truncate to @p max happens after the merge below. */
         memory_fact_t graph_facts[MEMORY_GRAPH_INTERMEDIATE_CAP];
         float graph_scores[MEMORY_GRAPH_INTERMEDIATE_CAP];
         int graph_count = 0;
         int per_query_cap = g_config.memory.graph_retrieval.max_facts_per_query;
         if (per_query_cap > MEMORY_GRAPH_INTERMEDIATE_CAP)
            per_query_cap = MEMORY_GRAPH_INTERMEDIATE_CAP;
         memory_graph_expand_fact_linked(user_id, seeds, seed_count, graph_facts, graph_scores,
                                         per_query_cap, &graph_count);

         /* Step 3: Phase 2 Step 1 query-aware scoring of graph candidates.
          * Replaces the legacy flat entity_grounding_bonus with
          * vec_weight*cosine + entity_bonus so topically-irrelevant
          * entity-grounded facts don't unfairly compete with hybrid hits.
          * Toggle via g_config.memory.graph_retrieval.use_query_scoring. */
         if (g_config.memory.graph_retrieval.use_query_scoring && graph_count > 0) {
            int64_t graph_fact_ids[MEMORY_GRAPH_INTERMEDIATE_CAP];
            for (int i = 0; i < graph_count; i++)
               graph_fact_ids[i] = graph_facts[i].id;
            /* Embed query once and thread through the rescore so we don't
             * pay a second ONNX inference on the hot path. */
            float query_emb[MAX_EMBEDDING_DIMS];
            int qdims = 0;
            float query_norm = 0.0f;
            if (memory_embeddings_available() &&
                memory_embeddings_embed(query, query_emb, &qdims) == 0 &&
                qdims == embedding_engine_dims()) {
               query_norm = memory_embeddings_l2_norm(query_emb, qdims);
            }
            memory_embeddings_rescore_against_query(user_id, query_norm >= 1e-6f ? query_emb : NULL,
                                                    query_norm,
                                                    g_config.memory.graph_retrieval.entity_bonus,
                                                    graph_fact_ids, graph_count, graph_scores);
            /* Drop sentinel-scored facts per the rescore contract. */
            int kept = 0;
            for (int i = 0; i < graph_count; i++) {
               if (graph_scores[i] > MEMORY_EMBEDDINGS_RESCORE_SENTINEL) {
                  if (kept != i) {
                     graph_facts[kept] = graph_facts[i];
                     graph_scores[kept] = graph_scores[i];
                  }
                  kept++;
               }
            }
            graph_count = kept;
         }

         /* Step 4: score-based merge.  Union(hybrid, graph) → sort by
          * score desc → truncate to @p max.  Allows high-scoring graph
          * candidates to displace weak hybrid tail entries. */
         memory_fact_t merged[MEMORY_SEARCH_HYBRID_MAX + MEMORY_GRAPH_INTERMEDIATE_CAP];
         float merged_scores[MEMORY_SEARCH_HYBRID_MAX + MEMORY_GRAPH_INTERMEDIATE_CAP];
         int merged_count = fact_count;
         for (int i = 0; i < fact_count; i++) {
            merged[i] = out_facts[i];
            merged_scores[i] = out_scores[i];
         }
         /* Append novel graph candidates (dedup against hybrid pool). */
         for (int i = 0; i < graph_count; i++) {
            bool dup = false;
            for (int j = 0; j < fact_count; j++) {
               if (out_facts[j].id == graph_facts[i].id) {
                  dup = true;
                  break;
               }
            }
            if (!dup) {
               merged[merged_count] = graph_facts[i];
               merged_scores[merged_count] = graph_scores[i];
               merged_count++;
            }
         }
         /* Insertion sort merged pool by score descending (small N ≤ 40). */
         for (int i = 1; i < merged_count; i++) {
            memory_fact_t tmp_fact = merged[i];
            float tmp_score = merged_scores[i];
            int j = i - 1;
            while (j >= 0 && merged_scores[j] < tmp_score) {
               merged[j + 1] = merged[j];
               merged_scores[j + 1] = merged_scores[j];
               j--;
            }
            merged[j + 1] = tmp_fact;
            merged_scores[j + 1] = tmp_score;
         }
         /* Truncate to @p max — final LLM-facing pool. */
         const int final_count = merged_count > max ? max : merged_count;
         for (int i = 0; i < final_count; i++) {
            out_facts[i] = merged[i];
            out_scores[i] = merged_scores[i];
         }
         fact_count = final_count;
      }
   }

   /* Step 5: "I don't remember" gate — drop marginal-cosine hits. */
   const float score_floor = g_config.memory.search_score_floor;
   const int before = fact_count;
   fact_count = memory_search_apply_score_floor(out_facts, out_scores, fact_count, score_floor);
   if (fact_count < before) {
      OLOG_DEBUG("memory_search_execute: floor=%.2f dropped %d/%d facts for query '%s'",
                 score_floor, before - fact_count, before, query);
   }

   *out_count = fact_count;
   return SUCCESS;
}
