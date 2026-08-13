/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * By contributing to this project, you agree to license your contributions
 * under the GPLv3 (or any later version) or any future licenses chosen by
 * the project author(s).
 */

#include "llm/llm_runtime_identity.h"

#include <stdio.h>

#include "dawn_error.h"

int llm_runtime_identity_format(const char *provider,
                                const char *model,
                                char *output,
                                size_t output_size) {
   if (output == NULL || output_size == 0) {
      return FAILURE;
   }

   output[0] = '\0';

   if (provider == NULL || provider[0] == '\0' || model == NULL || model[0] == '\0') {
      return FAILURE;
   }

   int written = snprintf(output, output_size,
                          "[Runtime identity]\n"
                          "Your active LLM provider is \"%s\" and your exact configured model "
                          "identifier is \"%s\". When asked which provider or model you are "
                          "using, answer with these exact values. Never guess, infer, or "
                          "substitute another provider or model name.",
                          provider, model);

   if (written < 0 || (size_t)written >= output_size) {
      output[0] = '\0';
      return FAILURE;
   }

   return SUCCESS;
}
