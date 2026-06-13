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
 * Unit tests for cross-device utterance dedup (src/core/utterance_dedup.c).
 */

#include <stdint.h>

#include "core/utterance_dedup.h"
#include "unity.h"

#define WINDOW_SEC 4

/* Injectable monotonic-millisecond clock. */
static uint64_t s_mock_ms;
static uint64_t mock_clock(void) {
   return s_mock_ms;
}

void setUp(void) {
   s_mock_ms = 1000; /* nonzero start so "T-1" math never underflows */
   utterance_dedup_set_clock(mock_clock);
   utterance_dedup_init(WINDOW_SEC);
   utterance_dedup_reset_all();
}

void tearDown(void) {
   utterance_dedup_set_clock(NULL);
}

/* First source in a window proceeds; a different source within the window is
 * suppressed. */
static void test_cross_session_suppressed(void) {
   TEST_ASSERT_FALSE(utterance_dedup_check(0)); /* local mic wins */
   TEST_ASSERT_TRUE(utterance_dedup_check(42)); /* satellite suppressed */
}

/* A deliberate repeat on the SAME device ("next, next") is always allowed. */
static void test_same_session_repeat_allowed(void) {
   TEST_ASSERT_FALSE(utterance_dedup_check(7));
   TEST_ASSERT_FALSE(utterance_dedup_check(7));
   TEST_ASSERT_FALSE(utterance_dedup_check(7));
}

/* Three devices hear one command within the window: only the first proceeds. */
static void test_three_devices_first_wins(void) {
   TEST_ASSERT_FALSE(utterance_dedup_check(1));
   TEST_ASSERT_TRUE(utterance_dedup_check(2));
   TEST_ASSERT_TRUE(utterance_dedup_check(3));
}

/* Two different sessions firing OUTSIDE the window are both allowed (the first
 * has expired and is purged before the second checks). */
static void test_window_expiry_allows_both(void) {
   TEST_ASSERT_FALSE(utterance_dedup_check(1));
   s_mock_ms += (uint64_t)WINDOW_SEC * 1000u; /* exactly at the window edge => expired */
   TEST_ASSERT_FALSE(utterance_dedup_check(2));
}

/* Window of 0 disables the gate entirely. */
static void test_disabled_when_zero(void) {
   utterance_dedup_set_window(0);
   TEST_ASSERT_FALSE(utterance_dedup_check(1));
   TEST_ASSERT_FALSE(utterance_dedup_check(2)); /* would suppress if enabled */
   TEST_ASSERT_FALSE(utterance_dedup_check(3));
}

/* The walkthrough from the design doc: suppressed events do NOT record, so the
 * window anchors to the winner and the winner's own deliberate repeat still
 * goes through even after losers piled up. */
static void test_winner_anchor_deliberate_repeat(void) {
   TEST_ASSERT_FALSE(utterance_dedup_check(1)); /* A wins, records A@T */
   s_mock_ms += 1000;
   TEST_ASSERT_TRUE(utterance_dedup_check(2)); /* B suppressed, not recorded */
   s_mock_ms += 1000;
   TEST_ASSERT_TRUE(utterance_dedup_check(3)); /* C suppressed, not recorded */
   /* A repeats while still inside its own window: only its own slot is live. */
   TEST_ASSERT_FALSE(utterance_dedup_check(1));
}

/* Window boundary is half-open: an event at +(window-1)ms is suppressed, an
 * event at exactly +window*1000ms sees the winner as expired and is allowed. */
static void test_window_boundary(void) {
   const uint64_t base = s_mock_ms;

   utterance_dedup_check(1);                             /* winner at base */
   s_mock_ms = base + (uint64_t)WINDOW_SEC * 1000u - 1u; /* 3999ms */
   TEST_ASSERT_TRUE(utterance_dedup_check(2));           /* still inside window */

   utterance_dedup_reset_all();
   s_mock_ms = base; /* re-anchor the second winner at base, not at +3999 */
   utterance_dedup_check(1);
   s_mock_ms = base + (uint64_t)WINDOW_SEC * 1000u; /* 4000ms => expired */
   TEST_ASSERT_FALSE(utterance_dedup_check(2));
}

/* A chatty same-session burst (more events than there are slots) must not cause
 * the session to evict its own anchor: a late different source is still
 * suppressed, proving the winner kept its slot via update-in-place. */
static void test_chatty_same_session_keeps_anchor(void) {
   TEST_ASSERT_FALSE(utterance_dedup_check(1)); /* anchor */
   for (int i = 0; i < 50; i++) {
      s_mock_ms += 10; /* well inside the window */
      TEST_ASSERT_FALSE(utterance_dedup_check(1));
   }
   /* Different source, still inside the window relative to the last repeat. */
   TEST_ASSERT_TRUE(utterance_dedup_check(2));
}

/* Many distinct sources hammering the gate within a window must not crash and
 * must all be suppressed after the first (saturation / scan robustness). */
static void test_saturation_no_crash(void) {
   TEST_ASSERT_FALSE(utterance_dedup_check(1000)); /* winner */
   for (uint32_t s = 1001; s < 1100; s++) {
      TEST_ASSERT_TRUE(utterance_dedup_check(s));
   }
}

/* Live retune: disabling mid-stream stops suppression; re-enabling resumes it. */
static void test_set_window_live_retune(void) {
   TEST_ASSERT_FALSE(utterance_dedup_check(1));
   TEST_ASSERT_TRUE(utterance_dedup_check(2)); /* enabled => suppress */

   utterance_dedup_set_window(0);
   TEST_ASSERT_FALSE(utterance_dedup_check(3)); /* disabled => allow */

   utterance_dedup_set_window(WINDOW_SEC);
   utterance_dedup_reset_all();
   TEST_ASSERT_FALSE(utterance_dedup_check(4));
   TEST_ASSERT_TRUE(utterance_dedup_check(5)); /* suppression back */
}

/* reset_all clears recorded events so a prior winner no longer suppresses. */
static void test_reset_all_clears(void) {
   TEST_ASSERT_FALSE(utterance_dedup_check(1));
   TEST_ASSERT_TRUE(utterance_dedup_check(2));
   utterance_dedup_reset_all();
   TEST_ASSERT_FALSE(utterance_dedup_check(2)); /* fresh winner after reset */
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_cross_session_suppressed);
   RUN_TEST(test_same_session_repeat_allowed);
   RUN_TEST(test_three_devices_first_wins);
   RUN_TEST(test_window_expiry_allows_both);
   RUN_TEST(test_disabled_when_zero);
   RUN_TEST(test_winner_anchor_deliberate_repeat);
   RUN_TEST(test_window_boundary);
   RUN_TEST(test_chatty_same_session_keeps_anchor);
   RUN_TEST(test_saturation_no_crash);
   RUN_TEST(test_set_window_live_retune);
   RUN_TEST(test_reset_all_clears);
   return UNITY_END();
}
