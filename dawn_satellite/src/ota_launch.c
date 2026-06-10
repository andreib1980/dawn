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
 * dawn-satellite-launch — the OTA rollback agent (entry point).
 *
 * systemd runs THIS as ExecStart, not dawn_satellite directly.  It is a tiny,
 * libc-only, dependency-free helper that is installed once and NEVER
 * OTA-updated, so it always runs even when a freshly-applied binary is
 * dead-on-arrival (won't dynamically link, crashes before main()).  On each
 * start it runs launch_step() — which reads the pending-update marker and
 * decides commit / count / rollback (see ota_launch_core.c) — then execv()s the
 * real binary.  Because the decision happens here — before control ever reaches
 * the (possibly unlaunchable) binary — rollback works for a binary that can't
 * run its own startup code.  See docs/OTA_DESIGN.md §7.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* execv */
#endif

#include "ota_launch.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ota_marker.h"

/* exec the real binary with the launcher's pass-through args (argv[0] becomes
 * the binary path so it sees a sane name).  Returns ONLY on failure. */
static void exec_binary(char **argv, int argc) {
   char *newargv[64];
   int n = 0;
   newargv[n++] = (char *)OTA_BIN_PATH;
   for (int i = 1; i < argc && n < (int)(sizeof(newargv) / sizeof(newargv[0])) - 1; i++) {
      newargv[n++] = argv[i];
   }
   newargv[n] = NULL;
   execv(OTA_BIN_PATH, newargv);
   /* only reaches here if execv failed (missing/corrupt/non-exec ELF) */
   LLOG("exec failed: %s: %s", OTA_BIN_PATH, strerror(errno));
   exit(1); /* non-zero → systemd counts a failed start toward the next rollback */
}

int main(int argc, char **argv) {
   /* All of the commit/count/rollback decision + marker side effects (the
    * host-tested logic) happen in launch_step(); main() only adds the terminal
    * execv that a unit test can't perform. */
   (void)launch_step((int64_t)time(NULL));
   exec_binary(argv, argc);
   return 1; /* unreachable (exec_binary exits) */
}
