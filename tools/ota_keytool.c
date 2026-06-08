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
 * ota-keytool — OFFLINE OTA signing CLI (docs/OTA_DESIGN.md §Trust model, §2).
 *
 * Runs on a secure operator machine, NOT the daemon: it holds the Ed25519
 * private key and signs release images.  The daemon only stores/serves the
 * resulting manifest+sig; the device verifies against a baked-in public keyring.
 *
 * Subcommands:
 *   keygen  --out-dir DIR
 *       Generate a keypair → DIR/ota_signing.key (hex secret, 0600) +
 *       DIR/ota_signing.pub (hex public).  Back up the .key offline.
 *   pubkey-header  --pub HEX[,HEX...]  --out ota_pubkey.h
 *       Emit the C keyring header devices bake in (current[,next] pubkeys).
 *   sign  --sk KEYFILE --image FILE --version X.Y.Z --platform rpi|esp32
 *         --tier N --abi-tag TAG [--min-version X.Y.Z] --out-dir DIR
 *       Compute the image SHA-256, build + sign a manifest →
 *       DIR/manifest (164B) + DIR/manifest.sig (64B).
 */

#include <sodium.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "core/ota_manifest.h"

/* Scan argv for "--name" and return the following token, or def. */
static const char *opt(int argc, char **argv, const char *name, const char *def) {
   for (int i = 0; i < argc - 1; i++) {
      if (strcmp(argv[i], name) == 0) {
         return argv[i + 1];
      }
   }
   return def;
}

static int write_file(const char *path, const void *data, size_t len, mode_t mode) {
   FILE *f = fopen(path, "wb");
   if (!f) {
      fprintf(stderr, "ota-keytool: cannot open %s for writing\n", path);
      return FAILURE;
   }
   size_t w = fwrite(data, 1, len, f);
   fclose(f);
   if (w != len) {
      fprintf(stderr, "ota-keytool: short write to %s\n", path);
      return FAILURE;
   }
   if (mode != 0) {
      chmod(path, mode);
   }
   return SUCCESS;
}

/* Read a hex string (optionally with trailing whitespace/newline) of exactly
 * bin_len bytes from a file. */
static int read_hex_file(const char *path, uint8_t *out, size_t bin_len) {
   FILE *f = fopen(path, "rb");
   if (!f) {
      fprintf(stderr, "ota-keytool: cannot open %s\n", path);
      return FAILURE;
   }
   char hex[256];
   size_t n = fread(hex, 1, sizeof(hex) - 1, f);
   fclose(f);
   hex[n] = '\0';
   size_t decoded = 0;
   if (sodium_hex2bin(out, bin_len, hex, n, " \r\n\t", &decoded, NULL) != 0 || decoded != bin_len) {
      fprintf(stderr, "ota-keytool: %s is not %zu hex bytes\n", path, bin_len);
      return FAILURE;
   }
   return SUCCESS;
}

static int cmd_keygen(int argc, char **argv) {
   const char *dir = opt(argc, argv, "--out-dir", ".");
   uint8_t pk[OTA_PK_BYTES], sk[OTA_SK_BYTES];
   ota_keygen(pk, sk);

   char hex[OTA_SK_BYTES * 2 + 1];
   char path[1024];

   sodium_bin2hex(hex, sizeof(hex), sk, OTA_SK_BYTES);
   snprintf(path, sizeof(path), "%s/ota_signing.key", dir);
   if (write_file(path, hex, strlen(hex), 0600) != SUCCESS) {
      sodium_memzero(sk, sizeof(sk));
      return FAILURE;
   }
   sodium_memzero(hex, sizeof(hex));
   sodium_memzero(sk, sizeof(sk));

   sodium_bin2hex(hex, sizeof(hex), pk, OTA_PK_BYTES);
   snprintf(path, sizeof(path), "%s/ota_signing.pub", dir);
   if (write_file(path, hex, strlen(hex), 0644) != SUCCESS) {
      return FAILURE;
   }

   printf("Keypair written to %s/ota_signing.{key,pub}\n", dir);
   printf("Public key: %s\n", hex);
   printf("Keep ota_signing.key OFFLINE (it signs satellite code).\n");
   return SUCCESS;
}

