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
 * LLM Context Management - Track context usage and auto-summarize conversations
 */

#include "llm/llm_context.h"

#include <curl/curl.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "config/dawn_config.h"
#include "core/session_manager.h"
#include "llm/llm_interface.h"
#include "llm/llm_local_provider.h"
#include "llm/llm_tools.h"
#include "logging.h"
#include "tools/curl_buffer.h"
#include "tts/text_to_speech.h"
#include "utils/string_utils.h"
#ifdef ENABLE_WEBUI
#include "webui/webui_server.h"
#endif

/* Forward declaration — avoid including path_utils.h which conflicts with
 * string_utils.h's static inline safe_strncpy */
bool path_ensure_parent_dir(const char *file_path);

/* =============================================================================
 * Configuration Access
 * ============================================================================= */

extern dawn_config_t g_config;

#include "tools/time_utils.h"

/* =============================================================================
 * Model Context Size Lookup Table
 * ============================================================================= */

typedef struct {
   const char *model_prefix; /* Model name prefix to match */
   int context_size;         /* Context size in tokens */
} model_context_entry_t;

/* OpenAI models (verified February 2026 from developers.openai.com) */
static const model_context_entry_t s_openai_models[] = {
   /* GPT-5.x family (400K context) */
   { "gpt-5.2", 400000 },
   { "gpt-5.1", 400000 },
   { "gpt-5-mini", 400000 },
   { "gpt-5-nano", 400000 },
   { "gpt-5", 400000 },
   /* GPT-4.1 family (1,047,576 context) */
   { "gpt-4.1-mini", 1047576 },
   { "gpt-4.1-nano", 1047576 },
   { "gpt-4.1", 1047576 },
   /* O-series reasoning models */
   { "o4-mini", 200000 },
   { "o3-pro", 200000 },
   { "o3-mini", 200000 },
   { "o3", 200000 },
   { "o1-pro", 200000 },
   { "o1-mini", 128000 },
   { "o1-preview", 128000 },
   { "o1", 200000 },
   /* GPT-4o family */
   { "gpt-4o-mini", 128000 },
   { "gpt-4o", 128000 },
   /* GPT-4 Turbo */
   { "gpt-4-turbo", 128000 },
   { "gpt-4-0125", 128000 },
   { "gpt-4-1106", 128000 },
   /* Legacy GPT-4 */
   { "gpt-4-32k", 32768 },
   { "gpt-4", 8192 },
   /* Legacy GPT-3.5 */
   { "gpt-3.5-turbo-16k", 16384 },
   { "gpt-3.5-turbo", 16384 },
   { NULL, 0 } /* Sentinel */
};

/* Claude models (verified February 2026 from platform.claude.com)
 * All Claude models use 200K context (1M available via beta header, but
 * we report the standard limit since DAWN doesn't use the beta header). */
static const model_context_entry_t s_claude_models[] = {
   /* Claude 4.6 family */
   { "claude-opus-4-6", 200000 },
   { "claude-sonnet-4-6", 200000 },
   /* Claude 4.5 family */
   { "claude-haiku-4-5", 200000 },
   { "claude-opus-4-5", 200000 },
   { "claude-sonnet-4-5", 200000 },
   /* Claude 4.x family */
   { "claude-opus-4.5", 200000 },
   { "claude-sonnet-4.5", 200000 },
   { "claude-opus-4.1", 200000 },
   { "claude-sonnet-4.1", 200000 },
   { "claude-opus-4", 200000 },
   { "claude-sonnet-4", 200000 },
   /* Claude 3.5 family */
   { "claude-3-5-sonnet", 200000 },
   { "claude-3-5-haiku", 200000 },
   { "claude-3.5", 200000 },
   /* Claude 3 family (haiku deprecated April 2026) */
   { "claude-3-opus", 200000 },
   { "claude-3-sonnet", 200000 },
   { "claude-3-haiku", 200000 },
   { NULL, 0 } /* Sentinel */
};

/* Gemini models (verified February 2026 from firebase.google.com/docs/ai-logic/models)
 * All current Gemini models use 1,048,576 (1M) input token limit. */
static const model_context_entry_t s_gemini_models[] = {
   /* Gemini 3.x family (preview) */
   { "gemini-3.1-pro", 1048576 },
   { "gemini-3-flash", 1048576 },
   { "gemini-3-pro", 1048576 },
   /* Gemini 2.5 family */
   { "gemini-2.5-pro", 1048576 },
   { "gemini-2.5-flash-lite", 1048576 },
   { "gemini-2.5-flash", 1048576 },
   /* Gemini 2.0 (retiring June 2026) */
   { "gemini-2.0-flash-lite", 1048576 },
   { "gemini-2.0-flash", 1048576 },
   /* Legacy Gemini 1.5 */
   { "gemini-1.5-pro", 1048576 },
   { "gemini-1.5-flash", 1048576 },
   { NULL, 0 } /* Sentinel */
};

/* =============================================================================
 * Module State
 * ============================================================================= */

/* Re-query local context size every 5 minutes (matches model list TTL) */
#define LLM_CONTEXT_LOCAL_TTL 300

