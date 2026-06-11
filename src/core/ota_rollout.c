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
 * OTA fleet rollout orchestrator — canary-then-rollout for `push all`.  See
 * ota_rollout.h and docs/OTA_DESIGN.md §Phase 5.
 */

#include "core/ota_rollout.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "auth/auth_db.h"         /* satellite_db_list, satellite_mapping_t, AUTH_DB_* */
#include "core/ota_db.h"          /* ota_db_get, ota_device_state_t, OTA_* sizes */
#include "core/session_manager.h" /* session_find_by_uuid / session_release (online check) */
#include "dawn_error.h"           /* SUCCESS / FAILURE */
#include "logging.h"

/* ── rollout state (single active rollout, in memory) ─────────────────────── */

typedef enum {
   RO_IDLE = 0,    /* nothing has run, or the last one is reported done/failed */
   RO_WAIT_CANARY, /* canary offer delivered, waiting for its commit */
   RO_FANNING,     /* canary committed; fan-out wave pushed, waiting on the rest */
   RO_DONE,        /* terminal: all settled (committed or failed/timed-out) */
   RO_FAILED,      /* terminal: canary reverted or timed out — rest never touched */
} ro_state_t;

typedef struct {
   char uuid[SATELLITE_UUID_MAX];
   bool committed;
   bool failed;
} ro_target_t;

static struct {
   ro_state_t state;
   ota_platform_t platform;
   int tier;
   char version[OTA_VERSION_MAX];
   bool allow_downgrade;
   ro_target_t targets[OTA_ROLLOUT_MAX_TARGETS]; /* targets[0] is the canary */
   int n_targets;
   int n_skipped;     /* eligible-but-dropped (online offline / overflow) for the summary */
   bool needs_fanout; /* canary committed; tick should deliver the fan-out wave */
   time_t deadline;   /* canary-commit deadline, then fan-out-commit deadline */
   char last_error[OTA_ERROR_MAX];
} s_ro;

static pthread_mutex_t s_ro_mutex = PTHREAD_MUTEX_INITIALIZER;
static ota_rollout_push_fn s_push_fn = NULL;

void ota_rollout_set_push_fn(ota_rollout_push_fn fn) {
   pthread_mutex_lock(&s_ro_mutex);
   s_push_fn = fn;
   pthread_mutex_unlock(&s_ro_mutex);
}

/* ── eligibility enumeration ──────────────────────────────────────────────── */

/* Collected under the auth_db lock by satellite_db_list — we ONLY copy uuids of
 * the matching tier here (no ota_db_get / session lookups, which would re-enter
 * auth_db / take the session lock while auth_db is held).  Eligibility that needs
 * those is resolved afterwards, once this returns and the auth_db lock is freed. */
typedef struct {
   char uuids[OTA_ROLLOUT_MAX_TARGETS][SATELLITE_UUID_MAX];
   int count;
   int tier;
   int n_overflowed; /* matching devices beyond the cap, never examined */
} candidate_set_t;

static int collect_candidate(const satellite_mapping_t *m, void *ctx) {
   candidate_set_t *cs = (candidate_set_t *)ctx;
   if (!m->enabled || m->tier != cs->tier) {
      return 0; /* continue */
   }
   if (cs->count >= OTA_ROLLOUT_MAX_TARGETS) {
      cs->n_overflowed++;
      return 0;
   }
   snprintf(cs->uuids[cs->count], SATELLITE_UUID_MAX, "%s", m->uuid);
   cs->count++;
   return 0;
}

/* Is @p uuid online right now? */
static bool device_online(const char *uuid) {
   session_t *s = session_find_by_uuid(uuid);
   if (!s) {
      return false;
   }
   session_release(s);
   return true;
}

/* ── start ─────────────────────────────────────────────────────────────────── */

