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
 * Unit tests for the OTA signed-manifest core (src/core/ota_manifest.c).
 */

#include <sodium.h>
#include <string.h>

#include "core/ota_manifest.h"
#include "unity.h"

static uint8_t g_pk[OTA_PK_BYTES];
static uint8_t g_sk[OTA_SK_BYTES];

void setUp(void) {
   TEST_ASSERT_EQUAL_INT(SUCCESS, ota_crypto_init());
   ota_keygen(g_pk, g_sk);
}

void tearDown(void) {
}

static ota_manifest_t sample_manifest(void) {
   ota_manifest_t m;
   memset(&m, 0, sizeof(m));
   m.fmt_version = OTA_MANIFEST_FMT_VERSION;
   m.platform = OTA_PLATFORM_RPI;
   m.tier = 1;
   strcpy(m.version, "2.1.0");
   strcpy(m.abi_tag, "debian-trixie-aarch64");
   m.image_size = 1234567;
   for (int i = 0; i < OTA_SHA256_BYTES; i++) {
      m.sha256[i] = (uint8_t)i;
   }
   strcpy(m.min_version, "2.0.0");
   return m;
}

/* --- SHA-256 known vector --- */
static void test_sha256_abc(void) {
   /* SHA-256("abc") */
   const uint8_t expected[OTA_SHA256_BYTES] = { 0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
                                                0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
                                                0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
                                                0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad };
   uint8_t out[OTA_SHA256_BYTES];
   TEST_ASSERT_EQUAL_INT(SUCCESS, ota_sha256((const uint8_t *)"abc", 3, out));
   TEST_ASSERT_EQUAL_MEMORY(expected, out, OTA_SHA256_BYTES);
}

/* --- serialize / deserialize roundtrip --- */
static void test_serialize_roundtrip(void) {
   ota_manifest_t m = sample_manifest();
   uint8_t buf[OTA_MANIFEST_WIRE_SIZE];
   size_t len = 0;
   TEST_ASSERT_EQUAL_INT(SUCCESS, ota_manifest_serialize(&m, buf, sizeof(buf), &len));
   TEST_ASSERT_EQUAL_UINT(OTA_MANIFEST_WIRE_SIZE, len);

   ota_manifest_t back;
   TEST_ASSERT_EQUAL_INT(SUCCESS, ota_manifest_deserialize(buf, len, &back));
   TEST_ASSERT_EQUAL_UINT16(m.fmt_version, back.fmt_version);
   TEST_ASSERT_EQUAL_UINT8(m.platform, back.platform);
   TEST_ASSERT_EQUAL_UINT8(m.tier, back.tier);
   TEST_ASSERT_EQUAL_STRING(m.version, back.version);
   TEST_ASSERT_EQUAL_STRING(m.abi_tag, back.abi_tag);
   TEST_ASSERT_EQUAL_UINT64(m.image_size, back.image_size);
   TEST_ASSERT_EQUAL_MEMORY(m.sha256, back.sha256, OTA_SHA256_BYTES);
   TEST_ASSERT_EQUAL_STRING(m.min_version, back.min_version);
}

static void test_serialize_buffer_too_small(void) {
   ota_manifest_t m = sample_manifest();
   uint8_t buf[OTA_MANIFEST_WIRE_SIZE - 1];
   size_t len = 0;
   TEST_ASSERT_EQUAL_INT(FAILURE, ota_manifest_serialize(&m, buf, sizeof(buf), &len));
}

static void test_deserialize_bad_magic(void) {
   ota_manifest_t m = sample_manifest();
   uint8_t buf[OTA_MANIFEST_WIRE_SIZE];
   size_t len = 0;
   ota_manifest_serialize(&m, buf, sizeof(buf), &len);
   buf[0] ^= 0xff; /* corrupt magic */
   ota_manifest_t back;
   TEST_ASSERT_EQUAL_INT(FAILURE, ota_manifest_deserialize(buf, len, &back));
}

