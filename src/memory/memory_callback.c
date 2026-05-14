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

#include "auth/auth_db.h"
#include "config/dawn_config.h"
#include "core/session_manager.h"
#include "core/strbuf.h"
#include "logging.h"
#include "memory/contacts_db.h"
#include "memory/memory_callback_internal.h"
#include "memory/memory_db.h"
#include "memory/memory_db_aliases.h"
#include "memory/memory_db_provenance.h"
#include "memory/memory_embeddings.h"
#include "memory/memory_fact_search.h"
/* SOURCE_DEDUP_CAP / source_dedup_set_t / source_dedup_{seen,add} live in
 * memory_callback_internal.h so the unit tests can exercise them directly. */
#include "memory/memory_filter.h"
#include "memory/memory_similarity.h"
#include "memory/memory_types.h"
#include "mosquitto_comms.h"
#include "tools/time_utils.h"
#include "tools/tool_registry.h"

/* Shared char budget for with_source verbatim excerpts in a single tool call.
 * The response buffer is now growable (strbuf, capped at STRBUF_DEFAULT_MAX_CAP =
 * 256 KiB) so this is no longer a "stay under buffer size" invariant — it is
 * a soft cap on how much verbatim source text any one memory_callback call
 * can inject into the LLM context.  3072 chars ≈ 768 tokens; bench validation
 * (May 2026) showed 6144 / 12288 lift answer quality further but cost more
 * context window per call.  Tune via dawn.toml [memory] source_budget_chars
 * when production telemetry justifies it. */
#define MEMORY_SOURCE_BUDGET_CHARS 3072

/* Hard upper bound on per-call source_budget — prevents a misconfigured
 * caller (LLM tool param escape, future admin path, etc.) from injecting
 * an arbitrary amount of verbatim source text and dominating the LLM
 * context.  Defense-in-depth — the bench legitimately needs values up to
 * ~12 KiB for the budget sweep, so the cap is set well above that. */
#define MEMORY_SOURCE_BUDGET_MAX (32 * 1024)

/* =============================================================================
 * Helper: Append verbatim source excerpt for a (conv_id, msg-range) tuple
 * (with_source=true).
 *
 * Appends "[source: conv=N msgs=X-Y]\n<role>: <content>\n..." to buf.
 * Respects the shared budget; returns 1 if anything rendered, 0 otherwise.
 *
 * Dedup: when a non-NULL source_dedup_set_t is passed, repeated ranges emit a
 * single back-ref line ("[source: same as above ...]") instead of refetching
 * the message body.  Triples are added to the set ONLY after a successful
 * fetch through the privacy-filtered path — so a private upstream cannot
 * leak its existence as a "same as above" marker on a later public record.
 * ============================================================================= */

struct source_msg_ctx {
   strbuf_t *sb;
   int *budget;
};

/* source_dedup_seen / source_dedup_add live in memory_source_dedup.c so unit
 * tests can link them without the full memory_callback dependency cone. */

static int source_msg_callback(const conversation_message_t *msg, void *ctx_ptr) {
   struct source_msg_ctx *ctx = (struct source_msg_ctx *)ctx_ptr;
   if (!ctx || !ctx->sb || !ctx->budget || !msg || !msg->content || *ctx->budget <= 0)
      return 1; /* stop */
   /* Skip system/tool messages in verbatim excerpt */
   if (strcmp(msg->role, "user") != 0 && strcmp(msg->role, "assistant") != 0)
      return 0;
   int written = strbuf_appendf(ctx->sb, "%s: %.500s\n", msg->role, msg->content);
   if (written < 0) {
      /* STRBUF_E_OOM — buffer cap exhausted.  Drain budget so the caller's
       * outer loop also bails (memory_action_search checks strbuf_oom after
       * each excerpt, but zeroing budget short-circuits any other consumers
       * that watch budget rather than oom). */
      *ctx->budget = 0;
      return 1;
   }
   *ctx->budget -= written;
   return 0;
}

/* Core renderer — caller passes a (conv_id, range) triple already filtered
 * upstream (typically from a *_get_sources batch API which excludes private
 * conversations in SQL).  Caller is responsible for passing conv_id > 0;
 * conv_id = 0 means "no provenance" and we silently skip.
 *
 * `seen` may be NULL to disable dedup. */
