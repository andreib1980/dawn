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
 * Host unit tests for the device-side OTA offer decision (ota_apply_decide) and
 * its helpers.  Covers section-E rows 2 (bad signature), 4 (abi mismatch),
 * 5 (downgrade ± allow_downgrade), 6 (already current), the min_version floor,
 * malformed-offer / bad-url / hex / offer↔manifest cross-check rejects, and
 * section C (no keyring → fail closed).  Pure libsodium + json-c — no device.
 *
 * Compiled with -DOTA_KEYRING_PATH pointed at a temp file so load_keyring() can
 * be exercised without writing /etc/dawn/ota_pubkey.
 */

#include <json-c/json.h>
#include <sodium.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "core/ota_manifest.h"
#include "dawn_error.h"
#include "ota_apply_internal.h"
#include "ota_marker.h" /* OTA_MAX_IMAGE_BYTES */
#include "unity.h"

#define DEV_ABI "debian-trixie-aarch64"

static uint8_t g_pk[OTA_PK_BYTES];
static uint8_t g_sk[OTA_SK_BYTES];

void setUp(void) {
}
void tearDown(void) {
   unlink(OTA_KEYRING_PATH);
}

/* Build an ota_offer JSON.  Signs with @p sk (pass a non-keyring sk to forge a
 * bad signature).  @p offer_size overrides the offer's image_size field so the
 * offer↔manifest cross-check can be exercised (pass 0 to mirror the manifest). */
static struct json_object *make_offer(const char *version,
                                      const char *abi,
                                      const char *min_version,
                                      uint64_t image_size,
                                      uint64_t offer_size,
                                      bool allow_downgrade,
                                      const uint8_t *sk) {
   ota_manifest_t m = { 0 };
   m.fmt_version = OTA_MANIFEST_FMT_VERSION;
   m.platform = OTA_PLATFORM_RPI;
   m.tier = 1;
   snprintf(m.version, sizeof(m.version), "%s", version);
   snprintf(m.abi_tag, sizeof(m.abi_tag), "%s", abi);
   snprintf(m.min_version, sizeof(m.min_version), "%s", min_version);
   m.image_size = image_size;
   for (int i = 0; i < OTA_SHA256_BYTES; i++) {
      m.sha256[i] = (uint8_t)(i + 1);
   }

   uint8_t raw[OTA_MANIFEST_WIRE_SIZE], sig[OTA_SIG_BYTES];
   size_t len = 0;
   TEST_ASSERT_EQUAL_INT(SUCCESS, ota_manifest_sign(&m, sk, raw, sizeof(raw), &len, sig));

   char man_hex[OTA_MANIFEST_WIRE_SIZE * 2 + 1];
   char sig_hex[OTA_SIG_BYTES * 2 + 1];
   char sha_hex[OTA_SHA256_BYTES * 2 + 1];
   sodium_bin2hex(man_hex, sizeof(man_hex), raw, len);
   sodium_bin2hex(sig_hex, sizeof(sig_hex), sig, sizeof(sig));
   sodium_bin2hex(sha_hex, sizeof(sha_hex), m.sha256, OTA_SHA256_BYTES);

   char token[OTA_OFFER_TOKEN_HEX + 1];
   memset(token, 'a', OTA_OFFER_TOKEN_HEX);
   token[OTA_OFFER_TOKEN_HEX] = '\0';

   struct json_object *o = json_object_new_object();
   json_object_object_add(o, "url_path", json_object_new_string("/api/ota/rpi/2.3.0/image"));
   json_object_object_add(o, "token", json_object_new_string(token));
   json_object_object_add(o, "sha256", json_object_new_string(sha_hex));
   json_object_object_add(o, "manifest", json_object_new_string(man_hex));
   json_object_object_add(o, "sig", json_object_new_string(sig_hex));
   json_object_object_add(o, "image_size",
                          json_object_new_int64((int64_t)(offer_size ? offer_size : image_size)));
   if (allow_downgrade) {
      json_object_object_add(o, "allow_downgrade", json_object_new_boolean(1));
   }
   return o;
}