static int cmd_pubkey_header(int argc, char **argv) {
   const char *pubs = opt(argc, argv, "--pub", NULL);
   const char *out = opt(argc, argv, "--out", "ota_pubkey.h");
   if (!pubs) {
      fprintf(stderr, "ota-keytool pubkey-header: --pub HEX[,HEX] required\n");
      return FAILURE;
   }

   /* Decode up to a small keyring (current[,next]). */
   uint8_t keys[4][OTA_PK_BYTES];
   int n = 0;
   char buf[1024];
   snprintf(buf, sizeof(buf), "%s", pubs);
   for (char *tok = strtok(buf, ","); tok && n < 4; tok = strtok(NULL, ",")) {
      size_t decoded = 0;
      if (sodium_hex2bin(keys[n], OTA_PK_BYTES, tok, strlen(tok), " \r\n\t", &decoded, NULL) != 0 ||
          decoded != OTA_PK_BYTES) {
         fprintf(stderr, "ota-keytool: bad pubkey hex: %s\n", tok);
         return FAILURE;
      }
      n++;
   }
   if (n == 0) {
      fprintf(stderr, "ota-keytool: no pubkeys parsed\n");
      return FAILURE;
   }

   FILE *f = fopen(out, "w");
   if (!f) {
      fprintf(stderr, "ota-keytool: cannot write %s\n", out);
      return FAILURE;
   }
   fprintf(f, "/* Auto-generated by ota-keytool — do not edit.\n");
   fprintf(f, " * OTA verification keyring baked into the device image. */\n");
   fprintf(f, "#ifndef OTA_PUBKEY_H\n#define OTA_PUBKEY_H\n\n#include <stdint.h>\n\n");
   fprintf(f, "#define OTA_KEYRING_COUNT %d\n\n", n);
   fprintf(f, "static const uint8_t OTA_KEYRING[OTA_KEYRING_COUNT][%d] = {\n", OTA_PK_BYTES);
   for (int k = 0; k < n; k++) {
      fprintf(f, "   {");
      for (int i = 0; i < OTA_PK_BYTES; i++) {
         fprintf(f, "0x%02x%s", keys[k][i], (i == OTA_PK_BYTES - 1) ? "" : ", ");
      }
      fprintf(f, "}, /* key %d%s */\n", k, k == 0 ? " (current)" : "");
   }
   fprintf(f, "};\n\n#endif /* OTA_PUBKEY_H */\n");
   fclose(f);
   printf("Wrote %s with %d key(s).\n", out, n);
   return SUCCESS;
}

static int sha256_file(const char *path, uint8_t out[OTA_SHA256_BYTES], uint64_t *size_out) {
   FILE *f = fopen(path, "rb");
   if (!f) {
      fprintf(stderr, "ota-keytool: cannot open image %s\n", path);
      return FAILURE;
   }
   crypto_hash_sha256_state st;
   crypto_hash_sha256_init(&st);
   uint8_t buf[65536];
   uint64_t total = 0;
   size_t r;
   while ((r = fread(buf, 1, sizeof(buf), f)) > 0) {
      crypto_hash_sha256_update(&st, buf, r);
      total += r;
   }
   fclose(f);
   crypto_hash_sha256_final(&st, out);
   if (size_out) {
      *size_out = total;
   }
   return SUCCESS;
}

static int cmd_sign(int argc, char **argv) {
   const char *skfile = opt(argc, argv, "--sk", NULL);
   const char *image = opt(argc, argv, "--image", NULL);
   const char *version = opt(argc, argv, "--version", NULL);
   const char *platform = opt(argc, argv, "--platform", NULL);
   const char *tier_s = opt(argc, argv, "--tier", NULL);
   const char *abi_tag = opt(argc, argv, "--abi-tag", "");
   const char *min_version = opt(argc, argv, "--min-version", "");
   const char *dir = opt(argc, argv, "--out-dir", ".");

   if (!skfile || !image || !version || !platform || !tier_s) {
      fprintf(stderr, "ota-keytool sign: --sk --image --version --platform --tier required\n");
      return FAILURE;
   }

   ota_manifest_t m;
   memset(&m, 0, sizeof(m));
   m.fmt_version = OTA_MANIFEST_FMT_VERSION;
   if (strcmp(platform, "rpi") == 0) {
      m.platform = OTA_PLATFORM_RPI;
   } else if (strcmp(platform, "esp32") == 0) {
      m.platform = OTA_PLATFORM_ESP32;
   } else {
      fprintf(stderr, "ota-keytool: --platform must be rpi or esp32\n");
      return FAILURE;
   }
   m.tier = (uint8_t)atoi(tier_s);
   snprintf(m.version, sizeof(m.version), "%s", version);
   snprintf(m.abi_tag, sizeof(m.abi_tag), "%s", abi_tag);
   snprintf(m.min_version, sizeof(m.min_version), "%s", min_version);

   if (sha256_file(image, m.sha256, &m.image_size) != SUCCESS) {
      return FAILURE;
   }

   uint8_t sk[OTA_SK_BYTES];
   if (read_hex_file(skfile, sk, OTA_SK_BYTES) != SUCCESS) {
      return FAILURE;
   }

   uint8_t raw[OTA_MANIFEST_WIRE_SIZE];
   uint8_t sig[OTA_SIG_BYTES];
   size_t raw_len = 0;
   int rc = ota_manifest_sign(&m, sk, raw, sizeof(raw), &raw_len, sig);
   sodium_memzero(sk, sizeof(sk));
   if (rc != SUCCESS) {
      fprintf(stderr, "ota-keytool: signing failed\n");
      return FAILURE;
   }

   char path[1024];
   snprintf(path, sizeof(path), "%s/manifest", dir);
   if (write_file(path, raw, raw_len, 0644) != SUCCESS) {
      return FAILURE;
   }
   snprintf(path, sizeof(path), "%s/manifest.sig", dir);
   if (write_file(path, sig, sizeof(sig), 0644) != SUCCESS) {
      return FAILURE;
   }

   char shahex[OTA_SHA256_BYTES * 2 + 1];
   sodium_bin2hex(shahex, sizeof(shahex), m.sha256, OTA_SHA256_BYTES);
   printf("Signed %s (%llu bytes, sha256=%s)\n", image, (unsigned long long)m.image_size, shahex);
   printf("  version=%s platform=%s tier=%d abi_tag=%s min_version=%s\n", m.version, platform,
          m.tier, m.abi_tag, m.min_version[0] ? m.min_version : "(none)");
   printf("Wrote %s/manifest + %s/manifest.sig\n", dir, dir);
   return SUCCESS;
}

