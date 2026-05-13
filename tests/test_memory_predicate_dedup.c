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
 * Unit tests for memory_predicate_dedup.{c,h} — Phase 0 predicate
 * canonicalization helpers.  Tests the DB-free helpers
 * (memory_predicate_is_standard, memory_predicate_jaccard); the
 * full canonicalize path needs DB setup and is integration-tested via
 * the bench harness.
 */

#include <stdbool.h>
#include <string.h>

#include "memory/memory_predicate_dedup.h"
#include "unity.h"

void setUp(void) {
}
void tearDown(void) {
}

/* ============================================================================
 * Standard-vocab membership
 * ============================================================================ */

void test_standard_includes_core_schema_org_types(void) {
   TEST_ASSERT_TRUE(memory_predicate_is_standard("lives_in"));
   TEST_ASSERT_TRUE(memory_predicate_is_standard("works_at"));
   TEST_ASSERT_TRUE(memory_predicate_is_standard("married_to"));
   TEST_ASSERT_TRUE(memory_predicate_is_standard("has_pet"));
   TEST_ASSERT_TRUE(memory_predicate_is_standard("primary_language"));
}

void test_standard_includes_family_relations(void) {
   /* Phase 0 additions */
   TEST_ASSERT_TRUE(memory_predicate_is_standard("parent_of"));
   TEST_ASSERT_TRUE(memory_predicate_is_standard("child_of"));
   TEST_ASSERT_TRUE(memory_predicate_is_standard("sibling_of"));
   TEST_ASSERT_TRUE(memory_predicate_is_standard("friend_of"));
   TEST_ASSERT_TRUE(memory_predicate_is_standard("member_of"));
   TEST_ASSERT_TRUE(memory_predicate_is_standard("nationality"));
}

void test_standard_includes_conceptnet_types(void) {
   TEST_ASSERT_TRUE(memory_predicate_is_standard("can"));
   TEST_ASSERT_TRUE(memory_predicate_is_standard("cannot"));
   TEST_ASSERT_TRUE(memory_predicate_is_standard("is_a"));
   TEST_ASSERT_TRUE(memory_predicate_is_standard("is"));
}

void test_standard_excludes_off_vocab(void) {
   /* These are off-vocab — the LLM has been inventing them but they're
    * NOT in the standard list (intentionally — open-vocab path). */
   TEST_ASSERT_FALSE(memory_predicate_is_standard("attended"));
   TEST_ASSERT_FALSE(memory_predicate_is_standard("plays_for"));
   TEST_ASSERT_FALSE(memory_predicate_is_standard("organized"));
   TEST_ASSERT_FALSE(memory_predicate_is_standard("created"));
   /* is_not was intentionally removed from the standard list (unused
    * in observed data; negation handled via is + negated object). */
   TEST_ASSERT_FALSE(memory_predicate_is_standard("is_not"));
}

void test_standard_rejects_null_and_empty(void) {
   TEST_ASSERT_FALSE(memory_predicate_is_standard(NULL));
   TEST_ASSERT_FALSE(memory_predicate_is_standard(""));
}

void test_standard_is_case_sensitive(void) {
   /* Predicates are stored in canonical lowercase; uppercase variants
    * are not standard.  Caller (canonicalize) lowercases before
    * checking. */
   TEST_ASSERT_FALSE(memory_predicate_is_standard("Lives_In"));
   TEST_ASSERT_FALSE(memory_predicate_is_standard("LIVES_IN"));
}

/* ============================================================================
 * Token Jaccard similarity
 * ============================================================================ */

void test_jaccard_identical_predicates(void) {
   TEST_ASSERT_EQUAL_FLOAT(1.0f, memory_predicate_jaccard("works_at", "works_at"));
   TEST_ASSERT_EQUAL_FLOAT(1.0f, memory_predicate_jaccard("single", "single"));
}

void test_jaccard_no_overlap(void) {
   TEST_ASSERT_EQUAL_FLOAT(0.0f, memory_predicate_jaccard("works_at", "married_to"));
   TEST_ASSERT_EQUAL_FLOAT(0.0f, memory_predicate_jaccard("alpha", "bravo"));
}

