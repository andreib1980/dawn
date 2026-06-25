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
 * Integration test for the document-original store + the generic blob engine's
 * document-only features (content-hash dedup, aggregate caps, orphan sweep) and
 * the v68 stmt_blob_* statements, against a real in-memory SQLite + temp dir.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "auth/auth_db.h"
#include "document_original_store.h"
#include "unity.h"

static char s_tmpdir[256];

void setUp(void) {
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(":memory:"));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_create_user("owner", "h", true));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_create_user("other", "h", false));

   snprintf(s_tmpdir, sizeof(s_tmpdir), "/tmp/dawn_docorigtest_XXXXXX");
   TEST_ASSERT_NOT_NULL(mkdtemp(s_tmpdir));

   documents_config_t cfg = { 0 };
   cfg.originals_enabled = true;
   cfg.original_retention_days = 0;
   cfg.max_original_size_mb = 1;
   cfg.max_originals_per_user = 5;
   cfg.max_originals_total_mb_per_user = 100;
   cfg.original_grace_minutes = 45;
   TEST_ASSERT_EQUAL_INT(BLOB_STORE_SUCCESS, document_originals_init(&cfg, s_tmpdir));
}

void tearDown(void) {
   document_originals_shutdown();
   blob_store_shutdown(); /* reset the shared registry between tests */
   auth_db_shutdown();
}

/* ---- save + round-trip --------------------------------------------------- */

static void test_save_roundtrip(void) {
   const char pdf[] = "%PDF-1.4 fake pdf bytes \x00\x01";
   char id[BLOB_ID_LEN] = { 0 };
   TEST_ASSERT_EQUAL_INT(BLOB_STORE_SUCCESS,
                         document_original_save(1, pdf, sizeof(pdf), "application/pdf", "hash_aaa",
                                                "MyReport.pdf", id));
   TEST_ASSERT_TRUE(document_original_validate_id(id));
   TEST_ASSERT_EQUAL_STRING_LEN("blb_", id, 4);

   char path[BLOB_PATH_MAX] = { 0 };
   char mime[BLOB_MIME_MAX] = { 0 };
   TEST_ASSERT_EQUAL_INT(BLOB_STORE_SUCCESS, document_original_get_path(id, 1, path, mime));
   TEST_ASSERT_EQUAL_STRING("application/pdf", mime);
   /* On-disk name uses the .pdf extension from the MIME map. */
   TEST_ASSERT_NOT_NULL(strstr(path, ".pdf"));

   FILE *f = fopen(path, "rb");
   TEST_ASSERT_NOT_NULL(f);
   char buf[64] = { 0 };
   size_t n = fread(buf, 1, sizeof(buf), f);
   fclose(f);
   TEST_ASSERT_EQUAL_size_t(sizeof(pdf), n);
   TEST_ASSERT_EQUAL_MEMORY(pdf, buf, sizeof(pdf));

   /* Original filename round-trips for the download Content-Disposition. */
   char fn[BLOB_FILENAME_ORIGINAL_MAX] = { 0 };
   TEST_ASSERT_EQUAL_INT(BLOB_STORE_SUCCESS, document_original_get_filename(id, 1, fn, sizeof(fn)));
   TEST_ASSERT_EQUAL_STRING("MyReport.pdf", fn);
}

/* ---- dedup --------------------------------------------------------------- */

static void test_dedup_same_bytes(void) {
   const char doc[] = "identical content";
   char id1[BLOB_ID_LEN] = { 0 }, id2[BLOB_ID_LEN] = { 0 };
   TEST_ASSERT_EQUAL_INT(BLOB_STORE_SUCCESS,
                         document_original_save(1, doc, sizeof(doc), "text/plain", "hash_dup",
                                                "a.txt", id1));
   /* Second save with the same (user, hash) returns the SAME id, no new row. */
   TEST_ASSERT_EQUAL_INT(BLOB_STORE_SUCCESS,
                         document_original_save(1, doc, sizeof(doc), "text/plain", "hash_dup",
                                                "a-again.txt", id2));
   TEST_ASSERT_EQUAL_STRING(id1, id2);

   int count = -1;
   TEST_ASSERT_EQUAL_INT(BLOB_STORE_SUCCESS, document_original_count_user(1, &count));
   TEST_ASSERT_EQUAL_INT(1, count);

   /* Same bytes for a DIFFERENT user are a distinct blob (per-user dedup). */
   char id3[BLOB_ID_LEN] = { 0 };
   TEST_ASSERT_EQUAL_INT(BLOB_STORE_SUCCESS,
                         document_original_save(2, doc, sizeof(doc), "text/plain", "hash_dup",
                                                "a.txt", id3));
   TEST_ASSERT_TRUE(strcmp(id1, id3) != 0);
}

/* ---- access control ------------------------------------------------------ */