static struct {
   bool initialized;
   int local_context_size;            /* Cached context size */
   bool local_context_queried;        /* True if we've queried local LLM */
   bool local_context_authoritative;  /* True if from runtime source (not model max/default) */
   bool local_context_querying;       /* True if a thread is currently doing HTTP refresh */
   time_t local_context_queried_at;   /* When context was last queried (for TTL) */
   uint32_t local_context_generation; /* Incremented on invalidation, detects stale writes */
   int last_prompt_tokens;            /* Last known prompt tokens (for WebUI) */
   int last_context_size;             /* Last known context size (for WebUI) */
   pthread_mutex_t mutex;             /* Protects state */
} s_state = {
   .initialized = false,
   .local_context_size = LLM_CONTEXT_DEFAULT_LOCAL,
   .local_context_queried = false,
   .local_context_authoritative = false,
   .local_context_querying = false,
   .local_context_queried_at = 0,
   .local_context_generation = 0,
   .last_prompt_tokens = 0,
   .last_context_size = 0,
};

/* Per-session token tracking */
typedef struct {
   uint32_t session_id;
   int last_prompt_tokens;
   int last_completion_tokens;
   int total_prompt_tokens;
   int total_completion_tokens;
} session_token_tracking_t;

#define MAX_TRACKED_SESSIONS 16
static session_token_tracking_t s_session_tokens[MAX_TRACKED_SESSIONS];
static int s_session_token_count = 0;

/* =============================================================================
 * CURL Response Buffer - Uses shared curl_buffer.h
 * ============================================================================= */

#define LLM_CONTEXT_MAX_RESPONSE_SIZE (64 * 1024) /* 64KB max response */

/* =============================================================================
 * Lifecycle Functions
 * ============================================================================= */

int llm_context_init(void) {
   if (s_state.initialized) {
      return 0;
   }

   if (pthread_mutex_init(&s_state.mutex, NULL) != 0) {
      OLOG_ERROR("llm_context: Failed to initialize mutex");
      return 1;
   }

   s_state.local_context_size = LLM_CONTEXT_DEFAULT_LOCAL;
   s_state.local_context_queried = false;
   s_state.local_context_querying = false;
   s_state.local_context_queried_at = 0;
   s_state.local_context_generation = 0;
   s_session_token_count = 0;
   memset(s_session_tokens, 0, sizeof(s_session_tokens));

   s_state.initialized = true;
   OLOG_INFO("llm_context: Initialized (default local context: %d)", s_state.local_context_size);

   return 0;
}

void llm_context_cleanup(void) {
   if (!s_state.initialized) {
      return;
   }

   pthread_mutex_destroy(&s_state.mutex);
   s_state.initialized = false;
   OLOG_INFO("llm_context: Cleaned up");
}

void llm_compaction_result_free(llm_compaction_result_t *result) {
   if (!result) {
      return;
   }
   if (result->summary) {
      free(result->summary);
   }
   memset(result, 0, sizeof(*result));
}

/* =============================================================================
 * Context Size Functions
 * ============================================================================= */

/**
 * @brief Look up context size from model table
 */
static int lookup_model_context(const model_context_entry_t *table, const char *model) {
   if (!model || !table) {
      return 0;
   }

   for (int i = 0; table[i].model_prefix != NULL; i++) {
      if (strncasecmp(model, table[i].model_prefix, strlen(table[i].model_prefix)) == 0) {
         return table[i].context_size;
      }
   }

   return 0; /* Not found */
}

int llm_context_query_local(const char *endpoint) {
   if (!endpoint || endpoint[0] == '\0') {
      return LLM_CONTEXT_DEFAULT_LOCAL;
   }

   /* Build /props URL */
   char url[512];
   snprintf(url, sizeof(url), "%s/props", endpoint);

   /* Remove any trailing /v1/chat/completions if present */
   char *v1_pos = strstr(url, "/v1/");
   if (v1_pos) {
      strcpy(v1_pos, "/props");
   }

   CURL *curl = curl_easy_init();
   if (!curl) {
      OLOG_WARNING("llm_context: Failed to init CURL for /props query");
      return LLM_CONTEXT_DEFAULT_LOCAL;
   }

   curl_buffer_t response;
   curl_buffer_init_with_max(&response, LLM_CONTEXT_MAX_RESPONSE_SIZE);

   curl_easy_setopt(curl, CURLOPT_URL, url);
   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_buffer_write_callback);
   curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
   curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
   curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);

   CURLcode res = curl_easy_perform(curl);
   curl_easy_cleanup(curl);

   if (res != CURLE_OK) {
      OLOG_WARNING("llm_context: Failed to query %s: %s", url, curl_easy_strerror(res));
      curl_buffer_free(&response);
      return LLM_CONTEXT_DEFAULT_LOCAL;
   }

   if (!response.data) {
      curl_buffer_free(&response);
      return LLM_CONTEXT_DEFAULT_LOCAL;
   }

   /* Parse JSON response */
   struct json_object *root = json_tokener_parse(response.data);
   curl_buffer_free(&response);

   if (!root) {
      OLOG_WARNING("llm_context: Failed to parse /props response");
      return LLM_CONTEXT_DEFAULT_LOCAL;
   }

   int context_size = LLM_CONTEXT_DEFAULT_LOCAL;

   /* Try to get n_ctx from default_generation_settings */
   struct json_object *settings = NULL;
   struct json_object *n_ctx_obj = NULL;

   if (json_object_object_get_ex(root, "default_generation_settings", &settings)) {
      if (json_object_object_get_ex(settings, "n_ctx", &n_ctx_obj)) {
         context_size = json_object_get_int(n_ctx_obj);
      }
   }

   /* Also try top-level n_ctx (some versions) */
   if (context_size == LLM_CONTEXT_DEFAULT_LOCAL) {
      if (json_object_object_get_ex(root, "n_ctx", &n_ctx_obj)) {
         context_size = json_object_get_int(n_ctx_obj);
      }
   }

   json_object_put(root);

   OLOG_INFO("llm_context: Local LLM context size: %d tokens", context_size);
   return context_size;
}

