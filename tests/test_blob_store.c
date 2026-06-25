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
 * Unit tests for the blob store's pure helpers (no DB).
 */

#include <string.h>

#include "blob_store.h"
#include "unity.h"

void setUp(void) {
}
void tearDown(void) {
}

/* ---- blob_generate_id ---------------------------------------------------- */

static void test_generate_id_shape(void) {
   char id[BLOB_ID_LEN];
   TEST_ASSERT_EQUAL_INT(BLOB_STORE_SUCCESS, blob_generate_id("blb_", id));
   TEST_ASSERT_EQUAL_size_t(16, strlen(id));
   TEST_ASSERT_EQUAL_STRING_LEN("blb_", id, 4);
   for (int i = 4; i < 16; i++) {
      TEST_ASSERT_TRUE((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'z') ||
                       (id[i] >= 'A' && id[i] <= 'Z'));
   }
}

static void test_generate_id_unique(void) {
   char a[BLOB_ID_LEN], b[BLOB_ID_LEN];
   TEST_ASSERT_EQUAL_INT(BLOB_STORE_SUCCESS, blob_generate_id("img_", a));
   TEST_ASSERT_EQUAL_INT(BLOB_STORE_SUCCESS, blob_generate_id("img_", b));
   TEST_ASSERT_TRUE(strcmp(a, b) != 0); /* astronomically unlikely to collide */
}

static void test_generate_id_bad_prefix(void) {
   char id[BLOB_ID_LEN];
   TEST_ASSERT_EQUAL_INT(BLOB_STORE_INVALID, blob_generate_id("toolong_", id));
   TEST_ASSERT_EQUAL_INT(BLOB_STORE_INVALID, blob_generate_id(NULL, id));
}

/* ---- blob_validate_id ---------------------------------------------------- */

static void test_validate_id(void) {
   TEST_ASSERT_TRUE(blob_validate_id("blb_abcDEF012345", "blb_"));
   TEST_ASSERT_TRUE(blob_validate_id("img_000000000000", "img_"));
   /* wrong prefix */
   TEST_ASSERT_FALSE(blob_validate_id("img_abcDEF012345", "blb_"));
   /* too short / too long */
   TEST_ASSERT_FALSE(blob_validate_id("blb_abc", "blb_"));
   TEST_ASSERT_FALSE(blob_validate_id("blb_abcDEF0123456", "blb_"));
   /* non-alnum in the random section */
   TEST_ASSERT_FALSE(blob_validate_id("blb_abc-EF012345", "blb_"));
   /* NULL guards */
   TEST_ASSERT_FALSE(blob_validate_id(NULL, "blb_"));
   TEST_ASSERT_FALSE(blob_validate_id("blb_abcDEF012345", NULL));
}

/* ---- blob_mime_to_ext / blob_build_filename ------------------------------ */

static const blob_mime_map_t k_doc_mimes[] = {
   { "application/pdf", "pdf" },
   { "application/vnd.openxmlformats-officedocument.wordprocessingml.document", "docx" },
   { "text/plain", "txt" },
};

static void test_mime_to_ext(void) {
   size_t n = sizeof(k_doc_mimes) / sizeof(k_doc_mimes[0]);
   TEST_ASSERT_EQUAL_STRING("pdf", blob_mime_to_ext("application/pdf", k_doc_mimes, n));
   TEST_ASSERT_EQUAL_STRING("txt", blob_mime_to_ext("text/plain", k_doc_mimes, n));
   /* unknown -> bin */
   TEST_ASSERT_EQUAL_STRING("bin", blob_mime_to_ext("application/x-unknown", k_doc_mimes, n));
   /* NULL guards -> bin */
   TEST_ASSERT_EQUAL_STRING("bin", blob_mime_to_ext(NULL, k_doc_mimes, n));
}

static void test_build_filename(void) {
   size_t n = sizeof(k_doc_mimes) / sizeof(k_doc_mimes[0]);
   char fn[BLOB_FILENAME_MAX];
   blob_build_filename("blb_abcDEF012345", "application/pdf", k_doc_mimes, n, fn, sizeof(fn));
   TEST_ASSERT_EQUAL_STRING("blb_abcDEF012345.pdf", fn);
}

/* ---- blob_validate_db_filename ------------------------------------------- */

static void test_validate_db_filename(void) {
   TEST_ASSERT_TRUE(blob_validate_db_filename("blb_abcDEF012345.pdf"));
   TEST_ASSERT_FALSE(blob_validate_db_filename(""));
   TEST_ASSERT_FALSE(blob_validate_db_filename(NULL));
   TEST_ASSERT_FALSE(blob_validate_db_filename("../etc/passwd"));
   TEST_ASSERT_FALSE(blob_validate_db_filename("sub/dir.pdf"));
   TEST_ASSERT_FALSE(blob_validate_db_filename("a..b"));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_generate_id_shape);
   RUN_TEST(test_generate_id_unique);
   RUN_TEST(test_generate_id_bad_prefix);
   RUN_TEST(test_validate_id);
   RUN_TEST(test_mime_to_ext);
   RUN_TEST(test_build_filename);
   RUN_TEST(test_validate_db_filename);
   return UNITY_END();
}