int ota_rollout_start(ota_platform_t platform,
                      int tier,
                      const char *version,
                      bool allow_downgrade,
                      char *summary_out,
                      size_t summary_len) {
   if (summary_out && summary_len) {
      summary_out[0] = '\0';
   }
   if (!version || !version[0]) {
      return FAILURE;
   }

#define RO_FAIL(...)                                      \
   do {                                                   \
      if (summary_out && summary_len) {                   \
         snprintf(summary_out, summary_len, __VA_ARGS__); \
      }                                                   \
      return FAILURE;                                     \
   } while (0)

   pthread_mutex_lock(&s_ro_mutex);
   bool busy = (s_ro.state == RO_WAIT_CANARY || s_ro.state == RO_FANNING);
   ota_rollout_push_fn push_fn = s_push_fn;
   pthread_mutex_unlock(&s_ro_mutex);

   if (busy) {
      RO_FAIL("A rollout is already in progress (abort it first).");
   }
   if (!push_fn) {
      RO_FAIL("No delivery transport registered — cannot start a rollout.");
   }

   /* 1) Collect tier candidates under the auth_db lock (uuids only). */
   candidate_set_t cs;
   memset(&cs, 0, sizeof(cs));
   cs.tier = tier;
   if (satellite_db_list(collect_candidate, &cs) != AUTH_DB_SUCCESS) {
      RO_FAIL("Failed to enumerate satellites.");
   }

   /* 2) Resolve eligibility (online + not already on the target) outside the
    *    auth_db lock.  Build the rollout target list with the canary at [0]. */
   ro_target_t targets[OTA_ROLLOUT_MAX_TARGETS];
   int n = 0;
   int skipped = 0;
   for (int i = 0; i < cs.count; i++) {
      const char *uuid = cs.uuids[i];
      if (!device_online(uuid)) {
         skipped++;
         continue;
      }
      ota_device_state_t st;
      if (ota_db_get(uuid, &st) == AUTH_DB_SUCCESS && strcmp(st.current_version, version) == 0) {
         skipped++; /* already on the target version */
         continue;
      }
      snprintf(targets[n].uuid, SATELLITE_UUID_MAX, "%s", uuid);
      targets[n].committed = false;
      targets[n].failed = false;
      n++;
   }
   skipped += cs.n_overflowed; /* matching devices past the cap (never examined) */

   if (n == 0) {
      RO_FAIL("No eligible devices (none online, or all already on %s).", version);
   }

   /* 3) Push the canary (targets[0]) synchronously — we're on the admin thread,
    *    the same context handle_ota_push_cmd already pushes from.  This happens
    *    BEFORE the rollout is armed (step 4): a commit/fail event in that gap would
    *    hit RO_IDLE and be dropped, but the window is unreachable in practice — a
    *    commit requires the device to download, flash, reboot and re-register, which
    *    is seconds at minimum, far longer than the few microseconds to step 4. */
   int rc = push_fn(targets[0].uuid, version, allow_downgrade);
   if (rc != AUTH_DB_SUCCESS) {
      const char *why = (rc == AUTH_DB_LOCKED)      ? "already mid-update"
                        : (rc == AUTH_DB_NOT_FOUND) ? "went offline"
                                                    : "push failed";
      RO_FAIL("Canary %s could not be offered (%s) — rollout not started.", targets[0].uuid, why);
   }

   /* 4) Arm the rollout. */
   pthread_mutex_lock(&s_ro_mutex);
   memset(&s_ro, 0, sizeof(s_ro));
   s_ro.state = RO_WAIT_CANARY;
   s_ro.platform = platform;
   s_ro.tier = tier;
   snprintf(s_ro.version, sizeof(s_ro.version), "%s", version);
   s_ro.allow_downgrade = allow_downgrade;
   memcpy(s_ro.targets, targets, sizeof(ro_target_t) * (size_t)n);
   s_ro.n_targets = n;
   s_ro.n_skipped = skipped;
   s_ro.deadline = time(NULL) + OTA_ROLLOUT_CANARY_TIMEOUT_SEC;
   pthread_mutex_unlock(&s_ro_mutex);

   OLOG_INFO("ota: rollout started — canary %s for %s (tier %d), %d target(s), %d skipped",
             targets[0].uuid, version, tier, n, skipped);
   if (summary_out && summary_len) {
      snprintf(summary_out, summary_len,
               "Rollout started: canary %s for %s, %d device(s) queued, %d skipped.",
               targets[0].uuid, version, n, skipped);
   }
   return SUCCESS;
#undef RO_FAIL
}

/* ── events (from ota_finalize_on_register) ──────────────────────────────────
 * These run in the registration handler's thread, which may hold session/db
 * locks — so they NEVER deliver offers here (push_fn takes those locks).  The
 * fan-out is flagged and delivered later from ota_rollout_tick (main loop). */

static int find_target_unlocked(const char *uuid) {
   for (int i = 0; i < s_ro.n_targets; i++) {
      if (strcmp(s_ro.targets[i].uuid, uuid) == 0) {
         return i;
      }
   }
   return -1;
}

/* All targets settled (committed or failed) → terminal DONE.  Only from an active
 * state — never overwrite RO_FAILED (canary halt / abort), whose semantics must
 * survive a settle check racing in behind it. */
static void maybe_finish_unlocked(void) {
   if (s_ro.state != RO_WAIT_CANARY && s_ro.state != RO_FANNING) {
      return;
   }
   for (int i = 0; i < s_ro.n_targets; i++) {
      if (!s_ro.targets[i].committed && !s_ro.targets[i].failed) {
         return;
      }
   }
   s_ro.state = RO_DONE;
}

