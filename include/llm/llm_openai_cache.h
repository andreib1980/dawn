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
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Whether the OpenRouter→Anthropic explicit prompt-cache path applies.
 *
 * True only when @p base_url targets openrouter.ai AND @p model_name is an
 * `anthropic/`-prefixed slug.  This is the gate for llm_openai_add_anthropic_cache and,
 * equivalently, the discriminator that selects between keeping the two-segment
 * system-prompt split (Anthropic cache breakpoint) and collapsing it (every
 * other provider).  NULL @p base_url or @p model_name yields false.
 */
bool llm_openai_anthropic_cache_applies(const char *model_name, const char *base_url);

/**
 * @brief Finalize a chat-completions request's system-prompt shape for caching.
 *
 * Mutually-exclusive dispatch keyed on llm_openai_anthropic_cache_applies():
 *   - Anthropic path  → llm_openai_add_anthropic_cache() (keeps the stable/volatile
 *     system split SEPARATE so the cache_control breakpoint covers the stable
 *     region only).
 *   - every other path → llm_openai_merge_leading_system_messages() (collapses the
 *     split into one system message).
 *
 * Call this from each request builder instead of add_anthropic_cache directly.
 *
 * @param root       Request body (modified in place: tools[] and/or "messages").
 * @param model_name Model name / OpenRouter slug. May be NULL.
 * @param base_url   Request base URL. May be NULL.
 */
void llm_openai_apply_system_prompt_caching(struct json_object *root,
                                            const char *model_name,
                                            const char *base_url);

/**
 * @brief Collapse the contiguous run of leading plain-string system messages
 *        into a single system message.
 *
 * DAWN emits the system prompt as TWO consecutive system messages (stable prefix
 * + volatile tail) purely as an Anthropic explicit-cache boundary marker.  The
 * real OpenAI API tolerates multiple system messages, but a strict local Jinja
 * chat template (Qwen 3.5/3.6) hard-raises "System message must be at the
 * beginning" on the second one → HTTP 500.  For providers that do NOT use the
 * Anthropic breakpoint (native OpenAI implicit caching, llama.cpp/Ollama KV
 * prefix reuse) the split is inert, so merging is safe and fixes the rejection.
 *
 * Merges only the leading run whose messages carry plain-string content; a
 * non-string (e.g. already cache-wrapped) system message ends the run.  No-op if
 * fewer than two such messages lead the array.  Bodies are joined with a blank
 * line ("\n\n").
 *
 * Copy-on-write: @p root's "messages" array may ALIAS the session's canonical
 * conversation_history, so the merged run is written into a request-private clone
 * of the messages array; the session's history object is never mutated (same
 * contract as llm_openai_add_anthropic_cache).
 *
 * @param root Request body (its "messages" array may be swapped for a private clone).
 */
void llm_openai_merge_leading_system_messages(struct json_object *root);

/**
 * @brief Inject Anthropic prompt-cache breakpoints into an OpenRouter
 *        chat-completions request body.
 *
 * No-op unless @p base_url targets openrouter.ai AND @p model_name is an
 * `anthropic/`-prefixed slug.  When it applies, it marks two cache breakpoints in
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
