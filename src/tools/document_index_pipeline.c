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
 * Shared RAG document indexing pipeline — chunk, embed, store
 *
 * Extracted from webui_doc_library.c so both WebUI upload and the
 * document_index LLM tool can share the same indexing pipeline.
 */

#include "tools/document_index_pipeline.h"

#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config/dawn_config.h"
#include "core/embedding_engine.h"
#include "dawn_error.h"
#include "logging.h"
#include "memory/memory_stem.h"
#include "tools/document_chunker.h"
#include "tools/document_db.h"

/* =============================================================================
 * Helpers
 * ============================================================================= */

static void sha256_hex(const char *data, size_t len, char *out_hex) {
   unsigned char hash[SHA256_DIGEST_LENGTH];
   SHA256((const unsigned char *)data, len, hash);
   for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
      snprintf(out_hex + (i * 2), 3, "%02x", hash[i]);
   }
   out_hex[SHA256_DIGEST_LENGTH * 2] = '\0';
}

static void set_error(doc_index_result_t *out, int code, const char *msg) {
   out->error_code = code;
   out->doc_id = -1;
   snprintf(out->error_msg, sizeof(out->error_msg), "%s", msg);
}

/* =============================================================================
 * Public API
 * ============================================================================= */

const char *document_index_error_string(int error_code) {
   switch (error_code) {
      case DOC_INDEX_SUCCESS:
         return "Success";
      case DOC_INDEX_ERROR_EMPTY:
         return "Document text is empty";
      case DOC_INDEX_ERROR_TOO_LARGE:
         return "Document text exceeds maximum size";
      case DOC_INDEX_ERROR_LIMIT:
         return "Document limit reached";
      case DOC_INDEX_ERROR_NO_EMBEDDING:
         return "Embedding engine not available";
      case DOC_INDEX_ERROR_DUPLICATE:
         return "Document already indexed (duplicate content)";
      case DOC_INDEX_ERROR_CHUNK_FAIL:
         return "Failed to chunk document text";
      case DOC_INDEX_ERROR_DB_FAIL:
         return "Failed to create document record";
      case DOC_INDEX_ERROR_ALLOC:
         return "Memory allocation failed";
      default:
         return "Unknown indexing error";
   }
}

