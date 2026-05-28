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
 * Two-segment prompt composer.  Implementation; contract documented
 * in prompt_compose.h.
 */

#include "core/prompt_compose.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dawn_error.h"

void prompt_compose_free(composed_prompt_t *p) {
   if (p == NULL)
      return;
   free(p->stable_prefix);
   p->stable_prefix = NULL;
   free(p->volatile_block);
   p->volatile_block = NULL;
}

char *prompt_compose_to_string(const composed_prompt_t *blocks) {
   if (blocks == NULL)
      return NULL;

   const bool have_stable = blocks->stable_prefix != NULL && blocks->stable_prefix[0] != '\0';
   const bool have_volatile = blocks->volatile_block != NULL && blocks->volatile_block[0] != '\0';
   if (!have_stable && !have_volatile)
      return NULL;

   const size_t stable_len = have_stable ? strlen(blocks->stable_prefix) : 0;
   const size_t volatile_len = have_volatile ? strlen(blocks->volatile_block) : 0;

   /* No separator: the volatile_block already opens with its own
    * framing markers ("\n\n--- USER MEMORY ---" / "\n\n--- TURN CONTEXT ---")
    * so a manufactured "\n\n" here would just double the gap. */
   char *out = malloc(stable_len + volatile_len + 1);
   if (out == NULL)
      return NULL;

   size_t off = 0;
   if (have_stable) {
      memcpy(out + off, blocks->stable_prefix, stable_len);
      off += stable_len;
   }
   if (have_volatile) {
      memcpy(out + off, blocks->volatile_block, volatile_len);
      off += volatile_len;
   }
   out[off] = '\0';
   return out;
}

uint32_t prompt_compose_fnv1a(const char *s) {
   /* FNV-1a 32-bit.  Basis + prime constants per the FNV reference. */
   uint32_t h = 0x811c9dc5u;
   if (s == NULL)
      return h;
   for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++) {
      h ^= (uint32_t)*p;
      h *= 0x01000193u;
   }
   return h;
}
