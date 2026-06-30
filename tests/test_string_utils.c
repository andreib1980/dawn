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
 * Unit tests for sanitize_utf8_for_json().
 *
 * This is the defensive guard that keeps invalid UTF-8 out of WebSocket text
 * frames (RFC 6455 §5.6 requires text frames to be valid UTF-8 — an invalid
 * frame fails the connection).  A real regression had a scheduler briefing
 * notification truncated mid-multibyte-character; replaying it on every
 * reconnect wedged the web client in an endless connect/close loop.  These
 * tests pin both the field-level behavior and the whole-string invariant
 * "output is always valid UTF-8".
 */

#include <string.h>

#include "unity.h"
#include "utils/string_utils.h"

void setUp(void) {
}
void tearDown(void) {
}

/* Standalone UTF-8 validator so we can assert the post-sanitize invariant
 * without depending on iconv/locale. */
static int is_valid_utf8(const char *s) {
   const unsigned char *p = (const unsigned char *)s;
   while (*p) {
      unsigned char c = *p;
      int len;
      if (c < 0x80)
         len = 1;
      else if ((c & 0xE0) == 0xC0)
         len = 2;
      else if ((c & 0xF0) == 0xE0)
         len = 3;
      else if ((c & 0xF8) == 0xF0)
         len = 4;
      else
         return 0;
      for (int k = 1; k < len; k++) {
         if ((p[k] & 0xC0) != 0x80)
            return 0;
      }
      p += len;
   }
   return 1;
}

/* ── Guards ─────────────────────────────────────────────────────────────── */

static void test_null_is_safe(void) {
   sanitize_utf8_for_json(NULL); /* must not crash */
   TEST_PASS();
}

static void test_empty_stays_empty(void) {
   char buf[1] = "";
   sanitize_utf8_for_json(buf);
   TEST_ASSERT_EQUAL_STRING("", buf);
}

/* ── Valid input is preserved ───────────────────────────────────────────── */

static void test_ascii_unchanged(void) {
   char buf[] = "Hello, world! 123 ~`";
   sanitize_utf8_for_json(buf);
   TEST_ASSERT_EQUAL_STRING("Hello, world! 123 ~`", buf);
}

static void test_allowed_whitespace_kept(void) {
   char buf[] = "line1\nline2\ttab\r\n";
   sanitize_utf8_for_json(buf);
   TEST_ASSERT_EQUAL_STRING("line1\nline2\ttab\r\n", buf);
}

static void test_valid_multibyte_kept(void) {
   /* é (C3 A9, 2-byte), … (E2 80 A6, 3-byte), 😀 (F0 9F 98 80, 4-byte) */
   char buf[] = "caf\xC3\xA9 \xE2\x80\xA6 \xF0\x9F\x98\x80 done";
   sanitize_utf8_for_json(buf);
   TEST_ASSERT_EQUAL_STRING("caf\xC3\xA9 \xE2\x80\xA6 \xF0\x9F\x98\x80 done", buf);
   TEST_ASSERT_TRUE(is_valid_utf8(buf));
}

/* ── Invalid input is scrubbed ──────────────────────────────────────────── */

static void test_control_chars_stripped(void) {
   char buf[] = "a\x01\x07\x1f"
                "b"; /* SOH, BEL, US removed; \n\r\t handled elsewhere */
   sanitize_utf8_for_json(buf);
   TEST_ASSERT_EQUAL_STRING("ab", buf);
}

/* The exact failure that wedged the web client: a 3-byte lead byte (0xE2)
 * left dangling by a mid-character truncation, followed by ASCII. */
static void test_real_poison_lone_lead_byte(void) {
   char buf[] = "g toward a scorcher\xE2...";
   TEST_ASSERT_FALSE(is_valid_utf8(buf)); /* precondition: input is invalid */
   sanitize_utf8_for_json(buf);
   TEST_ASSERT_EQUAL_STRING("g toward a scorcher?...", buf);
   TEST_ASSERT_TRUE(is_valid_utf8(buf));
}

static void test_truncated_two_byte(void) {
   char buf[] = "x\xC3"; /* lead of a 2-byte seq, no continuation */
   sanitize_utf8_for_json(buf);
   TEST_ASSERT_EQUAL_STRING("x?", buf);
   TEST_ASSERT_TRUE(is_valid_utf8(buf));
}

static void test_truncated_three_byte(void) {
   char buf[] = "x\xE2\x80"
                "y"; /* 2 of 3 bytes, then ASCII 'y' */
   sanitize_utf8_for_json(buf);
   TEST_ASSERT_TRUE(is_valid_utf8(buf));
   /* The bad lead is replaced; the stray continuation is also dropped/replaced. */
   TEST_ASSERT_EQUAL_INT(0, (int)(strchr(buf, '\xE2') != NULL));
}

static void test_surrogate_replaced(void) {
   /* ED A0 80 encodes U+D800, a UTF-16 surrogate — illegal in UTF-8. */
   char buf[] = "x\xED\xA0\x80"
                "y";
   sanitize_utf8_for_json(buf);
   TEST_ASSERT_EQUAL_STRING("x?y", buf);
   TEST_ASSERT_TRUE(is_valid_utf8(buf));
}

int main(void) {
   UNITY_BEGIN();

   RUN_TEST(test_null_is_safe);
   RUN_TEST(test_empty_stays_empty);

   RUN_TEST(test_ascii_unchanged);
   RUN_TEST(test_allowed_whitespace_kept);
   RUN_TEST(test_valid_multibyte_kept);

   RUN_TEST(test_control_chars_stripped);
   RUN_TEST(test_real_poison_lone_lead_byte);
   RUN_TEST(test_truncated_two_byte);
   RUN_TEST(test_truncated_three_byte);
   RUN_TEST(test_surrogate_replaced);

   return UNITY_END();
}