void llm_context_refresh_local(void) {
   pthread_mutex_lock(&s_state.mutex);
   s_state.local_context_queried = false;
   s_state.local_context_authoritative = false;
   s_state.local_context_queried_at = 0;
   s_state.local_context_generation++;
   pthread_mutex_unlock(&s_state.mutex);
   OLOG_INFO("llm_context: Local context cache invalidated, will re-query on next use");
}

int llm_context_get_size(llm_type_t type, cloud_provider_t provider, const char *model) {
   if (type == LLM_LOCAL) {
      pthread_mutex_lock(&s_state.mutex);

      /* Check if we need to query: first time or TTL expired */
      time_t now = time(NULL);
      bool ttl_expired = s_state.local_context_queried &&
                         (now - s_state.local_context_queried_at) >= LLM_CONTEXT_LOCAL_TTL;
      bool need_query = !s_state.local_context_queried || ttl_expired;

      /* Single-flight guard: if another thread is already querying, skip */
      if (need_query && !s_state.local_context_querying) {
         s_state.local_context_querying = true;
         uint32_t gen_before = s_state.local_context_generation;
         int old_size = s_state.local_context_size;

         const char *endpoint = g_config.llm.local.endpoint[0] != '\0' ? g_config.llm.local.endpoint
                                                                       : "http://127.0.0.1:8080";
         const char *local_model = g_config.llm.local.model;
         if ((!local_model || local_model[0] == '\0') && model && model[0] != '\0') {
            local_model = model;
         }

         /* Release mutex during HTTP call to avoid blocking other threads */
         pthread_mutex_unlock(&s_state.mutex);
         int new_size = llm_local_query_context_size(endpoint, local_model);
         pthread_mutex_lock(&s_state.mutex);

         s_state.local_context_querying = false;

         /* If state was invalidated while we were doing HTTP, discard result —
          * the next caller will re-query with the updated config */
         if (s_state.local_context_generation != gen_before) {
            OLOG_INFO("llm_context: Discarding stale TTL refresh (generation changed)");
         } else if (new_size == LLM_CONTEXT_DEFAULT_LOCAL &&
                    old_size != LLM_CONTEXT_DEFAULT_LOCAL && ttl_expired) {
            /* Server-down tolerance: keep old value during brief restart */
            OLOG_WARNING("llm_context: Local server unreachable during TTL refresh, "
                         "keeping cached context size %d",
                         old_size);
            s_state.local_context_queried_at = now;
         } else {
            if (ttl_expired && new_size != old_size) {
               OLOG_WARNING("llm_context: Local context size changed: %d -> %d tokens", old_size,
                            new_size);
            }
            s_state.local_context_size = new_size;
            s_state.local_context_queried = true;
            s_state.local_context_queried_at = now;
            s_state.local_context_authoritative = (llm_local_get_provider() !=
                                                   LOCAL_PROVIDER_OLLAMA);
         }
      }

      int size = s_state.local_context_size;
      pthread_mutex_unlock(&s_state.mutex);
      return size;
   }

   /* Cloud LLM - use lookup table */
   int size = 0;

   if (provider == CLOUD_PROVIDER_OPENAI) {
      size = lookup_model_context(s_openai_models, model);
      if (size == 0) {
         size = LLM_CONTEXT_DEFAULT_OPENAI;
      }
   } else if (provider == CLOUD_PROVIDER_CLAUDE) {
      size = lookup_model_context(s_claude_models, model);
      if (size == 0) {
         size = LLM_CONTEXT_DEFAULT_CLAUDE;
      }
   } else if (provider == CLOUD_PROVIDER_GEMINI) {
      size = lookup_model_context(s_gemini_models, model);
      if (size == 0) {
         size = LLM_CONTEXT_DEFAULT_GEMINI;
      }
   } else {
      size = LLM_CONTEXT_DEFAULT_OPENAI; /* Fallback for unknown providers */
   }

   return size;
}

/* =============================================================================
 * Token Tracking Functions
 * ============================================================================= */

/**
 * @brief Find or create token tracking entry for session
 */
static session_token_tracking_t *get_session_tracking(uint32_t session_id, bool create) {
   for (int i = 0; i < s_session_token_count; i++) {
      if (s_session_tokens[i].session_id == session_id) {
         return &s_session_tokens[i];
      }
   }

   if (!create || s_session_token_count >= MAX_TRACKED_SESSIONS) {
      return NULL;
   }

   /* Create new entry */
   session_token_tracking_t *entry = &s_session_tokens[s_session_token_count++];
   memset(entry, 0, sizeof(*entry));
   entry->session_id = session_id;
   return entry;
}

