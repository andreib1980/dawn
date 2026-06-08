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
 * OTA signed-manifest core — the trust-model primitive for server→satellite
 * updates (docs/OTA_DESIGN.md §Trust model).  PURE: no globals, no DB, no
 * config, no file I/O — operates on caller buffers only, so it links into a
 * host unit test with just libsodium.  The daemon glue (release-store I/O,
 * device state) lives in ota.c.
 *
 * Wire format is a FIXED, explicitly-packed little-endian binary blob — NOT
 * JSON — so the Ed25519 signature covers canonical bytes with no parser-vs-
 * signature ambiguity.  Verification runs on the raw bytes FIRST; the struct is
 * parsed only after the signature checks out.
 */

#ifndef OTA_MANIFEST_H
#define OTA_MANIFEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dawn_error.h" /* SUCCESS / FAILURE */

#ifdef __cplusplus
extern "C" {
#endif

/* libsodium crypto_sign (Ed25519) sizes — mirrored so callers/tests don't need
 * sodium.h.  Static-asserted against the real values in ota_manifest.c. */
#define OTA_SIG_BYTES 64
#define OTA_PK_BYTES 32
#define OTA_SK_BYTES 64

#define OTA_SHA256_BYTES 32
#define OTA_VERSION_MAX 32 /* semver string incl. NUL */
#define OTA_ABI_TAG_MAX 48 /* e.g. "debian-trixie-aarch64" */

#define OTA_MANIFEST_MAGIC "DAWNOTA1" /* 8 bytes, no NUL stored */
#define OTA_MANIFEST_FMT_VERSION 1
/* Serialized size: 8 magic + 2 fmt + 1 platform + 1 tier + 32 version +
 * 48 abi_tag + 8 image_size + 32 sha256 + 32 min_version. */
#define OTA_MANIFEST_WIRE_SIZE \
   (8 + 2 + 1 + 1 + OTA_VERSION_MAX + OTA_ABI_TAG_MAX + 8 + OTA_SHA256_BYTES + OTA_VERSION_MAX)

typedef enum {
   OTA_PLATFORM_UNKNOWN = 0,
   OTA_PLATFORM_RPI = 1,
   OTA_PLATFORM_ESP32 = 2,
} ota_platform_t;

typedef struct {
   uint16_t fmt_version;              /**< OTA_MANIFEST_FMT_VERSION */
   uint8_t platform;                  /**< ota_platform_t */
   uint8_t tier;                      /**< 1 or 2 */
   char version[OTA_VERSION_MAX];     /**< semver of this image */
   char abi_tag[OTA_ABI_TAG_MAX];     /**< build-target OS/ABI (§10) */
   uint64_t image_size;               /**< bytes of the image artifact */
   uint8_t sha256[OTA_SHA256_BYTES];  /**< SHA-256 of the image artifact */
   char min_version[OTA_VERSION_MAX]; /**< anti-rollback floor ("" = none) */
} ota_manifest_t;

/**
 * @brief Initialize libsodium.  Call once before any other function here.
 * @return SUCCESS or FAILURE
 */
int ota_crypto_init(void);

/**
 * @brief Generate an Ed25519 signing keypair (offline keygen tooling).
 * @param pk Output public key (OTA_PK_BYTES)
 * @param sk Output secret key (OTA_SK_BYTES) — keep offline, never on the daemon
 */
void ota_keygen(uint8_t pk[OTA_PK_BYTES], uint8_t sk[OTA_SK_BYTES]);

/**
 * @brief SHA-256 of a buffer (image integrity).
 */
int ota_sha256(const uint8_t *data, size_t len, uint8_t out[OTA_SHA256_BYTES]);

/**
 * @brief Serialize a manifest to its canonical wire bytes.
 * @param out      Buffer (>= OTA_MANIFEST_WIRE_SIZE)
 * @param out_size Capacity of @p out
 * @param out_len  Written length (= OTA_MANIFEST_WIRE_SIZE) on success
 * @return SUCCESS or FAILURE
 */
int ota_manifest_serialize(const ota_manifest_t *m, uint8_t *out, size_t out_size, size_t *out_len);

/**
 * @brief Parse a manifest from wire bytes.  Validates magic + fmt_version.
 * @note Callers that received the bytes over an untrusted channel MUST verify
 *       the signature first (ota_manifest_verify) — do not call this on
 *       unverified input.
 * @return SUCCESS or FAILURE
 */
int ota_manifest_deserialize(const uint8_t *buf, size_t len, ota_manifest_t *out);

/**
 * @brief Serialize @p m and produce a detached Ed25519 signature (offline sign).
 * @param sk       Secret key
 * @param out      Buffer for the serialized manifest (>= OTA_MANIFEST_WIRE_SIZE)
 * @param out_size Capacity of @p out
 * @param out_len  Written manifest length
 * @param sig      Output detached signature (OTA_SIG_BYTES)
 * @return SUCCESS or FAILURE
 */
int ota_manifest_sign(const ota_manifest_t *m,
                      const uint8_t sk[OTA_SK_BYTES],
                      uint8_t *out,
                      size_t out_size,
                      size_t *out_len,
                      uint8_t sig[OTA_SIG_BYTES]);

/**
 * @brief Verify raw manifest bytes against a public-key RING, then parse.
 *
 * Verifies the detached signature over the EXACT @p raw bytes against each key
 * in @p pubkeys (keyring supports rotation — current + next).  Parses into
 * @p out ONLY if a key validates.  This is the device-side entry point.
 *
 * @param raw      Received manifest bytes (untrusted)
 * @param raw_len  Length of @p raw
 * @param sig      Detached signature (OTA_SIG_BYTES)
 * @param pubkeys  Array of public keys, each OTA_PK_BYTES
 * @param n_keys   Number of keys in @p pubkeys
 * @param out      Parsed manifest (filled only on success; may be NULL to just verify)
 * @return SUCCESS if a key verifies and parse succeeds, FAILURE otherwise
 */
int ota_manifest_verify(const uint8_t *raw,
                        size_t raw_len,
                        const uint8_t sig[OTA_SIG_BYTES],
                        const uint8_t (*pubkeys)[OTA_PK_BYTES],
                        size_t n_keys,
                        ota_manifest_t *out);

/**
 * @brief Compare two semver strings numerically (major.minor.patch).
 * @param result Output: -1 if a<b, 0 if equal, 1 if a>b
 * @return SUCCESS if both parsed, FAILURE on malformed input
 */
int ota_semver_compare(const char *a, const char *b, int *result);

/**
 * @brief Anti-rollback / compatibility decision for applying @p m over @p installed.
 *
 * Rejects a downgrade (manifest version < installed) unless @p allow_downgrade,
 * and rejects when @p installed is below the manifest's min_version floor.
 *
 * @return true if the update may be applied, false otherwise
 */
bool ota_manifest_rollback_ok(const char *installed, const ota_manifest_t *m, bool allow_downgrade);

#ifdef __cplusplus
}
#endif

#endif /* OTA_MANIFEST_H */
