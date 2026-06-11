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
 * Unit tests for the OTA fleet rollout orchestrator (src/core/ota_rollout.c):
 * the canary-then-fan-out state machine, plus the slot-claim invariants from
 * the rollout-start TOCTOU fix — a failed start must RELEASE the single-rollout
 * slot (never wedge it), and a start while one is active must be rejected.
 *
 * The orchestrator's externals are stubbed here (no DB / no sessions): a
 * test-controlled fleet feeds satellite_db_list, session_find_by_uuid drives the
 * online check, and a counting push_fn stands in for the transport.
 */

#include <stdbool.h>
#include <string.h>

#include "auth/auth_db.h" /* satellite_mapping_t, AUTH_DB_* */
#include "core/ota_db.h"  /* ota_device_state_t */
#include "core/ota_manifest.h"
#include "core/ota_rollout.h"
#include "unity.h"

/* ── stubbed externals ───────────────────────────────────────────────────────
 * session_t is a named struct (struct session) in session_manager.h; forward-
 * declare it opaquely so we needn't pull that heavy header — ota_rollout.c only
 * ever holds the pointer (online check + release), never dereferences it. */
typedef struct session session_t;

#define MAX_FLEET 8
static satellite_mapping_t s_fleet[MAX_FLEET];
static bool s_online[MAX_FLEET];
static int s_fleet_n;

static int s_push_count;
static int s_push_rc;                                  /* what the stub push returns */
static char s_push_uuids[OTA_ROLLOUT_MAX_TARGETS][64]; /* uuids the transport saw */

/* satellite_db_list — replays the test fleet through the caller's callback. */
int satellite_db_list(int (*callback)(const satellite_mapping_t *, void *), void *ctx) {
   for (int i = 0; i < s_fleet_n; i++) {
      callback(&s_fleet[i], ctx);
   }
   return AUTH_DB_SUCCESS;
}

/* ota_db_get — report "no recorded version" so an online device is never treated
 * as already-on-target (the eligibility filter we don't exercise here). */
int ota_db_get(const char *uuid, ota_device_state_t *out) {
   (void)uuid;
   if (out) {
      memset(out, 0, sizeof(*out));
   }
   return AUTH_DB_NOT_FOUND;
}

/* session_find_by_uuid — online iff the fleet entry is flagged online.  Returns
 * a non-NULL sentinel; the orchestrator only checks non-NULL then releases. */
session_t *session_find_by_uuid(const char *uuid) {
   static int sentinel;
   for (int i = 0; i < s_fleet_n; i++) {
      if (strcmp(s_fleet[i].uuid, uuid) == 0 && s_online[i]) {
         return (session_t *)&sentinel;
      }
   }
   return NULL;
}

void session_release(session_t *session) {
   (void)session;
}

static int stub_push(const char *uuid, const char *version, bool allow_downgrade) {
   (void)version;
   (void)allow_downgrade;
   if (s_push_count < OTA_ROLLOUT_MAX_TARGETS) {
      snprintf(s_push_uuids[s_push_count], sizeof(s_push_uuids[0]), "%s", uuid);
   }
   s_push_count++;
   return s_push_rc;
}

/* ── fixtures ────────────────────────────────────────────────────────────────*/

static void add_device(const char *uuid, bool online) {
   TEST_ASSERT_TRUE(s_fleet_n < MAX_FLEET);
   memset(&s_fleet[s_fleet_n], 0, sizeof(s_fleet[s_fleet_n]));
   snprintf(s_fleet[s_fleet_n].uuid, sizeof(s_fleet[s_fleet_n].uuid), "%s", uuid);
   s_fleet[s_fleet_n].tier = 1;
   s_fleet[s_fleet_n].enabled = true;
   s_online[s_fleet_n] = online;
   s_fleet_n++;
}

#define CANARY "11111111-1111-4111-8111-111111111111"
#define DEV2 "22222222-2222-4222-8222-222222222222"

static int start(char *summary, size_t len) {
   return ota_rollout_start(OTA_PLATFORM_RPI, 1, "2.2.0", false, summary, len);
}

void setUp(void) {
   /* Reset the orchestrator's module-static state between tests.  abort() only
    * frees the slot when a rollout is ACTIVE (a prior test ending in RO_DONE makes
    * it a no-op); the real isolation guarantee is ota_rollout_start()'s arm-time
    * memset of s_ro, so any non-active leftover is wiped on the next start.  abort()
    * here just ensures a still-active rollout doesn't fail the next start as busy. */
   ota_rollout_abort();
   s_fleet_n = 0;
   memset(s_online, 0, sizeof(s_online));
   s_push_count = 0;
   s_push_rc = AUTH_DB_SUCCESS;
   memset(s_push_uuids, 0, sizeof(s_push_uuids));
   ota_rollout_set_push_fn(stub_push);
}

void tearDown(void) {
   ota_rollout_abort();
}

/* No transport registered → start fails (nothing could deliver the offer). */
static void test_start_no_transport(void) {
   ota_rollout_set_push_fn(NULL);
   add_device(CANARY, true);
   char s[256];
   TEST_ASSERT_EQUAL_INT(FAILURE, start(s, sizeof(s)));
   TEST_ASSERT_NOT_NULL(strstr(s, "transport"));
}

/* C1 regression: a start that fails for "no eligible devices" must RELEASE the
 * claimed slot — a second start sees the same eligibility failure, NOT a stuck
 * "already in progress" (which is what a wedged RO_STARTING slot would produce). */
