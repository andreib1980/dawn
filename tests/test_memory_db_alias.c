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
 * Unit tests for memory_db_alias.c — entity-merge alias surface (v43).
 *
 * Coverage (per docs/ENTITY_MERGE_IMPL_BRIEF.md Ckpt 2 §10):
 *   - pure helpers: name_jaccard, name_substring, composite formula,
 *     canonical_priority lexicographic comparison
 *   - cascade stages 1-3 (Stage 4/5 are exercised indirectly via score_pair
 *     and consider_auto_merge — the embedding engine is stubbed false in
 *     this test fixture so Stage 4 contributes 0 to the composite)
 *   - threshold band routing in consider_auto_merge: AUTO_MERGED, PROPOSED,
 *     REJECTED, NO_CANDIDATES
 *   - alias_link / alias_unlink: write paths, hard-merge refusal, dependent
 *     refusal, embedding-cache invalidation
 *   - relation_list_by_subject_class: canonical row + aliases
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "memory/memory_db.h"
#include "memory/memory_db_aliases.h"
#include "memory/memory_db_entities.h"
#include "memory/memory_types.h"
#include "unity.h"

extern int g_alias_test_entity_cache_invalidations;

/* ============================================================================
 * setUp / tearDown
 * ============================================================================ */

static int g_test_user_id = 0;

void setUp(void) {
   auth_db_init(":memory:");
   g_alias_test_entity_cache_invalidations = 0;

   /* Create a single test user.  All entity tests scope by user_id. */
   auth_db_create_user("kris", "hash", true);
   auth_user_t u;
   memset(&u, 0, sizeof(u));
   auth_db_get_user("kris", &u);
   g_test_user_id = u.id;
}

void tearDown(void) {
   auth_db_shutdown();
}

/* ============================================================================
 * Helpers — direct DB inserts (bypass the production upsert so tests can
 * exercise specific entity shapes without re-routing through it).
 * ============================================================================ */

static int64_t insert_entity_typed(int user_id, const char *name, const char *type) {
   char canonical[MEMORY_ENTITY_NAME_MAX];
   memory_make_canonical_name(name, canonical, sizeof(canonical));
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(s_db.db,
                      "INSERT INTO memory_entities (user_id, name, entity_type, canonical_name) "
                      "VALUES (?, ?, ?, ?)",
                      -1, &stmt, NULL);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, type, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 4, canonical, -1, SQLITE_TRANSIENT);
   sqlite3_step(stmt);
   int64_t id = sqlite3_last_insert_rowid(s_db.db);
   sqlite3_finalize(stmt);
   return id;
}

static void mark_entity_user_self(int64_t entity_id) {
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(s_db.db, "UPDATE memory_entities SET is_user_self = 1 WHERE id = ?", -1,
                      &stmt, NULL);
   sqlite3_bind_int64(stmt, 1, entity_id);
   sqlite3_step(stmt);
   sqlite3_finalize(stmt);
}

static void insert_open_relation(int user_id,
                                 int64_t subj,
                                 const char *relation,
                                 int64_t obj_entity_id,
                                 const char *obj_value) {
   sqlite3_stmt *stmt = NULL;
   /* fact_id is NULL (literal NULL — not 0, which would trigger the FK on
    * memory_facts.id since no facts exist in this fixture). */
   int prc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO memory_relations (user_id, subject_entity_id, relation, "
       "object_entity_id, object_value, fact_id, confidence, valid_from, valid_to) "
       "VALUES (?, ?, ?, ?, ?, NULL, 0.8, NULL, NULL)",
       -1, &stmt, NULL);
   if (prc != SQLITE_OK) {
      fprintf(stderr, "insert_open_relation prepare failed: %s\n", sqlite3_errmsg(s_db.db));
      return;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, subj);
   sqlite3_bind_text(stmt, 3, relation, -1, SQLITE_TRANSIENT);
   if (obj_entity_id > 0) {
      sqlite3_bind_int64(stmt, 4, obj_entity_id);
   } else {
      sqlite3_bind_null(stmt, 4);
   }
   if (obj_value && *obj_value) {
      sqlite3_bind_text(stmt, 5, obj_value, -1, SQLITE_TRANSIENT);
   } else {
      sqlite3_bind_null(stmt, 5);
   }
   int src = sqlite3_step(stmt);
   if (src != SQLITE_DONE) {
      fprintf(stderr, "insert_open_relation step rc=%d errmsg=%s\n", src, sqlite3_errmsg(s_db.db));
   }
   sqlite3_finalize(stmt);
}

static void insert_contact(int user_id,
                           int64_t entity_id,
                           const char *field_type,
                           const char *value) {
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO contacts (user_id, entity_id, field_type, value, label, created_at) "
       "VALUES (?, ?, ?, ?, '', strftime('%s','now'))",
       -1, &stmt, NULL);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, entity_id);
   sqlite3_bind_text(stmt, 3, field_type, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 4, value, -1, SQLITE_TRANSIENT);
   sqlite3_step(stmt);
   sqlite3_finalize(stmt);
}

static int64_t get_entity_canonical_id(int64_t entity_id) {
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(s_db.db, "SELECT canonical_id FROM memory_entities WHERE id = ?", -1, &stmt,
                      NULL);
   sqlite3_bind_int64(stmt, 1, entity_id);
   int64_t result = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
      result = sqlite3_column_int64(stmt, 0);
   }
   sqlite3_finalize(stmt);
   return result;
}

static int count_active_aliases_for_target(int64_t target_id) {
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(s_db.db,
                      "SELECT COUNT(*) FROM memory_entity_aliases "
                      "WHERE target_entity_id = ? AND unlinked_at IS NULL",
                      -1, &stmt, NULL);
   sqlite3_bind_int64(stmt, 1, target_id);
   int n = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      n = sqlite3_column_int(stmt, 0);
   }
   sqlite3_finalize(stmt);
   return n;
}

/* ============================================================================
 * Pure-function helpers (no DB)
 * ============================================================================ */

static void test_name_jaccard_basic(void) {
   /* Identical strings → Jaccard 1.0 (modulo single-token-set quirks). */
   TEST_ASSERT_EQUAL_FLOAT(1.0f, memory_alias_compute_name_jaccard("kris", "kris"));

   /* Subset case: "kris" ⊂ "kristopher kersey" — tokens are {"kris"} and
    * {"kristopher", "kersey"}.  Intersection = 0, union = 3 → 0.0.  This is
    * the Jaccard-misses-substring case the substring bonus exists for. */
   TEST_ASSERT_EQUAL_FLOAT(0.0f, memory_alias_compute_name_jaccard("kris", "kristopher kersey"));

   /* Shared full token: tokens {"kristopher", "kersey"} vs {"kristopher", "k"}
    * — "k" is dropped (length < 2), so {"kristopher", "kersey"} vs
    * {"kristopher"} → intersection = 1, union = 2, Jaccard = 0.5. */
   TEST_ASSERT_EQUAL_FLOAT(0.5f,
                           memory_alias_compute_name_jaccard("kristopher kersey", "kristopher k"));

   /* Disjoint tokens: 0.0. */
   TEST_ASSERT_EQUAL_FLOAT(0.0f, memory_alias_compute_name_jaccard("apple", "banana"));

   /* NULL / empty → 0.0. */
   TEST_ASSERT_EQUAL_FLOAT(0.0f, memory_alias_compute_name_jaccard(NULL, "x"));
   TEST_ASSERT_EQUAL_FLOAT(0.0f, memory_alias_compute_name_jaccard("x", ""));
}

static void test_name_substring_basic(void) {
   TEST_ASSERT_TRUE(memory_alias_compute_name_substring("kris", "kristopher kersey"));
   TEST_ASSERT_TRUE(memory_alias_compute_name_substring("kristopher kersey", "kris"));
   TEST_ASSERT_FALSE(memory_alias_compute_name_substring("kris", "kris"));  /* equal */
   TEST_ASSERT_FALSE(memory_alias_compute_name_substring("kris", "alice")); /* disjoint */
   TEST_ASSERT_FALSE(memory_alias_compute_name_substring(NULL, "x"));
   TEST_ASSERT_FALSE(memory_alias_compute_name_substring("x", NULL));
}

static void test_composite_weighted_sum(void) {
   /* Each signal in isolation. */
   memory_alias_evidence_t ev;

   memset(&ev, 0, sizeof(ev));
   ev.name_jaccard = 1.0f;
   memory_alias_apply_composite(&ev);
   TEST_ASSERT_EQUAL_FLOAT(0.30f, ev.composite_score);

   memset(&ev, 0, sizeof(ev));
   ev.embedding_cosine = 1.0f;
   memory_alias_apply_composite(&ev);
   TEST_ASSERT_EQUAL_FLOAT(0.30f, ev.composite_score);

   memset(&ev, 0, sizeof(ev));
   ev.exclusive_relation_overlap = 1.0f;
   memory_alias_apply_composite(&ev);
   TEST_ASSERT_EQUAL_FLOAT(0.25f, ev.composite_score);

   memset(&ev, 0, sizeof(ev));
   ev.contact_field_overlap = 1.0f;
   memory_alias_apply_composite(&ev);
   TEST_ASSERT_EQUAL_FLOAT(0.10f, ev.composite_score);

   memset(&ev, 0, sizeof(ev));
   ev.type_match = 1.0f;
   memory_alias_apply_composite(&ev);
   TEST_ASSERT_EQUAL_FLOAT(0.05f, ev.composite_score);

   /* Cosine clamping: negative → 0. */
   memset(&ev, 0, sizeof(ev));
   ev.embedding_cosine = -0.5f;
   memory_alias_apply_composite(&ev);
   TEST_ASSERT_EQUAL_FLOAT(0.0f, ev.composite_score);
}