/* Decide with the module's own keyring (g_pk) and the device abi. */
static ota_decide_t decide(struct json_object *o,
                           const char *installed,
                           const char *abi,
                           ota_manifest_t *out,
                           char *reason,
                           size_t rn) {
   uint8_t keyring[1][OTA_PK_BYTES];
   memcpy(keyring[0], g_pk, OTA_PK_BYTES);
   return ota_apply_decide(o, installed, abi, keyring, 1, out, reason, rn);
}

/* ---- happy path --------------------------------------------------------- */
static void test_accept_newer(void) {
   struct json_object *o = make_offer("2.3.0", DEV_ABI, "", 1000, 0, false, g_sk);
   ota_manifest_t m;
   char r[OTA_DETAIL_BUF] = "";
   TEST_ASSERT_EQUAL_INT(OTA_DECIDE_ACCEPT, decide(o, "2.2.0", DEV_ABI, &m, r, sizeof(r)));
   TEST_ASSERT_EQUAL_STRING("2.3.0", m.version);
   json_object_put(o);
}

/* ---- row 2: bad signature ----------------------------------------------- */
static void test_bad_signature(void) {
   uint8_t bad_pk[OTA_PK_BYTES], bad_sk[OTA_SK_BYTES];
   ota_keygen(bad_pk, bad_sk); /* not in the keyring */
   struct json_object *o = make_offer("2.3.0", DEV_ABI, "", 1000, 0, false, bad_sk);
   ota_manifest_t m;
   char r[OTA_DETAIL_BUF] = "";
   TEST_ASSERT_EQUAL_INT(OTA_DECIDE_REJECT, decide(o, "2.2.0", DEV_ABI, &m, r, sizeof(r)));
   TEST_ASSERT_EQUAL_STRING("bad signature", r);
   json_object_put(o);
}

/* ---- row 4: abi mismatch ------------------------------------------------ */
static void test_abi_mismatch(void) {
   struct json_object *o = make_offer("2.3.0", "debian-bookworm-aarch64", "", 1000, 0, false, g_sk);
   ota_manifest_t m;
   char r[OTA_DETAIL_BUF] = "";
   TEST_ASSERT_EQUAL_INT(OTA_DECIDE_REJECT, decide(o, "2.2.0", DEV_ABI, &m, r, sizeof(r)));
   TEST_ASSERT_EQUAL_INT(0, strncmp(r, "abi mismatch", 12));
   json_object_put(o);
}

/* ---- row 6: already current --------------------------------------------- */
static void test_already_current(void) {
   struct json_object *o = make_offer("2.2.0", DEV_ABI, "", 1000, 0, false, g_sk);
   ota_manifest_t m;
   char r[OTA_DETAIL_BUF] = "";
   TEST_ASSERT_EQUAL_INT(OTA_DECIDE_REJECT, decide(o, "2.2.0", DEV_ABI, &m, r, sizeof(r)));
   TEST_ASSERT_EQUAL_STRING("already on this version", r);
   json_object_put(o);
}

/* ---- row 5: downgrade blocked, then allowed ----------------------------- */
static void test_downgrade_blocked_then_allowed(void) {
   ota_manifest_t m;
   char r[OTA_DETAIL_BUF] = "";

   struct json_object *o = make_offer("2.0.0", DEV_ABI, "", 1000, 0, false, g_sk);
   TEST_ASSERT_EQUAL_INT(OTA_DECIDE_REJECT, decide(o, "2.2.0", DEV_ABI, &m, r, sizeof(r)));
   TEST_ASSERT_EQUAL_INT(0, strncmp(r, "downgrade blocked", 17));
   json_object_put(o);

   o = make_offer("2.0.0", DEV_ABI, "", 1000, 0, true /* allow_downgrade */, g_sk);
   TEST_ASSERT_EQUAL_INT(OTA_DECIDE_ACCEPT, decide(o, "2.2.0", DEV_ABI, &m, r, sizeof(r)));
   json_object_put(o);
}

