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
 * Unit tests for the generic rate limiter (src/core/rate_limiter.c).
 */

#include <string.h>

#include "core/rate_limiter.h"
#include "unity.h"

#define SLOT_COUNT 4
#define MAX_COUNT 3
#define WINDOW_SEC 60

static rate_limit_entry_t s_entries[SLOT_COUNT];
static rate_limiter_t s_limiter;

void setUp(void) {
   memset(s_entries, 0, sizeof(s_entries));
   rate_limiter_config_t config = {
      .max_count = MAX_COUNT,
      .window_sec = WINDOW_SEC,
      .slot_count = SLOT_COUNT,
   };
   rate_limiter_init(&s_limiter, s_entries, &config);
}

void tearDown(void) {
}

static void test_first_request_allowed(void) {
   TEST_ASSERT_FALSE(rate_limiter_check(&s_limiter, "192.168.1.1"));
}

static void test_under_limit_allowed(void) {
   TEST_ASSERT_FALSE(rate_limiter_check(&s_limiter, "192.168.1.1"));
   TEST_ASSERT_FALSE(rate_limiter_check(&s_limiter, "192.168.1.1"));
   TEST_ASSERT_FALSE(rate_limiter_check(&s_limiter, "192.168.1.1"));
}

static void test_rate_limited_at_max(void) {
   for (int i = 0; i < MAX_COUNT; i++) {
      TEST_ASSERT_FALSE(rate_limiter_check(&s_limiter, "10.0.0.1"));
   }
   TEST_ASSERT_TRUE(rate_limiter_check(&s_limiter, "10.0.0.1"));
   TEST_ASSERT_TRUE(rate_limiter_check(&s_limiter, "10.0.0.1"));
}

static void test_different_ips_independent(void) {
   for (int i = 0; i < MAX_COUNT; i++) {
      rate_limiter_check(&s_limiter, "10.0.0.1");
   }
   TEST_ASSERT_TRUE(rate_limiter_check(&s_limiter, "10.0.0.1"));
   TEST_ASSERT_FALSE(rate_limiter_check(&s_limiter, "10.0.0.2"));
}

static void test_reset_clears_entry(void) {
   for (int i = 0; i < MAX_COUNT; i++) {
      rate_limiter_check(&s_limiter, "10.0.0.1");
   }
   TEST_ASSERT_TRUE(rate_limiter_check(&s_limiter, "10.0.0.1"));

   rate_limiter_reset(&s_limiter, "10.0.0.1");
   TEST_ASSERT_FALSE(rate_limiter_check(&s_limiter, "10.0.0.1"));
}

static void test_reset_only_affects_target(void) {
   rate_limiter_check(&s_limiter, "10.0.0.1");
   rate_limiter_check(&s_limiter, "10.0.0.2");

   rate_limiter_reset(&s_limiter, "10.0.0.1");

   TEST_ASSERT_FALSE(rate_limiter_check(&s_limiter, "10.0.0.1"));
   TEST_ASSERT_FALSE(rate_limiter_check(&s_limiter, "10.0.0.2"));
}

static void test_clear_all(void) {
   rate_limiter_check(&s_limiter, "10.0.0.1");
   rate_limiter_check(&s_limiter, "10.0.0.2");
   rate_limiter_check(&s_limiter, "10.0.0.3");

   rate_limiter_clear_all(&s_limiter);

   TEST_ASSERT_FALSE(rate_limiter_check(&s_limiter, "10.0.0.1"));
   TEST_ASSERT_FALSE(rate_limiter_check(&s_limiter, "10.0.0.2"));
   TEST_ASSERT_FALSE(rate_limiter_check(&s_limiter, "10.0.0.3"));
}

static void test_lru_eviction(void) {
   rate_limiter_check(&s_limiter, "10.0.0.1");
   rate_limiter_check(&s_limiter, "10.0.0.2");
   rate_limiter_check(&s_limiter, "10.0.0.3");
   rate_limiter_check(&s_limiter, "10.0.0.4");

   /* Touch IP .2 so .1 remains the oldest */
   rate_limiter_check(&s_limiter, "10.0.0.2");

   /* 5th IP should evict the LRU entry (.1) */
   TEST_ASSERT_FALSE(rate_limiter_check(&s_limiter, "10.0.0.5"));

   /* .1 was evicted — next check creates a fresh entry with count=1 */
   TEST_ASSERT_FALSE(rate_limiter_check(&s_limiter, "10.0.0.1"));
}

static void test_null_limiter(void) {
   TEST_ASSERT_FALSE(rate_limiter_check(NULL, "10.0.0.1"));
}

static void test_null_ip(void) {
   TEST_ASSERT_FALSE(rate_limiter_check(&s_limiter, NULL));
}

static void test_normalize_ipv4_passthrough(void) {
   char out[RATE_LIMIT_IP_SIZE];
   rate_limiter_normalize_ip("192.168.1.1", out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("192.168.1.1", out);
}

static void test_normalize_ipv6_zeros_lower_64(void) {
   char out[RATE_LIMIT_IP_SIZE];
   rate_limiter_normalize_ip("2001:db8:85a3::8a2e:370:7334", out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("2001:db8:85a3::", out);
}

static void test_normalize_ipv6_same_prefix(void) {
   char a[RATE_LIMIT_IP_SIZE];
   char b[RATE_LIMIT_IP_SIZE];
   rate_limiter_normalize_ip("2001:db8:1::1", a, sizeof(a));
   rate_limiter_normalize_ip("2001:db8:1::ffff", b, sizeof(b));
   TEST_ASSERT_EQUAL_STRING(a, b);
}

static void test_normalize_null_ip(void) {
   char out[RATE_LIMIT_IP_SIZE] = "dirty";
   rate_limiter_normalize_ip(NULL, out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("", out);
}

static void test_normalize_non_ip_passthrough(void) {
   char out[RATE_LIMIT_IP_SIZE];
   rate_limiter_normalize_ip("not-an-ip", out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("not-an-ip", out);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_first_request_allowed);
   RUN_TEST(test_under_limit_allowed);
   RUN_TEST(test_rate_limited_at_max);
   RUN_TEST(test_different_ips_independent);
   RUN_TEST(test_reset_clears_entry);
   RUN_TEST(test_reset_only_affects_target);
   RUN_TEST(test_clear_all);
   RUN_TEST(test_lru_eviction);
   RUN_TEST(test_null_limiter);
   RUN_TEST(test_null_ip);
   RUN_TEST(test_normalize_ipv4_passthrough);
   RUN_TEST(test_normalize_ipv6_zeros_lower_64);
   RUN_TEST(test_normalize_ipv6_same_prefix);
   RUN_TEST(test_normalize_null_ip);
   RUN_TEST(test_normalize_non_ip_passthrough);
   return UNITY_END();
}