void llm_context_update_usage(uint32_t session_id,
                              int prompt_tokens,
                              int completion_tokens,
                              int cached_tokens) {
   pthread_mutex_lock(&s_state.mutex);

   session_token_tracking_t *tracking = get_session_tracking(session_id, true);
   if (tracking) {
      tracking->last_prompt_tokens = prompt_tokens;
      tracking->last_completion_tokens = completion_tokens;
      tracking->total_prompt_tokens += prompt_tokens;
      tracking->total_completion_tokens += completion_tokens;
   }

   /* Check if we need to re-query Ollama context. Set authoritative=true under
    * mutex BEFORE releasing, so only one thread attempts the HTTP refresh (M1). */
   bool need_ollama_refresh = !s_state.local_context_authoritative &&
                              llm_local_get_provider() == LOCAL_PROVIDER_OLLAMA;
   if (need_ollama_refresh) {
      s_state.local_context_authoritative = true; /* Claim refresh slot */
   }

   pthread_mutex_unlock(&s_state.mutex);

   /* For Ollama: re-query context size once after first LLM response.
    * The model is now guaranteed loaded, so /api/ps will return the runtime
    * context_length from Ollama's settings (not the model's theoretical max).
    * Done outside mutex to avoid blocking other threads during HTTP call. */
   if (need_ollama_refresh) {
      const char *endpoint = g_config.llm.local.endpoint[0] != '\0' ? g_config.llm.local.endpoint
                                                                    : "http://127.0.0.1:8080";
      /* Use session model if global config model is empty (user picked model via WebUI) */
      const char *local_model = g_config.llm.local.model;
      if (!local_model || local_model[0] == '\0') {
         session_t *session = session_get_command_context();
         if (session) {
            session_llm_config_t scfg = { 0 };
            session_get_llm_config(session, &scfg);
            if (scfg.model[0] != '\0') {
               local_model = scfg.model;
            }
         }
      }
      int refreshed = llm_local_query_context_size(endpoint, local_model);
      if (refreshed != LLM_CONTEXT_DEFAULT_LOCAL) {
         pthread_mutex_lock(&s_state.mutex);
         s_state.local_context_size = refreshed;
         s_state.local_context_queried_at = time(NULL);
         pthread_mutex_unlock(&s_state.mutex);
         OLOG_INFO("llm_context: Refreshed Ollama runtime context: %d tokens", refreshed);
      } else {
         /* HTTP call failed — release the claim so a future call retries */
         pthread_mutex_lock(&s_state.mutex);
         s_state.local_context_authoritative = false;
         pthread_mutex_unlock(&s_state.mutex);
         OLOG_WARNING("llm_context: Ollama context refresh failed, will retry");
      }
   }

   /* Log context usage with threshold check */
   llm_type_t type = llm_get_type();
   cloud_provider_t provider = llm_get_cloud_provider();
   int context_size = llm_context_get_size(type, provider, llm_get_model_name());
   float usage_pct = (context_size > 0) ? (float)prompt_tokens / (float)context_size * 100.0f : 0;
   float threshold = g_config.llm.summarize_threshold;
   float threshold_pct = threshold * 100.0f;

   /* Store last values for WebUI retrieval (under mutex for M2 consistency) */
   pthread_mutex_lock(&s_state.mutex);
   s_state.last_prompt_tokens = prompt_tokens;
   s_state.last_context_size = context_size;
   pthread_mutex_unlock(&s_state.mutex);

   OLOG_INFO("Context: %d/%d tokens (%.1f%%), threshold: %.0f%%", prompt_tokens, context_size,
             usage_pct, threshold_pct);

   if (usage_pct >= threshold_pct) {
      OLOG_WARNING("Context usage (%.1f%%) exceeds threshold (%.0f%%) - compaction recommended",
                   usage_pct, threshold_pct);
   }

#ifdef ENABLE_MULTI_CLIENT
   /* Record query metrics to session (persisted to database per-query) */
   session_t *session = session_get_command_context();
   if (session) {
      /* Build provider name string */
      const char *provider_name;
      if (type == LLM_LOCAL) {
         provider_name = "local";
      } else if (provider == CLOUD_PROVIDER_CLAUDE) {
         provider_name = "claude";
      } else {
         provider_name = "openai";
      }

      /* Get timing from session streaming metrics */
      double ttft_ms = 0.0;
      double total_ms = 0.0;
      if (session->stream_start_ms > 0) {
         uint64_t now_ms = get_time_ms();
         total_ms = (double)(now_ms - session->stream_start_ms);
         if (session->first_token_ms > 0) {
            ttft_ms = (double)(session->first_token_ms - session->stream_start_ms);
         }
      }

      session_record_query(session, provider_name, (uint64_t)prompt_tokens,
                           (uint64_t)completion_tokens, (uint64_t)cached_tokens, ttft_ms, total_ms,
                           false /* is_error */);
   }
#endif
}

void llm_context_get_last_usage(int *current_tokens, int *max_tokens, float *threshold) {
   pthread_mutex_lock(&s_state.mutex);
   if (current_tokens) {
      *current_tokens = s_state.last_prompt_tokens;
   }
   if (max_tokens) {
      *max_tokens = s_state.last_context_size;
   }
   pthread_mutex_unlock(&s_state.mutex);
   if (threshold) {
      *threshold = g_config.llm.summarize_threshold;
   }
}

