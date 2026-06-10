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
 * OTA device-apply (Tier 2, ESP32-S3) — docs/OTA_DESIGN.md §8.
 *
 * Reboot-first design.  At offer time (full heap, WS up) the manifest signature
 * is Ed25519-verified and the authenticated metadata is stored in NVS; the
 * device reboots.  A minimal early-setup() path (WiFi only, before audio/WS so
 * there is no concurrent TLS) downloads the image into the inactive OTA
 * partition, SHA-256-checks it, switches the boot partition, and reboots into
 * the new image (PENDING-VERIFY).  Rollback is an NVS boot-count guard: if the
 * new image boots but can't re-register within OTA_ESP_ROLLBACK_THRESHOLD
 * boots, the guard reverts to the old slot (the stock Arduino bootloader has no
 * native app-rollback, so this guard is the safety net — see OTA_DESIGN §8).
 *
 * References the main sketch's webSocket + persistentUUID (extern) and
 * updateTFTStatus(); server address + WiFi creds come from arduino_secrets.h
 * (SECRET_* macros), the CA from ca_cert.h, the verify key from ota_pubkey.h,
 * and FIRMWARE_VERSION / OTA_* config from satellite_version.h + ota_apply.h.
 */

/* This is a real C++ translation unit (.cpp), NOT a second .ino — Arduino's
 * auto-prototype generator only rewrites .ino files, and hoisting a prototype
 * for a function that takes a sketch-defined type (ota_rec_t) above its typedef
 * breaks the build (and perturbs the main sketch's own prototypes).  As a normal
 * .cpp it compiles with ordinary include/declaration semantics. */
#include "ota_apply.h"

#include <Adafruit_ST7789.h> /* ST77XX_* color constants */
#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WebSocketsClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_random.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <mbedtls/sha256.h>

#include "arduino_secrets.h" /* SECRET_SSID / SECRET_PASSWORD / SECRET_SERVER_IP / SECRET_SERVER_PORT */
#include "ca_cert.h"         /* CA_CERT_PEM */
#include "ota_pubkey.h"      /* OTA_PUBKEY_HEX / OTA_PUBKEY_COUNT */

/* Globals owned by the main sketch (dawn_satellite_arduino.ino), referenced here
 * across translation units — so they must have external linkage there. */
extern WebSocketsClient webSocket;
extern char persistentUUID[];
void updateTFTStatus(const char *message, uint16_t color, bool clearScreen = false);

/* Image cap must fit the app slot (defines from ota_apply.h). */
static_assert(OTA_MAX_IMAGE_BYTES < OTA_APP_SLOT_BYTES, "OTA image cap must fit the app slot");
/* At least one verify key must be baked, or no OTA could ever be accepted. */
static_assert(OTA_PUBKEY_COUNT >= 1, "ota_pubkey.h must define at least one OTA verify key");

/* Greppable version marker baked into the .bin so ota-release.sh's version gate
 * can read the compiled version (parity with the Tier-1 RPi binary).  `used`
 * keeps it past the compiler; arduino-esp32 links with --gc-sections which would
 * still drop it unreferenced, so ota_boot_path() prints it (a real reference). */
__attribute__((used)) const char dawn_sat_fw_marker[] = DAWN_SAT_FW_VERSION_MARKER;

/* TweetNaCl (vendored, public domain — see ota_pubkey.h.example / DEPENDENCIES.md).
 * Verify-only.  We do NOT #include "tweetnacl.h" (it declares the API with C++
 * linkage here and macro-aliases the names); instead we declare the REAL
 * exported symbol directly with C linkage.  tweetnacl.h macro-maps
 * crypto_sign_open → crypto_sign_ed25519_tweet_open, so that suffixed name is
 * what tweetnacl.c actually exports.  It verifies sig‖msg against the public key
 * and writes the recovered message to m.  tweetnacl.c references randombytes()
 * (unused on the verify path) so we provide a stub to satisfy the linker. */
extern "C" {
int crypto_sign_ed25519_tweet_open(unsigned char *m,
                                   unsigned long long *mlen,
                                   const unsigned char *sm,
                                   unsigned long long smlen,
                                   const unsigned char *pk);
void randombytes(unsigned char *p, unsigned long long n) {
   esp_fill_random(p, (size_t)n);
}
}