static void test_owner_only(void) {
   char id[BLOB_ID_LEN] = { 0 };
   TEST_ASSERT_EQUAL_INT(BLOB_STORE_SUCCESS,
                         document_original_save(1, "x", 1, "application/pdf", "h1", "x.pdf", id));
   char path[BLOB_PATH_MAX];
   TEST_ASSERT_EQUAL_INT(BLOB_STORE_SUCCESS, document_original_get_path(id, 1, path, NULL));
   TEST_ASSERT_EQUAL_INT(BLOB_STORE_FORBIDDEN, document_original_get_path(id, 2, path, NULL));
   /* Service token (user_id 0) is denied — originals are private. */
   TEST_ASSERT_EQUAL_INT(BLOB_STORE_FORBIDDEN, document_original_get_path(id, 0, path, NULL));
}

/* ---- per-user count cap -------------------------------------------------- */

static void test_count_cap(void) {
   char id[BLOB_ID_LEN] = { 0 };
   for (int i = 0; i < 5; i++) {
      char hash[16];
      snprintf(hash, sizeof(hash), "h%d", i);
      TEST_ASSERT_EQUAL_INT(BLOB_STORE_SUCCESS,
                            document_original_save(1, &i, sizeof(i), "application/pdf", hash,
                                                   "f.pdf", id));
   }
   /* 6th distinct blob exceeds max_originals_per_user = 5. */
   int six = 99;
   TEST_ASSERT_EQUAL_INT(BLOB_STORE_LIMIT_EXCEEDED,
                         document_original_save(1, &six, sizeof(six), "application/pdf", "h_six",
                                                "f.pdf", id));
}

/* ---- account deletion purges rows + files -------------------------------- */

static void test_delete_user(void) {
   char id[BLOB_ID_LEN] = { 0 };
   TEST_ASSERT_EQUAL_INT(BLOB_STORE_SUCCESS, document_original_save(1, "abc", 3, "application/pdf",
                                                                    "h_du", "d.pdf", id));
   char path[BLOB_PATH_MAX];
   TEST_ASSERT_EQUAL_INT(BLOB_STORE_SUCCESS, document_original_get_path(id, 1, path, NULL));

   TEST_ASSERT_EQUAL_INT(BLOB_STORE_SUCCESS, document_original_delete_user(1));
   int count = -1;
   TEST_ASSERT_EQUAL_INT(BLOB_STORE_SUCCESS, document_original_count_user(1, &count));
   TEST_ASSERT_EQUAL_INT(0, count);
   TEST_ASSERT_NOT_EQUAL(0, access(path, F_OK)); /* file gone */
}

/* ---- orphan sweep -------------------------------------------------------- */

static void test_orphan_sweep(void) {
   /* A blob no document references is an orphan once past the grace window. */
   char id[BLOB_ID_LEN] = { 0 };
   TEST_ASSERT_EQUAL_INT(BLOB_STORE_SUCCESS,
                         document_original_save(1, "orphan", 6, "application/pdf", "h_orph",
                                                "o.pdf", id));
   char path[BLOB_PATH_MAX];
   TEST_ASSERT_EQUAL_INT(BLOB_STORE_SUCCESS, document_original_get_path(id, 1, path, NULL));

   /* Negative grace => cutoff in the future => the just-created blob is eligible. */
   int deleted = 0;
   TEST_ASSERT_EQUAL_INT(BLOB_STORE_SUCCESS, document_original_cleanup_orphans(-5, &deleted));
   TEST_ASSERT_EQUAL_INT(1, deleted);
   TEST_ASSERT_EQUAL_INT(BLOB_STORE_NOT_FOUND, document_original_get_path(id, 1, path, NULL));
   TEST_ASSERT_NOT_EQUAL(0, access(path, F_OK)); /* file reclaimed */
}

/* A blob still referenced by a conversation message (chat attachment marker) is
 * NOT an orphan, even though no documents row points at it. */
static void test_orphan_keeps_message_referenced(void) {
   char id[BLOB_ID_LEN] = { 0 };
   TEST_ASSERT_EQUAL_INT(BLOB_STORE_SUCCESS,
                         document_original_save(1, "chat doc", 8, "application/pdf", "h_chat",
                                                "c.pdf", id));
   /* Persist a conversation message embedding the blob marker, as the chat
    * attachment path does. */
   int64_t conv_id = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(1, "t", &conv_id));
   char content[128];
   snprintf(content, sizeof(content),
            "[ATTACHED DOCUMENT: c.pdf (8 bytes) blob:%s]\nx\n[END DOCUMENT]", id);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_add_message(conv_id, 1, "user", content));

   int deleted = 0;
   TEST_ASSERT_EQUAL_INT(BLOB_STORE_SUCCESS, document_original_cleanup_orphans(-5, &deleted));
   TEST_ASSERT_EQUAL_INT(0, deleted); /* message reference protects it */
   char path[BLOB_PATH_MAX];
   TEST_ASSERT_EQUAL_INT(BLOB_STORE_SUCCESS, document_original_get_path(id, 1, path, NULL));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_save_roundtrip);
   RUN_TEST(test_dedup_same_bytes);
   RUN_TEST(test_owner_only);
   RUN_TEST(test_count_cap);
   RUN_TEST(test_delete_user);
   RUN_TEST(test_orphan_sweep);
   RUN_TEST(test_orphan_keeps_message_referenced);
   return UNITY_END();
}