/* --- sign / verify roundtrip --- */
static void test_sign_verify_roundtrip(void) {
   ota_manifest_t m = sample_manifest();
   uint8_t raw[OTA_MANIFEST_WIRE_SIZE];
   uint8_t sig[OTA_SIG_BYTES];
   size_t len = 0;
   TEST_ASSERT_EQUAL_INT(SUCCESS, ota_manifest_sign(&m, g_sk, raw, sizeof(raw), &len, sig));

   uint8_t keyring[1][OTA_PK_BYTES];
   memcpy(keyring[0], g_pk, OTA_PK_BYTES);
   ota_manifest_t got;
   TEST_ASSERT_EQUAL_INT(SUCCESS, ota_manifest_verify(raw, len, sig, keyring, 1, &got));
   TEST_ASSERT_EQUAL_STRING("2.1.0", got.version);
}

static void test_verify_rejects_tampered_payload(void) {
   ota_manifest_t m = sample_manifest();
   uint8_t raw[OTA_MANIFEST_WIRE_SIZE];
   uint8_t sig[OTA_SIG_BYTES];
   size_t len = 0;
   ota_manifest_sign(&m, g_sk, raw, sizeof(raw), &len, sig);

   raw[20] ^= 0x01; /* flip a byte in the version field */
   uint8_t keyring[1][OTA_PK_BYTES];
   memcpy(keyring[0], g_pk, OTA_PK_BYTES);
   TEST_ASSERT_EQUAL_INT(FAILURE, ota_manifest_verify(raw, len, sig, keyring, 1, NULL));
}

static void test_verify_rejects_tampered_sig(void) {
   ota_manifest_t m = sample_manifest();
   uint8_t raw[OTA_MANIFEST_WIRE_SIZE];
   uint8_t sig[OTA_SIG_BYTES];
   size_t len = 0;
   ota_manifest_sign(&m, g_sk, raw, sizeof(raw), &len, sig);

   sig[0] ^= 0x01;
   uint8_t keyring[1][OTA_PK_BYTES];
   memcpy(keyring[0], g_pk, OTA_PK_BYTES);
   TEST_ASSERT_EQUAL_INT(FAILURE, ota_manifest_verify(raw, len, sig, keyring, 1, NULL));
}

static void test_verify_rejects_wrong_key(void) {
   ota_manifest_t m = sample_manifest();
   uint8_t raw[OTA_MANIFEST_WIRE_SIZE];
   uint8_t sig[OTA_SIG_BYTES];
   size_t len = 0;
   ota_manifest_sign(&m, g_sk, raw, sizeof(raw), &len, sig);

   uint8_t other_pk[OTA_PK_BYTES], other_sk[OTA_SK_BYTES];
   ota_keygen(other_pk, other_sk);
   uint8_t keyring[1][OTA_PK_BYTES];
   memcpy(keyring[0], other_pk, OTA_PK_BYTES);
   TEST_ASSERT_EQUAL_INT(FAILURE, ota_manifest_verify(raw, len, sig, keyring, 1, NULL));
}

/* Keyring (rotation): correct key present among others → verifies. */
static void test_verify_keyring_rotation(void) {
   ota_manifest_t m = sample_manifest();
   uint8_t raw[OTA_MANIFEST_WIRE_SIZE];
   uint8_t sig[OTA_SIG_BYTES];
   size_t len = 0;
   ota_manifest_sign(&m, g_sk, raw, sizeof(raw), &len, sig);

   uint8_t old_pk[OTA_PK_BYTES], old_sk[OTA_SK_BYTES];
   ota_keygen(old_pk, old_sk);
   uint8_t keyring[2][OTA_PK_BYTES]; /* [0]=old, [1]=current */
   memcpy(keyring[0], old_pk, OTA_PK_BYTES);
   memcpy(keyring[1], g_pk, OTA_PK_BYTES);
   TEST_ASSERT_EQUAL_INT(SUCCESS, ota_manifest_verify(raw, len, sig, keyring, 2, NULL));
}

/* A valid signature over MALFORMED bytes must still be rejected (parse runs
 * after verify, and a bad manifest fails the parse). */
static void test_verify_rejects_signed_garbage(void) {
   uint8_t raw[OTA_MANIFEST_WIRE_SIZE];
   memset(raw, 0x55, sizeof(raw)); /* not a valid manifest (bad magic) */
   uint8_t sig[OTA_SIG_BYTES];
   TEST_ASSERT_EQUAL_INT(0, crypto_sign_detached(sig, NULL, raw, sizeof(raw), g_sk));

   uint8_t keyring[1][OTA_PK_BYTES];
   memcpy(keyring[0], g_pk, OTA_PK_BYTES);
   TEST_ASSERT_EQUAL_INT(FAILURE, ota_manifest_verify(raw, sizeof(raw), sig, keyring, 1, NULL));
}