/* ── OTA manifest wire layout (must mirror src/core/ota_manifest.c exactly) ──── */
#define OTA_MAN_WIRE 164
#define OTA_MAN_MAGIC "DAWNOTA1"
#define OTA_MAN_OFF_FMT 8       /* u16le */
#define OTA_MAN_OFF_PLATFORM 10 /* u8 */
#define OTA_MAN_OFF_TIER 11     /* u8 */
#define OTA_MAN_OFF_VERSION 12  /* char[32] */
#define OTA_MAN_OFF_ABI 44      /* char[48] */
#define OTA_MAN_OFF_SIZE 92     /* u64le */
#define OTA_MAN_OFF_SHA 100     /* u8[32] */
#define OTA_MAN_OFF_MINVER 132  /* char[32] */
#define OTA_PLATFORM_ESP32 2

#define OTA_NVS_NS "dawnota"
#define OTA_NVS_KEY "rec"
#define OTA_DL_CHUNK 4096

/* Persisted OTA record (one putBytes blob — atomic-ish, minimal NVS wear). */
typedef struct {
   char state;            /* 'D' download pending, 'V' verify pending */
   uint8_t boots;         /* verify-state boot count */
   uint64_t image_size;   /* authenticated (from the signed manifest) */
   uint8_t sha256[32];    /* authenticated image hash */
   char version[32];      /* target version */
   char url_path[160];    /* image route from the offer (allowlisted) */
   char token[80];        /* one-time download token from the offer */
   char revert_label[17]; /* label of the known-good slot to roll back TO */
} ota_rec_t;

/* ── helpers ─────────────────────────────────────────────────────────────── */

