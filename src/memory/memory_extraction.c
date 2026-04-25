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
 * Memory Extraction Implementation
 *
 * Extracts facts, preferences, and summaries from conversation history.
 */

#define _GNU_SOURCE /* strptime */

#include "memory/memory_extraction.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db.h"
#include "config/dawn_config.h"
#include "core/buf_printf.h"
#include "core/session_manager.h"
#include "llm/llm_interface.h"
#include "logging.h"
#include "memory/memory_db.h"
#include "memory/memory_embeddings.h"
#include "memory/memory_filter.h"
#include "memory/memory_types.h"

/* =============================================================================
 * Extraction Prompt Template
 * ============================================================================= */

static const char *EXTRACTION_PROMPT_TEMPLATE =
    "Analyze this conversation and extract user information in JSON format.\n\n"
    "CONVERSATION:\n%s\n\n"
    "EXISTING USER PROFILE:\n%s\n\n"
    "Extract the following and respond ONLY with valid JSON:\n"
    "{\n"
    "  \"facts\": [\n"
    "    {\"text\": \"factual statement about user\", "
    "\"category\": \"personal|professional|relationships|health|interests|practical|"
    "preferences|general\", "
    "\"source\": \"explicit|inferred\", \"confidence\": 0.0-1.0}\n"
    "  ],\n"
    "  \"preferences\": [\n"
    "    {\"category\": \"verbosity\", \"value\": \"prefers concise responses\", "
    "\"confidence\": 0.0-1.0}\n"
    "  ],\n"
    "  \"corrections\": [\n"
    "    {\"old_fact\": \"outdated statement\", \"new_fact\": \"corrected statement\"}\n"
    "  ],\n"
    "  \"entities\": [\n"
    "    {\"name\": \"entity name\", \"type\": \"person|pet|place|org|thing\", "
    "\"attributes\": {\"key\": \"value\"}}\n"
    "  ],\n"
    "  \"relations\": [\n"
    "    {\"subject\": \"entity name\", \"relation\": \"has_pet|lives_in|works_at|likes|is_a\", "
    "\"object\": \"entity name or value\", "
    "\"valid_from\": \"YYYY-MM-DD or YYYY (optional)\", "
    "\"valid_to\": \"YYYY-MM-DD or YYYY (optional)\"}\n"
    "  ],\n"
    "  \"summary\": \"1-2 sentence conversation summary\",\n"
    "  \"title\": \"short conversation title, max 40 chars\",\n"
    "  \"topics\": [\"topic1\", \"topic2\"]\n"
    "}\n\n"
    "Guidelines:\n"
    "- \"explicit\" source: user directly stated it\n"
    "- \"inferred\" source: reasonably deduced from context\n"
    "- Fact category: pick the SINGLE dominant category. Only use \"general\" if the fact "
    "is truly cross-cutting and fits no other category.\n"
    "  * personal: biographical (name, age, where born, where grew up)\n"
    "  * professional: job, employer, education, skills\n"
    "  * relationships: family, friends, contacts (the user's connections to other people)\n"
    "  * health: medical, fitness, dietary, allergies\n"
    "  * interests: hobbies, media tastes, travel, sports\n"
    "  * practical: home, vehicles, schedules, routines, addresses, accounts\n"
    "  * preferences: communication style, UI tastes, formats (overlaps preferences[]; "
    "use this category for free-text preference facts)\n"
    "- Use short, simple categories for preferences (e.g., \"verbosity\", \"humor\", "
    "\"formality\", \"detail_level\", \"units\", \"theme\")\n"
    "- Only include facts that are specific to this user, not general knowledge\n"
    "- High confidence (0.8-1.0) for explicit statements, lower for inferences\n"
    "- List corrections if new information contradicts existing profile\n"
    "- Extract named entities (people, pets, places, organizations) mentioned by the user\n"
    "- Extract relationships between entities (e.g., user owns pet, person works at org)\n"
    "- For relations with time bounds (e.g., \"worked at Google 2018-2022\"), include "
    "valid_from and/or valid_to. Year-only is OK — emit YYYY-01-01 (e.g., \"2018-01-01\"). "
    "Omit fields when no time information is given.\n"
    "- IMPORTANT: Reuse entity names from EXISTING USER PROFILE exactly as listed. "
    "Do NOT create alternate names for the same entity (e.g., use \"Kris\" not \"Kris Kersey\" "
    "if \"Kris\" is already known)\n"
    "- Generate a concise title (under 40 characters) that captures the main topic(s)\n"
    "- Title should be human-friendly, not a sentence — more like a label\n"
    "- If nothing notable to extract, return empty arrays\n";

/* =============================================================================
 * Thread Context
 * ============================================================================= */

typedef struct {
   int user_id;
   int64_t conversation_id; /* For incremental extraction tracking */
   char session_id[MEMORY_SESSION_ID_MAX];
   char *conversation_json; /* Serialized conversation history */
   int message_count;       /* Total messages in conversation */
   int new_message_count;   /* Messages being extracted this time */
   int duration_seconds;
   bool has_fallback;                     /* Whether fallback LLM info is available */
   memory_extraction_fallback_t fallback; /* Session's active LLM for retry */
} extraction_context_t;

/* =============================================================================
 * Extraction Concurrency Tracking
 *
 * Tracks in-progress extractions using a bounded array instead of a bitmap.
 * This removes the 64-user limit and enables system-wide concurrency limiting
 * tied to max_clients configuration.
 * ============================================================================= */

#define MAX_EXTRACTION_SLOTS 16 /* Compile-time max; runtime limit is min(this, max_clients) */

