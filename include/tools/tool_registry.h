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
 * Tool Registry - Modular Tool Registration System
 *
 * This module provides a registration system for standalone tools. Each tool
 * registers its metadata (name, description, parameters), callback, and config
 * parser. This enables:
 * - Compile-time exclusion via CMake options (DAWN_ENABLE_X)
 * - Tools owning their own configuration and LLM schema
 * - Clean separation between core system and plugin tools
 */

#ifndef TOOL_REGISTRY_H
#define TOOL_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>

/* Forward declaration for TOML table (avoid including toml.h everywhere) */
typedef struct toml_table_t toml_table_t;

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Constants
 * ============================================================================= */

#define TOOL_MAX_REGISTERED 64 /* Max tools in registry */
#define TOOL_NAME_MAX 64       /* Max length of tool name */
#define TOOL_DESC_MAX 512      /* Max length of description */
#define TOOL_TOPIC_MAX 32      /* Max length of MQTT topic */
#define TOOL_PARAM_MAX 12      /* Max parameters per tool */
#define TOOL_PARAM_ENUM_MAX 16 /* Max enum values per parameter */
#define TOOL_ALIAS_MAX 8       /* Max aliases per tool */
#define TOOL_DEVICE_MAP_MAX 8  /* Max device map entries for meta-tools */
#define TOOL_SECRET_MAX 4      /* Max secret requirements per tool */

/* =============================================================================
 * Parameter Types and Mapping
 * ============================================================================= */

/**
 * @brief Parameter data types for tool definitions
 */
typedef enum {
   TOOL_PARAM_TYPE_STRING, /**< String parameter */
   TOOL_PARAM_TYPE_INT,    /**< Integer parameter */
   TOOL_PARAM_TYPE_NUMBER, /**< Floating-point parameter */
   TOOL_PARAM_TYPE_BOOL,   /**< Boolean parameter */
   TOOL_PARAM_TYPE_ENUM,   /**< Enumeration (string with allowed values) */
} tool_param_type_t;

/**
 * @brief How a parameter maps to the device/action/value model
 */
typedef enum {
   TOOL_MAPS_TO_VALUE,  /**< Parameter becomes "value" field */
   TOOL_MAPS_TO_ACTION, /**< Parameter becomes "action" field */
   TOOL_MAPS_TO_DEVICE, /**< Parameter becomes "device" field (for meta-tools) */
   TOOL_MAPS_TO_CUSTOM, /**< Custom field name (specified by field_name) */
} tool_param_mapping_t;

/**
 * @brief Device type (determines action_words pattern)
 */
typedef enum {
   TOOL_DEVICE_TYPE_BOOLEAN,    /**< enable/disable actions */
   TOOL_DEVICE_TYPE_ANALOG,     /**< set to value */
   TOOL_DEVICE_TYPE_GETTER,     /**< read-only query */
   TOOL_DEVICE_TYPE_MUSIC,      /**< play/pause/next/prev/stop */
   TOOL_DEVICE_TYPE_TRIGGER,    /**< single action */
   TOOL_DEVICE_TYPE_PASSPHRASE, /**< requires passphrase */
} tool_device_type_t;

/**
 * @brief Capability flags for tools
 *
 * Used for security decisions and runtime filtering.
 */
typedef enum {
   TOOL_CAP_NONE = 0,
   TOOL_CAP_DANGEROUS = (1 << 0),     /**< Requires explicit enable (e.g., shutdown) */
   TOOL_CAP_NETWORK = (1 << 1),       /**< Requires network access */
   TOOL_CAP_FILESYSTEM = (1 << 2),    /**< Accesses filesystem */
   TOOL_CAP_SECRETS = (1 << 3),       /**< Uses secrets.toml credentials */
   TOOL_CAP_ARMOR_FEATURE = (1 << 4), /**< OASIS armor-specific feature */
   TOOL_CAP_SCHEDULABLE = (1 << 5),   /**< Safe for scheduled task execution */
} tool_capability_t;

/* =============================================================================
 * Parameter Definition
 * ============================================================================= */

/**
 * @brief Parameter definition for a tool
 *
 * Note: Named treg_param_t to avoid conflict with tool_param_t in llm_tools.h
 */
