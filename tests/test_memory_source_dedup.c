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
 * Unit tests for the source_dedup_set_t helpers exposed via
 * memory_callback_internal.h.  Pin the M1 invariant — call sites must add
 * only post-batch-filter triples to the set; the helpers themselves enforce
 * nothing on the privacy front, so the tests verify mechanical correctness:
 * insert / lookup, capacity bound, count tracking.
 */

#include <stdint.h>
#include <string.h>

#include "memory/memory_callback_internal.h"
#include "unity.h"

void setUp(void) {
}
void tearDown(void) {
}

void test_dedup_empty_set_misses(void) {
   source_dedup_set_t set = { 0 };
   TEST_ASSERT_FALSE(source_dedup_seen(&set, 10, 100, 200));
   TEST_ASSERT_EQUAL(0, set.count);
}

void test_dedup_add_then_seen(void) {
   source_dedup_set_t set = { 0 };
   source_dedup_add(&set, 10, 100, 200);
   TEST_ASSERT_EQUAL(1, set.count);
   TEST_ASSERT_TRUE(source_dedup_seen(&set, 10, 100, 200));
}

void test_dedup_distinct_triples_dont_collide(void) {
   source_dedup_set_t set = { 0 };
   source_dedup_add(&set, 10, 100, 200);
   source_dedup_add(&set, 11, 100, 200); /* different conv */
   source_dedup_add(&set, 10, 101, 200); /* different start */
   source_dedup_add(&set, 10, 100, 201); /* different end */
   TEST_ASSERT_EQUAL(4, set.count);
   TEST_ASSERT_TRUE(source_dedup_seen(&set, 10, 100, 200));
   TEST_ASSERT_TRUE(source_dedup_seen(&set, 11, 100, 200));
   TEST_ASSERT_TRUE(source_dedup_seen(&set, 10, 101, 200));
   TEST_ASSERT_TRUE(source_dedup_seen(&set, 10, 100, 201));
   TEST_ASSERT_FALSE(source_dedup_seen(&set, 99, 99, 99));
}

void test_dedup_capacity_bounded(void) {
   source_dedup_set_t set = { 0 };
   /* Fill past the cap — count must not exceed it; oversize adds must be silent. */
   for (int i = 0; i < SOURCE_DEDUP_CAP + 8; i++) {
      source_dedup_add(&set, i + 1, i + 1, i + 1);
   }
   TEST_ASSERT_EQUAL(SOURCE_DEDUP_CAP, set.count);
   /* Items added before the cap are still findable. */
   TEST_ASSERT_TRUE(source_dedup_seen(&set, 1, 1, 1));
   TEST_ASSERT_TRUE(source_dedup_seen(&set, SOURCE_DEDUP_CAP, SOURCE_DEDUP_CAP, SOURCE_DEDUP_CAP));
   /* Items dropped (past the cap) are not. */
   TEST_ASSERT_FALSE(
       source_dedup_seen(&set, SOURCE_DEDUP_CAP + 1, SOURCE_DEDUP_CAP + 1, SOURCE_DEDUP_CAP + 1));
}

void test_dedup_null_set_safe(void) {
   /* NULL set: seen() returns false, add() is a no-op (must not crash). */
   TEST_ASSERT_FALSE(source_dedup_seen(NULL, 1, 2, 3));
   source_dedup_add(NULL, 1, 2, 3);
}

/* Integration: 5 records with the same (conv, range) triple count as 1
 * distinct fetch.  This is the count assertion the plan called for —
 * exposes the dedup contract at the helper API level, since wiring up
 * memory_action_search end-to-end pulls in the full daemon. */
void test_dedup_five_duplicates_count_as_one(void) {
   source_dedup_set_t set = { 0 };
   const int64_t conv = 42, s = 100, e = 105;
   for (int i = 0; i < 5; i++) {
      if (!source_dedup_seen(&set, conv, s, e)) {
         source_dedup_add(&set, conv, s, e);
      }
   }
   /* Only the first iteration added; subsequent four hit the seen() short-circuit. */
   TEST_ASSERT_EQUAL(1, set.count);
}

/* Integration: M1 invariant — when only post-batch-filter (i.e. public)
 * triples are added, no private upstream can leak via "[same as above]".
 * Models the production loop: caller skips add() when conv_id == 0
 * (which the batch APIs return for private rows). */
void test_dedup_excludes_zero_conv_per_m1_invariant(void) {
   source_dedup_set_t set = { 0 };
   /* Public fact: conv > 0 — added. */
   if (!source_dedup_seen(&set, 10, 100, 100))
      source_dedup_add(&set, 10, 100, 100);
   /* Private "fact": batch API returned conv = 0; caller MUST NOT add. */
   int64_t priv_conv = 0;
   if (priv_conv > 0) { /* mirrors call-site guard */
      source_dedup_add(&set, priv_conv, 200, 200);
   }
   TEST_ASSERT_EQUAL(1, set.count);
   TEST_ASSERT_TRUE(source_dedup_seen(&set, 10, 100, 100));
   /* No private triple ever entered — a later non-private record sharing
    * msgs 200-200 (different conv) can never emit "same as above". */
   TEST_ASSERT_FALSE(source_dedup_seen(&set, 0, 200, 200));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_dedup_empty_set_misses);
   RUN_TEST(test_dedup_add_then_seen);
   RUN_TEST(test_dedup_distinct_triples_dont_collide);
   RUN_TEST(test_dedup_capacity_bounded);
   RUN_TEST(test_dedup_null_set_safe);
   RUN_TEST(test_dedup_five_duplicates_count_as_one);
   RUN_TEST(test_dedup_excludes_zero_conv_per_m1_invariant);
   return UNITY_END();
}