static struct {
   int user_ids[MAX_EXTRACTION_SLOTS]; /* User IDs with active extractions (0 = empty slot) */
   int count;                          /* Current number of active extractions */
} s_extraction_state = { { 0 }, 0 };
static pthread_mutex_t s_extraction_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Helper: Get runtime concurrency limit */
static int get_max_concurrent_extractions(void) {
   int limit = g_config.webui.max_clients;
   if (limit <= 0 || limit > MAX_EXTRACTION_SLOTS) {
      limit = MAX_EXTRACTION_SLOTS;
   }
   return limit;
}

/* Helper: Check if user has active extraction (must hold mutex) */
static bool extraction_is_active_locked(int user_id) {
   for (int i = 0; i < MAX_EXTRACTION_SLOTS; i++) {
      if (s_extraction_state.user_ids[i] == user_id) {
         return true;
      }
   }
   return false;
}

/* Helper: Try to acquire extraction slot for user (must hold mutex)
 * Returns true if slot acquired, false if user already active or slots full */
static bool extraction_slot_acquire_locked(int user_id) {
   /* Check if user already has active extraction */
   if (extraction_is_active_locked(user_id)) {
      return false;
   }

   /* Check if we've hit the concurrency limit */
   int max_concurrent = get_max_concurrent_extractions();
   if (s_extraction_state.count >= max_concurrent) {
      return false;
   }

   /* Find empty slot and claim it */
   for (int i = 0; i < MAX_EXTRACTION_SLOTS; i++) {
      if (s_extraction_state.user_ids[i] == 0) {
         s_extraction_state.user_ids[i] = user_id;
         s_extraction_state.count++;
         return true;
      }
   }

   return false; /* Should not reach here if count is accurate */
}

/* Helper: Release extraction slot for user (must hold mutex) */
static void extraction_slot_release_locked(int user_id) {
   for (int i = 0; i < MAX_EXTRACTION_SLOTS; i++) {
      if (s_extraction_state.user_ids[i] == user_id) {
         s_extraction_state.user_ids[i] = 0;
         s_extraction_state.count--;
         return;
      }
   }
}

/* =============================================================================
 * Helper: Build existing profile string
 * ============================================================================= */

static char *build_existing_profile(int user_id) {
   char *profile = malloc(4096);
   if (!profile)
      return strdup("(none)");

   size_t off = 0;
   size_t rem = 4096;

   /* Load existing preferences */
   memory_preference_t prefs[10];
   int pref_count = memory_db_pref_list(user_id, prefs, 10, 0);

   if (pref_count > 0) {
      BUF_PRINTF(profile, off, rem, "Preferences:\n");
      for (int i = 0; i < pref_count && rem > 1; i++) {
         BUF_PRINTF(profile, off, rem, "- %s: %s\n", prefs[i].category, prefs[i].value);
      }
   }

   /* Load existing facts */
   memory_fact_t facts[10];
   int fact_count = memory_db_fact_list(user_id, facts, 10, 0);

   if (fact_count > 0) {
      if (off > 0)
         BUF_PRINTF(profile, off, rem, "\n");
      BUF_PRINTF(profile, off, rem, "Known facts:\n");
      for (int i = 0; i < fact_count && rem > 1; i++) {
         BUF_PRINTF(profile, off, rem, "- %s\n", facts[i].fact_text);
      }
   }

   /* Load existing entities so LLM reuses canonical names */
   memory_entity_t entities[20];
   int entity_count = memory_db_entity_list(user_id, entities, 20, 0);

   if (entity_count > 0) {
      if (off > 0)
         BUF_PRINTF(profile, off, rem, "\n");
      BUF_PRINTF(profile, off, rem,
                 "Known entities (reuse these exact names, do NOT create variants):\n");
      for (int i = 0; i < entity_count && rem > 1; i++) {
         BUF_PRINTF(profile, off, rem, "- %s (%s)\n", entities[i].name, entities[i].entity_type);
      }
   }

   if (off == 0) {
      strcpy(profile, "(none)");
   }

   return profile;
}

/* =============================================================================
 * Helper: Extract JSON from LLM response
 *
 * LLMs often wrap JSON in markdown code blocks or include preamble text.
 * This function extracts the JSON object from such responses.
 * ============================================================================= */

static struct json_object *extract_json_from_response(const char *response) {
   if (!response || response[0] == '\0') {
      return NULL;
   }

   struct json_object *root = NULL;

   /* First try direct parse (pure JSON) */
   root = json_tokener_parse(response);
   if (root) {
      return root;
   }

   /* Look for JSON in markdown code block: ```json ... ``` or ``` ... ``` */
   const char *json_block_start = strstr(response, "```json");
   if (json_block_start) {
      json_block_start += 7; /* Skip "```json" */
   } else {
      json_block_start = strstr(response, "```");
      if (json_block_start) {
         json_block_start += 3; /* Skip "```" */
      }
   }

   if (json_block_start) {
      /* Skip any newline after opening ``` */
      while (*json_block_start == '\n' || *json_block_start == '\r') {
         json_block_start++;
      }

      const char *json_block_end = strstr(json_block_start, "```");
      if (json_block_end) {
         size_t len = json_block_end - json_block_start;
         char *json_str = malloc(len + 1);
         if (json_str) {
            memcpy(json_str, json_block_start, len);
            json_str[len] = '\0';
            root = json_tokener_parse(json_str);
            free(json_str);
            if (root) {
               return root;
            }
         }
      }
   }

   /* Last resort: find first '{' and try to parse from there */
   const char *brace = strchr(response, '{');
   if (brace) {
      root = json_tokener_parse(brace);
      if (root) {
         return root;
      }
   }

   return NULL;
}