int llm_context_estimate_tokens(struct json_object *history) {
   if (!history || !json_object_is_type(history, json_type_array)) {
      return 0;
   }

   int total_chars = 0;
   int len = json_object_array_length(history);

   for (int i = 0; i < len; i++) {
      struct json_object *msg = json_object_array_get_idx(history, i);
      struct json_object *content_obj = NULL;

      if (json_object_object_get_ex(msg, "content", &content_obj)) {
         if (json_object_is_type(content_obj, json_type_string)) {
            const char *content = json_object_get_string(content_obj);
            if (content) {
               total_chars += strlen(content);
            }
         } else if (json_object_is_type(content_obj, json_type_array)) {
            /* Handle content arrays (vision messages) */
            int content_len = json_object_array_length(content_obj);
            for (int j = 0; j < content_len; j++) {
               struct json_object *part = json_object_array_get_idx(content_obj, j);
               struct json_object *text_obj = NULL;
               if (json_object_object_get_ex(part, "text", &text_obj)) {
                  const char *text = json_object_get_string(text_obj);
                  if (text) {
                     total_chars += strlen(text);
                  }
               }
               /* Images are roughly 85 tokens for low detail, 765+ for high */
               struct json_object *type_obj = NULL;
               if (json_object_object_get_ex(part, "type", &type_obj)) {
                  const char *type = json_object_get_string(type_obj);
                  if (type && (strcmp(type, "image_url") == 0 || strcmp(type, "image") == 0)) {
                     total_chars += 3000; /* Rough estimate for image */
                  }
               }
            }
         }
      }

      /* Add overhead for role, formatting */
      total_chars += 20;
   }

   /* Rough estimate: ~4 characters per token */
   return total_chars / 4;
}

int llm_context_get_usage(uint32_t session_id,
                          llm_type_t type,
                          cloud_provider_t provider,
                          const char *model,
                          llm_context_usage_t *usage) {
   if (!usage) {
      return 1;
   }

   memset(usage, 0, sizeof(*usage));

   /* Get context size for current provider */
   usage->max_tokens = llm_context_get_size(type, provider, model);

   /* Get last known prompt tokens for this session */
   pthread_mutex_lock(&s_state.mutex);
   session_token_tracking_t *tracking = get_session_tracking(session_id, false);
   if (tracking) {
      usage->current_tokens = tracking->last_prompt_tokens;
   }
   pthread_mutex_unlock(&s_state.mutex);

   /* Calculate percentage */
   if (usage->max_tokens > 0) {
      usage->usage_percent = (float)usage->current_tokens / (float)usage->max_tokens;
   }

   /* Check against threshold */
   float threshold = g_config.llm.summarize_threshold;
   if (threshold <= 0 || threshold > 1.0) {
      threshold = 0.80; /* Default 80% */
   }
   usage->needs_compaction = (usage->usage_percent >= threshold);

   return 0;
}

/* =============================================================================
 * Compaction Functions
 * ============================================================================= */

bool llm_context_needs_compaction_for_switch(uint32_t session_id,
                                             struct json_object *history,
                                             llm_type_t target_type,
                                             cloud_provider_t target_provider,
                                             const char *target_model) {
   int target_context = llm_context_get_size(target_type, target_provider, target_model);
   int estimated_tokens = llm_context_estimate_tokens(history);

   float threshold = g_config.llm.summarize_threshold;
   if (threshold <= 0 || threshold > 1.0) {
      threshold = 0.80;
   }

   int threshold_tokens = (int)(target_context * threshold);
   bool needs_compaction = (estimated_tokens > threshold_tokens);

   if (needs_compaction) {
      OLOG_INFO(
          "llm_context: Compaction needed for switch - estimated %d tokens, target context %d "
          "(threshold %d)",
          estimated_tokens, target_context, threshold_tokens);
   }

   return needs_compaction;
}

bool llm_context_needs_compaction(uint32_t session_id,
                                  struct json_object *history,
                                  llm_type_t type,
                                  cloud_provider_t provider,
                                  const char *model) {
   llm_context_usage_t usage;
   if (llm_context_get_usage(session_id, type, provider, model, &usage) != 0) {
      return false;
   }

   int tracked_tokens = usage.current_tokens;

   /*
    * Always estimate from current history if available. The tracked token count
    * (last_prompt_tokens) is from the PREVIOUS call and doesn't account for new
    * messages added since. Use the maximum of tracked vs estimated to catch cases
    * where new messages have pushed us over the threshold.
    */
   if (history) {
      int estimated = llm_context_estimate_tokens(history);
      if (estimated > usage.current_tokens) {
         usage.current_tokens = estimated;
         usage.usage_percent = (float)usage.current_tokens / (float)usage.max_tokens;

         float threshold = g_config.llm.summarize_threshold;
         if (threshold <= 0 || threshold > 1.0) {
            threshold = 0.80;
         }
         usage.needs_compaction = (usage.usage_percent >= threshold);
      }
   }

   OLOG_INFO("llm_context: Compaction check for session %u: tracked=%d, current=%d, max=%d, "
             "usage=%.1f%%, needs_compaction=%s",
             session_id, tracked_tokens, usage.current_tokens, usage.max_tokens,
             usage.usage_percent * 100.0f, usage.needs_compaction ? "YES" : "NO");

   return usage.needs_compaction;
}

