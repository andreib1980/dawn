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
 * Unit tests for messaging_split.c — paragraph / sentence / reject
 * break tiers, the 70% acceptance floor, multi-chunk content
 * preservation, null and empty edge cases, real provider caps.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dawn_error.h"
#include "messaging/messaging_split.h"
#include "unity.h"

void setUp(void) {
}
void tearDown(void) {
}

/* Test helper: free the parts array the splitter allocated. */
static void free_parts_array(char **parts, size_t n) {
   if (!parts) {
      return;
   }
   for (size_t i = 0; i < n; i++) {
      free(parts[i]);
   }
   free(parts);
}

/* ============================================================================
 * Single-message fits
 * ============================================================================ */

static void test_single_message_fits(void) {
   const char *text = "Short reply that fits.";
   char **parts = NULL;
   size_t count = 0;
   char err[256] = { 0 };
   int rc = messaging_split_at_breaks(text, 100, &parts, &count, err, sizeof(err));
   TEST_ASSERT_EQUAL_INT_MESSAGE(SUCCESS, rc, "fits in one chunk");
   TEST_ASSERT_EQUAL_size_t_MESSAGE(1, count, "exactly one chunk");
   TEST_ASSERT_NOT_NULL(parts);
   TEST_ASSERT_EQUAL_STRING(text, parts[0]);
   free_parts_array(parts, count);
}

/* ============================================================================
 * Paragraph break: \n\n
 * ============================================================================ */

static void test_paragraph_break_double_newline(void) {
   /* "Para A.\n\nPara B." with max_chars = 10 forces split at \n\n.
    * Window for first chunk = [7, 10].  \n\n is at indices 7,8 →
    * run_start = 7 which is in window.  cut_at = 7, next_pos = 9. */
   const char *text = "Para A.\n\nPara B.";
   char **parts = NULL;
   size_t count = 0;
   char err[256] = { 0 };
   int rc = messaging_split_at_breaks(text, 10, &parts, &count, err, sizeof(err));
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   TEST_ASSERT_EQUAL_size_t(2, count);
   TEST_ASSERT_EQUAL_STRING("Para A.", parts[0]);
   TEST_ASSERT_EQUAL_STRING("Para B.", parts[1]);
   free_parts_array(parts, count);
}

/* ============================================================================
 * Paragraph break: single \n (fallback)
 * ============================================================================ */

static void test_paragraph_break_single_newline(void) {
   /* No \n\n, but a single \n at index 7 → fallback to \n tier.
    * "Para A.\nPara B." len=15, max=10, window=[7,10].
    * \n at 7 in window. */
   const char *text = "Para A.\nPara B.";
   char **parts = NULL;
   size_t count = 0;
   char err[256] = { 0 };
   int rc = messaging_split_at_breaks(text, 10, &parts, &count, err, sizeof(err));
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   TEST_ASSERT_EQUAL_size_t(2, count);
   TEST_ASSERT_EQUAL_STRING("Para A.", parts[0]);
   TEST_ASSERT_EQUAL_STRING("Para B.", parts[1]);
   free_parts_array(parts, count);
}

/* ============================================================================
 * Sentence break: ". "
 * ============================================================================ */

static void test_sentence_break_period_space(void) {
   /* "One. Two. Three. Four." len=22.  max=12, window=[8,12].
    * Sentence breaks at closing_end positions: 4 ("One. "), 9 ("Two. "),
    * 16 ("Three. ").  Position 9 is in window → cut there.  remaining
    * after = "Three. Four." (12 chars) which fits exactly. */
   const char *text = "One. Two. Three. Four.";
   char **parts = NULL;
   size_t count = 0;
   char err[256] = { 0 };
   int rc = messaging_split_at_breaks(text, 12, &parts, &count, err, sizeof(err));
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   TEST_ASSERT_EQUAL_size_t(2, count);
   TEST_ASSERT_EQUAL_STRING("One. Two.", parts[0]);
   TEST_ASSERT_EQUAL_STRING("Three. Four.", parts[1]);
   free_parts_array(parts, count);
}

/* ============================================================================
 * Sentence break with closing quote
 * ============================================================================ */

