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
 * OTA marker codec — see ota_marker.h.  Defensive read (fail-safe on anything
 * unrecognized) + atomic-replace write.  Pure libc; shared by the satellite
 * binary and the standalone launcher.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* O_DIRECTORY, O_CLOEXEC, fdopen, fileno */
#endif

#include "ota_marker.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "dawn_error.h"

/* Bounded line buffer — a marker line is `key=value\n`, all values short. */
#define OTA_MARKER_LINE_MAX 256
/* Defensive ceiling on the boot counter (a corrupt huge value still parses,
 * but is clamped so arithmetic stays sane; >= threshold simply rolls back). */
#define OTA_MARKER_BOOTS_MAX 100000

/* Parse a bounded "key=value" line in place.  Returns 1 and sets key+val on
 * success, 0 on a blank/malformed line (caller decides tolerance). */
static int split_kv(char *line, char **key, char **val) {
   /* Strip trailing CR/LF. */
   size_t n = strlen(line);
   while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
      line[--n] = '\0';
   }
   if (n == 0) {
      return 0;
   }
   char *eq = strchr(line, '=');
   if (!eq || eq == line) {
      return 0;
   }
   *eq = '\0';
   *key = line;
   *val = eq + 1;
   return 1;
}

/* strtoll with strict full-string validation into [min,max].  64-bit so a
 * unix timestamp doesn't saturate on a 32-bit build (else a post-2038 clock
 * would fail the parse → FAILSAFE → no rollback).  Returns SUCCESS + *out, or
 * FAILURE on any non-numeric / out-of-range input. */
static int parse_ll(const char *s, long long min, long long max, long long *out) {
   if (!s || !*s) {
      return FAILURE;
   }
   errno = 0;
   char *end = NULL;
   long long v = strtoll(s, &end, 10);
   if (errno != 0 || end == s || *end != '\0' || v < min || v > max) {
      return FAILURE;
   }
   *out = v;
   return SUCCESS;
}

static void copy_version(char *dst, const char *src) {
   size_t i = 0;
   for (; src[i] && i < OTA_VERSION_STR_MAX - 1; i++) {
      dst[i] = src[i];
   }
   dst[i] = '\0';
}

ota_marker_status_t ota_marker_read(const char *path, ota_marker_t *out) {
   FILE *f = fopen(path, "re");
   if (!f) {
      return (errno == ENOENT) ? OTA_MARKER_ABSENT : OTA_MARKER_FAILSAFE;
   }

   ota_marker_t m = { 0 };
   m.marker_version = -1; /* sentinel: must be set by the file */
   int saw_version = 0;
   char line[OTA_MARKER_LINE_MAX];

   while (fgets(line, sizeof(line), f)) {
      /* A line longer than the buffer (no newline yet, full buffer) is
       * treated as corruption — bail fail-safe. */
      if (strlen(line) == sizeof(line) - 1 && line[sizeof(line) - 2] != '\n') {
         fclose(f);
         return OTA_MARKER_FAILSAFE;
      }
      char *key = NULL, *val = NULL;
      if (!split_kv(line, &key, &val)) {
         continue; /* blank/comment-ish line — tolerate */
      }
      long long lv;
      if (strcmp(key, "marker_version") == 0) {
         if (parse_ll(val, 0, INT32_MAX, &lv) != SUCCESS) {
            fclose(f);
            return OTA_MARKER_FAILSAFE;
         }
         m.marker_version = (int)lv;
         saw_version = 1;
      } else if (strcmp(key, "boots") == 0) {
         if (parse_ll(val, 0, OTA_MARKER_BOOTS_MAX, &lv) != SUCCESS) {
            fclose(f);
            return OTA_MARKER_FAILSAFE;
         }
         m.boots = (int)lv;
      } else if (strcmp(key, "created") == 0) {
         if (parse_ll(val, 0, INT64_MAX, &lv) != SUCCESS) {
            fclose(f);
            return OTA_MARKER_FAILSAFE;
         }
         m.created = (int64_t)lv;
      } else if (strcmp(key, "ran_ok") == 0) {
         if (parse_ll(val, 0, 1, &lv) != SUCCESS) {
            fclose(f);
            return OTA_MARKER_FAILSAFE;
         }
         m.ran_ok = (int)lv;
      } else if (strcmp(key, "target_version") == 0) {
         copy_version(m.target_version, val);
      } else if (strcmp(key, "prev_version") == 0) {
         copy_version(m.prev_version, val);
      }
      /* unknown keys ignored (forward-compat) */
   }
   fclose(f);

   /* A marker with no recognizable version field, or a version this build does
    * not understand, is fail-safe (frozen launcher must not brick on a newer
    * marker written by a future binary). */
   if (!saw_version || m.marker_version != OTA_MARKER_VERSION) {
      return OTA_MARKER_FAILSAFE;
   }
   if (out) {
      *out = m;
   }
   return OTA_MARKER_OK;
}

/* fsync the directory containing @p path so the rename/unlink is durable.
 * Shared (declared in ota_marker.h) by the apply worker + launcher restore. */
void ota_fsync_dir_of(const char *path) {
   char buf[512];
   size_t n = strlen(path);
   if (n >= sizeof(buf)) {
      return;
   }
   memcpy(buf, path, n + 1);
   char *slash = strrchr(buf, '/');
   if (!slash) {
      return;
   }
   *slash = '\0';
   int dfd = open(buf[0] ? buf : "/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
   if (dfd >= 0) {
      (void)fsync(dfd);
      close(dfd);
   }
}

int ota_marker_write(const char *path, const ota_marker_t *m) {
   if (!path || !m) {
      return FAILURE;
   }
   char tmp[600];
   if ((size_t)snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= sizeof(tmp)) {
      return FAILURE;
   }

   int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
   if (fd < 0) {
      return FAILURE;
   }
   FILE *f = fdopen(fd, "w");
   if (!f) {
      close(fd);
      unlink(tmp);
      return FAILURE;
   }
   /* marker_version first so an old reader keys off it immediately. */
   fprintf(f, "marker_version=%d\n", OTA_MARKER_VERSION);
   fprintf(f, "target_version=%s\n", m->target_version);
   fprintf(f, "prev_version=%s\n", m->prev_version);
   fprintf(f, "boots=%d\n", m->boots);
   fprintf(f, "created=%lld\n", (long long)m->created);
   fprintf(f, "ran_ok=%d\n", m->ran_ok ? 1 : 0);

   if (fflush(f) != 0 || fsync(fileno(f)) != 0) {
      fclose(f);
      unlink(tmp);
      return FAILURE;
   }
   fclose(f);

   if (rename(tmp, path) != 0) {
      unlink(tmp);
      return FAILURE;
   }
   ota_fsync_dir_of(path);
   return SUCCESS;
}

int ota_marker_remove(const char *path) {
   if (!path) {
      return FAILURE;
   }
   if (unlink(path) != 0 && errno != ENOENT) {
      return FAILURE;
   }
   ota_fsync_dir_of(path);
   return SUCCESS;
}
