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
 * Unit tests for tools/tavily_rate_limit — leaky-bucket per-user counter
 * that bounds Tavily quota burn on runaway tool loops (SEC-M3).
 */

#include "tools/tavily_rate_limit.h"
#include "unity.h"

void setUp(void) {
   /* Test isolation: zero all buckets between cases. */
   tavily_rate_limit_reset();
}

void tearDown(void) {
}

/* ── Initial budget ─────────────────────────────────────────────────────── */

static void test_initial_budget_full(void) {
   int m = -1, h = -1;
   tavily_rate_limit_remaining(1, &m, &h);
   TEST_ASSERT_EQUAL_INT(TAVILY_RL_PER_MINUTE, m);
   TEST_ASSERT_EQUAL_INT(TAVILY_RL_PER_HOUR, h);
}

static void test_unknown_user_full_budget(void) {
   /* Querying a fresh user reports the full budget without creating a charged
    * bucket. */
   int m = -1, h = -1;
   tavily_rate_limit_remaining(42, &m, &h);
   TEST_ASSERT_EQUAL_INT(TAVILY_RL_PER_MINUTE, m);
   TEST_ASSERT_EQUAL_INT(TAVILY_RL_PER_HOUR, h);
}

/* ── Charging within budget ─────────────────────────────────────────────── */

static void test_within_minute_budget(void) {
   for (int i = 0; i < TAVILY_RL_PER_MINUTE; i++) {
      TEST_ASSERT_TRUE(tavily_rate_limit_check(1));
   }
}

static void test_minute_cap_blocks_extra(void) {
   for (int i = 0; i < TAVILY_RL_PER_MINUTE; i++) {
      TEST_ASSERT_TRUE(tavily_rate_limit_check(1));
   }
   /* 11th call within the minute should be blocked. */
   TEST_ASSERT_FALSE(tavily_rate_limit_check(1));
}

static void test_denied_call_does_not_charge(void) {
   for (int i = 0; i < TAVILY_RL_PER_MINUTE; i++) {
      (void)tavily_rate_limit_check(1);
   }
   /* Multiple denied calls should keep counters pegged at cap, not above. */
   (void)tavily_rate_limit_check(1);
   (void)tavily_rate_limit_check(1);
   int m = -1, h = -1;
   tavily_rate_limit_remaining(1, &m, &h);
   TEST_ASSERT_EQUAL_INT(0, m);
   /* Hour budget should still match minute count (since they're in lockstep
    * on the way up). Above-cap rejections don't get counted. */
   TEST_ASSERT_EQUAL_INT(TAVILY_RL_PER_HOUR - TAVILY_RL_PER_MINUTE, h);
}

/* ── Per-user isolation ─────────────────────────────────────────────────── */

static void test_per_user_isolation(void) {
   for (int i = 0; i < TAVILY_RL_PER_MINUTE; i++) {
      TEST_ASSERT_TRUE(tavily_rate_limit_check(1));
   }
   /* User 1 exhausted; user 2 should still have full budget. */
   TEST_ASSERT_TRUE(tavily_rate_limit_check(2));
   int m = -1, h = -1;
   tavily_rate_limit_remaining(2, &m, &h);
   TEST_ASSERT_EQUAL_INT(TAVILY_RL_PER_MINUTE - 1, m);
}

static void test_anonymous_bucket(void) {
   /* Non-positive user ids share one anonymous bucket. */
   TEST_ASSERT_TRUE(tavily_rate_limit_check(0));
   TEST_ASSERT_TRUE(tavily_rate_limit_check(-1));
   TEST_ASSERT_TRUE(tavily_rate_limit_check(-42));

   int m = -1, h = -1;
   tavily_rate_limit_remaining(0, &m, &h);
   TEST_ASSERT_EQUAL_INT(TAVILY_RL_PER_MINUTE - 3, m);
   /* Query under a different non-positive id reports the same bucket state. */
   tavily_rate_limit_remaining(-1, &m, &h);
   TEST_ASSERT_EQUAL_INT(TAVILY_RL_PER_MINUTE - 3, m);
}

/* ── Reset ──────────────────────────────────────────────────────────────── */

static void test_reset_restores_budget(void) {
   for (int i = 0; i < TAVILY_RL_PER_MINUTE; i++) {
      (void)tavily_rate_limit_check(1);
   }
   TEST_ASSERT_FALSE(tavily_rate_limit_check(1));
   tavily_rate_limit_reset();
   TEST_ASSERT_TRUE(tavily_rate_limit_check(1));
}

/* ── Many users (bucket-table sizing) ───────────────────────────────────── */

static void test_many_users_eventually_overflow(void) {
   /* The implementation sizes the bucket table to TAVILY_RL_MAX_USERS=8.
    * Overflow path: when full, the call is allowed (fail-open) and logged.
    * We can't observe the log, but we can confirm calls keep returning true. */
   for (int uid = 1; uid <= 20; uid++) {
      TEST_ASSERT_TRUE(tavily_rate_limit_check(uid));
   }
}

/* ── Entrypoint ─────────────────────────────────────────────────────────── */

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_initial_budget_full);
   RUN_TEST(test_unknown_user_full_budget);
   RUN_TEST(test_within_minute_budget);
   RUN_TEST(test_minute_cap_blocks_extra);
   RUN_TEST(test_denied_call_does_not_charge);
   RUN_TEST(test_per_user_isolation);
   RUN_TEST(test_anonymous_bucket);
   RUN_TEST(test_reset_restores_budget);
   RUN_TEST(test_many_users_eventually_overflow);
   return UNITY_END();
}
