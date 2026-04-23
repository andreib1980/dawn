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
 * Memory Tool - Persistent memory for facts and preferences
 *
 * Supports: remember (store), search (query), forget (delete), recent (list)
 * Implementation is in memory/memory_callback.c, this file handles tool registration.
 */

#include "tools/memory_tool.h"

#include "config/dawn_config.h"
#include "logging.h"
#include "memory/memory_types.h" /* MEMORY_FACT_CATEGORIES */
#include "tools/tool_registry.h"

/* Forward declaration of callback from memory/memory_callback.c */
char *memoryCallback(const char *actionName, char *value, int *should_respond);

/* Forward declaration for availability check */
static bool memory_tool_is_available(void);

/* ========== Tool Parameter Definition ========== */

static const treg_param_t memory_params[] = {
   {
       .name = "action",
       .description =
           "Memory action: 'remember' (store a fact), 'search' (find memories), "
           "'forget' (delete a memory by numeric ID — use search/recent first to find the "
           "ID), 'recent' (list recent memories), "
           "'save_contact' (store email/phone for a person), "
           "'find_contact' (look up contact info), "
           "'list_contacts' (list all contacts), "
           "'delete_contact' (remove contact by ID), "
           "'merge_entities' (combine two entities that refer to the same person/thing)",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = true,
       .maps_to = TOOL_MAPS_TO_ACTION,
       .enum_values = { "remember", "search", "forget", "recent", "save_contact", "find_contact",
                        "list_contacts", "delete_contact", "merge_entities" },
       .enum_count = 9,
   },
   {
       .name = "query",
       .description = "For 'remember': the fact to store (e.g., 'User prefers dark mode'). "
                      "For 'search': keywords to find relevant memories. "
                      "For 'forget': numeric fact ID to delete (from search/recent results). "
                      "For 'recent': optional limit number (default 10). "
                      "For 'save_contact': the person's name. "
                      "For 'find_contact': name to search for. "
                      "For 'list_contacts': optional field_type filter (email/phone). "
                      "For 'delete_contact': contact ID to remove. "
                      "For 'merge_entities': the source entity name (will be deleted).",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_VALUE,
   },
   {
       .name = "time_range",
       .description = "Optional time filter for 'search'. Limit results to memories from this "
                      "period. Examples: '24h', '7d', '2w', '30d'. Only used with 'search'.",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "time_range",
   },
   {
       .name = "target_name",
       .description = "For 'merge_entities': the name of the entity to merge INTO (keeps its name, "
                      "absorbs all relations and contacts from the source entity).",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "target_name",
   },
   /* v34 — fact-category pre-filter for retrieval.  Mirrors the canonical
    * MEMORY_FACT_CATEGORIES taxonomy in memory_db.c.  Inline duplication is
    * intentional: tool registry needs compile-time enum_values, and centralizing
    * it via a runtime pointer would force a registry-shape change. */
   {
       .name = "category",
       .description = "Optional category filter for 'search'/'recent'. Restricts results to "
                      "facts in one topic area (e.g., ask only about professional or health "
                      "memories). Improves retrieval precision on focused queries.",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "category",
       .enum_values = { "personal", "professional", "relationships", "health", "interests",
                        "practical", "preferences", "general" },
       .enum_count = 8,
   },
   /* v33 — relation validity at a historical timestamp. */
   {
       .name = "as_of",
       .description = "Optional ISO date (YYYY-MM-DD or YYYY) for 'search'. Returns relations "
                      "valid at that point in time (e.g., 'who did Alice work for in 2020'). "
                      "Defaults to now() when omitted.",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "as_of",
   },
   {
       .name = "include_historical",
       .description = "Set to true to include closed relations (those with a past valid_to) "
                      "in 'search' results. Default false returns only currently-valid "
                      "relations.",
       .type = TOOL_PARAM_TYPE_BOOL,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "include_historical",
   },
};

/* ========== Tool Metadata ========== */

static const tool_metadata_t memory_metadata = {
   .name = "memory",
   .device_string = "memory",
   .topic = "dawn",
   .aliases = { "remember", "recall" },
   .alias_count = 2,

   .description = "Store and retrieve persistent memories about the user. "
                  "Use 'remember' to store facts (preferences, information shared by user). "
                  "Use 'search' to find relevant stored memories (optionally filtered by "
                  "time_range like '24h', '7d', '2w'). "
                  "Use 'forget' to delete a specific memory by its numeric ID (you MUST use "
                  "'search' or 'recent' first to find the ID). "
                  "Use 'recent' to list recently stored memories. "
                  "Use 'save_contact' to store contact info (email, phone) for a person "
                  "(query: person name, field_type: email/phone, value: the address/number, "
                  "label: work/personal, entity_id: optional, use to select a specific person "
                  "when prompted for disambiguation). "
                  "Use 'find_contact' to look up contact info by name "
                  "(query: name, optional field_type). "
                  "Use 'list_contacts' to list all stored contacts (query: optional field_type). "
                  "Use 'delete_contact' to remove a contact record by ID. "
                  "Use 'merge_entities' to combine two entities that refer to the same "
                  "person/pet/place (query: source name to delete, target_name: entity to keep). "
                  "Memories persist across sessions and are private to each user.",
   .params = memory_params,
   .param_count = 7,

   .device_type = TOOL_DEVICE_TYPE_GETTER,
   .capabilities = TOOL_CAP_FILESYSTEM,
   .is_getter = false, /* remember/forget have side effects */
   .skip_followup = false,
   .default_remote = true,

   .config = NULL,
   .config_size = 0,
   .config_parser = NULL,
   .config_section = NULL, /* Uses [memory] section from main config */

   .is_available = memory_tool_is_available,

   .init = NULL,
   .cleanup = NULL,
   .callback = memoryCallback, /* Use existing callback from memory_callback.c */
};

/* ========== Availability Check ========== */

static bool memory_tool_is_available(void) {
   return g_config.memory.enabled;
}

/* ========== Public API ========== */

int memory_tool_register(void) {
   return tool_registry_register(&memory_metadata);
}
