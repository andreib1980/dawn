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
 * Focus-source framework — registry, ranker, filter-on-retrieval pipeline.
 *
 * Phase 1b of the Dynamic Context Injection workstream — pure infrastructure.
 * No real adapters land here; 1c/1d ship the memory / document / calendar /
 * email adapters that register via `focus_register_source()`.
 */

#include "memory/focus_source.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "config/dawn_config.h"
#include "dawn_error.h"
#include "logging.h"
#include "memory/focus_source_internal.h"
#include "memory/memory_filter.h"

/* =============================================================================
 * Registry — fixed-size linear array.
 *
 * Linear-array (not FNV-1a hash like `tool_registry`): `focus_compose()`
 * iterates ALL adapters every call, no name-keyed hot lookup.  N is small
 * (~8-10 in production); a hash table would be premature optimization.
 *
 * Register-once semantics: production code calls `focus_register_source()`
 * exactly once per source at daemon init.  The mutex protects the
 * register path; after init completes, the registry is logically
 * read-only and `focus_compose()` is reentrant + lock-free for
 * iteration.  Tests use `focus_unregister_all()` (test-only) in setUp().
 * ============================================================================= */

static const focus_source_adapter_t *s_registry[MAX_FOCUS_SOURCES];
static int s_registry_count = 0;
static pthread_mutex_t s_registry_mutex = PTHREAD_MUTEX_INITIALIZER;

/* =============================================================================
 * Public API
 * ============================================================================= */

int focus_register_source(const focus_source_adapter_t *adapter) {
   if (adapter == NULL || adapter->source_id == NULL || adapter->query == NULL) {
      OLOG_ERROR("focus_source: register rejected — NULL adapter/source_id/query");
      return FAILURE;
   }

   pthread_mutex_lock(&s_registry_mutex);

   /* Reject duplicate source_id (no silent overwrite). */
   for (int i = 0; i < s_registry_count; i++) {
      if (strcmp(s_registry[i]->source_id, adapter->source_id) == 0) {
         pthread_mutex_unlock(&s_registry_mutex);
         OLOG_WARNING("focus_source: register rejected — duplicate source_id='%s'",
                      adapter->source_id);
         return FAILURE;
      }
   }

   if (s_registry_count >= MAX_FOCUS_SOURCES) {
      pthread_mutex_unlock(&s_registry_mutex);
      OLOG_ERROR("focus_source: register rejected — registry full (cap=%d)", MAX_FOCUS_SOURCES);
      return FAILURE;
   }

   s_registry[s_registry_count++] = adapter;
   pthread_mutex_unlock(&s_registry_mutex);

   OLOG_INFO("focus_source: registered source_id='%s' type=%d requires_embedding=%d",
             adapter->source_id, (int)adapter->source_type, (int)adapter->requires_embedding);
   return SUCCESS;
}

void focus_unregister_all(void) {
   pthread_mutex_lock(&s_registry_mutex);
   memset(s_registry, 0, sizeof(s_registry));
   s_registry_count = 0;
   pthread_mutex_unlock(&s_registry_mutex);
}

/* =============================================================================
 * Helpers
 * ============================================================================= */

static void candidate_release(focus_candidate_t *c) {
   if (c == NULL)
      return;
   free(c->text);
   c->text = NULL;
   free(c->item_id);
   c->item_id = NULL;
}

/* Increment the rejection counter for `source_id`.  Linear scan + strcmp
 * (no pointer-equality shortcut): a future adapter that heap-duplicates
 * its source_id string would silently spawn duplicate slot entries
 * under a pointer-equality optimization, exhausting the slot table and
 * letting a misbehaving adapter suppress the UI warning chip.  N≤16 so
 * the strcmp cost is irrelevant. */
static void rejection_bump(focus_compose_result_t *result, const char *source_id) {
   for (int i = 0; i < result->rejection_count; i++) {
      if (result->rejections[i].source_id != NULL &&
          strcmp(result->rejections[i].source_id, source_id) == 0) {
         result->rejections[i].count++;
         return;
      }
   }
   if (result->rejection_count >= MAX_FOCUS_SOURCES)
      return;
   result->rejections[result->rejection_count].source_id = source_id;
   result->rejections[result->rejection_count].count = 1;
   result->rejection_count++;
}

static float score_or_zero(float v) {
   return (v == FOCUS_SCORE_NA) ? 0.0f : v;
}

