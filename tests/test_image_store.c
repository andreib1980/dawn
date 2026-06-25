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
 * Integration test for the image store AFTER it became a thin wrapper over the
 * generic blob store.  Exercises the full delegation (save -> file on disk +
 * metadata row, get_path round-trip, source-based access control, count,
 * delete, validation) against a real in-memory SQLite + a temp file dir.
 * This is the Phase 2 live-path regression gate.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "auth/auth_db.h"
#include "blob_store.h"
#include "image_store.h"
#include "unity.h"

static char s_tmpdir[256];

void setUp(void) {
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(":memory:"));
   /* Two users: ids 1 (owner) and 2 (other), per the sequential-id convention. */
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_create_user("owner", "h", true));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_create_user("other", "h", false));

   snprintf(s_tmpdir, sizeof(s_tmpdir), "/tmp/dawn_imgtest_XXXXXX");
   TEST_ASSERT_NOT_NULL(mkdtemp(s_tmpdir));

   image_store_config_t cfg = {
      .max_size = 1024 * 1024,
      .max_per_user = 100,
      .retention_days = 0,
      .cache_size_mb = 0,
      .data_dir = s_tmpdir,
   };
   TEST_ASSERT_EQUAL_INT(IMAGE_STORE_SUCCESS, image_store_init(&cfg));
}

void tearDown(void) {
   image_store_shutdown();
   blob_store_shutdown(); /* reset the shared registry between tests */
   auth_db_shutdown();
   /* Best-effort temp cleanup (CI /tmp is ephemeral anyway). */
}

/* ---- save + round-trip --------------------------------------------------- */

static void test_save_and_read_back(void) {
   const char payload[] = "\xff\xd8\xff\xe0 fake jpeg bytes \x00\x01\x02";
   char id[IMAGE_ID_LEN] = { 0 };
   TEST_ASSERT_EQUAL_INT(IMAGE_STORE_SUCCESS,
                         image_store_save(1, payload, sizeof(payload), "image/jpeg", id));
   TEST_ASSERT_TRUE(image_store_validate_id(id));

   char path[IMAGE_PATH_MAX] = { 0 };
   char mime[IMAGE_MIME_MAX] = { 0 };
   TEST_ASSERT_EQUAL_INT(IMAGE_STORE_SUCCESS, image_store_get_path(id, 1, path, mime));
   TEST_ASSERT_EQUAL_STRING("image/jpeg", mime);

   FILE *f = fopen(path, "rb");
   TEST_ASSERT_NOT_NULL(f);
   char buf[64] = { 0 };
   size_t n = fread(buf, 1, sizeof(buf), f);
   fclose(f);
   TEST_ASSERT_EQUAL_size_t(sizeof(payload), n);
   TEST_ASSERT_EQUAL_MEMORY(payload, buf, sizeof(payload));
}

static void test_metadata(void) {
   const char payload[] = "pngdata";
   char id[IMAGE_ID_LEN] = { 0 };
   TEST_ASSERT_EQUAL_INT(IMAGE_STORE_SUCCESS,
                         image_store_save(1, payload, sizeof(payload), "image/png", id));

   image_metadata_t md;
   TEST_ASSERT_EQUAL_INT(IMAGE_STORE_SUCCESS, image_store_get_metadata(id, &md));
   TEST_ASSERT_EQUAL_INT(1, md.user_id);
   TEST_ASSERT_EQUAL_STRING("image/png", md.mime_type);
   TEST_ASSERT_EQUAL_size_t(sizeof(payload), md.size);
   TEST_ASSERT_EQUAL_INT(IMAGE_SOURCE_UPLOAD, md.source);
}

/* ---- access control ------------------------------------------------------ */

static void test_upload_is_private(void) {
   const char payload[] = "secret";
   char id[IMAGE_ID_LEN] = { 0 };
   TEST_ASSERT_EQUAL_INT(IMAGE_STORE_SUCCESS,
                         image_store_save(1, payload, sizeof(payload), "image/jpeg", id));
   char path[IMAGE_PATH_MAX];
   /* Owner can read; other user and the service token (0) cannot. */
   TEST_ASSERT_EQUAL_INT(IMAGE_STORE_SUCCESS, image_store_get_path(id, 1, path, NULL));
   TEST_ASSERT_EQUAL_INT(IMAGE_STORE_FORBIDDEN, image_store_get_path(id, 2, path, NULL));
   TEST_ASSERT_EQUAL_INT(IMAGE_STORE_FORBIDDEN, image_store_get_path(id, 0, path, NULL));
}

static void test_generated_is_shareable(void) {
   const char payload[] = "gen";
   char id[IMAGE_ID_LEN] = { 0 };
   TEST_ASSERT_EQUAL_INT(IMAGE_STORE_SUCCESS,
                         image_store_save_ex(1, payload, sizeof(payload), "image/png",
                                             IMAGE_SOURCE_GENERATED, IMAGE_RETAIN_DEFAULT, id));
   char path[IMAGE_PATH_MAX];
   /* Non-private source: any authenticated user may read. */
   TEST_ASSERT_EQUAL_INT(IMAGE_STORE_SUCCESS, image_store_get_path(id, 2, path, NULL));
}

/* ---- count + delete ------------------------------------------------------ */

static void test_count_and_delete(void) {
   char id[IMAGE_ID_LEN] = { 0 };
   int count = -1;
   TEST_ASSERT_EQUAL_INT(IMAGE_STORE_SUCCESS, image_store_count_user(1, &count));
   TEST_ASSERT_EQUAL_INT(0, count);

   TEST_ASSERT_EQUAL_INT(IMAGE_STORE_SUCCESS, image_store_save(1, "x", 1, "image/gif", id));
   TEST_ASSERT_EQUAL_INT(IMAGE_STORE_SUCCESS, image_store_count_user(1, &count));
   TEST_ASSERT_EQUAL_INT(1, count);

   char path[IMAGE_PATH_MAX];
   TEST_ASSERT_EQUAL_INT(IMAGE_STORE_SUCCESS, image_store_get_path(id, 1, path, NULL));

   /* Wrong user cannot delete; owner can. */
   TEST_ASSERT_EQUAL_INT(IMAGE_STORE_FORBIDDEN, image_store_delete(id, 2));
   TEST_ASSERT_EQUAL_INT(IMAGE_STORE_SUCCESS, image_store_delete(id, 1));
   TEST_ASSERT_EQUAL_INT(IMAGE_STORE_NOT_FOUND, image_store_get_path(id, 1, path, NULL));
   /* File removed from disk. */
   TEST_ASSERT_NOT_EQUAL(0, access(path, F_OK));
}

/* ---- validation ---------------------------------------------------------- */

static void test_validation(void) {
   char id[IMAGE_ID_LEN] = { 0 };
   /* Disallowed MIME (SVG) rejected. */
   TEST_ASSERT_EQUAL_INT(IMAGE_STORE_INVALID, image_store_save(1, "x", 1, "image/svg+xml", id));
   /* Oversize rejected (max_size = 1 MB). */
   size_t big = 2 * 1024 * 1024;
   char *buf = calloc(1, big);
   TEST_ASSERT_NOT_NULL(buf);
   TEST_ASSERT_EQUAL_INT(IMAGE_STORE_TOO_LARGE, image_store_save(1, buf, big, "image/jpeg", id));
   free(buf);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_save_and_read_back);
   RUN_TEST(test_metadata);
   RUN_TEST(test_upload_is_private);
   RUN_TEST(test_generated_is_shareable);
   RUN_TEST(test_count_and_delete);
   RUN_TEST(test_validation);
   return UNITY_END();
}
