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
 * LLM-based fact recategorization — classifies facts still labeled "general"
 * by sending batches to the extraction LLM for per-fact category assignment.
 */

#define _GNU_SOURCE /* pthread_timedjoin_np */

#include "memory/memory_recategorize.h"

#include <errno.h>
#include <json-c/json.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config/dawn_config.h"
#include "dawn_error.h"
#include "llm/llm_interface.h"
#include "logging.h"
#include "memory/memory_db.h"
#include "memory/memory_extraction.h"
#include "memory/memory_filter.h"
#include "memory/memory_types.h"

#define RECAT_BATCH_SIZE 25
#define RECAT_MAX_LOOPS 500
#define RECAT_THROTTLE_US 100000 /* 100ms between batches */

static atomic_bool s_recat_running = false;
static atomic_bool s_recat_shutdown = false;
static pthread_t s_recat_thread;

static const char *RECAT_PROMPT_TEMPLATE =
    "Classify each fact into exactly ONE category. Respond ONLY with a JSON array.\n\n"
    "Categories:\n"
    "- personal: biographical (name, age, birthplace, hometown, background)\n"
    "- professional: job, employer, education, skills, career\n"
    "- relationships: family, friends, contacts, pets (connections to people/animals)\n"
    "- health: medical conditions, fitness, dietary, allergies, medications\n"
    "- interests: hobbies, media tastes, travel, sports, learning\n"
    "- practical: home, vehicles, schedules, routines, addresses, accounts, devices\n"
    "- preferences: communication style, UI tastes, formats, likes/dislikes\n"
    "- general: ONLY if the fact truly fits no other category\n\n"
    "Strongly prefer a specific category over \"general\".\n"
    "Return only ids from the input.\n\n"
    "Facts:\n%s\n\n"
    "Respond with ONLY a JSON array:\n"
    "[{\"id\": 42, \"category\": \"relationships\"}, ...]\n";

static int build_llm_config(llm_resolved_config_t *cfg,
                            char *model_buf,
                            size_t model_buf_sz,
                            char *endpoint_buf,
                            size_t endpoint_buf_sz) {
   memset(cfg, 0, sizeof(*cfg));

   const char *provider = g_config.memory.extraction_provider;
   const char *model = g_config.memory.extraction_model;

   if (!provider || provider[0] == '\0') {
      OLOG_ERROR("memory_recategorize: extraction_provider not configured");
      return FAILURE;
   }

   if (model && model[0] != '\0') {
      strncpy(model_buf, model, model_buf_sz - 1);
      model_buf[model_buf_sz - 1] = '\0';
      cfg->model = model_buf;
   }

   if (strcmp(provider, "local") == 0 || strcmp(provider, "ollama") == 0) {
      cfg->type = LLM_LOCAL;
      cfg->cloud_provider = CLOUD_PROVIDER_NONE;
      strncpy(endpoint_buf, g_config.llm.local.endpoint, endpoint_buf_sz - 1);
      endpoint_buf[endpoint_buf_sz - 1] = '\0';
      cfg->endpoint = endpoint_buf;
   } else if (strcmp(provider, "openai") == 0) {
      cfg->type = LLM_CLOUD;
      cfg->cloud_provider = CLOUD_PROVIDER_OPENAI;
      cfg->api_key = g_secrets.openai_api_key;
      cfg->endpoint = NULL;
   } else if (strcmp(provider, "claude") == 0) {
      cfg->type = LLM_CLOUD;
      cfg->cloud_provider = CLOUD_PROVIDER_CLAUDE;
      cfg->api_key = g_secrets.claude_api_key;
      cfg->endpoint = NULL;
   } else {
      OLOG_WARNING("memory_recategorize: unknown provider '%s', falling back to local", provider);
      cfg->type = LLM_LOCAL;
      cfg->cloud_provider = CLOUD_PROVIDER_NONE;
      strncpy(endpoint_buf, g_config.llm.local.endpoint, endpoint_buf_sz - 1);
      endpoint_buf[endpoint_buf_sz - 1] = '\0';
      cfg->endpoint = endpoint_buf;
   }

   strncpy(cfg->tool_mode, "disabled", sizeof(cfg->tool_mode) - 1);
   strncpy(cfg->thinking_mode, "disabled", sizeof(cfg->thinking_mode) - 1);
   cfg->timeout_ms = g_config.memory.extraction_timeout_ms;

   return SUCCESS;
}

