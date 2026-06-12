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
 * Lifecycle tests for the memory→note bridge (Phase 9).  Drives the bridge
 * against a real in-memory v61 schema via auth_db_init.  The embedding engine
 * is stubbed (no-op), so these exercise the fact/note_doc_id lifecycle, not
 * cosine retrieval (that is covered end-to-end by the live 809 re-run): gloss
 * create + note_doc_id link, delete, rename-replaces (delete+create, no
 * duplicate), and two-notes-two-glosses, plus the memory-disabled no-op.
 */

#include <sqlite3.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "auth/auth_db.h"
#define AUTH_DB_INTERNAL_ALLOWED /* test seeds documents rows via the shared s_db handle */
#include "auth/auth_db_internal.h"
#include "config/dawn_config.h"
#include "dawn_error.h"
#include "memory/memory_db.h"
#include "memory/memory_note_bridge.h"
#include "memory/memory_stem.h"
#include "memory/memory_types.h"
#include "unity.h"

/* Single g_config definition for the linked auth/memory chain. */
dawn_config_t g_config;

/* Embedding-engine stubs — the bridge calls embed_and_store; memory_db_facts
 * calls invalidate_cache.  No-ops keep the lifecycle tests DB-only. */
int memory_embeddings_embed_and_store(int user_id, int64_t fact_id, const char *text) {
   (void)user_id;
   (void)fact_id;
   (void)text;
   return SUCCESS;
}
void memory_embeddings_invalidate_cache(void) {
}
void memory_embeddings_invalidate_all(void) {
}
void memory_embeddings_invalidate_entity_cache(void) {
}
float memory_embeddings_l2_norm(const float *vec, int dims) {
   (void)vec;
   (void)dims;
   return 0.0f;
}
float memory_embeddings_cosine_with_norms(const float *a,
                                          const float *b,
                                          int dims,
                                          float na,
                                          float nb) {
   (void)a;
   (void)b;
   (void)dims;
   (void)na;
   (void)nb;
   return 0.0f;
}

void setUp(void) {
   memset(&g_config, 0, sizeof(g_config));
   g_config.memory.enabled = true;
   auth_db_init(":memory:");
   memory_stem_init();
   auth_db_create_user("tester", "hash", true); /* user id 1 */
   auth_db_create_user("other", "hash", false); /* user id 2 */
}

void tearDown(void) {
   auth_db_shutdown();
}

/* Seed a minimal documents row so note_doc_id's FK (foreign_keys=ON) is
 * satisfiable — in production the note always exists before the gloss links. */
static void seed_doc(int64_t doc_id) {
   char sql[256];
   snprintf(sql, sizeof(sql),
            "INSERT INTO documents (id, user_id, filename, filepath, filetype, file_hash, "
            "num_chunks, created_at) VALUES (%lld, 1, 'note', 'note', 'note', 'h', 1, 0)",
            (long long)doc_id);
   char *err = NULL;
   if (sqlite3_exec(s_db.db, sql, NULL, NULL, &err) != SQLITE_OK) {
      TEST_FAIL_MESSAGE(err ? err : "seed_doc failed");
   }
}

static bool gloss_text_of(int64_t fact_id, char *out, size_t out_len) {
   memory_fact_t f;
   if (memory_db_fact_get(fact_id, 1, &f) != MEMORY_DB_SUCCESS)
      return false;
   snprintf(out, out_len, "%s", f.fact_text);
   return true;
}

/* ---- tests ------------------------------------------------------------- */

void test_upsert_creates_gloss_linked_to_note(void) {
   seed_doc(100);
   TEST_ASSERT_EQUAL_INT(SUCCESS, memory_note_bridge_upsert_gloss(1, 100, "Public Bio"));

   int64_t fact_id = 0;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_fact_find_by_note_doc_id(1, 100, &fact_id));
   TEST_ASSERT_GREATER_THAN(0, fact_id);

   char text[MEMORY_FACT_TEXT_MAX];
   TEST_ASSERT_TRUE(gloss_text_of(fact_id, text, sizeof(text)));
   TEST_ASSERT_NOT_NULL(strstr(text, "Public Bio"));
   TEST_ASSERT_NOT_NULL(strstr(text, "document_read"));
}

