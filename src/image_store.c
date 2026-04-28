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
 * Image Store Module
 *
 * Provides filesystem-backed image storage with SQLite metadata.
 * Images are stored as files on disk; the database holds metadata only.
 *
 * Thread Safety: File I/O happens outside the auth_db mutex.
 * Only metadata operations (INSERT/UPDATE/DELETE) hold the lock.
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include "auth/auth_db_internal.h"
#undef AUTH_DB_INTERNAL_ALLOWED

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

#include "image_store.h"
#include "logging.h"

/* =============================================================================
 * Static State
 * ============================================================================= */

static struct {
   bool initialized;
   size_t max_size;
   int max_per_user;
   int retention_days;
   int cache_size_mb;
   char images_dir[IMAGE_PATH_MAX]; /* <data_dir>/images/ */
} s_store = {
   .initialized = false,
   .max_size = IMAGE_MAX_SIZE_DEFAULT,
   .max_per_user = IMAGE_MAX_PER_USER_DEFAULT,
   .retention_days = IMAGE_RETENTION_DAYS_DEFAULT,
   .cache_size_mb = IMAGE_CACHE_SIZE_MB_DEFAULT,
};

/* =============================================================================
 * Internal Helpers
 * ============================================================================= */

/**
 * @brief Generate a random image ID
 *
 * Format: "img_" + 12 alphanumeric characters
 */
static int generate_image_id(char *out) {
   static const char charset[] = "0123456789"
                                 "abcdefghijklmnopqrstuvwxyz"
                                 "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
   unsigned char random_bytes[12];

   if (getrandom(random_bytes, sizeof(random_bytes), 0) != sizeof(random_bytes)) {
      OLOG_ERROR("Image store: getrandom failed");
      return IMAGE_STORE_FAILURE;
   }

   out[0] = 'i';
   out[1] = 'm';
   out[2] = 'g';
   out[3] = '_';

   for (int i = 0; i < 12; i++) {
      out[4 + i] = charset[random_bytes[i] % 62];
   }
   out[16] = '\0';
   return IMAGE_STORE_SUCCESS;
}

/**
 * @brief Get file extension from MIME type
 */
static const char *mime_to_ext(const char *mime_type) {
   if (!mime_type)
      return "bin";
   if (strcmp(mime_type, "image/jpeg") == 0)
      return "jpg";
   if (strcmp(mime_type, "image/png") == 0)
      return "png";
   if (strcmp(mime_type, "image/gif") == 0)
      return "gif";
   if (strcmp(mime_type, "image/webp") == 0)
      return "webp";
   return "bin";
}

/**
 * @brief Build filename from ID and MIME type
 */
static void build_filename(const char *id, const char *mime_type, char *out, size_t out_size) {
   snprintf(out, out_size, "%s.%s", id, mime_to_ext(mime_type));
}

/**
 * @brief Validate a filename from the database contains no path traversal
 */
static bool validate_db_filename(const char *filename) {
   if (!filename || filename[0] == '\0')
      return false;
   if (strchr(filename, '/') || strstr(filename, ".."))
      return false;
   return true;
}

/**
 * @brief Build full filesystem path for an image file
 */
static void build_filepath(const char *filename, char *out, size_t out_size) {
   snprintf(out, out_size, "%s/%s", s_store.images_dir, filename);
}

/**
 * @brief Write data to file atomically (tmp + fsync + rename)
 *
 * Uses O_NOFOLLOW to prevent symlink attacks.
 */