/* Per-source weight lookup, called ONCE per adapter per compose pass and
 * cached into `compute_ctx_t.adapter_weights[]` for the candidate-scoring
 * loop.  Unknown source_id resolves to 1.0 with a single per-call WARN —
 * the warn-once-per-process state used to live in a static array but
 * caching at compose-call scope avoids the lock-free reentrant
 * thread-safety hole that produced. */
static float lookup_source_weight(const char *source_id, bool *out_unknown) {
   const focus_source_weights_t *w = &g_config.memory.focus_injection.source_weights;
   *out_unknown = false;
   if (strcmp(source_id, "memory_fact") == 0)
      return w->memory_fact;
   if (strcmp(source_id, "memory_entity") == 0)
      return w->memory_entity;
   if (strcmp(source_id, "memory_relation") == 0)
      return w->memory_relation;
   if (strcmp(source_id, "memory_summary") == 0)
      return w->memory_summary;
   if (strcmp(source_id, "document_chunk") == 0)
      return w->document_chunk;
   if (strcmp(source_id, "calendar_event") == 0)
      return w->calendar_event;
   if (strcmp(source_id, "recent_email") == 0)
      return w->recent_email;
   if (strcmp(source_id, "dawn_background") == 0)
      return w->dawn_background;

   *out_unknown = true;
   return 1.0f;
}

static float compute_final_score(const focus_candidate_t *c, float source_weight) {
   const focus_injection_config_t *fi = &g_config.memory.focus_injection;
   return fi->weight_semantic * score_or_zero(c->semantic_score) +
          fi->weight_recency * score_or_zero(c->recency_score) +
          fi->weight_importance * c->importance_score + fi->weight_source * source_weight;
}

/* Sort key bundle: keeps `focus_candidate_t` itself sortable in place
 * via a parallel score array.  Avoids re-running the score formula on
 * every comparator call. */
typedef struct {
   int idx;
   float score;
   time_t ts;
} ranker_entry_t;

static int ranker_cmp(const void *a, const void *b) {
   const ranker_entry_t *x = (const ranker_entry_t *)a;
   const ranker_entry_t *y = (const ranker_entry_t *)b;
   /* Descending by score. */
   if (x->score < y->score)
      return 1;
   if (x->score > y->score)
      return -1;
   /* Tie-break: descending by item_timestamp (newer first). */
   if (x->ts < y->ts)
      return 1;
   if (x->ts > y->ts)
      return -1;
   return 0;
}

/* Approximate token cost.  English: ~4 chars/token; UTF-8 multibyte
 * undercounts (~15% English, worse for emoji-heavy content).
 * Acceptable for v1; 1j tightens after bench shows actual budget
 * breaches. */
static int approx_token_cost(const char *text) {
   if (text == NULL)
      return 0;
   size_t n = strlen(text);
   return (int)((n + 3) / 4);
}

/* =============================================================================
 * focus_compose — pipeline
 * ============================================================================= */