/* =============================================================================
 * WebUI broadcast (provided by webui_server.c when ENABLE_WEBUI)
 * ============================================================================= */

#ifdef ENABLE_WEBUI
#include "webui/webui_server.h"
#endif

/* =============================================================================
 * Helper: UTF-8-safe truncation
 * ============================================================================= */

static void utf8_truncate(char *str, size_t max_bytes) {
   if (strlen(str) <= max_bytes)
      return;
   str[max_bytes] = '\0';
   /* Back up past any UTF-8 continuation bytes (10xxxxxx) */
   while (max_bytes > 0 && (str[max_bytes - 1] & 0xC0) == 0x80) {
      str[--max_bytes] = '\0';
   }
   /* Remove the leading byte of the incomplete sequence */
   if (max_bytes > 0 && (str[max_bytes - 1] & 0x80) != 0) {
      int expected_len = 0;
      unsigned char c = (unsigned char)str[max_bytes - 1];
      if ((c & 0xE0) == 0xC0)
         expected_len = 2;
      else if ((c & 0xF0) == 0xE0)
         expected_len = 3;
      else if ((c & 0xF8) == 0xF0)
         expected_len = 4;
      if (expected_len > 0 && strlen(str + max_bytes - 1) < (size_t)expected_len) {
         str[max_bytes - 1] = '\0';
      }
   }
}

/* =============================================================================
 * Helpers: Category validation + ISO-8601 date parsing
 * ============================================================================= */

/* Validate against canonical taxonomy from memory_db.c; fall back to "general"
 * on miss.  We log unknowns at INFO so taxonomy drift is visible without spam. */
static const char *validate_fact_category(const char *raw) {
   if (!raw || !*raw)
      return "general";
   for (int i = 0; i < MEMORY_FACT_CATEGORY_COUNT; i++) {
      if (strcmp(raw, MEMORY_FACT_CATEGORIES[i]) == 0) {
         return MEMORY_FACT_CATEGORIES[i];
      }
   }
   OLOG_INFO("memory_extraction: unknown category '%s', mapping to 'general'", raw);
   return "general";
}

/* Parse ISO-8601 date strings the LLM may emit.  Tries:
 *   YYYY-MM-DD  -> exact day at 00:00:00 UTC
 *   YYYY        -> Jan 1 of that year at 00:00:00 UTC
 * Returns 0 on parse failure (callers treat 0 as NULL/open-ended). */
static int64_t parse_iso8601_date(const char *s) {
   if (!s || !*s)
      return 0;
   struct tm tm = { 0 };
   /* Try full date first. */
   char *end = strptime(s, "%Y-%m-%d", &tm);
   if (!end) {
      /* Year-only fallback. */
      memset(&tm, 0, sizeof(tm));
      end = strptime(s, "%Y", &tm);
      if (!end)
         return 0;
      tm.tm_mday = 1;
      tm.tm_mon = 0;
   }
   /* Convert as UTC using timegm().  DAWN currently targets glibc; returning
    * 0 on failure is sufficient — callers treat 0 as "no validity bound". */
   time_t t = timegm(&tm);
   if (t == (time_t)-1)
      return 0;
   return (int64_t)t;
}

/* =============================================================================
 * Helper: Parse extraction response
 * ============================================================================= */

