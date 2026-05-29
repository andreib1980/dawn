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
 * OpenAI provider — public entry points and shared helpers. Dispatches to
 * either the /v1/chat/completions implementation (llm_openai_chat_completions.c)
 * or the /v1/responses implementation (llm_openai_responses.c) based on model
 * and configuration.
 */

#include "llm/llm_openai.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config/dawn_config.h"
#include "llm/llm_interface.h"
#include "llm/llm_openai_internal.h"
#include "llm/llm_openai_responses.h"
#include "llm/llm_tools.h"
#include "logging.h"

/* ── Routing ────────────────────────────────────────────────────────────── */

/**
 * @brief Decide whether this call should use /v1/responses instead of /v1/chat/completions.
 *
 * Triggers when api_key is set (cloud path only), and the configured mode permits it:
 *   - "auto"   (default) — route iff llm_openai_model_prefers_responses_api(model)
 *   - "always"           — route every cloud OpenAI call
 *   - "never"            — never route (gpt-5.4 will fail per OpenAI's HTTP 400)
 *
 * Local LLMs and non-OpenAI cloud providers (Gemini, OpenRouter, and other
 * OpenAI-compat proxies) never route — they only serve /v1/chat/completions.
 */
static bool should_dispatch_to_responses_api(const char *api_key,
                                             const char *base_url,
                                             const char *model_name) {
   if (!api_key)
      return false;
   if (!model_name || !*model_name)
      return false;
   if (base_url &&
       (strstr(base_url, "generativelanguage.googleapis.com") || strstr(base_url, "openrouter.ai")))
      return false;

   const char *mode = g_config.llm.cloud.openai_use_responses_api;
   if (!mode || !*mode)
      mode = "auto";

   if (strcmp(mode, "never") == 0)
      return false;
   if (strcmp(mode, "always") == 0)
      return true;
   /* auto */
   return llm_openai_model_prefers_responses_api(model_name);
}

/* ── Shared helpers (used by chat-completions and responses) ────────────── */

struct curl_slist *llm_openai_build_headers(const char *api_key, const char *base_url) {
   struct curl_slist *headers = NULL;

   headers = curl_slist_append(headers, "Content-Type: application/json");

   if (api_key != NULL) {
      char auth_header[512];
      snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", api_key);
      headers = curl_slist_append(headers, auth_header);
   }

   /* OpenRouter optional attribution headers (used for their app rankings).
    * Harmless on other endpoints, but only sent when talking to OpenRouter. */
   if (base_url != NULL && strstr(base_url, "openrouter.ai") != NULL) {
      headers = curl_slist_append(headers,
                                  "HTTP-Referer: https://github.com/The-OASIS-Project/dawn");
      headers = curl_slist_append(headers, "X-Title: DAWN");
   }

   return headers;
}

const char *llm_openai_parse_error_message(const char *response_body, long http_code) {
   static _Thread_local char error_msg[512];

   if (response_body && *response_body) {
      struct json_object *parsed = json_tokener_parse(response_body);
      if (parsed) {
         struct json_object *error_obj, *msg_obj;
         if (json_object_object_get_ex(parsed, "error", &error_obj) &&
             json_object_object_get_ex(error_obj, "message", &msg_obj)) {
            const char *msg = json_object_get_string(msg_obj);
            if (msg && *msg) {
               snprintf(error_msg, sizeof(error_msg), "%s", msg);
               json_object_put(parsed);
               return error_msg;
            }
         }
         json_object_put(parsed);
      }
   }

   snprintf(error_msg, sizeof(error_msg), "Request failed with HTTP %ld", http_code);
   return error_msg;
}

bool llm_openai_is_gpt5_base_family(const char *model_name) {
   if (!model_name)
      return false;
   if (strncmp(model_name, "gpt-5", 5) != 0)
      return false;
   /* gpt-5, gpt-5-mini, gpt-5-nano — but NOT gpt-5.1, gpt-5.2, gpt-5.4* */
   if (model_name[5] == '\0' || model_name[5] == '-')
      return true;
   return false;
}