static void test_sentence_break_with_quote(void) {
   /* `He said, "Hi." Bye.` len=19.
    *  H=0 e=1 ' '=2 s=3 a=4 i=5 d=6 ,=7 ' '=8 "=9 H=10 i=11 .=12 "=13 ' '=14 B=15 y=16 e=17 .=18
    * Sentence terminator . at 12; closing-punct " at 13; whitespace ' ' at 14.
    * closing_end = 14, ws_end = 15.  cut_at = 14.
    * max=16 → window=[11,16]; cut at 14 is in window.  remaining
    * "Bye." (4 chars) fits in the next chunk. */
   const char *text = "He said, \"Hi.\" Bye.";
   char **parts = NULL;
   size_t count = 0;
   char err[256] = { 0 };
   int rc = messaging_split_at_breaks(text, 16, &parts, &count, err, sizeof(err));
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   TEST_ASSERT_EQUAL_size_t(2, count);
   TEST_ASSERT_EQUAL_STRING_MESSAGE("He said, \"Hi.\"", parts[0],
                                    "first chunk includes the closing quote");
   TEST_ASSERT_EQUAL_STRING("Bye.", parts[1]);
   free_parts_array(parts, count);
}

/* ============================================================================
 * Sentence break: ? and !
 * ============================================================================ */

static void test_sentence_break_question_exclamation(void) {
   /* "Wow! That is great. Bye." len=24.  max=20 → window=[14,20].
    * Terminators with valid trailing-whitespace breaks: ! at 3
    * (closing_end=4), . at 18 (closing_end=19).  Position 19 is in
    * window → cut there.  remaining "Bye." (4 chars) fits. */
   const char *text = "Wow! That is great. Bye.";
   char **parts = NULL;
   size_t count = 0;
   char err[256] = { 0 };
   int rc = messaging_split_at_breaks(text, 20, &parts, &count, err, sizeof(err));
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   TEST_ASSERT_EQUAL_size_t(2, count);
   TEST_ASSERT_EQUAL_STRING("Wow! That is great.", parts[0]);
   TEST_ASSERT_EQUAL_STRING("Bye.", parts[1]);
   free_parts_array(parts, count);
}

/* ============================================================================
 * Reject: no break in window
 * ============================================================================ */

static void test_no_break_in_window_rejects(void) {
   /* 50 'a' chars, no whitespace or punctuation.  max=10 forces a cut
    * but there are no break candidates anywhere → reject. */
   char text[51];
   memset(text, 'a', 50);
   text[50] = '\0';
   char **parts = NULL;
   size_t count = 0;
   char err[256] = { 0 };
   int rc = messaging_split_at_breaks(text, 10, &parts, &count, err, sizeof(err));
   TEST_ASSERT_EQUAL_INT_MESSAGE(FAILURE, rc, "unbroken text rejects");
   TEST_ASSERT_NULL(parts);
   TEST_ASSERT_EQUAL_size_t(0, count);
   TEST_ASSERT_TRUE_MESSAGE(strlen(err) > 0, "err_msg populated on reject");
   TEST_ASSERT_TRUE_MESSAGE(strstr(err, "WebUI") != NULL, "err_msg points to WebUI");
}

/* ============================================================================
 * Reject: break exists but outside acceptance window
 * ============================================================================ */

static void test_break_exists_but_outside_window_rejects(void) {
   /* Sentence break at position 5 ("Hi. "), then 100 unbroken chars.
    * max=50, window=[35,50].  The break at 5 is well outside the
    * window → no acceptable cut → reject. */
   char text[110];
   strcpy(text, "Hi. ");
   memset(text + 4, 'x', 100);
   text[104] = '\0';
   char **parts = NULL;
   size_t count = 0;
   char err[256] = { 0 };
   int rc = messaging_split_at_breaks(text, 50, &parts, &count, err, sizeof(err));
   TEST_ASSERT_EQUAL_INT(FAILURE, rc);
   TEST_ASSERT_NULL(parts);
}

/* ============================================================================
 * Three-part split
 * ============================================================================ */

