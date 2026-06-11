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
 * Integration tests for the OTA daemon glue (src/core/ota.c + ota_db.c):
 * release store scan, single-flight offers, one-time download tokens,
 * path-traversal rejection, and server-owned register-time finalize.
 */

#include <sodium.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "auth/auth_db.h"
#include "config/dawn_config.h"
#include "core/ota.h"
#include "core/ota_db.h"
#include "core/ota_manifest.h"
#include "unity.h"

/* ota.c references the global config; provide it for the test link. */
dawn_config_t g_config;

/* ota.c's finalize path calls the rollout hooks; test_ota exercises the offer/
 * manifest/db logic, not fleet rollout, and doesn't link ota_rollout.c (which
 * would drag in session_manager). Stub them — no-ops with no rollout active. */
void ota_rollout_on_commit(const char *uuid, const char *version) {
   (void)uuid;
   (void)version;
}
void ota_rollout_on_fail(const char *uuid, const char *version) {
   (void)uuid;
   (void)version;
}

#define UUID_A "aaaaaaaa-aaaa-4aaa-aaaa-aaaaaaaaaaaa"
#define UUID_B "bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb"

static char s_tmpdir[256];

static void write_bin(const char *path, const void *data, size_t len) {
   FILE *f = fopen(path, "wb");
   TEST_ASSERT_NOT_NULL(f);
   TEST_ASSERT_EQUAL_UINT(len, fwrite(data, 1, len, f));
   fclose(f);
}

/* Build a signed release at <tmpdir>/<platform>/<version>/{image,manifest,manifest.sig}. */
static void make_release(const char *platform, ota_platform_t plat, int tier, const char *version) {
   char dir[512], path[600];
   snprintf(dir, sizeof(dir), "%s/%s", s_tmpdir, platform);
   mkdir(dir, 0755);
   snprintf(dir, sizeof(dir), "%s/%s/%s", s_tmpdir, platform, version);
   mkdir(dir, 0755);

   uint8_t image[4096];
   randombytes_buf(image, sizeof(image));
   snprintf(path, sizeof(path), "%s/image", dir);
   write_bin(path, image, sizeof(image));

   ota_manifest_t m;
   memset(&m, 0, sizeof(m));
   m.fmt_version = OTA_MANIFEST_FMT_VERSION;
   m.platform = plat;
   m.tier = tier;
   snprintf(m.version, sizeof(m.version), "%s", version);
   snprintf(m.abi_tag, sizeof(m.abi_tag), "debian-trixie-aarch64");
   m.image_size = sizeof(image);
   TEST_ASSERT_EQUAL_INT(SUCCESS, ota_sha256(image, sizeof(image), m.sha256));

   uint8_t pk[OTA_PK_BYTES], sk[OTA_SK_BYTES];
   ota_keygen(pk, sk);
   uint8_t raw[OTA_MANIFEST_WIRE_SIZE];
   uint8_t sig[OTA_SIG_BYTES];
   size_t raw_len = 0;
   TEST_ASSERT_EQUAL_INT(SUCCESS, ota_manifest_sign(&m, sk, raw, sizeof(raw), &raw_len, sig));

   snprintf(path, sizeof(path), "%s/manifest", dir);
   write_bin(path, raw, raw_len);
   snprintf(path, sizeof(path), "%s/manifest.sig", dir);
   write_bin(path, sig, sizeof(sig));
}

void setUp(void) {
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(":memory:"));

   snprintf(s_tmpdir, sizeof(s_tmpdir), "/tmp/ota_test_XXXXXX");
   TEST_ASSERT_NOT_NULL(mkdtemp(s_tmpdir));

   memset(&g_config.ota, 0, sizeof(g_config.ota));
   g_config.ota.enabled = true;
   snprintf(g_config.ota.release_dir, sizeof(g_config.ota.release_dir), "%s", s_tmpdir);
   g_config.ota.download_token_ttl_sec = 120;
   g_config.ota.require_tls = true;

   make_release("rpi", OTA_PLATFORM_RPI, 1, "2.1.0");
   make_release("rpi", OTA_PLATFORM_RPI, 1, "2.0.0");

   TEST_ASSERT_EQUAL_INT(SUCCESS, ota_init());
   TEST_ASSERT_TRUE(ota_enabled());
}

