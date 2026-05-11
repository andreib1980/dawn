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
 * Document chunk focus adapter — Phase 1d of Dynamic Context Injection.
 *
 * source_id          = "document_chunk"
 * source_type        = FOCUS_SOURCE_EXTERNAL
 * requires_embedding = true
 *
 * Pipeline:
 *   1. Load up to (max_candidates * OVERFETCH_FACTOR) chunks accessible
 *      to user_id (own + global) via document_db_chunk_search_load,
 *      which JOINs to populate `doc_filename`/`doc_filetype` so the
 *      filename comes for free without a per-chunk N+1 fetch.
 *   2. Cosine-rank against query_embedding using
 *      memory_embeddings_cosine_with_norms (chunk norms are
 *      pre-computed in the DB).
 *   3. Trim to max_candidates by score (selection sort — N small).
 *   4. Render "[<filename>] <chunk_text>" through focus_candidate_init
 *      which truncates to FOCUS_TEXT_MAX_BYTES.
 *
 * Memory shape: stack-allocated `document_chunk_t` array (128
 * × ~5 KB ≈ 640 KB worst case at max_candidates=32 and OVERFETCH=4)
 * is too large; we heap-allocate the chunk + flat-embedding buffers
 * sized to the actual fetch and free both before returning.
 *
 * Provenance: {0,0,0} sentinel — documents have no conv-based source
 * linkage; the WebUI surfaces filename via the rendered text.
 *
 * Network-call audit (verified cache-only on 2026-05-08 by reading
 * src/tools/document_db.c:426-476 — pure SQLite via
 * AUTH_DB_LOCK_OR_FAIL → s_db.stmt_doc_chunk_search; no curl, socket,
 * connect, lws, or SSL calls in the call chain):
 *   - document_db_chunk_search_load — cache-only
 *
 * Filter-on-retrieval is FRAMEWORK-OWNED + trust-tier-gated.  This
 * adapter does NOT call `memory_filter_check()` — `focus_compose()`
 * decides based on `source_type`.  Document chunks are
 * FOCUS_SOURCE_EXTERNAL (user-uploaded, trusted) and pass through
 * without filtering.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "dawn_error.h"
#include "logging.h"
#include "memory/focus_candidate_helpers.h"
#include "memory/focus_recency.h"
#include "memory/focus_source.h"
#include "memory/memory_embeddings.h"
#include "tools/document_db.h"

/* Constants — file-static, all TODO(1j) for bench-driven tuning. */

/* Importance score for every document chunk in v1.  Documents lack a
 * confidence-style intrinsic per-chunk signal (unlike facts); 0.5
 * keeps documents below curated memory facts (1.0) while above
 * future low-importance sources.  TODO(1j). */
#define DOCUMENT_DEFAULT_IMPORTANCE 0.5f

/* Over-fetch factor: load 4x max_candidates so cosine ranking has a
 * meaningful pool to re-sort instead of just returning whatever
 * SQLite happened to scan first.  Capped at DOCUMENT_FETCH_HARD_CAP
 * to bound stack/heap.  TODO(1j) — bench against R@k after Phase 1
 * lands. */
#define DOCUMENT_OVERFETCH_FACTOR 4
#define DOCUMENT_FETCH_HARD_CAP 128

typedef struct {
   int chunk_index; /* index in original chunks[] / norms — used to
                       look up text + filename + timestamp + id. */
   float cosine;
} doc_rank_entry_t;

/* Selection sort by cosine desc.  N is small (<= 128); a full qsort
 * would be overkill and would mask the deterministic ordering tests
 * rely on for tie-breaking. */
static void rank_chunks_desc(doc_rank_entry_t *rows, int n) {
   for (int i = 1; i < n; i++) {
      doc_rank_entry_t tmp = rows[i];
      int j = i - 1;
      while (j >= 0 && rows[j].cosine < tmp.cosine) {
         rows[j + 1] = rows[j];
         j--;
      }
      rows[j + 1] = tmp;
   }
}

