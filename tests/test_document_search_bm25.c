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
 * v61 document lexical (BM25) search — the "bios" search-quality test.
 *
 * Reproduces the core of conversation 809: three near-twin reference notes
 * stored under distinct labels.  Asserts the column-weighted BM25 lexical
 * channel (document_db_chunk_search_bm25) returns the EXACT-label match rank-1,
 * so a label query is not buried under its semantic neighbors.  The lexical
 * channel uses no embeddings, so this needs only the real auth_db + FTS index.
 */

#include <string.h>

#include "auth/auth_db.h"
#include "dawn_error.h"
#include "memory/memory_stem.h"
#include "memory/memory_types.h" /* MEMORY_FACT_STEMS_MAX */
#include "tools/document_db.h"
#include "unity.h"

/* Match the production BM25 column-weight defaults (config_defaults.c). */
#define TEST_BM25_LABEL_WEIGHT 3.0f
#define TEST_BM25_BODY_WEIGHT 1.0f

/* Seed one single-chunk "note": a document whose filename IS the label, plus its
 * one chunk indexed into document_chunks_fts (label + body stemmed). */
static int64_t seed_note(int user_id, const char *label, const char *body, int idx) {
   char hash[65];
   snprintf(hash, sizeof(hash), "hash_%d", idx);
   int64_t doc_id = 0;
   TEST_ASSERT_EQUAL_INT(SUCCESS, document_db_create(user_id, label, label, "note", hash, 1, false,
                                                     &doc_id));

   float emb[4] = { 0.5f, 0.5f, 0.5f, 0.5f };
   int64_t chunk_id = 0;
   TEST_ASSERT_EQUAL_INT(SUCCESS,
                         document_db_chunk_create(doc_id, 0, body, emb, 4, 1.0f, 0, &chunk_id));

   char label_stems[MEMORY_FACT_STEMS_MAX];
   char body_stems[MEMORY_FACT_STEMS_MAX];
   memory_stem_string(label, label_stems, sizeof(label_stems));
   memory_stem_string(body, body_stems, sizeof(body_stems));
   TEST_ASSERT_EQUAL_INT(SUCCESS, document_db_chunk_index_fts(chunk_id, label_stems, body_stems));
   return doc_id;
}

void setUp(void) {
   auth_db_init(":memory:");
   memory_stem_init();
   auth_db_create_user("tester", "hash", true); /* user id 1 */

   seed_note(1, "Public Bio",
             "Jon Smith is a passionate maker and cosplayer who builds the things that inspire "
             "him from science fiction.",
             1);
   seed_note(1, "Dragon Con Bio",
             "Jon Smith is a passionate maker and cosplayer who shares his knowledge on 3D "
             "printing through his YouTube channels.",
             2);
   seed_note(1, "Open Sauce About You",
             "Jon Smith is an embedded systems engineer developing open-source home and wearable "
             "technology.",
             3);
}

void tearDown(void) {
   auth_db_shutdown();
}

/* The exact-label query "public bio" must return the Public Bio note rank-1 —
 * its label matches BOTH query terms, while the near-twins match only "bio" on
 * the (heavily weighted) label column. */
void test_exact_label_ranks_first(void) {
   doc_bm25_hit_t hits[10];
   float scores[10];
   int count = 0;
   TEST_ASSERT_EQUAL_INT(
       SUCCESS, document_db_chunk_search_bm25(1, "public bio", TEST_BM25_LABEL_WEIGHT,
                                              TEST_BM25_BODY_WEIGHT, hits, scores, 10, &count));
   TEST_ASSERT_GREATER_THAN_INT(0, count);
   TEST_ASSERT_EQUAL_STRING("Public Bio", hits[0].filename);
   TEST_ASSERT_TRUE(scores[0] > 0.0f);
}

/* A label query for a different note returns that note rank-1 — confirms the
 * label channel discriminates between the near-twins rather than collapsing. */
void test_other_label_ranks_first(void) {
   doc_bm25_hit_t hits[10];
   float scores[10];
   int count = 0;
   TEST_ASSERT_EQUAL_INT(
       SUCCESS, document_db_chunk_search_bm25(1, "dragon con bio", TEST_BM25_LABEL_WEIGHT,
                                              TEST_BM25_BODY_WEIGHT, hits, scores, 10, &count));
   TEST_ASSERT_GREATER_THAN_INT(0, count);
   TEST_ASSERT_EQUAL_STRING("Dragon Con Bio", hits[0].filename);
}

/* A body-only term ("cosplayer") still retrieves via the body column even though
 * it is absent from every label. */
void test_body_term_retrieves(void) {
   doc_bm25_hit_t hits[10];
   float scores[10];
   int count = 0;
   TEST_ASSERT_EQUAL_INT(
       SUCCESS, document_db_chunk_search_bm25(1, "cosplayer", TEST_BM25_LABEL_WEIGHT,
                                              TEST_BM25_BODY_WEIGHT, hits, scores, 10, &count));
   TEST_ASSERT_GREATER_THAN_INT(0, count);
}