static void process_extraction_response(int user_id,
                                        int64_t conversation_id,
                                        const char *session_id,
                                        const char *response_text,
                                        int message_count,
                                        int duration_seconds) {
   if (!response_text) {
      OLOG_WARNING("memory_extraction: NULL response from LLM");
      return;
   }

   /* Extract JSON from response (handles markdown blocks, preamble text, etc.) */
   struct json_object *root = extract_json_from_response(response_text);
   if (!root) {
      OLOG_WARNING("memory_extraction: Failed to parse LLM response as JSON");
      OLOG_WARNING("memory_extraction: Response preview: %.200s...", response_text);
      return;
   }

   /* Process facts */
   struct json_object *facts_arr;
   if (json_object_object_get_ex(root, "facts", &facts_arr)) {
      int count = json_object_array_length(facts_arr);
      for (int i = 0; i < count; i++) {
         struct json_object *fact = json_object_array_get_idx(facts_arr, i);
         struct json_object *text_obj, *source_obj, *conf_obj;

         if (json_object_object_get_ex(fact, "text", &text_obj)) {
            const char *text = json_object_get_string(text_obj);
            const char *source = "inferred";
            const char *category = "general";
            float confidence = 0.7f;

            if (json_object_object_get_ex(fact, "source", &source_obj)) {
               source = json_object_get_string(source_obj);
            }
            if (json_object_object_get_ex(fact, "confidence", &conf_obj)) {
               confidence = (float)json_object_get_double(conf_obj);
            }
            struct json_object *cat_obj;
            if (json_object_object_get_ex(fact, "category", &cat_obj)) {
               category = validate_fact_category(json_object_get_string(cat_obj));
            }

            /* Check for injection patterns before storing */
            if (memory_filter_check(text)) {
               OLOG_WARNING("memory_extraction: blocked injection in fact: %.80s", text);
               continue;
            }

            /* Check for duplicates before storing */
            memory_fact_t similar[3];
            int similar_count = memory_db_fact_find_similar(user_id, text, similar, 3);

            if (similar_count == 0) {
               int64_t fact_id = memory_db_fact_create(user_id, text, confidence, source, category);
               OLOG_INFO("memory_extraction: stored fact [%s]: %s", category, text);
               /* Embed the new fact for semantic search */
               if (fact_id > 0 && memory_embeddings_available()) {
                  memory_embeddings_embed_and_store(user_id, fact_id, text);
               }
            } else {
               /* Update confidence of existing similar fact */
               float new_conf = similar[0].confidence + 0.1f;
               if (new_conf > 1.0f)
                  new_conf = 1.0f;
               memory_db_fact_update_confidence(similar[0].id, new_conf);
            }
         }
      }
   }

   /* Process preferences */
   struct json_object *prefs_arr;
   if (json_object_object_get_ex(root, "preferences", &prefs_arr)) {
      int count = json_object_array_length(prefs_arr);
      for (int i = 0; i < count; i++) {
         struct json_object *pref = json_object_array_get_idx(prefs_arr, i);
         struct json_object *cat_obj, *val_obj, *conf_obj;

         if (json_object_object_get_ex(pref, "category", &cat_obj) &&
             json_object_object_get_ex(pref, "value", &val_obj)) {
            const char *category = json_object_get_string(cat_obj);
            const char *value = json_object_get_string(val_obj);
            float confidence = 0.7f;

            if (json_object_object_get_ex(pref, "confidence", &conf_obj)) {
               confidence = (float)json_object_get_double(conf_obj);
            }

            if (memory_filter_check(value) || memory_filter_check(category)) {
               OLOG_WARNING("memory_extraction: blocked injection in preference: %.32s=%.80s",
                            category, value);
               continue;
            }

            memory_db_pref_upsert(user_id, category, value, confidence, "inferred");
            OLOG_INFO("memory_extraction: stored preference: %s=%s", category, value);
         }
      }
   }

   /* Process corrections */
   struct json_object *corr_arr;
   if (json_object_object_get_ex(root, "corrections", &corr_arr)) {
      int count = json_object_array_length(corr_arr);
      for (int i = 0; i < count; i++) {
         struct json_object *corr = json_object_array_get_idx(corr_arr, i);
         struct json_object *old_obj, *new_obj;

         if (json_object_object_get_ex(corr, "old_fact", &old_obj) &&
             json_object_object_get_ex(corr, "new_fact", &new_obj)) {
            const char *old_fact = json_object_get_string(old_obj);
            const char *new_fact = json_object_get_string(new_obj);

            if (memory_filter_check(new_fact)) {
               OLOG_WARNING("memory_extraction: blocked injection in correction: %.80s", new_fact);
               continue;
            }

            /* Find and supersede old fact */
            memory_fact_t similar[3];
            int similar_count = memory_db_fact_find_similar(user_id, old_fact, similar, 3);

            if (similar_count > 0) {
               /* Create new fact and supersede old one */
               int64_t new_id = memory_db_fact_create(user_id, new_fact, 0.9f, "explicit", NULL);
               if (new_id > 0) {
                  memory_db_fact_supersede(similar[0].id, new_id);
                  OLOG_INFO("memory_extraction: corrected fact: %s -> %s", old_fact, new_fact);
                  /* Embed the corrected fact */
                  if (memory_embeddings_available()) {
                     memory_embeddings_embed_and_store(user_id, new_id, new_fact);
                  }
               }
            }
         }
      }
   }

   /* Store extracted entities and build local name→ID map */
#define ENTITY_MAP_MAX 32
   struct {
      char canonical[MEMORY_ENTITY_NAME_MAX];
      int64_t id;
   } entity_map[ENTITY_MAP_MAX];
   int entity_map_count = 0;

   struct json_object *entities_arr;
   if (json_object_object_get_ex(root, "entities", &entities_arr)) {
      int count = json_object_array_length(entities_arr);
      for (int i = 0; i < count; i++) {
         struct json_object *entity = json_object_array_get_idx(entities_arr, i);
         struct json_object *name_obj, *type_obj;
         if (!json_object_object_get_ex(entity, "name", &name_obj) ||
             !json_object_object_get_ex(entity, "type", &type_obj)) {
            continue;
         }

         const char *ent_name = json_object_get_string(name_obj);
         const char *ent_type = json_object_get_string(type_obj);
         if (!ent_name || !ent_type)
            continue;

         /* Validate entity type against known allowlist */
         static const char *valid_types[] = { "person", "pet", "place", "org", "thing", NULL };
         bool type_valid = false;
         for (int t = 0; valid_types[t]; t++) {
            if (strcmp(ent_type, valid_types[t]) == 0) {
               type_valid = true;
               break;
            }
         }
         if (!type_valid)
            ent_type = "thing";

         if (memory_filter_check(ent_name)) {
            OLOG_WARNING("memory_extraction: blocked injection in entity name: %.64s", ent_name);
            continue;
         }

         char canonical[MEMORY_ENTITY_NAME_MAX];
         memory_make_canonical_name(ent_name, canonical, sizeof(canonical));

         bool was_created = false;
         int64_t eid = memory_db_entity_upsert(user_id, ent_name, ent_type, canonical,
                                               &was_created);
         if (eid < 0)
            continue;

         OLOG_INFO("memory_extraction: entity: %s (%s) id=%ld %s", ent_name, ent_type, (long)eid,
                   was_created ? "[new]" : "[updated]");

         /* Embed only newly created entities */
         if (was_created && memory_embeddings_available()) {
            memory_embeddings_embed_and_store_entity(eid, user_id, ent_name);
         }

         /* Add to local map */
         if (entity_map_count < ENTITY_MAP_MAX) {
            strncpy(entity_map[entity_map_count].canonical, canonical, MEMORY_ENTITY_NAME_MAX - 1);
            entity_map[entity_map_count].canonical[MEMORY_ENTITY_NAME_MAX - 1] = '\0';
            entity_map[entity_map_count].id = eid;
            entity_map_count++;
         } else {
            OLOG_WARNING("memory_extraction: entity map full (max %d), skipping '%s'",
                         ENTITY_MAP_MAX, ent_name);
         }
      }
   }

   /* Store extracted relations */
#define RELATION_MAX 32
   struct json_object *relations_arr;
   int relations_stored = 0;
   if (json_object_object_get_ex(root, "relations", &relations_arr)) {
      int count = json_object_array_length(relations_arr);
      for (int i = 0; i < count && relations_stored < RELATION_MAX; i++) {
         struct json_object *rel = json_object_array_get_idx(relations_arr, i);
         struct json_object *subj_obj, *rel_obj, *obj_obj;
         if (!json_object_object_get_ex(rel, "subject", &subj_obj) ||
             !json_object_object_get_ex(rel, "relation", &rel_obj) ||
             !json_object_object_get_ex(rel, "object", &obj_obj)) {
            continue;
         }

         const char *subj_name = json_object_get_string(subj_obj);
         const char *rel_type = json_object_get_string(rel_obj);
         const char *obj_name = json_object_get_string(obj_obj);
         if (!subj_name || !rel_type || !obj_name)
            continue;

         if (memory_filter_check(subj_name) || memory_filter_check(rel_type) ||
             memory_filter_check(obj_name)) {
            OLOG_WARNING("memory_extraction: blocked injection in relation: %.64s -> %.64s",
                         subj_name, obj_name);
            continue;
         }

         /* Resolve subject entity from local map, fallback to upsert */
         char subj_canonical[MEMORY_ENTITY_NAME_MAX];
         memory_make_canonical_name(subj_name, subj_canonical, sizeof(subj_canonical));
         int64_t subj_id = -1;
         for (int m = 0; m < entity_map_count; m++) {
            if (strcmp(entity_map[m].canonical, subj_canonical) == 0) {
               subj_id = entity_map[m].id;
               break;
            }
         }
         if (subj_id < 0) {
            bool created = false;
            subj_id = memory_db_entity_upsert(user_id, subj_name, "thing", subj_canonical,
                                              &created);
            if (subj_id < 0)
               continue;
         }

         /* Resolve object: check local map first, then DB search */
         char obj_canonical[MEMORY_ENTITY_NAME_MAX];
         memory_make_canonical_name(obj_name, obj_canonical, sizeof(obj_canonical));
         int64_t obj_entity_id = 0;
         for (int m = 0; m < entity_map_count; m++) {
            if (strcmp(entity_map[m].canonical, obj_canonical) == 0) {
               obj_entity_id = entity_map[m].id;
               break;
            }
         }
         if (obj_entity_id == 0) {
            /* Exact canonical name lookup (avoids LIKE substring false matches) */
            memory_entity_t found;
            if (memory_db_entity_get_by_name(user_id, obj_canonical, &found) == MEMORY_DB_SUCCESS) {
               obj_entity_id = found.id;
            }
         }

         /* Parse optional validity period.  Default 0 = NULL/open-ended. */
         int64_t valid_from = 0, valid_to = 0;
         struct json_object *vf_obj, *vt_obj;
         if (json_object_object_get_ex(rel, "valid_from", &vf_obj)) {
            valid_from = parse_iso8601_date(json_object_get_string(vf_obj));
         }
         if (json_object_object_get_ex(rel, "valid_to", &vt_obj)) {
            valid_to = parse_iso8601_date(json_object_get_string(vt_obj));
         }
         /* Sanity-check the range the LLM emitted.  An inverted or zero-length
          * window would never match as_of queries and interacts badly with the
          * supersede close logic — drop both bounds and store as open-ended. */
         if (valid_from != 0 && valid_to != 0 && valid_to <= valid_from) {
            OLOG_WARNING("memory_extraction: dropping invalid validity range for "
                         "(%s, %s, %s): [%ld..%ld]",
                         subj_name, rel_type, obj_name, (long)valid_from, (long)valid_to);
            valid_from = 0;
            valid_to = 0;
         }

         /* Use supersede so exclusive relations (works_at, lives_in, ...) auto-close
          * any prior open instance with a different object.  Non-exclusive relations
          * skip the close branch internally.  Both writes happen in one transaction. */
         memory_db_relation_supersede(user_id, subj_id, rel_type, obj_entity_id,
                                      (obj_entity_id == 0) ? obj_name : NULL, 0, 0.8f, valid_from,
                                      valid_to);
         relations_stored++;
         if (valid_from || valid_to) {
            OLOG_INFO("memory_extraction: relation: (%s, %s, %s) valid [%ld..%ld]", subj_name,
                      rel_type, obj_name, (long)valid_from, (long)valid_to);
         } else {
            OLOG_INFO("memory_extraction: relation: (%s, %s, %s)", subj_name, rel_type, obj_name);
         }
      }
   }

   /* Invalidate entity embedding cache once after all extractions */
   if (entity_map_count > 0) {
      memory_embeddings_invalidate_entity_cache();
   }

   /* Process summary */
   struct json_object *summary_obj, *topics_arr;
   if (json_object_object_get_ex(root, "summary", &summary_obj)) {
      const char *summary = json_object_get_string(summary_obj);

      /* Build topics string */
      char topics[MEMORY_TOPICS_MAX] = { 0 };
      if (json_object_object_get_ex(root, "topics", &topics_arr)) {
         int count = json_object_array_length(topics_arr);
         size_t toff = 0;
         size_t trem = MEMORY_TOPICS_MAX;
         for (int i = 0; i < count && trem > 1; i++) {
            const char *topic = json_object_get_string(json_object_array_get_idx(topics_arr, i));
            if (topic && !memory_filter_check(topic)) {
               if (toff > 0) {
                  BUF_PRINTF(topics, toff, trem, ", ");
               }
               BUF_PRINTF(topics, toff, trem, "%s", topic);
            }
         }
      }

      if (memory_filter_check(summary)) {
         OLOG_WARNING("memory_extraction: blocked injection in summary: %.80s", summary);
      } else {
         memory_db_summary_create(user_id, session_id, summary, topics, "neutral", message_count,
                                  duration_seconds);
         OLOG_INFO("memory_extraction: stored summary for session %s", session_id);
      }
   }

   /* Process title — auto-rename conversation if not locked (atomic check-and-set) */
   struct json_object *title_obj;
   if (json_object_object_get_ex(root, "title", &title_obj)) {
      const char *title = json_object_get_string(title_obj);
      if (title && title[0] != '\0' && conversation_id > 0) {
         char safe_title[48];
         strncpy(safe_title, title, sizeof(safe_title) - 1);
         safe_title[sizeof(safe_title) - 1] = '\0';
         utf8_truncate(safe_title, 40);

         int rc = conv_db_auto_title(conversation_id, user_id, safe_title);
         if (rc == AUTH_DB_SUCCESS) {
#ifdef ENABLE_WEBUI
            webui_broadcast_conversation_renamed(user_id, conversation_id, safe_title);
#endif
            OLOG_INFO("memory_extraction: auto-titled conversation %lld: %s",
                      (long long)conversation_id, safe_title);
         }
      }
   }

   json_object_put(root);
}