void tearDown(void) {
   /* Best-effort cleanup of the temp release tree. */
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", s_tmpdir);
   if (system(cmd) != 0) {
      /* ignore */
   }
   auth_db_shutdown();
}

static void test_resolve_latest(void) {
   char ver[OTA_VERSION_MAX];
   TEST_ASSERT_EQUAL_INT(SUCCESS, ota_resolve_latest(OTA_PLATFORM_RPI, 1, ver, sizeof(ver)));
   TEST_ASSERT_EQUAL_STRING("2.1.0", ver); /* numeric latest of {2.0.0, 2.1.0} */
   /* No esp32 releases were created. */
   TEST_ASSERT_EQUAL_INT(FAILURE, ota_resolve_latest(OTA_PLATFORM_ESP32, 2, ver, sizeof(ver)));
}

static void test_begin_push_offer_fields(void) {
   ota_offer_t offer;
   TEST_ASSERT_EQUAL_INT(SUCCESS, ota_begin_push(UUID_A, OTA_PLATFORM_RPI, "2.1.0", false, &offer));
   TEST_ASSERT_EQUAL_STRING("rpi", offer.platform_str);
   TEST_ASSERT_EQUAL_STRING("2.1.0", offer.version);
   TEST_ASSERT_EQUAL_STRING("/api/ota/rpi/2.1.0/image", offer.url_path);
   TEST_ASSERT_EQUAL_UINT(OTA_TOKEN_HEX, strlen(offer.token));
   TEST_ASSERT_EQUAL_UINT(OTA_MANIFEST_WIRE_SIZE * 2, strlen(offer.manifest_hex));
   TEST_ASSERT_EQUAL_UINT(OTA_SIG_BYTES * 2, strlen(offer.sig_hex));
   TEST_ASSERT_EQUAL_UINT64(4096, offer.image_size);
}

static void test_begin_push_unknown_release(void) {
   ota_offer_t offer;
   TEST_ASSERT_EQUAL_INT(FAILURE, ota_begin_push(UUID_A, OTA_PLATFORM_RPI, "9.9.9", false, &offer));
}

static void test_single_flight(void) {
   ota_offer_t offer;
   TEST_ASSERT_EQUAL_INT(SUCCESS, ota_begin_push(UUID_A, OTA_PLATFORM_RPI, "2.1.0", false, &offer));
   /* A second push while one is in flight is rejected. */
   TEST_ASSERT_EQUAL_INT(AUTH_DB_LOCKED,
                         ota_begin_push(UUID_A, OTA_PLATFORM_RPI, "2.0.0", false, &offer));
}

static void test_download_token_single_use(void) {
   ota_offer_t offer;
   TEST_ASSERT_EQUAL_INT(SUCCESS, ota_begin_push(UUID_A, OTA_PLATFORM_RPI, "2.1.0", false, &offer));

   char path[1024];
   /* Correct token → authorized once. */
   TEST_ASSERT_EQUAL_INT(SUCCESS, ota_authorize_image_download(UUID_A, "rpi", "2.1.0", offer.token,
                                                               path, sizeof(path)));
   TEST_ASSERT_NOT_NULL(strstr(path, "/rpi/2.1.0/image"));
   /* Same token again → rejected (single use). */
   TEST_ASSERT_EQUAL_INT(FAILURE, ota_authorize_image_download(UUID_A, "rpi", "2.1.0", offer.token,
                                                               path, sizeof(path)));
}

static void test_download_wrong_token(void) {
   ota_offer_t offer;
   TEST_ASSERT_EQUAL_INT(SUCCESS, ota_begin_push(UUID_B, OTA_PLATFORM_RPI, "2.1.0", false, &offer));
   char path[1024];
   TEST_ASSERT_EQUAL_INT(FAILURE, ota_authorize_image_download(UUID_B, "rpi", "2.1.0", "deadbeef",
                                                               path, sizeof(path)));
}

