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
 * Unit tests for common/src/tts/tts_preprocessing.cpp via the C wrapper.
 */

#include <string.h>

#include "tts/tts_preprocessing.h"
#include "unity.h"

void setUp(void) {
}
void tearDown(void) {
}

/* ── remove_chars ────────────────────────────────────────────────────────── */

static void test_remove_chars_basic(void) {
   char buf[64] = "**hello** world";
   remove_chars(buf, "*");
   TEST_ASSERT_EQUAL_STRING("hello world", buf);
}

static void test_remove_chars_multiple_chars(void) {
   char buf[64] = "(hello) [world]";
   remove_chars(buf, "()[]");
   TEST_ASSERT_EQUAL_STRING("hello world", buf);
}

static void test_remove_chars_no_match(void) {
   char buf[64] = "hello world";
   remove_chars(buf, "xyz");
   TEST_ASSERT_EQUAL_STRING("hello world", buf);
}

static void test_remove_chars_empty_input(void) {
   char buf[8] = "";
   remove_chars(buf, "*");
   TEST_ASSERT_EQUAL_STRING("", buf);
}

static void test_remove_chars_null_safe(void) {
   /* Should not crash */
   remove_chars(NULL, "x");
   char buf[8] = "abc";
   remove_chars(buf, NULL);
   TEST_ASSERT_EQUAL_STRING("abc", buf);
}

/* ── is_emoji ────────────────────────────────────────────────────────────── */

static void test_is_emoji_ascii(void) {
   TEST_ASSERT_FALSE(is_emoji('A'));
   TEST_ASSERT_FALSE(is_emoji(' '));
   TEST_ASSERT_FALSE(is_emoji('1'));
}

static void test_is_emoji_emoji_block(void) {
   /* U+1F600 grinning face */
   TEST_ASSERT_TRUE(is_emoji(0x1F600));
   /* U+1F4A9 pile of poo */
   TEST_ASSERT_TRUE(is_emoji(0x1F4A9));
}

static void test_is_emoji_misc_symbols(void) {
   /* U+2600 (☀) sun */
   TEST_ASSERT_TRUE(is_emoji(0x2600));
   /* U+2764 (❤) heart */
   TEST_ASSERT_TRUE(is_emoji(0x2764));
}

static void test_is_emoji_zwj_and_variation_selector(void) {
   /* U+200D Zero Width Joiner */
   TEST_ASSERT_TRUE(is_emoji(0x200D));
   /* U+FE0F Variation Selector-16 */
   TEST_ASSERT_TRUE(is_emoji(0xFE0F));
}

static void test_is_emoji_normal_unicode(void) {
   /* U+00E9 é (Latin small e with acute) */
   TEST_ASSERT_FALSE(is_emoji(0x00E9));
   /* U+4E2D 中 (Chinese) */
   TEST_ASSERT_FALSE(is_emoji(0x4E2D));
}

/* ── remove_emojis ───────────────────────────────────────────────────────── */

static void test_remove_emojis_simple(void) {
   /* "Hello 👋 world" — U+1F44B (4 bytes UTF-8: F0 9F 91 8B) */
   char buf[64] = "Hello \xF0\x9F\x91\x8B world";
   remove_emojis(buf);
   TEST_ASSERT_EQUAL_STRING("Hello  world", buf);
}

static void test_remove_emojis_preserves_normal_unicode(void) {
   /* "café" — é is U+00E9 (2 bytes: C3 A9) */
   char buf[64] = "caf\xC3\xA9";
   remove_emojis(buf);
   TEST_ASSERT_EQUAL_STRING("caf\xC3\xA9", buf);
}

static void test_remove_emojis_only_ascii(void) {
   char buf[64] = "no emoji here";
   remove_emojis(buf);
   TEST_ASSERT_EQUAL_STRING("no emoji here", buf);
}

static void test_remove_emojis_null_safe(void) {
   remove_emojis(NULL);
   /* Just ensures no crash */
   TEST_PASS();
}