static int write_file_atomic(const char *final_path,
                             const char *filename,
                             const void *data,
                             size_t size) {
   char tmppath[IMAGE_PATH_MAX + 16];
   snprintf(tmppath, sizeof(tmppath), "%s/.%s.tmp", s_store.images_dir, filename);

   int fd = open(tmppath, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0640);
   if (fd < 0) {
      OLOG_ERROR("Image store: failed to create temp file %s: %s", tmppath, strerror(errno));
      return IMAGE_STORE_FAILURE;
   }

   const unsigned char *p = (const unsigned char *)data;
   size_t remaining = size;
   while (remaining > 0) {
      ssize_t written = write(fd, p, remaining);
      if (written < 0) {
         if (errno == EINTR)
            continue;
         OLOG_ERROR("Image store: write failed for %s: %s", tmppath, strerror(errno));
         close(fd);
         unlink(tmppath);
         return IMAGE_STORE_FAILURE;
      }
      p += written;
      remaining -= (size_t)written;
   }

   if (fsync(fd) != 0) {
      OLOG_WARNING("Image store: fsync failed for %s: %s", tmppath, strerror(errno));
   }
   close(fd);

   if (rename(tmppath, final_path) != 0) {
      OLOG_ERROR("Image store: rename failed %s -> %s: %s", tmppath, final_path, strerror(errno));
      unlink(tmppath);
      return IMAGE_STORE_FAILURE;
   }

   return IMAGE_STORE_SUCCESS;
}

/* =============================================================================
 * Lifecycle
 * ============================================================================= */

int image_store_init(const image_store_config_t *config) {
   if (s_store.initialized) {
      return IMAGE_STORE_SUCCESS;
   }

   /* Check that auth_db is ready */
   if (!auth_db_is_ready()) {
      OLOG_ERROR("Image store: auth_db not initialized");
      return IMAGE_STORE_FAILURE;
   }

   /* Apply configuration */
   if (config) {
      if (config->max_size > 0) {
         s_store.max_size = config->max_size;
      }
      if (config->max_per_user > 0) {
         s_store.max_per_user = config->max_per_user;
      }
      s_store.retention_days = config->retention_days;
      if (config->cache_size_mb > 0) {
         s_store.cache_size_mb = config->cache_size_mb;
      }

      /* Set up images directory */
      if (config->data_dir && config->data_dir[0]) {
         snprintf(s_store.images_dir, sizeof(s_store.images_dir), "%s/images", config->data_dir);
      }
   }

   if (s_store.images_dir[0] == '\0') {
      OLOG_ERROR("Image store: no data_dir configured");
      return IMAGE_STORE_FAILURE;
   }

   /* Verify path leaves room for filename: images_dir + '/' + filename (max 32) */
   if (strlen(s_store.images_dir) + 1 + IMAGE_FILENAME_MAX >= IMAGE_PATH_MAX) {
      OLOG_ERROR("Image store: data_dir path too long (%zu chars)", strlen(s_store.images_dir));
      return IMAGE_STORE_FAILURE;
   }

   /* Create images directory if needed */
   if (mkdir(s_store.images_dir, 0750) != 0 && errno != EEXIST) {
      OLOG_ERROR("Image store: failed to create %s: %s", s_store.images_dir, strerror(errno));
      return IMAGE_STORE_FAILURE;
   }

   s_store.initialized = true;
   OLOG_INFO("Image store initialized: dir=%s, max_size=%zu, max_per_user=%d, "
             "retention=%d days, cache=%d MB",
             s_store.images_dir, s_store.max_size, s_store.max_per_user, s_store.retention_days,
             s_store.cache_size_mb);

   return IMAGE_STORE_SUCCESS;
}

void image_store_shutdown(void) {
   s_store.initialized = false;
   OLOG_INFO("Image store shutdown");
}

bool image_store_is_ready(void) {
   return s_store.initialized;
}

/* =============================================================================
 * Validation
 * ============================================================================= */

bool image_store_validate_id(const char *id) {
   if (!id)
      return false;

   /* Must start with "img_" */
   if (strncmp(id, "img_", 4) != 0)
      return false;

   /* Must be exactly 16 characters (img_ + 12 alphanumeric) */
   if (strlen(id) != 16)
      return false;

   /* Characters 4-15 must be alphanumeric */
   for (int i = 4; i < 16; i++) {
      if (!isalnum((unsigned char)id[i]))
         return false;
   }

   return true;
}

bool image_store_validate_mime(const char *mime_type) {
   if (!mime_type)
      return false;

   /* Allowed types - NO SVG (XSS risk) */
   return (strcmp(mime_type, "image/jpeg") == 0 || strcmp(mime_type, "image/png") == 0 ||
           strcmp(mime_type, "image/gif") == 0 || strcmp(mime_type, "image/webp") == 0);
}

