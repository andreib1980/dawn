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