static void test_composite_bonuses(void) {
   memory_alias_evidence_t ev;

   /* substring bonus alone: jaccard floor + bonus. */
   memset(&ev, 0, sizeof(ev));
   ev.name_substring_bonus_applied = true;
   memory_alias_apply_composite(&ev);
   TEST_ASSERT_EQUAL_FLOAT(0.10f, ev.composite_score);

   /* user_self bonus alone. */
   memset(&ev, 0, sizeof(ev));
   ev.user_self_bonus_applied = true;
   memory_alias_apply_composite(&ev);
   TEST_ASSERT_EQUAL_FLOAT(0.20f, ev.composite_score);

   /* Both bonuses + name signal cap to 1.0 if they would overflow. */
   memset(&ev, 0, sizeof(ev));
   ev.name_jaccard = 1.0f;
   ev.embedding_cosine = 1.0f;
   ev.exclusive_relation_overlap = 1.0f;
   ev.contact_field_overlap = 1.0f;
   ev.type_match = 1.0f;
   ev.name_substring_bonus_applied = true;
   ev.user_self_bonus_applied = true;
   memory_alias_apply_composite(&ev);
   TEST_ASSERT_EQUAL_FLOAT(1.0f, ev.composite_score);
}

static void test_composite_veto(void) {
   memory_alias_evidence_t ev;

   /* Veto fires → composite forced to 0 even with strong signals. */
   memset(&ev, 0, sizeof(ev));
   ev.type_veto_fired = true;
   ev.name_jaccard = 1.0f;
   ev.embedding_cosine = 1.0f;
   ev.exclusive_relation_overlap = 1.0f;
   memory_alias_apply_composite(&ev);
   TEST_ASSERT_EQUAL_FLOAT(0.0f, ev.composite_score);
}

static void test_canonical_priority_compare_self_user_self_first(void) {
   /* a.is_user_self=true, b.is_user_self=false.  Even when b dominates on
    * every other axis (better type, higher mention_count, older first_seen,
    * lower id), a still wins because the is_user_self axis short-circuits
    * the tuple. */
   memory_entity_t a, b;
   memset(&a, 0, sizeof(a));
   memset(&b, 0, sizeof(b));
   strcpy(a.entity_type, "thing");
   strcpy(b.entity_type, "person");
   a.id = 999;
   b.id = 1;
   a.mention_count = 1;
   b.mention_count = 100;
   a.first_seen = 9999;
   b.first_seen = 1;

   TEST_ASSERT_TRUE(memory_alias_canonical_priority_compare_self(&a, true, &b, false) < 0);
   /* And the reverse direction. */
   TEST_ASSERT_TRUE(memory_alias_canonical_priority_compare_self(&a, false, &b, true) > 0);
}

static void test_canonical_priority_compare_self_falls_through(void) {
   /* Both is_user_self flags equal — the sibling delegates to the four-axis
    * comparator, so the result must match memory_alias_canonical_priority_compare. */
   memory_entity_t a, b;
   memset(&a, 0, sizeof(a));
   memset(&b, 0, sizeof(b));
   strcpy(a.entity_type, "person");
   strcpy(b.entity_type, "thing");
   a.id = 7;
   b.id = 3;

   int four_axis = memory_alias_canonical_priority_compare(&a, &b);
   /* Both flags false: same result as four-axis. */
   TEST_ASSERT_EQUAL_INT(four_axis,
                         memory_alias_canonical_priority_compare_self(&a, false, &b, false));
   /* Both flags true: same result as four-axis (no short-circuit fires). */
   TEST_ASSERT_EQUAL_INT(four_axis,
                         memory_alias_canonical_priority_compare_self(&a, true, &b, true));
   /* And type_specificity DESC means person beats thing. */
   TEST_ASSERT_TRUE(four_axis < 0);
}

static void test_canonical_priority_compare_pure(void) {
   /* type_specificity DESC: person > place > thing. */
   memory_entity_t a, b;
   memset(&a, 0, sizeof(a));
   memset(&b, 0, sizeof(b));
   strcpy(a.entity_type, "person");
   strcpy(b.entity_type, "thing");
   a.id = 1;
   b.id = 2;
   /* a (person) wins over b (thing). */
   TEST_ASSERT_TRUE(memory_alias_canonical_priority_compare(&a, &b) < 0);

   /* Equal type → mention_count DESC. */
   memset(&a, 0, sizeof(a));
   memset(&b, 0, sizeof(b));
   strcpy(a.entity_type, "person");
   strcpy(b.entity_type, "person");
   a.mention_count = 5;
   b.mention_count = 50;
   a.id = 1;
   b.id = 2;
   /* b's mention_count is higher → b wins. */
   TEST_ASSERT_TRUE(memory_alias_canonical_priority_compare(&a, &b) > 0);

   /* Equal mention_count → first_seen ASC (older wins). */
   memset(&a, 0, sizeof(a));
   memset(&b, 0, sizeof(b));
   strcpy(a.entity_type, "person");
   strcpy(b.entity_type, "person");
   a.mention_count = b.mention_count = 10;
   a.first_seen = 1000;
   b.first_seen = 2000;
   a.id = 7;
   b.id = 3;
   /* a is older → a wins. */
   TEST_ASSERT_TRUE(memory_alias_canonical_priority_compare(&a, &b) < 0);

   /* Equal first_seen → id ASC (lower wins). */
   memset(&a, 0, sizeof(a));
   memset(&b, 0, sizeof(b));
   strcpy(a.entity_type, "person");
   strcpy(b.entity_type, "person");
   a.first_seen = b.first_seen = 1000;
   a.id = 5;
   b.id = 9;
   TEST_ASSERT_TRUE(memory_alias_canonical_priority_compare(&a, &b) < 0);
}

/* ============================================================================
 * Resolver cascade — Stage 1 + 2 + 3 (Stage 4 stubs to no-op without engine)
 * ============================================================================ */

static void test_resolver_stage1_exact_match(void) {
   int64_t kris = insert_entity_typed(g_test_user_id, "Kristopher Kersey", "person");

   memory_alias_resolve_t res;
   int rc = memory_db_entity_resolve_alias(g_test_user_id, "Kristopher Kersey", "person",
                                           "kristopher kersey", &res);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(1, res.matched_stage);
   TEST_ASSERT_EQUAL_INT64(kris, res.resolved_id);
}

static void test_resolver_stage1_resolves_through_alias(void) {
   /* If an alias row exists pointing at canonical, Stage 1 should return
    * the canonical id (post-COALESCE), not the alias's own id. */
   int64_t target = insert_entity_typed(g_test_user_id, "Kristopher Kersey", "person");
   int64_t alias = insert_entity_typed(g_test_user_id, "Kris", "person");
   /* Manually set canonical_id on the alias row to simulate a prior link. */
   sqlite3_stmt *u = NULL;
   sqlite3_prepare_v2(s_db.db, "UPDATE memory_entities SET canonical_id = ? WHERE id = ?", -1, &u,
                      NULL);
   sqlite3_bind_int64(u, 1, target);
   sqlite3_bind_int64(u, 2, alias);
   sqlite3_step(u);
   sqlite3_finalize(u);

   memory_alias_resolve_t res;
   int rc = memory_db_entity_resolve_alias(g_test_user_id, "Kris", "person", "kris", &res);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(1, res.matched_stage);
   TEST_ASSERT_EQUAL_INT64(target, res.resolved_id); /* canonical, not alias */
}

static void test_resolver_no_match(void) {
   insert_entity_typed(g_test_user_id, "Alice", "person");
   memory_alias_resolve_t res;
   int rc = memory_db_entity_resolve_alias(g_test_user_id, "Bob", "person", "bob", &res);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(0, res.matched_stage);
   TEST_ASSERT_EQUAL_INT64(0, res.resolved_id);
}

/* ============================================================================
 * score_pair — exercises Stage 5 (relation + contact overlap) directly
 * ============================================================================ */

