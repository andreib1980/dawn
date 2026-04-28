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
 * Document Search Tool - RAG semantic search over uploaded documents
 *
 * Embeds the query, loads all accessible chunks, computes cosine similarity
 * with optional keyword boosting, and returns top results with citations.
 */

#include "tools/document_search.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config/dawn_config.h"
#include "core/embedding_engine.h"
#include "core/time_query_parser.h"
#include "dawn_error.h"
#include "logging.h"
#include "tools/document_db.h"
#include "tools/tool_registry.h"

/* =============================================================================
 * Constants
 * ============================================================================= */

#define DOC_SEARCH_MAX_RESULTS 5
#define DOC_SEARCH_MAX_CONTEXT_TOKENS 2000
#define DOC_SEARCH_MAX_CHUNKS 10000
#define DOC_SEARCH_KEYWORD_BOOST 0.15f
#define DOC_SEARCH_MIN_SCORE 0.3f

/* =============================================================================
 * Forward Declarations
 * ============================================================================= */

static char *doc_search_callback(const char *action, char *value, int *should_respond);
static bool doc_search_is_available(void);

/* =============================================================================
 * Tool Metadata
 * ============================================================================= */

static const treg_param_t doc_search_params[] = {
   {
       .name = "query",
       .description = "The search query — what you want to find in the user's documents",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = true,
       .maps_to = TOOL_MAPS_TO_VALUE,
   },
};

static const tool_metadata_t doc_search_metadata = {
   .name = "document_search",
   .device_string = "document search",
   .description = "Search the user's uploaded documents for relevant information. "
                  "Use this when the user asks about content they've previously uploaded "
                  "(PDFs, manuals, notes, etc.). Returns relevant excerpts with source "
                  "citations. Do NOT use this for general web searches.",
   .params = doc_search_params,
   .param_count = 1,
   .device_type = TOOL_DEVICE_TYPE_GETTER,
   .capabilities = 0,
   .is_getter = true,
   .is_available = doc_search_is_available,
   .callback = doc_search_callback,
};

/* =============================================================================
 * Registration
 * ============================================================================= */

int document_search_tool_register(void) {
   return tool_registry_register(&doc_search_metadata);
}

/* =============================================================================
 * Availability Check
 * ============================================================================= */

static bool doc_search_is_available(void) {
   return embedding_engine_available();
}

/* =============================================================================
 * Internal: Case-insensitive substring search
 * ============================================================================= */

static bool contains_keyword(const char *text, const char *keyword, int keyword_len) {
   if (!text || !keyword || keyword_len <= 0)
      return false;

   for (const char *p = text; *p; p++) {
      if (tolower((unsigned char)*p) == tolower((unsigned char)keyword[0])) {
         bool match = true;
         int matched = 1;
         for (int i = 1; i < keyword_len && p[i]; i++) {
            if (tolower((unsigned char)p[i]) != tolower((unsigned char)keyword[i])) {
               match = false;
               break;
            }
            matched++;
         }
         if (match && matched == keyword_len &&
             (p[keyword_len] == '\0' || !isalnum((unsigned char)p[keyword_len])))
            return true;
      }
   }
   return false;
}

/* =============================================================================
 * Internal: Tokenize query into words for keyword boosting
 * ============================================================================= */

typedef struct {
   const char *words[32];
   int lengths[32];
   int count;
} query_words_t;

static void tokenize_query(const char *query, query_words_t *qw) {
   qw->count = 0;
   const char *p = query;

   while (*p && qw->count < 32) {
      while (*p && !isalnum((unsigned char)*p))
         p++;
      if (!*p)
         break;
      const char *start = p;
      while (*p && isalnum((unsigned char)*p))
         p++;
      int len = (int)(p - start);
      if (len >= 3) { /* Skip very short words */
         qw->words[qw->count] = start;
         qw->lengths[qw->count] = len;
         qw->count++;
      }
   }
}

/* =============================================================================
 * Internal: Compute keyword match ratio
 * ============================================================================= */

static float keyword_score(const char *text, const query_words_t *qw) {
   if (qw->count == 0)
      return 0.0f;

   int matches = 0;
   for (int i = 0; i < qw->count; i++) {
      if (contains_keyword(text, qw->words[i], qw->lengths[i]))
         matches++;
   }
   return (float)matches / (float)qw->count;
}

/* =============================================================================
 * Search Callback
 * ============================================================================= */

typedef struct {
   int index;
   float score;
} scored_chunk_t;

static int score_compare(const void *a, const void *b) {
   float sa = ((const scored_chunk_t *)a)->score;
   float sb = ((const scored_chunk_t *)b)->score;
   if (sb > sa)
      return 1;
   if (sb < sa)
      return -1;
   return 0;
}

