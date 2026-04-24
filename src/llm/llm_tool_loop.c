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
 * Central tool iteration loop for LLM streaming with tool calling.
 *
 * This module extracts the tool call -> execute -> re-call loop from the
 * individual providers (OpenAI, Claude) into a single central loop that:
 * - Runs auto-compaction between iterations (THE KEY FIX)
 * - Provides duplicate tool call detection for all providers
 * - Handles provider switching mid-loop (switch_llm tool)
 * - Enforces uniform iteration limits
 */

#include "llm/llm_tool_loop.h"

#include <stdlib.h>
#include <string.h>

#include "core/session_manager.h"
#include "llm/llm_claude.h"
#include "llm/llm_context.h"
#include "llm/llm_interface.h"
#include "llm/llm_openai.h"
#include "llm/llm_rate_limit.h"
#include "llm/llm_tools.h"
#include "logging.h"
#include "webui/webui_server.h"

/* Thread-local flag: set when the tool loop skips follow-up */
static _Thread_local bool s_did_skip_followup = false;

bool llm_tool_loop_did_skip_followup(void) {
   return s_did_skip_followup;
}

/* =============================================================================
 * History Format Helpers
 *
 * Format assistant messages with tool calls in the provider's native format.
 * Extracted from llm_openai.c and llm_claude.c recursion blocks.
 * ============================================================================= */

/**
 * @brief Add assistant message with tool calls in OpenAI format
 *
 * Appends the assistant message containing tool_calls array, then adds
 * tool result messages. Handles Gemini thought_signature if present.
 */
static void append_openai_tool_history(struct json_object *history,
                                       const llm_tool_response_t *response,
                                       const tool_result_list_t *results) {
   json_object *assistant_msg = json_object_new_object();
   json_object_object_add(assistant_msg, "role", json_object_new_string("assistant"));
   /* Preserve any text the LLM streamed before its tool calls, so the follow-up
    * iteration can see what was already said and won't repeat itself.
    * Use empty string as fallback for Gemini API compatibility (rejects NULL). */
   const char *pre_tool_text = (response->text && response->text[0] != '\0') ? response->text : "";
   json_object_object_add(assistant_msg, "content", json_object_new_string(pre_tool_text));

   json_object *tc_array = json_object_new_array();
   for (int i = 0; i < response->tool_calls.count; i++) {
      json_object *tc = json_object_new_object();
      json_object_object_add(tc, "id", json_object_new_string(response->tool_calls.calls[i].id));
      json_object_object_add(tc, "type", json_object_new_string("function"));

      json_object *func = json_object_new_object();
      json_object_object_add(func, "name",
                             json_object_new_string(response->tool_calls.calls[i].name));
      json_object_object_add(func, "arguments",
                             json_object_new_string(response->tool_calls.calls[i].arguments));
      json_object_object_add(tc, "function", func);

      /* Gemini 3+ models: Include thought_signature in first tool call */
      if (i == 0 && response->tool_calls.thought_signature[0] != '\0') {
         json_object *extra_content = json_object_new_object();
         json_object *google_obj = json_object_new_object();
         json_object_object_add(google_obj, "thought_signature",
                                json_object_new_string(response->tool_calls.thought_signature));
         json_object_object_add(extra_content, "google", google_obj);
         json_object_object_add(tc, "extra_content", extra_content);
         OLOG_INFO("Tool loop: Including Gemini thought_signature in follow-up request");
      }

      json_object_array_add(tc_array, tc);
   }
   json_object_object_add(assistant_msg, "tool_calls", tc_array);

   /* OpenAI Responses API round-trip: attach opaque per-provider state
    * (reasoning items + response_id) so the next iteration can echo it. */
   if (response->provider_state_json && *response->provider_state_json) {
      json_object *prov = json_tokener_parse(response->provider_state_json);
      if (prov) {
         json_object_object_add(assistant_msg, "_provider_state", prov);
      }
   }

   json_object_array_add(history, assistant_msg);

   /* Add tool results */
   llm_tools_add_results_openai(history, results);
}

/**
 * @brief Add assistant message with tool calls in Claude format
 *
 * Appends the assistant message containing thinking blocks (if any) and
 * tool_use content blocks, then adds tool result messages.
 */