int document_index_text(int user_id,
                        const char *filename,
                        const char *filetype,
                        const char *text,
                        size_t text_len,
                        bool is_global,
                        doc_index_result_t *out) {
   if (!out)
      return DOC_INDEX_ERROR_ALLOC;

   memset(out, 0, sizeof(*out));
   out->doc_id = -1;

   /* Validate text */
   if (!text || text_len == 0) {
      set_error(out, DOC_INDEX_ERROR_EMPTY, "Document text is empty");
      return DOC_INDEX_ERROR_EMPTY;
   }

   if (text_len > (size_t)g_config.documents.max_index_size_kb * 1024) {
      set_error(out, DOC_INDEX_ERROR_TOO_LARGE, "Document text exceeds maximum size");
      return DOC_INDEX_ERROR_TOO_LARGE;
   }

   /* Check user document count limit */
   int user_doc_count = 0;
   document_db_count_user(user_id, &user_doc_count);
   if (user_doc_count >= g_config.documents.max_indexed_documents) {
      char msg[128];
      snprintf(msg, sizeof(msg), "Document limit reached (%d max)",
               g_config.documents.max_indexed_documents);
      set_error(out, DOC_INDEX_ERROR_LIMIT, msg);
      return DOC_INDEX_ERROR_LIMIT;
   }

   /* Check embedding engine */
   if (!embedding_engine_available()) {
      set_error(out, DOC_INDEX_ERROR_NO_EMBEDDING, "Embedding engine not available");
      return DOC_INDEX_ERROR_NO_EMBEDDING;
   }

   /* Compute file hash for dedup */
   char file_hash[65];
   sha256_hex(text, text_len, file_hash);

   /* Check for duplicate */
   int64_t existing = 0;
   document_db_find_by_hash(file_hash, user_id, &existing);
   if (existing > 0) {
      char msg[128];
      snprintf(msg, sizeof(msg), "Document already indexed (id=%lld)", (long long)existing);
      set_error(out, DOC_INDEX_ERROR_DUPLICATE, msg);
      return DOC_INDEX_ERROR_DUPLICATE;
   }

   /* Chunk the text */
   chunk_config_t chunk_cfg = CHUNK_CONFIG_DEFAULT;
   chunk_result_t chunks;
   if (document_chunk_text(text, &chunk_cfg, &chunks) != 0 || chunks.count == 0) {
      set_error(out, DOC_INDEX_ERROR_CHUNK_FAIL, "Failed to chunk document text");
      return DOC_INDEX_ERROR_CHUNK_FAIL;
   }

   int dims = embedding_engine_dims();

   /* Create document record */
   int64_t doc_id = 0;
   if (document_db_create(user_id, filename, filename, filetype, file_hash, chunks.count, is_global,
                          &doc_id) != SUCCESS) {
      set_error(out, DOC_INDEX_ERROR_DB_FAIL, "Failed to create document record");
      chunk_result_free(&chunks);
      return DOC_INDEX_ERROR_DB_FAIL;
   }

   /* Embed and store each chunk */
   float *emb_buf = malloc((size_t)dims * sizeof(float));
   int embedded_count = 0;
   int failed_count = 0;

   if (emb_buf) {
      /* Capture ingest time once so all chunks of the same document share an
       * identical created_at.  Calling time(NULL) per-chunk would give chunks
       * slightly different timestamps across a slow embedding pass, which
       * contradicts the "inherit from the document's ingest time" intent. */
      int64_t ingest_ts = (int64_t)time(NULL);

      /* v61: stem the filename/label ONCE (same for every chunk) outside the
       * auth_db lock (leaf-lock rule).  Each chunk's body is stemmed in-loop,
       * also outside the lock; document_db_chunk_index_fts does only the locked
       * insert.  FTS indexing is best-effort — a failure (e.g. v61 migration not
       * yet run) never fails the ingest; search degrades to pure-semantic. */
      char label_stems[MEMORY_FACT_STEMS_MAX];
      (void)memory_stem_string(filename, label_stems, sizeof(label_stems));

      for (int i = 0; i < chunks.count; i++) {
         int out_dims = 0;
         int rc = embedding_engine_embed(chunks.chunks[i], emb_buf, dims, &out_dims);
         if (rc != 0 || out_dims != dims) {
            failed_count++;
            continue;
         }

         float norm = embedding_engine_l2_norm(emb_buf, dims);
         int64_t chunk_id = 0;
         if (document_db_chunk_create(doc_id, i, chunks.chunks[i], emb_buf, dims, norm, ingest_ts,
                                      &chunk_id) == SUCCESS) {
            embedded_count++;
            char body_stems[MEMORY_FACT_STEMS_MAX];
            (void)memory_stem_string(chunks.chunks[i], body_stems, sizeof(body_stems));
            (void)document_db_chunk_index_fts(chunk_id, label_stems, body_stems);
         } else {
            failed_count++;
         }
      }
      free(emb_buf);
   } else {
      /* Memory allocation failed — delete the document record */
      document_db_delete(doc_id);
      chunk_result_free(&chunks);
      set_error(out, DOC_INDEX_ERROR_ALLOC, "Memory allocation failed");
      return DOC_INDEX_ERROR_ALLOC;
   }

   chunk_result_free(&chunks);

   OLOG_INFO("document_index_pipeline: indexed '%s' — %d chunks embedded, %d failed%s", filename,
             embedded_count, failed_count, is_global ? " [GLOBAL]" : "");

   out->doc_id = doc_id;
   out->num_chunks = embedded_count;
   out->failed_chunks = failed_count;
   out->error_code = DOC_INDEX_SUCCESS;
   out->error_msg[0] = '\0';
   return DOC_INDEX_SUCCESS;
}

