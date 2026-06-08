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
 * OTA daemon glue — release store + update orchestration.  See ota.h.
 */

#include "core/ota.h"

#include <dirent.h>
#include <limits.h>
#include <sodium.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "config/dawn_config.h"
#include "core/ota_db.h"
#include "logging.h"

#define OTA_MAX_RELEASES 64
#define OTA_PATH_MAX 1024
/* In-flight rows older than this on startup are presumed orphaned by a restart. */
#define OTA_STALE_RECONCILE_SEC 600

typedef struct {
   ota_platform_t platform;
   int tier;
   char version[OTA_VERSION_MAX];
   char dir[OTA_PATH_MAX]; /* <release_dir>/<platform>/<version> */
   ota_manifest_t manifest;
} ota_release_t;

static ota_release_t s_releases[OTA_MAX_RELEASES];
static int s_release_count = 0;
static bool s_enabled = false;

static const char *platform_to_str(ota_platform_t p) {
   switch (p) {
      case OTA_PLATFORM_RPI:
         return "rpi";
      case OTA_PLATFORM_ESP32:
         return "esp32";
      default:
         return NULL;
   }
}

static ota_platform_t str_to_platform(const char *s) {
   if (s && strcmp(s, "rpi") == 0) {
      return OTA_PLATFORM_RPI;
   }
   if (s && strcmp(s, "esp32") == 0) {
      return OTA_PLATFORM_ESP32;
   }
   return OTA_PLATFORM_UNKNOWN;
}

/* Path components are attacker-influenced on the download route: allow only a
 * safe charset and reject any "." traversal token or separator. */
static bool version_is_safe(const char *v) {
   if (!v || v[0] == '\0' || strlen(v) >= OTA_VERSION_MAX) {
      return false;
   }
   if (strstr(v, "..") != NULL) {
      return false;
   }
   for (const char *p = v; *p; p++) {
      unsigned char c = (unsigned char)*p;
      bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                c == '.' || c == '-' || c == '_';
      if (!ok) {
         return false;
      }
   }
   return true;
}

static long read_exact_file(const char *path, uint8_t *buf, size_t max) {
   FILE *f = fopen(path, "rb");
   if (!f) {
      return -1;
   }
   size_t n = fread(buf, 1, max, f);
   fclose(f);
   return (long)n;
}

/* Load <release_dir>/<platform>/<version>/manifest for one version dir. */
static void load_one_release(const char *release_dir,
                             const char *platform_str,
                             const char *version) {
   if (s_release_count >= OTA_MAX_RELEASES) {
      return;
   }
   if (!version_is_safe(version)) {
      return;
   }
   ota_platform_t platform = str_to_platform(platform_str);
   if (platform == OTA_PLATFORM_UNKNOWN) {
      return;
   }

   char dir[OTA_PATH_MAX];
   char mpath[OTA_PATH_MAX + 16]; /* dir + "/manifest" */
   snprintf(dir, sizeof(dir), "%s/%s/%s", release_dir, platform_str, version);
   snprintf(mpath, sizeof(mpath), "%s/manifest", dir);

   uint8_t raw[OTA_MANIFEST_WIRE_SIZE];
   long n = read_exact_file(mpath, raw, sizeof(raw));
   if (n != (long)OTA_MANIFEST_WIRE_SIZE) {
      return; /* missing/short manifest — skip */
   }

   ota_release_t *r = &s_releases[s_release_count];
   if (ota_manifest_deserialize(raw, (size_t)n, &r->manifest) != SUCCESS) {
      OLOG_WARNING("ota: malformed manifest in %s — skipping", dir);
      return;
   }
   /* Trust the operator's signed artifact for metadata; the DEVICE verifies the
    * signature.  Sanity-check the dir name matches the manifest's platform. */
   if (r->manifest.platform != platform) {
      OLOG_WARNING("ota: %s manifest platform mismatch — skipping", dir);
      return;
   }
   r->platform = platform;
   r->tier = r->manifest.tier;
   snprintf(r->version, sizeof(r->version), "%s", version);
   snprintf(r->dir, sizeof(r->dir), "%s", dir);
   s_release_count++;
   OLOG_INFO("ota: release %s tier=%d %s", platform_str, r->tier, version);
}

