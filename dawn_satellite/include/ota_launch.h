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
 * dawn-satellite-launch — rollback-agent decision surface.
 *
 * The launcher's commit / count / rollback decision (and its marker + rollback
 * side effects) live in launch_step() so they can be host-unit-tested without
 * the terminal execv() that main() performs.  See ota_launch.c (main) and
 * ota_launch_core.c (launch_step), and docs/OTA_DESIGN.md §7.
 */

#ifndef OTA_LAUNCH_H
#define OTA_LAUNCH_H

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LLOG(...)                                   \
   do {                                             \
      fprintf(stderr, "[ota-launch] " __VA_ARGS__); \
      fputc('\n', stderr);                          \
   } while (0)

/* The decision the launcher took this start (the caller then execv()s the
 * binary regardless — these describe what happened to the probation marker). */
typedef enum {
   OTA_LAUNCH_BOOT = 0,  /* absent / fail-safe marker → ordinary boot */
   OTA_LAUNCH_COMMIT,    /* ran_ok or probation expired → marker removed */
   OTA_LAUNCH_COUNT,     /* under threshold → bumped boot count persisted */
   OTA_LAUNCH_ROLLBACK,  /* >= threshold and slot restored → marker removed */
   OTA_LAUNCH_NO_TARGET, /* >= threshold but no usable restore slot → marker kept */
} ota_launch_action_t;

/**
 * @brief Run the rollback-agent decision for one start: read the pending marker,
 *        decide commit/count/rollback, and apply the marker + rollback side
 *        effects.  Performs everything EXCEPT the terminal execv (the caller
 *        does that), so it is fully host-testable.
 * @param now  Current unix time (injected so the probation-expiry branch is
 *             deterministic in tests; production passes time(NULL)).
 * @return The action taken (see ota_launch_action_t).
 */
ota_launch_action_t launch_step(int64_t now);

#ifdef __cplusplus
}
#endif

#endif /* OTA_LAUNCH_H */