static void test_download_path_traversal(void) {
   ota_offer_t offer;
   TEST_ASSERT_EQUAL_INT(SUCCESS, ota_begin_push(UUID_A, OTA_PLATFORM_RPI, "2.1.0", false, &offer));
   char path[1024];
   /* Malformed version components must be rejected (before the token is spent). */
   TEST_ASSERT_EQUAL_INT(FAILURE, ota_authorize_image_download(UUID_A, "rpi", "../../etc",
                                                               offer.token, path, sizeof(path)));
   TEST_ASSERT_EQUAL_INT(FAILURE, ota_authorize_image_download(UUID_A, "bogusplat", "2.1.0",
                                                               offer.token, path, sizeof(path)));
   /* Token survived the malformed attempts and still works. */
   TEST_ASSERT_EQUAL_INT(SUCCESS, ota_authorize_image_download(UUID_A, "rpi", "2.1.0", offer.token,
                                                               path, sizeof(path)));
}

/* A download token issued for one platform must not fetch another platform's
 * image of the same version (the token is bound to target_platform). */
static void test_download_platform_bound_token(void) {
   /* Stage an esp32 release at the SAME version, so only the platform binding —
    * not a missing path — can reject the cross-platform fetch. */
   make_release("esp32", OTA_PLATFORM_ESP32, 2, "2.1.0");
   TEST_ASSERT_EQUAL_INT(SUCCESS, ota_init()); /* rescan so esp32/2.1.0 is known */

   ota_offer_t offer;
   TEST_ASSERT_EQUAL_INT(SUCCESS, ota_begin_push(UUID_A, OTA_PLATFORM_RPI, "2.1.0", false, &offer));

   char path[1024];
   /* rpi-issued token must NOT authorize the esp32 image of the same version. */
   TEST_ASSERT_EQUAL_INT(FAILURE, ota_authorize_image_download(UUID_A, "esp32", "2.1.0",
                                                               offer.token, path, sizeof(path)));
   /* The token survived the cross-platform attempt and still works for rpi. */
   TEST_ASSERT_EQUAL_INT(SUCCESS, ota_authorize_image_download(UUID_A, "rpi", "2.1.0", offer.token,
                                                               path, sizeof(path)));
   TEST_ASSERT_NOT_NULL(strstr(path, "/rpi/2.1.0/image"));
}

static void test_finalize_on_register(void) {
   ota_offer_t offer;
   TEST_ASSERT_EQUAL_INT(SUCCESS, ota_begin_push(UUID_A, OTA_PLATFORM_RPI, "2.1.0", false, &offer));

   /* Wrong reported version → no commit, target still set. */
   ota_finalize_on_register(UUID_A, "2.0.0");
   ota_device_state_t st;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, ota_db_get(UUID_A, &st));
   TEST_ASSERT_EQUAL_STRING("2.1.0", st.target_version);

   /* Reported == target → server commits success and clears the target. */
   ota_finalize_on_register(UUID_A, "2.1.0");
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, ota_db_get(UUID_A, &st));
   TEST_ASSERT_EQUAL_STRING("success", st.state);
   TEST_ASSERT_EQUAL_STRING("", st.target_version);
}

/* A device that has started applying (state past 'offered') and reconnects on a
 * non-target version resolves the otherwise-stuck row to 'failed' (target kept so
 * a later success register can still finalize) instead of hanging in-flight. */
static void test_finalize_mismatch_after_apply(void) {
   ota_offer_t offer;
   TEST_ASSERT_EQUAL_INT(SUCCESS, ota_begin_push(UUID_A, OTA_PLATFORM_RPI, "2.1.0", false, &offer));
   /* Device progressed past the offer (e.g. began downloading) then came back on
    * the OLD version — a rollback or an unbumped firmware-version header. */
   TEST_ASSERT_EQUAL_INT(SUCCESS, ota_report_status(UUID_A, OTA_STATE_DOWNLOADING, NULL));
   ota_finalize_on_register(UUID_A, "2.0.0");
   ota_device_state_t st;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, ota_db_get(UUID_A, &st));
   TEST_ASSERT_EQUAL_STRING("failed", st.state);
   TEST_ASSERT_EQUAL_STRING("2.1.0", st.target_version); /* kept for self-correct */

   /* The real success register that follows still finalizes via the success path. */
   ota_finalize_on_register(UUID_A, "2.1.0");
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, ota_db_get(UUID_A, &st));
   TEST_ASSERT_EQUAL_STRING("success", st.state);
   TEST_ASSERT_EQUAL_STRING("", st.target_version);
}

