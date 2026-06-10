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
 * dawn-satellite-launch — decision + side effects (everything but the execv).
 *
 * Split out of main() (ota_launch.c) so the commit / count / rollback logic is
 * host-unit-testable (test_ota_launch).  Pure libc + the shared marker codec;
 * no curl/sodium/SDL — this links into the frozen, always-runs launcher.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* O_DIRECTORY, O_CLOEXEC */
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "dawn_error.h"
#include "ota_launch.h"
#include "ota_marker.h"

/* Atomically restore the rollback binary over the live binary path.  Copies
 * (does NOT move) so the rollback slot survives a second failure / manual
 * recovery.  Returns SUCCESS or FAILURE (empty/missing/non-exec slot). */
static int restore_rollback(void) {
   struct stat st;
   if (stat(OTA_ROLLBACK_BIN, &st) != 0 || st.st_size <= 0 || !(st.st_mode & S_IXUSR)) {
      return FAILURE; /* empty/missing/non-exec slot */
   }

   int src = open(OTA_ROLLBACK_BIN, O_RDONLY | O_CLOEXEC);
   if (src < 0) {
      return FAILURE;
   }
   char tmp[600];
   if ((size_t)snprintf(tmp, sizeof(tmp), "%s.restore", OTA_BIN_PATH) >= sizeof(tmp)) {
      close(src);
      return FAILURE;
   }
   unlink(tmp);
   int dst = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0755);
   if (dst < 0) {
      close(src);
      return FAILURE;
   }

   char buf[OTA_IO_CHUNK_BYTES];
   ssize_t r;
   int ok = 1;
   while ((r = read(src, buf, sizeof(buf))) > 0) {
      ssize_t off = 0;
      while (off < r) {
         ssize_t w = write(dst, buf + off, (size_t)(r - off));
         if (w <= 0) {
            ok = 0;
            break;
         }
         off += w;
      }
      if (!ok) {
         break;
      }
   }
   if (r < 0) {
      ok = 0;
   }
   close(src);
   if (ok && fsync(dst) != 0) {
      ok = 0;
   }
   close(dst);
   if (!ok) {
      unlink(tmp);
      return FAILURE;
   }
   if (rename(tmp, OTA_BIN_PATH) != 0) {
      unlink(tmp);
      return FAILURE;
   }
   ota_fsync_dir_of(OTA_BIN_PATH);
   return SUCCESS;
}

ota_launch_action_t launch_step(int64_t now) {
   ota_marker_t m;
   ota_marker_status_t st = ota_marker_read(OTA_PENDING_MARKER, &m);

   if (st == OTA_MARKER_ABSENT) {
      return OTA_LAUNCH_BOOT; /* normal boot */
   }
   if (st == OTA_MARKER_FAILSAFE) {
      /* Unrecognized / corrupt marker — never brick on it; boot normally. */
      LLOG("marker unreadable/unknown — booting normally, no rollback");
      return OTA_LAUNCH_BOOT;
   }

   /* st == OTA_MARKER_OK: an update is on probation. */
   int expired = (m.created > 0 && now - m.created > OTA_PROBATION_SEC);

   if (m.ran_ok || expired) {
      /* Commit: the update proved it runs (registered or >= 60 s uptime), or the
       * probation window elapsed.  Full commit — remove the marker so unrelated
       * later crashes are normal Restart=always, never re-counted to rollback. */
      LLOG("commit target=%s (ran_ok=%d expired=%d) — clearing probation", m.target_version,
           m.ran_ok, expired);
      ota_marker_remove(OTA_PENDING_MARKER);
      return OTA_LAUNCH_COMMIT;
   }

   /* Still on probation: count this boot. */
   m.boots++;
   if (m.boots >= OTA_ROLLBACK_BOOT_THRESHOLD) {
      if (restore_rollback() == SUCCESS) {
         LLOG("ROLLBACK: target=%s crash-looped (%d boots) — restored %s. If it still fails, "
              "suspect server/network, not the image.",
              m.target_version, m.boots, m.prev_version);
         ota_marker_remove(OTA_PENDING_MARKER);
         return OTA_LAUNCH_ROLLBACK;
      }
      /* No usable restore target — do NOT clear the marker; keep trying so a
       * reachable server can re-push, and surface the condition loudly. */
      LLOG("ROLLBACK NEEDED but no restore target at %s — MANUAL RECOVERY REQUIRED",
           OTA_ROLLBACK_BIN);
      return OTA_LAUNCH_NO_TARGET;
   }

   /* Under threshold: persist the bumped count BEFORE the caller execs so a
    * dead-on-arrival binary's attempt is recorded (1 systemd start = 1 bump). */
   if (ota_marker_write(OTA_PENDING_MARKER, &m) != SUCCESS) {
      LLOG("warning: could not persist boot count (marker write failed)");
   }
   LLOG("probation boot %d/%d target=%s — launching", m.boots, OTA_ROLLBACK_BOOT_THRESHOLD,
        m.target_version);
   return OTA_LAUNCH_COUNT;
}