static long read_bin_file(const char *path, uint8_t *out, size_t max) {
   FILE *f = fopen(path, "rb");
   if (!f) {
      fprintf(stderr, "ota-keytool: cannot open %s\n", path);
      return -1;
   }
   size_t n = fread(out, 1, max, f);
   fclose(f);
   return (long)n;
}

static int cmd_verify(int argc, char **argv) {
   const char *mfile = opt(argc, argv, "--manifest", NULL);
   const char *sfile = opt(argc, argv, "--sig", NULL);
   const char *pubs = opt(argc, argv, "--pub", NULL);
   if (!mfile || !sfile || !pubs) {
      fprintf(stderr, "ota-keytool verify: --manifest --sig --pub required\n");
      return FAILURE;
   }

   uint8_t raw[OTA_MANIFEST_WIRE_SIZE];
   long raw_len = read_bin_file(mfile, raw, sizeof(raw));
   if (raw_len < 0) {
      return FAILURE;
   }
   uint8_t sig[OTA_SIG_BYTES];
   if (read_bin_file(sfile, sig, sizeof(sig)) != (long)sizeof(sig)) {
      fprintf(stderr, "ota-keytool: %s is not %d bytes\n", sfile, OTA_SIG_BYTES);
      return FAILURE;
   }

   uint8_t keys[4][OTA_PK_BYTES];
   int n = 0;
   char buf[1024];
   snprintf(buf, sizeof(buf), "%s", pubs);
   for (char *tok = strtok(buf, ","); tok && n < 4; tok = strtok(NULL, ",")) {
      size_t decoded = 0;
      if (sodium_hex2bin(keys[n], OTA_PK_BYTES, tok, strlen(tok), " \r\n\t", &decoded, NULL) != 0 ||
          decoded != OTA_PK_BYTES) {
         fprintf(stderr, "ota-keytool: bad pubkey hex: %s\n", tok);
         return FAILURE;
      }
      n++;
   }

   ota_manifest_t m;
   if (ota_manifest_verify(raw, (size_t)raw_len, sig, keys, (size_t)n, &m) != SUCCESS) {
      fprintf(stderr, "VERIFY FAILED\n");
      return FAILURE;
   }
   char shahex[OTA_SHA256_BYTES * 2 + 1];
   sodium_bin2hex(shahex, sizeof(shahex), m.sha256, OTA_SHA256_BYTES);
   printf("VERIFY OK\n");
   printf("  version=%s platform=%u tier=%u image_size=%llu\n", m.version, m.platform, m.tier,
          (unsigned long long)m.image_size);
   printf("  abi_tag=%s min_version=%s sha256=%s\n", m.abi_tag,
          m.min_version[0] ? m.min_version : "(none)", shahex);
   return SUCCESS;
}

static void usage(void) {
   fprintf(stderr, "ota-keytool — offline OTA signing (keep the key off the daemon)\n\n"
                   "  ota-keytool keygen --out-dir DIR\n"
                   "  ota-keytool pubkey-header --pub HEX[,HEX] --out ota_pubkey.h\n"
                   "  ota-keytool sign --sk KEYFILE --image FILE --version X.Y.Z\n"
                   "                   --platform rpi|esp32 --tier N --abi-tag TAG\n"
                   "                   [--min-version X.Y.Z] --out-dir DIR\n"
                   "  ota-keytool verify --manifest FILE --sig FILE --pub HEX[,HEX]\n");
}

int main(int argc, char **argv) {
   if (sodium_init() < 0) {
      fprintf(stderr, "ota-keytool: libsodium init failed\n");
      return 1;
   }
   if (argc < 2) {
      usage();
      return 1;
   }
   int rc;
   if (strcmp(argv[1], "keygen") == 0) {
      rc = cmd_keygen(argc, argv);
   } else if (strcmp(argv[1], "pubkey-header") == 0) {
      rc = cmd_pubkey_header(argc, argv);
   } else if (strcmp(argv[1], "sign") == 0) {
      rc = cmd_sign(argc, argv);
   } else if (strcmp(argv[1], "verify") == 0) {
      rc = cmd_verify(argc, argv);
   } else {
      usage();
      return 1;
   }
   return (rc == SUCCESS) ? 0 : 1;
}