typedef struct {
   const char *name;                             /**< Parameter name */
   const char *description;                      /**< Description for LLM */
   tool_param_type_t type;                       /**< Parameter type */
   bool required;                                /**< Is this parameter required? */
   tool_param_mapping_t maps_to;                 /**< How to map to device/action/value */
   const char *field_name;                       /**< Custom field for MAPS_TO_CUSTOM */
   const char *enum_values[TOOL_PARAM_ENUM_MAX]; /**< Allowed values for ENUM type */
   int enum_count;                               /**< Number of enum values */
   const char *unit;                             /**< Unit for analog params (e.g., "pixels") */
} treg_param_t;

/**
 * @brief Device map entry for meta-tools
 *
 * Maps a parameter value to an actual device name for meta-tools
 * that dispatch to multiple underlying devices.
 */
typedef struct {
   const char *key;    /**< Parameter value (e.g., "capture") */
   const char *device; /**< Actual device name (e.g., "audio capture device") */
} tool_device_map_t;

/**
 * @brief Secret requirement declaration (security)
 *
 * Tools declare what secrets they need at compile time.
 * Registry validates TOOL_CAP_SECRETS matches declarations.
 */
typedef struct {
   const char *secret_name; /**< Key in secrets.toml (e.g., "smartthings_access_token") */
   bool required;           /**< Fail init if missing? */
} tool_secret_requirement_t;

/* =============================================================================
 * Function Pointer Types
 * ============================================================================= */

/**
 * @brief Tool config parser function type
 *
 * Called during config parsing to let tool parse its TOML section.
 *
 * @param table TOML table for the tool's section (may be NULL if not present)
 * @param config Pointer to tool's config struct
 */
typedef void (*tool_config_parser_fn)(toml_table_t *table, void *config);

/**
 * @brief Tool config writer function type
 *
 * Called during config save to let tool write its TOML section.
 * The section header (e.g. "[home_assistant]\n") is written by the caller.
 *
 * @param fp     File pointer (FILE*) to write TOML key-value pairs to (void* to avoid stdio.h)
 * @param config Pointer to tool's config struct
 */
typedef void (*tool_config_writer_fn)(void *fp, const void *config);

/**
 * @brief Tool initialization function type
 *
 * Called after config parsing. Tool should initialize resources.
 *
 * @return 0 on success, non-zero on error
 */
typedef int (*tool_init_fn)(void);

/**
 * @brief Tool cleanup function type
 *
 * Called at shutdown. Tool should free resources.
 */
typedef void (*tool_cleanup_fn)(void);

/**
 * @brief Tool callback function type
 *
 * Called to execute the tool's functionality.
 *
 * @param action The action/subcommand (from MAPS_TO_ACTION parameter)
 * @param value The primary value (from MAPS_TO_VALUE parameter)
 * @param should_respond Set to 1 to return result to LLM, 0 to handle directly
 * @return Heap-allocated response string, or NULL
 */
typedef char *(*tool_callback_fn)(const char *action, char *value, int *should_respond);

/* =============================================================================
 * Tool Metadata (Complete Definition)
 * ============================================================================= */

/**
 * @brief Complete tool metadata
 *
 * Contains all information needed to register, execute, and generate
 * LLM tool schemas for a tool. Replaces JSON device entries.
 */
typedef struct {
   /* Identity */
   const char *name;                    /**< API name (e.g., "search") */
   const char *device_string;           /**< Callback device name */
   const char *topic;                   /**< MQTT topic */
   const char *aliases[TOOL_ALIAS_MAX]; /**< Alternative names */
   int alias_count;                     /**< Number of aliases */

   /* LLM Tool Schema */
   const char *description;    /**< Tool description for LLM */
   const treg_param_t *params; /**< Parameter definitions */
   int param_count;            /**< Number of parameters */

   /* Device Mapping (for meta-tools) */
   const tool_device_map_t *device_map; /**< Maps param values to devices */
   int device_map_count;                /**< Number of device map entries */

   /* Behavior Flags */
   tool_device_type_t device_type; /**< boolean, analog, getter, etc. */
   tool_capability_t capabilities; /**< Capability flags */
   bool is_getter;                 /**< Read-only, no side effects */
   bool skip_followup;             /**< Skip LLM follow-up response (see guide for details) */
   bool mqtt_only;                 /**< Only available via MQTT */
   bool sync_wait;                 /**< Wait for MQTT response */
   bool default_local;             /**< Available to local sessions */
   bool default_remote;            /**< Available to remote sessions */

   /** Optional runtime availability check (NULL = always available) */
   bool (*is_available)(void);

   /* Config (optional - NULL if tool has no config) */
   void *config;                        /**< Pointer to tool's config struct */
   size_t config_size;                  /**< sizeof() the config struct */
   tool_config_parser_fn config_parser; /**< Parser for TOML section */
   tool_config_writer_fn config_writer; /**< Writer for TOML section (optional) */
   const char *config_section;          /**< TOML section name */

   /* Secret Requirements (security - NULL-terminated array or NULL) */
   const tool_secret_requirement_t *secret_requirements;

   /* Lifecycle (optional - NULL if not needed) */
   tool_init_fn init;       /**< Called after config parse */
   tool_cleanup_fn cleanup; /**< Called at shutdown */

   /* Callback (required) */
   tool_callback_fn callback; /**< Execute tool functionality */
} tool_metadata_t;