void test_jaccard_partial_overlap(void) {
   /* "has_child" tokens = {has, child}, "has_children" tokens = {has, children}
    * Intersection = {has} (1), Union = {has, child, children} (3) → 1/3 ≈ 0.333 */
   float score = memory_predicate_jaccard("has_child", "has_children");
   TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.333f, score);
   /* This is BELOW the 0.66 dedup floor — the resolver won't collapse
    * these via Jaccard alone.  The "previously-used relations" prompt
    * hint is what catches this duplication at extraction time. */
   TEST_ASSERT_TRUE(score < MEMORY_PREDICATE_DEDUP_JACCARD_FLOOR);
}

void test_jaccard_high_overlap_above_floor(void) {
   /* "is_friends_with" = {is, friends, with}
    * "is_friend_with"  = {is, friend, with}
    * Intersection = {is, with} (2), Union = {is, friends, friend, with} (4) → 0.5
    * Still BELOW the 0.66 floor — single-token variation in a 3-token
    * predicate keeps Jaccard at 0.5.  Real-world: relies on
    * "previously-used" hint. */
   float score = memory_predicate_jaccard("is_friends_with", "is_friend_with");
   TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.5f, score);

   /* Cleaner positive case: two-token predicates differing in tense */
   /* "has_visited" = {has, visited}, "have_visited" = {have, visited}
    * Intersection = {visited}, Union = {has, have, visited} → 1/3 */
   score = memory_predicate_jaccard("has_visited", "have_visited");
   TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.333f, score);

   /* Strong overlap case: identical tokens in different order
    * "with_is_friends" = {with, is, friends}; "is_friends_with" = same set → 1.0 */
   score = memory_predicate_jaccard("with_is_friends", "is_friends_with");
   TEST_ASSERT_EQUAL_FLOAT(1.0f, score);
}

void test_jaccard_subset_relationship(void) {
   /* "is" = {is}, "is_a" = {is, a}
    * Intersection = {is} (1), Union = {is, a} (2) → 0.5 */
   float score = memory_predicate_jaccard("is", "is_a");
   TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.5f, score);
}

void test_jaccard_null_and_empty_inputs(void) {
   TEST_ASSERT_EQUAL_FLOAT(0.0f, memory_predicate_jaccard(NULL, "works_at"));
   TEST_ASSERT_EQUAL_FLOAT(0.0f, memory_predicate_jaccard("works_at", NULL));
   TEST_ASSERT_EQUAL_FLOAT(0.0f, memory_predicate_jaccard(NULL, NULL));
   TEST_ASSERT_EQUAL_FLOAT(0.0f, memory_predicate_jaccard("", "works_at"));
   TEST_ASSERT_EQUAL_FLOAT(0.0f, memory_predicate_jaccard("works_at", ""));
}

void test_jaccard_handles_multiple_underscores(void) {
   /* Leading / trailing / repeated underscores treated as empty tokens
    * skipped — "__has__pet__" should tokenize to {has, pet}. */
   float score = memory_predicate_jaccard("__has__pet__", "has_pet");
   TEST_ASSERT_EQUAL_FLOAT(1.0f, score);
}

int main(void) {
   UNITY_BEGIN();
   /* Standard-vocab membership */
   RUN_TEST(test_standard_includes_core_schema_org_types);
   RUN_TEST(test_standard_includes_family_relations);
   RUN_TEST(test_standard_includes_conceptnet_types);
   RUN_TEST(test_standard_excludes_off_vocab);
   RUN_TEST(test_standard_rejects_null_and_empty);
   RUN_TEST(test_standard_is_case_sensitive);
   /* Jaccard primitive */
   RUN_TEST(test_jaccard_identical_predicates);
   RUN_TEST(test_jaccard_no_overlap);
   RUN_TEST(test_jaccard_partial_overlap);
   RUN_TEST(test_jaccard_high_overlap_above_floor);
   RUN_TEST(test_jaccard_subset_relationship);
   RUN_TEST(test_jaccard_null_and_empty_inputs);
   RUN_TEST(test_jaccard_handles_multiple_underscores);
   return UNITY_END();
}
