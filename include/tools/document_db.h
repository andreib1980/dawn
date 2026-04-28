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
 * Document Database Layer - CRUD for documents and document_chunks tables
 *
 * Part of the RAG document search system. Uses the shared auth_db handle
 * and prepared statements. All functions are thread-safe via the auth_db mutex.
 */

#ifndef DOCUMENT_DB_H
#define DOCUMENT_DB_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Constants
 * ============================================================================= */

#define DOC_FILENAME_MAX 256
#define DOC_FILEPATH_MAX 512
#define DOC_FILETYPE_MAX 16
#define DOC_HASH_MAX 65 /* SHA-256 hex + null */
#define DOC_MAX_RESULTS 100
#define DOC_CHUNK_TEXT_MAX 4096

/* =============================================================================
 * Types
 * ============================================================================= */

typedef struct {
   int64_t id;
   int user_id;
   char filename[DOC_FILENAME_MAX];
   char filepath[DOC_FILEPATH_MAX];
   char filetype[DOC_FILETYPE_MAX];
   char file_hash[DOC_HASH_MAX];
   int num_chunks;
   bool is_global;
   int64_t created_at;
   char owner_name[65]; /* Populated only by document_db_list_all (JOIN) */
} document_t;

typedef struct {
   int64_t id;
   int chunk_index;
   char text[DOC_CHUNK_TEXT_MAX];
   float *embedding; /* Caller-managed buffer */
   float embedding_norm;
   int64_t document_id;
   char doc_filename[DOC_FILENAME_MAX];
   char doc_filetype[DOC_FILETYPE_MAX];
   int64_t created_at; /* v35 — chunk origin timestamp; 0 = unknown */
} document_chunk_t;

/* =============================================================================
 * Document CRUD
 * ============================================================================= */

/**
 * @brief Create a new document record
 *
 * @param user_id  Owner (0 for global/filesystem-ingested)
 * @param filename Display name
 * @param filepath Original path
 * @param filetype Extension (pdf, docx, txt, md)
 * @param file_hash SHA-256 hex string
 * @param num_chunks Number of chunks
 * @param is_global Whether accessible to all users
 * @param[out] id_out Created document ID (must not be NULL)
 * @return SUCCESS (0) on success, FAILURE (1) on error
 */
int document_db_create(int user_id,
                       const char *filename,
                       const char *filepath,
                       const char *filetype,
                       const char *file_hash,
                       int num_chunks,
                       bool is_global,
                       int64_t *id_out);

/**
 * @brief Get a document by ID
 * @return SUCCESS (0) on success, FAILURE (1) on not found/error
 */
int document_db_get(int64_t doc_id, document_t *out);

/**
 * @brief Check if a document with this hash already exists for the user
 *
 * @param file_hash SHA-256 hex string
 * @param user_id   User ID
 * @param[out] id_out Existing document ID if found, 0 if not found (must not be NULL)
 * @return SUCCESS (0) on success, FAILURE (1) on error
 */
int document_db_find_by_hash(const char *file_hash, int user_id, int64_t *id_out);

/**
 * @brief List documents accessible to a user (own + global), paginated
 *
 * @param user_id   User ID
 * @param out       Output array (caller allocates)
 * @param limit     Maximum documents to return
 * @param offset    Pagination offset
 * @param[out] count_out Number of documents written to out[] (must not be NULL)
 * @return SUCCESS (0) on success, FAILURE (1) on error
 */
int document_db_list(int user_id, document_t *out, int limit, int offset, int *count_out);

/**
 * @brief List all documents across all users (admin view), paginated
 *
 * @param out       Output array (caller allocates)
 * @param limit     Maximum documents to return
 * @param offset    Pagination offset
 * @param[out] count_out Number of documents written to out[] (must not be NULL)
 * @return SUCCESS (0) on success, FAILURE (1) on error
 */
int document_db_list_all(document_t *out, int limit, int offset, int *count_out);

/**
 * @brief Update a document's global visibility flag
 * @return SUCCESS (0) on success, FAILURE (1) on error
 */
int document_db_update_global(int64_t doc_id, bool is_global);

/**
 * @brief Delete a document and all its chunks (cascade)
 * @return SUCCESS (0) on success, FAILURE (1) on error
 */
int document_db_delete(int64_t doc_id);

/**
 * @brief Count documents owned by a specific user (excludes global)
 *
 * @param user_id   User ID
 * @param[out] count_out Number of documents (must not be NULL)
 * @return SUCCESS (0) on success, FAILURE (1) on error
 */
int document_db_count_user(int user_id, int *count_out);

/* =============================================================================
 * Chunk CRUD
 * ============================================================================= */

/**
 * @brief Create a chunk for a document
 *
 * @param document_id Parent document
 * @param chunk_index Order within document (0-based)
 * @param text Chunk text
 * @param embedding Float vector (copied as BLOB)
 * @param dims Number of dimensions
 * @param embedding_norm Pre-computed L2 norm
 * @param created_at Chunk origin timestamp (0 = unknown)
 * @param[out] id_out Created chunk ID (must not be NULL)
 * @return SUCCESS (0) on success, FAILURE (1) on error
 */
int document_db_chunk_create(int64_t document_id,
                             int chunk_index,
                             const char *text,
                             const float *embedding,
                             int dims,
                             float embedding_norm,
                             int64_t created_at,
                             int64_t *id_out);

/**
 * @brief Find a document by name (exact match preferred, then partial)
 *
 * Searches documents accessible to the user (own + global).
 * If name is all digits, tries ID lookup first.
 *
 * @param user_id User ID
 * @param name Document name or partial name to search for
 * @param out Output document struct
 * @return SUCCESS (0) on success, FAILURE (1) on not found/error
 */
int document_db_find_by_name(int user_id, const char *name, document_t *out);

/**
 * @brief Read chunks from a document in order (paginated)
 *
 * @param document_id Document ID
 * @param chunks Output array (caller allocates, only chunk_index and text populated)
 * @param max_count Maximum chunks to read
 * @param start_chunk Starting chunk index (offset)
 * @param[out] count_out Number of chunks read (must not be NULL)
 * @return SUCCESS (0) on success, FAILURE (1) on error
 */
int document_db_chunk_read(int64_t document_id,
                           document_chunk_t *chunks,
                           int max_count,
                           int start_chunk,
                           int *count_out);

/**
 * @brief Load all chunks accessible to a user for vector search
 *
 * Caller must provide embedding_buf with enough space for max_count * dims floats.
 * Each chunk's embedding pointer is set into embedding_buf.
 *
 * @param user_id User ID (loads own docs + global)
 * @param chunks Output array
 * @param embedding_buf Flat float buffer for embeddings
 * @param dims Expected embedding dimensions
 * @param max_count Maximum chunks to load
 * @param[out] count_out Number of chunks loaded (must not be NULL)
 * @return SUCCESS (0) on success, FAILURE (1) on error
 */
int document_db_chunk_search_load(int user_id,
                                  document_chunk_t *chunks,
                                  float *embedding_buf,
                                  int dims,
                                  int max_count,
                                  int *count_out);

#ifdef __cplusplus
}
#endif

#endif /* DOCUMENT_DB_H */