/* =============================================================================
 * Lifecycle Functions
 * ============================================================================= */

/**
 * @brief Initialize the tool registry
 *
 * Must be called before any other registry functions.
 * Does NOT call tool init functions - call tool_registry_init_tools() after
 * config parsing is complete.
 *
 * @return 0 on success, non-zero on error
 */
int tool_registry_init(void);

/**
 * @brief Initialize all registered tools
 *
 * Calls init() for each registered tool in registration order.
 * Should be called after config parsing is complete.
 *
 * @return 0 on success, non-zero if any tool init fails
 */
int tool_registry_init_tools(void);

/**
 * @brief Lock the registry to prevent further registrations
 *
 * Should be called after all tools are registered but before network
 * services start. Prevents registration race conditions.
 */
void tool_registry_lock(void);

/**
 * @brief Check if registry is locked
 *
 * @return true if locked, false if registrations still allowed
 */
bool tool_registry_is_locked(void);

/**
 * @brief Check if tool registry is available for use
 *
 * Returns false if tool_registry_init() failed, indicating
 * the system should operate in degraded mode without tool support.
 *
 * @return true if tools are available, false if degraded mode
 */
bool tool_registry_is_available(void);

/**
 * @brief Shutdown all tools and free registry resources
 *
 * Calls cleanup() for each tool in reverse registration order.
 */
void tool_registry_shutdown(void);

/* =============================================================================
 * Registration Functions
 * ============================================================================= */

/**
 * @brief Register a tool with the registry
 *
 * Tools call this during initialization to register themselves.
 * Registration fails if:
 * - Registry is locked
 * - Registry is full
 * - Tool name already registered
 * - TOOL_CAP_DANGEROUS tool doesn't have config with enabled field
 * - TOOL_CAP_SECRETS tool doesn't declare secret_requirements
 *
 * @param metadata Pointer to tool's static metadata (must remain valid)
 * @return 0 on success, non-zero on error
 */
int tool_registry_register(const tool_metadata_t *metadata);

/* =============================================================================
 * Lookup Functions
 * ============================================================================= */

/**
 * @brief Look up a tool by name
 *
 * @param name Tool name to look up
 * @return Pointer to metadata, or NULL if not found
 */
const tool_metadata_t *tool_registry_lookup(const char *name);

/**
 * @brief Look up a tool by alias
 *
 * @param alias Alias to look up
 * @return Pointer to metadata, or NULL if not found
 */
const tool_metadata_t *tool_registry_lookup_alias(const char *alias);

/**
 * @brief Look up a tool by name or alias
 *
 * Checks both name and aliases.
 *
 * @param name_or_alias Name or alias to look up
 * @return Pointer to metadata, or NULL if not found
 */
const tool_metadata_t *tool_registry_find(const char *name_or_alias);

/**
 * @brief Get a tool's callback function
 *
 * Convenience function for callback lookup.
 *
 * @param name Tool name
 * @return Callback function, or NULL if not found
 */
tool_callback_fn tool_registry_get_callback(const char *name);

/**
 * @brief Check if a tool is enabled
 *
 * For TOOL_CAP_DANGEROUS tools, checks the config enabled field.
 * For other tools, always returns true if registered.
 *
 * @param name Tool name
 * @return true if enabled, false if disabled or not found
 */
bool tool_registry_is_enabled(const char *name);

/**
 * @brief Resolve device name from meta-tool device map
 *
 * For meta-tools, maps parameter values to actual device names.
 *
 * @param metadata The meta-tool metadata
 * @param key The parameter value to look up
 * @return The actual device name, or NULL if not found
 */
const char *tool_registry_resolve_device(const tool_metadata_t *metadata, const char *key);

/**
 * @brief Get the effective parameter definition for a tool
 *
 * Returns the parameter with any dynamic enum overrides applied.
 * This should be used for schema generation to ensure discovery
 * updates are reflected.
 *
 * @param tool_name Tool name
 * @param param_index Parameter index (0-based)
 * @return Pointer to effective param, or NULL if not found
 */