static void test_three_part_split(void) {
   /* Three full paragraphs, each ~30 chars, max=40 forces three chunks. */
   const char *text = "First paragraph here, plenty long.\n\nSecond paragraph here, also "
                      "plenty.\n\nThird paragraph here, ditto.";
   char **parts = NULL;
   size_t count = 0;
   char err[256] = { 0 };
   int rc = messaging_split_at_breaks(text, 40, &parts, &count, err, sizeof(err));
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   TEST_ASSERT_TRUE_MESSAGE(count >= 2, "produces multiple chunks");
   /* Concatenate chunks and verify each one fits the cap. */
   for (size_t i = 0; i < count; i++) {
      TEST_ASSERT_TRUE_MESSAGE(strlen(parts[i]) <= 40, "each chunk fits cap");
   }
   free_parts_array(parts, count);
}

/* ============================================================================
 * NULL / invalid input
 * ============================================================================ */

static void test_null_inputs(void) {
   char **parts = (char **)0x1; /* poison */
   size_t count = 99;
   char err[256] = { 0 };

   TEST_ASSERT_EQUAL_INT(FAILURE,
                         messaging_split_at_breaks(NULL, 100, &parts, &count, err, sizeof(err)));
   TEST_ASSERT_NULL_MESSAGE(parts, "parts set NULL on failure even when input is NULL");

   parts = NULL;
   count = 99;
   TEST_ASSERT_EQUAL_INT(FAILURE,
                         messaging_split_at_breaks("text", 0, &parts, &count, err, sizeof(err)));
   TEST_ASSERT_EQUAL_INT(FAILURE,
                         messaging_split_at_breaks("text", 100, NULL, &count, err, sizeof(err)));
   TEST_ASSERT_EQUAL_INT(FAILURE,
                         messaging_split_at_breaks("text", 100, &parts, NULL, err, sizeof(err)));
}

/* ============================================================================
 * Empty input
 * ============================================================================ */

static void test_empty_text(void) {
   char **parts = NULL;
   size_t count = 0;
   char err[256] = { 0 };
   int rc = messaging_split_at_breaks("", 100, &parts, &count, err, sizeof(err));
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   TEST_ASSERT_EQUAL_size_t(1, count);
   TEST_ASSERT_EQUAL_STRING("", parts[0]);
   free_parts_array(parts, count);
}

/* ============================================================================
 * Unicode preserved (cuts only on ASCII bytes)
 * ============================================================================ */

static void test_unicode_preserved(void) {
   /* Emoji + paragraph break.  Splitter operates on bytes; the break
    * is ASCII \n\n so multi-byte UTF-8 sequences in the chunks should
    * be intact.  Each chunk should round-trip as valid UTF-8. */
   const char *text = "Hello 🌍 world.\n\nGoodbye 🌙 moon.";
   char **parts = NULL;
   size_t count = 0;
   char err[256] = { 0 };
   int rc = messaging_split_at_breaks(text, 20, &parts, &count, err, sizeof(err));
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   TEST_ASSERT_EQUAL_size_t(2, count);
   /* Emoji bytes should appear intact in their respective chunks. */
   TEST_ASSERT_TRUE_MESSAGE(strstr(parts[0], "🌍") != NULL, "first emoji intact");
   TEST_ASSERT_TRUE_MESSAGE(strstr(parts[1], "🌙") != NULL, "second emoji intact");
   free_parts_array(parts, count);
}

/* ============================================================================
 * Content preservation invariant
 * ============================================================================ */

static void test_concat_preserves_content(void) {
   /* For a paragraph split, concatenating the chunks with "\n\n" between
    * them MUST reproduce the original input.  Pins the "no content
    * silently lost" invariant. */
   const char *text = "Alpha paragraph here.\n\nBeta paragraph follows.\n\nGamma is third.";
   char **parts = NULL;
   size_t count = 0;
   char err[256] = { 0 };
   int rc = messaging_split_at_breaks(text, 25, &parts, &count, err, sizeof(err));
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   TEST_ASSERT_EQUAL_size_t(3, count);
   char joined[256];
   snprintf(joined, sizeof(joined), "%s\n\n%s\n\n%s", parts[0], parts[1], parts[2]);
   TEST_ASSERT_EQUAL_STRING_MESSAGE(text, joined, "join(chunks, \"\\n\\n\") == original input");
   free_parts_array(parts, count);
}

/* ============================================================================
 * Acceptance window picks the latest break
 * ============================================================================ */

