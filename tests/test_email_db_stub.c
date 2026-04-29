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
 * the project author(s). Contributions include any modifications,
 * enhancements, or additions to the project. These contributions become
 * part of the project and are adopted by the project author(s).
 *
 * Stubs for test_email_db: replaces crypto_store_encrypt/decrypt with no-op
 * failing variants so the linker is satisfied without pulling crypto_store.c
 * (and its g_config / libsodium init dependencies) into the test binary.
 *
 * email_db tests place raw bytes directly into encrypted_password and never
 * invoke email_encrypt_password() / email_decrypt_password(), so these stubs
 * are never actually called from the test paths. The encrypt/decrypt logic
 * itself is covered by test_crypto_store.c.
 */

#include <stddef.h>

int crypto_store_encrypt(const void *plaintext,
                         size_t plaintext_len,
                         unsigned char *out,
                         size_t out_capacity,
                         size_t *out_written) {
   (void)plaintext;
   (void)plaintext_len;
   (void)out;
   (void)out_capacity;
   if (out_written)
      *out_written = 0;
   return 1; /* FAILURE — never invoked in this test */
}

int crypto_store_decrypt(const unsigned char *ciphertext,
                         size_t ciphertext_len,
                         void *out,
                         size_t out_capacity,
                         size_t *out_written) {
   (void)ciphertext;
   (void)ciphertext_len;
   (void)out;
   (void)out_capacity;
   if (out_written)
      *out_written = 0;
   return 1; /* FAILURE — never invoked in this test */
}