int focus_compose(int user_id,
                  bool include_private,
                  const char *query_text,
                  const float *query_embedding,
                  size_t embed_dim,
                  time_t now,
                  int per_source_max_candidates,
                  focus_compose_result_t *out_result) {
   if (out_result == NULL)
      return FAILURE;

   /* Initialize result first — caller may pass an uninitialized struct. */
   out_result->candidates = NULL;
   out_result->candidate_count = 0;
   out_result->rejection_count = 0;
   memset(out_result->rejections, 0, sizeof(out_result->rejections));

   if (per_source_max_candidates <= 0)
      per_source_max_candidates = 1;

   /* Snapshot the registry count ONCE.  Production registers at daemon
    * init and never unregisters; pinning here keeps `worst_case` and
    * the iteration bound consistent even if a late register slips in. */
   const int n_sources = s_registry_count;

   /* Working pool sized to the worst case: every adapter returns its cap. */
   const int worst_case = n_sources * per_source_max_candidates;
   if (worst_case == 0) {
      /* Empty registry — production-1b state.  SUCCESS, zero results. */
      return SUCCESS;
   }

   focus_candidate_t *pool = calloc((size_t)worst_case, sizeof(*pool));
   if (pool == NULL) {
      OLOG_ERROR("focus_source: OOM allocating compose pool (worst_case=%d)", worst_case);
      return FAILURE;
   }
   /* Per-pool-entry source weight, populated as we copy from each adapter.
    * Avoids re-doing the source_id strcmp ladder once per candidate at
    * scoring time (was 8 strcmps × N_candidates per turn). */
   float *pool_weights = calloc((size_t)worst_case, sizeof(*pool_weights));
   if (pool_weights == NULL) {
      OLOG_ERROR("focus_source: OOM allocating pool_weights (worst_case=%d)", worst_case);
      free(pool);
      return FAILURE;
   }
   int pool_count = 0;

   for (int s = 0; s < n_sources; s++) {
      const focus_source_adapter_t *a = s_registry[s];
      if (a == NULL)
         continue;

      if (a->requires_embedding && query_embedding == NULL)
         continue;

      /* Resolve the per-adapter source weight ONCE.  Constant for every
       * candidate this adapter emits — no per-candidate strcmp. */
      bool unknown_source = false;
      const float adapter_weight = lookup_source_weight(a->source_id, &unknown_source);
      if (unknown_source) {
         OLOG_WARNING(
             "focus_source: no source_weight entry for source_id='%s' — using 1.0 fallback",
             a->source_id);
      }

      focus_candidate_t *adapter_out = NULL;
      int adapter_count = 0;
      int rc = a->query(user_id, include_private, query_text, query_embedding, embed_dim, now,
                        per_source_max_candidates, &adapter_out, &adapter_count);
      if (rc != SUCCESS) {
         OLOG_WARNING("focus_source: adapter '%s' returned FAILURE — skipping (registry intact)",
                      a->source_id);
         continue;
      }

      OLOG_DEBUG("focus_source: adapter '%s' returned %d candidates (cap=%d)", a->source_id,
                 adapter_count, per_source_max_candidates);

      /* Filter-on-retrieval (UNCONDITIONAL — even if adapter pre-filtered)
       * + defensive cap on per-source survivor count. */
      int survived = 0;
      const int survive_cap = per_source_max_candidates;
      for (int i = 0; i < adapter_count; i++) {
         focus_candidate_t *c = &adapter_out[i];

         /* NULL or empty text is dropped silently — adapter contract
          * violation, but failing closed is the right move. */
         if (c->text == NULL || c->text[0] == '\0') {
            candidate_release(c);
            continue;
         }

         if (memory_filter_check(c->text)) {
            /* Counter only — never log the offending text. */
            rejection_bump(out_result, a->source_id);
            candidate_release(c);
            continue;
         }

         if (survived >= survive_cap) {
            /* Adapter exceeded its hint; silently drop the excess. */
            candidate_release(c);
            continue;
         }

         /* Defensive overflow guard: survive_cap could in principle be
          * tuned per-adapter in the future, in which case `pool_count`
          * could push past `worst_case`.  Today (single survive_cap)
          * this branch is unreachable — but writing it now avoids a
          * 1-line OOB on the day someone adjusts the cap. */
         if (pool_count >= worst_case) {
            candidate_release(c);
            continue;
         }

         /* Stable copy into the working pool — overwrite source_id with
          * the adapter's static string just in case.  Hidden invariant:
          * one adapter == one source_id; the framework collapses any
          * candidate-supplied source_id back to the registered value. */
         pool[pool_count] = *c;
         pool[pool_count].source_id = a->source_id;
         pool[pool_count].source_type = a->source_type;
         pool_weights[pool_count] = adapter_weight;
         /* Adapter buffer no longer owns text/item_id; zero so a stray
          * later free won't double-free. */
         c->text = NULL;
         c->item_id = NULL;
         pool_count++;
         survived++;
      }

      if (survived < adapter_count) {
         OLOG_DEBUG("focus_source: adapter '%s' — %d/%d candidates survived filter+cap",
                    a->source_id, survived, adapter_count);
      }

      free(adapter_out);

      if (pool_count >= worst_case)
         break; /* Defensive — should not happen given the worst-case sizing. */
   }

   if (pool_count == 0) {
      free(pool);
      free(pool_weights);
      return SUCCESS;
   }

   /* =====================================================================
    * Rank
    * ===================================================================== */
   ranker_entry_t *order = calloc((size_t)pool_count, sizeof(*order));
   if (order == NULL) {
      OLOG_ERROR("focus_source: OOM allocating ranker order (pool_count=%d)", pool_count);
      for (int i = 0; i < pool_count; i++)
         candidate_release(&pool[i]);
      free(pool);
      free(pool_weights);
      return FAILURE;
   }
   for (int i = 0; i < pool_count; i++) {
      order[i].idx = i;
      order[i].score = compute_final_score(&pool[i], pool_weights[i]);
      order[i].ts = pool[i].item_timestamp;
   }
   qsort(order, (size_t)pool_count, sizeof(*order), ranker_cmp);

   /* =====================================================================
    * Trim — min_score, top_k, token-budget
    * ===================================================================== */
   const focus_injection_config_t *fi = &g_config.memory.focus_injection;
   int kept = 0;
   int budget_left = fi->focus_budget_tokens;
   const int top_k = (fi->top_k > 0) ? fi->top_k : pool_count;

   /* `keep[]` marks pool indices that survive trimming. */
   bool *keep = calloc((size_t)pool_count, sizeof(*keep));
   if (keep == NULL) {
      OLOG_ERROR("focus_source: OOM allocating keep mask (pool_count=%d)", pool_count);
      free(order);
      for (int i = 0; i < pool_count; i++)
         candidate_release(&pool[i]);
      free(pool);
      free(pool_weights);
      return FAILURE;
   }

   for (int rank = 0; rank < pool_count && kept < top_k; rank++) {
      const ranker_entry_t *e = &order[rank];
      if (e->score < fi->min_score)
         break; /* Sorted desc — once below, all rest are below. */

      const int cost = approx_token_cost(pool[e->idx].text);
      if (cost > budget_left) {
         /* Budget exceeded.  Subtle case: when this is the FIRST
          * candidate (kept == 0), a strict break produces an empty
          * focus block even though the highest-ranked item barely
          * exceeded the budget.  At FOCUS_TEXT_MAX_BYTES = 4096
          * (≈ 1024 tokens), a single max-sized candidate plus the
          * "[source_id] " framing prefix can land just above the
          * default 1024-token budget; dropping it leaves the LLM
          * with no focus context at all.  Force-keep the first
          * candidate (the per-candidate truncation cap already
          * bounds it) and let the next iteration cleanly break on
          * the second candidate's cost.  Subsequent candidates
          * honor the budget normally.
          *
          * Termination invariant: focus_candidate_init rejects NULL
          * / empty text, so every surviving candidate has length ≥ 1
          * and approx_token_cost ≥ 1.  After saturating budget_left
          * to 0, the next iteration's `cost > budget_left` is true
          * and the regular `break` fires — no infinite loop. */
         if (kept == 0) {
            OLOG_INFO("focus_source: top-ranked candidate cost=%d exceeds budget=%d — "
                      "force-keeping single candidate (source='%s')",
                      cost, budget_left, pool[e->idx].source_id);
            keep[e->idx] = true;
            budget_left = 0;
            kept++;
            continue;
         }
         /* Truncate at last fully-included candidate.  Don't consume
          * partial budget — keeps the output coherent. */
         break;
      }
      keep[e->idx] = true;
      budget_left -= cost;
      kept++;
   }

   /* Build the final ordered output array.  Walk `order[]` so the
    * candidate array preserves rank order. */
   focus_candidate_t *out = NULL;
   if (kept > 0) {
      out = calloc((size_t)kept, sizeof(*out));
      if (out == NULL) {
         OLOG_ERROR("focus_source: OOM allocating result candidates (kept=%d)", kept);
         free(keep);
         free(order);
         for (int i = 0; i < pool_count; i++)
            candidate_release(&pool[i]);
         free(pool);
         free(pool_weights);
         return FAILURE;
      }
   }

   int out_idx = 0;
   for (int rank = 0; rank < pool_count; rank++) {
      const int p = order[rank].idx;
      if (keep[p]) {
         out[out_idx++] = pool[p];
         /* Transfer ownership: zero the pool slot so the trim sweep
          * below skips it. */
         pool[p].text = NULL;
         pool[p].item_id = NULL;
      }
   }

   /* Free pool entries that didn't make the cut. */
   for (int i = 0; i < pool_count; i++)
      candidate_release(&pool[i]);

   free(keep);
   free(order);
   free(pool);
   free(pool_weights);

   out_result->candidates = out;
   out_result->candidate_count = kept;

   OLOG_DEBUG("focus_source: composed %d candidates (kept %d/%d after trim)", kept, kept,
              pool_count);

   return SUCCESS;
}

void focus_result_free(focus_compose_result_t *result) {
   if (result == NULL)
      return;
   if (result->candidates != NULL) {
      for (int i = 0; i < result->candidate_count; i++)
         candidate_release(&result->candidates[i]);
      free(result->candidates);
   }
   result->candidates = NULL;
   result->candidate_count = 0;
   memset(result->rejections, 0, sizeof(result->rejections));
   result->rejection_count = 0;
}