void ota_rollout_on_commit(const char *uuid, const char *version) {
   if (!uuid || !version) {
      return;
   }
   pthread_mutex_lock(&s_ro_mutex);
   if ((s_ro.state != RO_WAIT_CANARY && s_ro.state != RO_FANNING) ||
       strcmp(version, s_ro.version) != 0) {
      pthread_mutex_unlock(&s_ro_mutex);
      return;
   }
   int idx = find_target_unlocked(uuid);
   if (idx < 0) {
      pthread_mutex_unlock(&s_ro_mutex);
      return;
   }
   if (!s_ro.targets[idx].committed) {
      s_ro.targets[idx].committed = true;
   }
   if (s_ro.state == RO_WAIT_CANARY && idx == 0) {
      /* Canary proved the image — release the fan-out (delivered by the tick) and
       * re-arm the deadline for the rest of the wave. */
      s_ro.state = RO_FANNING;
      s_ro.needs_fanout = (s_ro.n_targets > 1);
      s_ro.deadline = time(NULL) + OTA_ROLLOUT_CANARY_TIMEOUT_SEC;
      OLOG_INFO("ota: rollout canary %s committed %s — fanning out to %d device(s)", uuid, version,
                s_ro.n_targets - 1);
   }
   maybe_finish_unlocked();
   pthread_mutex_unlock(&s_ro_mutex);
}

void ota_rollout_on_fail(const char *uuid, const char *version) {
   if (!uuid) {
      return;
   }
   pthread_mutex_lock(&s_ro_mutex);
   if (s_ro.state != RO_WAIT_CANARY && s_ro.state != RO_FANNING) {
      pthread_mutex_unlock(&s_ro_mutex);
      return;
   }
   int idx = find_target_unlocked(uuid);
   if (idx < 0) {
      pthread_mutex_unlock(&s_ro_mutex);
      return;
   }
   if (s_ro.state == RO_WAIT_CANARY && idx == 0) {
      /* Canary failed → halt: the rest are never touched. */
      s_ro.targets[0].failed = true;
      s_ro.state = RO_FAILED;
      snprintf(s_ro.last_error, sizeof(s_ro.last_error),
               "canary %s did not take (reported %s) — rollout halted", uuid,
               version ? version : "?");
      OLOG_WARNING("ota: rollout halted — %s", s_ro.last_error);
      pthread_mutex_unlock(&s_ro_mutex);
      return;
   }
   if (!s_ro.targets[idx].failed) {
      s_ro.targets[idx].failed = true;
      OLOG_WARNING("ota: rollout target %s did not take (reported %s) — continuing", uuid,
                   version ? version : "?");
   }
   maybe_finish_unlocked();
   pthread_mutex_unlock(&s_ro_mutex);
}

/* ── tick (main loop): canary/wave timeout + deferred fan-out delivery ─────── */