/* =============================================================================
 * Image Operations
 * ============================================================================= */

int image_store_save(int user_id,
                     const void *data,
                     size_t size,
                     const char *mime_type,
                     char *id_out) {
   return image_store_save_ex(user_id, data, size, mime_type, IMAGE_SOURCE_UPLOAD,
                              IMAGE_RETAIN_DEFAULT, id_out);
}

int image_store_save_ex(int user_id,
                        const void *data,
                        size_t size,
                        const char *mime_type,
                        image_source_t source,
                        image_retention_t retention,
                        char *id_out) {
   if (!data || size == 0 || !mime_type || !id_out) {
      return IMAGE_STORE_INVALID;
   }

   if (!s_store.initialized) {
      return IMAGE_STORE_FAILURE;
   }

   /* Validate MIME type */
   if (!image_store_validate_mime(mime_type)) {
      return IMAGE_STORE_INVALID;
   }

   /* Check size limit */
   if (size > s_store.max_size) {
      return IMAGE_STORE_TOO_LARGE;
   }

   /* Check user image count (skip for system-wide images) */
   if (user_id > 0) {
      int count = 0;
      if (image_store_count_user(user_id, &count) != IMAGE_STORE_SUCCESS) {
         return IMAGE_STORE_FAILURE;
      }
      if (count >= s_store.max_per_user) {
         return IMAGE_STORE_LIMIT_EXCEEDED;
      }
   }

   /* Generate unique ID and filename */
   if (generate_image_id(id_out) != IMAGE_STORE_SUCCESS) {
      return IMAGE_STORE_FAILURE;
   }

   char filename[IMAGE_FILENAME_MAX];
   build_filename(id_out, mime_type, filename, sizeof(filename));

   char filepath[IMAGE_PATH_MAX];
   build_filepath(filename, filepath, sizeof(filepath));

   /* Write file to disk OUTSIDE the mutex */
   int write_result = write_file_atomic(filepath, filename, data, size);
   if (write_result != IMAGE_STORE_SUCCESS) {
      return write_result;
   }

   /* Insert metadata into database */
   AUTH_DB_LOCK_OR_FAIL();

   time_t now = time(NULL);
   sqlite3_reset(s_db.stmt_image_create);
   sqlite3_bind_text(s_db.stmt_image_create, 1, id_out, -1, SQLITE_STATIC);
   sqlite3_bind_int(s_db.stmt_image_create, 2, user_id);
   sqlite3_bind_int(s_db.stmt_image_create, 3, (int)source);
   sqlite3_bind_int(s_db.stmt_image_create, 4, (int)retention);
   sqlite3_bind_text(s_db.stmt_image_create, 5, mime_type, -1, SQLITE_STATIC);
   sqlite3_bind_int64(s_db.stmt_image_create, 6, (int64_t)size);
   sqlite3_bind_text(s_db.stmt_image_create, 7, filename, -1, SQLITE_STATIC);
   sqlite3_bind_int64(s_db.stmt_image_create, 8, (int64_t)now);

   int rc = sqlite3_step(s_db.stmt_image_create);
   sqlite3_reset(s_db.stmt_image_create);

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("Image store: failed to save metadata for %s: %s", id_out,
                 sqlite3_errmsg(s_db.db));
      unlink(filepath); /* Clean up orphan file */
      return IMAGE_STORE_FAILURE;
   }

   OLOG_INFO("Image store: saved %s (%zu bytes, %s, source=%d, retention=%d)", id_out, size,
             mime_type, (int)source, (int)retention);
   return IMAGE_STORE_SUCCESS;
}