int llm_context_save_conversation(uint32_t session_id,
                                  struct json_object *history,
                                  const char *suffix,
                                  char *filename_out,
                                  size_t filename_len) {
   /* Check if logging is enabled */
   if (!g_config.llm.conversation_logging) {
      OLOG_INFO("llm_context: Conversation logging disabled, skipping save");
      return 1;
   }

   if (!history) {
      return -1;
   }

   /* Generate timestamped filename */
   time_t now = time(NULL);
   struct tm tm_storage;
   struct tm *tm_info = localtime_r(&now, &tm_storage);
   char timestamp[32];
   strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", tm_info);

   char filename[256];
   snprintf(filename, sizeof(filename), "logs/chat_history_session%u_%s_%s.json", session_id,
            suffix, timestamp);

   /* Create logs directory if needed */
   if (!path_ensure_parent_dir(filename)) {
      OLOG_WARNING("llm_context: Could not create logs directory");
   }

   /* Strip provider-private fields (encrypted reasoning blobs etc.) before
    * writing to disk — they're session-bound and must not be persisted. */
   struct json_object *sanitized = llm_history_strip_provider_state(history);
   if (!sanitized) {
      OLOG_ERROR("llm_context: Failed to strip provider state — skipping save to avoid "
                 "persisting session-bound fields");
      return -1;
   }

   /* Write JSON to file */
   const char *json_str = json_object_to_json_string_ext(
       sanitized, JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_NOSLASHESCAPE);

   FILE *fp = fopen(filename, "w");
   if (!fp) {
      OLOG_ERROR("llm_context: Failed to open %s for writing", filename);
      json_object_put(sanitized);
      return -1;
   }

   fprintf(fp, "%s\n", json_str);
   fclose(fp);
   json_object_put(sanitized);

   OLOG_INFO("llm_context: Conversation saved to %s", filename);

   if (filename_out && filename_len > 0) {
      safe_strncpy(filename_out, filename, filename_len);
   }

   return 0;
}

/**
 * @brief Check if a message is part of a tool call/result exchange
 *
 * Detects tool-related messages in both OpenAI and Claude history formats:
 * - OpenAI: role "tool", or assistant with "tool_calls" field
 * - Claude: assistant content with "tool_use" blocks, user content with "tool_result" blocks
 */
static bool is_tool_message(struct json_object *msg) {
   struct json_object *role_obj = NULL;
   if (!json_object_object_get_ex(msg, "role", &role_obj)) {
      return false;
   }
   const char *role = json_object_get_string(role_obj);

   /* OpenAI: tool result message */
   if (strcmp(role, "tool") == 0) {
      return true;
   }

   /* Assistant with tool_calls (OpenAI) or tool_use content (Claude) */
   if (strcmp(role, "assistant") == 0) {
      if (json_object_object_get_ex(msg, "tool_calls", NULL)) {
         return true;
      }
      struct json_object *content = NULL;
      if (json_object_object_get_ex(msg, "content", &content) &&
          json_object_is_type(content, json_type_array)) {
         int len = json_object_array_length(content);
         for (int i = 0; i < len; i++) {
            struct json_object *block = json_object_array_get_idx(content, i);
            struct json_object *type_val = NULL;
            if (json_object_object_get_ex(block, "type", &type_val) &&
                strcmp(json_object_get_string(type_val), "tool_use") == 0) {
               return true;
            }
         }
      }
      return false;
   }

   /* User with tool_result content (Claude format) */
   if (strcmp(role, "user") == 0) {
      struct json_object *content = NULL;
      if (json_object_object_get_ex(msg, "content", &content) &&
          json_object_is_type(content, json_type_array)) {
         int len = json_object_array_length(content);
         for (int i = 0; i < len; i++) {
            struct json_object *block = json_object_array_get_idx(content, i);
            struct json_object *type_val = NULL;
            if (json_object_object_get_ex(block, "type", &type_val) &&
                strcmp(json_object_get_string(type_val), "tool_result") == 0) {
               return true;
            }
         }
      }
   }

   return false;
}