/* v61: save a short authored "note" — a single-chunk document whose filename IS
 * the user's label.  Bypasses the chunker entirely (M-1): rejects anything the
 * chunker would split so num_chunks == 1 holds by construction on every path
 * (WebUI and tool).  No hash-dedup: identical bodies under different labels are
 * legitimate, and the tool layer handles same-label overwrite via note_update. */
int document_index_note(int user_id,
                        const char *label,
                        const char *text,
                        size_t text_len,
                        bool is_global,
                        doc_index_result_t *out) {
   if (!out)
      return DOC_INDEX_ERROR_ALLOC;
   memset(out, 0, sizeof(*out));
   out->doc_id = -1;

   if (!label || !label[0]) {
      set_error(out, DOC_INDEX_ERROR_EMPTY, "Note label is empty");
      return DOC_INDEX_ERROR_EMPTY;
   }
   if (!text || text_len == 0) {
      set_error(out, DOC_INDEX_ERROR_EMPTY, "Note text is empty");
      return DOC_INDEX_ERROR_EMPTY;
   }

   chunk_config_t cfg = CHUNK_CONFIG_DEFAULT;
   if (chunk_estimate_tokens(text, (int)text_len) > cfg.max_tokens) {
      set_error(
          out, DOC_INDEX_ERROR_TOO_LARGE,
          "Note is too long to file as a single note — shorten it or upload it as a document");
      return DOC_INDEX_ERROR_TOO_LARGE;
   }

   if (!embedding_engine_available()) {
      set_error(out, DOC_INDEX_ERROR_NO_EMBEDDING, "Embedding engine not available");
      return DOC_INDEX_ERROR_NO_EMBEDDING;
   }

   int user_doc_count = 0;
   if (document_db_count_user(user_id, &user_doc_count) == SUCCESS &&
       user_doc_count >= g_config.documents.max_indexed_documents) {
      set_error(out, DOC_INDEX_ERROR_LIMIT, "Document limit reached");
      return DOC_INDEX_ERROR_LIMIT;
   }

   int dims = embedding_engine_dims();
   if (dims <= 0) {
      set_error(out, DOC_INDEX_ERROR_NO_EMBEDDING, "Embedding engine not available");
      return DOC_INDEX_ERROR_NO_EMBEDDING;
   }

   char file_hash[65];
   sha256_hex(text, text_len, file_hash);

   int64_t doc_id = 0;
   if (document_db_create(user_id, label, label, "note", file_hash, 1, is_global, &doc_id) !=
       SUCCESS) {
      set_error(out, DOC_INDEX_ERROR_DB_FAIL, "Failed to create note record");
      return DOC_INDEX_ERROR_DB_FAIL;
   }

   float *emb = malloc((size_t)dims * sizeof(float));
   if (!emb) {
      document_db_delete(doc_id);
      set_error(out, DOC_INDEX_ERROR_ALLOC, "Memory allocation failed");
      return DOC_INDEX_ERROR_ALLOC;
   }
   int out_dims = 0;
   if (embedding_engine_embed(text, emb, dims, &out_dims) != 0 || out_dims != dims) {
      free(emb);
      document_db_delete(doc_id);
      set_error(out, DOC_INDEX_ERROR_CHUNK_FAIL, "Failed to embed note text");
      return DOC_INDEX_ERROR_CHUNK_FAIL;
   }
   float norm = embedding_engine_l2_norm(emb, dims);
   int64_t chunk_id = 0;
   int rc = document_db_chunk_create(doc_id, 0, text, emb, dims, norm, (int64_t)time(NULL),
                                     &chunk_id);
   free(emb);
   if (rc != SUCCESS) {
      document_db_delete(doc_id);
      set_error(out, DOC_INDEX_ERROR_DB_FAIL, "Failed to store note chunk");
      return DOC_INDEX_ERROR_DB_FAIL;
   }

   char label_stems[MEMORY_FACT_STEMS_MAX], body_stems[MEMORY_FACT_STEMS_MAX];
   (void)memory_stem_string(label, label_stems, sizeof(label_stems));
   (void)memory_stem_string(text, body_stems, sizeof(body_stems));
   (void)document_db_chunk_index_fts(chunk_id, label_stems, body_stems);

   out->doc_id = doc_id;
   out->num_chunks = 1;
   out->failed_chunks = 0;
   out->error_code = DOC_INDEX_SUCCESS;
   out->error_msg[0] = '\0';
   OLOG_INFO("document_index_pipeline: saved note '%s' (doc %lld)", label, (long long)doc_id);
   return DOC_INDEX_SUCCESS;
}

