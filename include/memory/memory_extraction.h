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
 * Memory Extraction API
 *
 * Triggers memory extraction at session end to store facts, preferences,
 * and conversation summaries.
 */

#ifndef MEMORY_EXTRACTION_H
#define MEMORY_EXTRACTION_H

#include <json-c/json.h>
#include <stdbool.h>

#include "llm/llm_interface.h"
#include "memory/memory_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Fallback LLM info for extraction retry
 *
 * When the configured extraction model is unavailable (e.g. HTTP 404),
 * the extraction thread retries using the session's active model.
 * Pass NULL to memory_trigger_extraction() to disable fallback.
 */
typedef struct {
   llm_type_t type;
   cloud_provider_t cloud_provider;
   char endpoint[128];
   char model[LLM_MODEL_NAME_MAX];
} memory_extraction_fallback_t;

/**
 * @brief Build fallback LLM info from a session's active model
 *
 * Uses session_get_llm_config() for proper mutex acquisition.
 *
 * @param session Session to read LLM config from (may be NULL)
 * @param fb      Output fallback struct (zeroed if session is NULL)
 */
struct session;
void memory_extraction_build_fallback(struct session *session, memory_extraction_fallback_t *fb);

/**
 * @brief Trigger memory extraction for a session
 *
 * Spawns a detached thread to extract facts, preferences, and a summary
 * from the conversation history. The extraction runs asynchronously so
 * it doesn't block session cleanup.
 *
 * Supports incremental extraction: if conversation_id is provided, only
 * processes messages since the last extraction (tracked via last_extracted_msg_count).
 *
 * @param user_id User ID (must be > 0)
 * @param conversation_id Conversation ID for tracking (0 to skip tracking)
 * @param session_id_str Session identifier string (for summary)
 * @param conversation_history JSON array of messages (copied internally)
 * @param message_count Number of messages in conversation
 * @param duration_seconds Session duration
 * @param fallback Session's active LLM config to retry with on failure (NULL to disable)
 * @return 0 on success, 1 on failure
 */
int memory_trigger_extraction(int user_id,
                              int64_t conversation_id,
                              const char *session_id_str,
                              struct json_object *conversation_history,
                              int message_count,
                              int duration_seconds,
                              const memory_extraction_fallback_t *fallback);

/**
 * @brief Check if extraction is in progress for a user
 *
 * @param user_id User ID to check
 * @return true if extraction is running, false otherwise
 */
bool memory_extraction_in_progress(int user_id);

/**
 * @brief Parse a JSON response from an LLM, handling common formatting variants.
 *
 * Tries direct parse, markdown code block extraction, and brace/bracket fallback.
 * Caller owns the returned object and must call json_object_put().
 *
 * @param response Raw LLM response text
 * @return Parsed json_object, or NULL on failure
 */
struct json_object *memory_extraction_parse_json(const char *response);

/**
 * @brief Return the character size of the extraction prompt template.
 *
 * The prompt sent to the LLM is the template plus an anchor-date line plus
 * the conversation JSON plus the user's existing-profile snapshot.  This
 * helper exposes the constant template size so cost estimators can avoid
 * hardcoding a guess (the actual template is ~3-5K characters, well above
 * historical 1500-token guesses).  Caller adds per-message + profile
 * estimates on top.
 *
 * @return Size of EXTRACTION_PROMPT_TEMPLATE in characters (excluding NUL).
 */
size_t memory_extraction_get_template_size_chars(void);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_EXTRACTION_H */
