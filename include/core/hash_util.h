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
 * Shared hashing helpers (Layer 0/1) — no DAWN deps beyond libc + OpenSSL.
 * Used by the document index pipeline (content_hash over extracted text) and
 * the blob store (content_hash over raw file bytes), so the implementation
 * lives in one place rather than being copied per consumer.
 */

#ifndef CORE_HASH_UTIL_H
#define CORE_HASH_UTIL_H

#include <stddef.h>

/** @brief Length of a SHA-256 hex string including the NUL (64 hex + 1). */
#define DAWN_SHA256_HEX_LEN 65

/**
 * @brief Compute the SHA-256 of a buffer and format it as lowercase hex.
 *
 * @param data    Input bytes (may be binary; not treated as a C string).
 * @param len     Number of bytes in @p data.
 * @param out_hex Output buffer of at least DAWN_SHA256_HEX_LEN bytes; receives
 *                a NUL-terminated 64-char lowercase hex string.
 */
void dawn_sha256_hex(const void *data, size_t len, char *out_hex);

#endif /* CORE_HASH_UTIL_H */
