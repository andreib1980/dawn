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
 * Claude API format conversion utilities.
 * Converts OpenAI-format conversation history to Claude's native format.
 */

#ifndef LLM_CLAUDE_FORMAT_H
#define LLM_CLAUDE_FORMAT_H

#include <json-c/json.h>
#include <stdbool.h>
#include <stddef.h>

#include "llm/llm_tools.h"

/**
 * @brief Check if conversation history has tool_use blocks without thinking blocks
 *
 * Claude requires that when thinking is enabled, assistant messages with tool_use
 * must start with a thinking block. This checks for incompatible history that would
 * cause Claude API to reject the request.
 *
 * @param conversation OpenAI-format conversation history
 * @return true if history has tool_use without thinking (incompatible with thinking mode)
 */
bool claude_history_has_tool_use_without_thinking(struct json_object *conversation);

/**
 * @brief Check if conversation history contains any thinking blocks
 *
 * Used to detect if thinking was previously enabled for this conversation.
 * If history has thinking blocks, we cannot disable thinking mid-conversation or
 * Claude will reject with "assistant message cannot contain thinking".
 *
 * @param conversation OpenAI-format conversation history
 * @return true if history contains thinking blocks
 */
bool claude_history_has_thinking_blocks(struct json_object *conversation);

/**
 * @brief Check if conversation history contains OpenAI-format tool_calls.
 *
 * These appear when the user switched providers mid-conversation (gpt-5.x or
 * gemini turn produced an OpenAI-shaped `tool_calls` field rather than a Claude
 * `tool_use` content block). Distinct from the catch-all
 * claude_history_has_tool_use_without_thinking() — useful for log severity:
 * Claude follow-ups after a tool_result legitimately omit thinking, so that
 * shape isn't a real concern; OpenAI tool_calls in a Claude conversation
 * usually are.
 *
 * @param conversation OpenAI-format conversation history
 * @return true if any assistant message carries a non-empty `tool_calls` array
 */
bool claude_history_has_openai_tool_calls(struct json_object *conversation);

/**
 * @brief Convert OpenAI-format conversation to Claude's native format
 *
 * Transforms conversation history from OpenAI's message format to Claude's format:
 * - Extracts system messages for Claude's system parameter
 * - Converts role names and content structure
 * - Handles tool calls and results
 * - Adds vision content if provided
 * - Handles thinking blocks for extended thinking mode
 *
 * @param openai_conversation OpenAI-format conversation array
 * @param input_text Current user input (may be NULL if already in conversation)
 * @param vision_images Array of base64-encoded image data (may be NULL)
 * @param vision_image_sizes Array of image sizes (may be NULL)
 * @param vision_image_count Number of images (0 if not used)
 * @param model Model name (NULL to use config default)
 * @param iteration Tool iteration count (0 for initial call, >0 for follow-ups).
 *                  Orphaned tool_use filtering only runs on iteration 0.
 * @return json_object containing Claude-format request, or NULL on error
 *         Caller must json_object_put() when done
 */
json_object *convert_to_claude_format(struct json_object *openai_conversation,
                                      const char *input_text,
                                      const char **vision_images,
                                      const size_t *vision_image_sizes,
                                      int vision_image_count,
                                      const char *model,
                                      int iteration);

#endif  // LLM_CLAUDE_FORMAT_H