static void scan_releases(const char *release_dir) {
   s_release_count = 0;
   const char *platforms[] = { "rpi", "esp32" };
   for (size_t i = 0; i < sizeof(platforms) / sizeof(platforms[0]); i++) {
      char pdir[OTA_PATH_MAX];
      snprintf(pdir, sizeof(pdir), "%s/%s", release_dir, platforms[i]);
      DIR *d = opendir(pdir);
      if (!d) {
         continue; /* platform has no releases yet */
      }
      struct dirent *e;
      while ((e = readdir(d)) != NULL) {
         if (e->d_name[0] == '.') {
            continue;
         }
         load_one_release(release_dir, platforms[i], e->d_name);
      }
      closedir(d);
   }
}

int ota_init(void) {
   s_enabled = false;
   s_release_count = 0;

   if (!g_config.ota.enabled) {
      OLOG_INFO("ota: disabled");
      return SUCCESS;
   }
   if (g_config.ota.release_dir[0] == '\0') {
      OLOG_ERROR("ota: enabled but release_dir is empty");
      return FAILURE;
   }
   if (ota_crypto_init() != SUCCESS) {
      OLOG_ERROR("ota: crypto init failed");
      return FAILURE;
   }

   scan_releases(g_config.ota.release_dir);

   /* Reconcile in-flight rows orphaned by a restart. */
   int reconciled = 0;
   ota_db_reconcile_stale(time(NULL) - OTA_STALE_RECONCILE_SEC, &reconciled);
   if (reconciled > 0) {
      OLOG_INFO("ota: reconciled %d stale in-flight row(s) to 'unknown'", reconciled);
   }

   s_enabled = true;
   OLOG_INFO("ota: enabled, %d release(s) from %s", s_release_count, g_config.ota.release_dir);
   return SUCCESS;
}

bool ota_enabled(void) {
   return s_enabled;
}

static const ota_release_t *find_release(ota_platform_t platform, const char *version) {
   for (int i = 0; i < s_release_count; i++) {
      if (s_releases[i].platform == platform && strcmp(s_releases[i].version, version) == 0) {
         return &s_releases[i];
      }
   }
   return NULL;
}

int ota_resolve_latest(ota_platform_t platform, int tier, char *out_version, size_t out_size) {
   if (!out_version || out_size == 0) {
      return FAILURE;
   }
   const ota_release_t *best = NULL;
   for (int i = 0; i < s_release_count; i++) {
      const ota_release_t *r = &s_releases[i];
      if (r->platform != platform || r->tier != tier) {
         continue;
      }
      if (best == NULL) {
         best = r;
         continue;
      }
      int cmp = 0;
      if (ota_semver_compare(r->version, best->version, &cmp) == SUCCESS && cmp > 0) {
         best = r;
      }
   }
   if (!best) {
      return FAILURE;
   }
   snprintf(out_version, out_size, "%s", best->version);
   return SUCCESS;
}

int ota_release_count(void) {
   return s_release_count;
}

int ota_release_info(int index,
                     ota_platform_t *platform_out,
                     int *tier_out,
                     char *version_out,
                     size_t version_size) {
   if (index < 0 || index >= s_release_count) {
      return FAILURE;
   }
   const ota_release_t *r = &s_releases[index];
   if (platform_out) {
      *platform_out = r->platform;
   }
   if (tier_out) {
      *tier_out = r->tier;
   }
   if (version_out && version_size > 0) {
      snprintf(version_out, version_size, "%s", r->version);
   }
   return SUCCESS;
}