/* =============================================================================
 * Fallback Builder
 * ============================================================================= */

void memory_extraction_build_fallback(session_t *session, memory_extraction_fallback_t *fb) {
   memset(fb, 0, sizeof(*fb));
   if (!session)
      return;
   session_llm_config_t cfg;
   session_get_llm_config(session, &cfg);
   fb->type = cfg.type;
   fb->cloud_provider = cfg.cloud_provider;
   strncpy(fb->endpoint, cfg.endpoint, sizeof(fb->endpoint) - 1);
   strncpy(fb->model, cfg.model, sizeof(fb->model) - 1);
}

/* =============================================================================
 * Extraction Thread
 * ============================================================================= */

static void *extraction_thread(void *arg) {
   extraction_context_t *ctx = (extraction_context_t *)arg;

   OLOG_INFO("memory_extraction: starting for user %d, session %s", ctx->user_id, ctx->session_id);

   /* Build existing profile */
   char *existing_profile = build_existing_profile(ctx->user_id);

   /* Build extraction prompt */
   size_t prompt_size = strlen(EXTRACTION_PROMPT_TEMPLATE) + strlen(ctx->conversation_json) +
                        strlen(existing_profile) + 100;
   char *prompt = malloc(prompt_size);
   if (!prompt) {
      OLOG_ERROR("memory_extraction: failed to allocate prompt");
      goto cleanup;
   }

   snprintf(prompt, prompt_size, EXTRACTION_PROMPT_TEMPLATE, ctx->conversation_json,
            existing_profile);

   /* Call LLM for extraction using configured provider/model */
   char *response = NULL;

   /* Build a minimal conversation history with just the extraction prompt */
   struct json_object *extraction_history = json_object_new_array();
   struct json_object *user_msg = json_object_new_object();
   json_object_object_add(user_msg, "role", json_object_new_string("user"));
   json_object_object_add(user_msg, "content", json_object_new_string(prompt));
   json_object_array_add(extraction_history, user_msg);

   /* Build resolved config from memory extraction settings */
   llm_resolved_config_t extraction_config = { 0 };
   char model_buf[LLM_MODEL_NAME_MAX];
   char endpoint_buf[128];

   const char *provider = g_config.memory.extraction_provider;
   const char *model = g_config.memory.extraction_model;

   /* Copy model to local buffer */
   if (model && model[0] != '\0') {
      strncpy(model_buf, model, sizeof(model_buf) - 1);
      model_buf[sizeof(model_buf) - 1] = '\0';
      extraction_config.model = model_buf;
   }

   /* Determine provider type and configure accordingly */
   if (strcmp(provider, "local") == 0 || strcmp(provider, "ollama") == 0) {
      extraction_config.type = LLM_LOCAL;
      extraction_config.cloud_provider = CLOUD_PROVIDER_NONE;
      /* Use local endpoint from config */
      strncpy(endpoint_buf, g_config.llm.local.endpoint, sizeof(endpoint_buf) - 1);
      endpoint_buf[sizeof(endpoint_buf) - 1] = '\0';
      extraction_config.endpoint = endpoint_buf;
   } else if (strcmp(provider, "openai") == 0) {
      extraction_config.type = LLM_CLOUD;
      extraction_config.cloud_provider = CLOUD_PROVIDER_OPENAI;
      extraction_config.api_key = g_secrets.openai_api_key;
      extraction_config.endpoint = NULL; /* Use default */
   } else if (strcmp(provider, "claude") == 0) {
      extraction_config.type = LLM_CLOUD;
      extraction_config.cloud_provider = CLOUD_PROVIDER_CLAUDE;
      extraction_config.api_key = g_secrets.claude_api_key;
      extraction_config.endpoint = NULL; /* Use default */
   } else {
      /* Default to local if unknown - warn with valid options */
      OLOG_WARNING("memory_extraction: unknown provider '%s' in config "
                   "(valid: local, ollama, openai, claude) - falling back to local",
                   provider);
      extraction_config.type = LLM_LOCAL;
      extraction_config.cloud_provider = CLOUD_PROVIDER_NONE;
      strncpy(endpoint_buf, g_config.llm.local.endpoint, sizeof(endpoint_buf) - 1);
      endpoint_buf[sizeof(endpoint_buf) - 1] = '\0';
      extraction_config.endpoint = endpoint_buf;
   }

   /* Disable tools and thinking for extraction - we just want JSON output */
   strncpy(extraction_config.tool_mode, "disabled", sizeof(extraction_config.tool_mode) - 1);
   strncpy(extraction_config.thinking_mode, "disabled",
           sizeof(extraction_config.thinking_mode) - 1);

   OLOG_INFO("memory_extraction: using provider=%s, model=%s", provider,
             model ? model : "(default)");

   /* Set per-request timeout for extraction (thread-local, no global mutation) */
   extraction_config.timeout_ms = g_config.memory.extraction_timeout_ms;

   /* Use the configured LLM for extraction */
   response = llm_chat_completion_with_config(extraction_history, prompt, NULL, NULL, 0,
                                              &extraction_config);

   /* If primary model failed and we have a fallback, retry with the session's active model */
   bool used_fallback = false;
   if (!response && ctx->has_fallback && ctx->fallback.model[0] != '\0') {
      /* Skip retry if fallback is the same model, provider, and endpoint */
      bool same_config = (extraction_config.model && ctx->fallback.model[0] != '\0' &&
                          strcmp(extraction_config.model, ctx->fallback.model) == 0 &&
                          extraction_config.type == ctx->fallback.type &&
                          extraction_config.endpoint && ctx->fallback.endpoint[0] != '\0' &&
                          strcmp(extraction_config.endpoint, ctx->fallback.endpoint) == 0);
      if (!same_config) {
         OLOG_WARNING("memory_extraction: primary model failed, retrying with session model %s",
                      ctx->fallback.model);

         llm_resolved_config_t fallback_config = { 0 };
         fallback_config.type = ctx->fallback.type;
         fallback_config.cloud_provider = ctx->fallback.cloud_provider;
         fallback_config.endpoint = ctx->fallback.endpoint;
         fallback_config.model = ctx->fallback.model;
         strncpy(fallback_config.tool_mode, "disabled", sizeof(fallback_config.tool_mode) - 1);
         strncpy(fallback_config.thinking_mode, "disabled",
                 sizeof(fallback_config.thinking_mode) - 1);

         fallback_config.timeout_ms = g_config.memory.extraction_timeout_ms;

         /* Resolve API key for cloud providers */
         if (fallback_config.type == LLM_CLOUD) {
            if (fallback_config.cloud_provider == CLOUD_PROVIDER_OPENAI)
               fallback_config.api_key = g_secrets.openai_api_key;
            else if (fallback_config.cloud_provider == CLOUD_PROVIDER_CLAUDE)
               fallback_config.api_key = g_secrets.claude_api_key;
         }

         response = llm_chat_completion_with_config(extraction_history, prompt, NULL, NULL, 0,
                                                    &fallback_config);
         if (response) {
            used_fallback = true;
         }
      }
   }

   json_object_put(extraction_history);

   if (response) {
      process_extraction_response(ctx->user_id, ctx->conversation_id, ctx->session_id, response,
                                  ctx->new_message_count, ctx->duration_seconds);
      free(response);

      /* Notify user if we had to fall back */
      if (used_fallback) {
         char notice[256];
         snprintf(notice, sizeof(notice),
                  "Memory extraction used fallback model \"%s\" "
                  "(configured model \"%s\" unavailable)",
                  ctx->fallback.model, model ? model : "(default)");
         OLOG_WARNING("memory_extraction: %s", notice);
#ifdef ENABLE_WEBUI
         webui_broadcast_memory_notice(ctx->user_id, "warning", notice);
#endif
      }

      /* Update extraction high-water mark on success */
      if (ctx->conversation_id > 0) {
         memory_db_set_last_extracted(ctx->conversation_id, ctx->message_count);
         OLOG_INFO("memory_extraction: updated high-water mark to %d for conversation %ld",
                   ctx->message_count, (long)ctx->conversation_id);
      }

      /* Run fact pruning if enabled */
      if (g_config.memory.pruning_enabled) {
         int pruned_superseded = memory_db_fact_prune_superseded(
             ctx->user_id, g_config.memory.prune_superseded_days);
         int pruned_stale = memory_db_fact_prune_stale(ctx->user_id,
                                                       g_config.memory.prune_stale_days,
                                                       g_config.memory.prune_stale_min_confidence);
         if (pruned_superseded > 0 || pruned_stale > 0) {
            OLOG_INFO("memory_extraction: pruned %d superseded, %d stale facts for user %d",
                      pruned_superseded > 0 ? pruned_superseded : 0,
                      pruned_stale > 0 ? pruned_stale : 0, ctx->user_id);
         }
      }
   } else {
      OLOG_WARNING("memory_extraction: LLM returned no response");
      char notice[256];
      snprintf(notice, sizeof(notice),
               "Memory extraction failed — model \"%s\" unavailable. "
               "Check extraction_model in Settings.",
               model ? model : "(default)");
#ifdef ENABLE_WEBUI
      webui_broadcast_memory_notice(ctx->user_id, "error", notice);
#endif
   }

   free(prompt);

cleanup:
   free(existing_profile);
   free(ctx->conversation_json);

   /* Clear in-progress flag - save user_id before freeing ctx */
   int saved_user_id = ctx->user_id;
   free(ctx);

   pthread_mutex_lock(&s_extraction_mutex);
   extraction_slot_release_locked(saved_user_id);
   pthread_mutex_unlock(&s_extraction_mutex);

   OLOG_INFO("memory_extraction: completed for user %d", saved_user_id);
   return NULL;
}