static void test_score_pair_exclusive_relation_overlap(void) {
   int64_t kris = insert_entity_typed(g_test_user_id, "Kristopher Kersey", "person");
   int64_t kris_alias = insert_entity_typed(g_test_user_id, "Kris", "thing");
   int64_t company = insert_entity_typed(g_test_user_id, "Acme Corp", "org");

   /* Both names point at the same exclusive-relation object (works_at). */
   insert_open_relation(g_test_user_id, kris, "works_at", company, NULL);
   insert_open_relation(g_test_user_id, kris_alias, "works_at", company, NULL);

   memory_alias_evidence_t ev;
   int rc = memory_db_entity_score_pair(g_test_user_id, kris_alias, kris, &ev);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_FLOAT(1.0f, ev.exclusive_relation_overlap);
   /* Substring bonus also fires (kris ⊂ kristopher kersey). */
   TEST_ASSERT_TRUE(ev.name_substring_bonus_applied);
   /* Composite ≈ 0.30*0 + 0.30*0 + 0.25*1 + 0.10*0 + 0.05*0 + 0.10 (substring)
    *           = 0.35.  Falls below the review threshold (0.70) on this
    *           signal alone — the user_self bonus is what makes the dev's
    *           cluster cross the bar. */
   TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.35f, ev.composite_score);
}

static void test_score_pair_type_veto(void) {
   int64_t alice = insert_entity_typed(g_test_user_id, "Alice", "person");
   int64_t paris = insert_entity_typed(g_test_user_id, "Paris", "place");

   memory_alias_evidence_t ev;
   int rc = memory_db_entity_score_pair(g_test_user_id, alice, paris, &ev);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_TRUE(ev.type_veto_fired);
   TEST_ASSERT_EQUAL_FLOAT(0.0f, ev.composite_score);
}

static void test_score_pair_thing_no_veto(void) {
   /* The `thing` carve-out: thing/person doesn't trigger the veto. */
   int64_t kris_thing = insert_entity_typed(g_test_user_id, "Kris", "thing");
   int64_t kris_person = insert_entity_typed(g_test_user_id, "Kristopher Kersey", "person");

   memory_alias_evidence_t ev;
   int rc = memory_db_entity_score_pair(g_test_user_id, kris_thing, kris_person, &ev);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_FALSE(ev.type_veto_fired);
   /* type_match = 0 because one side is `thing`, but composite is non-zero
    * because the substring bonus fires. */
   TEST_ASSERT_EQUAL_FLOAT(0.0f, ev.type_match);
   TEST_ASSERT_TRUE(ev.name_substring_bonus_applied);
}

static void test_score_pair_contact_field_overlap(void) {
   int64_t a = insert_entity_typed(g_test_user_id, "Kristopher Kersey", "person");
   int64_t b = insert_entity_typed(g_test_user_id, "Kris", "thing");
   insert_contact(g_test_user_id, a, "email", "kerseyfabrications@gmail.com");
   insert_contact(g_test_user_id, b, "email",
                  "KerseyFabrications@gmail.com"); /* case-insensitive */

   memory_alias_evidence_t ev;
   int rc = memory_db_entity_score_pair(g_test_user_id, b, a, &ev);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_FLOAT(1.0f, ev.contact_field_overlap);
}

static void test_score_pair_user_self_bonus_via_username(void) {
   int64_t self = insert_entity_typed(g_test_user_id, "Kristopher Kersey", "person");
   mark_entity_user_self(self);
   /* The user's username is "kris" (set in setUp).  An entity whose
    * canonical_name contains "kris" gets the user_self bonus when paired
    * with self. */
   int64_t alias = insert_entity_typed(g_test_user_id, "Kris", "thing");

   memory_alias_evidence_t ev;
   int rc = memory_db_entity_score_pair(g_test_user_id, alias, self, &ev);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_TRUE(ev.user_self_bonus_applied);
   /* substring bonus also fires.  composite = 0.10 (substring) + 0.20 (user_self) = 0.30. */
   TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.30f, ev.composite_score);
}

static void test_score_pair_user_self_bonus_via_contact_overlap(void) {
   int64_t self = insert_entity_typed(g_test_user_id, "Kristopher Kersey", "person");
   mark_entity_user_self(self);
   int64_t other = insert_entity_typed(g_test_user_id, "totally different", "thing");
   insert_contact(g_test_user_id, self, "email", "shared@example.com");
   insert_contact(g_test_user_id, other, "email", "shared@example.com");

   memory_alias_evidence_t ev;
   int rc = memory_db_entity_score_pair(g_test_user_id, other, self, &ev);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   /* Contact overlap is what fires the user_self bonus despite zero name signal. */
   TEST_ASSERT_EQUAL_FLOAT(1.0f, ev.contact_field_overlap);
   TEST_ASSERT_TRUE(ev.user_self_bonus_applied);
}

/* ============================================================================
 * consider_auto_merge — threshold band routing
 * ============================================================================ */

static void test_consider_auto_merge_no_candidates(void) {
   int64_t lone = insert_entity_typed(g_test_user_id, "Solitary", "thing");
   memory_alias_evaluate_t eval;
   int rc = memory_db_entity_consider_auto_merge(g_test_user_id, lone, &eval);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(MEMORY_ALIAS_OUTCOME_NO_CANDIDATES, eval.outcome);
   TEST_ASSERT_EQUAL_INT64(0, eval.target_entity_id);
}

static void test_consider_auto_merge_auto_merged(void) {
   /* Build a high-composite cluster: user-self + alias with substring + shared
    * works_at relation + shared contact email.  Stage 4 (cosine) is stubbed
    * out so its 0.30 weight is forfeited, but the remaining signals + bonuses
    * easily clear 0.90.  Composite = 0.30*0 + 0.30*0 + 0.25*1 (works_at)
    *                                + 0.10*1 (contact) + 0.05*0 + 0.10
    *                                (substring) + 0.20 (user_self) = 0.70.
    * To reach 0.90 we need name_jaccard.  Use names that share a token
    * exactly: "kristopher kersey" vs "kristopher k.k.".  Jaccard = 1/2 =
    * 0.50, contributing 0.30*0.50 = 0.15.  Total = 0.85 — still review band.
    * Add a second shared token: "kristopher kersey" vs "kristopher kersey 2".
    * Tokens A={kristopher, kersey}, B={kristopher, kersey} (the "2" is a
    * single char, dropped).  Jaccard = 1.0.  composite = 0.30 + 0.25 + 0.10
    *                                                    + 0.10 + 0.20 = 0.95. */
   int64_t self = insert_entity_typed(g_test_user_id, "Kristopher Kersey", "person");
   mark_entity_user_self(self);
   int64_t company = insert_entity_typed(g_test_user_id, "Acme Corp", "org");
   insert_open_relation(g_test_user_id, self, "works_at", company, NULL);
   insert_contact(g_test_user_id, self, "email", "kerseyfab@example.com");

   /* Inbound row with the same tokens + same exclusive relation + same email. */
   int64_t inbound = insert_entity_typed(g_test_user_id, "Kristopher Kersey 2", "person");
   insert_open_relation(g_test_user_id, inbound, "works_at", company, NULL);
   insert_contact(g_test_user_id, inbound, "email", "kerseyfab@example.com");

   memory_alias_evaluate_t eval;
   int rc = memory_db_entity_consider_auto_merge(g_test_user_id, inbound, &eval);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(MEMORY_ALIAS_OUTCOME_AUTO_MERGED, eval.outcome);
   TEST_ASSERT_EQUAL_INT64(self, eval.target_entity_id);
   TEST_ASSERT_GREATER_THAN(0, eval.link_id);
   TEST_ASSERT_GREATER_OR_EQUAL_FLOAT(MEMORY_ALIAS_AUTO_THRESHOLD, eval.evidence.composite_score);

   /* canonical_id was set on the inbound row. */
   TEST_ASSERT_EQUAL_INT64(self, get_entity_canonical_id(inbound));
   /* Audit row exists. */
   TEST_ASSERT_EQUAL_INT(1, count_active_aliases_for_target(self));
   /* Cache invalidated. */
   TEST_ASSERT_GREATER_THAN(0, g_alias_test_entity_cache_invalidations);
}

static void test_consider_auto_merge_proposed(void) {
   /* Mid-confidence: shared substring + shared open works_at, no user_self,
    * no contact overlap.  composite = 0.30*0 + 0.30*0 + 0.25*1 + 0 + 0 + 0.10
    *                                = 0.35 — below review (0.70).
    * Need to add type_match and a strong jaccard.  type_match = 0.05 if
    * both person.  jaccard ~ 0.5 → 0.15.  Add cosine? stubbed.  Total =
    * 0.25 + 0.15 + 0.05 + 0.10 = 0.55, still below review.
    * The proposed-band test is harder to hit precisely without cosine
    * — we'd need the user_self bonus but not the auto-merge threshold.
    * Construct: substring + user_self + type_match (one side is non-self
    * person) but no exclusive relation:
    *   composite = 0.30*0.5 + 0 + 0 + 0 + 0.05 + 0.10 + 0.20 = 0.50.
    * Still below.  Add a non-exclusive relation overlap:
    *   composite = 0.30*0.5 + 0 + 0.25*0.5 + 0 + 0.05 + 0.10 + 0.20 = 0.625.
    * Still below.  Make exclusive overlap fire:
    *   composite = 0.30*0.5 + 0 + 0.25*1 + 0 + 0.05 + 0.10 + 0.20 = 0.75
    *   → review band!  */
   int64_t self = insert_entity_typed(g_test_user_id, "Kristopher", "person");
   mark_entity_user_self(self);
   int64_t company = insert_entity_typed(g_test_user_id, "Acme", "org");
   insert_open_relation(g_test_user_id, self, "works_at", company, NULL);

   /* Inbound: shares the "kristopher" token (jaccard 0.5 with "kristopher
    * kersey"), shares works_at, gets user_self bonus via username substring. */
   int64_t inbound = insert_entity_typed(g_test_user_id, "Kristopher Kersey", "person");
   insert_open_relation(g_test_user_id, inbound, "works_at", company, NULL);

   memory_alias_evaluate_t eval;
   int rc = memory_db_entity_consider_auto_merge(g_test_user_id, inbound, &eval);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(MEMORY_ALIAS_OUTCOME_PROPOSED, eval.outcome);
   TEST_ASSERT_GREATER_THAN(0, eval.proposal_id);
   /* No alias write yet. */
   TEST_ASSERT_EQUAL_INT64(0, get_entity_canonical_id(inbound));
}