/* ---- min_version anti-skip floor ---------------------------------------- */
static void test_min_version_floor(void) {
   /* Upgrade 2.0.0 → 2.3.0 but the manifest requires the device be >= 2.2.0. */
   struct json_object *o = make_offer("2.3.0", DEV_ABI, "2.2.0", 1000, 0, false, g_sk);
   ota_manifest_t m;
   char r[OTA_DETAIL_BUF] = "";
   TEST_ASSERT_EQUAL_INT(OTA_DECIDE_REJECT, decide(o, "2.0.0", DEV_ABI, &m, r, sizeof(r)));
   TEST_ASSERT_EQUAL_INT(0, strncmp(r, "installed=", 10));
   TEST_ASSERT_NOT_NULL(strstr(r, "floor"));
   json_object_put(o);
}

/* ---- offer↔manifest cross-check (trust only signed values) -------------- */
static void test_offer_manifest_mismatch(void) {
   /* Offer claims a different image_size than the signed manifest. */
   struct json_object *o = make_offer("2.3.0", DEV_ABI, "", 1000, 9999, false, g_sk);
   ota_manifest_t m;
   char r[OTA_DETAIL_BUF] = "";
   TEST_ASSERT_EQUAL_INT(OTA_DECIDE_REJECT, decide(o, "2.2.0", DEV_ABI, &m, r, sizeof(r)));
   TEST_ASSERT_EQUAL_STRING("offer/manifest mismatch", r);
   json_object_put(o);
}

/* ---- section C: no keyring provisioned → fail closed -------------------- */
static void test_no_keyring_fail_closed(void) {
   struct json_object *o = make_offer("2.3.0", DEV_ABI, "", 1000, 0, false, g_sk);
   ota_manifest_t m;
   char r[OTA_DETAIL_BUF] = "";
   /* n_keys = 0 → reject without ever verifying. */
   TEST_ASSERT_EQUAL_INT(OTA_DECIDE_REJECT,
                         ota_apply_decide(o, "2.2.0", DEV_ABI, NULL, 0, &m, r, sizeof(r)));
   TEST_ASSERT_NOT_NULL(strstr(r, "keyring"));
   json_object_put(o);
}

/* ---- malformed / hostile offers ----------------------------------------- */
static void test_malformed_missing_field(void) {
   struct json_object *o = make_offer("2.3.0", DEV_ABI, "", 1000, 0, false, g_sk);
   json_object_object_del(o, "sig"); /* drop a required field */
   ota_manifest_t m;
   char r[OTA_DETAIL_BUF] = "";
   TEST_ASSERT_EQUAL_INT(OTA_DECIDE_REJECT, decide(o, "2.2.0", DEV_ABI, &m, r, sizeof(r)));
   TEST_ASSERT_EQUAL_STRING("malformed offer", r);
   json_object_put(o);
}

static void test_bad_url_path(void) {
   struct json_object *o = make_offer("2.3.0", DEV_ABI, "", 1000, 0, false, g_sk);
   json_object_object_del(o, "url_path");
   json_object_object_add(o, "url_path", json_object_new_string("/api/ota/../../etc/passwd"));
   ota_manifest_t m;
   char r[OTA_DETAIL_BUF] = "";
   TEST_ASSERT_EQUAL_INT(OTA_DECIDE_REJECT, decide(o, "2.2.0", DEV_ABI, &m, r, sizeof(r)));
   TEST_ASSERT_EQUAL_STRING("bad url_path", r);
   json_object_put(o);
}

static void test_bad_image_size(void) {
   struct json_object *o = make_offer("2.3.0", DEV_ABI, "", 1000, 0, false, g_sk);
   json_object_object_del(o, "image_size");
   json_object_object_add(o, "image_size", json_object_new_int64(0)); /* must be > 0 */
   ota_manifest_t m;
   char r[OTA_DETAIL_BUF] = "";
   TEST_ASSERT_EQUAL_INT(OTA_DECIDE_REJECT, decide(o, "2.2.0", DEV_ABI, &m, r, sizeof(r)));
   TEST_ASSERT_EQUAL_STRING("malformed offer", r);
   json_object_put(o);
}