static uint16_t ota_u16le(const uint8_t *p) {
   return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static uint64_t ota_u64le(const uint8_t *p) {
   uint64_t v = 0;
   for (int i = 7; i >= 0; i--) {
      v = (v << 8) | p[i];
   }
   return v;
}

/* Strict hex → bytes; returns true only on exactly out_len decoded bytes. */
static bool ota_hex2bin(const char *hex, uint8_t *out, size_t out_len) {
   if (!hex || strlen(hex) != out_len * 2) {
      return false;
   }
   for (size_t i = 0; i < out_len; i++) {
      int hi = hex[i * 2], lo = hex[i * 2 + 1];
      hi = (hi >= '0' && hi <= '9')   ? hi - '0'
           : (hi >= 'a' && hi <= 'f') ? hi - 'a' + 10
           : (hi >= 'A' && hi <= 'F') ? hi - 'A' + 10
                                      : -1;
      lo = (lo >= '0' && lo <= '9')   ? lo - '0'
           : (lo >= 'a' && lo <= 'f') ? lo - 'a' + 10
           : (lo >= 'A' && lo <= 'F') ? lo - 'A' + 10
                                      : -1;
      if (hi < 0 || lo < 0) {
         return false;
      }
      out[i] = (uint8_t)((hi << 4) | lo);
   }
   return true;
}

/* Parse up to 3 numeric components of "X.Y.Z[-suffix]". */
static void ota_semver(const char *s, long v[3]) {
   v[0] = v[1] = v[2] = 0;
   for (int i = 0; i < 3 && s && *s; i++) {
      char *end = NULL;
      long n = strtol(s, &end, 10);
      if (end == s) {
         break;
      }
      v[i] = n;
      s = (*end == '.') ? end + 1 : "";
   }
}
/* -1 if a<b, 0 if equal, 1 if a>b (numeric semver). */
static int ota_semver_cmp(const char *a, const char *b) {
   long va[3], vb[3];
   ota_semver(a, va);
   ota_semver(b, vb);
   for (int i = 0; i < 3; i++) {
      if (va[i] < vb[i]) {
         return -1;
      }
      if (va[i] > vb[i]) {
         return 1;
      }
   }
   return 0;
}

/* Ed25519 verify the detached sig over the manifest, against any baked operator
 * pubkey. Stack buffers (no heap on the constrained offer path); the loop-task
 * stack is bumped via SET_LOOP_TASK_STACK_SIZE for the TweetNaCl call depth. */
static bool ota_verify_manifest(const uint8_t *sig64, const uint8_t *manifest) {
   uint8_t sm[64 + OTA_MAN_WIRE]; /* signed message: sig ‖ manifest */
   uint8_t m[64 + OTA_MAN_WIRE];  /* crypto_sign_open output scratch */
   memcpy(sm, sig64, 64);
   memcpy(sm + 64, manifest, OTA_MAN_WIRE);

   for (size_t k = 0; k < OTA_PUBKEY_COUNT; k++) {
      uint8_t pk[32];
      if (!ota_hex2bin(OTA_PUBKEY_HEX[k], pk, sizeof(pk))) {
         continue; /* malformed baked key — skip */
      }
      /* Reject the all-zeros placeholder key (ota_pubkey.h.example default): its
       * private half is universally known, so a device flashed before the operator
       * baked a real key must NOT trust it.  Skipping it makes an unprovisioned
       * device fail-closed (every offer → "bad signature"). */
      bool all_zero = true;
      for (size_t b = 0; b < sizeof(pk); b++) {
         if (pk[b] != 0) {
            all_zero = false;
            break;
         }
      }
      if (all_zero) {
         continue;
      }
      unsigned long long out_len = 0;
      if (crypto_sign_ed25519_tweet_open(m, &out_len, sm, sizeof(sm), pk) == 0 &&
          out_len == OTA_MAN_WIRE) {
         return true;
      }
   }
   return false;
}

/* ── status messages (mirror Tier-1 wire shapes) ─────────────────────────── */
static void ota_send_ack() {
   webSocket.sendTXT("{\"type\":\"ota_ack\"}");
}
static void ota_send_reject(const char *reason) {
   JsonDocument doc;
   doc["type"] = "ota_reject";
   doc["payload"]["reason"] = reason;
   char buf[160];
   size_t n = serializeJson(doc, buf, sizeof(buf));
   webSocket.sendTXT((uint8_t *)buf, n);
   Serial.printf("OTA: reject: %s\n", reason);
}
static void ota_send_status(const char *state) {
   JsonDocument doc;
   doc["type"] = "ota_status";
   doc["payload"]["state"] = state;
   char buf[96];
   size_t n = serializeJson(doc, buf, sizeof(buf));
   webSocket.sendTXT((uint8_t *)buf, n);
}

/* ── NVS record ──────────────────────────────────────────────────────────── */
static bool ota_rec_load(ota_rec_t *rec) {
   Preferences p;
   if (!p.begin(OTA_NVS_NS, true)) {
      return false;
   }
   size_t got = p.getBytes(OTA_NVS_KEY, rec, sizeof(*rec));
   p.end();
   return got == sizeof(*rec);
}
static bool ota_rec_save(const ota_rec_t *rec) {
   Preferences p;
   if (!p.begin(OTA_NVS_NS, false)) {
      return false;
   }
   size_t put = p.putBytes(OTA_NVS_KEY, rec, sizeof(*rec));
   p.end(); /* commits */
   return put == sizeof(*rec);
}
static void ota_rec_clear() {
   Preferences p;
   if (p.begin(OTA_NVS_NS, false)) {
      p.remove(OTA_NVS_KEY);
      p.end();
   }
}

/* ── offer decision (runs synchronously on the loop task; verify needs the
 *    bumped SET_LOOP_TASK_STACK_SIZE).  Mirrors Tier-1 ota_apply_decide order:
 *    validate → verify → cross-check → abi → already-current → rollback floor. */
void ota_handle_offer(JsonObject payload) {
   const char *url_path = payload["url_path"];
   const char *token = payload["token"];
   const char *sha_hex = payload["sha256"];
   const char *man_hex = payload["manifest"];
   const char *sig_hex = payload["sig"];
   int64_t offer_size = payload["image_size"] | (int64_t)-1; /* signed: reject negative/non-int */
   bool allow_downgrade = payload["allow_downgrade"] | false;

   /* Field presence + length + bounds. */
   if (!url_path || !token || !sha_hex || !man_hex || !sig_hex || strlen(token) != 64 ||
       strlen(sha_hex) != 64 || strlen(man_hex) != OTA_MAN_WIRE * 2 || strlen(sig_hex) != 128 ||
       strlen(url_path) >= sizeof(((ota_rec_t *)0)->url_path) || offer_size <= 0 ||
       (uint64_t)offer_size > OTA_MAX_IMAGE_BYTES) {
      ota_send_reject("malformed offer");
      return;
   }
   /* url_path allowlist: OTA image route, no traversal, safe charset. */
   if (strncmp(url_path, "/api/ota/", 9) != 0 || strstr(url_path, "..")) {
      ota_send_reject("bad url_path");
      return;
   }
   for (const char *c = url_path; *c; c++) {
      if (!((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') ||
            *c == '.' || *c == '_' || *c == '-' || *c == '/')) {
         ota_send_reject("bad url_path");
         return;
      }
   }

   uint8_t manifest[OTA_MAN_WIRE], sig[64], sha[32];
   if (!ota_hex2bin(man_hex, manifest, sizeof(manifest)) ||
       !ota_hex2bin(sig_hex, sig, sizeof(sig)) || !ota_hex2bin(sha_hex, sha, sizeof(sha))) {
      ota_send_reject("bad hex encoding");
      return;
   }

   /* Verify-before-parse: nothing in the manifest is trusted until the signature
    * checks out against a baked operator key. */
   if (!ota_verify_manifest(sig, manifest)) {
      ota_send_reject("bad signature");
      return;
   }
   Serial.printf("OTA: verify ok; loop stack HWM=%u words free\n",
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
   if (memcmp(manifest, OTA_MAN_MAGIC, 8) != 0 || ota_u16le(manifest + OTA_MAN_OFF_FMT) != 1) {
      ota_send_reject("bad manifest");
      return;
   }

   /* Read the signed fields. */
   uint8_t m_platform = manifest[OTA_MAN_OFF_PLATFORM];
   uint8_t m_tier = manifest[OTA_MAN_OFF_TIER];
   uint64_t m_size = ota_u64le(manifest + OTA_MAN_OFF_SIZE);
   const uint8_t *m_sha = manifest + OTA_MAN_OFF_SHA;
   char m_ver[32], m_abi[48], m_min[32];
   memcpy(m_ver, manifest + OTA_MAN_OFF_VERSION, 32);
   m_ver[31] = '\0';
   memcpy(m_abi, manifest + OTA_MAN_OFF_ABI, 48);
   m_abi[47] = '\0';
   memcpy(m_min, manifest + OTA_MAN_OFF_MINVER, 32);
   m_min[31] = '\0';

   /* Cross-check the unsigned offer against the SIGNED values; trust only signed. */
   if (m_size != (uint64_t)offer_size || memcmp(m_sha, sha, 32) != 0 ||
       m_platform != OTA_PLATFORM_ESP32 || m_tier != 2) {
      ota_send_reject("offer/manifest mismatch");
      return;
   }
   if (strcmp(m_abi, OTA_ABI_TAG) != 0) {
      ota_send_reject("abi mismatch");
      return;
   }
   if (strcmp(m_ver, FIRMWARE_VERSION) == 0) {
      ota_send_reject("already on this version");
      return;
   }
   if (ota_semver_cmp(m_ver, FIRMWARE_VERSION) < 0 && !allow_downgrade) {
      ota_send_reject("downgrade blocked");
      return;
   }
   if (m_min[0] && ota_semver_cmp(FIRMWARE_VERSION, m_min) < 0) {
      ota_send_reject("below required min_version floor");
      return;
   }

   /* Single-flight: only a genuine in-flight download ('D') blocks a new offer.
    * A lingering 'V' record means a prior update flashed but its commit didn't
    * fire — but we're clearly running + registered to even receive this offer,
    * so that update is healthy: commit it (clear) and accept the new one. */
   ota_rec_t existing;
   if (ota_rec_load(&existing)) {
      if (existing.state == 'D') {
         ota_send_reject("busy");
         return;
      }
      /* Reuse the commit path intentionally: receiving this offer proves we are
       * running + registered, so the prior 'V' update is healthy.  ota_on_registered
       * commits it (clears the record, disarms any watchdog, marks the app valid)
       * before we accept the new one. */
      ota_on_registered();
   }

   /* Persist the authenticated download record and reboot into the apply path.
    * revert_label is filled at switch time (the slot we're running now). */
   ota_rec_t rec;
   memset(&rec, 0, sizeof(rec));
   rec.state = 'D';
   rec.boots = 0;
   rec.image_size = m_size;
   memcpy(rec.sha256, m_sha, 32);
   snprintf(rec.version, sizeof(rec.version), "%s", m_ver);
   snprintf(rec.url_path, sizeof(rec.url_path), "%s", url_path);
   snprintf(rec.token, sizeof(rec.token), "%s", token);
   if (!ota_rec_save(&rec)) {
      ota_send_reject("nvs write failed");
      return;
   }

   /* Flush the ack, then the status, separately — the satellite WS has a single
    * pending-TX slot, so don't queue both back-to-back. */
   ota_send_ack();
   for (int i = 0; i < 3; i++) {
      webSocket.loop();
      delay(40);
   }
   ota_send_status("rebooting"); /* best-effort; the download happens post-reboot */
   updateTFTStatus("Updating...", ST77XX_YELLOW);
   Serial.printf("OTA: accepted %s -> rebooting to apply\n", m_ver);
   for (int i = 0; i < 3; i++) {
      webSocket.loop();
      delay(40);
   }
   esp_restart();
}

/* ── boot path: download+flash (state 'D') / rollback guard (state 'V') ─────── */

/* Is @p part a flashable app image (first byte is the ESP image magic 0xE9)? */
static bool ota_partition_bootable(const esp_partition_t *part) {
   if (!part) {
      return false;
   }
   uint8_t magic = 0;
   if (esp_partition_read(part, 0, &magic, 1) != ESP_OK) {
      return false;
   }
   return magic == 0xE9;
}

/* Bring up WiFi + NTP (cert validation needs real time). Returns true if ready. */
static bool ota_net_up() {
   WiFi.begin(SECRET_SSID, SECRET_PASSWORD);
   for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) {
      delay(500);
      esp_task_wdt_reset();
   }
   if (WiFi.status() != WL_CONNECTED) {
      return false;
   }
   configTime(0, 0, "pool.ntp.org", "time.nist.gov");
   time_t now = 0;
   for (int i = 0; i < 20 && now < 8 * 3600 * 2; i++) {
      delay(500);
      now = time(nullptr);
      esp_task_wdt_reset();
   }
   return now >= 8 * 3600 * 2; /* real time, not epoch 0 → CA dates validate */
}

/* SHA-256 the bytes actually committed to @p part (the on-flash artifact, like
 * Tier-1 hashes the swapped fd) and compare to the authenticated hash. */
static bool ota_flash_hash_ok(const esp_partition_t *part,
                              const ota_rec_t *rec,
                              uint8_t *buf,
                              size_t bufsz) {
   mbedtls_sha256_context sha;
   mbedtls_sha256_init(&sha);
   mbedtls_sha256_starts(&sha, 0); /* 0 = SHA-256 */
   uint64_t off = 0;
   bool ok = true;
   while (off < rec->image_size) {
      size_t n = (rec->image_size - off) > bufsz ? bufsz : (size_t)(rec->image_size - off);
      if (esp_partition_read(part, off, buf, n) != ESP_OK) {
         ok = false;
         break;
      }
      mbedtls_sha256_update(&sha, buf, n);
      off += n;
      esp_task_wdt_reset();
      delay(1);
   }
   uint8_t digest[32];
   mbedtls_sha256_finish(&sha, digest);
   mbedtls_sha256_free(&sha);
   return ok && memcmp(digest, rec->sha256, 32) == 0;
}

/* Download the staged image into the inactive partition, verify the COMMITTED
 * flash against the signed hash, switch boot.  Returns true on a fully-applied
 * image (caller persists 'verify' + reboots).  Records the running slot as the
 * explicit rollback target in @p rec.  Assumes the caller has widened/removed
 * the task WDT for the duration (the TLS handshake is a long blocking call). */
static bool ota_download_and_flash(ota_rec_t *rec) {
   const esp_partition_t *running = esp_ota_get_running_partition();
   const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
   /* next == running guards a misconfigured single-app-slot build — never erase
    * and flash the partition we're executing from. */
   if (!running || !next || next == running) {
      Serial.println("OTA: partition lookup failed");
      return false;
   }
   /* Pin the rollback target to the slot we are running RIGHT NOW (known-good),
    * by label — not inferred later via get_next_update_partition. */
   snprintf(rec->revert_label, sizeof(rec->revert_label), "%s", running->label);

   String url = String("https://") + SECRET_SERVER_IP + ":" + String(SECRET_SERVER_PORT) +
                rec->url_path + "?uuid=" + persistentUUID + "&token=" + rec->token;

   WiFiClientSecure client;
   client.setCACert(CA_CERT_PEM);
   HTTPClient http;
   http.setConnectTimeout(15000);
   http.setTimeout(20000);
   if (!http.begin(client, url)) {
      Serial.println("OTA: http begin failed");
      return false;
   }
   int code = http.GET();
   if (code != HTTP_CODE_OK) {
      Serial.printf("OTA: GET failed (%d)\n", code);
      http.end();
      return false;
   }

   Serial.printf("OTA: GET 200, erasing slot '%s' (%llu bytes)…\n", next->label,
                 (unsigned long long)rec->image_size);
   Serial.flush();
   esp_ota_handle_t handle = 0;
   if (esp_ota_begin(next, rec->image_size, &handle) != ESP_OK) {
      Serial.println("OTA: esp_ota_begin failed");
      http.end();
      return false;
   }
   Serial.println("OTA: erase done, downloading…");
   Serial.flush();

   static uint8_t chunk[OTA_DL_CHUNK]; /* one reused buffer (not per-iteration) */
   WiFiClient *stream = http.getStreamPtr();
   if (!stream) {
      Serial.println("OTA: null stream");
      esp_ota_abort(handle);
      http.end();
      return false;
   }
   stream->setTimeout(50); /* short — never block a full second inside one read */
   uint64_t written = 0;
   bool ok = true;
   uint32_t last_pct = 101;
   unsigned long last_data_ms = millis();
   unsigned long dl_start = millis();

   /* Loop while there are buffered bytes OR the connection is live — so the tail
    * the server flushed right before closing is still drained. */
   while ((stream->available() || http.connected()) && written < rec->image_size) {
      if (millis() - dl_start > 180000) { /* absolute wall-clock cap */
         ok = false;
         break;
      }
      int avail = stream->available();
      if (avail <= 0) {
         if (millis() - last_data_ms > 20000) { /* no progress — stalled, give up */
            ok = false;
            break;
         }
         delay(1);
         continue;
      }
      size_t want = (size_t)avail > sizeof(chunk) ? sizeof(chunk) : (size_t)avail;
      if (want > rec->image_size - written) {
         want = (size_t)(rec->image_size - written);
      }
      int n = stream->read(chunk, want); /* non-blocking: returns what's ready */
      if (n <= 0) {
         delay(1);
         continue;
      }
      last_data_ms = millis();
      if (esp_ota_write(handle, chunk, (size_t)n) != ESP_OK) {
         ok = false;
         break;
      }
      written += (uint64_t)n;
      uint32_t pct = (uint32_t)((written * 100) / rec->image_size);
      if (pct != last_pct && (pct % 10) == 0) {
         char b[24];
         snprintf(b, sizeof(b), "Update %lu%%", (unsigned long)pct);
         updateTFTStatus(b, ST77XX_YELLOW);
         last_pct = pct;
      }
      delay(1); /* yield → feeds the idle WDT */
   }
   http.end();

   /* Known-bad (incomplete) → abort, releasing the handle without end-time
    * validation; do NOT esp_ota_end a partial image. */
   if (!ok || written != rec->image_size) {
      esp_ota_abort(handle);
      Serial.printf("OTA: download incomplete (%llu/%llu) — aborting\n",
                    (unsigned long long)written, (unsigned long long)rec->image_size);
      return false;
   }
   if (esp_ota_end(handle) != ESP_OK) {
      Serial.println("OTA: esp_ota_end failed");
      return false;
   }
   /* Hash the COMMITTED flash (not just the received stream) before switching. */
   if (!ota_flash_hash_ok(next, rec, chunk, sizeof(chunk))) {
      Serial.println("OTA: flashed-image hash mismatch — not switching");
      return false;
   }
   if (esp_ota_set_boot_partition(next) != ESP_OK) {
      Serial.println("OTA: set_boot_partition failed");
      return false;
   }
   return true;
}

/* ── verify-boot watchdog ────────────────────────────────────────────────────
 * A just-flashed ('V') image gets OTA_VERIFY_REGISTER_TIMEOUT_MS to reach the
 * server.  This one-shot esp_timer runs in its own high-priority task, so it
 * fires and reboots even if the loop/setup task is wedged in a while(1) hang
 * (the audio-alloc failure that motivated this) — a plain loop()-driven timeout
 * could not.  The reboot re-enters ota_boot_path() FIRST, which increments the
 * NVS boot counter, so repeated hangs march toward the rollback threshold.
 * ota_on_registered() disarms it on a healthy registration. */
static esp_timer_handle_t s_verify_wdt = NULL;

static void ota_verify_wdt_cb(void *arg) {
   (void)arg;
   Serial.println(
       "OTA: verify watchdog — not registered in time; rebooting to advance rollback guard");
   Serial.flush();
   esp_restart();
}

static void ota_arm_verify_watchdog() {
   if (s_verify_wdt) {
      return; /* already armed */
   }
   esp_timer_create_args_t args = {};
   args.callback = ota_verify_wdt_cb;
   args.arg = NULL;
   args.dispatch_method = ESP_TIMER_TASK;
   args.name = "ota_verify_wdt";
   if (esp_timer_create(&args, &s_verify_wdt) != ESP_OK) {
      s_verify_wdt = NULL;
      return; /* best-effort: without the watchdog a hang still can't auto-revert */
   }
   /* If the start fails, delete the handle and NULL it — otherwise a created-but-
    * never-started timer would look "armed" yet never fire, silently disabling the
    * safety net (and blocking a later re-arm via the guard above). */
   if (esp_timer_start_once(s_verify_wdt, (uint64_t)OTA_VERIFY_REGISTER_TIMEOUT_MS * 1000ULL) !=
       ESP_OK) {
      esp_timer_delete(s_verify_wdt);
      s_verify_wdt = NULL;
      Serial.println("OTA: verify watchdog start failed");
      return;
   }
   Serial.printf("OTA: verify watchdog armed (%d ms to register)\n",
                 OTA_VERIFY_REGISTER_TIMEOUT_MS);
}

static void ota_disarm_verify_watchdog() {
   if (!s_verify_wdt) {
      return;
   }
   esp_timer_stop(s_verify_wdt);
   esp_timer_delete(s_verify_wdt);
   s_verify_wdt = NULL;
}

void ota_boot_path() {
   /* Also forces the linker to keep dawn_sat_fw_marker in the .bin (ota-release.sh
    * greps it); logs the compiled version at boot as a side benefit. */
   Serial.printf("OTA: %s\n", dawn_sat_fw_marker);

   ota_rec_t rec;
   if (!ota_rec_load(&rec)) {
      return; /* no OTA in flight — normal boot */
   }

   if (rec.state == 'V') {
      /* New image is now the running partition (PENDING-VERIFY). Count this boot
       * FIRST [review H2] so a crash-loop reliably reaches the threshold. */
      rec.boots++;
      ota_rec_save(&rec);
      if (rec.boots < OTA_ESP_ROLLBACK_THRESHOLD) {
         /* Give normal init a chance to register + commit — but arm a watchdog so
          * a hang/crash later in setup() (which never reboots on its own) still
          * forces a reboot, advancing this counter toward the threshold above. */
         ota_arm_verify_watchdog();
         return;
      }
      /* Threshold hit: the new image won't register. Revert to the EXPLICIT
       * known-good slot recorded at flash time (found by label) — not inferred,
       * so a crash-timing quirk can't make us "revert" onto the bad image.  Only
       * switch if it actually holds a bootable app (0xE9); a blank slot would
       * brick, so in that case stay put and clear. */
      const esp_partition_t *old = rec.revert_label[0]
                                       ? esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                                                  ESP_PARTITION_SUBTYPE_ANY,
                                                                  rec.revert_label)
                                       : NULL;
      if (ota_partition_bootable(old) && esp_ota_set_boot_partition(old) == ESP_OK) {
         Serial.printf("OTA: rollback — %s did not register in %d boots; reverting to %s\n",
                       rec.version, rec.boots, rec.revert_label);
         ota_rec_clear();
         updateTFTStatus("OTA reverted", ST77XX_RED);
         delay(200);
         esp_restart();
      }
      Serial.println("OTA: rollback needed but no bootable target — staying, clearing state");
      ota_rec_clear();
      return;
   }

   if (rec.state == 'D') {
      /* Reboot-first apply: WiFi-only, max heap, one TLS session, no WS/audio. */
      Serial.printf("OTA: applying %s (download path)\n", rec.version);
      updateTFTStatus("OTA: connecting", ST77XX_CYAN);
      /* Disable ALL watchdogs for the flash window: esp_ota_begin erases the
       * whole app slot (~1.9 MB) in one blocking call that starves the idle task
       * on this core, and the TLS handshake is a long blocking call.  The loop
       * (subscribed) WDT AND the per-core IDLE-task WDTs must both be off, so use
       * the arduino-esp32 helpers (esp_task_wdt_delete only covered the loop
       * task, not the idle-core watchdogs — the likely silent-reset cause). */
      disableLoopWDT();
      disableCore0WDT();
      disableCore1WDT();
      bool applied = false;
      if (ota_net_up()) {
         Serial.printf("OTA: largest free heap block before TLS: %u\n",
                       (unsigned)ESP.getMaxAllocHeap());
         applied = ota_download_and_flash(&rec);
      } else {
         Serial.println("OTA: WiFi/NTP not ready — aborting this attempt");
      }
      if (applied) {
         rec.state = 'V';
         rec.boots = 0;
         ota_rec_save(&rec); /* persists revert_label set during the download */
         Serial.println("OTA: flashed + boot switched — rebooting into new image");
         updateTFTStatus("OTA: rebooting", ST77XX_GREEN);
         delay(200);
         esp_restart();
      }
      /* One attempt, fail-closed [review M4]: clear state, restore the WDTs, boot
       * the OLD image.  The server marks this device failed on re-register. */
      ota_rec_clear();
      enableLoopWDT();
      enableCore0WDT();
      enableCore1WDT();
      Serial.println("OTA: download failed — booting current image");
      updateTFTStatus("OTA failed", ST77XX_RED);
      return;
   }

   /* Unknown state — fail safe, clear. */
   ota_rec_clear();
}

/* Commit hook: a successful registration proves the new image runs + reaches
 * the server, so a verify-state record can be cleared (no rollback). */
void ota_on_registered() {
   /* Registered → the new image works.  Clear the verify record FIRST, before
    * disarming: if the watchdog timer fires in the narrow window between here and
    * the disarm (registration landing right at the timeout), the resulting reboot
    * then finds no 'V' record and is a clean normal boot rather than a spurious
    * boots++ toward a needless revert. */
   ota_rec_t rec;
   bool committed = ota_rec_load(&rec) && rec.state == 'V';
   if (committed) {
      ota_rec_clear();
   }
   ota_disarm_verify_watchdog();

   /* Also mark the running app valid: harmless no-op if the stock bootloader has
    * no rollback support (our NVS guard is the real mechanism), but if this core
    * build DID enable CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE it cancels the
    * bootloader's own pending-verify auto-revert so the two don't fight. */
   esp_ota_mark_app_valid_cancel_rollback();

   if (committed) {
      Serial.printf("OTA: committed %s (registered healthy)\n", rec.version);
   }
}