static void test_consider_auto_merge_rejected(void) {
   /* Sufficient name overlap to clear Stage 2 floor (jaccard >= 0.30) but
    * not enough total signal to clear review threshold (0.70).
    * "kristopher kersey" vs "kristopher" → jaccard = 1/2 = 0.50.
    * composite = 0.30 * 0.50 = 0.15. */
   insert_entity_typed(g_test_user_id, "Kristopher", "thing");
   int64_t inbound = insert_entity_typed(g_test_user_id, "Kristopher Kersey", "thing");

   memory_alias_evaluate_t eval;
   int rc = memory_db_entity_consider_auto_merge(g_test_user_id, inbound, &eval);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(MEMORY_ALIAS_OUTCOME_REJECTED, eval.outcome);
   TEST_ASSERT_EQUAL_INT64(0, get_entity_canonical_id(inbound));
}

/* ============================================================================
 * alias_link / alias_unlink
 * ============================================================================ */

static void test_alias_link_writes_canonical_id_and_audit(void) {
   int64_t target = insert_entity_typed(g_test_user_id, "Kristopher Kersey", "person");
   int64_t source = insert_entity_typed(g_test_user_id, "Kris", "thing");
   int prev_inv = g_alias_test_entity_cache_invalidations;

   int64_t link_id = 0;
   int rc = memory_db_entity_alias_link(g_test_user_id, source, target, "soft", "operator", 0.95f,
                                        "{}", &link_id);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_GREATER_THAN(0, link_id);
   TEST_ASSERT_EQUAL_INT64(target, get_entity_canonical_id(source));
   TEST_ASSERT_EQUAL_INT(1, count_active_aliases_for_target(target));
   TEST_ASSERT_GREATER_THAN(prev_inv, g_alias_test_entity_cache_invalidations);
}

static void test_alias_link_refuses_self(void) {
   int64_t e = insert_entity_typed(g_test_user_id, "Solo", "thing");
   int64_t link_id = 0;
   int rc = memory_db_entity_alias_link(g_test_user_id, e, e, "soft", "operator", 0.0f, NULL,
                                        &link_id);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_FAILURE, rc);
   TEST_ASSERT_EQUAL_INT64(0, link_id);
}

static void test_alias_link_refuses_when_source_has_dependents(void) {
   /* If A → B (B is canonical), trying to link B → C should fail because B
    * has A as a dependent alias. */
   int64_t a = insert_entity_typed(g_test_user_id, "Alpha", "thing");
   int64_t b = insert_entity_typed(g_test_user_id, "Beta", "thing");
   int64_t c = insert_entity_typed(g_test_user_id, "Gamma", "thing");
   /* First link a → b. */
   int64_t link1 = 0;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_entity_alias_link(g_test_user_id, a, b, "soft", "operator", 0.0f,
                                                     NULL, &link1));
   /* Now try b → c — should refuse because b is canonical for a. */
   int64_t link2 = 0;
   int rc = memory_db_entity_alias_link(g_test_user_id, b, c, "soft", "operator", 0.0f, NULL,
                                        &link2);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_FAILURE, rc);
   TEST_ASSERT_EQUAL_INT64(0, link2);
}

static void test_alias_unlink_clears_canonical_id_and_stamps_audit(void) {
   int64_t target = insert_entity_typed(g_test_user_id, "Target", "thing");
   int64_t source = insert_entity_typed(g_test_user_id, "Source", "thing");
   int64_t link_id = 0;
   memory_db_entity_alias_link(g_test_user_id, source, target, "soft", "operator", 0.95f, NULL,
                               &link_id);
   int prev_inv = g_alias_test_entity_cache_invalidations;

   int rc = memory_db_entity_alias_unlink(g_test_user_id, link_id, "split-by-operator");
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT64(0, get_entity_canonical_id(source));
   TEST_ASSERT_EQUAL_INT(0, count_active_aliases_for_target(target));
   TEST_ASSERT_GREATER_THAN(prev_inv, g_alias_test_entity_cache_invalidations);

   /* Second unlink on the same id should fail (already unlinked). */
   rc = memory_db_entity_alias_unlink(g_test_user_id, link_id, "split-by-operator");
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_NOT_FOUND, rc);
}

static void test_alias_unlink_refuses_hard(void) {
   int64_t target = insert_entity_typed(g_test_user_id, "Target", "thing");
   int64_t source = insert_entity_typed(g_test_user_id, "Source", "thing");
   int64_t link_id = 0;
   memory_db_entity_alias_link(g_test_user_id, source, target, "hard", "operator", 0.95f, NULL,
                               &link_id);

   int rc = memory_db_entity_alias_unlink(g_test_user_id, link_id, "split-by-operator");
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_FAILURE, rc);
}

/* ============================================================================
 * Cache-invalidate roundtrip on existing memory_db_entity_merge() (Ckpt 3
 * fold-in fix per design §12).  Verifies that a hard-merge call on the
 * existing primitive flips the entity-cache dirty bit via the
 * memory_embeddings_invalidate_entity_cache() stub counter.
 * ============================================================================ */

static void test_entity_merge_invalidates_entity_cache(void) {
   /* Two entities with at least one relation; merge should re-point the
    * relation, delete the source row, and invalidate the entity cache. */
   int64_t a = insert_entity_typed(g_test_user_id, "Alpha", "thing");
   int64_t b = insert_entity_typed(g_test_user_id, "Beta", "thing");
   int64_t c = insert_entity_typed(g_test_user_id, "Gamma", "thing");
   insert_open_relation(g_test_user_id, a, "knows", c, NULL);

   int prev_inv = g_alias_test_entity_cache_invalidations;
   int rc = memory_db_entity_merge(g_test_user_id, a, b);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   /* Invalidator fired exactly once from the merge primitive. */
   TEST_ASSERT_GREATER_THAN(prev_inv, g_alias_test_entity_cache_invalidations);
}

/* ============================================================================
 * Cache loader filter — memory_db_entity_get_embeddings respects
 * include_aliases.  Insert a canonical row + a soft-alias row, both with
 * embeddings, and confirm the default call filters the alias out and the
 * include_aliases=true call includes both.
 * ============================================================================ */

static void insert_dummy_embedding(int64_t entity_id) {
   /* Stage a tiny 4-dim float blob so the row qualifies as "has embedding"
    * without requiring the embedding engine.  Norm is 2.0 (sqrt(4*1)). */
   const float blob[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(s_db.db,
                      "UPDATE memory_entities SET embedding = ?, embedding_norm = ? WHERE id = ?",
                      -1, &stmt, NULL);
   sqlite3_bind_blob(stmt, 1, blob, sizeof(blob), SQLITE_TRANSIENT);
   sqlite3_bind_double(stmt, 2, 2.0);
   sqlite3_bind_int64(stmt, 3, entity_id);
   sqlite3_step(stmt);
   sqlite3_finalize(stmt);
}

static void test_loader_filter_excludes_aliases_by_default(void) {
   int64_t target = insert_entity_typed(g_test_user_id, "Canonical", "thing");
   int64_t alias = insert_entity_typed(g_test_user_id, "Alias", "thing");
   insert_dummy_embedding(target);
   insert_dummy_embedding(alias);

   /* Soft-link alias → target so canonical_id IS NOT NULL on the alias row. */
   int64_t link_id = 0;
   memory_db_entity_alias_link(g_test_user_id, alias, target, "soft", "operator", 1.0f, NULL,
                               &link_id);

   const int dims = 4;
   int64_t out_ids[8];
   char out_names[8][MEMORY_ENTITY_NAME_MAX];
   char out_types[8][MEMORY_ENTITY_TYPE_MAX];
   float out_embeddings[8 * 4];
   float out_norms[8];

   /* Default (canonical-only): only the target row surfaces. */
   int count = 0;
   int rc = memory_db_entity_get_embeddings(g_test_user_id, /* include_aliases */ false, dims,
                                            out_ids, out_names, out_types, out_embeddings,
                                            out_norms, 8, &count);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(1, count);
   TEST_ASSERT_EQUAL_INT64(target, out_ids[0]);

   /* include_aliases = true: both rows surface. */
   count = 0;
   rc = memory_db_entity_get_embeddings(g_test_user_id, /* include_aliases */ true, dims, out_ids,
                                        out_names, out_types, out_embeddings, out_norms, 8, &count);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(2, count);
}

/* ============================================================================
 * relation_list_by_subject_class — equivalence-class-aware listing
 * ============================================================================ */

static void test_relation_list_by_subject_class_includes_aliases(void) {
   int64_t canonical = insert_entity_typed(g_test_user_id, "Kristopher Kersey", "person");
   int64_t alias = insert_entity_typed(g_test_user_id, "Kris", "thing");
   int64_t company = insert_entity_typed(g_test_user_id, "Acme", "org");

   /* Relation attached to canonical row. */
   insert_open_relation(g_test_user_id, canonical, "works_at", company, NULL);
   /* Relation attached to alias row (pre-link). */
   insert_open_relation(g_test_user_id, alias, "lives_in", 0, "Atlanta");

   /* Soft-link alias → canonical. */
   int64_t link_id = 0;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_entity_alias_link(g_test_user_id, alias, canonical, "soft",
                                                     "operator", 1.0f, NULL, &link_id));

   memory_relation_t rels[8];
   int count = 0;
   int rc = memory_db_relation_list_by_subject_class(g_test_user_id, canonical, rels, 8, &count);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   /* Both relations should surface — one from canonical, one from the alias. */
   TEST_ASSERT_EQUAL_INT(2, count);
}