static int append_source_excerpt_from_range(int user_id,
                                            int64_t conv_id,
                                            int64_t start_id,
                                            int64_t end_id,
                                            strbuf_t *sb,
                                            int *budget_remaining,
                                            source_dedup_set_t *seen) {
   if (!sb || !budget_remaining)
      return 0;
   if (*budget_remaining <= 0)
      return 0;
   if (conv_id <= 0)
      return 0;

   /* Dedup: emit a one-line back-ref instead of refetching identical messages.
    * `seen` is populated only with post-batch-filter triples (see Sec M1 in
    * PROVENANCE.md) so this path cannot leak existence of a private upstream. */
   if (source_dedup_seen(seen, conv_id, start_id, end_id)) {
      int hdr = strbuf_appendf(sb, "[source: same as above (conv=%lld msgs=%lld-%lld)]\n",
                               (long long)conv_id, (long long)start_id, (long long)end_id);
      if (hdr > 0)
         *budget_remaining -= hdr;
      return 1;
   }

   /* Header */
   int hdr = strbuf_appendf(sb, "[source: conv=%lld msgs=%lld-%lld]\n", (long long)conv_id,
                            (long long)start_id, (long long)end_id);
   if (hdr > 0)
      *budget_remaining -= hdr;

   /* Verbatim messages — cap range at 500 messages per spec */
   int64_t capped_end = (end_id - start_id > 500) ? start_id + 500 : end_id;
   struct source_msg_ctx msg_ctx = { sb, budget_remaining };
   conv_db_get_messages_by_range(conv_id, user_id, start_id, capped_end, /*include_private=*/false,
                                 source_msg_callback, &msg_ctx);

   /* Mark range as seen — only after a successful (privacy-filtered) fetch. */
   source_dedup_add(seen, conv_id, start_id, end_id);
   return 1;
}

/* (Phase B fold-in removed the single-fact `append_source_excerpt` wrapper —
 * all production paths batch-fetch sources via the `_get_sources` APIs and
 * call `append_source_excerpt_from_range` directly.  The single-record
 * `memory_db_fact_get_source` is still used by the WebUI memory panel
 * endpoint in webui_memory.c, which doesn't render the excerpt itself.) */

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

/* `multi_token_fact_search` was extracted to `src/memory/memory_fact_search.c`
 * (Phase 1c-i) so the focus-source fact adapter and the legacy recall path
 * share identical retrieval semantics.  `tokenize_query` stays here because
 * `append_graph_context` below also tokenizes the query for entity-keyword
 * fallback; promoting tokenize_query to a shared header would force
 * non-memory-callback callers to depend on this module's tokenization
 * conventions, which is not what we want at this scope. */

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

static void append_graph_context(int user_id,
                                 const char *keywords,
                                 int64_t as_of_ts,
                                 bool include_historical,
                                 strbuf_t *sb,
                                 bool with_source,
                                 int *source_budget,
                                 source_dedup_set_t *seen) {
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
      return;

   /* Show top 3 entities with their relations (skip entities with no relations) */
   int show_count = entity_count > 3 ? 3 : entity_count;
   bool header_written = false;

   /* Hoisted out of the entity loop so we don't pay 3× zero-init cost per
    * call.  reset to zeros only when we actually batch-fetch (embedded MED). */
   int64_t out_conv[8], out_start[8], out_end[8];
   int64_t rel_ids[8];

   for (int i = 0; i < show_count; i++) {
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
         if (strbuf_len(sb) > 0)
            strbuf_append(sb, "\n");
         strbuf_append(sb, "ENTITIES:\n");
         header_written = true;
      }

      strbuf_appendf(sb, "- %s (%s):\n", entity_names[i], entity_types[i]);

      /* Batch-fetch source ranges for this entity's outgoing relations so we
       * can render verbatim excerpts inline.  Caller decides whether to render
       * via with_source / *source_budget; we still skip the DB hit when
       * source rendering is disabled. */
      bool render_rel_source = with_source && source_budget && *source_budget > 0;
      if (render_rel_source && rel_count > 0) {
         /* Zero only the slots we'll consult for this entity. */
         for (int r = 0; r < rel_count; r++) {
            out_conv[r] = 0;
            out_start[r] = 0;
            out_end[r] = 0;
            rel_ids[r] = rels[r].id;
         }
         memory_db_relations_get_sources(user_id, rel_ids, rel_count, out_conv, out_start, out_end);
      }

      for (int r = 0; r < rel_count; r++) {
         strbuf_appendf(sb, "  %s: %s\n", rels[r].relation, rels[r].object_name);
         if (render_rel_source && out_conv[r] > 0 && *source_budget > 0) {
            append_source_excerpt_from_range(user_id, out_conv[r], out_start[r], out_end[r], sb,
                                             source_budget, seen);
            if (strbuf_oom(sb))
               return;
         }
      }

      for (int r = 0; r < in_count; r++) {
         strbuf_appendf(sb, "  %s %s %s\n", in_rels[r].object_name, in_rels[r].relation,
                        entity_names[i]);
      }
   }
}

