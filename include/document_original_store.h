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
 * Document Original Store — the document-original consumer of the generic blob
 * store.  Keeps `s_db` access and the document-specific policy (owner-only read,
 * doc MIME map) in Layer 2; webui/admin (Layer 4) call only this public API.
 * Return codes are the BLOB_STORE_* space (see blob_store.h).
 */

#ifndef DOCUMENT_ORIGINAL_STORE_H
#define DOCUMENT_ORIGINAL_STORE_H

#include <stdbool.h>
#include <stddef.h>

#include "blob_store.h" /* BLOB_STORE_* codes, BLOB_ID_LEN, BLOB_MIME_MAX, BLOB_PATH_MAX */
#include "config/dawn_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Register the document-original blob table.  Calls blob_store_init()
 *  (idempotent) then registers the descriptor.  Safe to call once at startup. */
int document_originals_init(const documents_config_t *cfg, const char *data_dir);

void document_originals_shutdown(void);
bool document_originals_ready(void);

/**
 * @brief Store an original uploaded file.  @p content_hash (sha256 hex over the
 *  raw bytes) enables per-user dedup.  @p filename_original is the user-facing
 *  name (download).  @p id_out receives the blob id (>= BLOB_ID_LEN bytes).
 */
int document_original_save(int user_id,
                           const void *data,
                           size_t size,
                           const char *mime_type,
                           const char *content_hash,
                           const char *filename_original,
                           char *id_out);

/** @brief Resolve a blob id to its filesystem path (owner-only). */
int document_original_get_path(const char *blob_id, int user_id, char *path_out, char *mime_out);

/** @brief Fetch the user-facing original filename for a blob (for download
 *  Content-Disposition).  @p out >= BLOB_FILENAME_MAX is conservative; original
 *  names can be longer, so pass @p out_size. */
int document_original_get_filename(const char *blob_id, int user_id, char *out, size_t out_size);

int document_original_delete_user(int user_id);
int document_original_count_user(int user_id, int *count_out);
int document_original_cleanup(int *deleted_out);
int document_original_cleanup_orphans(int grace_sec, int *deleted_out);

bool document_original_validate_id(const char *blob_id);

#ifdef __cplusplus
}
#endif

#endif /* DOCUMENT_ORIGINAL_STORE_H */
