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
 * Unit tests for the scheduled-origin thread-local context — the mechanism
 * that carries the owning user_id (and an is-scheduled flag) from the
 * scheduler's briefing executor into tool callbacks that have no session.
 */

#include "core/scheduled_context.h"
#include "unity.h"

void setUp(void) {
   scheduled_context_clear();
}
void tearDown(void) {
   scheduled_context_clear();
}

static void test_default_not_scheduled(void) {
   int uid = -1;
   TEST_ASSERT_FALSE(scheduled_context_get(&uid));
   TEST_ASSERT_EQUAL_INT(0, uid);
}

static void test_set_and_get(void) {
   scheduled_context_set(42);
   int uid = 0;
   TEST_ASSERT_TRUE(scheduled_context_get(&uid));
   TEST_ASSERT_EQUAL_INT(42, uid);
}

static void test_clear_resets(void) {
   scheduled_context_set(7);
   scheduled_context_clear();
   int uid = 99;
   TEST_ASSERT_FALSE(scheduled_context_get(&uid));
   TEST_ASSERT_EQUAL_INT(0, uid);
}

static void test_nonpositive_user_is_not_scheduled(void) {
   scheduled_context_set(0);
   TEST_ASSERT_FALSE(scheduled_context_get(NULL));
   scheduled_context_set(-5);
   TEST_ASSERT_FALSE(scheduled_context_get(NULL));
}

static void test_null_out_param_ok(void) {
   scheduled_context_set(3);
   TEST_ASSERT_TRUE(scheduled_context_get(NULL));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_default_not_scheduled);
   RUN_TEST(test_set_and_get);
   RUN_TEST(test_clear_resets);
   RUN_TEST(test_nonpositive_user_is_not_scheduled);
   RUN_TEST(test_null_out_param_ok);
   return UNITY_END();
}