int llm_context_compact(uint32_t session_id,
                        struct json_object *history,
                        llm_type_t type,
                        cloud_provider_t provider,
                        const char *model,
                        llm_compaction_result_t *result) {
   if (!history || !result) {
      return 1;
   }

   memset(result, 0, sizeof(*result));

   int history_len = json_object_array_length(history);
   if (history_len < 4) {
      /* Too few messages to compact */
      OLOG_INFO("llm_context: History too short to compact (%d messages)", history_len);
      return 0;
   }

   result->tokens_before = llm_context_estimate_tokens(history);

   /* Save conversation before compacting */
   llm_context_save_conversation(session_id, history, "precompact", result->log_filename,
                                 sizeof(result->log_filename));

   /* Extract system prompt (usually first message) */
   struct json_object *system_msg = NULL;
   int start_idx = 0;

   struct json_object *first_msg = json_object_array_get_idx(history, 0);
   struct json_object *role_obj = NULL;
   if (json_object_object_get_ex(first_msg, "role", &role_obj)) {
      if (strcmp(json_object_get_string(role_obj), "system") == 0) {
         system_msg = json_object_get(first_msg); /* Increment ref */
         start_idx = 1;
      }
   }

   /* Keep last N exchanges (user + assistant pairs) */
   int keep_messages = LLM_CONTEXT_KEEP_EXCHANGES * 2;
   int end_idx = history_len - keep_messages;

   /* Expand keep window backward to preserve complete tool call sequences.
    * The LLM needs to see: [user question] + [assistant(tool_calls)] + [tool results]
    * to continue coherently. Walk backward past all tool messages, then include
    * the preceding user message that triggered the tool exchange. */
   while (end_idx > start_idx && is_tool_message(json_object_array_get_idx(history, end_idx - 1))) {
      end_idx--;
   }
   /* Include the user message that triggered the tool call sequence */
   if (end_idx > start_idx) {
      struct json_object *prev = json_object_array_get_idx(history, end_idx - 1);
      struct json_object *prev_role = NULL;
      if (json_object_object_get_ex(prev, "role", &prev_role) &&
          strcmp(json_object_get_string(prev_role), "user") == 0) {
         end_idx--;
      }
   }

   if (end_idx <= start_idx) {
      /* Tool-call expansion consumed the entire summarizable window.
       * Fallback: strip tool result content from the conversation history.
       * The assistant responses that followed already incorporate the information,
       * so raw tool output (often large JSON) is dead weight. This is a mechanical
       * in-place transformation — no LLM summarization call needed. */
      OLOG_WARNING("llm_context: Tool-heavy conversation — stripping tool result content "
                   "(fallback compaction)");

      int stripped = 0;
      for (int i = start_idx; i < history_len; i++) {
         struct json_object *msg = json_object_array_get_idx(history, i);
         struct json_object *role_obj = NULL;
         if (!json_object_object_get_ex(msg, "role", &role_obj))
            continue;
         const char *role = json_object_get_string(role_obj);

         /* OpenAI format: role="tool" — replace content with empty string */
         if (strcmp(role, "tool") == 0) {
            json_object_object_del(msg, "content");
            json_object_object_add(msg, "content", json_object_new_string(""));
            stripped++;
            continue;
         }

         /* Claude format: role="user" with tool_result content blocks —
          * empty the content field within each tool_result block */
         if (strcmp(role, "user") == 0) {
            struct json_object *content = NULL;
            if (json_object_object_get_ex(msg, "content", &content) &&
                json_object_is_type(content, json_type_array)) {
               int clen = json_object_array_length(content);
               for (int j = 0; j < clen; j++) {
                  struct json_object *block = json_object_array_get_idx(content, j);
                  struct json_object *type_val = NULL;
                  if (json_object_object_get_ex(block, "type", &type_val) &&
                      strcmp(json_object_get_string(type_val), "tool_result") == 0) {
                     json_object_object_del(block, "content");
                     json_object_object_add(block, "content", json_object_new_string(""));
                     stripped++;
                  }
               }
            }
         }
         /* Assistant messages with tool_calls/tool_use are left intact —
          * call metadata is small and needed for conversation coherence */
      }

      result->tokens_after = llm_context_estimate_tokens(history);
      int saved = result->tokens_before - result->tokens_after;
      result->messages_summarized = stripped;
      result->performed = (stripped > 0);

      if (system_msg) {
         json_object_put(system_msg);
      }

      if (stripped > 0) {
         OLOG_INFO("llm_context: Stripped %d tool result(s), saved %d tokens (%d -> %d)", stripped,
                   saved, result->tokens_before, result->tokens_after);
      } else {
         OLOG_INFO("llm_context: No tool results to strip");
      }
      return 0;
   }

   result->messages_summarized = end_idx - start_idx;

   /* Build content to summarize */
   struct json_object *to_summarize = json_object_new_array();
   for (int i = start_idx; i < end_idx; i++) {
      struct json_object *msg = json_object_array_get_idx(history, i);
      json_object_array_add(to_summarize, json_object_get(msg));
   }

   /* Generate summary prompt */
   const char *summary_content = json_object_to_json_string(to_summarize);
   char summary_prompt[8192];
   snprintf(summary_prompt, sizeof(summary_prompt),
            "Summarize this conversation in 100 words or less, preserving key facts, decisions, "
            "and user preferences needed to continue naturally. Be extremely brief:\n\n%s",
            summary_content);
   json_object_put(to_summarize);

   /* Call LLM to generate summary */
   OLOG_INFO("llm_context: Requesting summary of %d messages from %s", result->messages_summarized,
             type == LLM_LOCAL ? "local LLM" : "cloud LLM");

   /* Create minimal conversation for summary request */
   struct json_object *summary_request = json_object_new_array();
   struct json_object *user_msg = json_object_new_object();
   json_object_object_add(user_msg, "role", json_object_new_string("user"));
   json_object_object_add(user_msg, "content", json_object_new_string(summary_prompt));
   json_object_array_add(summary_request, user_msg);

   /* Suppress tools for summarization - we just want text, not tool calls.
    * Without this, the LLM might respond with tool_calls instead of content,
    * causing a "content field is empty" error. */
   llm_tools_suppress_push();

   /* Set thread-local timeout for summarization (avoids racing on
    * g_config.network.llm_timeout_ms from concurrent threads) */
   llm_set_timeout_override(g_config.network.summarization_timeout_ms);

   /* Make LLM call for summary (with fallback enabled) */
   char *summary = llm_chat_completion(summary_request, NULL, NULL, NULL, 0, true);

   llm_set_timeout_override(0);
   llm_tools_suppress_pop();

   json_object_put(summary_request);

   if (!summary) {
      OLOG_ERROR("llm_context: Failed to generate summary");
#ifdef ENABLE_WEBUI
      /* Notify WebUI session about compaction failure */
      session_t *session = session_get(session_id);
      if (session && session->type == SESSION_TYPE_WEBUI) {
         webui_send_error(session, "COMPACTION_FAILED",
                          "Context compaction failed. Response may be truncated.");
      }
#endif
      if (system_msg) {
         json_object_put(system_msg);
      }
      return 1;
   }

   /* Rebuild history: system + summary + last N messages */
   struct json_object *new_history = json_object_new_array();

   /* Add system prompt if present */
   if (system_msg) {
      json_object_array_add(new_history, system_msg); /* Transfers ownership */
   }

   /* Add summary as assistant message */
   struct json_object *summary_msg = json_object_new_object();
   json_object_object_add(summary_msg, "role", json_object_new_string("assistant"));

   char summary_with_note[8192];
   snprintf(summary_with_note, sizeof(summary_with_note), "[Previous conversation summary: %s]",
            summary);
   json_object_object_add(summary_msg, "content", json_object_new_string(summary_with_note));
   json_object_array_add(new_history, summary_msg);

   /* Transfer summary ownership to result (caller must free) */
   result->summary = summary;

   /* Add last N messages */
   for (int i = end_idx; i < history_len; i++) {
      struct json_object *msg = json_object_array_get_idx(history, i);
      json_object_array_add(new_history, json_object_get(msg));
   }

   /* Replace history contents */
   /* Clear old history */
   while (json_object_array_length(history) > 0) {
      json_object_array_del_idx(history, 0, 1);
   }

   /* Copy new history into original array */
   int new_len = json_object_array_length(new_history);
   for (int i = 0; i < new_len; i++) {
      struct json_object *msg = json_object_array_get_idx(new_history, i);
      json_object_array_add(history, json_object_get(msg));
   }
   json_object_put(new_history);

   result->tokens_after = llm_context_estimate_tokens(history);
   result->performed = true;

   OLOG_INFO("llm_context: Compaction complete - %d messages summarized, %d -> %d tokens",
             result->messages_summarized, result->tokens_before, result->tokens_after);

   return 0;
}