/* Resolve a note's document_id by an exact-label query (rank-1). */
static int64_t doc_id_for_label(const char *label_query) {
   doc_bm25_hit_t hits[10];
   float scores[10];
   int count = 0;
   document_db_chunk_search_bm25(1, label_query, TEST_BM25_LABEL_WEIGHT, TEST_BM25_BODY_WEIGHT,
                                 hits, scores, 10, &count);
   return count > 0 ? hits[0].document_id : 0;
}

/* M-5: a stable-id note edit keeps doc_id, swaps the FTS rows (new label
 * searchable, old label gone), and stays single-chunk. */
void test_note_update_stable_id(void) {
   int64_t doc_id = doc_id_for_label("public bio");
   TEST_ASSERT_GREATER_THAN_INT(0, doc_id);

   float emb[4] = { 0.5f, 0.5f, 0.5f, 0.5f };
   TEST_ASSERT_EQUAL_INT(SUCCESS, document_db_note_update(1, doc_id, "Public Profile",
                                                          "Jon Smith is an engineer and maker.",
                                                          emb, 4, 1.0f, "hash_updated"));

   /* doc_id stable + new label retrievable rank-1. */
   doc_bm25_hit_t hits[10];
   float scores[10];
   int count = 0;
   document_db_chunk_search_bm25(1, "public profile", TEST_BM25_LABEL_WEIGHT, TEST_BM25_BODY_WEIGHT,
                                 hits, scores, 10, &count);
   TEST_ASSERT_GREATER_THAN_INT(0, count);
   TEST_ASSERT_EQUAL_STRING("Public Profile", hits[0].filename);
   TEST_ASSERT_EQUAL_INT64(doc_id, hits[0].document_id);

   /* num_chunks still 1. */
   document_t doc;
   TEST_ASSERT_EQUAL_INT(SUCCESS, document_db_get(doc_id, &doc));
   TEST_ASSERT_EQUAL_INT(1, doc.num_chunks);

   /* Old label "Public Bio" no longer exists as a label. */
   count = 0;
   document_db_chunk_search_bm25(1, "public bio", TEST_BM25_LABEL_WEIGHT, TEST_BM25_BODY_WEIGHT,
                                 hits, scores, 10, &count);
   for (int i = 0; i < count; i++)
      TEST_ASSERT_NOT_EQUAL(0, strcmp(hits[i].filename, "Public Bio"));
}

/* delete_indexed removes the doc AND its FTS rows (no orphans): the deleted
 * label stops matching, siblings remain. */
void test_note_delete_indexed(void) {
   int64_t doc_id = doc_id_for_label("dragon con bio");
   TEST_ASSERT_GREATER_THAN_INT(0, doc_id);
   TEST_ASSERT_EQUAL_INT(SUCCESS, document_db_delete_indexed(doc_id));

   doc_bm25_hit_t hits[10];
   float scores[10];
   int count = 0;
   document_db_chunk_search_bm25(1, "dragon con bio", TEST_BM25_LABEL_WEIGHT, TEST_BM25_BODY_WEIGHT,
                                 hits, scores, 10, &count);
   for (int i = 0; i < count; i++)
      TEST_ASSERT_NOT_EQUAL(0, strcmp(hits[i].filename, "Dragon Con Bio"));

   /* A sibling note is untouched. */
   TEST_ASSERT_GREATER_THAN_INT(0, doc_id_for_label("public bio"));
}

/* M-5: exact-label routing must NOT match a substring — "Bio" must not resolve
 * to "Public Bio" (else a save/delete would clobber the wrong note). */
void test_find_by_label_exact(void) {
   document_t doc;
   /* Exact full label hits. */
   TEST_ASSERT_EQUAL_INT(SUCCESS, document_db_find_by_label_exact(1, "Public Bio", true, &doc));
   TEST_ASSERT_EQUAL_STRING("Public Bio", doc.filename);
   /* Case-insensitive. */
   TEST_ASSERT_EQUAL_INT(SUCCESS, document_db_find_by_label_exact(1, "public bio", true, &doc));
   /* Substring must NOT match. */
   TEST_ASSERT_EQUAL_INT(FAILURE, document_db_find_by_label_exact(1, "Bio", true, &doc));
   TEST_ASSERT_EQUAL_INT(FAILURE, document_db_find_by_label_exact(1, "Public", true, &doc));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_exact_label_ranks_first);
   RUN_TEST(test_other_label_ranks_first);
   RUN_TEST(test_body_term_retrieves);
   RUN_TEST(test_note_update_stable_id);
   RUN_TEST(test_note_delete_indexed);
   RUN_TEST(test_find_by_label_exact);
   return UNITY_END();
}