int image_store_get_path(const char *id, int user_id, char *path_out, char *mime_out) {
   if (!id || !path_out) {
      return IMAGE_STORE_INVALID;
   }

   if (!image_store_validate_id(id)) {
      return IMAGE_STORE_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_image_get_file);
   sqlite3_bind_text(s_db.stmt_image_get_file, 1, id, -1, SQLITE_STATIC);

   int rc = sqlite3_step(s_db.stmt_image_get_file);
   if (rc != SQLITE_ROW) {
      sqlite3_reset(s_db.stmt_image_get_file);
      AUTH_DB_UNLOCK();
      return IMAGE_STORE_NOT_FOUND;
   }

   /* Column 0: filename, 1: user_id, 2: source, 3: mime_type, 4: last_accessed */
   const char *filename = (const char *)sqlite3_column_text(s_db.stmt_image_get_file, 0);
   int owner_id = sqlite3_column_int(s_db.stmt_image_get_file, 1);
   int source = sqlite3_column_int(s_db.stmt_image_get_file, 2);
   const char *mime = (const char *)sqlite3_column_text(s_db.stmt_image_get_file, 3);
   time_t last_acc = (time_t)sqlite3_column_int64(s_db.stmt_image_get_file, 4);

   /* Access check: UPLOAD and MMS require ownership; others are accessible to any auth'd user.
    * user_id=0 (service token) skips owner match but still blocks private sources. */
   if ((source == IMAGE_SOURCE_UPLOAD || source == IMAGE_SOURCE_MMS) &&
       (user_id == 0 || owner_id != user_id)) {
      sqlite3_reset(s_db.stmt_image_get_file);
      AUTH_DB_UNLOCK();
      return IMAGE_STORE_FORBIDDEN;
   }

   /* Copy and validate filename (defense-in-depth against DB corruption) */
   char filename_buf[IMAGE_FILENAME_MAX];
   if (filename && validate_db_filename(filename)) {
      strncpy(filename_buf, filename, sizeof(filename_buf) - 1);
      filename_buf[sizeof(filename_buf) - 1] = '\0';
   } else {
      sqlite3_reset(s_db.stmt_image_get_file);
      AUTH_DB_UNLOCK();
      return IMAGE_STORE_FAILURE;
   }

   if (mime_out && mime) {
      strncpy(mime_out, mime, IMAGE_MIME_MAX - 1);
      mime_out[IMAGE_MIME_MAX - 1] = '\0';
   }

   sqlite3_reset(s_db.stmt_image_get_file);

   /* Rate-limit last_accessed updates: only write if >15 min stale (reduces flash wear) */
   time_t now = time(NULL);
   if (now - last_acc > 900) {
      sqlite3_reset(s_db.stmt_image_update_access);
      sqlite3_bind_int64(s_db.stmt_image_update_access, 1, (int64_t)now);
      sqlite3_bind_text(s_db.stmt_image_update_access, 2, id, -1, SQLITE_STATIC);
      sqlite3_step(s_db.stmt_image_update_access);
      sqlite3_reset(s_db.stmt_image_update_access);
   }

   AUTH_DB_UNLOCK();

   /* Build full path */
   build_filepath(filename_buf, path_out, IMAGE_PATH_MAX);

   return IMAGE_STORE_SUCCESS;
}

int image_store_get_metadata(const char *id, image_metadata_t *metadata_out) {
   if (!id || !metadata_out) {
      return IMAGE_STORE_INVALID;
   }

   if (!image_store_validate_id(id)) {
      return IMAGE_STORE_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_image_get);
   sqlite3_bind_text(s_db.stmt_image_get, 1, id, -1, SQLITE_STATIC);

   int rc = sqlite3_step(s_db.stmt_image_get);
   if (rc == SQLITE_ROW) {
      /* Columns: id, user_id, mime_type, size, filename, source, retention_policy,
       *          created_at, last_accessed */
      strncpy(metadata_out->id, (const char *)sqlite3_column_text(s_db.stmt_image_get, 0),
              IMAGE_ID_LEN - 1);
      metadata_out->id[IMAGE_ID_LEN - 1] = '\0';

      metadata_out->user_id = sqlite3_column_int(s_db.stmt_image_get, 1);

      const char *mime = (const char *)sqlite3_column_text(s_db.stmt_image_get, 2);
      if (mime) {
         strncpy(metadata_out->mime_type, mime, IMAGE_MIME_MAX - 1);
         metadata_out->mime_type[IMAGE_MIME_MAX - 1] = '\0';
      } else {
         metadata_out->mime_type[0] = '\0';
      }

      metadata_out->size = (size_t)sqlite3_column_int64(s_db.stmt_image_get, 3);

      const char *fname = (const char *)sqlite3_column_text(s_db.stmt_image_get, 4);
      if (fname) {
         strncpy(metadata_out->filename, fname, IMAGE_FILENAME_MAX - 1);
         metadata_out->filename[IMAGE_FILENAME_MAX - 1] = '\0';
      } else {
         metadata_out->filename[0] = '\0';
      }

      metadata_out->source = (image_source_t)sqlite3_column_int(s_db.stmt_image_get, 5);
      metadata_out->retention_policy = (image_retention_t)sqlite3_column_int(s_db.stmt_image_get,
                                                                             6);
      metadata_out->created_at = (time_t)sqlite3_column_int64(s_db.stmt_image_get, 7);
      metadata_out->last_accessed = (time_t)sqlite3_column_int64(s_db.stmt_image_get, 8);

      sqlite3_reset(s_db.stmt_image_get);
      AUTH_DB_UNLOCK();
      return IMAGE_STORE_SUCCESS;
   }

   sqlite3_reset(s_db.stmt_image_get);
   AUTH_DB_UNLOCK();
   return IMAGE_STORE_NOT_FOUND;
}

