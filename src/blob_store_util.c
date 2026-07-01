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
 * Blob store — pure helpers (no DB / no module state).
 *
 * Kept separate from the engine so the unit test can link them standalone.
 * Adapted from the static helpers in image_store.c (generate_image_id,
 * mime_to_ext, build_filename, validate_db_filename), parameterized by id
 * prefix and MIME map.
 */

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/random.h>

#include "blob_store.h"

/* Id layout: 4-char prefix + BLOB_ID_RANDOM_LEN random base62 chars + NUL. */
#define BLOB_ID_PREFIX_LEN 4
#define BLOB_ID_RANDOM_LEN 12
#define BLOB_ID_CHARSET_SIZE 62 /* digits + lower + upper */
/* Largest multiple of the charset size that fits in a byte (4*62 = 248); bytes
 * >= this are rejected so `% CHARSET_SIZE` stays unbiased (256 is not a multiple
 * of 62).  NB: this corrects a prior hand-coded literal of 252 — 252 is not a
 * multiple of 62, so it leaked a slight bias toward chars 0-3. */
#define BLOB_ID_REJECT_AT (256 / BLOB_ID_CHARSET_SIZE * BLOB_ID_CHARSET_SIZE) /* = 248 */

int blob_generate_id(const char *prefix, char *out) {
   static const char charset[] = "0123456789"
                                 "abcdefghijklmnopqrstuvwxyz"
                                 "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
   if (!prefix || !out || strlen(prefix) != BLOB_ID_PREFIX_LEN) {
      return BLOB_STORE_INVALID;
   }

   memcpy(out, prefix, BLOB_ID_PREFIX_LEN);
   /* Draw randomness in one batch instead of a syscall per byte; refill only if
    * rejection sampling drains the buffer.  Rejection keeps the base62 mapping
    * unbiased (see BLOB_ID_REJECT_AT). */
   unsigned char rnd[32];
   size_t avail = 0, pos = 0;
   for (int i = 0; i < BLOB_ID_RANDOM_LEN; i++) {
      unsigned char b;
      do {
         if (pos >= avail) {
            if (getrandom(rnd, sizeof(rnd), 0) != (ssize_t)sizeof(rnd)) {
               return BLOB_STORE_FAILURE;
            }
            avail = sizeof(rnd);
            pos = 0;
         }
         b = rnd[pos++];
      } while (b >= BLOB_ID_REJECT_AT);
      out[BLOB_ID_PREFIX_LEN + i] = charset[b % BLOB_ID_CHARSET_SIZE];
   }
   out[BLOB_ID_PREFIX_LEN + BLOB_ID_RANDOM_LEN] = '\0';
   return BLOB_STORE_SUCCESS;
}

bool blob_validate_id(const char *id, const char *prefix) {
   if (!id || !prefix || strlen(prefix) != 4) {
      return false;
   }
   if (strncmp(id, prefix, 4) != 0) {
      return false;
   }
   if (strlen(id) != 16) {
      return false;
   }
   for (int i = 4; i < 16; i++) {
      if (!isalnum((unsigned char)id[i])) {
         return false;
      }
   }
   return true;
}

const char *blob_mime_to_ext(const char *mime_type, const blob_mime_map_t *map, size_t map_count) {
   if (!mime_type || !map) {
      return "bin";
   }
   for (size_t i = 0; i < map_count; i++) {
      if (map[i].mime && strcmp(mime_type, map[i].mime) == 0) {
         return map[i].ext;
      }
   }
   return "bin";
}

void blob_build_filename(const char *id,
                         const char *mime_type,
                         const blob_mime_map_t *map,
                         size_t map_count,
                         char *out,
                         size_t out_size) {
   snprintf(out, out_size, "%s.%s", id, blob_mime_to_ext(mime_type, map, map_count));
}

bool blob_validate_db_filename(const char *filename) {
   if (!filename || filename[0] == '\0') {
      return false;
   }
   if (strchr(filename, '/') || strstr(filename, "..")) {
      return false;
   }
   return true;
}
