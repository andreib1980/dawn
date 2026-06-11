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
 * OTA fleet rollout orchestrator (Layer 2) — canary-then-rollout for `push all`.
 * See docs/OTA_DESIGN.md §Phase 5.
 *
 * Pushes a release to every eligible device of a platform/tier, but safely: one
 * CANARY device first, and only after it re-registers reporting the new version
 * (the server-owned commit, ota_finalize_on_register) does the rest fan out.  A
 * canary that reverts (re-registers on the old version) or never comes back
 * (timeout) HALTS the rollout — the remaining devices are never touched.
 *
 * Single active rollout at a time, held in memory (a daemon restart cancels it;
 * individual device state still persists + reconciles as usual).  Delivery is via
 * a callback the transport layer registers (ota_rollout_set_push_fn) so this stays
 * free of the webui dependency ota.h deliberately avoids.
 */

#ifndef OTA_ROLLOUT_H
#define OTA_ROLLOUT_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include "core/ota_manifest.h" /* ota_platform_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Seconds to wait for the canary to commit before declaring the rollout failed
 * (covers the silent-canary case — a bricked/offline canary fires no event). */
#define OTA_ROLLOUT_CANARY_TIMEOUT_SEC 600
/* Max devices a single rollout tracks (fleet cap; excess eligible devices are
 * reported and skipped rather than silently dropped). */
#define OTA_ROLLOUT_MAX_TARGETS 64

/**
 * @brief Register the per-device delivery function (transport layer, Layer 4).
 *
 * Called once at init with webui_ota_push.  Until set, ota_rollout_start() fails
 * (no way to deliver an offer).  Signature matches webui_ota_push():
 *   returns AUTH_DB_SUCCESS | AUTH_DB_LOCKED | AUTH_DB_NOT_FOUND | AUTH_DB_FAILURE.
 */
typedef int (*ota_rollout_push_fn)(const char *uuid, const char *version, bool allow_downgrade);
void ota_rollout_set_push_fn(ota_rollout_push_fn fn);

/**
 * @brief Start a canary-then-rollout of @p version to a platform/tier.
 *
 * Enumerates eligible devices (tier match, enabled, online, not already on
 * @p version), pushes to the canary, and arms the canary deadline.  The rest are
 * held until the canary commits.  Fails if a rollout is already active, no device
 * is eligible, no push fn is registered, or the canary push fails.
 *
 * @param platform        Target platform.
 * @param tier            Target tier (1 = RPi, 2 = ESP32).
 * @param version         Release version to roll out.
 * @param allow_downgrade Permit offering older-than-installed.
 * @param summary_out     Optional buffer; receives a one-line human summary.
 * @param summary_len     Size of summary_out.
 * @return SUCCESS, or FAILURE (with the reason in summary_out when provided).
 */
int ota_rollout_start(ota_platform_t platform,
                      int tier,
                      const char *version,
                      bool allow_downgrade,
                      char *summary_out,
                      size_t summary_len);

/**
 * @brief Commit event — a device re-registered reporting its target version.
 *
 * Hooked from ota_finalize_on_register.  If it is the canary, fans out to the
 * rest; if a fan-out target, advances the completion count.  No-op when no
 * rollout is active or the uuid isn't part of it.
 */
void ota_rollout_on_commit(const char *uuid, const char *version);

/**
 * @brief Failure event — a device that was mid-update re-registered on a
 *        DIFFERENT version (reverted / update didn't take).
 *
 * Hooked from ota_finalize_on_register's mismatch path.  A canary failure halts
 * the rollout (no fan-out); a fan-out target failure is counted and the rest
 * continue.  No-op when no rollout is active or the uuid isn't part of it.
 */
void ota_rollout_on_fail(const char *uuid, const char *version);

/**
 * @brief Periodic tick — fails an active rollout whose canary deadline passed.
 *        Call roughly once a second from the main loop.  @p now is time(NULL).
 */
void ota_rollout_tick(time_t now);

/**
 * @brief Write a human-readable status of the current/last rollout into @p buf.
 * @return SUCCESS always (writes "No rollout has run." when idle).
 */
int ota_rollout_status(char *buf, size_t len);

/**
 * @brief Abort an in-progress rollout (operator command).  No-op if none active.
 * @return SUCCESS if a rollout was aborted, FAILURE if none was active.
 */
int ota_rollout_abort(void);

#ifdef __cplusplus
}
#endif

#endif /* OTA_ROLLOUT_H */