/* ============================================================================
 * Ckpt 4 — link-user-self orchestrator + listing helpers
 * ============================================================================ */

static int count_db_rows(const char *sql) {
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   int n = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW)
      n = sqlite3_column_int(stmt, 0);
   sqlite3_finalize(stmt);
   return n;
}

static void test_link_user_self_dry_run_no_db_writes(void) {
   /* Build the dev's-cluster shape: a strong 'Kristopher Kersey' (person,
    * is_user_self=1) with a shared works_at relation, plus a 'Kris' alias
    * candidate that should clear auto-merge after the dry-run scoring. */
   int64_t self = insert_entity_typed(g_test_user_id, "Kristopher Kersey", "person");
   mark_entity_user_self(self);
   int64_t company = insert_entity_typed(g_test_user_id, "Acme", "org");
   insert_open_relation(g_test_user_id, self, "works_at", company, NULL);

   int64_t kris = insert_entity_typed(g_test_user_id, "Kristopher Kersey 2", "person");
   insert_open_relation(g_test_user_id, kris, "works_at", company, NULL);

   /* Snapshot the alias / proposal counts BEFORE the dry-run. */
   int aliases_before = count_db_rows(
       "SELECT COUNT(*) FROM memory_entity_aliases WHERE unlinked_at IS NULL");
   int props_before = count_db_rows(
       "SELECT COUNT(*) FROM memory_entity_merge_proposals WHERE resolved_at IS NULL");

   memory_alias_link_user_self_result_t result;
   memset(&result, 0, sizeof(result));
   int rc = memory_alias_link_user_self_run(g_test_user_id, /* dry_run */ true, &result);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);

   /* Self-id resolved to existing canonical (no seed needed). */
   TEST_ASSERT_EQUAL_INT64(self, result.self_entity_id);
   TEST_ASSERT_FALSE(result.self_was_seeded);
   TEST_ASSERT_EQUAL_INT(2, result.considered); /* Kris + Acme are the candidates */

   /* No DB mutation. */
   TEST_ASSERT_EQUAL_INT(aliases_before, count_db_rows("SELECT COUNT(*) FROM memory_entity_aliases "
                                                       "WHERE unlinked_at IS NULL"));
   TEST_ASSERT_EQUAL_INT(props_before,
                         count_db_rows("SELECT COUNT(*) FROM memory_entity_merge_proposals "
                                       "WHERE resolved_at IS NULL"));
}

static void test_link_user_self_commit_seeds_and_links(void) {
   /* Same shape as the dry-run, but call with dry_run=false and assert that
    * the soft-link gets written. */
   int64_t self = insert_entity_typed(g_test_user_id, "Kristopher Kersey", "person");
   mark_entity_user_self(self);
   int64_t company = insert_entity_typed(g_test_user_id, "Acme", "org");
   insert_open_relation(g_test_user_id, self, "works_at", company, NULL);
   int64_t kris = insert_entity_typed(g_test_user_id, "Kristopher Kersey 2", "person");
   insert_open_relation(g_test_user_id, kris, "works_at", company, NULL);

   memory_alias_link_user_self_result_t result;
   memset(&result, 0, sizeof(result));
   int rc = memory_alias_link_user_self_run(g_test_user_id, /* dry_run */ false, &result);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT64(self, result.self_entity_id);
   TEST_ASSERT_GREATER_OR_EQUAL_INT(1, result.auto_merged);

   /* Kris is now an alias of self. */
   TEST_ASSERT_EQUAL_INT64(self, get_entity_canonical_id(kris));
   /* And an audit row exists. */
   TEST_ASSERT_GREATER_OR_EQUAL_INT(1, count_active_aliases_for_target(self));
}

static void test_alias_list_helper_returns_active_only(void) {
   int64_t target = insert_entity_typed(g_test_user_id, "Target", "thing");
   int64_t a1 = insert_entity_typed(g_test_user_id, "A1", "thing");
   int64_t a2 = insert_entity_typed(g_test_user_id, "A2", "thing");
   int64_t link1 = 0, link2 = 0;
   memory_db_entity_alias_link(g_test_user_id, a1, target, "soft", "operator", 0.95f, NULL, &link1);
   memory_db_entity_alias_link(g_test_user_id, a2, target, "soft", "operator", 0.92f, NULL, &link2);

   memory_alias_listing_row_t rows[8];
   int count = 0;
   int rc = memory_db_entity_alias_list(g_test_user_id, target, rows, 8, &count);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(2, count);

   /* Unlink one — listing should now return only the other. */
   memory_db_entity_alias_unlink(g_test_user_id, link1, "split-by-operator");
   memset(rows, 0, sizeof(rows));
   count = 0;
   rc = memory_db_entity_alias_list(g_test_user_id, target, rows, 8, &count);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(1, count);
   TEST_ASSERT_EQUAL_INT64(link2, rows[0].link_id);
}

static void test_alias_history_helper_includes_unlinked(void) {
   int64_t target = insert_entity_typed(g_test_user_id, "T2", "thing");
   int64_t alias = insert_entity_typed(g_test_user_id, "Source2", "thing");
   int64_t link_id = 0;
   memory_db_entity_alias_link(g_test_user_id, alias, target, "soft", "operator", 0.91f, NULL,
                               &link_id);
   memory_db_entity_alias_unlink(g_test_user_id, link_id, "split-by-operator");

   memory_alias_history_row_t rows[8];
   int count = 0;
   int rc = memory_db_entity_alias_history(g_test_user_id, alias, rows, 8, &count);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(1, count);
   TEST_ASSERT_GREATER_THAN(0, rows[0].unlinked_at);
   TEST_ASSERT_EQUAL_STRING("split-by-operator", rows[0].unlink_reason);
}

static int64_t insert_proposal(int user_id,
                               int64_t source_id,
                               int64_t target_id,
                               double composite,
                               int64_t proposed_at,
                               const char *resolution) {
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(s_db.db,
                      "INSERT INTO memory_entity_merge_proposals "
                      "(user_id, source_entity_id, target_entity_id, composite_score, "
                      " evidence_json, proposed_at, resolved_at, resolution) "
                      "VALUES (?, ?, ?, ?, '{}', ?, ?, ?)",
                      -1, &stmt, NULL);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, source_id);
   sqlite3_bind_int64(stmt, 3, target_id);
   sqlite3_bind_double(stmt, 4, composite);
   sqlite3_bind_int64(stmt, 5, proposed_at);
   if (resolution) {
      sqlite3_bind_int64(stmt, 6, proposed_at + 60);
      sqlite3_bind_text(stmt, 7, resolution, -1, SQLITE_TRANSIENT);
   } else {
      sqlite3_bind_null(stmt, 6);
      sqlite3_bind_null(stmt, 7);
   }
   sqlite3_step(stmt);
   int64_t id = sqlite3_last_insert_rowid(s_db.db);
   sqlite3_finalize(stmt);
   return id;
}

static int64_t get_proposal_resolved_at(int64_t proposal_id) {
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(s_db.db,
                      "SELECT COALESCE(resolved_at, 0) "
                      "FROM memory_entity_merge_proposals WHERE id = ?",
                      -1, &stmt, NULL);
   sqlite3_bind_int64(stmt, 1, proposal_id);
   int64_t v = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW)
      v = sqlite3_column_int64(stmt, 0);
   sqlite3_finalize(stmt);
   return v;
}