int image_store_update_retention(const char *id, int user_id, image_retention_t retention) {
   if (!id || !image_store_validate_id(id)) {
      return IMAGE_STORE_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_image_update_retention);
   sqlite3_bind_int(s_db.stmt_image_update_retention, 1, (int)retention);
   sqlite3_bind_text(s_db.stmt_image_update_retention, 2, id, -1, SQLITE_STATIC);
   sqlite3_bind_int(s_db.stmt_image_update_retention, 3, user_id);
   sqlite3_bind_int(s_db.stmt_image_update_retention, 4, user_id);

   int rc = sqlite3_step(s_db.stmt_image_update_retention);
   int changes = sqlite3_changes(s_db.db);
   sqlite3_reset(s_db.stmt_image_update_retention);

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      return IMAGE_STORE_FAILURE;
   }
   return (changes > 0) ? IMAGE_STORE_SUCCESS : IMAGE_STORE_NOT_FOUND;
}

int image_store_delete(const char *id, int user_id) {
   if (!id) {
      return IMAGE_STORE_INVALID;
   }

   if (!image_store_validate_id(id)) {
      return IMAGE_STORE_INVALID;
   }

   /* Single lock: fetch metadata + check ownership + delete in one acquisition */
   AUTH_DB_LOCK_OR_FAIL();

   /* Get filename and owner for access check */
   sqlite3_reset(s_db.stmt_image_get_file);
   sqlite3_bind_text(s_db.stmt_image_get_file, 1, id, -1, SQLITE_STATIC);

   int rc = sqlite3_step(s_db.stmt_image_get_file);
   if (rc != SQLITE_ROW) {
      sqlite3_reset(s_db.stmt_image_get_file);
      AUTH_DB_UNLOCK();
      return IMAGE_STORE_NOT_FOUND;
   }

   const char *filename = (const char *)sqlite3_column_text(s_db.stmt_image_get_file, 0);
   int owner_id = sqlite3_column_int(s_db.stmt_image_get_file, 1);

   /* Check access permission (user_id=0 bypasses check for admin) */
   if (user_id != 0 && owner_id != user_id) {
      sqlite3_reset(s_db.stmt_image_get_file);
      AUTH_DB_UNLOCK();
      return IMAGE_STORE_FORBIDDEN;
   }

   /* Copy and validate filename (defense-in-depth against DB corruption) */
   char filename_buf[IMAGE_FILENAME_MAX] = { 0 };
   if (filename && validate_db_filename(filename)) {
      strncpy(filename_buf, filename, sizeof(filename_buf) - 1);
   }
   sqlite3_reset(s_db.stmt_image_get_file);

   /* Delete row */
   sqlite3_reset(s_db.stmt_image_delete);
   sqlite3_bind_text(s_db.stmt_image_delete, 1, id, -1, SQLITE_STATIC);
   sqlite3_bind_int(s_db.stmt_image_delete, 2, owner_id);
   rc = sqlite3_step(s_db.stmt_image_delete);
   sqlite3_reset(s_db.stmt_image_delete);

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      return IMAGE_STORE_FAILURE;
   }

   /* Delete file from disk (outside lock) */
   if (filename_buf[0]) {
      char filepath[IMAGE_PATH_MAX];
      build_filepath(filename_buf, filepath, sizeof(filepath));
      if (unlink(filepath) != 0 && errno != ENOENT) {
         OLOG_WARNING("Image store: failed to unlink %s: %s", filepath, strerror(errno));
      }
   }

   OLOG_INFO("Image store: deleted %s", id);
   return IMAGE_STORE_SUCCESS;
}