/* v61: edit an existing note in place (re-embed + delegate the stable-id DB
 * swap).  Same single-chunk cap as create.  document_db_note_update enforces the
 * note + ownership gate. */
int document_note_update(int user_id,
                         int64_t doc_id,
                         const char *new_label,
                         const char *new_text,
                         size_t new_len,
                         doc_index_result_t *out) {
   if (!out)
      return DOC_INDEX_ERROR_ALLOC;
   memset(out, 0, sizeof(*out));
   out->doc_id = doc_id;

   if (!new_label || !new_label[0] || !new_text || new_len == 0) {
      set_error(out, DOC_INDEX_ERROR_EMPTY, "Note label and text are required");
      return DOC_INDEX_ERROR_EMPTY;
   }
   chunk_config_t cfg = CHUNK_CONFIG_DEFAULT;
   if (chunk_estimate_tokens(new_text, (int)new_len) > cfg.max_tokens) {
      set_error(
          out, DOC_INDEX_ERROR_TOO_LARGE,
          "Note is too long to file as a single note — shorten it or upload it as a document");
      return DOC_INDEX_ERROR_TOO_LARGE;
   }
   if (!embedding_engine_available()) {
      set_error(out, DOC_INDEX_ERROR_NO_EMBEDDING, "Embedding engine not available");
      return DOC_INDEX_ERROR_NO_EMBEDDING;
   }
   int dims = embedding_engine_dims();
   if (dims <= 0) {
      set_error(out, DOC_INDEX_ERROR_NO_EMBEDDING, "Embedding engine not available");
      return DOC_INDEX_ERROR_NO_EMBEDDING;
   }
   float *emb = malloc((size_t)dims * sizeof(float));
   if (!emb) {
      set_error(out, DOC_INDEX_ERROR_ALLOC, "Memory allocation failed");
      return DOC_INDEX_ERROR_ALLOC;
   }
   int out_dims = 0;
   if (embedding_engine_embed(new_text, emb, dims, &out_dims) != 0 || out_dims != dims) {
      free(emb);
      set_error(out, DOC_INDEX_ERROR_CHUNK_FAIL, "Failed to embed note text");
      return DOC_INDEX_ERROR_CHUNK_FAIL;
   }
   float norm = embedding_engine_l2_norm(emb, dims);
   char new_hash[65];
   sha256_hex(new_text, new_len, new_hash);
   int rc = document_db_note_update(user_id, doc_id, new_label, new_text, emb, dims, norm,
                                    new_hash);
   free(emb);
   if (rc != SUCCESS) {
      set_error(out, DOC_INDEX_ERROR_DB_FAIL,
                "Failed to update note (not a note, not yours, or a database error)");
      return DOC_INDEX_ERROR_DB_FAIL;
   }
   out->doc_id = doc_id;
   out->num_chunks = 1;
   out->error_code = DOC_INDEX_SUCCESS;
   out->error_msg[0] = '\0';
   return DOC_INDEX_SUCCESS;
}