bool llm_openai_model_prefers_responses_api(const char *model_name) {
   if (!model_name)
      return false;
   /* gpt-5.4 family: "gpt-5.4", "gpt-5.4-mini", "gpt-5.4-turbo", etc. */
   if (strncmp(model_name, "gpt-5.4", 7) == 0)
      return true;
   return false;
}

const char *llm_openai_clamp_effort_for_model(const char *model_name, const char *effort) {
   if (!effort || !model_name)
      return effort;

   bool is_o_series = (strncmp(model_name, "o1", 2) == 0 || strncmp(model_name, "o3", 2) == 0);
   bool is_gpt5_base = llm_openai_is_gpt5_base_family(model_name);
   bool is_gemini = (strncmp(model_name, "gemini-", 7) == 0);

   /* xhigh: only gpt-5.2+ supports it */
   if (strcmp(effort, "xhigh") == 0) {
      if (is_o_series || is_gpt5_base || is_gemini)
         return "high";
   }

   /* none: o-series and Gemini don't support it */
   if (strcmp(effort, "none") == 0) {
      if (is_o_series || is_gemini)
         return "low";
      if (is_gpt5_base)
         return "minimal";
   }

   /* minimal: only gpt-5 base family */
   if (strcmp(effort, "minimal") == 0) {
      if (!is_gpt5_base)
         return "low";
   }

   return effort;
}

/* ── Public entry points (dispatch to chat-completions or responses) ────── */

char *llm_openai_chat_completion(struct json_object *conversation_history,
                                 const char *input_text,
                                 const char **vision_images,
                                 const size_t *vision_image_sizes,
                                 int vision_image_count,
                                 const char *base_url,
                                 const char *api_key,
                                 const char *model) {
   if (should_dispatch_to_responses_api(api_key, base_url, model)) {
      OLOG_ERROR("OpenAI: model '%s' requires /v1/responses; "
                 "use llm_openai_streaming_single_shot() instead",
                 model ? model : "(default)");
      return NULL;
   }

   return llm_openai_cc_chat_completion(conversation_history, input_text, vision_images,
                                        vision_image_sizes, vision_image_count, base_url, api_key,
                                        model);
}

char *llm_openai_chat_completion_streaming(struct json_object *conversation_history,
                                           const char *input_text,
                                           const char **vision_images,
                                           const size_t *vision_image_sizes,
                                           int vision_image_count,
                                           const char *base_url,
                                           const char *api_key,
                                           const char *model,
                                           llm_openai_text_chunk_callback chunk_callback,
                                           void *callback_userdata) {
   if (should_dispatch_to_responses_api(api_key, base_url, model)) {
      OLOG_ERROR("OpenAI: model '%s' requires /v1/responses; "
                 "use llm_openai_streaming_single_shot() instead",
                 model ? model : "(default)");
      return NULL;
   }
   return llm_openai_cc_streaming(conversation_history, input_text, vision_images,
                                  vision_image_sizes, vision_image_count, base_url, api_key, model,
                                  chunk_callback, callback_userdata);
}

int llm_openai_streaming_single_shot(struct json_object *conversation_history,
                                     const char *input_text,
                                     const char **vision_images,
                                     const size_t *vision_image_sizes,
                                     int vision_image_count,
                                     const char *base_url,
                                     const char *api_key,
                                     const char *model,
                                     llm_openai_text_chunk_callback chunk_callback,
                                     void *callback_userdata,
                                     int iteration,
                                     llm_tool_response_t *result) {
   if (!result) {
      return 1;
   }

   /* Resolve model for routing decision */
   const char *route_model = model;
   if (!route_model || !*route_model) {
      route_model = (api_key == NULL) ? g_config.llm.local.model : llm_get_default_openai_model();
   }

   if (should_dispatch_to_responses_api(api_key, base_url, route_model)) {
      return llm_openai_responses_streaming_single_shot(
          conversation_history, input_text, vision_images, vision_image_sizes, vision_image_count,
          base_url, api_key, route_model, chunk_callback, callback_userdata, iteration, result);
   }

   return llm_openai_cc_streaming_single_shot(conversation_history, input_text, vision_images,
                                              vision_image_sizes, vision_image_count, base_url,
                                              api_key, model, chunk_callback, callback_userdata,
                                              iteration, result);
}