/* =============================================================================
 * Public API
 * ============================================================================= */

int memory_trigger_extraction(int user_id,
                              int64_t conversation_id,
                              const char *session_id_str,
                              struct json_object *conversation_history,
                              int message_count,
                              int duration_seconds,
                              const memory_extraction_fallback_t *fallback) {
   if (user_id <= 0 || !conversation_history) {
      return 1;
   }

   if (!g_config.memory.enabled) {
      return 1;
   }

#ifdef ENABLE_AUTH
   /* Re-check privacy status from database (prevents race condition with set_private) */
   if (conversation_id > 0) {
      int is_private = conv_db_is_private(conversation_id, user_id);
      if (is_private > 0) {
         OLOG_INFO("memory_extraction: skipping - conversation %lld is private (DB check)",
                   (long long)conversation_id);
         return 0;
      }
      /* is_private == -1 means error or not found, proceed with extraction */
   }
#endif

   /* Skip if too few messages */
   if (message_count < 2) {
      OLOG_INFO("memory_extraction: skipping - too few messages (%d)", message_count);
      return 0;
   }

   /* Check incremental extraction: how many messages were already processed? */
   int last_extracted = 0;
   if (conversation_id > 0) {
      last_extracted = memory_db_get_last_extracted(conversation_id);
      if (last_extracted < 0) {
         last_extracted = 0; /* Treat error as never extracted */
      }

      /* Skip if no new messages since last extraction */
      if (message_count <= last_extracted) {
         OLOG_INFO("memory_extraction: skipping - no new messages (count=%d, last=%d)",
                   message_count, last_extracted);
         return 0;
      }
   }

   int new_messages = message_count - last_extracted;

   /* Check concurrency limits and acquire extraction slot */
   pthread_mutex_lock(&s_extraction_mutex);
   if (!extraction_slot_acquire_locked(user_id)) {
      /* Either user already has extraction in progress, or we've hit max concurrent */
      bool user_active = extraction_is_active_locked(user_id);
      int max_concurrent = get_max_concurrent_extractions();
      pthread_mutex_unlock(&s_extraction_mutex);

      if (user_active) {
         OLOG_INFO("memory_extraction: already in progress for user %d", user_id);
      } else {
         OLOG_INFO("memory_extraction: at capacity (%d/%d), skipping user %d",
                   s_extraction_state.count, max_concurrent, user_id);
      }
      return 0;
   }
   pthread_mutex_unlock(&s_extraction_mutex);

   /* Prepare context */
   extraction_context_t *ctx = calloc(1, sizeof(extraction_context_t));
   if (!ctx) {
      pthread_mutex_lock(&s_extraction_mutex);
      extraction_slot_release_locked(user_id);
      pthread_mutex_unlock(&s_extraction_mutex);
      return 1;
   }

   ctx->user_id = user_id;
   ctx->conversation_id = conversation_id;
   if (session_id_str) {
      strncpy(ctx->session_id, session_id_str, MEMORY_SESSION_ID_MAX - 1);
   } else {
      snprintf(ctx->session_id, MEMORY_SESSION_ID_MAX, "session_%ld", (long)time(NULL));
   }
   ctx->message_count = message_count;
   ctx->new_message_count = new_messages;
   ctx->duration_seconds = duration_seconds;
   if (fallback) {
      ctx->has_fallback = true;
      ctx->fallback = *fallback;
   }

   /* Serialize conversation history (skip system messages, only include new messages) */
   size_t arr_len = json_object_array_length(conversation_history);
   struct json_object *filtered = json_object_new_array();

   /* Calculate start index: skip already-extracted messages
    * Account for the fact that arr_len includes system messages which we skip */
   int skipped_system = 0;
   int messages_seen = 0;

   for (size_t i = 0; i < arr_len; i++) {
      struct json_object *msg = json_object_array_get_idx(conversation_history, i);
      struct json_object *role_obj;
      if (json_object_object_get_ex(msg, "role", &role_obj)) {
         const char *role = json_object_get_string(role_obj);
         /* Skip system messages */
         if (strcmp(role, "system") == 0) {
            skipped_system++;
            continue;
         }

         messages_seen++;

         /* Only include messages after the last extraction point */
         if (messages_seen > last_extracted) {
            json_object_array_add(filtered, json_object_get(msg));
         }
      }
   }

   ctx->conversation_json = strdup(
       json_object_to_json_string_ext(filtered, JSON_C_TO_STRING_PLAIN));
   json_object_put(filtered);

   if (!ctx->conversation_json) {
      free(ctx);
      pthread_mutex_lock(&s_extraction_mutex);
      extraction_slot_release_locked(user_id);
      pthread_mutex_unlock(&s_extraction_mutex);
      return 1;
   }

   OLOG_INFO("memory_extraction: extracting %d new messages (last=%d, total=%d)", new_messages,
             last_extracted, message_count);

   /* Spawn detached extraction thread */
   pthread_t thread;
   pthread_attr_t attr;
   pthread_attr_init(&attr);
   pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

   int rc = pthread_create(&thread, &attr, extraction_thread, ctx);
   pthread_attr_destroy(&attr);

   if (rc != 0) {
      OLOG_ERROR("memory_extraction: failed to create thread: %d", rc);
      free(ctx->conversation_json);
      free(ctx);
      pthread_mutex_lock(&s_extraction_mutex);
      extraction_slot_release_locked(user_id);
      pthread_mutex_unlock(&s_extraction_mutex);
      return 1;
   }

   OLOG_INFO("memory_extraction: triggered for user %d", user_id);
   return 0;
}

bool memory_extraction_in_progress(int user_id) {
   if (user_id <= 0) {
      return false;
   }

   pthread_mutex_lock(&s_extraction_mutex);
   bool in_progress = extraction_is_active_locked(user_id);
   pthread_mutex_unlock(&s_extraction_mutex);

   return in_progress;
}