int llm_context_compact_for_switch(uint32_t session_id,
                                   struct json_object *history,
                                   llm_type_t current_type,
                                   cloud_provider_t current_provider,
                                   const char *current_model,
                                   llm_type_t target_type,
                                   cloud_provider_t target_provider,
                                   const char *target_model,
                                   llm_compaction_result_t *result) {
   if (!result) {
      return 1;
   }

   memset(result, 0, sizeof(*result));

   /* Check if compaction needed for target */
   if (!llm_context_needs_compaction_for_switch(session_id, history, target_type, target_provider,
                                                target_model)) {
      OLOG_INFO("llm_context: No compaction needed for switch");
      return 0;
   }

   /* Perform compaction using CURRENT provider (has larger context) */
   OLOG_INFO("llm_context: Performing pre-switch compaction using current provider");
   return llm_context_compact(session_id, history, current_type, current_provider, current_model,
                              result);
}

/* =============================================================================
 * Auto-Compaction Function
 * ============================================================================= */

int llm_context_auto_compact(struct json_object *history, uint32_t session_id) {
   if (!history || !s_state.initialized) {
      return 0;
   }

   /* Get current LLM config */
   llm_type_t type = llm_get_type();
   cloud_provider_t provider = llm_get_cloud_provider();
   const char *model = llm_get_model_name();

   /* Check if compaction is needed */
   if (!llm_context_needs_compaction(session_id, history, type, provider, model)) {
      return 0;
   }

   OLOG_WARNING("llm_context: Auto-compacting conversation before LLM call");

   /* Notify local user via TTS before compaction (can take a few seconds) */
   if (session_id == 0) {
      text_to_speech((char *)"Compacting my memory. Just a moment.");
   }

   /* Perform compaction */
   llm_compaction_result_t result = { 0 };
   int rc = llm_context_compact(session_id, history, type, provider, model, &result);

   if (rc == 0 && result.performed) {
      OLOG_INFO("llm_context: Auto-compaction complete - %d tokens -> %d tokens",
                result.tokens_before, result.tokens_after);
      llm_compaction_result_free(&result);
      return 1; /* Compaction was performed */
   }

   llm_compaction_result_free(&result);
   return 0;
}

int llm_context_auto_compact_with_config(struct json_object *history,
                                         uint32_t session_id,
                                         llm_type_t type,
                                         cloud_provider_t provider,
                                         const char *model) {
   if (!history || !s_state.initialized) {
      return 0;
   }

   /* Check if compaction is needed using provided config */
   if (!llm_context_needs_compaction(session_id, history, type, provider, model)) {
      return 0;
   }

   OLOG_WARNING("llm_context: Auto-compacting conversation before LLM call (session %u)",
                session_id);

   /* Notify user before compaction (can take a few seconds) */
   if (session_id == 0) {
      text_to_speech((char *)"Compacting my memory. Just a moment.");
   }
#ifdef ENABLE_WEBUI
   else {
      /* Notify WebUI session */
      session_t *session = session_get(session_id);
      if (session && session->type == SESSION_TYPE_WEBUI) {
         webui_send_state_with_detail(session, "thinking", "Compacting context...");
      }
   }
#endif

   /* Perform compaction */
   llm_compaction_result_t result = { 0 };
   int rc = llm_context_compact(session_id, history, type, provider, model, &result);

   if (rc == 0 && result.performed) {
      OLOG_INFO("llm_context: Auto-compaction complete - %d tokens -> %d tokens",
                result.tokens_before, result.tokens_after);

#ifdef ENABLE_WEBUI
      /* Notify WebUI about compaction completion (for database continuation) */
      if (session_id != 0) {
         session_t *session = session_get(session_id);
         if (session && session->type == SESSION_TYPE_WEBUI) {
            webui_send_compaction_complete(session, result.tokens_before, result.tokens_after,
                                           result.messages_summarized, result.summary);
         }
      }
#endif

      llm_compaction_result_free(&result);
      return 1; /* Compaction was performed */
   }

   llm_compaction_result_free(&result);
   return 0;
}

/* =============================================================================
 * Utility Functions
 * ============================================================================= */

char *llm_context_usage_string(const llm_context_usage_t *usage, char *buf, size_t buf_len) {
   if (!usage || !buf || buf_len == 0) {
      return buf;
   }

   snprintf(buf, buf_len, "%d/%d (%.0f%%)", usage->current_tokens, usage->max_tokens,
            usage->usage_percent * 100.0f);

   return buf;
}