int ota_begin_push(const char *uuid,
                   ota_platform_t platform,
                   const char *version,
                   bool allow_downgrade,
                   ota_offer_t *out) {
   if (!s_enabled || !uuid || !uuid[0] || !version || !out) {
      return FAILURE;
   }
   const char *platform_str = platform_to_str(platform);
   if (!platform_str) {
      return FAILURE;
   }
   const ota_release_t *r = find_release(platform, version);
   if (!r) {
      OLOG_WARNING("ota: no release %s/%s to push", platform_str, version);
      return FAILURE;
   }

   /* One-time download token. */
   uint8_t tok[OTA_TOKEN_BYTES];
   randombytes_buf(tok, sizeof(tok));
   char tok_hex[OTA_TOKEN_HEX + 1];
   sodium_bin2hex(tok_hex, sizeof(tok_hex), tok, sizeof(tok));

   time_t expires = time(NULL) + g_config.ota.download_token_ttl_sec;
   int rc = ota_db_begin_offer(uuid, version, tok_hex, expires);
   if (rc == AUTH_DB_LOCKED) {
      return AUTH_DB_LOCKED;
   }
   if (rc != AUTH_DB_SUCCESS) {
      return FAILURE;
   }

   memset(out, 0, sizeof(*out));
   snprintf(out->platform_str, sizeof(out->platform_str), "%s", platform_str);
   snprintf(out->version, sizeof(out->version), "%s", version);
   snprintf(out->url_path, sizeof(out->url_path), "/api/ota/%s/%s/image", platform_str, version);
   snprintf(out->token, sizeof(out->token), "%s", tok_hex);
   sodium_bin2hex(out->sha256_hex, sizeof(out->sha256_hex), r->manifest.sha256, OTA_SHA256_BYTES);
   out->image_size = r->manifest.image_size;
   out->allow_downgrade = allow_downgrade;

   /* Inline the (tiny) manifest + signature so the device can verify before it
    * pulls the image; only the image goes over the token-gated HTTPS route. */
   uint8_t raw[OTA_MANIFEST_WIRE_SIZE];
   size_t raw_len = 0;
   if (ota_manifest_serialize(&r->manifest, raw, sizeof(raw), &raw_len) != SUCCESS) {
      return FAILURE;
   }
   sodium_bin2hex(out->manifest_hex, sizeof(out->manifest_hex), raw, raw_len);

   char sigpath[OTA_PATH_MAX + 16]; /* dir + "/manifest.sig" */
   snprintf(sigpath, sizeof(sigpath), "%s/manifest.sig", r->dir);
   uint8_t sig[OTA_SIG_BYTES];
   if (read_exact_file(sigpath, sig, sizeof(sig)) != (long)OTA_SIG_BYTES) {
      OLOG_ERROR("ota: missing/short signature %s", sigpath);
      return FAILURE;
   }
   sodium_bin2hex(out->sig_hex, sizeof(out->sig_hex), sig, sizeof(sig));

   OLOG_INFO("ota: offer %s/%s to %s (token ttl=%ds)", platform_str, version, uuid,
             g_config.ota.download_token_ttl_sec);
   return SUCCESS;
}

int ota_authorize_image_download(const char *uuid,
                                 const char *platform_str,
                                 const char *version,
                                 const char *token,
                                 char *out_path,
                                 size_t out_size) {
   if (!s_enabled || !uuid || !uuid[0] || !platform_str || !version || !token || !out_path) {
      return FAILURE;
   }
   /* Validate path components BEFORE consuming the token (a malformed path must
    * not burn the device's one-time token). */
   if (str_to_platform(platform_str) == OTA_PLATFORM_UNKNOWN || !version_is_safe(version)) {
      return FAILURE;
   }

   if (ota_db_consume_token(uuid, version, token, time(NULL)) != AUTH_DB_SUCCESS) {
      OLOG_WARNING("ota: download token rejected for %s %s/%s", uuid, platform_str, version);
      return FAILURE;
   }

   char raw[OTA_PATH_MAX];
   snprintf(raw, sizeof(raw), "%s/%s/%s/image", g_config.ota.release_dir, platform_str, version);

   /* Defense in depth: canonicalize and confirm the resolved path is inside the
    * release dir (realpath also requires the image to exist). */
   char canon_base[PATH_MAX];
   char canon_path[PATH_MAX];
   if (!realpath(g_config.ota.release_dir, canon_base) || !realpath(raw, canon_path)) {
      return FAILURE;
   }
   size_t base_len = strlen(canon_base);
   if (strncmp(canon_path, canon_base, base_len) != 0 ||
       (canon_path[base_len] != '/' && canon_path[base_len] != '\0')) {
      OLOG_ERROR("ota: path escape blocked: %s", canon_path);
      return FAILURE;
   }

   snprintf(out_path, out_size, "%s", canon_path);
   return SUCCESS;
}

int ota_report_status(const char *uuid, const char *state, const char *error) {
   if (!uuid || !uuid[0] || !state || !state[0]) {
      return FAILURE;
   }
   return ota_db_set_state(uuid, state, error);
}

void ota_finalize_on_register(const char *uuid, const char *reported_version) {
   if (!uuid || !uuid[0] || !reported_version || !reported_version[0]) {
      return;
   }
   ota_device_state_t st;
   if (ota_db_get(uuid, &st) != AUTH_DB_SUCCESS) {
      return;
   }
   /* Server-owned commit: only when the device reconnects reporting exactly the
    * version we targeted.  (Connecting alone does not prove a good image, so we
    * never let the device self-declare success.) */
   if (st.target_version[0] != '\0' && strcmp(reported_version, st.target_version) == 0) {
      ota_db_clear_target(uuid, OTA_STATE_SUCCESS);
      OLOG_INFO("ota: %s updated to %s (committed)", uuid, reported_version);
   }
}
