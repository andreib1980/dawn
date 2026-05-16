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
 * Unit tests for memory_stem — Porter2 wrapper around libstemmer.
 *
 * These tests verify the canonical reductions used by the BM25 retrieval
 * path.  The exact stems are anchored to libstemmer 2.x's Porter2 output
 * for English with UTF-8 charset.
 */

#include <stdio.h>
#include <string.h>

#include "memory/memory_stem.h"
#include "unity.h"

void setUp(void) {
   memory_stem_init();
}
void tearDown(void) {
   /* Don't shutdown — keeping the stemmer warm across tests matches
    * production lifetime (init-on-first-use, never torn down until
    * process exit). */
}

/* Empty / NULL inputs return 0 stems and emit empty string. */
void test_stem_empty_input(void) {
   char out[64];
   TEST_ASSERT_EQUAL_INT(0, memory_stem_string(NULL, out, sizeof(out)));
   TEST_ASSERT_EQUAL_STRING("", out);

   TEST_ASSERT_EQUAL_INT(0, memory_stem_string("", out, sizeof(out)));
   TEST_ASSERT_EQUAL_STRING("", out);
}

/* NULL output buffer is rejected safely. */
void test_stem_null_output(void) {
   TEST_ASSERT_EQUAL_INT(0, memory_stem_string("test", NULL, 0));
}

/* Common English plurals → singular stem. */
void test_stem_plurals(void) {
   char out[64];
   memory_stem_string("memories", out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("memori", out);

   memory_stem_string("dogs", out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("dog", out);
}

/* Verb tense families collapse to one stem.
 * Note: "organize" / "organizing" / "organized" all stem to "organ" in
 * Porter2 — same as "organization".  Porter2 is aggressive on -ize/-ation
 * and conflates them.  This is the documented behavior we accept. */
void test_stem_verb_tenses_collapse(void) {
   char out_a[64], out_b[64], out_c[64];
   memory_stem_string("attending", out_a, sizeof(out_a));
   memory_stem_string("attended", out_b, sizeof(out_b));
   memory_stem_string("attends", out_c, sizeof(out_c));
   TEST_ASSERT_EQUAL_STRING("attend", out_a);
   TEST_ASSERT_EQUAL_STRING("attend", out_b);
   TEST_ASSERT_EQUAL_STRING("attend", out_c);
}

/* Multi-token input: stems joined by single spaces, in input order. */
void test_stem_multi_token(void) {
   char out[128];
   int n = memory_stem_string("running quickly through forests", out, sizeof(out));
   TEST_ASSERT_EQUAL_INT(4, n);
   /* Each piece comes from Porter2; assemble and check contains.  Exact
    * forms: "run quick through forest" for libstemmer 2.x. */
   TEST_ASSERT_NOT_NULL(strstr(out, "run"));
   TEST_ASSERT_NOT_NULL(strstr(out, "quick"));
   TEST_ASSERT_NOT_NULL(strstr(out, "through"));
   TEST_ASSERT_NOT_NULL(strstr(out, "forest"));
}

/* Tokens are lowercased. */
void test_stem_lowercases(void) {
   char out[64];
   memory_stem_string("HELLO World", out, sizeof(out));
   /* "hello world" stems → "hello world" (no change in Porter2). */
   TEST_ASSERT_EQUAL_STRING("hello world", out);
}

/* Punctuation acts as a token separator. */
void test_stem_punctuation_splits(void) {
   char out[64];
   int n = memory_stem_string("dogs, cats; birds.", out, sizeof(out));
   TEST_ASSERT_EQUAL_INT(3, n);
   TEST_ASSERT_EQUAL_STRING("dog cat bird", out);
}

/* Single-char tokens are dropped (< 2 chars). */
void test_stem_drops_single_chars(void) {
   char out[64];
   int n = memory_stem_string("a memory", out, sizeof(out));
   TEST_ASSERT_EQUAL_INT(1, n);
   TEST_ASSERT_EQUAL_STRING("memori", out);
}

/* memory_stem_count matches what memory_stem_string emits. */
void test_stem_count_matches_string(void) {
   char out[64];
   const char *cases[] = {
      "", "single", "two tokens", "three little tokens", "a b c short", NULL,
   };
   for (int i = 0; cases[i] != NULL; i++) {
      int from_string = memory_stem_string(cases[i], out, sizeof(out));
      int from_count = memory_stem_count(cases[i]);
      TEST_ASSERT_EQUAL_INT(from_string, from_count);
   }
}

/* Output buffer too small does not overflow and is NUL-terminated within
 * the buffer.  Stems too large to fit are dropped silently rather than
 * truncated mid-token — strlen(out) < out_sz is the contract, NOT
 * out[out_sz - 1] == 0 (the buffer may end with partial sentinel bytes
 * beyond the data terminator). */
void test_stem_truncation_safe(void) {
   char out[8];
   memset(out, 0xAA, sizeof(out));
   memory_stem_string("memories dogs cats", out, sizeof(out));
   size_t len = strlen(out);
   TEST_ASSERT_TRUE(len < sizeof(out));
   /* "memori" (6 chars) should fit; "dogs" and "cats" depend on the
    * remaining space.  Just confirm the first stem landed and the
    * terminator is in-bounds. */
   TEST_ASSERT_NOT_NULL(strstr(out, "memori"));
   TEST_ASSERT_EQUAL_INT('\0', out[len]);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_stem_empty_input);
   RUN_TEST(test_stem_null_output);
   RUN_TEST(test_stem_plurals);
   RUN_TEST(test_stem_verb_tenses_collapse);
   RUN_TEST(test_stem_multi_token);
   RUN_TEST(test_stem_lowercases);
   RUN_TEST(test_stem_punctuation_splits);
   RUN_TEST(test_stem_drops_single_chars);
   RUN_TEST(test_stem_count_matches_string);
   RUN_TEST(test_stem_truncation_safe);
   return UNITY_END();
}