static bool validate_category(const char *cat) {
   if (!cat || !*cat)
      return false;
   for (int i = 0; i < MEMORY_FACT_CATEGORY_COUNT; i++) {
      if (strcmp(cat, MEMORY_FACT_CATEGORIES[i]) == 0)
         return true;
   }
   return false;
}

static int process_batch(memory_fact_t *facts,
                         int count,
                         const llm_resolved_config_t *cfg,
                         int *out_updated) {
   *out_updated = 0;

   struct json_object *facts_arr = json_object_new_array();
   if (!facts_arr)
      return FAILURE;

   for (int i = 0; i < count; i++) {
      if (memory_filter_check(facts[i].fact_text)) {
         OLOG_WARNING("memory_recategorize: skipping fact %lld (injection filter)",
                      (long long)facts[i].id);
         continue;
      }
      struct json_object *item = json_object_new_object();
      json_object_object_add(item, "id", json_object_new_int64(facts[i].id));
      json_object_object_add(item, "text", json_object_new_string(facts[i].fact_text));
      json_object_array_add(facts_arr, item);
   }

   if (json_object_array_length(facts_arr) == 0) {
      json_object_put(facts_arr);
      return SUCCESS;
   }

   const char *facts_json = json_object_to_json_string_ext(facts_arr, JSON_C_TO_STRING_PLAIN);

   size_t prompt_sz = strlen(RECAT_PROMPT_TEMPLATE) + strlen(facts_json) + 64;
   char *prompt = malloc(prompt_sz);
   if (!prompt) {
      json_object_put(facts_arr);
      return FAILURE;
   }
   snprintf(prompt, prompt_sz, RECAT_PROMPT_TEMPLATE, facts_json);
   json_object_put(facts_arr);

   struct json_object *history = json_object_new_array();
   struct json_object *msg = json_object_new_object();
   json_object_object_add(msg, "role", json_object_new_string("user"));
   json_object_object_add(msg, "content", json_object_new_string(prompt));
   json_object_array_add(history, msg);
   free(prompt);

   char *response = llm_chat_completion_with_config(history, NULL, NULL, NULL, 0, cfg);
   json_object_put(history);

   if (!response) {
      OLOG_WARNING("memory_recategorize: LLM call failed for batch");
      return FAILURE;
   }

   struct json_object *root = memory_extraction_parse_json(response);
   free(response);
   if (!root) {
      OLOG_WARNING("memory_recategorize: failed to parse LLM response");
      return FAILURE;
   }

   if (!json_object_is_type(root, json_type_array)) {
      OLOG_WARNING("memory_recategorize: LLM response is not a JSON array");
      json_object_put(root);
      return FAILURE;
   }

   int updated = 0;
   int arr_len = json_object_array_length(root);
   for (int i = 0; i < arr_len; i++) {
      struct json_object *entry = json_object_array_get_idx(root, i);
      if (!entry)
         continue;

      struct json_object *id_obj = NULL, *cat_obj = NULL;
      json_object_object_get_ex(entry, "id", &id_obj);
      json_object_object_get_ex(entry, "category", &cat_obj);
      if (!id_obj || !cat_obj)
         continue;

      int64_t fact_id = json_object_get_int64(id_obj);
      const char *cat = json_object_get_string(cat_obj);

      if (strcmp(cat, "general") == 0)
         continue;
      if (!validate_category(cat)) {
         OLOG_WARNING("memory_recategorize: LLM returned invalid category '%s' for fact %lld", cat,
                      (long long)fact_id);
         continue;
      }

      /* Verify this id was actually in our batch */
      bool found = false;
      for (int j = 0; j < count; j++) {
         if (facts[j].id == fact_id) {
            found = true;
            break;
         }
      }
      if (!found) {
         OLOG_WARNING("memory_recategorize: LLM returned unknown fact_id %lld", (long long)fact_id);
         continue;
      }

      if (memory_db_fact_update_category(fact_id, cat) == MEMORY_DB_SUCCESS)
         updated++;
   }

   json_object_put(root);
   *out_updated = updated;
   return SUCCESS;
}