int image_store_count_user(int user_id, int *count_out) {
   if (count_out)
      *count_out = 0;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_image_count_user);
   sqlite3_bind_int(s_db.stmt_image_count_user, 1, user_id);

   int count = 0;
   if (sqlite3_step(s_db.stmt_image_count_user) == SQLITE_ROW) {
      count = sqlite3_column_int(s_db.stmt_image_count_user, 0);
   }

   sqlite3_reset(s_db.stmt_image_count_user);
   AUTH_DB_UNLOCK();

   if (count_out)
      *count_out = count;
   return IMAGE_STORE_SUCCESS;
}

/* =============================================================================
 * Maintenance
 * ============================================================================= */

int image_store_cleanup(int *deleted_out) {
   if (deleted_out)
      *deleted_out = 0;

   if (!s_store.initialized) {
      return IMAGE_STORE_SUCCESS;
   }

   int total_deleted = 0;

   /* Phase 1: DEFAULT retention — delete images older than retention_days */
   if (s_store.retention_days > 0) {
      time_t cutoff = time(NULL) - (s_store.retention_days * 86400);

      AUTH_DB_LOCK_OR_FAIL();

      /* Collect expired IDs + filenames first */
      sqlite3_reset(s_db.stmt_image_get_expired_ids);
      sqlite3_bind_int64(s_db.stmt_image_get_expired_ids, 1, (int64_t)cutoff);

      /* Collect up to 100 at a time to avoid unbounded memory */
      char ids[100][IMAGE_ID_LEN];
      char filenames[100][IMAGE_FILENAME_MAX];
      int batch_count = 0;

      while (sqlite3_step(s_db.stmt_image_get_expired_ids) == SQLITE_ROW && batch_count < 100) {
         const char *id = (const char *)sqlite3_column_text(s_db.stmt_image_get_expired_ids, 0);
         const char *fn = (const char *)sqlite3_column_text(s_db.stmt_image_get_expired_ids, 1);
         if (id && fn) {
            strncpy(ids[batch_count], id, IMAGE_ID_LEN - 1);
            ids[batch_count][IMAGE_ID_LEN - 1] = '\0';
            strncpy(filenames[batch_count], fn, IMAGE_FILENAME_MAX - 1);
            filenames[batch_count][IMAGE_FILENAME_MAX - 1] = '\0';
            batch_count++;
         }
      }
      sqlite3_reset(s_db.stmt_image_get_expired_ids);

      /* Delete from DB (both params are the cutoff — outer WHERE + inner LIMIT subquery) */
      sqlite3_reset(s_db.stmt_image_delete_old);
      sqlite3_bind_int64(s_db.stmt_image_delete_old, 1, (int64_t)cutoff);
      sqlite3_bind_int64(s_db.stmt_image_delete_old, 2, (int64_t)cutoff);
      sqlite3_step(s_db.stmt_image_delete_old);
      int deleted = sqlite3_changes(s_db.db);
      sqlite3_reset(s_db.stmt_image_delete_old);

      AUTH_DB_UNLOCK();

      /* Unlink files outside the lock */
      for (int i = 0; i < batch_count; i++) {
         char filepath[IMAGE_PATH_MAX];
         build_filepath(filenames[i], filepath, sizeof(filepath));
         if (unlink(filepath) != 0 && errno != ENOENT) {
            OLOG_WARNING("Image store: cleanup failed to unlink %s: %s", filepath, strerror(errno));
         }
      }

      total_deleted += deleted;
   }

   /* Phase 2: CACHE retention — LRU eviction if total exceeds cap */
   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_image_cache_total_size);
   int64_t cache_bytes = 0;
   if (sqlite3_step(s_db.stmt_image_cache_total_size) == SQLITE_ROW) {
      cache_bytes = sqlite3_column_int64(s_db.stmt_image_cache_total_size, 0);
   }
   sqlite3_reset(s_db.stmt_image_cache_total_size);

   int64_t cap = (int64_t)s_store.cache_size_mb * 1024 * 1024;

   if (cache_bytes > cap) {
      /* Pass 1: collect LRU items to evict using running size total */
      sqlite3_reset(s_db.stmt_image_get_cache_lru_ids);

      char cache_ids[100][IMAGE_ID_LEN];
      char cache_filenames[100][IMAGE_FILENAME_MAX];
      int cache_evict_count = 0;

      while (sqlite3_step(s_db.stmt_image_get_cache_lru_ids) == SQLITE_ROW && cache_bytes > cap &&
             cache_evict_count < 100) {
         /* Columns: 0=id, 1=filename, 2=size */
         const char *cid = (const char *)sqlite3_column_text(s_db.stmt_image_get_cache_lru_ids, 0);
         const char *fn = (const char *)sqlite3_column_text(s_db.stmt_image_get_cache_lru_ids, 1);
         int64_t row_size = sqlite3_column_int64(s_db.stmt_image_get_cache_lru_ids, 2);
         if (cid && fn) {
            strncpy(cache_ids[cache_evict_count], cid, IMAGE_ID_LEN - 1);
            cache_ids[cache_evict_count][IMAGE_ID_LEN - 1] = '\0';
            strncpy(cache_filenames[cache_evict_count], fn, IMAGE_FILENAME_MAX - 1);
            cache_filenames[cache_evict_count][IMAGE_FILENAME_MAX - 1] = '\0';
            cache_evict_count++;
            cache_bytes -= row_size;
         }
      }
      sqlite3_reset(s_db.stmt_image_get_cache_lru_ids);

      /* Pass 2: delete collected rows by ID (uses pre-prepared statement) */
      for (int i = 0; i < cache_evict_count; i++) {
         sqlite3_reset(s_db.stmt_image_delete_cache_lru);
         sqlite3_bind_text(s_db.stmt_image_delete_cache_lru, 1, cache_ids[i], -1, SQLITE_STATIC);
         if (sqlite3_step(s_db.stmt_image_delete_cache_lru) == SQLITE_DONE) {
            total_deleted++;
         }
         sqlite3_reset(s_db.stmt_image_delete_cache_lru);
      }

      AUTH_DB_UNLOCK();

      /* Pass 3: unlink cache files outside the lock */
      for (int i = 0; i < cache_evict_count; i++) {
         char filepath[IMAGE_PATH_MAX];
         build_filepath(cache_filenames[i], filepath, sizeof(filepath));
         if (unlink(filepath) != 0 && errno != ENOENT) {
            OLOG_WARNING("Image store: cache cleanup failed to unlink %s: %s", filepath,
                         strerror(errno));
         }
      }
   } else {
      AUTH_DB_UNLOCK();
   }

   if (total_deleted > 0) {
      OLOG_INFO("Image store: cleaned up %d images", total_deleted);
   }

   if (deleted_out)
      *deleted_out = total_deleted;
   return IMAGE_STORE_SUCCESS;
}

int image_store_stats(int *total_count, int64_t *total_bytes) {
   if (!total_count || !total_bytes) {
      return IMAGE_STORE_INVALID;
   }

   *total_count = 0;
   *total_bytes = 0;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_image_stats);
   if (sqlite3_step(s_db.stmt_image_stats) == SQLITE_ROW) {
      *total_count = sqlite3_column_int(s_db.stmt_image_stats, 0);
      *total_bytes = sqlite3_column_int64(s_db.stmt_image_stats, 1);
   }
   sqlite3_reset(s_db.stmt_image_stats);

   AUTH_DB_UNLOCK();

   return IMAGE_STORE_SUCCESS;
}
