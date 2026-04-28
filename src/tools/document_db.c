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
 * Document Database Layer - SQLite CRUD for documents and document_chunks
 *
 * Part of the RAG document search system. Accesses s_db directly
 * (same pattern as scheduler_db.c). All functions acquire the auth_db mutex.
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include "tools/document_db.h"

#include <string.h>
#include <time.h>

#include "auth/auth_db_internal.h"
#include "dawn_error.h"
#include "logging.h"

/* =============================================================================
 * Helper: safe string copy from SQLite column
 * ============================================================================= */

static void col_text_copy(char *dst, size_t dst_size, sqlite3_stmt *stmt, int col) {
   const char *src = (const char *)sqlite3_column_text(stmt, col);
   if (src) {
      size_t len = strlen(src);
      if (len >= dst_size)
         len = dst_size - 1;
      memcpy(dst, src, len);
      dst[len] = '\0';
   } else {
      dst[0] = '\0';
   }
}

/* =============================================================================
 * Helper: populate document_t from a SELECT row
 * Columns: id, user_id, filename, filepath, filetype, file_hash,
 *          num_chunks, is_global, created_at
 * ============================================================================= */

static void row_to_document(sqlite3_stmt *stmt, document_t *doc) {
   doc->id = sqlite3_column_int64(stmt, 0);
   doc->user_id = sqlite3_column_int(stmt, 1);
   col_text_copy(doc->filename, sizeof(doc->filename), stmt, 2);
   col_text_copy(doc->filepath, sizeof(doc->filepath), stmt, 3);
   col_text_copy(doc->filetype, sizeof(doc->filetype), stmt, 4);
   col_text_copy(doc->file_hash, sizeof(doc->file_hash), stmt, 5);
   doc->num_chunks = sqlite3_column_int(stmt, 6);
   doc->is_global = sqlite3_column_int(stmt, 7) != 0;
   doc->created_at = sqlite3_column_int64(stmt, 8);
}

/* =============================================================================
 * Document CRUD
 * ============================================================================= */

