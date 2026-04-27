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
 * Context Expand Tool — retrieve original messages from compacted summaries
 *
 * LCM Phase 3: when compaction summarizes messages, the summary includes a
 * [COMPACTED conv=N msgs=X-Y] tag. This tool retrieves the original messages
 * by querying the database, making compaction non-destructive from the model's
 * perspective.
 */

#include "tools/context_expand_tool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db.h"
#include "core/session_manager.h"
#include "logging.h"
#include "tools/tool_registry.h"
#ifdef ENABLE_WEBUI
#include "webui/webui_server.h"
#endif

#define EXPAND_TOKEN_BUDGET 4000
#define EXPAND_CHAR_BUDGET (EXPAND_TOKEN_BUDGET * 4)

static char *context_expand_callback(const char *action, char *value, int *should_respond);

static const treg_param_t context_expand_params[] = {
   {
       .name = "action",
       .description = "Action to perform",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = true,
       .maps_to = TOOL_MAPS_TO_ACTION,
       .enum_values = { "expand" },
       .enum_count = 1,
   },
   {
       .name = "start_id",
       .description = "First message ID from the [COMPACTED] tag",
       .type = TOOL_PARAM_TYPE_INT,
       .required = true,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "start_id",
   },
   {
       .name = "end_id",
       .description = "Last message ID from the [COMPACTED] tag",
       .type = TOOL_PARAM_TYPE_INT,
       .required = true,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "end_id",
   },
   {
       .name = "conversation_id",
       .description = "Conversation ID from the [COMPACTED] tag. "
                      "Omit to use the current conversation or its parent.",
       .type = TOOL_PARAM_TYPE_INT,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "conversation_id",
   },
};

static const tool_metadata_t context_expand_metadata = {
   .name = "context_expand",
   .device_string = "context_expand",
   .topic = "dawn",
   .aliases = { NULL },
   .alias_count = 0,

   .description = "Retrieve original messages that were compacted into a summary. "
                  "Look for [COMPACTED conv=N msgs=X-Y] tags in conversation summaries "
                  "and pass the values to this tool.",
   .params = context_expand_params,
   .param_count = 4,

   .device_type = TOOL_DEVICE_TYPE_GETTER,
   .capabilities = TOOL_CAP_NONE,
   .is_getter = true,
   .skip_followup = false,

   .is_available = NULL,
   .callback = context_expand_callback,
};

int context_expand_tool_register(void) {
   return tool_registry_register(&context_expand_metadata);
}

typedef struct {
   char *buf;
   int offset;
   int capacity;
   int count;
} expand_ctx_t;

static int expand_message_cb(const conversation_message_t *msg, void *ctx) {
   expand_ctx_t *ec = (expand_ctx_t *)ctx;

   int remaining = ec->capacity - ec->offset - 1;
   if (remaining < 100)
      return 1;

   int written = snprintf(ec->buf + ec->offset, remaining, "[%s]: %s\n", msg->role,
                          msg->content ? msg->content : "");
   if (written >= remaining) {
      ec->buf[ec->offset] = '\0';
      return 1;
   }
   ec->offset += written;
   ec->count++;
   return 0;
}

static char *context_expand_callback(const char *action, char *value, int *should_respond) {
   (void)action;
   *should_respond = 1;

   if (!value || value[0] == '\0')
      return strdup("Error: no message range provided.");

   char tmp[32];
   int64_t start_id = 0, end_id = 0, conv_id = 0;

   if (tool_param_extract_custom(value, "start_id", tmp, sizeof(tmp)))
      start_id = atoll(tmp);
   if (tool_param_extract_custom(value, "end_id", tmp, sizeof(tmp)))
      end_id = atoll(tmp);
   if (tool_param_extract_custom(value, "conversation_id", tmp, sizeof(tmp)))
      conv_id = atoll(tmp);

   if (start_id <= 0 || end_id <= 0)
      return strdup("Error: start_id and end_id are required (positive integers).");
   if (end_id < start_id)
      return strdup("Error: end_id must be >= start_id.");
   if (end_id - start_id > 500)
      return strdup("Error: range too large (max 500 messages).");

   int user_id = tool_get_current_user_id();
   if (user_id <= 0)
      return strdup("Error: no authenticated user.");

   /* If conversation_id not provided, use current or its parent */
   if (conv_id <= 0) {
#ifdef ENABLE_WEBUI
      session_t *session = session_get_command_context();
      if (session) {
         conv_id = webui_get_active_conversation_id(session);
         if (conv_id > 0) {
            conversation_t conv = { 0 };
            if (conv_db_get(conv_id, user_id, &conv) == AUTH_DB_SUCCESS) {
               if (conv.continued_from > 0)
                  conv_id = conv.continued_from;
               conv_free(&conv);
            }
         }
      }
#endif
      if (conv_id <= 0)
         return strdup("Error: conversation_id required (could not determine from context).");
   }

   OLOG_INFO("context_expand: expanding msgs %lld-%lld from conv %lld for user %d",
             (long long)start_id, (long long)end_id, (long long)conv_id, user_id);

   expand_ctx_t ec = { 0 };
   ec.capacity = EXPAND_CHAR_BUDGET + 256;
   ec.buf = malloc(ec.capacity);
   if (!ec.buf)
      return strdup("Error: memory allocation failed.");

   int header_len = snprintf(ec.buf, ec.capacity,
                             "Original messages %lld-%lld from conversation %lld:\n\n",
                             (long long)start_id, (long long)end_id, (long long)conv_id);
   ec.offset = header_len;

   int rc = conv_db_get_messages_by_range(conv_id, user_id, start_id, end_id, expand_message_cb,
                                          &ec);

   if (rc == AUTH_DB_FORBIDDEN) {
      free(ec.buf);
      return strdup("Error: access denied to that conversation.");
   }
   if (rc != AUTH_DB_SUCCESS) {
      free(ec.buf);
      return strdup("Error: failed to retrieve messages.");
   }

   if (ec.count == 0) {
      free(ec.buf);
      return strdup("No messages found in the specified range.");
   }

   if (ec.offset >= EXPAND_CHAR_BUDGET) {
      snprintf(ec.buf + ec.offset, ec.capacity - ec.offset,
               "\n[... truncated at token budget, %d messages shown]", ec.count);
   }

   OLOG_INFO("context_expand: returned %d messages (%d bytes)", ec.count, ec.offset);

   return ec.buf;
}