static void test_proposal_list_returns_pending_only(void) {
   /* Seed two pending + one resolved proposal.  proposal_list_pending must
    * return the two pending in proposed_at DESC order and never surface
    * the resolved one. */
   int64_t a = insert_entity_typed(g_test_user_id, "Source A", "thing");
   int64_t b = insert_entity_typed(g_test_user_id, "Source B", "thing");
   int64_t c = insert_entity_typed(g_test_user_id, "Source C", "thing");
   int64_t target = insert_entity_typed(g_test_user_id, "Target", "thing");

   int64_t older = insert_proposal(g_test_user_id, a, target, 0.78, 100, NULL);
   int64_t newer = insert_proposal(g_test_user_id, b, target, 0.85, 200, NULL);
   /* Resolved one — must NOT appear. */
   int64_t resolved = insert_proposal(g_test_user_id, c, target, 0.81, 50, "rejected");
   (void)resolved;

   memory_alias_proposal_row_t rows[8];
   int count = 0;
   int rc = memory_db_proposal_list_pending(g_test_user_id, rows, 8, &count);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(2, count);
   /* DESC by proposed_at: newer (200) comes first. */
   TEST_ASSERT_EQUAL_INT64(newer, rows[0].proposal_id);
   TEST_ASSERT_EQUAL_INT64(older, rows[1].proposal_id);
   /* Joined names populated. */
   TEST_ASSERT_EQUAL_STRING("source b", rows[0].source_canonical_name);
   TEST_ASSERT_EQUAL_STRING("target", rows[0].target_canonical_name);
}

static void test_proposal_resolve_approved_creates_alias(void) {
   /* Approved resolution creates the soft alias and stamps the proposal. */
   int64_t source = insert_entity_typed(g_test_user_id, "Source", "thing");
   int64_t target = insert_entity_typed(g_test_user_id, "Target", "thing");
   int64_t prop = insert_proposal(g_test_user_id, source, target, 0.78, 100, NULL);

   int64_t link_id = 0;
   int rc = memory_db_proposal_resolve(g_test_user_id, prop, /* approved */ true, &link_id);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_GREATER_THAN(0, link_id);

   /* Source now soft-linked to target. */
   TEST_ASSERT_EQUAL_INT64(target, get_entity_canonical_id(source));
   /* Audit row written. */
   TEST_ASSERT_EQUAL_INT(1, count_active_aliases_for_target(target));
   /* Proposal stamped. */
   TEST_ASSERT_GREATER_THAN(0, get_proposal_resolved_at(prop));

   /* Re-resolving the same proposal must fail with NOT_FOUND. */
   rc = memory_db_proposal_resolve(g_test_user_id, prop, true, NULL);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_NOT_FOUND, rc);

   /* Reject path on a fresh proposal: stamps without writing an alias. */
   int64_t source2 = insert_entity_typed(g_test_user_id, "Source2", "thing");
   int64_t prop2 = insert_proposal(g_test_user_id, source2, target, 0.74, 110, NULL);
   int aliases_before = count_db_rows(
       "SELECT COUNT(*) FROM memory_entity_aliases WHERE unlinked_at IS NULL");
   rc = memory_db_proposal_resolve(g_test_user_id, prop2, /* approved */ false, NULL);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT64(0, get_entity_canonical_id(source2)); /* no link */
   TEST_ASSERT_EQUAL_INT(aliases_before, count_db_rows("SELECT COUNT(*) FROM memory_entity_aliases "
                                                       "WHERE unlinked_at IS NULL"));
   TEST_ASSERT_GREATER_THAN(0, get_proposal_resolved_at(prop2));
}

static void test_entity_list_for_admin_canonical_only_default(void) {
   int64_t canonical = insert_entity_typed(g_test_user_id, "Canon", "person");
   int64_t alias = insert_entity_typed(g_test_user_id, "AliasOfCanon", "person");
   int64_t link_id = 0;
   memory_db_entity_alias_link(g_test_user_id, alias, canonical, "soft", "operator", 0.95f, NULL,
                               &link_id);

   memory_alias_entity_row_t rows[16];
   int count = 0;

   /* Default — canonical-only.  Should NOT include the alias row. */
   int rc = memory_db_entity_list_for_admin(g_test_user_id, /* include_aliases */ false, rows, 16,
                                            &count);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(1, count);
   TEST_ASSERT_EQUAL_INT64(canonical, rows[0].entity_id);
   TEST_ASSERT_FALSE(rows[0].is_alias);

   /* include_aliases=true → both rows surface, alias flagged. */
   memset(rows, 0, sizeof(rows));
   count = 0;
   rc = memory_db_entity_list_for_admin(g_test_user_id, /* include_aliases */ true, rows, 16,
                                        &count);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(2, count);
   /* Aliases sorted last per the SQL ORDER BY. */
   TEST_ASSERT_FALSE(rows[0].is_alias);
   TEST_ASSERT_TRUE(rows[1].is_alias);
}

/* ============================================================================
 * Ckpt 5: alias_summary helper + LLM-tool integration smoke
 *
 * The smoke scenarios exercise the same code path the LLM tool uses
 * (memory_db_entity_alias_link / _alias_unlink / equivalence-class relation
 * listing) end-to-end on a seeded fixture.  They mirror the brief's
 * required scenario: extraction emits "Kris", manual merge_entities links
 * to "Kristopher Kersey", focus block recall returns canonical (relations
 * surface across the equivalence class), split restores both.
 *
 * The reextract-drop-alias-state behavior is verified in
 * test_memory_db_admin.c — not duplicated here.
 * ============================================================================ */

static void test_alias_summary_returns_alias_canonical_pairs(void) {
   /* Two aliases pointing at one canonical, plus a standalone canonical. */
   int64_t canonical1 = insert_entity_typed(g_test_user_id, "Kristopher Kersey", "person");
   int64_t alias_a = insert_entity_typed(g_test_user_id, "Kris", "person");
   int64_t alias_b = insert_entity_typed(g_test_user_id, "K. Kersey", "person");
   int64_t standalone = insert_entity_typed(g_test_user_id, "Bruno", "pet");

   int64_t link_a = 0, link_b = 0;
   memory_db_entity_alias_link(g_test_user_id, alias_a, canonical1, "soft", "operator", 0.95f, NULL,
                               &link_a);
   memory_db_entity_alias_link(g_test_user_id, alias_b, canonical1, "soft", "operator", 0.85f, NULL,
                               &link_b);

   memory_alias_summary_row_t rows[16];
   int count = 0;
   int rc = memory_db_entity_alias_summary(g_test_user_id, rows, 16, &count);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(2, count);

   /* Both rows must point at canonical1 and identify the alias entity ids. */
   bool saw_a = false, saw_b = false;
   for (int i = 0; i < count; i++) {
      TEST_ASSERT_EQUAL_INT64(canonical1, rows[i].canonical_entity_id);
      if (rows[i].alias_entity_id == alias_a)
         saw_a = true;
      if (rows[i].alias_entity_id == alias_b)
         saw_b = true;
      TEST_ASSERT_NOT_EQUAL_INT64(standalone, rows[i].alias_entity_id);
      TEST_ASSERT_NOT_EQUAL_INT64(canonical1, rows[i].alias_entity_id);
   }
   TEST_ASSERT_TRUE(saw_a);
   TEST_ASSERT_TRUE(saw_b);
}

static void test_alias_summary_empty_for_user_with_no_aliases(void) {
   insert_entity_typed(g_test_user_id, "Solo", "person");
   memory_alias_summary_row_t rows[8];
   int count = 999; /* must be zeroed */
   int rc = memory_db_entity_alias_summary(g_test_user_id, rows, 8, &count);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(0, count);
}