int document_db_create(int user_id,
                       const char *filename,
                       const char *filepath,
                       const char *filetype,
                       const char *file_hash,
                       int num_chunks,
                       bool is_global,
                       int64_t *id_out) {
   if (!filename || !filepath || !filetype || !file_hash || !id_out)
      return FAILURE;

   *id_out = 0;
   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_doc_create;
   sqlite3_reset(stmt);

   if (user_id > 0)
      sqlite3_bind_int(stmt, 1, user_id);
   else
      sqlite3_bind_null(stmt, 1);
   sqlite3_bind_text(stmt, 2, filename, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, filepath, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 4, filetype, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 5, file_hash, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 6, num_chunks);
   sqlite3_bind_int(stmt, 7, is_global ? 1 : 0);
   sqlite3_bind_int64(stmt, 8, (int64_t)time(NULL));

   int rc = sqlite3_step(stmt);
   int result = FAILURE;
   if (rc == SQLITE_DONE) {
      *id_out = sqlite3_last_insert_rowid(s_db.db);
      result = SUCCESS;
   } else {
      OLOG_ERROR("document_db: create failed: %s", sqlite3_errmsg(s_db.db));
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   return result;
}

int document_db_get(int64_t doc_id, document_t *out) {
   if (!out)
      return FAILURE;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_doc_get;
   sqlite3_reset(stmt);
   sqlite3_bind_int64(stmt, 1, doc_id);

   int result = FAILURE;
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      row_to_document(stmt, out);
      result = SUCCESS;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   return result;
}

int document_db_find_by_hash(const char *file_hash, int user_id, int64_t *id_out) {
   if (!file_hash || !id_out)
      return FAILURE;

   *id_out = 0;
   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_doc_get_by_hash;
   sqlite3_reset(stmt);
   sqlite3_bind_text(stmt, 1, file_hash, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 2, user_id);

   /* *id_out remains 0 (not found) unless a row is returned */
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      *id_out = sqlite3_column_int64(stmt, 0);
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   return SUCCESS;
}

int document_db_list(int user_id, document_t *out, int limit, int offset, int *count_out) {
   if (!out || limit <= 0 || !count_out)
      return FAILURE;
   *count_out = 0;
   if (limit > DOC_MAX_RESULTS)
      limit = DOC_MAX_RESULTS;
   if (offset < 0)
      offset = 0;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_doc_list;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int(stmt, 2, limit);
   sqlite3_bind_int(stmt, 3, offset);

   int count = 0;
   while (count < limit && sqlite3_step(stmt) == SQLITE_ROW) {
      row_to_document(stmt, &out[count]);
      count++;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   *count_out = count;
   return SUCCESS;
}

int document_db_list_all(document_t *out, int limit, int offset, int *count_out) {
   if (!out || limit <= 0 || !count_out)
      return FAILURE;
   *count_out = 0;
   if (limit > DOC_MAX_RESULTS)
      limit = DOC_MAX_RESULTS;
   if (offset < 0)
      offset = 0;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_doc_list_all;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, limit);
   sqlite3_bind_int(stmt, 2, offset);

   int count = 0;
   while (count < limit && sqlite3_step(stmt) == SQLITE_ROW) {
      row_to_document(stmt, &out[count]);
      /* Column 9 = COALESCE(u.username, '') from the JOIN */
      col_text_copy(out[count].owner_name, sizeof(out[count].owner_name), stmt, 9);
      count++;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   *count_out = count;
   return SUCCESS;
}

int document_db_update_global(int64_t doc_id, bool is_global) {
   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_doc_update_global;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, is_global ? 1 : 0);
   sqlite3_bind_int64(stmt, 2, doc_id);

   int result = FAILURE;
   if (sqlite3_step(stmt) == SQLITE_DONE) {
      result = (sqlite3_changes(s_db.db) > 0) ? SUCCESS : FAILURE;
   } else {
      OLOG_ERROR("document_db: update_global failed: %s", sqlite3_errmsg(s_db.db));
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   return result;
}

int document_db_delete(int64_t doc_id) {
   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_doc_delete;
   sqlite3_reset(stmt);
   sqlite3_bind_int64(stmt, 1, doc_id);

   int result = FAILURE;
   if (sqlite3_step(stmt) == SQLITE_DONE) {
      result = (sqlite3_changes(s_db.db) > 0) ? SUCCESS : FAILURE;
   } else {
      OLOG_ERROR("document_db: delete failed: %s", sqlite3_errmsg(s_db.db));
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   return result;
}

int document_db_count_user(int user_id, int *count_out) {
   if (!count_out)
      return FAILURE;
   *count_out = 0;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_doc_count_user;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);

   int result = FAILURE;
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      *count_out = sqlite3_column_int(stmt, 0);
      result = SUCCESS;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   return result;
}

/* =============================================================================
 * Document Lookup by Name
 * ============================================================================= */

int document_db_find_by_name(int user_id, const char *name, document_t *out) {
   if (!name || !out)
      return FAILURE;

   /* If name is all digits, try direct ID lookup first */
   bool all_digits = true;
   for (const char *p = name; *p; p++) {
      if (*p < '0' || *p > '9') {
         all_digits = false;
         break;
      }
   }
   if (all_digits && name[0] != '\0') {
      int64_t doc_id = strtoll(name, NULL, 10);
      if (document_db_get(doc_id, out) == SUCCESS) {
         /* Verify user access: must be owner or global */
         if (out->user_id == user_id || out->is_global)
            return SUCCESS;
      }
   }

   /* Escape LIKE wildcards in name */
   size_t name_len = strlen(name);
   char escaped[DOC_FILENAME_MAX * 2];
   size_t esc_len = 0;
   for (size_t i = 0; i < name_len && esc_len + 1 < sizeof(escaped); i++) {
      if (name[i] == '%' || name[i] == '_' || name[i] == '\\') {
         escaped[esc_len++] = '\\';
      }
      escaped[esc_len++] = name[i];
   }
   escaped[esc_len] = '\0';

   /* Build LIKE pattern: %escaped_name% */
   char pattern[DOC_FILENAME_MAX * 2 + 3];
   if (esc_len + 2 >= sizeof(pattern))
      return FAILURE;
   pattern[0] = '%';
   memcpy(pattern + 1, escaped, esc_len);
   pattern[esc_len + 1] = '%';
   pattern[esc_len + 2] = '\0';

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_doc_find_by_name;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, pattern, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, name, -1, SQLITE_TRANSIENT);

   int result = FAILURE;
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      row_to_document(stmt, out);
      result = SUCCESS;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   return result;
}

/* =============================================================================
 * Chunk Read (Paginated)
 * ============================================================================= */

int document_db_chunk_read(int64_t document_id,
                           document_chunk_t *chunks,
                           int max_count,
                           int start_chunk,
                           int *count_out) {
   if (!chunks || max_count <= 0 || start_chunk < 0 || !count_out)
      return FAILURE;

   *count_out = 0;
   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_doc_chunk_read;
   sqlite3_reset(stmt);
   sqlite3_bind_int64(stmt, 1, document_id);
   sqlite3_bind_int(stmt, 2, max_count);
   sqlite3_bind_int(stmt, 3, start_chunk);

   int count = 0;
   while (count < max_count && sqlite3_step(stmt) == SQLITE_ROW) {
      document_chunk_t *c = &chunks[count];
      memset(c, 0, sizeof(*c));
      c->chunk_index = sqlite3_column_int(stmt, 0);
      col_text_copy(c->text, sizeof(c->text), stmt, 1);
      c->document_id = document_id;
      count++;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   *count_out = count;
   return SUCCESS;
}

/* =============================================================================
 * Chunk CRUD
 * ============================================================================= */

int document_db_chunk_create(int64_t document_id,
                             int chunk_index,
                             const char *text,
                             const float *embedding,
                             int dims,
                             float embedding_norm,
                             int64_t created_at,
                             int64_t *id_out) {
   if (!text || !embedding || dims <= 0 || !id_out)
      return FAILURE;

   *id_out = 0;
   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_doc_chunk_create;
   sqlite3_reset(stmt);

   sqlite3_bind_int64(stmt, 1, document_id);
   sqlite3_bind_int(stmt, 2, chunk_index);
   sqlite3_bind_text(stmt, 3, text, -1, SQLITE_TRANSIENT);
   sqlite3_bind_blob(stmt, 4, embedding, dims * (int)sizeof(float), SQLITE_TRANSIENT);
   sqlite3_bind_double(stmt, 5, (double)embedding_norm);
   sqlite3_bind_int64(stmt, 6, created_at); /* 0 = unknown — schema default also 0 */

   int rc = sqlite3_step(stmt);
   int result = FAILURE;
   if (rc == SQLITE_DONE) {
      *id_out = sqlite3_last_insert_rowid(s_db.db);
      result = SUCCESS;
   } else {
      OLOG_ERROR("document_db: chunk_create failed: %s", sqlite3_errmsg(s_db.db));
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   return result;
}

int document_db_chunk_search_load(int user_id,
                                  document_chunk_t *chunks,
                                  float *embedding_buf,
                                  int dims,
                                  int max_count,
                                  int *count_out) {
   if (!chunks || !embedding_buf || dims <= 0 || max_count <= 0 || !count_out)
      return FAILURE;

   *count_out = 0;
   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_doc_chunk_search;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int(stmt, 2, max_count);

   int count = 0;
   int expected_blob_size = dims * (int)sizeof(float);

   while (count < max_count && sqlite3_step(stmt) == SQLITE_ROW) {
      /* Verify embedding dimensions match */
      int blob_size = sqlite3_column_bytes(stmt, 3);
      if (blob_size != expected_blob_size)
         continue;

      document_chunk_t *c = &chunks[count];
      c->id = sqlite3_column_int64(stmt, 0);
      c->chunk_index = sqlite3_column_int(stmt, 1);
      col_text_copy(c->text, sizeof(c->text), stmt, 2);

      /* Copy embedding into the flat buffer */
      const void *blob = sqlite3_column_blob(stmt, 3);
      float *emb_dest = embedding_buf + (count * dims);
      memcpy(emb_dest, blob, (size_t)blob_size);
      c->embedding = emb_dest;
      c->embedding_norm = (float)sqlite3_column_double(stmt, 4);

      c->document_id = sqlite3_column_int64(stmt, 5);
      col_text_copy(c->doc_filename, sizeof(c->doc_filename), stmt, 6);
      col_text_copy(c->doc_filetype, sizeof(c->doc_filetype), stmt, 7);
      c->created_at = sqlite3_column_int64(stmt, 8); /* v35 — 0 if column was NULL/0 */

      count++;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   *count_out = count;
   return SUCCESS;
}
