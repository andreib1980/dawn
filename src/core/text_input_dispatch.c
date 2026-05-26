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
 * Provider-agnostic text-input dispatch — Layer 2.  See
 * include/core/text_input_dispatch.h for the public contract.
 *
 * Extracted from src/webui/webui_text_processing.c to let non-WebUI
 * sources (the planned messaging-channels engine for Telegram / Discord /
 * Slack, and any future text input path) share the same add-history /
 * persist / focus-injection / LLM-call core without reaching upward into
 * Layer 4 WebUI code.
 */
#include "core/text_input_dispatch.h"

#include <stdlib.h>
#include <string.h>

#include "auth/auth_db.h"
#include "core/session_manager.h"
#include "logging.h"

char *core_text_input_dispatch(session_t *session,
                               const char *text,
                               const char **vision_images,
                               const size_t *vision_image_sizes,
                               const char (*vision_mimes)[24],
                               int vision_image_count,
                               const text_input_dispatch_opts_t *opts) {
   if (!session || !text || text[0] == '\0') {
      return NULL;
   }

   /* Step 1: add user message to session history.  When images are
    * attached, store in the multi-part shape so they persist across
    * turns. */
   if (vision_image_count > 0 && vision_images) {
      session_add_message_with_images(session, "user", text, (const char *const *)vision_images,
                                      vision_image_count);
   } else {
      session_add_message(session, "user", text);
   }

   /* Step 2: persist to conv_db if requested.  The conv_db row ID is
    * stamped back into the in-memory history entry so subsequent
    * memory-extraction / context-injection code paths can reference
    * it. */
   bool persisted = false;
   if (opts && opts->conversation_id > 0) {
      int64_t msg_id = 0;
      if (conv_db_add_message_ex(opts->conversation_id, opts->auth_user_id, "user", text,
                                 &msg_id) == AUTH_DB_SUCCESS) {
         persisted = true;
         session_stamp_last_message_id(session, "user", msg_id);
      }
   }

   /* Step 3: fire the caller's user-msg-added hook BEFORE focus
    * injection + LLM call.  This preserves the existing WebUI
    * transcript-echo-before-LLM timing — the user sees their own
    * message render immediately while the server is still preparing
    * the response. */
   if (opts && opts->on_user_msg_added) {
      opts->on_user_msg_added(opts->user_msg_added_ctx, text, persisted);
   }

   /* Step 4: per-turn focus injection (memory + entity + relation +
    * summary + document + calendar candidates ranked into the system
    * prompt for this turn only).  Returns SUCCESS even on focus
    * failure; LLM dispatch is never blocked by this. */
   session_dispatch_user_turn(session, text);

   /* Step 4.5: optional channel hint.  When the caller (e.g. the
    * messaging engine for SMS) wants to give the LLM a one-turn
    * instruction about channel constraints, we append it AFTER focus
    * injection so the hint sits at the end of the system prompt where
    * recency-biased LLMs are most likely to honor it.  Re-acquires the
    * full prompt via session_get_system_prompt, appends, and writes
    * it back with session_update_system_prompt.  Applies only to this
    * turn — the next dispatch_user_turn call rebuilds the prompt. */
   if (opts && opts->channel_hint && opts->channel_hint[0] != '\0') {
      char *current = session_get_system_prompt(session);
      if (current) {
         size_t needed = strlen(current) + strlen(opts->channel_hint) + 8;
         char *augmented = malloc(needed);
         if (augmented) {
            snprintf(augmented, needed, "%s\n\n%s", current, opts->channel_hint);
            session_update_system_prompt(session, augmented);
            free(augmented);
         }
         free(current);
      }
   }

   /* Step 5: LLM call.  Uses the no_add variant because step 1
    * already added the user message; this also ensures the message
    * survives in history if the LLM call is interrupted or cancelled. */
   session_sentence_callback sentence_cb = opts ? opts->sentence_cb : NULL;
   void *sentence_userdata = opts ? opts->sentence_userdata : NULL;

   char *response = session_llm_call_with_tts_vision_no_add(session, text, vision_images,
                                                            vision_image_sizes, vision_mimes,
                                                            vision_image_count, sentence_cb,
                                                            sentence_userdata);

   return response;
}