static void test_oversize_image_rejected(void) {
   /* offer image_size above the sanity cap → rejected before any verify */
   struct json_object *o = make_offer("2.3.0", DEV_ABI, "", 1000, (uint64_t)OTA_MAX_IMAGE_BYTES + 1,
                                      false, g_sk);
   ota_manifest_t m;
   char r[OTA_DETAIL_BUF] = "";
   TEST_ASSERT_EQUAL_INT(OTA_DECIDE_REJECT, decide(o, "2.2.0", DEV_ABI, &m, r, sizeof(r)));
   TEST_ASSERT_EQUAL_STRING("malformed offer", r);
   json_object_put(o);
}

/* ---- helper: build_abi_tag join recipe (row 4 ground truth) ------------- */
static void test_build_abi_tag(void) {
   char abi[OTA_ABI_TAG_MAX];
   build_abi_tag("debian", "trixie", "aarch64", abi, sizeof(abi));
   TEST_ASSERT_EQUAL_STRING("debian-trixie-aarch64", abi);
   /* a missing field still produces a (non-matching) tag, never a crash */
   build_abi_tag("", "", "", abi, sizeof(abi));
   TEST_ASSERT_EQUAL_STRING("--", abi);
}

/* ---- helper: load_keyring file parsing (section C) ----------------------- */
static void write_keyring(const char *content) {
   FILE *f = fopen(OTA_KEYRING_PATH, "w");
   TEST_ASSERT_NOT_NULL(f);
   fputs(content, f);
   fclose(f);
}

static void test_load_keyring(void) {
   uint8_t keys[OTA_KEYRING_MAX][OTA_PK_BYTES];

   /* absent file → 0 (fail closed) */
   unlink(OTA_KEYRING_PATH);
   TEST_ASSERT_EQUAL_UINT(0, load_keyring(keys, OTA_KEYRING_MAX));

   /* two valid keys (rotation) + a comment + blank + a malformed line */
   char k1[OTA_PK_BYTES * 2 + 1], k2[OTA_PK_BYTES * 2 + 1];
   uint8_t pk2[OTA_PK_BYTES], sk2[OTA_SK_BYTES];
   ota_keygen(pk2, sk2);
   sodium_bin2hex(k1, sizeof(k1), g_pk, OTA_PK_BYTES);
   sodium_bin2hex(k2, sizeof(k2), pk2, OTA_PK_BYTES);
   char buf[512];
   snprintf(buf, sizeof(buf), "# operator keyring\n%s\n\n%s\nnot-a-key\n", k1, k2);
   write_keyring(buf);
   TEST_ASSERT_EQUAL_UINT(2, load_keyring(keys, OTA_KEYRING_MAX));
   TEST_ASSERT_EQUAL_MEMORY(g_pk, keys[0], OTA_PK_BYTES);
   TEST_ASSERT_EQUAL_MEMORY(pk2, keys[1], OTA_PK_BYTES);
}

int main(void) {
   if (sodium_init() < 0) {
      return 1;
   }
   ota_keygen(g_pk, g_sk);
   UNITY_BEGIN();
   RUN_TEST(test_accept_newer);
   RUN_TEST(test_bad_signature);
   RUN_TEST(test_abi_mismatch);
   RUN_TEST(test_already_current);
   RUN_TEST(test_downgrade_blocked_then_allowed);
   RUN_TEST(test_min_version_floor);
   RUN_TEST(test_offer_manifest_mismatch);
   RUN_TEST(test_no_keyring_fail_closed);
   RUN_TEST(test_malformed_missing_field);
   RUN_TEST(test_bad_url_path);
   RUN_TEST(test_bad_image_size);
   RUN_TEST(test_oversize_image_rejected);
   RUN_TEST(test_build_abi_tag);
   RUN_TEST(test_load_keyring);
   return UNITY_END();
}