/* ── preprocess_text_for_tts_c: NULL/error handling ──────────────────────── */

static void test_preprocess_null_input(void) {
   char out[64];
   int written = -1;
   TEST_ASSERT_EQUAL_INT(1, preprocess_text_for_tts_c(NULL, out, sizeof(out), &written));
   TEST_ASSERT_EQUAL_INT(0, written);
}

static void test_preprocess_null_output(void) {
   int written = -1;
   TEST_ASSERT_EQUAL_INT(1, preprocess_text_for_tts_c("hello", NULL, 64, &written));
   TEST_ASSERT_EQUAL_INT(0, written);
}

static void test_preprocess_zero_buffer(void) {
   char out[64];
   int written = -1;
   TEST_ASSERT_EQUAL_INT(1, preprocess_text_for_tts_c("hello", out, 0, &written));
}

static void test_preprocess_null_bytes_written_ok(void) {
   /* bytes_written may be NULL — should still succeed */
   char out[64];
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("hello", out, sizeof(out), NULL));
   TEST_ASSERT_EQUAL_STRING("hello", out);
}

/* ── preprocess_text_for_tts_c: passthrough cases ────────────────────────── */

static void test_preprocess_plain_text(void) {
   char out[64];
   int written = 0;
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("Hello world", out, sizeof(out), &written));
   TEST_ASSERT_EQUAL_STRING("Hello world", out);
   TEST_ASSERT_EQUAL_INT(11, written);
}

static void test_preprocess_empty_string(void) {
   char out[64] = "preset";
   int written = -1;
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("", out, sizeof(out), &written));
   TEST_ASSERT_EQUAL_STRING("", out);
   TEST_ASSERT_EQUAL_INT(0, written);
}

/* ── preprocess_text_for_tts_c: transformations ──────────────────────────── */

static void test_preprocess_strips_asterisks(void) {
   char out[64];
   int written = 0;
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("**bold** text", out, sizeof(out), &written));
   TEST_ASSERT_EQUAL_STRING("bold text", out);
}

static void test_preprocess_strips_emoji(void) {
   char out[64];
   int written = 0;
   /* "ok 👍" — U+1F44D F0 9F 91 8D */
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("ok \xF0\x9F\x91\x8D", out, sizeof(out),
                                                      &written));
   TEST_ASSERT_EQUAL_STRING("ok ", out);
}

static void test_preprocess_em_dash_to_comma(void) {
   char out[64];
   int written = 0;
   /* em-dash U+2014 = E2 80 94 */
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("yes\xE2\x80\x94no", out, sizeof(out),
                                                      &written));
   TEST_ASSERT_EQUAL_STRING("yes,no", out);
}

static void test_preprocess_temperature_fahrenheit(void) {
   char out[128];
   int written = 0;
   /* "72°F" — degree sign U+00B0 = C2 B0 */
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("72\xC2\xB0"
                                                      "F outside",
                                                      out, sizeof(out), &written));
   /* Output: "72 degrees Fahrenheit outside" */
   TEST_ASSERT_NOT_NULL(strstr(out, "degrees Fahrenheit"));
}

static void test_preprocess_temperature_celsius(void) {
   char out[128];
   int written = 0;
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("22\xC2\xB0"
                                                      "C",
                                                      out, sizeof(out), &written));
   TEST_ASSERT_NOT_NULL(strstr(out, "degrees Celsius"));
}

static void test_preprocess_currency_dollar_singular(void) {
   char out[128];
   int written = 0;
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("$1", out, sizeof(out), &written));
   TEST_ASSERT_NOT_NULL(strstr(out, "1 dollar"));
   /* Should NOT contain "dollars" (plural) */
   TEST_ASSERT_NULL(strstr(out, "dollars"));
}

static void test_preprocess_currency_dollar_plural(void) {
   char out[128];
   int written = 0;
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("$5", out, sizeof(out), &written));
   TEST_ASSERT_NOT_NULL(strstr(out, "5 dollars"));
}

