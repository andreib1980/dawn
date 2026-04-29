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
 * Stub symbols for test_crypto_store. Provides g_config and path_expand_tilde
 * so crypto_store.c links without the full daemon.
 */

#include <string.h>

#include "config/dawn_config.h"
#include "core/path_utils.h"

dawn_config_t g_config;
secrets_config_t g_secrets;

bool path_expand_tilde(const char *path, char *expanded, size_t expanded_size) {
   if (!path || !expanded || expanded_size == 0)
      return false;
   strncpy(expanded, path, expanded_size - 1);
   expanded[expanded_size - 1] = '\0';
   return true;
}
