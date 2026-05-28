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
 * Messaging LLM tool — actions: list_channels / send / link_status.
 * Delegates to messaging_engine.  See docs/MESSAGING_CHANNELS_DESIGN.md §3.
 */
#include "tools/messaging_tool.h"

#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config/dawn_config.h"
#include "core/session_manager.h"
#include "dawn_error.h"
#include "logging.h"
#include "messaging/messaging_discord.h"
#include "messaging/messaging_engine.h"
#include "messaging/messaging_slack.h"
#include "messaging/messaging_sms.h"
#include "messaging/messaging_telegram.h"
#include "tools/tool_registry.h"

static char *make_response(const char *msg) {
   return msg ? strdup(msg) : NULL;
}

static char *handle_list_channels(int user_id) {
   char *json = messaging_engine_list_channels_json(user_id);
   if (!json) {
      return make_response("Error: could not list channels.");
   }
   /* Wrap the JSON in a brief sentence so the LLM has natural framing. */
   size_t needed = strlen(json) + 64;
   char *result = malloc(needed);
   if (!result) {
      free(json);
      return NULL;
   }
   snprintf(result, needed, "Linked channels: %s", json);
   free(json);
   return result;
}

static char *handle_send(struct json_object *details, int user_id) {
   if (!details) {
      return make_response("Error: 'send' requires details with 'channel' and 'text'.");
   }

   struct json_object *chan_obj = NULL;
   struct json_object *text_obj = NULL;
   if (!json_object_object_get_ex(details, "channel", &chan_obj) ||
       !json_object_object_get_ex(details, "text", &text_obj)) {
      return make_response("Error: 'send' requires 'channel' and 'text' fields.");
   }
   const char *channel = json_object_get_string(chan_obj);
   const char *text = json_object_get_string(text_obj);
   if (!channel || !text || text[0] == '\0') {
      return make_response("Error: 'channel' and 'text' must be non-empty.");
   }

   int rc = messaging_engine_send(user_id, channel, text);
   switch (rc) {
      case MESSAGING_SUCCESS:
         return make_response("Message sent.");
      case MESSAGING_UNKNOWN_CHANNEL:
         return make_response(
             "Error: no channel by that name is linked. Use action 'list_channels' to see what's "
             "available, or have the user generate a linking code in the WebUI.");
      case MESSAGING_RATE_LIMITED:
         return make_response("Error: rate limit hit for this channel (default 10/min, 200/day). "
                              "Wait and retry, or surface this to the user.");
      case MESSAGING_PROVIDER_RATE_LIMITED:
         return make_response("Error: the provider's own rate limit was hit. Retry shortly.");
      case MESSAGING_DRIVER_NOT_REGISTERED:
         return make_response("Error: the driver for that channel's provider isn't configured. "
                              "Check secrets.toml and dawn.toml [messaging] section.");
      default:
         return make_response("Error: send failed (network or provider error).");
   }
}

static char *handle_reset_conversation(struct json_object *details, int user_id) {
   if (!details) {
      return make_response("Error: 'reset_conversation' requires details with a 'channel' field.");
   }
   struct json_object *chan_obj = NULL;
   if (!json_object_object_get_ex(details, "channel", &chan_obj)) {
      return make_response("Error: 'reset_conversation' requires 'channel'.");
   }
   const char *channel = json_object_get_string(chan_obj);
   if (!channel || channel[0] == '\0') {
      return make_response("Error: 'channel' must be non-empty.");
   }
   int rc = messaging_engine_reset_by_name(user_id, channel);
   switch (rc) {
      case MESSAGING_SUCCESS:
         return make_response("Conversation reset. The next message on that channel will start a "
                              "fresh thread; prior history is preserved in the WebUI.");
      case MESSAGING_UNKNOWN_CHANNEL:
         return make_response("Error: no channel by that name is linked. Use 'list_channels' to "
                              "see what's available.");
      default:
         return make_response("Error: reset failed (internal error).");
   }
}

static char *handle_link_status(struct json_object *details) {
   if (!details) {
      return make_response("Error: 'link_status' requires a 'code' field.");
   }
   struct json_object *code_obj = NULL;
   if (!json_object_object_get_ex(details, "code", &code_obj)) {
      return make_response("Error: 'link_status' requires 'code'.");
   }
   const char *code = json_object_get_string(code_obj);
   if (!code) {
      return make_response("Error: invalid code.");
   }

   messaging_link_state_t state = messaging_engine_link_status(code);
   switch (state) {
      case MESSAGING_LINK_STATE_PENDING:
         return make_response("Link code is pending — user has not yet sent /link to a bot.");
      case MESSAGING_LINK_STATE_CLAIMED:
         return make_response("Link code has been claimed. Channel is now active.");
      case MESSAGING_LINK_STATE_EXPIRED:
         return make_response(
             "Link code has expired. Generate a new one in WebUI Settings → Messaging.");
      case MESSAGING_LINK_STATE_NOT_FOUND:
      default:
         return make_response("Link code not recognized.");
   }
}

static char *messaging_callback(const char *action, char *value, int *should_respond) {
   if (should_respond) {
      *should_respond = 1;
   }
   if (!action) {
      return make_response("Error: missing action.");
   }

   /* Resolve user_id from session context (or default to 1). */
   int user_id = 1;
   session_t *ctx = session_get_command_context();
   if (ctx && ctx->metrics.user_id > 0) {
      user_id = ctx->metrics.user_id;
   }

   /* Parse details JSON if present. */
   struct json_object *details = NULL;
   if (value && value[0] != '\0') {
      details = json_tokener_parse(value);
   }

   char *result = NULL;
   if (strcmp(action, "list_channels") == 0) {
      result = handle_list_channels(user_id);
   } else if (strcmp(action, "send") == 0) {
      result = handle_send(details, user_id);
   } else if (strcmp(action, "link_status") == 0) {
      result = handle_link_status(details);
   } else if (strcmp(action, "reset_conversation") == 0) {
      result = handle_reset_conversation(details, user_id);
   } else {
      char buf[128];
      snprintf(buf, sizeof(buf), "Error: unknown action '%s'.", action);
      result = make_response(buf);
   }

   if (details) {
      json_object_put(details);
   }
   return result;
}

