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
 * Shared hashing helpers — SHA-256 hex.
 */

#include "core/hash_util.h"

#include <openssl/sha.h>
#include <stdio.h>

void dawn_sha256_hex(const void *data, size_t len, char *out_hex) {
   unsigned char hash[SHA256_DIGEST_LENGTH];
   SHA256((const unsigned char *)data, len, hash);
   for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
      snprintf(out_hex + (i * 2), 3, "%02x", hash[i]);
   }
   out_hex[SHA256_DIGEST_LENGTH * 2] = '\0';
}