void test_delete_removes_gloss(void) {
   seed_doc(100);
   TEST_ASSERT_EQUAL_INT(SUCCESS, memory_note_bridge_upsert_gloss(1, 100, "Public Bio"));
   int64_t before = 0;
   memory_db_fact_find_by_note_doc_id(1, 100, &before);
   TEST_ASSERT_GREATER_THAN(0, before);

   TEST_ASSERT_EQUAL_INT(SUCCESS, memory_note_bridge_delete_gloss(1, 100));

   int64_t after = 0;
   memory_db_fact_find_by_note_doc_id(1, 100, &after);
   TEST_ASSERT_EQUAL_INT64(0, after);
   /* old fact row is gone, not just unlinked */
   memory_fact_t f;
   TEST_ASSERT_NOT_EQUAL(MEMORY_DB_SUCCESS, memory_db_fact_get(before, 1, &f));
}

void test_rename_replaces_gloss_no_duplicate(void) {
   seed_doc(100);
   TEST_ASSERT_EQUAL_INT(SUCCESS, memory_note_bridge_upsert_gloss(1, 100, "Public Bio"));
   int64_t first = 0;
   memory_db_fact_find_by_note_doc_id(1, 100, &first);
   TEST_ASSERT_GREATER_THAN(0, first);

   /* Rename: upsert again for the SAME note with a new label. */
   TEST_ASSERT_EQUAL_INT(SUCCESS, memory_note_bridge_upsert_gloss(1, 100, "Dragon Con Bio"));
   int64_t second = 0;
   memory_db_fact_find_by_note_doc_id(1, 100, &second);
   TEST_ASSERT_GREATER_THAN(0, second);

   /* Fresh row (delete+create), old one gone → exactly one gloss for the note. */
   TEST_ASSERT_NOT_EQUAL(first, second);
   memory_fact_t old;
   TEST_ASSERT_NOT_EQUAL(MEMORY_DB_SUCCESS, memory_db_fact_get(first, 1, &old));

   char text[MEMORY_FACT_TEXT_MAX];
   TEST_ASSERT_TRUE(gloss_text_of(second, text, sizeof(text)));
   TEST_ASSERT_NOT_NULL(strstr(text, "Dragon Con Bio"));
   TEST_ASSERT_NULL(strstr(text, "Public Bio"));
}

void test_two_notes_two_distinct_glosses(void) {
   seed_doc(100);
   seed_doc(200);
   TEST_ASSERT_EQUAL_INT(SUCCESS, memory_note_bridge_upsert_gloss(1, 100, "Public Bio"));
   TEST_ASSERT_EQUAL_INT(SUCCESS, memory_note_bridge_upsert_gloss(1, 200, "Dragon Con Bio"));

   int64_t a = 0, b = 0;
   memory_db_fact_find_by_note_doc_id(1, 100, &a);
   memory_db_fact_find_by_note_doc_id(1, 200, &b);
   TEST_ASSERT_GREATER_THAN(0, a);
   TEST_ASSERT_GREATER_THAN(0, b);
   TEST_ASSERT_NOT_EQUAL(a, b);
}

/* A gloss must not be able to link to another user's note (the setter's
 * ownership guard).  Seed a note owned by user 2, then have user 1 try to
 * bridge it: the upsert must fail and leave no orphan gloss for user 1. */
void test_cross_user_note_link_refused(void) {
   /* documents row owned by user 2 */
   char *err = NULL;
   if (sqlite3_exec(s_db.db,
                    "INSERT INTO documents (id, user_id, filename, filepath, filetype, file_hash, "
                    "num_chunks, created_at) VALUES (300, 2, 'note', 'note', 'note', 'h', 1, 0)",
                    NULL, NULL, &err) != SQLITE_OK) {
      TEST_FAIL_MESSAGE(err ? err : "seed failed");
   }

   TEST_ASSERT_EQUAL_INT(FAILURE, memory_note_bridge_upsert_gloss(1, 300, "Public Bio"));

   int64_t fact_id = 0;
   memory_db_fact_find_by_note_doc_id(1, 300, &fact_id);
   TEST_ASSERT_EQUAL_INT64(0, fact_id); /* no orphan gloss left behind */
}

void test_disabled_memory_is_noop(void) {
   g_config.memory.enabled = false;
   TEST_ASSERT_EQUAL_INT(SUCCESS, memory_note_bridge_upsert_gloss(1, 100, "Public Bio"));
   int64_t fact_id = 0;
   memory_db_fact_find_by_note_doc_id(1, 100, &fact_id);
   TEST_ASSERT_EQUAL_INT64(0, fact_id); /* nothing created */
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_upsert_creates_gloss_linked_to_note);
   RUN_TEST(test_delete_removes_gloss);
   RUN_TEST(test_rename_replaces_gloss_no_duplicate);
   RUN_TEST(test_two_notes_two_distinct_glosses);
   RUN_TEST(test_cross_user_note_link_refused);
   RUN_TEST(test_disabled_memory_is_noop);
   return UNITY_END();
}
