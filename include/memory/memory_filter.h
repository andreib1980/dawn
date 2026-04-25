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
 * Memory injection filter — blocks prompt-injection payloads from being
 * stored as facts or preferences in the persistent memory system.
 */

#ifndef MEMORY_FILTER_H
#define MEMORY_FILTER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Normalize text for injection pattern matching.
 *
 * Strips zero-width/invisible Unicode characters, maps Cyrillic/Greek
 * homoglyphs to ASCII equivalents, collapses whitespace, and lowercases.
 * Non-ASCII characters not in the homoglyph table are dropped (the output
 * is ASCII-only). Caller must free() the returned string.
 *
 * @param text  Input text (UTF-8).
 * @return Heap-allocated normalized string, or NULL on error/NULL input.
 */
char *memory_filter_normalize(const char *text);

/**
 * @brief Check whether text contains a blocked injection pattern.
 *
 * Normalizes internally then checks against the blocklist and
 * structural attack detectors (ReAct co-occurrence, etc.).
 *
 * @param text  Input text to check.
 * @return true if the text should be REJECTED (contains injection).
 */
bool memory_filter_check(const char *text);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_FILTER_H */