static const treg_param_t messaging_params[] = {
   {
       .name = "action",
       .description = "The messaging action: 'list_channels' (show linked channels for the "
                      "current user), 'send' (deliver a text message to a named channel), "
                      "'link_status' (check whether a pending link code has been claimed), "
                      "'reset_conversation' (close the current forever-thread on a channel and "
                      "start fresh next message; prior history is preserved in the WebUI)",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = true,
       .maps_to = TOOL_MAPS_TO_ACTION,
       .enum_values = { "list_channels", "send", "link_status", "reset_conversation" },
       .enum_count = 4,
   },
   {
       .name = "details",
       .description =
           "JSON object with action-specific fields (pass as JSON-encoded string).\n"
           "For 'send': {channel: 'telegram_main', text: 'message body'}.\n"
           "For 'link_status': {code: 'DAWNA7K9PQ'}.\n"
           "For 'reset_conversation': {channel: 'telegram_main'} — equivalent to the user "
           "sending /new in the chat app.\n"
           "For 'list_channels': no fields required.\n"
           "  channel: the display_name shown by 'list_channels' — must match EXACTLY. "
           "Cannot be invented; if no channel by that name appears in 'list_channels' "
           "output, the user has not linked one. Tell the user to link via WebUI Settings.\n"
           "  text: message body. SMS-linked channels truncate to ~400 chars on outbound "
           "(engine-enforced); Telegram/Discord allow longer. Keep concise for SMS.\n"
           "  code: 8-character uppercase Crockford base32 (e.g. 'DAWNA7K9PQ'); generated "
           "via the WebUI Settings → Messaging panel.",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_VALUE,
   },
};

static int messaging_tool_init(void) {
   if (messaging_engine_init() != MESSAGING_SUCCESS) {
      OLOG_ERROR("messaging_tool: engine init failed");
      return FAILURE;
   }
   /* SMS driver always registers — modem connection lifecycle belongs
    * to ECHO, not the messaging engine.  When the modem is offline,
    * outbound send_text fails gracefully through phone_service. */
   if (messaging_sms_register() != SUCCESS) {
      OLOG_WARNING("messaging_tool: SMS driver registration failed");
   }

   /* Conditionally register drivers based on configured tokens.  When
    * no token is set, the engine still runs (the LLM-facing tool can
    * report "no channels linked"), but no driver listens. */
   if (g_secrets.telegram_bot_token[0] != '\0') {
      if (messaging_telegram_register(g_secrets.telegram_bot_token) != SUCCESS) {
         OLOG_WARNING("messaging_tool: Telegram driver registration failed");
      }
   } else {
      OLOG_INFO("messaging_tool: no telegram_bot_token configured; Telegram disabled");
   }

   if (g_secrets.discord_bot_token[0] != '\0') {
      if (messaging_discord_register(g_secrets.discord_bot_token) != SUCCESS) {
         OLOG_WARNING("messaging_tool: Discord driver registration failed");
      }
   } else {
      OLOG_INFO("messaging_tool: no discord_bot_token configured; Discord disabled");
   }

   if (g_secrets.slack_app_token[0] != '\0' && g_secrets.slack_bot_token[0] != '\0') {
      if (messaging_slack_register(g_secrets.slack_app_token, g_secrets.slack_bot_token) !=
          SUCCESS) {
         OLOG_WARNING("messaging_tool: Slack driver registration failed");
      }
   } else {
      OLOG_INFO("messaging_tool: no slack_app_token/slack_bot_token configured; Slack disabled");
   }
   return SUCCESS;
}

static void messaging_tool_cleanup(void) {
   if (g_secrets.slack_app_token[0] != '\0' && g_secrets.slack_bot_token[0] != '\0') {
      messaging_slack_shutdown();
   }
   if (g_secrets.discord_bot_token[0] != '\0') {
      messaging_discord_shutdown();
   }
   if (g_secrets.telegram_bot_token[0] != '\0') {
      messaging_telegram_shutdown();
   }
   messaging_sms_shutdown();
   messaging_engine_shutdown();
}

static const tool_metadata_t messaging_metadata = {
   .name = "messaging",
   .device_string = "messaging",
   .topic = "dawn",
   .aliases = { "message", "send_message", "chat" },
   .alias_count = 3,

   .description = "Send and manage messages across linked chat platforms (Telegram, "
                  "Discord, Slack) and SMS. Use 'list_channels' to see what's linked, "
                  "'send' to deliver text to a named channel, 'link_status' to check "
                  "pending link codes. Each user manages their own channels via the "
                  "WebUI Settings panel.",
   .params = messaging_params,
   .param_count = 2,

   .device_type = TOOL_DEVICE_TYPE_TRIGGER,
   .capabilities = TOOL_CAP_NETWORK | TOOL_CAP_SCHEDULABLE,
   .is_getter = false,
   .default_local = true,
   .default_remote = true,

   .init = messaging_tool_init,
   .cleanup = messaging_tool_cleanup,
   .callback = messaging_callback,
};

int messaging_tool_register(void) {
   return tool_registry_register(&messaging_metadata);
}