static void test_no_eligible_releases_slot(void) {
   add_device(CANARY, false); /* offline → ineligible */
   add_device(DEV2, false);
   char s1[256], s2[256];
   TEST_ASSERT_EQUAL_INT(FAILURE, start(s1, sizeof(s1)));
   TEST_ASSERT_NOT_NULL(strstr(s1, "No eligible"));
   TEST_ASSERT_EQUAL_INT(FAILURE, start(s2, sizeof(s2)));
   TEST_ASSERT_NOT_NULL(strstr(s2, "No eligible"));     /* slot was released */
   TEST_ASSERT_NULL(strstr(s2, "already in progress")); /* not wedged */
}

/* C1 regression: a canary-push failure must also release the slot — a retry
 * after the transport recovers succeeds rather than reporting "in progress". */
static void test_push_fail_releases_slot(void) {
   add_device(CANARY, true);
   char s1[256], s2[256];
   s_push_rc = AUTH_DB_LOCKED; /* canary push fails */
   TEST_ASSERT_EQUAL_INT(FAILURE, start(s1, sizeof(s1)));
   TEST_ASSERT_NULL(strstr(s1, "already in progress"));
   TEST_ASSERT_EQUAL_INT(1, s_push_count);                /* the failed canary push was attempted */
   s_push_rc = AUTH_DB_SUCCESS;                           /* transport recovers */
   s_push_count = 0;                                      /* count only the retry */
   TEST_ASSERT_EQUAL_INT(SUCCESS, start(s2, sizeof(s2))); /* slot was released */
   TEST_ASSERT_EQUAL_INT(1, s_push_count);                /* retry pushed its canary once */
}

/* A successful start pushes exactly the canary and arms the wait state. */
static void test_successful_start_arms_canary(void) {
   add_device(CANARY, true);
   add_device(DEV2, true);
   char s[256];
   TEST_ASSERT_EQUAL_INT(SUCCESS, start(s, sizeof(s)));
   TEST_ASSERT_EQUAL_INT(1, s_push_count); /* canary only — DEV2 waits for commit */
   TEST_ASSERT_EQUAL_STRING(CANARY, s_push_uuids[0]);
   char st[256];
   TEST_ASSERT_EQUAL_INT(SUCCESS, ota_rollout_status(st, sizeof(st)));
   TEST_ASSERT_NOT_NULL(strstr(st, "waiting on canary"));
}

/* A second start while one is active is rejected (the single-rollout slot). */
static void test_second_start_rejected(void) {
   add_device(CANARY, true);
   add_device(DEV2, true);
   char s[256];
   TEST_ASSERT_EQUAL_INT(SUCCESS, start(s, sizeof(s)));
   char s2[256];
   TEST_ASSERT_EQUAL_INT(FAILURE, start(s2, sizeof(s2)));
   TEST_ASSERT_NOT_NULL(strstr(s2, "already in progress"));
}

/* Full happy path: canary commits → tick fans out to the rest → all commit → DONE. */
static void test_canary_commit_fans_out(void) {
   add_device(CANARY, true);
   add_device(DEV2, true);
   char s[256];
   TEST_ASSERT_EQUAL_INT(SUCCESS, start(s, sizeof(s)));

   ota_rollout_on_commit(CANARY, "2.2.0"); /* canary proved the image */
   ota_rollout_tick(time(NULL));           /* delivers the fan-out wave */
   TEST_ASSERT_EQUAL_INT(2, s_push_count);
   TEST_ASSERT_EQUAL_STRING(DEV2, s_push_uuids[1]);

   ota_rollout_on_commit(DEV2, "2.2.0"); /* last target settles */
   char st[256];
   ota_rollout_status(st, sizeof(st));
   TEST_ASSERT_NOT_NULL(strstr(st, "done"));
}

/* A canary that reverts halts the rollout — the rest are never offered. */
static void test_canary_revert_halts(void) {
   add_device(CANARY, true);
   add_device(DEV2, true);
   char s[256];
   TEST_ASSERT_EQUAL_INT(SUCCESS, start(s, sizeof(s)));

   ota_rollout_on_fail(CANARY, "2.1.0"); /* canary came back on the old version */
   ota_rollout_tick(time(NULL));         /* must NOT fan out */
   TEST_ASSERT_EQUAL_INT(1, s_push_count);
   char st[256];
   ota_rollout_status(st, sizeof(st));
   TEST_ASSERT_NOT_NULL(strstr(st, "failed"));
}

/* Abort releases the slot so a fresh rollout can start. */
static void test_abort_releases(void) {
   add_device(CANARY, true);
   char s[256];
   TEST_ASSERT_EQUAL_INT(SUCCESS, start(s, sizeof(s)));
   TEST_ASSERT_EQUAL_INT(SUCCESS, ota_rollout_abort());
   TEST_ASSERT_EQUAL_INT(FAILURE, ota_rollout_abort()); /* nothing active now */
   TEST_ASSERT_EQUAL_INT(SUCCESS, start(s, sizeof(s))); /* slot free again */
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_start_no_transport);
   RUN_TEST(test_no_eligible_releases_slot);
   RUN_TEST(test_push_fail_releases_slot);
   RUN_TEST(test_successful_start_arms_canary);
   RUN_TEST(test_second_start_rejected);
   RUN_TEST(test_canary_commit_fans_out);
   RUN_TEST(test_canary_revert_halts);
   RUN_TEST(test_abort_releases);
   return UNITY_END();
}