static void append_claude_tool_history(struct json_object *history,
                                       const llm_tool_response_t *response,
                                       const tool_result_list_t *results) {
   json_object *assistant_msg = json_object_new_object();
   json_object_object_add(assistant_msg, "role", json_object_new_string("assistant"));

   json_object *content_array = json_object_new_array();

   /* If thinking was enabled, add the thinking block first (required by Claude API) */
   if (response->thinking_content) {
      json_object *thinking_block = json_object_new_object();
      json_object_object_add(thinking_block, "type", json_object_new_string("thinking"));
      json_object_object_add(thinking_block, "thinking",
                             json_object_new_string(response->thinking_content));

      if (response->thinking_signature) {
         json_object_object_add(thinking_block, "signature",
                                json_object_new_string(response->thinking_signature));
      }

      json_object_array_add(content_array, thinking_block);
   }

   /* Preserve any text the LLM streamed before its tool_use blocks, so the follow-up
    * iteration can see what was already said and won't repeat itself. */
   if (response->text && response->text[0] != '\0') {
      json_object *text_block = json_object_new_object();
      json_object_object_add(text_block, "type", json_object_new_string("text"));
      json_object_object_add(text_block, "text", json_object_new_string(response->text));
      json_object_array_add(content_array, text_block);
   }

   /* Add tool_use blocks */
   for (int i = 0; i < response->tool_calls.count; i++) {
      json_object *tool_use = json_object_new_object();
      json_object_object_add(tool_use, "type", json_object_new_string("tool_use"));
      json_object_object_add(tool_use, "id",
                             json_object_new_string(response->tool_calls.calls[i].id));
      json_object_object_add(tool_use, "name",
                             json_object_new_string(response->tool_calls.calls[i].name));

      json_object *args = json_tokener_parse(response->tool_calls.calls[i].arguments);
      if (args) {
         json_object_object_add(tool_use, "input", args);
      } else {
         json_object_object_add(tool_use, "input", json_object_new_object());
      }

      json_object_array_add(content_array, tool_use);
   }

   json_object_object_add(assistant_msg, "content", content_array);
   json_object_array_add(history, assistant_msg);

   /* Add tool results in Claude format */
   llm_tools_add_results_claude(history, results);
}

/**
 * @brief Add closing assistant message to complete tool call history
 *
 * When skip_followup is set, we need to add a synthetic assistant message
 * to prevent HTTP 400 on subsequent requests due to incomplete history.
 */
static void append_closing_message(struct json_object *history,
                                   const char *text,
                                   llm_history_format_t format) {
   if (!text) {
      return;
   }

   json_object *closing_msg = json_object_new_object();
   json_object_object_add(closing_msg, "role", json_object_new_string("assistant"));

   if (format == LLM_HISTORY_CLAUDE) {
      json_object *content_array = json_object_new_array();
      json_object *text_block = json_object_new_object();
      json_object_object_add(text_block, "type", json_object_new_string("text"));
      json_object_object_add(text_block, "text", json_object_new_string(text));
      json_object_array_add(content_array, text_block);
      json_object_object_add(closing_msg, "content", content_array);
   } else {
      json_object_object_add(closing_msg, "content", json_object_new_string(text));
   }

   json_object_array_add(history, closing_msg);
   OLOG_INFO("Tool loop: Added closing assistant message to complete history");
}

/**
 * @brief Free heap-allocated data from tool results (vision images, extended results)
 */
static void free_tool_result_resources(tool_result_list_t *results) {
   if (!results) {
      return;
   }
   for (int i = 0; i < results->count; i++) {
      if (results->results[i].vision_image) {
         free(results->results[i].vision_image);
         results->results[i].vision_image = NULL;
      }
      if (results->results[i].result_extended) {
         free(results->results[i].result_extended);
         results->results[i].result_extended = NULL;
      }
   }
}

/**
 * @brief Resolve current provider configuration for follow-up calls
 *
 * Checks if the provider was switched (e.g., switch_llm tool) and updates
 * the loop parameters accordingly.
 *
 * @return true if provider changed, false if same provider
 */