static void test_reconcile_stale(void) {
   ota_offer_t offer;
   TEST_ASSERT_EQUAL_INT(SUCCESS, ota_begin_push(UUID_A, OTA_PLATFORM_RPI, "2.1.0", false, &offer));
   /* stale_before in the future → the just-created in-flight row is reconciled. */
   int n = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, ota_db_reconcile_stale(time(NULL) + 100, &n));
   TEST_ASSERT_GREATER_OR_EQUAL_INT(1, n);
   ota_device_state_t st;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, ota_db_get(UUID_A, &st));
   TEST_ASSERT_EQUAL_STRING("unknown", st.state);
}

/* Trust-boundary state allowlist: only the OTA_STATE_* tokens are accepted; a
 * device-supplied unknown/empty/NULL token (or a case/whitespace variant) is
 * rejected before it can be persisted or dodge the in-flight predicate. */
static void test_state_is_valid(void) {
   TEST_ASSERT_TRUE(ota_state_is_valid(OTA_STATE_IDLE));
   TEST_ASSERT_TRUE(ota_state_is_valid(OTA_STATE_DOWNLOADING));
   TEST_ASSERT_TRUE(ota_state_is_valid(OTA_STATE_SUCCESS));
   TEST_ASSERT_TRUE(ota_state_is_valid(OTA_STATE_FAILED));
   TEST_ASSERT_TRUE(ota_state_is_valid(OTA_STATE_UNKNOWN));

   TEST_ASSERT_FALSE(ota_state_is_valid(NULL));
   TEST_ASSERT_FALSE(ota_state_is_valid(""));
   TEST_ASSERT_FALSE(ota_state_is_valid("bogus"));
   TEST_ASSERT_FALSE(ota_state_is_valid("DOWNLOADING"));  /* case-sensitive */
   TEST_ASSERT_FALSE(ota_state_is_valid("downloading ")); /* trailing space */
}

/* A pending offer the startup reconcile rewrote to 'unknown' must NOT be
 * auto-failed when the device reconnects on the old version — it never started
 * applying.  Regression for the restart-fails-pending-offers bug: only the truly
 * in-progress states (downloading/verifying/applying/rebooting) auto-fail. */
static void test_finalize_unknown_not_failed(void) {
   ota_offer_t offer;
   TEST_ASSERT_EQUAL_INT(SUCCESS, ota_begin_push(UUID_A, OTA_PLATFORM_RPI, "2.1.0", false, &offer));
   /* Simulate startup reconcile flipping the stale 'offered' row to 'unknown'. */
   int n = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, ota_db_reconcile_stale(time(NULL) + 100, &n));
   ota_device_state_t st;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, ota_db_get(UUID_A, &st));
   TEST_ASSERT_EQUAL_STRING("unknown", st.state);
   /* Device reconnects on the OLD version — never applied, so not a failure. */
   ota_finalize_on_register(UUID_A, "2.0.0");
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, ota_db_get(UUID_A, &st));
   TEST_ASSERT_EQUAL_STRING("unknown", st.state);        /* left alone, not 'failed' */
   TEST_ASSERT_EQUAL_STRING("2.1.0", st.target_version); /* target kept for a fresh push */
}

int main(void) {
   if (sodium_init() < 0) {
      return 1;
   }
   UNITY_BEGIN();
   RUN_TEST(test_state_is_valid);
   RUN_TEST(test_finalize_unknown_not_failed);
   RUN_TEST(test_resolve_latest);
   RUN_TEST(test_begin_push_offer_fields);
   RUN_TEST(test_begin_push_unknown_release);
   RUN_TEST(test_single_flight);
   RUN_TEST(test_download_token_single_use);
   RUN_TEST(test_download_wrong_token);
   RUN_TEST(test_download_path_traversal);
   RUN_TEST(test_download_platform_bound_token);
   RUN_TEST(test_finalize_on_register);
   RUN_TEST(test_finalize_mismatch_after_apply);
   RUN_TEST(test_reconcile_stale);
   return UNITY_END();
}