/* =============================================================================
 * Action: Search
 * ============================================================================= */

/* category (v34): when non-NULL/non-empty, pre-filters fact-ID set by exact category
 *   match before hybrid scoring.  Bypasses time_range path (categories layer above
 *   recency for now — combinable in a follow-up if useful).
 * as_of_ts (v33): timestamp for relation validity in entity recall (0 = now).
 * include_historical (v33): bypass relation validity filter entirely. */
char *memory_action_search(int user_id,
                           const char *keywords,
                           time_t since_ts,
                           const char *category,
                           int64_t as_of_ts,
                           bool include_historical,
                           bool with_source,
                           int source_budget) {
   if (!keywords || strlen(keywords) == 0) {
      return strdup("Please provide search keywords.");
   }

   /* Growable response buffer.  8KB initial sizes for the typical case (small
    * source budget, modest entity graph); strbuf doubles as needed so neither
    * a higher source_budget nor a denser conversation can silently truncate
    * facts/prefs/summaries the way the previous fixed 8KB did.  Cap at the
    * default (256 KiB) — well above any realistic memory tool response. */
   strbuf_t sb;
   strbuf_init(&sb, 8192);

   /* Tokenize query for per-word matching */
   char tokens[MAX_SEARCH_TOKENS][64];
   int token_count = tokenize_query(keywords, tokens, MAX_SEARCH_TOKENS);

   /* Search facts — keyword search first, then hybrid if embeddings available */
   memory_fact_t facts[10];
   int fact_count = 0;
   bool category_filter_active = (category && *category);

   if (category_filter_active) {
      /* Category pre-filter is independent of token count — single SQL per query.
       * Score-vs-time combinability deferred (see decision #7).  This path
       * intentionally bypasses the shared `memory_fact_search_hybrid` helper
       * because that helper has no category-filter codepath in 1c; v2 of the
       * helper will subsume this branch (TODO 1d/1j). */
      memory_db_fact_search_by_category(user_id, keywords, category, facts, 10, &fact_count);

      if (memory_embeddings_available() && fact_count > 0) {
         int64_t kw_ids[10];
         int kw_scores[10];
         for (int i = 0; i < fact_count; i++) {
            kw_ids[i] = facts[i].id;
            kw_scores[i] = 1;
         }
         embedding_search_result_t hybrid_results[10];
         int hybrid_count = memory_embeddings_hybrid_search(user_id, keywords, kw_ids, kw_scores,
                                                            fact_count,
                                                            (token_count > 0) ? token_count : 1,
                                                            hybrid_results, 10);
         if (hybrid_count > 0) {
            memory_fact_t reordered[10];
            int reordered_count = 0;
            for (int h = 0; h < hybrid_count && reordered_count < 10; h++) {
               bool found = false;
               for (int f = 0; f < fact_count; f++) {
                  if (facts[f].id == hybrid_results[h].fact_id) {
                     reordered[reordered_count++] = facts[f];
                     found = true;
                     break;
                  }
               }
               if (!found) {
                  memory_fact_t vec_fact;
                  if (memory_db_fact_get(hybrid_results[h].fact_id, &vec_fact) ==
                      MEMORY_DB_SUCCESS) {
                     /* Defense-in-depth: hybrid_search currently scopes by user_id,
                      * but `memory_db_fact_get` is NOT user-scoped.  Skip foreign
                      * facts. */
                     if (vec_fact.user_id != user_id) {
                        OLOG_ERROR("memory_callback: vector-only fact_id=%lld owned by user_id=%d "
                                   "(expected %d) — skipping",
                                   (long long)hybrid_results[h].fact_id, vec_fact.user_id, user_id);
                        continue;
                     }
                     reordered[reordered_count++] = vec_fact;
                  }
               }
            }
            memcpy(facts, reordered, reordered_count * sizeof(memory_fact_t));
            fact_count = reordered_count;
         }
      }
   } else {
      /* Non-category path: delegate to the unified retrieval primitive.
       * `memory_search_execute` does hybrid + entity-graph + Phase 2 Step 1
       * query-aware scoring + merge + score floor in a single call,
       * shared with the bench harness to prevent drift. */
      float scores[10];
      memory_search_execute(user_id, keywords, since_ts, facts, scores, 10, &fact_count);
   }

   /* source_budget==0 from caller means "use default"; else honor caller's value
    * (bench harness uses this to sweep budget values).  Cap at
    * MEMORY_SOURCE_BUDGET_MAX so a misconfigured caller cannot inject an
    * arbitrary amount of verbatim source text — defense-in-depth.  The
    * bench is the only legitimate caller passing > 3072 today; if a
    * future LLM-tool exposes the param to model output, the clamp here
    * is the backstop. */
   if (source_budget <= 0)
      source_budget = MEMORY_SOURCE_BUDGET_CHARS;
   else if (source_budget > MEMORY_SOURCE_BUDGET_MAX)
      source_budget = MEMORY_SOURCE_BUDGET_MAX;

   /* Source-fetch dedup set spans the whole call: same (conv, range) cited by
    * a fact + a summary + a relation emits "same as above" after the first
    * verbatim. */
   source_dedup_set_t seen = { 0 };

   if (fact_count > 0) {
      strbuf_appendf(&sb, "FACTS (%d):\n", fact_count);

      /* Batch-fetch fact provenance up front — one DB round-trip instead of
       * N × _fact_get_source calls.  fact_conv[i] = 0 for facts with no
       * provenance recorded or whose source is in a private conversation
       * (privacy filtered in SQL). */
      int64_t fact_conv[10] = { 0 }, fact_start[10] = { 0 }, fact_end[10] = { 0 };
      if (with_source) {
         int64_t fact_ids[10];
         for (int i = 0; i < fact_count; i++)
            fact_ids[i] = facts[i].id;
         memory_db_facts_get_sources(user_id, fact_ids, fact_count, fact_conv, fact_start,
                                     fact_end);
      }

      int elided = 0;
      for (int i = 0; i < fact_count; i++) {
         char time_str[32];
         format_time_ago(facts[i].created_at, time_str, sizeof(time_str));
         strbuf_appendf(&sb, "- [ID:%lld] %s (confidence: %.0f%%, %s)\n", (long long)facts[i].id,
                        facts[i].fact_text, facts[i].confidence * 100, time_str);

         if (with_source && source_budget > 0 && fact_conv[i] > 0) {
            append_source_excerpt_from_range(user_id, fact_conv[i], fact_start[i], fact_end[i], &sb,
                                             &source_budget, &seen);
         } else if (with_source && source_budget <= 0) {
            elided++;
         }

         /* Update access time */
         memory_db_fact_update_access(facts[i].id, user_id);

         /* Bail early if buffer max_cap was hit during source-excerpt
          * rendering.  Otherwise we'd burn N-1 wasted iterations on no-op
          * append_source_excerpt_from_range calls (embedded-review M4). */
         if (strbuf_oom(&sb))
            break;
      }
      if (elided > 0) {
         strbuf_appendf(&sb, "[%d more source(s) elided — call context_expand for full]\n", elided);
      }
   }

   /* Search preferences — SQL-filtered by LIKE on category and value */
   memory_preference_t prefs[10];
   int pref_count = 0;
   memory_db_pref_search(user_id, keywords, prefs, 10, &pref_count);

   if (pref_count > 0) {
      if (strbuf_len(&sb) > 0)
         strbuf_append(&sb, "\n");
      strbuf_append(&sb, "PREFERENCES:\n");

      int64_t pref_conv[10] = { 0 }, pref_start[10] = { 0 }, pref_end[10] = { 0 };
      if (with_source) {
         int64_t pref_ids[10];
         for (int i = 0; i < pref_count; i++)
            pref_ids[i] = prefs[i].id;
         memory_db_prefs_get_sources(user_id, pref_ids, pref_count, pref_conv, pref_start,
                                     pref_end);
      }

      for (int i = 0; i < pref_count; i++) {
         strbuf_appendf(&sb, "- %s: %s (reinforced %d times)\n", prefs[i].category, prefs[i].value,
                        prefs[i].reinforcement_count);
         if (with_source && source_budget > 0 && pref_conv[i] > 0) {
            append_source_excerpt_from_range(user_id, pref_conv[i], pref_start[i], pref_end[i], &sb,
                                             &source_budget, &seen);
            if (strbuf_oom(&sb))
               break;
         }
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

   if (summary_count > 0) {
      if (strbuf_len(&sb) > 0)
         strbuf_append(&sb, "\n");
      strbuf_appendf(&sb, "CONVERSATION SUMMARIES (%d):\n", summary_count);

      int64_t sum_conv[5] = { 0 }, sum_start[5] = { 0 }, sum_end[5] = { 0 };
      if (with_source) {
         int64_t sum_ids[5];
         for (int i = 0; i < summary_count; i++)
            sum_ids[i] = summaries[i].id;
         memory_db_summaries_get_sources(user_id, sum_ids, summary_count, sum_conv, sum_start,
                                         sum_end);
      }

      for (int i = 0; i < summary_count; i++) {
         char time_str[32];
         format_time_ago(summaries[i].created_at, time_str, sizeof(time_str));
         strbuf_appendf(&sb, "- [%s] %s\n  Topics: %s\n", time_str, summaries[i].summary,
                        summaries[i].topics);
         if (with_source && source_budget > 0 && sum_conv[i] > 0) {
            append_source_excerpt_from_range(user_id, sum_conv[i], sum_start[i], sum_end[i], &sb,
                                             &source_budget, &seen);
            if (strbuf_oom(&sb))
               break;
         }
      }
   }

   /* Append entity graph context (with relation source-rendering when enabled) */
   append_graph_context(user_id, keywords, as_of_ts, include_historical, &sb, with_source,
                        &source_budget, &seen);

   /* Empty result: surface a friendly fallback (must not be NULL — callers
    * expect a heap-allocated string). */
   if (strbuf_len(&sb) == 0) {
      strbuf_appendf(&sb, "No memories found matching '%s'.", keywords);
   }

   /* OOM during build: return a sentinel; consumer (LLM) sees the failure. */
   if (strbuf_oom(&sb)) {
      strbuf_free(&sb);
      return strdup("Memory search failed: response too large or out of memory.");
   }

   char *out = strbuf_steal(&sb);
   return out ? out : strdup("Memory search failed: out of memory.");
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

   /* No duplicates found - store the new fact (no provenance: user-initiated) */
   int64_t fact_id = 0;
   int create_rc = memory_db_fact_create(user_id, fact_text, 1.0f, "explicit", NULL, NULL,
                                         &fact_id);

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

/* MEMORY_RECENT_LIMIT_MAX + defaults moved to memory_callback_internal.h so
 * the tool schema description and the action layer share one source of
 * truth — see header for rationale. */
_Static_assert(MEMORY_RECENT_LIMIT_MAX >= MEMORY_RECENT_DEFAULT_FACT_LIMIT &&
                   MEMORY_RECENT_LIMIT_MAX >= MEMORY_RECENT_DEFAULT_SUMMARY_LIMIT,
               "MEMORY_RECENT_LIMIT_MAX must be >= the defaults used to size stack arrays");

char *memory_action_recent(int user_id,
                           const char *period,
                           const char *before,
                           bool sort_asc,
                           int limit,
                           bool with_source,
                           int source_budget) {
   /* Resolve lower bound — empty/NULL period defaults to "7d" so the LLM
    * can omit the time window for the common "show me recent stuff" case
    * (pre-Bundle-3 required a non-empty period and rejected with an
    * error message). */
   const char *period_resolved = (period && period[0]) ? period : "7d";
   time_t seconds = parse_time_period(period_resolved);
   if (seconds <= 0) {
      return strdup("Invalid time period. Use format like '24h', '7d', '1w', or '30m'.");
   }
   time_t now_ts = time(NULL);
   time_t since = now_ts - seconds;
   if (since < 0)
      since = 0;

   /* Resolve upper bound — empty/NULL `before` means "until now"; pass 0
    * to memory_db_*_list_window which resolves to INT64_MAX internally. */
   time_t until = 0;
   if (before && before[0]) {
      time_t before_seconds = parse_time_period(before);
      if (before_seconds <= 0) {
         return strdup("Invalid 'before' time period. Use format like '3d', '30d', '1w'.");
      }
      until = now_ts - before_seconds;
      if (until < 0)
         until = 0;
   }

   /* Clamp limit; 0 = "use defaults". */
   int fact_limit = MEMORY_RECENT_DEFAULT_FACT_LIMIT;
   int summary_limit = MEMORY_RECENT_DEFAULT_SUMMARY_LIMIT;
   if (limit > 0) {
      if (limit > MEMORY_RECENT_LIMIT_MAX)
         limit = MEMORY_RECENT_LIMIT_MAX;
      fact_limit = limit;
      summary_limit = limit;
   }

   /* Growable response buffer — see memory_action_search() for rationale. */
   strbuf_t sb;
   strbuf_init(&sb, 4096);

   /* Get facts within the window, sorted per sort_asc.
    * Stack footprint: sizeof(memory_fact_t) ≈ 612 B × MEMORY_RECENT_LIMIT_MAX
    * (50) ≈ 30 KB.  Plus three int64_t[50] provenance arrays below = ~1.2 KB.
    * Total fact-side stack ≈ 31 KB.  Safe on glibc's 8 MB default pthread
    * stack; if MEMORY_RECENT_LIMIT_MAX ever bumps past ~256 (≈150 KB), move
    * these to the heap. */
   memory_fact_t facts[MEMORY_RECENT_LIMIT_MAX];
   int fact_count = 0;
   memory_db_fact_list_window(user_id, since, until, sort_asc, facts, fact_limit, &fact_count);

   /* See memory_action_search for source_budget clamp rationale. */
   if (source_budget <= 0)
      source_budget = MEMORY_SOURCE_BUDGET_CHARS;
   else if (source_budget > MEMORY_SOURCE_BUDGET_MAX)
      source_budget = MEMORY_SOURCE_BUDGET_MAX;

   /* Dedup spans facts + summaries: same range cited by a fact and a summary
    * emits "same as above" after the first verbatim. */
   source_dedup_set_t seen = { 0 };

   if (fact_count > 0) {
      strbuf_append(&sb, sort_asc ? "OLDEST FACTS:\n" : "RECENT FACTS:\n");

      /* Batch-fetch fact provenance — memory_db_facts_get_sources chunks
       * internally in MAX_PROVENANCE_BATCH-sized (32) groups, so N > 32 is
       * fine.  fact_count is always ≤ fact_limit ≤ MEMORY_RECENT_LIMIT_MAX
       * (50), worst case 2 chunks. */
      int64_t fact_conv[MEMORY_RECENT_LIMIT_MAX] = { 0 };
      int64_t fact_start[MEMORY_RECENT_LIMIT_MAX] = { 0 };
      int64_t fact_end[MEMORY_RECENT_LIMIT_MAX] = { 0 };
      if (with_source) {
         int64_t fact_ids[MEMORY_RECENT_LIMIT_MAX];
         for (int i = 0; i < fact_count; i++)
            fact_ids[i] = facts[i].id;
         memory_db_facts_get_sources(user_id, fact_ids, fact_count, fact_conv, fact_start,
                                     fact_end);
      }

      int elided = 0;
      for (int i = 0; i < fact_count; i++) {
         char time_str[32];
         format_time_ago(facts[i].created_at, time_str, sizeof(time_str));
         strbuf_appendf(&sb, "- [ID:%lld] %s (%s, %s)\n", (long long)facts[i].id,
                        facts[i].fact_text, facts[i].source, time_str);
         if (with_source && source_budget > 0 && fact_conv[i] > 0) {
            append_source_excerpt_from_range(user_id, fact_conv[i], fact_start[i], fact_end[i], &sb,
                                             &source_budget, &seen);
         } else if (with_source && source_budget <= 0) {
            elided++;
         }
         /* See memory_action_search for OOM-bail rationale (embedded M4). */
         if (strbuf_oom(&sb))
            break;
      }
      if (elided > 0) {
         strbuf_appendf(&sb, "[%d more source(s) elided — call context_expand for full]\n", elided);
      }
   }

   /* Get summaries within the window, sorted per sort_asc.
    * Stack footprint: sizeof(memory_summary_t) ≈ 2420 B (dominated by the
    * 2048-byte `summary` field) × MEMORY_RECENT_LIMIT_MAX (50) ≈ 121 KB.
    * This is the bulk of the function's stack budget — combined with the
    * fact arrays above the total reaches ~154 KB.  Safe on glibc's 8 MB
    * default pthread stack but bumping MEMORY_RECENT_LIMIT_MAX past ~256
    * would push past 600 KB — move these to the heap if that bump lands. */
   memory_summary_t summaries[MEMORY_RECENT_LIMIT_MAX];
   int summary_count = 0;
   memory_db_summary_list_window(user_id, since, until, sort_asc, summaries, summary_limit,
                                 &summary_count);

   if (summary_count > 0) {
      if (strbuf_len(&sb) > 0)
         strbuf_append(&sb, "\n");
      strbuf_append(&sb, sort_asc ? "OLDEST CONVERSATIONS:\n" : "RECENT CONVERSATIONS:\n");

      int64_t sum_conv[MEMORY_RECENT_LIMIT_MAX] = { 0 };
      int64_t sum_start[MEMORY_RECENT_LIMIT_MAX] = { 0 };
      int64_t sum_end[MEMORY_RECENT_LIMIT_MAX] = { 0 };
      if (with_source) {
         int64_t sum_ids[MEMORY_RECENT_LIMIT_MAX];
         for (int i = 0; i < summary_count; i++)
            sum_ids[i] = summaries[i].id;
         memory_db_summaries_get_sources(user_id, sum_ids, summary_count, sum_conv, sum_start,
                                         sum_end);
      }

      for (int i = 0; i < summary_count; i++) {
         char time_str[32];
         format_time_ago(summaries[i].created_at, time_str, sizeof(time_str));
         if (strbuf_appendf(&sb, "- [%s] %s\n  Topics: %s\n", time_str, summaries[i].summary,
                            summaries[i].topics) < 0)
            break;
         if (with_source && source_budget > 0 && sum_conv[i] > 0) {
            append_source_excerpt_from_range(user_id, sum_conv[i], sum_start[i], sum_end[i], &sb,
                                             &source_budget, &seen);
         }
         /* Mirror memory_action_search's OOM-bail pattern (Arch review M4). */
         if (strbuf_oom(&sb))
            break;
      }
   }

   if (fact_count == 0 && summary_count == 0) {
      strbuf_appendf(&sb, "No memories found in the past %s.", period_resolved);
   } else {
      strbuf_appendf(&sb, "\nTotal: %d facts, %d conversations", fact_count, summary_count);
   }

   if (strbuf_oom(&sb)) {
      strbuf_free(&sb);
      return strdup("Memory query failed: response too large or out of memory.");
   }

   char *out = strbuf_steal(&sb);
   return out ? out : strdup("Memory query failed: out of memory.");
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
       * include_historical / with_source from encoded value. */
      char keywords[512] = "";
      time_t since_ts = 0;
      char category[MEMORY_CATEGORY_MAX] = "";
      int64_t as_of_ts = 0;
      bool include_historical = false;
      bool with_source = false;

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

         /* with_source: return verbatim excerpts for each fact. */
         char wsrc[8] = "";
         if (tool_param_extract_custom(value, "with_source", wsrc, sizeof(wsrc))) {
            with_source = (strcmp(wsrc, "true") == 0);
         }
      }

      /* source_budget from dawn.toml [memory] source_budget_chars; 0 falls
       * back to MEMORY_SOURCE_BUDGET_CHARS inside the action.  Bench harness
       * invokes memory_action_search() directly with its own override. */
      return memory_action_search(user_id, keywords[0] ? keywords : NULL, since_ts,
                                  category[0] ? category : NULL, as_of_ts, include_historical,
                                  with_source, g_config.memory.source_budget_chars);
   } else if (strcmp(actionName, "remember") == 0) {
      return memory_action_remember(user_id, value);
   } else if (strcmp(actionName, "forget") == 0) {
      return memory_action_forget(user_id, value);
   } else if (strcmp(actionName, "recent") == 0) {
      /* Bundle 3 (2026-05-13): extract the time-period window from the base
       * value, plus the new 'before' / 'sort' / 'limit' modifiers from
       * custom fields.  Pre-Bundle-3 the whole value string was passed
       * directly to memory_action_recent which then parsed it as a time
       * period — that's still the behavior when no modifiers are present. */
      char period[32] = "";
      tool_param_extract_base(value, period, sizeof(period));

      char before[32] = "";
      if (value) {
         tool_param_extract_custom(value, "before", before, sizeof(before));
      }

      bool sort_asc = false;
      char sort_str[16] = "";
      if (value && tool_param_extract_custom(value, "sort", sort_str, sizeof(sort_str))) {
         sort_asc = (strcmp(sort_str, "oldest") == 0);
      }

      int limit = 0;
      char limit_str[16] = "";
      if (value && tool_param_extract_custom(value, "limit", limit_str, sizeof(limit_str)) &&
          limit_str[0]) {
         /* strtol over atoi: atoi has implementation-defined behavior on
          * overflow (GCC saturates to INT_MAX but the standard doesn't
          * require it).  strtol with ERANGE check + explicit range bounds
          * matches the bounded-numeric pattern in parse_time_period. */
         char *endptr = NULL;
         errno = 0;
         long parsed = strtol(limit_str, &endptr, 10);
         if (errno == 0 && endptr != limit_str && parsed > 0 && parsed <= MEMORY_RECENT_LIMIT_MAX) {
            limit = (int)parsed;
         }
         /* Out-of-range / malformed → limit stays 0 → action layer uses
          * defaults.  memory_action_recent clamps in-range values
          * internally as a second gate. */
      }

      bool with_source = false;
      if (value) {
         char wsrc[8] = "";
         if (tool_param_extract_custom(value, "with_source", wsrc, sizeof(wsrc))) {
            with_source = (strcmp(wsrc, "true") == 0);
         }
      }
      return memory_action_recent(user_id, period[0] ? period : NULL, before[0] ? before : NULL,
                                  sort_asc, limit, with_source,
                                  g_config.memory.source_budget_chars);
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

      /* Run prompt-injection filter on both names before any DB lookup —
       * mirrors what the resolver path will do at extraction time, and
       * prevents a poisoned name from sliding into a downstream
       * canonical_name field via a misdirected merge. */
      if (memory_filter_check(source_name) || memory_filter_check(target_name)) {
         return strdup("Error: entity name failed prompt-injection filter");
      }

      /* Look up both entities by canonical name */
      char src_canonical[64], tgt_canonical[64];
      memory_make_canonical_name(source_name, src_canonical, sizeof(src_canonical));
      memory_make_canonical_name(target_name, tgt_canonical, sizeof(tgt_canonical));

      memory_entity_t src_entity, tgt_entity;
      if (memory_db_entity_get_by_name(user_id, src_canonical, &src_entity) != MEMORY_DB_SUCCESS)
         return strdup("Error: source entity not found");
      if (memory_db_entity_get_by_name(user_id, tgt_canonical, &tgt_entity) != MEMORY_DB_SUCCESS)
         return strdup("Error: target entity not found");

      /* Soft-link by default (entity-merge Phase 1, design §14): set
       * canonical_id on the source row + audit-row insert.  The link is
       * reversible — operators promote to permanent merge via
       * `dawn-admin memory entity consolidate` (Phase 3). */
      int64_t link_id = 0;
      int rc = memory_db_entity_alias_link(user_id, src_entity.id, tgt_entity.id, "soft",
                                           "llm-tool-action",
                                           /* composite_score */ -1.0f,
                                           /* evidence_json */ NULL, &link_id);
      if (rc == MEMORY_DB_SUCCESS) {
         char *result = malloc(768);
         if (result)
            snprintf(result, 768,
                     "Linked '%s' as a soft alias of '%s' (link id %lld). "
                     "The '%s' entity is preserved and will be resolved to '%s' on lookup. "
                     "Use 'split' to undo, or 'dawn-admin memory entity consolidate' to make it "
                     "permanent.",
                     source_name, target_name, (long long)link_id, source_name, target_name);
         return result ? result : strdup("Entities soft-linked.");
      } else if (rc == MEMORY_DB_NOT_FOUND) {
         /* Race: entity deleted between lookup and link */
         OLOG_WARNING("memory_callback: merge_entities race — entity vanished between lookup and "
                      "link (source='%s', target='%s')",
                      source_name, target_name);
         return strdup("Error: one or both entities no longer exist (may have been deleted)");
      } else {
         /* alias_link refuses self-link, source-with-existing-aliases (would
          * orphan the equivalence class), and DB failures.  Distinct return
          * codes aren't surfaced; report the operation refused. */
         return strdup("Error: cannot link these entities "
                       "(source may already be a canonical with its own aliases, or "
                       "source and target are the same)");
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