static void test_preprocess_currency_magnitude_million(void) {
   char out[128];
   int written = 0;
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("$1M", out, sizeof(out), &written));
   /* "1 million dollars" */
   TEST_ASSERT_NOT_NULL(strstr(out, "million"));
   TEST_ASSERT_NOT_NULL(strstr(out, "dollars"));
}

static void test_preprocess_state_abbrev(void) {
   char out[128];
   int written = 0;
   /* "CA" should expand to "California" with proper word boundaries */
   TEST_ASSERT_EQUAL_INT(0,
                         preprocess_text_for_tts_c("Visit CA today", out, sizeof(out), &written));
   TEST_ASSERT_NOT_NULL(strstr(out, "California"));
}

static void test_preprocess_day_abbrev(void) {
   char out[128];
   int written = 0;
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("Mon meeting", out, sizeof(out), &written));
   TEST_ASSERT_NOT_NULL(strstr(out, "Monday"));
}

static void test_preprocess_month_abbrev(void) {
   char out[128];
   int written = 0;
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("Jan 1st", out, sizeof(out), &written));
   TEST_ASSERT_NOT_NULL(strstr(out, "January"));
}

/* ── preprocess_text_for_tts_c: buffer truncation ────────────────────────── */

static void test_preprocess_buffer_too_small(void) {
   char out[6];
   int written = 0;
   /* "Hello world" is 11 chars but buffer is only 6 (5 + null) */
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("Hello world", out, sizeof(out), &written));
   /* Output should be null-terminated and 5 chars */
   TEST_ASSERT_EQUAL_INT(5, written);
   TEST_ASSERT_EQUAL_INT('\0', out[5]);
   TEST_ASSERT_EQUAL_INT(5, (int)strlen(out));
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void) {
   UNITY_BEGIN();

   /* remove_chars */
   RUN_TEST(test_remove_chars_basic);
   RUN_TEST(test_remove_chars_multiple_chars);
   RUN_TEST(test_remove_chars_no_match);
   RUN_TEST(test_remove_chars_empty_input);
   RUN_TEST(test_remove_chars_null_safe);

   /* is_emoji */
   RUN_TEST(test_is_emoji_ascii);
   RUN_TEST(test_is_emoji_emoji_block);
   RUN_TEST(test_is_emoji_misc_symbols);
   RUN_TEST(test_is_emoji_zwj_and_variation_selector);
   RUN_TEST(test_is_emoji_normal_unicode);

   /* remove_emojis */
   RUN_TEST(test_remove_emojis_simple);
   RUN_TEST(test_remove_emojis_preserves_normal_unicode);
   RUN_TEST(test_remove_emojis_only_ascii);
   RUN_TEST(test_remove_emojis_null_safe);

   /* preprocess: error handling */
   RUN_TEST(test_preprocess_null_input);
   RUN_TEST(test_preprocess_null_output);
   RUN_TEST(test_preprocess_zero_buffer);
   RUN_TEST(test_preprocess_null_bytes_written_ok);

   /* preprocess: passthrough */
   RUN_TEST(test_preprocess_plain_text);
   RUN_TEST(test_preprocess_empty_string);

   /* preprocess: transformations */
   RUN_TEST(test_preprocess_strips_asterisks);
   RUN_TEST(test_preprocess_strips_emoji);
   RUN_TEST(test_preprocess_em_dash_to_comma);
   RUN_TEST(test_preprocess_temperature_fahrenheit);
   RUN_TEST(test_preprocess_temperature_celsius);
   RUN_TEST(test_preprocess_currency_dollar_singular);
   RUN_TEST(test_preprocess_currency_dollar_plural);
   RUN_TEST(test_preprocess_currency_magnitude_million);
   RUN_TEST(test_preprocess_state_abbrev);
   RUN_TEST(test_preprocess_day_abbrev);
   RUN_TEST(test_preprocess_month_abbrev);

   /* preprocess: truncation */
   RUN_TEST(test_preprocess_buffer_too_small);

   return UNITY_END();
}