const treg_param_t *tool_registry_get_effective_param(const char *tool_name, int param_index);

/* =============================================================================
 * Config Integration
 * ============================================================================= */

/**
 * @brief Parse config sections for all registered tools
 *
 * Opens the config file and parses tool-specific sections.
 * Called after tools are registered but before they're initialized.
 *
 * @param config_path Path to dawn.toml config file
 * @return 0 on success, non-zero on error
 */
int tool_registry_parse_configs(const char *config_path);

/**
 * @brief Write tool-owned config sections to an open TOML file
 *
 * Iterates all registered tools that have config_writer and config_section,
 * writing their sections at the current file position. Called by config_write_toml()
 * to preserve tool config when rewriting the main config file.
 *
 * @param fp Open file pointer (FILE*, passed as void* to avoid stdio.h in header)
 */
void tool_registry_write_configs(void *fp);

/**
 * @brief Get a secret value by name
 *
 * Tools use this to access secrets they declared in secret_requirements.
 * Returns NULL if secret not found or tool didn't declare it.
 *
 * @param tool_name Name of requesting tool (for validation)
 * @param secret_name Secret key name
 * @return Secret value string, or NULL
 */
const char *tool_registry_get_secret(const char *tool_name, const char *secret_name);

/**
 * @brief Get a config string by path
 *
 * Allows tools to access global config values.
 * Path format: "section.key" (e.g., "localization.location")
 *
 * @param path Config path
 * @return Config value string, or NULL
 */
const char *tool_registry_get_config_string(const char *path);

/* =============================================================================
 * Iteration Functions
 * ============================================================================= */

/**
 * @brief Callback type for registry iteration
 */
typedef void (*tool_foreach_callback_t)(const tool_metadata_t *metadata, void *user_data);

/**
 * @brief Iterate over all registered tools
 *
 * @param callback Function to call for each tool
 * @param user_data Opaque pointer passed to callback
 */
void tool_registry_foreach(tool_foreach_callback_t callback, void *user_data);

/**
 * @brief Iterate over enabled tools only
 *
 * @param callback Function to call for each enabled tool
 * @param user_data Opaque pointer passed to callback
 */
void tool_registry_foreach_enabled(tool_foreach_callback_t callback, void *user_data);

/**
 * @brief Get count of registered tools
 *
 * @return Number of tools in registry
 */
int tool_registry_count(void);

/**
 * @brief Get tool metadata by index
 *
 * Allows iteration through all registered tools without needing
 * to know their names in advance.
 *
 * @param index Index from 0 to tool_registry_count()-1
 * @return Tool metadata pointer, or NULL if index out of range
 */
const tool_metadata_t *tool_registry_get_by_index(int index);

/**
 * @brief Get count of enabled tools
 *
 * @return Number of enabled tools
 */
int tool_registry_enabled_count(void);

/* =============================================================================
 * Capability Queries
 * ============================================================================= */

/**
 * @brief Check if a tool has a specific capability
 *
 * @param name Tool name
 * @param cap Capability flag to check
 * @return true if tool has capability, false otherwise
 */
bool tool_registry_has_capability(const char *name, tool_capability_t cap);

/**
 * @brief Iterate over tools with specific capability
 *
 * @param cap Capability flag to filter by
 * @param callback Function to call for each matching tool
 * @param user_data Opaque pointer passed to callback
 */
void tool_registry_foreach_with_capability(tool_capability_t cap,
                                           tool_foreach_callback_t callback,
                                           void *user_data);

/* =============================================================================
 * LLM Schema Generation
 * ============================================================================= */

/**
 * @brief Generate LLM tool schema for all enabled tools
 *
 * Creates JSON array of tool definitions for LLM native tool calling.
 * Filters based on session type (local vs remote) and armor mode.
 *
 * @param buffer Output buffer
 * @param size Buffer size
 * @param remote_session true if generating for remote session
 * @param armor_mode true if armor features should be included
 * @param bytes_written_out Output: number of bytes written (may be NULL)
 * @return SUCCESS or FAILURE
 */
int tool_registry_generate_llm_schema(char *buffer,
                                      size_t size,
                                      bool remote_session,
                                      bool armor_mode,
                                      int *bytes_written_out);

/* =============================================================================
 * Dynamic Parameter Updates
 * ============================================================================= */

