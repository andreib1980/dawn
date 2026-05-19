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
 * WebUI vision-upload image validation.
 *
 * Owns `validate_image_data` and its three internal helpers
 * (`is_vision_mime_allowed`, `is_valid_base64_charset`,
 * `check_image_magic`).  Security-hardened validation for uploaded images:
 * MIME-type whitelist, size check before allocation, base64 character-set
 * check, magic-byte verification on a short stack-decoded prefix.  Split
 * out of webui_server.c so that file can stay under the size limits in
 * CLAUDE.md.  `validate_image_data` is declared in
 * include/webui/webui_internal.h; callers stay inside the webui module.
 */

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "core/ocp_helpers.h"
#include "logging.h"
#include "webui/webui_internal.h"
#include "webui/webui_server.h"

/* =============================================================================
 * Vision Image Validation
 *
 * Security-hardened validation for uploaded images. Prevents memory
 * amplification attacks by checking sizes BEFORE allocation.
 * ============================================================================= */

/**
 * @brief Check if MIME type is in vision whitelist
 *
 * SVG explicitly excluded to prevent XSS attacks.
 *
 * @param mime_type MIME type string to check
 * @return true if allowed, false if rejected
 */
static bool is_vision_mime_allowed(const char *mime_type) {
   if (!mime_type)
      return false;

   /* Explicit whitelist - SVG excluded for XSS prevention */
   static const char *allowed[] = { "image/jpeg", "image/png", "image/gif", "image/webp" };
   static const int num_allowed = sizeof(allowed) / sizeof(allowed[0]);

   for (int i = 0; i < num_allowed; i++) {
      if (strcasecmp(mime_type, allowed[i]) == 0) {
         return true;
      }
   }
   return false;
}

/**
 * @brief Check if string contains only valid base64 characters
 *
 * @param str String to check
 * @param len Length of string
 * @return true if valid base64 charset, false otherwise
 */
static bool is_valid_base64_charset(const char *str, size_t len) {
   for (size_t i = 0; i < len; i++) {
      char c = str[i];
      /* Valid base64: A-Z, a-z, 0-9, +, /, = (padding) */
      if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '+' || c == '/' || c == '=')) {
         return false;
      }
   }
   return true;
}

/**
 * @brief Check image magic bytes against expected MIME type
 *
 * @param data First 16 bytes of decoded image
 * @param len Length of data (may be less than 16)
 * @param mime_type Expected MIME type
 * @return true if magic bytes match, false otherwise
 */
static bool check_image_magic(const unsigned char *data, size_t len, const char *mime_type) {
   if (!data || len < 3 || !mime_type)
      return false;

   /* JPEG: FF D8 FF */
   if (strcasecmp(mime_type, "image/jpeg") == 0) {
      return (len >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF);
   }

   /* PNG: 89 50 4E 47 0D 0A 1A 0A */
   if (strcasecmp(mime_type, "image/png") == 0) {
      static const unsigned char png_magic[] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
      return (len >= 8 && memcmp(data, png_magic, 8) == 0);
   }

   /* GIF: 47 49 46 38 ("GIF8") */
   if (strcasecmp(mime_type, "image/gif") == 0) {
      return (len >= 4 && memcmp(data, "GIF8", 4) == 0);
   }

   /* WebP: RIFF....WEBP */
   if (strcasecmp(mime_type, "image/webp") == 0) {
      return (len >= 12 && memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "WEBP", 4) == 0);
   }

   return false;
}

/**
 * @brief Validate base64-encoded image data (security-hardened)
 *
 * Validation order prevents memory amplification attacks:
 * 1. Check MIME type against whitelist (no allocation)
 * 2. Check base64 string length against WEBUI_MAX_BASE64_SIZE (no allocation)
 * 3. Validate base64 character set (no allocation)
 * 4. Decode first 24 bytes only to check magic bytes (stack buffer)
 *
 * @param base64_data Base64 encoded image string
 * @param base64_len Length of base64 string
 * @param mime_type MIME type string
 * @return 0 on success, error code on failure:
 *         1 = Invalid MIME type
 *         2 = Base64 data too large
 *         3 = Invalid base64 characters
 *         4 = Magic bytes don't match MIME type
 *         5 = Decode error
 */
int validate_image_data(const char *base64_data, size_t base64_len, const char *mime_type) {
   /* Step 1: MIME type whitelist (no allocation) */
   if (!is_vision_mime_allowed(mime_type)) {
      OLOG_WARNING("WebUI Vision: Rejected MIME type: %s", mime_type ? mime_type : "(null)");
      return 1;
   }

   /* Step 2: Size check (no allocation) - prevents memory amplification */
   if (base64_len > WEBUI_MAX_BASE64_SIZE) {
      OLOG_WARNING("WebUI Vision: Base64 data too large: %zu > %zu", base64_len,
                   (size_t)WEBUI_MAX_BASE64_SIZE);
      return 2;
   }

   /* Step 3: Validate base64 character set (no allocation) */
   if (!is_valid_base64_charset(base64_data, base64_len)) {
      OLOG_WARNING("WebUI Vision: Invalid base64 characters");
      return 3;
   }

   /* Step 4: Decode minimal prefix to check magic bytes (stack buffer)
    * We need 24 base64 chars to decode 18 bytes (enough for all magic checks) */
   if (base64_len >= 24) {
      /* Create null-terminated subset for decoder */
      char prefix[25];
      memcpy(prefix, base64_data, 24);
      prefix[24] = '\0';

      size_t decoded_len = 0;
      unsigned char *decoded = ocp_base64_decode(prefix, &decoded_len);
      if (!decoded) {
         OLOG_WARNING("WebUI Vision: Failed to decode base64 prefix");
         return 5;
      }

      bool magic_ok = check_image_magic(decoded, decoded_len, mime_type);
      free(decoded);

      if (!magic_ok) {
         OLOG_WARNING("WebUI Vision: Magic bytes don't match MIME type %s", mime_type);
         return 4;
      }
   }

   return 0;
}