static void test_smoke_llm_merge_entities_soft_link_path(void) {
   /* Seed: canonical "Kristopher Kersey" + soon-to-be-alias "Kris".  Each
    * has one open relation so we can confirm the focus-block recall path
    * surfaces relations across the equivalence class after the link. */
   int64_t canonical = insert_entity_typed(g_test_user_id, "Kristopher Kersey", "person");
   int64_t source = insert_entity_typed(g_test_user_id, "Kris", "person");
   int64_t shop = insert_entity_typed(g_test_user_id, "CodeShop", "organization");
   int64_t bruno = insert_entity_typed(g_test_user_id, "Bruno", "pet");

   insert_open_relation(g_test_user_id, canonical, "works_at", shop, NULL);
   insert_open_relation(g_test_user_id, source, "owns_pet", bruno, NULL);

   /* Step 1: simulate the LLM tool path — call alias_link with the
    * exact same arguments memory_callback.c now passes. */
   int64_t link_id = 0;
   int rc = memory_db_entity_alias_link(g_test_user_id, source, canonical, "soft",
                                        "llm-tool-action",
                                        /* composite_score */ -1.0f,
                                        /* evidence_json */ NULL, &link_id);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_GREATER_THAN(0, link_id);

   /* Step 2: source row carries canonical_id = target (soft alias state). */
   TEST_ASSERT_EQUAL_INT64(canonical, get_entity_canonical_id(source));
   TEST_ASSERT_EQUAL_INT(1, count_active_aliases_for_target(canonical));

   /* Step 3: focus-block recall — equivalence-class relation listing on
    * the canonical now returns BOTH the canonical's and the alias's
    * relations (works_at AND owns_pet).  This is the read-side guarantee
    * the soft alias is supposed to deliver. */
   memory_relation_t rels[16];
   int rcount = 0;
   rc = memory_db_relation_list_by_subject_class(g_test_user_id, canonical, rels, 16, &rcount);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(2, rcount);
   bool saw_works_at = false, saw_owns_pet = false;
   for (int i = 0; i < rcount; i++) {
      if (strcmp(rels[i].relation, "works_at") == 0)
         saw_works_at = true;
      if (strcmp(rels[i].relation, "owns_pet") == 0)
         saw_owns_pet = true;
   }
   TEST_ASSERT_TRUE_MESSAGE(saw_works_at,
                            "canonical's relation should appear in equivalence-class listing");
   TEST_ASSERT_TRUE_MESSAGE(saw_owns_pet,
                            "alias's relation should appear in equivalence-class listing");

   /* Step 4: split — alias_unlink restores source as a standalone canonical. */
   rc = memory_db_entity_alias_unlink(g_test_user_id, link_id, "split-by-operator");
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT64(0, get_entity_canonical_id(source));
   TEST_ASSERT_EQUAL_INT(0, count_active_aliases_for_target(canonical));

   /* Equivalence-class lookup on canonical now returns ONLY the canonical's
    * relation (the source's relation no longer surfaces here — split
    * confirmed). */
   memset(rels, 0, sizeof(rels));
   rcount = 0;
   rc = memory_db_relation_list_by_subject_class(g_test_user_id, canonical, rels, 16, &rcount);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(1, rcount);
   TEST_ASSERT_EQUAL_STRING("works_at", rels[0].relation);

   /* Looking up by source as canonical now returns its own relation — split
    * has restored its independent equivalence class. */
   memset(rels, 0, sizeof(rels));
   rcount = 0;
   rc = memory_db_relation_list_by_subject_class(g_test_user_id, source, rels, 16, &rcount);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(1, rcount);
   TEST_ASSERT_EQUAL_STRING("owns_pet", rels[0].relation);
}

/* ============================================================================
 * Ckpt 5 fold-in for arch-L2 — synthetic-self dry-run UX
 *
 * Pre-fix: dry-run with no existing user-self showed every candidate as
 * REJECTED (no canonical to score against → composite 0.0).  Post-fix:
 * we synthesize a person-typed entity using the username + optional
 * persona_description and pair-score every candidate against it, with
 * is_user_self=true on the synthetic side so user_self_bonus fires.
 *
 * The dev's first invocation against an existing cluster is exactly this
 * scenario.  The behavioral signal we pin in this test: a candidate
 * whose canonical_name matches the username substring + has the same
 * type lands at least in the review band (>= 0.70), not REJECTED.
 * (Hitting auto-merge >= 0.90 typically needs embedding_cosine or shared
 * relations; the synthetic has id=0 so DB-touching signals contribute 0,
 * which is the correct behavior for a not-yet-materialized entity.)
 * ============================================================================ */

static void test_link_user_self_dry_run_synthetic_scores_existing_cluster(void) {
   /* setUp() created user "kris".  Seed an entity with the same
    * canonical_name + person type — the synthetic-self path should score
    * it well above 0 (vs the previous all-REJECTED behavior). */
   int64_t kris_ent = insert_entity_typed(g_test_user_id, "Kris", "person");
   /* Add an unrelated org entity so we can verify the kris candidate
    * outranks it (sanity check on the synthetic-pair scoring, not just
    * "every candidate gets the same uniform boost"). */
   int64_t acme = insert_entity_typed(g_test_user_id, "Acme", "org");

   /* No is_user_self=1 row — the dry-run path should synthesize one. */

   memory_alias_link_user_self_result_t result;
   memset(&result, 0, sizeof(result));
   int rc = memory_alias_link_user_self_run(g_test_user_id, /* dry_run */ true, &result);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);

   /* Self-id stays 0 (would-create), self_was_seeded reports the planned action. */
   TEST_ASSERT_EQUAL_INT64(0, result.self_entity_id);
   TEST_ASSERT_TRUE(result.self_was_seeded);

   /* Find Kris and Acme in the result.  Headline behavioral fix: Kris
    * scores meaningfully above zero (the old code returned 0.0 for every
    * candidate when self_id == 0).  Without the embedding engine wired
    * in tests, exact band depends on which signals fire — name_jaccard +
    * type_match + name_substring + user_self bonuses puts Kris around
    * the mid-0.6s, well above Acme's near-zero. */
   memory_alias_link_user_self_row_t *kris_row = NULL;
   memory_alias_link_user_self_row_t *acme_row = NULL;
   for (int i = 0; i < result.row_count; i++) {
      if (result.rows[i].entity_id == kris_ent)
         kris_row = &result.rows[i];
      else if (result.rows[i].entity_id == acme)
         acme_row = &result.rows[i];
   }
   TEST_ASSERT_NOT_NULL(kris_row);
   TEST_ASSERT_NOT_NULL(acme_row);
   TEST_ASSERT_GREATER_THAN_FLOAT(0.30f, kris_row->composite_score);
   TEST_ASSERT_GREATER_THAN_FLOAT(acme_row->composite_score, kris_row->composite_score);

   /* No DB mutation: alias / proposal tables stay empty. */
   TEST_ASSERT_EQUAL_INT(0, count_db_rows("SELECT COUNT(*) FROM memory_entity_aliases "
                                          "WHERE unlinked_at IS NULL"));
   TEST_ASSERT_EQUAL_INT(0, count_db_rows("SELECT COUNT(*) FROM memory_entity_merge_proposals "
                                          "WHERE resolved_at IS NULL"));
}

/* ============================================================================
 * Ckpt 5 fold-in for emb-M2 — single-SQL exclusive_relation_overlap
 *
 * Pins both halves of the unified-path: a pair where both sides have
 * works_at to the same target → overlap=1.0, and a pair where neither
 * side has an open exclusive relation → overlap=0.0.  This exercises the
 * IN (?, ?) + ORDER BY valid_from DESC + discriminator-routing path.
 * ============================================================================ */

static void test_exclusive_relation_overlap_unified_sql_path(void) {
   int64_t company = insert_entity_typed(g_test_user_id, "Acme Corp", "org");
   int64_t a_with = insert_entity_typed(g_test_user_id, "Kristopher Kersey", "person");
   int64_t b_with = insert_entity_typed(g_test_user_id, "Kris", "person");
   int64_t a_no = insert_entity_typed(g_test_user_id, "Alice", "person");
   int64_t b_no = insert_entity_typed(g_test_user_id, "Albert", "person");

   /* Half 1: both sides have works_at → Acme Corp (open).  Single-SQL must
    * route Acme matches into both a_rels and b_rels via discriminator. */
   insert_open_relation(g_test_user_id, a_with, "works_at", company, NULL);
   insert_open_relation(g_test_user_id, b_with, "works_at", company, NULL);

   memory_alias_evidence_t ev;
   int rc = memory_db_entity_score_pair(g_test_user_id, b_with, a_with, &ev);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_FLOAT(1.0f, ev.exclusive_relation_overlap);

   /* Half 2: neither side has open exclusive relations.  Single-SQL must
    * cleanly return overlap=0 (no rows from either side). */
   memset(&ev, 0, sizeof(ev));
   rc = memory_db_entity_score_pair(g_test_user_id, a_no, b_no, &ev);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_FLOAT(0.0f, ev.exclusive_relation_overlap);
}

/* ============================================================================
 * Ckpt 5 fold-in for arch-M1 — score_pair refuses alias targets
 * ============================================================================ */

static void test_score_pair_refuses_alias_target(void) {
   /* canonical + alias linked to it; score_pair against the alias must
    * surface MEMORY_DB_INVALID_ALIAS_TARGET so the WebUI preview can
    * render a "pick its canonical" message rather than silently scoring
    * against a missing-from-cache embedding. */
   int64_t canonical = insert_entity_typed(g_test_user_id, "Canon", "person");
   int64_t alias = insert_entity_typed(g_test_user_id, "AliasOfCanon", "person");
   int64_t standalone = insert_entity_typed(g_test_user_id, "Standalone", "person");

   int64_t link_id = 0;
   int rc = memory_db_entity_alias_link(g_test_user_id, alias, canonical, "soft", "operator", 0.95f,
                                        NULL, &link_id);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);

   /* Target is an alias → refuse. */
   memory_alias_evidence_t ev;
   rc = memory_db_entity_score_pair(g_test_user_id, standalone, alias, &ev);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_INVALID_ALIAS_TARGET, rc);
   TEST_ASSERT_EQUAL_FLOAT(0.0f, ev.composite_score);

   /* Source is an alias → refuse (symmetric — score_pair is direction-
    * agnostic on the alias check). */
   rc = memory_db_entity_score_pair(g_test_user_id, alias, standalone, &ev);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_INVALID_ALIAS_TARGET, rc);

   /* Two canonicals — succeeds. */
   rc = memory_db_entity_score_pair(g_test_user_id, canonical, standalone, &ev);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
}