static void *recategorize_thread_fn(void *arg) {
   int user_id = *(int *)arg;
   free(arg);

   OLOG_INFO("memory_recategorize: started for user %d", user_id);

   llm_resolved_config_t cfg;
   char model_buf[LLM_MODEL_NAME_MAX];
   char endpoint_buf[128];
   if (build_llm_config(&cfg, model_buf, sizeof(model_buf), endpoint_buf, sizeof(endpoint_buf)) !=
       SUCCESS) {
      atomic_store(&s_recat_running, false);
      return NULL;
   }

   OLOG_INFO("memory_recategorize: using provider=%s, model=%s",
             g_config.memory.extraction_provider,
             g_config.memory.extraction_model[0] ? g_config.memory.extraction_model : "(default)");

   int total_touched = 0;
   int total_assigned = 0;
   int failed_batches = 0;
   int consecutive_fails = 0;
   int64_t cursor_id = 0;
   memory_fact_t facts[RECAT_BATCH_SIZE];

   for (int loop = 0; loop < RECAT_MAX_LOOPS && !atomic_load(&s_recat_shutdown); loop++) {
      int count = 0;
      memory_db_fact_list_general(user_id, cursor_id, facts, RECAT_BATCH_SIZE, &count);
      if (count <= 0)
         break;

      /* Advance cursor past this batch regardless of classification results */
      for (int i = 0; i < count; i++) {
         if (facts[i].id > cursor_id)
            cursor_id = facts[i].id;
      }

      total_touched += count;

      int batch_updated = 0;
      if (process_batch(facts, count, &cfg, &batch_updated) != SUCCESS) {
         failed_batches++;
         if (++consecutive_fails >= 3) {
            OLOG_ERROR("memory_recategorize: %d consecutive failures, aborting", consecutive_fails);
            break;
         }
      } else {
         total_assigned += batch_updated;
         consecutive_fails = 0;
      }

      if (count < RECAT_BATCH_SIZE)
         break;

      usleep(RECAT_THROTTLE_US);
   }

   int remaining = 0;
   memory_db_fact_count_general(user_id, &remaining);

   OLOG_INFO("memory_recategorize: complete user=%d touched=%d assigned=%d failed_batches=%d "
             "remaining_general=%d",
             user_id, total_touched, total_assigned, failed_batches, remaining);

   atomic_store(&s_recat_running, false);
   return NULL;
}

int memory_recategorize_start(int user_id) {
   bool expected = false;
   if (!atomic_compare_exchange_strong(&s_recat_running, &expected, true)) {
      OLOG_WARNING("memory_recategorize: already running");
      return FAILURE;
   }

   int *uid = malloc(sizeof(int));
   if (!uid) {
      atomic_store(&s_recat_running, false);
      return FAILURE;
   }
   *uid = user_id;

   atomic_store(&s_recat_shutdown, false);

   pthread_attr_t attr;
   pthread_attr_init(&attr);
   pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

   if (pthread_create(&s_recat_thread, &attr, recategorize_thread_fn, uid) != 0) {
      OLOG_ERROR("memory_recategorize: failed to create thread");
      free(uid);
      atomic_store(&s_recat_running, false);
      pthread_attr_destroy(&attr);
      return FAILURE;
   }

   pthread_attr_destroy(&attr);
   return SUCCESS;
}

bool memory_recategorize_is_running(void) {
   return atomic_load(&s_recat_running);
}

void memory_recategorize_stop(void) {
   if (!atomic_load(&s_recat_running))
      return;

   atomic_store(&s_recat_shutdown, true);

   struct timespec ts;
   clock_gettime(CLOCK_REALTIME, &ts);
   ts.tv_sec += 5;

   int ret = pthread_timedjoin_np(s_recat_thread, NULL, &ts);
   if (ret == ETIMEDOUT) {
      OLOG_WARNING("memory_recategorize: thread did not stop in 5s, cancelling");
      pthread_cancel(s_recat_thread);
      pthread_join(s_recat_thread, NULL);
   }
   atomic_store(&s_recat_running, false);
}