void ota_rollout_tick(time_t now) {
   /* Phase A: under the lock, decide what to do and snapshot the fan-out list. */
   pthread_mutex_lock(&s_ro_mutex);
   ota_rollout_push_fn push_fn = s_push_fn;

   if (s_ro.state == RO_WAIT_CANARY && now > s_ro.deadline) {
      s_ro.targets[0].failed = true;
      s_ro.state = RO_FAILED;
      snprintf(s_ro.last_error, sizeof(s_ro.last_error),
               "canary %s did not commit within %d s — rollout halted", s_ro.targets[0].uuid,
               OTA_ROLLOUT_CANARY_TIMEOUT_SEC);
      OLOG_WARNING("ota: rollout halted — %s", s_ro.last_error);
      pthread_mutex_unlock(&s_ro_mutex);
      return;
   }

   if (s_ro.state == RO_FANNING && now > s_ro.deadline) {
      /* Wave deadline: mark anyone who never committed as failed and finish. */
      for (int i = 1; i < s_ro.n_targets; i++) {
         if (!s_ro.targets[i].committed && !s_ro.targets[i].failed) {
            s_ro.targets[i].failed = true;
         }
      }
      s_ro.state = RO_DONE;
      OLOG_INFO("ota: rollout wave timed out — finishing with remaining devices marked failed");
      pthread_mutex_unlock(&s_ro_mutex);
      return;
   }

   if (!(s_ro.state == RO_FANNING && s_ro.needs_fanout && push_fn)) {
      pthread_mutex_unlock(&s_ro_mutex);
      return;
   }

   /* Snapshot the fan-out targets, then deliver OUTSIDE the lock (push_fn takes
    * session/db locks we must not nest under s_ro_mutex). */
   s_ro.needs_fanout = false;
   char wave[OTA_ROLLOUT_MAX_TARGETS][SATELLITE_UUID_MAX];
   int wave_n = 0;
   for (int i = 1; i < s_ro.n_targets; i++) {
      if (s_ro.targets[i].committed || s_ro.targets[i].failed) {
         continue; /* already settled (e.g. committed during WAIT_CANARY) — don't re-offer */
      }
      snprintf(wave[wave_n++], SATELLITE_UUID_MAX, "%s", s_ro.targets[i].uuid);
   }
   char ver[OTA_VERSION_MAX];
   snprintf(ver, sizeof(ver), "%s", s_ro.version);
   bool allow_dg = s_ro.allow_downgrade;
   pthread_mutex_unlock(&s_ro_mutex);

   /* Phase B: deliver. A device that won't take the offer (offline / locked /
    * error) is marked failed immediately so the wave can still complete.  Re-check
    * the state each iteration so an operator abort mid-wave stops the remainder. */
   for (int i = 0; i < wave_n; i++) {
      pthread_mutex_lock(&s_ro_mutex);
      bool still_fanning = (s_ro.state == RO_FANNING);
      pthread_mutex_unlock(&s_ro_mutex);
      if (!still_fanning) {
         break; /* aborted / terminal — don't touch the rest of the fleet */
      }
      int rc = push_fn(wave[i], ver, allow_dg);
      if (rc != AUTH_DB_SUCCESS) {
         pthread_mutex_lock(&s_ro_mutex);
         int idx = find_target_unlocked(wave[i]);
         if (idx >= 0 && !s_ro.targets[idx].committed) {
            s_ro.targets[idx].failed = true;
         }
         pthread_mutex_unlock(&s_ro_mutex);
         OLOG_WARNING("ota: rollout could not offer %s (rc=%d)", wave[i], rc);
      }
   }

   pthread_mutex_lock(&s_ro_mutex);
   maybe_finish_unlocked();
   pthread_mutex_unlock(&s_ro_mutex);
}

/* ── status / abort ───────────────────────────────────────────────────────── */

static const char *ro_state_str(ro_state_t s) {
   switch (s) {
      case RO_WAIT_CANARY:
         return "waiting on canary";
      case RO_FANNING:
         return "rolling out";
      case RO_DONE:
         return "done";
      case RO_FAILED:
         return "failed";
      case RO_IDLE:
      default:
         return "idle";
   }
}

static const char *ro_platform_str(ota_platform_t p) {
   switch (p) {
      case OTA_PLATFORM_ESP32:
         return "esp32";
      case OTA_PLATFORM_RPI:
         return "rpi";
      default:
         return "?";
   }
}

int ota_rollout_status(char *buf, size_t len) {
   if (!buf || len == 0) {
      return FAILURE;
   }
   pthread_mutex_lock(&s_ro_mutex);
   if (s_ro.state == RO_IDLE && s_ro.n_targets == 0) {
      snprintf(buf, len, "No rollout has run.");
      pthread_mutex_unlock(&s_ro_mutex);
      return SUCCESS;
   }
   int committed = 0, failed = 0;
   for (int i = 0; i < s_ro.n_targets; i++) {
      committed += s_ro.targets[i].committed ? 1 : 0;
      failed += s_ro.targets[i].failed ? 1 : 0;
   }
   size_t off = 0;
   int w = snprintf(
       buf, len, "Rollout %s/%s — %s (tier %d): %d target(s), %d committed, %d failed, %d skipped.",
       ro_platform_str(s_ro.platform), s_ro.version, ro_state_str(s_ro.state), s_ro.tier,
       s_ro.n_targets, committed, failed, s_ro.n_skipped);
   off = (w > 0 && (size_t)w < len) ? (size_t)w : len - 1;
   if (s_ro.n_targets > 0 && off < len) {
      snprintf(buf + off, len - off, " Canary: %s.", s_ro.targets[0].uuid);
   }
   if (s_ro.last_error[0] && (off = strlen(buf)) < len) {
      snprintf(buf + off, len - off, " Last error: %s", s_ro.last_error);
   }
   pthread_mutex_unlock(&s_ro_mutex);
   return SUCCESS;
}

int ota_rollout_abort(void) {
   pthread_mutex_lock(&s_ro_mutex);
   bool was_active = (s_ro.state == RO_WAIT_CANARY || s_ro.state == RO_FANNING);
   if (was_active) {
      s_ro.state = RO_FAILED;
      s_ro.needs_fanout = false;
      snprintf(s_ro.last_error, sizeof(s_ro.last_error), "aborted by operator");
      OLOG_INFO("ota: rollout aborted by operator");
   }
   pthread_mutex_unlock(&s_ro_mutex);
   return was_active ? SUCCESS : FAILURE;
}