static char *doc_search_callback(const char *action, char *value, int *should_respond) {
   (void)action;
   *should_respond = 1;

   if (!value || value[0] == '\0')
      return strdup("Error: no search query provided.");

   int user_id = tool_get_current_user_id();
   int dims = embedding_engine_dims();

   if (dims <= 0)
      return strdup("Error: embedding engine not initialized.");

   /* Embed the query */
   float *query_vec = malloc((size_t)dims * sizeof(float));
   if (!query_vec)
      return strdup("Error: memory allocation failed.");

   int out_dims = 0;
   if (embedding_engine_embed(value, query_vec, dims, &out_dims) != 0 || out_dims != dims) {
      free(query_vec);
      return strdup("Error: failed to generate query embedding.");
   }
   float query_norm = embedding_engine_l2_norm(query_vec, dims);

   /* Load all accessible chunks */
   int max_chunks = DOC_SEARCH_MAX_CHUNKS;
   document_chunk_t *chunks = calloc((size_t)max_chunks, sizeof(document_chunk_t));
   float *emb_buf = malloc((size_t)max_chunks * (size_t)dims * sizeof(float));
   if (!chunks || !emb_buf) {
      free(query_vec);
      free(chunks);
      free(emb_buf);
      return strdup("Error: memory allocation failed.");
   }

   int chunk_count = 0;
   if (document_db_chunk_search_load(user_id, chunks, emb_buf, dims, max_chunks, &chunk_count) !=
           SUCCESS ||
       chunk_count <= 0) {
      free(query_vec);
      free(chunks);
      free(emb_buf);
      return strdup("No documents indexed. Upload documents via the WebUI first.");
   }

   /* Score all chunks — cosine similarity only (first pass) */
   scored_chunk_t *scores = malloc((size_t)chunk_count * sizeof(scored_chunk_t));
   if (!scores) {
      free(query_vec);
      free(chunks);
      free(emb_buf);
      return strdup("Error: memory allocation failed.");
   }

   /* Parse the query once for temporal expressions (#3).  Skip the work when
    * the feature is disabled (temporal_weight == 0), matching the facts path. */
   time_query_t tq = { 0 };
   float temporal_weight = g_config.memory.temporal_weight;
   if (temporal_weight > 0.0f) {
      time_query_parse(value, (int64_t)time(NULL), &tq);
   }

   for (int i = 0; i < chunk_count; i++) {
      float cosine = embedding_engine_cosine_with_norms(query_vec, chunks[i].embedding, dims,
                                                        query_norm, chunks[i].embedding_norm);
      /* Additive temporal boost — same shape as memory_embeddings_hybrid_search.
       * Chunks with no created_at (legacy rows) forfeit the bonus silently. */
      if (tq.found && chunks[i].created_at > 0) {
         cosine += temporal_weight * time_query_proximity(&tq, chunks[i].created_at);
      }
      scores[i].index = i;
      scores[i].score = cosine;
   }

   free(query_vec);

   /* Sort by cosine score descending */
   qsort(scores, (size_t)chunk_count, sizeof(scored_chunk_t), score_compare);

   /* Apply keyword boosting only to top candidates (second pass) */
   query_words_t qw;
   tokenize_query(value, &qw);

   int top_n = chunk_count < 50 ? chunk_count : 50;
   for (int i = 0; i < top_n; i++) {
      float kw = keyword_score(chunks[scores[i].index].text, &qw);
      scores[i].score += kw * DOC_SEARCH_KEYWORD_BOOST;
   }

   /* Re-sort just the top candidates after keyword boosting */
   qsort(scores, (size_t)top_n, sizeof(scored_chunk_t), score_compare);

   /* Format top results with token budget */
   int max_results = DOC_SEARCH_MAX_RESULTS;
   int token_budget = DOC_SEARCH_MAX_CONTEXT_TOKENS;
   int result_buf_size = token_budget * 5; /* ~5 chars per token, generous */
   char *result = malloc((size_t)result_buf_size);
   if (!result) {
      free(chunks);
      free(emb_buf);
      free(scores);
      return strdup("Error: memory allocation failed.");
   }

   int pos = 0;
   int shown = 0;
   int total_matches = 0;

   /* Count matches above threshold */
   for (int i = 0; i < chunk_count; i++) {
      if (scores[i].score >= DOC_SEARCH_MIN_SCORE)
         total_matches++;
   }

   if (total_matches == 0) {
      free(chunks);
      free(emb_buf);
      free(scores);
      free(result);
      return strdup("No relevant documents found for this query.");
   }

   pos += snprintf(result + pos, (size_t)(result_buf_size - pos),
                   "DOCUMENT SEARCH RESULTS (showing up to %d of %d matches):\n", max_results,
                   total_matches);

   for (int i = 0; i < chunk_count && shown < max_results; i++) {
      if (scores[i].score < DOC_SEARCH_MIN_SCORE)
         break;

      document_chunk_t *c = &chunks[scores[i].index];
      int chunk_tokens = ((int)strlen(c->text) + 3) / 4;

      if (shown > 0 && chunk_tokens > token_budget)
         break; /* Would exceed budget */

      shown++;
      token_budget -= chunk_tokens;

      pos += snprintf(result + pos, (size_t)(result_buf_size - pos),
                      "\n[%d] (score: %.2f) %s, chunk %d:\n%s\n", shown, scores[i].score,
                      c->doc_filename, c->chunk_index + 1, c->text);

      if (pos >= result_buf_size - 256)
         break;
   }

   /* Add truncation notice if needed */
   int omitted = total_matches - shown;
   if (omitted > 0) {
      snprintf(result + pos, (size_t)(result_buf_size - pos),
               "\n[%d additional match%s omitted — token budget reached. "
               "User can ask for more detail.]",
               omitted, omitted == 1 ? "" : "es");
   }

   free(chunks);
   free(emb_buf);
   free(scores);

   return result;
}
