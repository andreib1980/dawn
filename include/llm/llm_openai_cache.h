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
 * Anthropic prompt-cache breakpoint injection for OpenRouter chat-completions
 * requests.  Split out of llm_openai_chat_completions.c so the (json-c-only)
 * logic is unit-testable without the provider's curl/config/streaming deps.
 */

#ifndef LLM_OPENAI_CACHE_H
#define LLM_OPENAI_CACHE_H

#include <json-c/json.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Inject Anthropic prompt-cache breakpoints into an OpenRouter
 *        chat-completions request body.
 *
 * No-op unless @p base_url targets openrouter.ai AND @p model_name is an
 * "anthropic/*" slug.  When it applies, it marks two cache breakpoints in
 * OpenAI chat-completions shape (mirroring the two native llm_claude_format.c
 * breakpoints): cache_control on the last tool, and the first system message
 * presented as a single-element content array carrying cache_control.
 *
 * Copy-on-write: @p root's "messages" array may ALIAS the caller's canonical
 * conversation_history (the history-prep step returns it by shared reference in
 * the common path).  To avoid corrupting session-owned state, the first system
 * message is wrapped inside a request-private clone of the messages array; the
 * session's history object is never mutated.
 *
 * @param root       Request body (modified in place: tools[] and "messages").
 * @param model_name OpenRouter model slug (e.g. "anthropic/claude-..."). May be NULL.
 * @param base_url   Request base URL. May be NULL.
 */
void llm_openai_add_anthropic_cache(struct json_object *root,
                                    const char *model_name,
                                    const char *base_url);

#ifdef __cplusplus
}
#endif

#endif /* LLM_OPENAI_CACHE_H */