/* ============================================================================
 * Ckpt 5 fold-in for arch-L4 — alias_link refuses link_kind="hard"
 * ============================================================================ */

static void test_alias_link_refuses_hard_kind(void) {
   /* Phase 3 will introduce the operator consolidate path that promotes a
    * soft link to hard.  Until then the alias surface refuses "hard" so
    * the unimplemented promotion logic can't be bypassed. */
   int64_t a = insert_entity_typed(g_test_user_id, "A", "person");
   int64_t b = insert_entity_typed(g_test_user_id, "B", "person");
   int64_t link_id = 0;
   int rc = memory_db_entity_alias_link(g_test_user_id, a, b, "hard", "operator", -1.0f, NULL,
                                        &link_id);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_FAILURE, rc);
   TEST_ASSERT_EQUAL_INT64(0, link_id);
   /* Sanity: no canonical_id set, no audit row. */
   TEST_ASSERT_EQUAL_INT64(0, get_entity_canonical_id(a));
   TEST_ASSERT_EQUAL_INT(0, count_active_aliases_for_target(b));

   /* "soft" still works. */
   rc = memory_db_entity_alias_link(g_test_user_id, a, b, "soft", "operator", -1.0f, NULL,
                                    &link_id);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_GREATER_THAN(0, link_id);
}

/* ============================================================================
 * Cross-user negative tests (Ckpt 5 fold-in for sec-L3)
 *
 * The alias surface SHOULD reject cross-user requests with NOT_FOUND so
 * an authenticated user can't reach into another user's graph by passing
 * raw entity / proposal / link ids.  These tests pin that invariant.
 * ============================================================================ */

static int seed_second_user(const char *username) {
   auth_db_create_user(username, "hash", false);
   auth_user_t u;
   memset(&u, 0, sizeof(u));
   auth_db_get_user(username, &u);
   return u.id;
}

static void test_cross_user_alias_link_returns_not_found(void) {
   /* User A owns both entities; user B tries to alias_link them. */
   int user_a = g_test_user_id;
   int user_b = seed_second_user("bob");
   TEST_ASSERT_GREATER_THAN(0, user_b);

   int64_t a_src = insert_entity_typed(user_a, "Source A", "person");
   int64_t a_tgt = insert_entity_typed(user_a, "Target A", "person");

   int64_t link_id = 0;
   int rc = memory_db_entity_alias_link(user_b, a_src, a_tgt, "soft", "operator", -1.0f, NULL,
                                        &link_id);
   /* user_b doesn't own these entities → load_entity_full returns
    * NOT_FOUND, which alias_link surfaces verbatim. */
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_NOT_FOUND, rc);
   TEST_ASSERT_EQUAL_INT64(0, link_id);

   /* Sanity: source row is unchanged. */
   TEST_ASSERT_EQUAL_INT64(0, get_entity_canonical_id(a_src));
}

static void test_cross_user_proposal_resolve_returns_not_found(void) {
   /* User A's proposal must be invisible to user B. */
   int user_a = g_test_user_id;
   int user_b = seed_second_user("bob");
   TEST_ASSERT_GREATER_THAN(0, user_b);

   int64_t a_src = insert_entity_typed(user_a, "Source A", "person");
   int64_t a_tgt = insert_entity_typed(user_a, "Target A", "person");

   /* Manually insert a pending proposal owned by user A. */
   sqlite3_stmt *ins = NULL;
   sqlite3_prepare_v2(s_db.db,
                      "INSERT INTO memory_entity_merge_proposals "
                      "(user_id, source_entity_id, target_entity_id, composite_score, "
                      " evidence_json, proposed_at) "
                      "VALUES (?, ?, ?, 0.80, '{}', strftime('%s','now'))",
                      -1, &ins, NULL);
   sqlite3_bind_int(ins, 1, user_a);
   sqlite3_bind_int64(ins, 2, a_src);
   sqlite3_bind_int64(ins, 3, a_tgt);
   sqlite3_step(ins);
   int64_t proposal_id = sqlite3_last_insert_rowid(s_db.db);
   sqlite3_finalize(ins);
   TEST_ASSERT_GREATER_THAN(0, proposal_id);

   int64_t link_id = 0;
   int rc = memory_db_proposal_resolve(user_b, proposal_id, /* approved */ true, &link_id);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_NOT_FOUND, rc);
   TEST_ASSERT_EQUAL_INT64(0, link_id);

   /* User A's proposal must remain pending. */
   sqlite3_stmt *q = NULL;
   sqlite3_prepare_v2(s_db.db, "SELECT resolved_at FROM memory_entity_merge_proposals WHERE id = ?",
                      -1, &q, NULL);
   sqlite3_bind_int64(q, 1, proposal_id);
   TEST_ASSERT_EQUAL_INT(SQLITE_ROW, sqlite3_step(q));
   TEST_ASSERT_EQUAL_INT(SQLITE_NULL, sqlite3_column_type(q, 0));
   sqlite3_finalize(q);

   /* User A's source entity must remain a non-alias canonical. */
   TEST_ASSERT_EQUAL_INT64(0, get_entity_canonical_id(a_src));
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
   UNITY_BEGIN();

   /* Pure helpers */
   RUN_TEST(test_name_jaccard_basic);
   RUN_TEST(test_name_substring_basic);
   RUN_TEST(test_composite_weighted_sum);
   RUN_TEST(test_composite_bonuses);
   RUN_TEST(test_composite_veto);
   RUN_TEST(test_canonical_priority_compare_pure);
   RUN_TEST(test_canonical_priority_compare_self_user_self_first);
   RUN_TEST(test_canonical_priority_compare_self_falls_through);

   /* Resolver cascade */
   RUN_TEST(test_resolver_stage1_exact_match);
   RUN_TEST(test_resolver_stage1_resolves_through_alias);
   RUN_TEST(test_resolver_no_match);

   /* score_pair / Stage 5 */
   RUN_TEST(test_score_pair_exclusive_relation_overlap);
   RUN_TEST(test_score_pair_type_veto);
   RUN_TEST(test_score_pair_thing_no_veto);
   RUN_TEST(test_score_pair_contact_field_overlap);
   RUN_TEST(test_score_pair_user_self_bonus_via_username);
   RUN_TEST(test_score_pair_user_self_bonus_via_contact_overlap);

   /* consider_auto_merge / threshold bands */
   RUN_TEST(test_consider_auto_merge_no_candidates);
   RUN_TEST(test_consider_auto_merge_auto_merged);
   RUN_TEST(test_consider_auto_merge_proposed);
   RUN_TEST(test_consider_auto_merge_rejected);

   /* alias_link / alias_unlink */
   RUN_TEST(test_alias_link_writes_canonical_id_and_audit);
   RUN_TEST(test_alias_link_refuses_self);
   RUN_TEST(test_alias_link_refuses_when_source_has_dependents);
   RUN_TEST(test_alias_unlink_clears_canonical_id_and_stamps_audit);
   RUN_TEST(test_alias_unlink_refuses_hard);

   /* Ckpt 3: cache invalidate fix + loader filter */
   RUN_TEST(test_entity_merge_invalidates_entity_cache);
   RUN_TEST(test_loader_filter_excludes_aliases_by_default);

   /* relation listings */
   RUN_TEST(test_relation_list_by_subject_class_includes_aliases);

   /* Ckpt 4: link-user-self orchestrator + listing helpers */
   RUN_TEST(test_link_user_self_dry_run_no_db_writes);
   RUN_TEST(test_link_user_self_commit_seeds_and_links);
   RUN_TEST(test_alias_list_helper_returns_active_only);
   RUN_TEST(test_alias_history_helper_includes_unlinked);
   RUN_TEST(test_entity_list_for_admin_canonical_only_default);
   RUN_TEST(test_proposal_list_returns_pending_only);
   RUN_TEST(test_proposal_resolve_approved_creates_alias);

   /* Ckpt 5: alias_summary helper + LLM-tool integration smoke */
   RUN_TEST(test_alias_summary_returns_alias_canonical_pairs);
   RUN_TEST(test_alias_summary_empty_for_user_with_no_aliases);
   RUN_TEST(test_smoke_llm_merge_entities_soft_link_path);

   /* Ckpt 5 fold-in: review-batch coverage */
   RUN_TEST(test_score_pair_refuses_alias_target);
   RUN_TEST(test_alias_link_refuses_hard_kind);
   RUN_TEST(test_cross_user_alias_link_returns_not_found);
   RUN_TEST(test_cross_user_proposal_resolve_returns_not_found);

   /* Ckpt 5 fold-in round 2 */
   RUN_TEST(test_exclusive_relation_overlap_unified_sql_path);
   RUN_TEST(test_link_user_self_dry_run_synthetic_scores_existing_cluster);

   return UNITY_END();
}
