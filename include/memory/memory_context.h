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
 * Memory Context API
 *
 * Builds memory context blocks to inject into LLM system prompts.
 */

#ifndef MEMORY_CONTEXT_H
#define MEMORY_CONTEXT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Build memory context for a user.
 *
 * Constructs a formatted string containing the user's preferences, known
 * facts, and recent conversation summaries.  Designed to be injected into
 * the LLM system prompt for personalization.
 *
 * Sections:
 * - USER PREFERENCES: category: value (from memory_preferences)
 * - KNOWN FACTS: bullet points (from memory_facts, confidence >= 0.5)
 * - RECENT CONVERSATIONS: summaries with topics (from memory_summaries,
 *   <= 30 days old)
 *
 * Output uses an internal growable buffer (strbuf) bounded only by the
 * @p token_budget — never silently truncates rows mid-section the way the
 * earlier fixed-buffer API could.  When @p token_budget is exhausted
 * mid-section, an "[N more …]" elision marker is appended so the LLM
 * knows the listing was clipped.
 *
 * @param user_id User ID to build context for
 * @param token_budget Approximate token budget (chars / 4, default ~800).
 *                     Hard upper bound on total output size in characters
 *                     (= token_budget * 4).  Pass 0 to use a sane default.
 * @return Heap-allocated NUL-terminated string on success (caller must
 *         `free()`).  Returns NULL when there are no memories to surface
 *         or on allocation failure.  Empty user → NULL (callers can
 *         skip the prompt-augmentation branch cheaply).
 */
char *memory_build_context(int user_id, int token_budget);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_CONTEXT_H */
