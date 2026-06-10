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
 * OTA apply — internal decision surface.
 *
 * The accept/reject decision for an incoming ota_offer (field validation, url
 * allowlist, signature verify, offer↔manifest cross-check, abi match, already-
 * current + downgrade/floor gates) lives in ota_apply_decide() in
 * ota_apply_decide.c — pure (libsodium + json-c + the manifest codec, NO
 * ws_client / curl / threads) so it is host-unit-testable (test_ota_apply).
 * ota_apply.c does the I/O: load the keyring, compute the device abi, then on
 * ACCEPT spawn the download/swap worker.
 */

#ifndef OTA_APPLY_INTERNAL_H
#define OTA_APPLY_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "core/ota_manifest.h"

struct json_object;

#ifdef __cplusplus
extern "C" {
#endif

/* Operator-provisioned Ed25519 verify keyring.  Loaded at runtime from a
 * root-owned, read-only file OUTSIDE the satellite's ReadWritePaths — so the
 * dawn-uid service can read it but never modify the trust anchor, and so a
 * prebuilt binary can be shipped without baking in anyone's key (each operator
 * drops their own pubkey here).  Path is hardcoded (NOT config): the satellite
 * config is dawn-writable, so a configurable path would let a dawn compromise
 * repoint the trust anchor.  See docs/OTA_DESIGN.md §Trust model. */
#ifndef OTA_KEYRING_PATH /* overridable only for host tests; production = the default */
#define OTA_KEYRING_PATH "/etc/dawn/ota_pubkey"
#endif
#define OTA_KEYRING_MAX 4 /* current + next + a little rotation headroom */

#define OTA_DETAIL_BUF 160
/* These two mirror server-side sizes across the daemon↔satellite boundary (the
 * satellite can't include daemon headers, so a static_assert can't span them —
 * keep in sync with the named symbols if the server side ever changes). */
#define OTA_OFFER_TOKEN_HEX \
   64 /* 32-byte one-time download token, hex; mirrors include/core/ota.h OTA_TOKEN_HEX */
#define OTA_URL_PATH_MAX \
   256 /* offer image url_path bound; mirrors ota_offer_t.url_path[] in include/core/ota.h */

/* Outcome of ota_apply_decide(). */
typedef enum {
   OTA_DECIDE_ACCEPT = 0, /* offer is valid + applicable; *out_m is filled */
   OTA_DECIDE_REJECT,     /* rejected; reason holds a human-readable detail */
} ota_decide_t;

/**
 * @brief Decide whether to accept an incoming ota_offer — the pure half of
 *        ota_apply_handle_offer (no ws_client / curl / threads / globals).
 *
 * Validates field presence/lengths, the url_path allowlist, hex decoding;
 * verifies the signed manifest against @p keyring; cross-checks the unsigned
 * offer against the signed values; matches @p device_abi; rejects an
 * already-current version; and applies the anti-rollback / anti-skip-floor gate
 * against @p installed_version.
 *
 * @param payload           the ota_offer JSON
 * @param installed_version the device's running firmware version
 * @param device_abi        the device's computed abi tag (must equal the manifest's)
 * @param keyring           operator pubkeys (caller loads them); n_keys==0 → reject
 * @param n_keys            number of valid keys in @p keyring
 * @param out_m             filled with the verified manifest on ACCEPT
 * @param reason            on REJECT, a human-readable detail (may be empty)
 * @param reason_size       size of @p reason
 * @return OTA_DECIDE_ACCEPT or OTA_DECIDE_REJECT
 */
ota_decide_t ota_apply_decide(struct json_object *payload,
                              const char *installed_version,
                              const char *device_abi,
                              const uint8_t keyring[][OTA_PK_BYTES],
                              size_t n_keys,
                              ota_manifest_t *out_m,
                              char *reason,
                              size_t reason_size);

/**
 * @brief Load the operator keyring from OTA_KEYRING_PATH (one 64-hex Ed25519
 *        pubkey per line; '#'/blank lines and malformed lines skipped).
 * @return number of keys parsed (0 → caller MUST fail closed).
 */
size_t load_keyring(uint8_t keys[][OTA_PK_BYTES], size_t max);

/**
 * @brief Join the abi components into "id-codename-machine" (bounded; an
 *        over-long/missing field truncates → an abi that can't match → safe reject).
 */
void build_abi_tag(const char *id,
                   const char *codename,
                   const char *machine,
                   char *out,
                   size_t out_size);

/**
 * @brief Compute this device's abi tag from /etc/os-release + uname().
 */
void compute_abi_tag(char *out, size_t out_size);

/** @brief Fetch a string field from a json object, or NULL if absent. */
const char *ota_jstr(struct json_object *o, const char *k);

#ifdef __cplusplus
}
#endif

#endif /* OTA_APPLY_INTERNAL_H */