/**
 * @brief Update enum values for a tool parameter dynamically
 *
 * This allows runtime modification of enum parameters, typically used for
 * MQTT-based discovery where external devices advertise their capabilities.
 *
 * The function makes a deep copy of the enum values into mutable storage
 * managed by the registry. The tool's original metadata is not modified;
 * instead, the registry maintains override storage for dynamic enums.
 *
 * Thread-safe: Uses registry mutex for synchronization.
 *
 * @param tool_name Name of the tool to update
 * @param param_name Name of the parameter with enum type
 * @param values Array of enum value strings (will be copied)
 * @param count Number of values in array
 * @return 0 on success, non-zero on error:
 *         1 = tool not found
 *         2 = parameter not found
 *         3 = parameter is not enum type
 *         4 = count exceeds TOOL_PARAM_ENUM_MAX
 */
int tool_registry_update_param_enum(const char *tool_name,
                                    const char *param_name,
                                    const char **values,
                                    int count);

/**
 * @brief Invalidate cached tool schemas
 *
 * Call after updating tool parameters to force regeneration of LLM schemas.
 * This ensures the LLM sees the updated enum values on the next request.
 *
 * Thread-safe: Uses registry mutex for synchronization.
 */
void tool_registry_invalidate_cache(void);

/**
 * @brief Check if schema cache is valid
 *
 * @return true if cache is valid, false if invalidated
 */
bool tool_registry_is_cache_valid(void);

/* =============================================================================
 * Direct Command Variation Statistics
 * ============================================================================= */

/**
 * @brief Count total direct command variations across all tools
 *
 * Calculates the total number of unique voice command patterns that can
 * be recognized for direct command execution. This counts:
 * - All patterns for each device type (boolean, analog, getter, etc.)
 * - Multiplied by (1 + alias_count) for each tool
 *
 * For example, a boolean tool with 2 aliases has:
 * - 14 patterns (8 enable + 6 disable) × 3 names (primary + 2 aliases) = 42 variations
 *
 * @return Total count of direct command variations
 */
int tool_registry_count_variations(void);

/**
 * @brief Count variations for a single tool
 *
 * @param name Tool name
 * @return Number of variations, or 0 if tool not found
 */
int tool_registry_count_tool_variations(const char *name);

/* =============================================================================
 * Custom Parameter Extraction Helpers
 *
 * TOOL_MAPS_TO_CUSTOM parameters are encoded by llm_tools.c as:
 *   "base_value::field_name::field_value[::field_name::field_value...]"
 *
 * These inline helpers decode the encoding. Co-located here so the
 * encode/decode contract lives in one place.
 * ============================================================================= */

#include <stdio.h>
#include <string.h>

#include "core/session_manager.h"

/**
 * @brief Extract a custom parameter value from an encoded value string
 *
 * @param value Full value string (may contain custom params)
 * @param field_name Name of field to extract
 * @param out_value Buffer for extracted value
 * @param out_len Size of out_value buffer
 * @return true if found, false otherwise
 */
static inline bool tool_param_extract_custom(const char *value,
                                             const char *field_name,
                                             char *out_value,
                                             size_t out_len) {
   if (!value || !field_name || !out_value)
      return false;

   char pattern[64];
   snprintf(pattern, sizeof(pattern), "::%s::", field_name);

   const char *pos = strstr(value, pattern);
   if (!pos)
      return false;

   const char *val_start = pos + strlen(pattern);
   const char *val_end = strstr(val_start, "::");
   size_t val_len = val_end ? (size_t)(val_end - val_start) : strlen(val_start);

   if (val_len >= out_len)
      val_len = out_len - 1;

   memcpy(out_value, val_start, val_len);
   out_value[val_len] = '\0';
   return true;
}

/**
 * @brief Extract the base value (before any custom params) from an encoded string
 *
 * @param value Full value string
 * @param out_base Buffer for base value
 * @param out_len Size of out_base buffer
 */
static inline void tool_param_extract_base(const char *value, char *out_base, size_t out_len) {
   if (!value || !out_base)
      return;

   const char *delim = strstr(value, "::");
   size_t base_len = delim ? (size_t)(delim - value) : strlen(value);

   if (base_len >= out_len)
      base_len = out_len - 1;

   memcpy(out_base, value, base_len);
   out_base[base_len] = '\0';
}

static inline int tool_get_current_user_id(void) {
   session_t *session = session_get_command_context();
   if (session && session->metrics.user_id > 0)
      return session->metrics.user_id;
   return 1;
}

#ifdef __cplusplus
}
#endif

#endif /* TOOL_REGISTRY_H */