/* --- semver compare --- */
static void test_semver_compare(void) {
   int r = 99;
   TEST_ASSERT_EQUAL_INT(SUCCESS, ota_semver_compare("2.0.0", "2.0.0", &r));
   TEST_ASSERT_EQUAL_INT(0, r);
   TEST_ASSERT_EQUAL_INT(SUCCESS, ota_semver_compare("2.0.1", "2.0.0", &r));
   TEST_ASSERT_EQUAL_INT(1, r);
   TEST_ASSERT_EQUAL_INT(SUCCESS, ota_semver_compare("2.0.0", "2.0.10", &r));
   TEST_ASSERT_EQUAL_INT(-1, r); /* numeric, not lexical: 0 < 10 */
   TEST_ASSERT_EQUAL_INT(SUCCESS, ota_semver_compare("2.1.0", "2.0.9", &r));
   TEST_ASSERT_EQUAL_INT(1, r);
   TEST_ASSERT_EQUAL_INT(SUCCESS, ota_semver_compare("3.0.0", "2.9.9", &r));
   TEST_ASSERT_EQUAL_INT(1, r);
}

static void test_semver_compare_malformed(void) {
   int r = 0;
   TEST_ASSERT_EQUAL_INT(FAILURE, ota_semver_compare("abc", "2.0.0", &r));
   TEST_ASSERT_EQUAL_INT(FAILURE, ota_semver_compare("2.0.0", "", &r));
}

/* --- rollback / compat policy --- */
static void test_rollback_upgrade_ok(void) {
   ota_manifest_t m = sample_manifest(); /* version 2.1.0, min 2.0.0 */
   TEST_ASSERT_TRUE(ota_manifest_rollback_ok("2.0.5", &m, false));
}

static void test_rollback_same_version_ok(void) {
   ota_manifest_t m = sample_manifest();
   strcpy(m.version, "2.0.5");
   m.min_version[0] = '\0';
   TEST_ASSERT_TRUE(ota_manifest_rollback_ok("2.0.5", &m, false));
}

static void test_rollback_downgrade_blocked(void) {
   ota_manifest_t m = sample_manifest();
   strcpy(m.version, "1.9.0");
   m.min_version[0] = '\0';
   TEST_ASSERT_FALSE(ota_manifest_rollback_ok("2.0.5", &m, false));
   TEST_ASSERT_TRUE(ota_manifest_rollback_ok("2.0.5", &m, true)); /* forced */
}

static void test_rollback_min_version_floor(void) {
   ota_manifest_t m = sample_manifest(); /* min 2.0.0 */
   strcpy(m.version, "2.1.0");
   strcpy(m.min_version, "2.0.0");
   TEST_ASSERT_FALSE(ota_manifest_rollback_ok("1.5.0", &m, false)); /* too old to jump */
   TEST_ASSERT_TRUE(ota_manifest_rollback_ok("2.0.0", &m, false));
}

static void test_rollback_fresh_device_ok(void) {
   ota_manifest_t m = sample_manifest();
   TEST_ASSERT_TRUE(ota_manifest_rollback_ok("", &m, false));
   TEST_ASSERT_TRUE(ota_manifest_rollback_ok(NULL, &m, false));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_sha256_abc);
   RUN_TEST(test_serialize_roundtrip);
   RUN_TEST(test_serialize_buffer_too_small);
   RUN_TEST(test_deserialize_bad_magic);
   RUN_TEST(test_sign_verify_roundtrip);
   RUN_TEST(test_verify_rejects_tampered_payload);
   RUN_TEST(test_verify_rejects_tampered_sig);
   RUN_TEST(test_verify_rejects_wrong_key);
   RUN_TEST(test_verify_keyring_rotation);
   RUN_TEST(test_verify_rejects_signed_garbage);
   RUN_TEST(test_semver_compare);
   RUN_TEST(test_semver_compare_malformed);
   RUN_TEST(test_rollback_upgrade_ok);
   RUN_TEST(test_rollback_same_version_ok);
   RUN_TEST(test_rollback_downgrade_blocked);
   RUN_TEST(test_rollback_min_version_floor);
   RUN_TEST(test_rollback_fresh_device_ok);
   return UNITY_END();
}