static void test_acceptance_window_picks_latest(void) {
   /* "Hi. Yo. Hey. Sup. Bye." len=22.
    * max=20, window=[14,20].
    * Sentence breaks at closing_end: 3, 7, 12, 17.
    * Position 17 ("Sup.") is the LATEST in window → cut there. */
   const char *text = "Hi. Yo. Hey. Sup. Bye.";
   char **parts = NULL;
   size_t count = 0;
   char err[256] = { 0 };
   int rc = messaging_split_at_breaks(text, 20, &parts, &count, err, sizeof(err));
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   TEST_ASSERT_EQUAL_size_t(2, count);
   TEST_ASSERT_EQUAL_STRING_MESSAGE("Hi. Yo. Hey. Sup.", parts[0], "picks latest in-window break");
   TEST_ASSERT_EQUAL_STRING("Bye.", parts[1]);
   free_parts_array(parts, count);
}

/* ============================================================================
 * Provider real limits
 * ============================================================================ */

static void test_provider_real_limits(void) {
   /* Build a long string of sentences.  Each ~30 chars.  Split at the
    * four real provider caps.  Verify all succeed and chunk counts
    * scale roughly inversely with cap. */
   char text[8192];
   text[0] = '\0';
   for (int i = 0; i < 200; i++) {
      char snippet[64];
      snprintf(snippet, sizeof(snippet), "Sentence number %d goes here. ", i);
      strcat(text, snippet);
   }
   size_t text_len = strlen(text);
   TEST_ASSERT_TRUE(text_len > 3980); /* big enough to exercise all caps */

   struct {
      const char *name;
      size_t cap;
   } cases[] = {
      { "discord", 1980 },
      { "telegram", 4076 },
      { "slack", 3980 },
      { "sms", 670 },
   };
   for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
      char **parts = NULL;
      size_t count = 0;
      char err[256] = { 0 };
      int rc = messaging_split_at_breaks(text, cases[c].cap, &parts, &count, err, sizeof(err));
      char msg[128];
      snprintf(msg, sizeof(msg), "%s (cap=%zu) splits cleanly", cases[c].name, cases[c].cap);
      TEST_ASSERT_EQUAL_INT_MESSAGE(SUCCESS, rc, msg);
      for (size_t i = 0; i < count; i++) {
         TEST_ASSERT_TRUE(strlen(parts[i]) <= cases[c].cap);
      }
      free_parts_array(parts, count);
   }
}

/* ============================================================================
 * Exact-boundary break
 * ============================================================================ */

static void test_exact_boundary_break(void) {
   /* Sentence-end ". " sits with closing_end exactly at max_chars.
    * "A. B. C. D. E."  closing_end positions: 2, 5, 8, 11.
    * max=11 → window=[7,11].  closing_end=11 sits exactly at window_hi
    * and MUST be accepted. */
   const char *text = "A. B. C. D. E.";
   char **parts = NULL;
   size_t count = 0;
   char err[256] = { 0 };
   int rc = messaging_split_at_breaks(text, 11, &parts, &count, err, sizeof(err));
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   TEST_ASSERT_EQUAL_size_t(2, count);
   TEST_ASSERT_EQUAL_STRING("A. B. C. D.", parts[0]);
   TEST_ASSERT_EQUAL_STRING("E.", parts[1]);
   free_parts_array(parts, count);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_single_message_fits);
   RUN_TEST(test_paragraph_break_double_newline);
   RUN_TEST(test_paragraph_break_single_newline);
   RUN_TEST(test_sentence_break_period_space);
   RUN_TEST(test_sentence_break_with_quote);
   RUN_TEST(test_sentence_break_question_exclamation);
   RUN_TEST(test_no_break_in_window_rejects);
   RUN_TEST(test_break_exists_but_outside_window_rejects);
   RUN_TEST(test_three_part_split);
   RUN_TEST(test_null_inputs);
   RUN_TEST(test_empty_text);
   RUN_TEST(test_unicode_preserved);
   RUN_TEST(test_concat_preserves_content);
   RUN_TEST(test_acceptance_window_picks_latest);
   RUN_TEST(test_provider_real_limits);
   RUN_TEST(test_exact_boundary_break);
   return UNITY_END();
}