static int document_adapter_query(int user_id,
                                  bool include_private,
                                  const char *query_text,
                                  const float *query_embedding,
                                  size_t embed_dim,
                                  time_t now,
                                  int max_candidates,
                                  focus_candidate_t **out_candidates,
                                  int *out_count) {
   (void)include_private; /* document_db_chunk_search_load surfaces own
                             docs + global; private-conv linkage is
                             1f scope (doesn't apply to documents). */
   (void)query_text;      /* Vector-only adapter; query_text consumed
                             upstream to compute query_embedding. */
   *out_candidates = NULL;
   *out_count = 0;
   if (max_candidates <= 0 || query_embedding == NULL || embed_dim == 0 ||
       !memory_embeddings_available())
      return SUCCESS;

   const int dims = (int)embed_dim;
   if (dims <= 0)
      return SUCCESS;

   /* Compute over-fetch size, clamped so a misconfigured top_k can't
    * drag the heap allocation arbitrarily high. */
   int fetch_n = max_candidates * DOCUMENT_OVERFETCH_FACTOR;
   if (fetch_n > DOCUMENT_FETCH_HARD_CAP)
      fetch_n = DOCUMENT_FETCH_HARD_CAP;
   if (fetch_n < max_candidates)
      fetch_n = max_candidates;

   /* Workspace allocations and `out` are released at the single
    * `cleanup:` epilogue at the end of the function.  Adding a new
    * buffer means adding one calloc + one free at the epilogue —
    * not chasing every error branch.  `goto cleanup` is used by
    * the framework's `focus_compose` for the same reason. */
   document_chunk_t *chunks = NULL;
   float *embed_buf = NULL;
   doc_rank_entry_t *rows = NULL;
   focus_candidate_t *out = NULL;
   int rc = SUCCESS;
   int produced = 0;

   chunks = calloc((size_t)fetch_n, sizeof(*chunks));
   if (chunks == NULL) {
      OLOG_ERROR("document_adapter: OOM allocating chunks buffer (n=%d)", fetch_n);
      rc = FAILURE;
      goto cleanup;
   }
   embed_buf = calloc((size_t)fetch_n * (size_t)dims, sizeof(float));
   if (embed_buf == NULL) {
      OLOG_ERROR("document_adapter: OOM allocating embedding buffer (n=%d, dims=%d)", fetch_n,
                 dims);
      rc = FAILURE;
      goto cleanup;
   }

   int loaded = 0;
   if (document_db_chunk_search_load(user_id, chunks, embed_buf, dims, fetch_n, &loaded) !=
       SUCCESS) {
      OLOG_ERROR("document_adapter: document_db_chunk_search_load failed (user_id=%d)", user_id);
      rc = FAILURE;
      goto cleanup;
   }
   if (loaded <= 0)
      goto cleanup; /* SUCCESS, zero candidates */

   /* Cosine-rank.  Allocate the rank workspace separately so we don't
    * have to keep the full chunk struct around for the trim/merge step. */
   rows = calloc((size_t)loaded, sizeof(*rows));
   if (rows == NULL) {
      OLOG_ERROR("document_adapter: OOM allocating rank workspace (n=%d)", loaded);
      rc = FAILURE;
      goto cleanup;
   }
   const float q_norm = memory_embeddings_l2_norm(query_embedding, dims);
   for (int i = 0; i < loaded; i++) {
      rows[i].chunk_index = i;
      rows[i].cosine = memory_embeddings_cosine_with_norms(query_embedding, &embed_buf[i * dims],
                                                           dims, q_norm, chunks[i].embedding_norm);
   }
   rank_chunks_desc(rows, loaded);

   const int kept = (loaded > max_candidates) ? max_candidates : loaded;
   out = calloc((size_t)kept, sizeof(*out));
   if (out == NULL) {
      OLOG_ERROR("document_adapter: OOM allocating candidate array (n=%d)", kept);
      rc = FAILURE;
      goto cleanup;
   }

   bool truncated_warned = false;
   for (int i = 0; i < kept; i++) {
      const document_chunk_t *c = &chunks[rows[i].chunk_index];

      /* Render "[<filename>] <chunk_text>".  filename comes from the
       * JOIN inside document_db_chunk_search_load — no per-chunk N+1.
       *
       * Sizing: filename ≤ DOC_FILENAME_MAX (256), `[` + `] ` = 3,
       * chunk text ≤ DOC_CHUNK_TEXT_MAX (4096), terminator = 1.
       * Worst-case 4356 bytes; buffer is FOCUS_TEXT_MAX_BYTES +
       * DOC_FILENAME_MAX + 16 (4368) so snprintf cannot truncate
       * even with maximum-length filename and chunk text together.
       * If the rendered string ever exceeds FOCUS_TEXT_MAX_BYTES,
       * focus_candidate_init's truncation handler downstream caps
       * and logs once via `truncated_warned`.  We do NOT pre-reject
       * here: pre-guarding work the framework already does correctly
       * silently dropped content the framework would have truncated
       * cleanly. */
      char rendered[FOCUS_TEXT_MAX_BYTES + DOC_FILENAME_MAX + 16];
      const char *fname = (c->doc_filename[0] != '\0') ? c->doc_filename : "(document)";
      (void)snprintf(rendered, sizeof(rendered), "[%s] %s", fname, c->text);

      char item_id[FOCUS_ITEM_ID_BUFLEN];
      if (focus_candidate_format_item_id(item_id, sizeof(item_id), "document_chunk", c->id) !=
          SUCCESS) {
         OLOG_ERROR("document_adapter: item_id formatting failed (chunk_id=%lld)",
                    (long long)c->id);
         focus_adapter_failure_cleanup(out, produced, out_candidates, out_count);
         out = NULL; /* ownership transferred to failure-cleanup */
         rc = FAILURE;
         goto cleanup;
      }

      const float recency = focus_recency_decay_uniform(c->created_at, now);
      if (focus_candidate_init(&out[produced], "document_chunk", FOCUS_SOURCE_EXTERNAL, rendered,
                               item_id, c->created_at, rows[i].cosine, recency,
                               DOCUMENT_DEFAULT_IMPORTANCE, &truncated_warned) != SUCCESS) {
         OLOG_ERROR("document_adapter: focus_candidate_init failed (chunk_id=%lld)",
                    (long long)c->id);
         focus_adapter_failure_cleanup(out, produced, out_candidates, out_count);
         out = NULL; /* ownership transferred to failure-cleanup */
         rc = FAILURE;
         goto cleanup;
      }
      /* Provenance intentionally zeroed — documents have no
       * conv-based provenance.  Filename is rendered into text. */
      produced++;
   }

cleanup:
   free(chunks);
   free(embed_buf);
   free(rows);
   if (rc == SUCCESS && out != NULL) {
      *out_candidates = out;
      *out_count = produced;
   }
   /* On FAILURE, focus_adapter_failure_cleanup already zeroed the
    * out-params and freed `out`; on SUCCESS-with-no-candidates
    * (loaded==0), out_candidates/out_count remain NULL/0 from the
    * function's top-of-body initialization. */
   return rc;
}

static const focus_source_adapter_t k_document_focus_adapter = {
   .source_id = "document_chunk",
   .source_type = FOCUS_SOURCE_EXTERNAL,
   .requires_embedding = true,
   .query = document_adapter_query,
};

int document_focus_adapter_register(void) {
   return focus_register_source(&k_document_focus_adapter);
}
