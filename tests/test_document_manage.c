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
 * Unit tests for the document_manage edit/append pure text ops (B1a) — the
 * find/replace unique-match contract and the append newline-join, tested in
 * isolation from the storage/embedding layer.
 */

#include <stdlib.h>
#include <string.h>

#include "tools/document_manage.h"
#include "unity.h"

void setUp(void) {
}
void tearDown(void) {
}

/* --- find/replace: unique-match contract --- */

void test_find_replace_unique(void) {
   char *out = NULL;
   int occ = docmgmt_find_replace_once("The quick brown fox", "quick", "slow", &out);
   TEST_ASSERT_EQUAL_INT(1, occ);
   TEST_ASSERT_NOT_NULL(out);
   TEST_ASSERT_EQUAL_STRING("The slow brown fox", out);
   free(out);
}

void test_find_replace_not_found(void) {
   char *out = (char *)0x1; /* poison — must be cleared */
   int occ = docmgmt_find_replace_once("The quick brown fox", "cat", "dog", &out);
   TEST_ASSERT_EQUAL_INT(0, occ);
   TEST_ASSERT_NULL(out);
}

void test_find_replace_ambiguous_untouched(void) {
   char *out = (char *)0x1;
   int occ = docmgmt_find_replace_once("a a a", "a", "b", &out);
   TEST_ASSERT_EQUAL_INT(2, occ); /* "two or more" — refuse, leave note alone */
   TEST_ASSERT_NULL(out);
}

void test_find_replace_empty_is_deletion(void) {
   char *out = NULL;
   int occ = docmgmt_find_replace_once("hello world", " world", "", &out);
   TEST_ASSERT_EQUAL_INT(1, occ);
   TEST_ASSERT_EQUAL_STRING("hello", out);
   free(out);
}

void test_find_replace_at_start_and_end(void) {
   char *out = NULL;
   docmgmt_find_replace_once("START middle END", "START", "BEGIN", &out);
   TEST_ASSERT_EQUAL_STRING("BEGIN middle END", out);
   free(out);
   out = NULL;
   docmgmt_find_replace_once("START middle END", "END", "FINISH", &out);
   TEST_ASSERT_EQUAL_STRING("START middle FINISH", out);
   free(out);
}

/* The whole point of the JSON-tail encoding: find/replace text containing "::"
 * (Obsidian Key:: value, C++ std::) must round-trip — these are plain strings to
 * the splice, so the only requirement is the splice doesn't choke on them. */
void test_find_replace_colon_colon_content(void) {
   char *out = NULL;
   int occ = docmgmt_find_replace_once("Deadline:: June 10 (std::move)", "Deadline:: June 10",
                                       "Done", &out);
   TEST_ASSERT_EQUAL_INT(1, occ);
   TEST_ASSERT_EQUAL_STRING("Done (std::move)", out);
   free(out);
}

void test_find_replace_grow_and_shrink(void) {
   char *out = NULL;
   docmgmt_find_replace_once("x", "x", "xxxxxxxxxx", &out); /* grow */
   TEST_ASSERT_EQUAL_STRING("xxxxxxxxxx", out);
   free(out);
   out = NULL;
   docmgmt_find_replace_once("aXXXXXXb", "XXXXXX", "Y", &out); /* shrink */
   TEST_ASSERT_EQUAL_STRING("aYb", out);
   free(out);
}

/* find == the entire note: a unique match, whole-note replace. */
void test_find_replace_whole_string(void) {
   char *out = NULL;
   int occ = docmgmt_find_replace_once("entire note", "entire note", "new body", &out);
   TEST_ASSERT_EQUAL_INT(1, occ);
   TEST_ASSERT_EQUAL_STRING("new body", out);
   free(out);
}

/* Overlapping occurrences count as ambiguous (>=2) and refuse — the safe choice:
 * "aa" matches at offset 0 AND 1 in "aaa". */
void test_find_replace_overlapping_is_ambiguous(void) {
   char *out = (char *)0x1;
   int occ = docmgmt_find_replace_once("aaa", "aa", "b", &out);
   TEST_ASSERT_EQUAL_INT(2, occ);
   TEST_ASSERT_NULL(out);
}

/* --- append: newline-join semantics --- */

void test_append_newline_joined(void) {
   char *out = docmgmt_append_text("first line", "second line");
   TEST_ASSERT_EQUAL_STRING("first line\nsecond line", out);
   free(out);
}

void test_append_no_double_newline(void) {
   char *out = docmgmt_append_text("already ends\n", "next");
   TEST_ASSERT_EQUAL_STRING("already ends\nnext", out); /* no blank line inserted */
   free(out);
}

/* The LLM commonly prepends its own '\n' to the appended text — we must NOT add a
 * second separator on top (that leaves a blank line).  Observed live 2026-06-12. */
void test_append_text_with_leading_newline(void) {
   char *out = docmgmt_append_text("first line", "\nsecond line");
   TEST_ASSERT_EQUAL_STRING("first line\nsecond line", out); /* single break, not "\n\n" */
   free(out);
}

void test_append_to_empty(void) {
   char *out = docmgmt_append_text("", "hello"); /* no leading newline on empty src */
   TEST_ASSERT_EQUAL_STRING("hello", out);
   free(out);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_find_replace_unique);
   RUN_TEST(test_find_replace_not_found);
   RUN_TEST(test_find_replace_ambiguous_untouched);
   RUN_TEST(test_find_replace_empty_is_deletion);
   RUN_TEST(test_find_replace_at_start_and_end);
   RUN_TEST(test_find_replace_colon_colon_content);
   RUN_TEST(test_find_replace_grow_and_shrink);
   RUN_TEST(test_find_replace_whole_string);
   RUN_TEST(test_find_replace_overlapping_is_ambiguous);
   RUN_TEST(test_append_newline_joined);
   RUN_TEST(test_append_no_double_newline);
   RUN_TEST(test_append_text_with_leading_newline);
   RUN_TEST(test_append_to_empty);
   return UNITY_END();
}
