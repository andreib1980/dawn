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
 * WebUI authentication helpers and system-prompt builder.
 *
 * Owns the per-connection auth gates (`conn_require_auth`,
 * `conn_require_admin`) used by every WebSocket message handler, plus
 * the system-prompt composition stack (`build_identity_block`,
 * `append_identity_block`, `build_base_block`, `dawn_build_prompt`) that
 * runs per turn to produce the user-specific system prompt + focus
 * block.  Split out of webui_server.c so that file can stay under the
 * size limits in CLAUDE.md.
 *
 * Whole-file compilation gated on ENABLE_AUTH — when authentication is
 * disabled the auth gates and prompt builder are absent from the build
 * and their callers (also under ENABLE_AUTH) vanish in lock-step.
 */

#include <stdlib.h>
#include <string.h>

#include "auth/auth_db.h"
#include "config/dawn_config.h"
#include "core/buf_printf.h"
#include "core/session_manager.h"
#include "llm/llm_command_parser.h"
#include "logging.h"
#include "memory/memory_context.h"
#include "webui/build_focus_block.h"
#include "webui/webui_internal.h"
#include "webui/webui_server.h"

/* =============================================================================
 * Authentication Helpers
 * ============================================================================= */

#ifdef ENABLE_AUTH

/* HTTP auth helpers (extract_session_cookie, is_request_authenticated)
 * moved to webui_http.c */

/**
 * @brief Check if WebSocket connection is authenticated
 *
 * CRITICAL: Re-validates session against database to prevent TOCTOU attacks
 * where session may have been revoked (password change, admin action, etc.)
 * but cached conn->authenticated flag remains true.
 *
 * Sends UNAUTHORIZED error if not authenticated or session invalid.
 *
 * @param conn WebSocket connection
 * @return true if authenticated with valid session, false otherwise (error sent)
 */
bool conn_require_auth(ws_connection_t *conn) {
   if (!conn->authenticated) {
      send_error_impl(conn->wsi, "UNAUTHORIZED", "Authentication required");
      return false;
   }

   /* Re-validate session from DB (prevents stale session exploitation) */
   auth_session_t session;
   if (auth_db_get_session(conn->auth_session_token, &session) != AUTH_DB_SUCCESS) {
      conn->authenticated = false;
      send_error_impl(conn->wsi, "UNAUTHORIZED", "Session expired or revoked");
      return false;
   }

   return true;
}

/**
 * @brief Check if WebSocket connection has admin privileges
 *
 * CRITICAL: Re-validates is_admin against database to prevent stale cache
 * exploitation if user is demoted mid-session.
 *
 * Sends UNAUTHORIZED if not authenticated, FORBIDDEN if not admin.
 *
 * @param conn WebSocket connection
 * @return true if admin, false otherwise (error sent)
 */
bool conn_require_admin(ws_connection_t *conn) {
   if (!conn->authenticated) {
      send_error_impl(conn->wsi, "UNAUTHORIZED", "Authentication required");
      return false;
   }

   /* Re-validate session from DB (prevents stale is_admin cache) */
   auth_session_t session;
   if (auth_db_get_session(conn->auth_session_token, &session) != AUTH_DB_SUCCESS) {
      conn->authenticated = false;
      send_error_impl(conn->wsi, "UNAUTHORIZED", "Session expired");
      return false;
   }

   if (!session.is_admin) {
      auth_db_log_event("PERMISSION_DENIED", conn->username, conn->client_ip,
                        "Admin access required");
      send_error_impl(conn->wsi, "FORBIDDEN", "Admin access required");
      return false;
   }

   return true;
}

/**
 * @brief Build the v44 user-identity block.
 *
 * Composes real_name / preferred_address / identity_aliases into the
 * "## User Identity" block that's appended after the persona/settings
 * block in the system prompt.  Returns an allocated string (caller
 * frees) or NULL if the user has no real_name set.
 *
 * Aliases parsing: newline-separated input, strip whitespace, drop empty
 * lines, dedupe case-insensitive, emit comma-joined.  Total output size
 * bounded by the AUTH_REAL_NAME_MAX + AUTH_PREFERRED_ADDRESS_MAX +
 * AUTH_IDENTITY_ALIASES_MAX caps and a small fixed overhead.
 *
 * @param user_id User ID (0 returns NULL — block requires a known user)
 * @return Allocated string (caller frees) or NULL if real_name unset
 */
