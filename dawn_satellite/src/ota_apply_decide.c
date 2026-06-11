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
 * OTA apply — pure offer-decision logic (see ota_apply_internal.h).  Split out
 * of ota_apply.c so the accept/reject path is host-unit-testable without
 * ws_client / curl / threads.  Deps: libsodium + json-c + the manifest codec.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* fopen("re") */
#endif

#include <ctype.h>
#include <json-c/json.h>
#include <sodium.h>
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>

#include "core/ota_manifest.h"
#include "dawn_error.h"
#include "ota_apply_internal.h"
#include "ota_marker.h" /* OTA_MAX_IMAGE_BYTES (shared OTA policy constants) */

const char *ota_jstr(struct json_object *o, const char *k) {
   struct json_object *v = NULL;
   if (o && json_object_object_get_ex(o, k, &v)) {
      return json_object_get_string(v);
   }
   return NULL;
}

static int hexdecode(const char *hex, uint8_t *out, size_t out_len) {
   size_t bin_len = 0;
   if (sodium_hex2bin(out, out_len, hex, strlen(hex), NULL, &bin_len, NULL) != 0 ||
       bin_len != out_len) {
      return FAILURE;
   }
   return SUCCESS;
}

/* url_path allowlist: must be the OTA image route, no traversal. */
static int url_path_ok(const char *p) {
   if (!p || strncmp(p, "/api/ota/", 9) != 0 || strlen(p) >= OTA_URL_PATH_MAX) {
      return 0;
   }
   if (strstr(p, "..")) {
      return 0;
   }
   for (const char *c = p; *c; c++) {
      if (!((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') ||
            *c == '.' || *c == '_' || *c == '-' || *c == '/')) {
         return 0;
      }
   }
   return 1;
}

size_t load_keyring(uint8_t keys[][OTA_PK_BYTES], size_t max) {
   FILE *f = fopen(OTA_KEYRING_PATH, "re");
   if (!f) {
      return 0;
   }
   char line[160];
   size_t n = 0;
   while (n < max && fgets(line, sizeof(line), f)) {
      char *p = line;
      while (*p == ' ' || *p == '\t') {
         p++;
      }
      size_t len = strlen(p);
      while (len > 0 && (p[len - 1] == '\n' || p[len - 1] == '\r' || p[len - 1] == ' ' ||
                         p[len - 1] == '\t')) {
         p[--len] = '\0';
      }
      if (len == 0 || p[0] == '#') {
         continue; /* blank / comment */
      }
      if (len != OTA_PK_BYTES * 2) {
         continue; /* not a 64-hex key line */
      }
      size_t bin_len = 0;
      if (sodium_hex2bin(keys[n], OTA_PK_BYTES, p, len, NULL, &bin_len, NULL) == 0 &&
          bin_len == OTA_PK_BYTES) {
         n++;
      }
   }
   fclose(f);
   return n;
}

void build_abi_tag(const char *id,
                   const char *codename,
                   const char *machine,
                   char *out,
                   size_t out_size) {
   snprintf(out, out_size, "%s-%s-%s", id ? id : "", codename ? codename : "",
            machine ? machine : "");
}

static void os_release_field(const char *key, char *out, size_t out_size) {
   out[0] = '\0';
   FILE *f = fopen("/etc/os-release", "re");
   if (!f) {
      return;
   }
   char line[256];
   size_t klen = strlen(key);
   while (fgets(line, sizeof(line), f)) {
      if (strncmp(line, key, klen) != 0 || line[klen] != '=') {
         continue;
      }
      char *v = line + klen + 1;
      /* strip newline + surrounding quotes */
      size_t n = strlen(v);
      while (n > 0 && (v[n - 1] == '\n' || v[n - 1] == '\r')) {
         v[--n] = '\0';
      }
      if (n >= 2 && v[0] == '"' && v[n - 1] == '"') {
         v[n - 1] = '\0';
         v++;
      }
      snprintf(out, out_size, "%s", v);
      break;
   }
   fclose(f);
}

void compute_abi_tag(char *out, size_t out_size) {
   /* Components are bounded well under OTA_ABI_TAG_MAX so the join can't
    * overflow (real values: debian / trixie / aarch64).  An over-long /
    * missing field truncates → an abi that can't match a signed manifest →
    * safe reject.  The signer's --abi-tag must use the same short recipe. */
   char id[16] = "", codename[16] = "", machine[16] = "";
   os_release_field("ID", id, sizeof(id));
   os_release_field("VERSION_CODENAME", codename, sizeof(codename));
   if (codename[0] == '\0') {
      os_release_field("VERSION_ID", codename, sizeof(codename));
   }
   struct utsname u;
   if (uname(&u) == 0) {
      snprintf(machine, sizeof(machine), "%.*s", (int)(sizeof(machine) - 1), u.machine);
   }
   build_abi_tag(id, codename, machine, out, out_size);
}

/* Small helper: set reason + return reject. */
static ota_decide_t reject(char *reason, size_t reason_size, const char *msg) {
   if (reason && reason_size) {
      snprintf(reason, reason_size, "%s", msg);
   }
   return OTA_DECIDE_REJECT;
}

ota_decide_t ota_apply_decide(struct json_object *payload,
                              const char *installed_version,
                              const char *device_abi,
                              const uint8_t keyring[][OTA_PK_BYTES],
                              size_t n_keys,
                              ota_manifest_t *out_m,
                              char *reason,
                              size_t reason_size) {
   const char *url_path = ota_jstr(payload, "url_path");
   const char *token = ota_jstr(payload, "token");
   const char *sha_hex = ota_jstr(payload, "sha256");
   const char *man_hex = ota_jstr(payload, "manifest");
   const char *sig_hex = ota_jstr(payload, "sig");
   struct json_object *jsz = NULL, *jdg = NULL;
   int64_t image_size = (payload && json_object_object_get_ex(payload, "image_size", &jsz))
                            ? json_object_get_int64(jsz)
                            : 0;
   bool allow_downgrade = (payload &&
                           json_object_object_get_ex(payload, "allow_downgrade", &jdg)) &&
                          json_object_get_boolean(jdg);

   /* Field presence + length validation. */
   if (!url_path || !token || !sha_hex || !man_hex || !sig_hex || !installed_version ||
       strlen(token) != OTA_OFFER_TOKEN_HEX || strlen(sha_hex) != OTA_SHA256_BYTES * 2 ||
       strlen(man_hex) != OTA_MANIFEST_WIRE_SIZE * 2 || strlen(sig_hex) != OTA_SIG_BYTES * 2 ||
       image_size <= 0 || image_size > OTA_MAX_IMAGE_BYTES) {
      return reject(reason, reason_size, "malformed offer");
   }
   if (!url_path_ok(url_path)) {
      return reject(reason, reason_size, "bad url_path");
   }
   /* The token is interpolated raw into the download URL query string (unlike
    * sha/manifest/sig, which are hex-decoded), so validate its charset here —
    * mirroring the uuid guard in ota_apply.c — to keep a stray '&'/'#'/space from
    * splitting or truncating the query.  It is documented as a 32-byte hex token. */
   for (const char *p = token; *p; p++) {
      if (!isxdigit((unsigned char)*p)) {
         return reject(reason, reason_size, "bad token");
      }
   }

   uint8_t raw[OTA_MANIFEST_WIRE_SIZE], sig[OTA_SIG_BYTES], sha[OTA_SHA256_BYTES];
   if (hexdecode(man_hex, raw, sizeof(raw)) != SUCCESS ||
       hexdecode(sig_hex, sig, sizeof(sig)) != SUCCESS ||
       hexdecode(sha_hex, sha, sizeof(sha)) != SUCCESS) {
      return reject(reason, reason_size, "bad hex encoding");
   }

   if (ota_crypto_init() != SUCCESS) {
      return reject(reason, reason_size, "crypto init failed");
   }

   /* Fail closed if the operator hasn't provisioned a keyring. */
   if (n_keys == 0) {
      return reject(reason, reason_size, "no OTA keyring provisioned (/etc/dawn/ota_pubkey)");
   }

   /* Verify the signed manifest BEFORE trusting any of it (verify-before-parse). */
   ota_manifest_t m;
   if (ota_manifest_verify(raw, sizeof(raw), sig, keyring, n_keys, &m) != SUCCESS) {
      return reject(reason, reason_size, "bad signature");
   }

   /* Cross-check the unsigned offer against the SIGNED manifest; trust only signed values. */
   if ((uint64_t)image_size != m.image_size ||
       sodium_memcmp(sha, m.sha256, OTA_SHA256_BYTES) != 0 || m.platform != OTA_PLATFORM_RPI ||
       m.tier != 1) {
      return reject(reason, reason_size, "offer/manifest mismatch");
   }

   /* abi_tag must match this device's computed tag (blocks cross-flashing). */
   if (!device_abi || strncmp(device_abi, m.abi_tag, OTA_ABI_TAG_MAX) != 0) {
      char d[OTA_DETAIL_BUF];
      snprintf(d, sizeof(d), "abi mismatch: dev=%s img=%s", device_abi ? device_abi : "",
               m.abi_tag);
      return reject(reason, reason_size, d);
   }

   if (strcmp(m.version, installed_version) == 0) {
      return reject(reason, reason_size, "already on this version");
   }
   if (!ota_manifest_rollback_ok(installed_version, &m, allow_downgrade)) {
      /* Distinguish the two failure modes so the operator knows which gate tripped:
       * an anti-rollback block (candidate older than installed) vs an anti-skip floor
       * (installed below the manifest's required min_version milestone). */
      int cmp = 0;
      char d[OTA_DETAIL_BUF];
      if (ota_semver_compare(m.version, installed_version, &cmp) == SUCCESS && cmp < 0) {
         snprintf(d, sizeof(d), "downgrade blocked: img=%s installed=%s", m.version,
                  installed_version);
      } else {
         snprintf(d, sizeof(d), "installed=%s below required floor min_version=%s",
                  installed_version, m.min_version);
      }
      return reject(reason, reason_size, d);
   }

   if (out_m) {
      *out_m = m;
   }
   return OTA_DECIDE_ACCEPT;
}
