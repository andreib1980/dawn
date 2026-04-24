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
 * Internal helpers shared between the OpenAI chat-completions implementation
 * (llm_openai.c) and the OpenAI Responses API implementation
 * (llm_openai_responses.c). Not part of the public LLM API; do not include
 * outside the OpenAI provider module.
 */

#ifndef LLM_OPENAI_INTERNAL_H
#define LLM_OPENAI_INTERNAL_H

#include <curl/curl.h>
#include <stdbool.h>

/**
 * @brief Build standard OpenAI HTTP headers (Content-Type + optional Bearer auth).
 *
 * @param api_key API key for cloud (NULL for local LLM — auth header omitted).
 * @return CURL header list (caller must free with curl_slist_free_all).
 */
struct curl_slist *llm_openai_build_headers(const char *api_key);

/**
 * @brief Extract the human-readable error message from an OpenAI API error body.
 *
 * Parses the standard `{"error": {"message": "...", ...}}` envelope shared by
 * both /v1/chat/completions and /v1/responses. Returns a thread-local buffer;
 * do not free, do not retain across calls.
 *
 * @param response_body Raw HTTP response body (may be empty/NULL).
 * @param http_code HTTP status code (used in fallback message).
 * @return Pointer to thread-local error string.
 */
const char *llm_openai_parse_error_message(const char *response_body, long http_code);

/**
 * @brief Whether the model is in the GPT-5 base family.
 *
 * Only the original gpt-5 / gpt-5-mini / gpt-5-nano accept reasoning_effort=minimal.
 * gpt-5.1 and later versioned variants reject "minimal" and use "none" instead.
 */
bool llm_openai_is_gpt5_base_family(const char *model_name);

/**
 * @brief Whether the model should be routed to /v1/responses instead of /v1/chat/completions.
 *
 * Returns true for gpt-5.4* (and any future model OpenAI gates the same way), where
 * combining reasoning_effort with function tools is rejected on chat completions.
 * The caller still chooses based on the configured mode (auto/always/never).
 */
bool llm_openai_model_prefers_responses_api(const char *model_name);

/**
 * @brief Clamp a `reasoning_effort` string to what the given model accepts on
 *        /v1/chat/completions or /v1/responses.
 *
 * Different model families accept different value sets:
 *   gpt-5 base/-mini/-nano:  minimal | low | medium | high
 *   gpt-5.1:                 none | low | medium | high
 *   gpt-5.2 / gpt-5.4*:      none | low | medium | high | xhigh
 *   o1 / o3 series:          low | medium | high
 *   Gemini 2.5+/3.x (OAI-compat): low | medium | high (no none, no xhigh, no minimal)
 *
 * `xhigh` not supported → clamps to `high`. `none` not supported → clamps to
 * `minimal` for gpt-5 base/-mini/-nano, otherwise to `low`. `minimal` is only
 * valid on gpt-5 base; clamps to `low` elsewhere. Returns a string-literal
 * pointer; do not free.
 */
const char *llm_openai_clamp_effort_for_model(const char *model_name, const char *effort);

#endif /* LLM_OPENAI_INTERNAL_H */