static char *build_identity_block(int user_id) {
   if (user_id <= 0)
      return NULL;

   auth_user_identity_t identity;
   if (auth_db_get_user_identity(user_id, &identity) != AUTH_DB_SUCCESS)
      return NULL;
   if (identity.real_name[0] == '\0')
      return NULL; /* no real_name → skip the entire block */

   /* Parse identity_aliases: split on \n, strip whitespace per token,
    * drop empties, dedupe case-insensitive.  Up to 16 aliases tracked. */
   char joined_aliases[AUTH_IDENTITY_ALIASES_MAX];
   joined_aliases[0] = '\0';
   if (identity.identity_aliases[0] != '\0') {
      char buf[AUTH_IDENTITY_ALIASES_MAX];
      strncpy(buf, identity.identity_aliases, sizeof(buf) - 1);
      buf[sizeof(buf) - 1] = '\0';

      char *seen[16];
      int seen_count = 0;
      size_t out_off = 0;

      char *save = NULL;
      for (char *line = strtok_r(buf, "\n", &save); line != NULL && seen_count < 16;
           line = strtok_r(NULL, "\n", &save)) {
         /* Strip leading whitespace */
         while (*line == ' ' || *line == '\t' || *line == '\r')
            line++;
         /* Strip trailing whitespace */
         char *end = line + strlen(line);
         while (end > line && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
            end--;
         *end = '\0';
         if (*line == '\0')
            continue;
         /* Case-insensitive dedupe against already-emitted aliases. */
         bool dup = false;
         for (int i = 0; i < seen_count; i++) {
            if (strcasecmp(seen[i], line) == 0) {
               dup = true;
               break;
            }
         }
         if (dup)
            continue;
         seen[seen_count++] = line;
         /* Append to joined_aliases with comma separator. */
         size_t alias_len = strlen(line);
         size_t need = alias_len + (out_off > 0 ? 2 : 0); /* + ", " when not first */
         if (out_off + need + 1 >= sizeof(joined_aliases))
            break;
         if (out_off > 0) {
            joined_aliases[out_off++] = ',';
            joined_aliases[out_off++] = ' ';
         }
         memcpy(joined_aliases + out_off, line, alias_len);
         out_off += alias_len;
         joined_aliases[out_off] = '\0';
      }
   }

   /* Worst-case stack: ~1.5 KB.  Trip if any cap bumps push past 4 KB so we
    * notice before the LLM/refresh worker stack (8 MB) gets uncomfortable. */
   _Static_assert(
       AUTH_REAL_NAME_MAX + AUTH_PREFERRED_ADDRESS_MAX + AUTH_IDENTITY_ALIASES_MAX + 256 < 4096,
       "build_identity_block stack buffer exceeded 4 KB; revisit caps or heap-alloc");
   char block[AUTH_REAL_NAME_MAX + AUTH_PREFERRED_ADDRESS_MAX + AUTH_IDENTITY_ALIASES_MAX + 256];
   size_t off = 0;
   size_t rem = sizeof(block);
   BUF_PRINTF(block, off, rem, "\n\n## User Identity\nYou are speaking with %s.",
              identity.real_name);
   if (identity.preferred_address[0] != '\0') {
      BUF_PRINTF(block, off, rem, " They prefer to be addressed as %s.",
                 identity.preferred_address);
   }
   if (joined_aliases[0] != '\0') {
      BUF_PRINTF(block, off, rem, " They may also be referred to as: %s.", joined_aliases);
   }
   BUF_PRINTF(block, off, rem,
              " Use this information to recognize when memory facts or extracted entities refer "
              "to them.\n");
   return strdup(block);
}

/* Concatenate an existing owned base prompt with an optional identity
 * block; transfers ownership of @p base on success and frees the
 * identity block.  Returns the combined string (caller frees) or @p base
 * unchanged when no identity block applies. */
static char *append_identity_block(char *base, char *identity_block) {
   if (!identity_block)
      return base;
   if (!base) {
      free(identity_block);
      return NULL;
   }
   size_t base_len = strlen(base);
   size_t id_len = strlen(identity_block);
   char *combined = malloc(base_len + id_len + 1);
   if (!combined) {
      free(identity_block);
      return base; /* fallback: leave base unchanged */
   }
   memcpy(combined, base, base_len);
   memcpy(combined + base_len, identity_block, id_len);
   combined[base_len + id_len] = '\0';
   free(base);
   free(identity_block);
   return combined;
}

/**
 * @brief Build the base + persona/settings + identity string (no memory, no focus).
 *
 * Phase 1e split — memory and focus blocks are owned by the composer in
 * session_manager and concatenated downstream.  Supports two persona
 * modes from user settings:
 * - "append" (default): user settings appended as additional context.
 * - "replace": user's custom persona prepended with an override
 *   instruction.
 *
 * @param user_id User ID (0 for unauthenticated → base prompt copy only)
 * @return Allocated prompt string (caller frees)
 */
static char *build_base_block(int user_id) {
   /* Take an owned copy of the base prompt up-front. get_remote_command_prompt()
    * returns a pointer into a shared static buffer that can be rebuilt in place
    * by invalidate_system_instructions() firing from MQTT callback threads
    * (HUD status / discovery). We may do DB I/O below — holding the static
    * pointer across that work would race against a concurrent rebuild and
    * read a torn buffer. */
   const char *source = get_remote_command_prompt();
   if (!source)
      return NULL;
   char *base_prompt = strdup(source);
   if (!base_prompt)
      return NULL;

   /* No user ID - return the owned base prompt (transfer ownership) */
   if (user_id <= 0)
      return base_prompt;

   /* Load user settings */
   auth_user_settings_t settings;
   if (auth_db_get_user_settings(user_id, &settings) != AUTH_DB_SUCCESS)
      return append_identity_block(base_prompt, build_identity_block(user_id));

   /* Check if any settings are customized */
   bool has_persona = settings.persona_description[0] != '\0';
   bool has_location = settings.location[0] != '\0';
   bool has_timezone = settings.timezone[0] != '\0';
   bool has_units = settings.units[0] != '\0';
   bool is_replace_mode = (strcmp(settings.persona_mode, "replace") == 0);

   if (!has_persona && !has_location && !has_timezone && !has_units)
      return append_identity_block(base_prompt, build_identity_block(user_id));

   size_t base_len = strlen(base_prompt);

   /* Replace mode: Prepend custom persona with override instruction */
   if (is_replace_mode && has_persona) {
      /* Build replacement prefix (persona 512 + boilerplate ~130 = ~650 max) */
      char prefix[768];
      int prefix_ret = snprintf(prefix, sizeof(prefix),
                                "## Your Identity\n%s\n\n"
                                "IMPORTANT: Use the identity above. Ignore any conflicting persona "
                                "descriptions that follow.\n\n",
                                settings.persona_description);
      /* Clamp to actual buffer content (snprintf may return would-be length on truncation) */
      size_t prefix_len = (prefix_ret > 0 && (size_t)prefix_ret < sizeof(prefix))
                              ? (size_t)prefix_ret
                              : sizeof(prefix) - 1;

      /* Build suffix with other user context (loc 128 + tz 64 + units 16 = ~250 max) */
      char suffix[320];
      size_t suffix_len = 0;
      size_t suffix_rem = sizeof(suffix);

      if (has_location || has_timezone || has_units) {
         BUF_PRINTF(suffix, suffix_len, suffix_rem, "\n\n## User Info\n");
         if (has_location)
            BUF_PRINTF(suffix, suffix_len, suffix_rem, "Location: %s\n", settings.location);
         if (has_timezone)
            BUF_PRINTF(suffix, suffix_len, suffix_rem, "Timezone: %s\n", settings.timezone);
         if (has_units)
            BUF_PRINTF(suffix, suffix_len, suffix_rem, "Preferred units: %s\n", settings.units);
      } else {
         suffix[0] = '\0';
      }

      char *combined = malloc(prefix_len + base_len + suffix_len + 1);
      if (!combined)
         return base_prompt; /* fallback: transfer ownership of base copy */

      memcpy(combined, prefix, prefix_len);
      memcpy(combined + prefix_len, base_prompt, base_len);
      memcpy(combined + prefix_len + base_len, suffix, suffix_len);
      combined[prefix_len + base_len + suffix_len] = '\0';

      OLOG_DEBUG("dawn_build_prompt: REPLACE base for user_id=%d (%zu + %zu + %zu bytes)", user_id,
                 prefix_len, base_len, suffix_len);

      free(base_prompt);
      return append_identity_block(combined, build_identity_block(user_id));
   }

   /* Append mode: Add user context (persona 512 + loc 128 + tz 64 + units 16 + headers ~40) */
   char user_context[1024];
   size_t offset = 0;
   size_t remain = sizeof(user_context);

   BUF_PRINTF(user_context, offset, remain, "\n\n## User Context\n");

   if (has_persona) {
      BUF_PRINTF(user_context, offset, remain, "Additional persona traits: %s\n",
                 settings.persona_description);
   }
   if (has_location)
      BUF_PRINTF(user_context, offset, remain, "Location: %s\n", settings.location);
   if (has_timezone)
      BUF_PRINTF(user_context, offset, remain, "Timezone: %s\n", settings.timezone);
   if (has_units)
      BUF_PRINTF(user_context, offset, remain, "Preferred units: %s\n", settings.units);

   size_t context_len = strlen(user_context);
   char *combined = malloc(base_len + context_len + 1);
   if (!combined)
      return base_prompt; /* fallback: transfer ownership of base copy */

   memcpy(combined, base_prompt, base_len);
   memcpy(combined + base_len, user_context, context_len);
   combined[base_len + context_len] = '\0';

   OLOG_DEBUG("dawn_build_prompt: APPEND base for user_id=%d (%zu + %zu bytes)", user_id, base_len,
              context_len);

   free(base_prompt);
   return append_identity_block(combined, build_identity_block(user_id));
}

int dawn_build_prompt(int user_id,
                      const char *user_turn_text,
                      prompt_refresh_kind_t kind,
                      composed_prompt_t *out) {
   if (out == NULL)
      return FAILURE;
   /* Initialize output so the caller can safely composed_prompt_free
    * on either SUCCESS or FAILURE return. */
   out->base_prompt = NULL;
   out->memory_block = NULL;
   out->focus_block = NULL;

   /* `kind` is forward-compat in 1e — both kinds rebuild everything;
    * 1f wires kind-aware optimization (skip base+memory rebuild on
    * PER_TURN when dedup state says nothing changed). */
   (void)kind;

   /* Block 1: base + persona/settings.  NULL on hard failure (no
    * remote prompt source); session manager treats that as a refresh
    * failure and skips the system-prompt swap. */
   out->base_prompt = build_base_block(user_id);
   if (out->base_prompt == NULL)
      return FAILURE;

   /* Block 2: memory context (existing logic).  NULL when memory is
    * disabled or has nothing to surface — composer omits the marker
    * pair via byte-identical pre-1e behavior. */
   if (g_config.memory.enabled && user_id > 0) {
      out->memory_block = memory_build_context(user_id, g_config.memory.context_budget_tokens);
      /* memory_build_context returns NULL on empty result; no error
       * propagation needed. */
   }

   /* Block 3: per-turn focus.  Builder short-circuits on disabled
    * feature, empty/NULL turn text, or unauthenticated user.  On hard
    * focus_compose failure, we leave focus_block NULL and proceed —
    * a missing focus block is preferable to no LLM dispatch.  The
    * NULL guarantees the previous turn's content cannot leak (cross-
    * turn isolation invariant).
    *
    * Phase 1g-i: derive conv_id + turn_id from the dispatching session
    * (TLS-published in session_dispatch_user_turn, NULL on
    * SESSION_START refresh paths).  Both default to 0 when the
    * dispatch session is unavailable — build_focus_block treats
    * conv_id=0 as "skip the WebSocket broadcast" so SESSION_START /
    * standalone build_system_prompt_string callers never broadcast. */
   int64_t conv_id = 0;
   int64_t turn_id = 0;
   session_t *dispatch = session_get_dispatch_session();
   if (dispatch != NULL) {
      conv_id = webui_get_active_conversation_id(dispatch);
      turn_id = session_get_last_user_msg_id(dispatch);
   }
   if (build_focus_block(user_id, conv_id, turn_id, user_turn_text, &out->focus_block) != SUCCESS)
      out->focus_block = NULL;

   return SUCCESS;
}

#endif /* ENABLE_AUTH */