static bool resolve_provider_switch(llm_tool_loop_params_t *params) {
   llm_resolved_config_t current_config;

   if (llm_get_current_resolved_config(&current_config) != 0) {
      return false; /* Can't resolve config, stay on current provider */
   }

   /* Copy model to params-owned buffer (resolved ptr may dangle after return) */
   if (current_config.model && current_config.model[0] != '\0') {
      strncpy(params->model_storage, current_config.model, LLM_MODEL_NAME_MAX - 1);
      params->model_storage[LLM_MODEL_NAME_MAX - 1] = '\0';
      params->model = params->model_storage;
   }

   /* Determine new provider type and format */
   llm_single_shot_fn new_fn = params->provider_fn;
   llm_history_format_t new_format = params->history_format;
   bool switched = false;

   if (current_config.type == LLM_LOCAL || current_config.cloud_provider == CLOUD_PROVIDER_OPENAI ||
       current_config.cloud_provider == CLOUD_PROVIDER_GEMINI) {
      if (params->history_format == LLM_HISTORY_CLAUDE) {
         /* Switched from Claude to OpenAI/local/Gemini */
         new_fn = (llm_single_shot_fn)llm_openai_streaming_single_shot;
         new_format = LLM_HISTORY_OPENAI;
         switched = true;
         OLOG_INFO("Tool loop: Provider switched to OpenAI/local");
      }
   } else if (current_config.cloud_provider == CLOUD_PROVIDER_CLAUDE) {
      if (params->history_format == LLM_HISTORY_OPENAI) {
         /* Switched from OpenAI/local/Gemini to Claude */
         new_fn = (llm_single_shot_fn)llm_claude_streaming_single_shot;
         new_format = LLM_HISTORY_CLAUDE;
         switched = true;
         OLOG_INFO("Tool loop: Provider switched to Claude");
      }
   }

   /* Always update credentials (even if provider didn't change,
    * config may have changed model/endpoint) */
   params->base_url = current_config.endpoint ? current_config.endpoint : params->base_url;
   params->api_key = current_config.api_key;
   params->llm_type = current_config.type;
   params->cloud_provider = current_config.cloud_provider;

   if (switched) {
      params->provider_fn = new_fn;
      params->history_format = new_format;
   }

   return switched;
}

/* =============================================================================
 * Central Tool Iteration Loop
 * ============================================================================= */

char *llm_tool_iteration_loop(llm_tool_loop_params_t *params) {
   if (!params || !params->provider_fn || !params->conversation_history) {
      OLOG_ERROR("Tool loop: Invalid parameters");
      return NULL;
   }

   s_did_skip_followup = false;
   char *final_response = NULL;

   /* Persistent vision state across iterations.
    * When a tool produces vision data, we take ownership of it here
    * so it survives across loop iterations (stack arrays would go out of scope). */
   char *loop_vision_image = NULL;
   size_t loop_vision_size = 0;
   const char *loop_vision_arr[1] = { NULL };
   size_t loop_vision_size_arr[1] = { 0 };

   for (int iteration = 0; iteration <= LLM_TOOLS_MAX_ITERATIONS; iteration++) {
      /* Step 1: Auto-compact if needed (THE KEY FIX)
       * This runs EVERY iteration, not just once at the top of the entry point.
       * Without this, context can overflow during multi-step tool iterations. */
      llm_context_auto_compact_with_config(params->conversation_history, params->session_id,
                                           params->llm_type, params->cloud_provider, params->model);

      /* Step 2: Gate cloud calls through rate limiter */
      if (params->llm_type != LLM_LOCAL) {
         if (llm_rate_limit_wait()) {
            OLOG_WARNING("Tool loop: rate limit wait interrupted at iteration %d", iteration);
            return NULL;
         }
      }

      /* Step 3: Call provider single-shot */
      llm_tool_response_t result;
      memset(&result, 0, sizeof(result));

      int rc = params->provider_fn(params->conversation_history, params->input_text,
                                   params->vision_images, params->vision_image_sizes,
                                   params->vision_image_count, params->base_url, params->api_key,
                                   params->model, params->chunk_callback, params->callback_userdata,
                                   iteration, &result);

      if (rc != 0) {
         OLOG_ERROR("Tool loop: Provider call failed at iteration %d", iteration);
         llm_tool_response_free(&result);
         return NULL;
      }

      /* Step 4: If no tool calls, return text response */
      if (!result.has_tool_calls) {
         final_response = result.text;
         result.text = NULL; /* Transfer ownership to caller */
         llm_tool_response_free(&result);
         return final_response;
      }

      /* Flush any streamed text from this iteration before executing tools.
       * The LLM may have streamed text before its tool_use blocks (e.g. "Let me check
       * that for you."). Without this, the sentence buffer holds "you." waiting for more
       * text, and the next iteration's response concatenates directly ("you.Alright")
       * without a sentence break. The paragraph break triggers the sentence buffer to
       * emit the pending sentence for TTS before the tool execution pause. */
      if (params->chunk_callback) {
         typedef void (*text_chunk_cb)(const char *, void *);
         ((text_chunk_cb)params->chunk_callback)("\n\n", params->callback_userdata);
      }

      OLOG_INFO("Tool loop: %d tool call(s) at iteration %d/%d", result.tool_calls.count, iteration,
                LLM_TOOLS_MAX_ITERATIONS);

      for (int i = 0; i < result.tool_calls.count; i++) {
         OLOG_INFO("  Tool call [%d]: id=%s name=%s args=%s", i, result.tool_calls.calls[i].id,
                   result.tool_calls.calls[i].name, result.tool_calls.calls[i].arguments);
      }

      /* Step 5: Check for duplicate tool calls (prevents infinite loops) */
      if (result.tool_calls.count > 0 &&
          llm_tools_is_duplicate_call(params->conversation_history, result.tool_calls.calls[0].name,
                                      result.tool_calls.calls[0].arguments,
                                      params->history_format)) {
         OLOG_WARNING("Tool loop: Duplicate tool call detected, forcing text response");

         /* Add a hint to use existing results */
         json_object *hint_msg = json_object_new_object();
         json_object_object_add(hint_msg, "role", json_object_new_string("user"));
         json_object_object_add(
             hint_msg, "content",
             json_object_new_string(
                 "[System: You already performed this search. Use the search results you "
                 "already have to answer the question. Do not search again.]"));
         json_object_array_add(params->conversation_history, hint_msg);

         llm_tool_response_free(&result);

         /* Make one more call with tools disabled (iteration = MAX forces no tools) */
         OLOG_INFO("Tool loop: Making final call without tools to force text response");
         memset(&result, 0, sizeof(result));
         rc = params->provider_fn(params->conversation_history, "", NULL, NULL, 0, params->base_url,
                                  params->api_key, params->model, params->chunk_callback,
                                  params->callback_userdata, LLM_TOOLS_MAX_ITERATIONS, &result);

         if (rc != 0) {
            llm_tool_response_free(&result);
            return NULL;
         }

         final_response = result.text;
         result.text = NULL;
         llm_tool_response_free(&result);
         return final_response;
      }

      /* Step 6: Execute tools */
      tool_result_list_t *results = calloc(1, sizeof(tool_result_list_t));
      if (!results) {
         OLOG_ERROR("Tool loop: Failed to allocate tool results");
         llm_tool_response_free(&result);
         return NULL;
      }
      llm_tools_execute_all(&result.tool_calls, results);

      /* Log tool results */
      for (int i = 0; i < results->count; i++) {
         const char *content = tool_result_content(&results->results[i]);
         OLOG_INFO("  Tool result [%d] id=%s result=%.200s%s", i, results->results[i].tool_call_id,
                   content, strlen(content) > 200 ? "..." : "");
      }

      /* Step 7: Check follow-up context BEFORE appending to history.
       * Tools like reset_conversation invalidate the conversation history pointer,
       * so we must not append to it after they run. */
      tool_followup_context_t followup;
      llm_tools_prepare_followup(results, &followup);

      if (followup.skip_followup) {
         OLOG_INFO("Tool loop: Skipping follow-up (tool requested no follow-up)");
         s_did_skip_followup = true;

         /* Send through chunk callback so TTS receives it */
         if (followup.direct_response && params->chunk_callback) {
            void (*cb)(const char *, void *) = params->chunk_callback;
            cb(followup.direct_response, params->callback_userdata);
         }

         free_tool_result_resources(results);
         free(results);
         llm_tool_response_free(&result);
         return followup.direct_response; /* Caller must free */
      }

      /* Step 8: Append assistant message + tool results to history */
      if (params->history_format == LLM_HISTORY_CLAUDE) {
         append_claude_tool_history(params->conversation_history, &result, results);
      } else {
         append_openai_tool_history(params->conversation_history, &result, results);
      }

      /* Step 8b: All-silent check — tools handled their own output */
      if (followup.all_silent) {
         OLOG_INFO("Tool loop: All tools silent (should_respond=false), skipping follow-up");
         append_closing_message(params->conversation_history,
                                "[Tool execution completed without follow-up response]",
                                params->history_format);
         free_tool_result_resources(results);
         free(results);
         llm_tool_response_free(&result);
         return strdup(""); /* Empty = no TTS output */
      }

      /* Step 9: Check iteration limit — force a final text response with what we have */
      if (iteration >= LLM_TOOLS_MAX_ITERATIONS) {
         OLOG_WARNING("Tool loop: Max iterations (%d) reached, forcing text response",
                      LLM_TOOLS_MAX_ITERATIONS);

         /* Inject a system hint telling the LLM to respond with what it has */
         json_object *hint_msg = json_object_new_object();
         json_object_object_add(hint_msg, "role", json_object_new_string("user"));
         json_object_object_add(
             hint_msg, "content",
             json_object_new_string(
                 "[System: Maximum tool iterations reached. Respond to the user now with "
                 "the information you have gathered so far. Do not call any more tools.]"));
         json_object_array_add(params->conversation_history, hint_msg);

         free_tool_result_resources(results);
         free(results);
         llm_tool_response_free(&result);

         /* Make one final call with tools disabled */
         OLOG_INFO("Tool loop: Making final call without tools to present gathered results");
         memset(&result, 0, sizeof(result));
         int final_rc = params->provider_fn(params->conversation_history, "", NULL, NULL, 0,
                                            params->base_url, params->api_key, params->model,
                                            params->chunk_callback, params->callback_userdata,
                                            LLM_TOOLS_MAX_ITERATIONS, &result);

         char *final_text = NULL;
         if (final_rc == 0 && result.text) {
            final_text = strdup(result.text);
         }
         llm_tool_response_free(&result);
         free(loop_vision_image);
         return final_text;
      }

      /* Step 10: Extract vision data from tool results (take ownership) */
      free(loop_vision_image); /* Free previous iteration's vision */
      loop_vision_image = NULL;
      loop_vision_size = 0;

      for (int i = 0; i < results->count; i++) {
         if (results->results[i].vision_image && results->results[i].vision_image_size > 0) {
            /* Take ownership: steal the pointer so free_tool_result_vision won't free it */
            loop_vision_image = results->results[i].vision_image;
            loop_vision_size = results->results[i].vision_image_size;
            results->results[i].vision_image = NULL; /* Prevent double-free */
            OLOG_INFO("Tool loop: Including vision from tool result (%zu bytes)", loop_vision_size);
            break;
         }
      }

      /* Update params to point to persistent vision state for next iteration */
      if (loop_vision_image) {
         loop_vision_arr[0] = loop_vision_image;
         loop_vision_size_arr[0] = loop_vision_size;
         params->vision_images = loop_vision_arr;
         params->vision_image_sizes = loop_vision_size_arr;
         params->vision_image_count = 1;
      } else {
         params->vision_images = NULL;
         params->vision_image_sizes = NULL;
         params->vision_image_count = 0;
      }

      /* Step 11: Check interrupt */
      if (llm_is_interrupt_requested()) {
         OLOG_INFO("Tool loop: Interrupted by user");
         free_tool_result_resources(results);
         free(results);
         llm_tool_response_free(&result);
         free(loop_vision_image);
         return NULL;
      }

      /* Step 12: Handle provider switching (switch_llm tool) */
      resolve_provider_switch(params);

      /* Clear input text for follow-up calls (history already contains the context) */
      params->input_text = "";

      /* Cleanup for next iteration */
      free_tool_result_resources(results);
      free(results);
      llm_tool_response_free(&result);
   }

   /* Should not reach here (loop exits via returns) */
   OLOG_ERROR("Tool loop: Fell through iteration loop unexpectedly");
   free(loop_vision_image);
   return NULL;
}
