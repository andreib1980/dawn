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
 * Conversation history conversion for the OpenAI chat-completions path.
 * Converts Claude-format tool/image blocks to OpenAI format, filters
 * orphaned tool messages from restored conversations, and strips vision
 * content when the target LLM lacks vision support.
 */

#include <json-c/json.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "llm/llm_command_parser.h"
#include "llm/llm_openai_internal.h"
#include "logging.h"

/* Stack buffer for accumulating concatenated Claude text blocks during conversion.
 * Sized generously for a single assistant turn's text; the two converters below
 * (convert_claude_tool_to_openai and the vision-strip pass) must stay in sync. */
#define CLAUDE_TOOL_TEXT_BUF_MAX 8192

/* ── Content block helpers ──────────────────────────────────────────────── */

/* Lossless reconstruction of a Claude-shaped tool message into OpenAI-canonical
 * entries appended to @p out_array.  Returns the number of messages appended, or
 * 0 if @p msg carries no tool blocks (caller falls through to image/passthrough).
 *
 *   Claude assistant {content:[..text.., tool_use{id,name,input}]}
 *     → one OpenAI {role:assistant, content:<text>,
 *                   tool_calls:[{id,type:function,function:{name,arguments:<input JSON string>}}]}
 *   Claude user {content:[tool_result{tool_use_id,content}, ...]}
 *     → one OpenAI {role:tool, tool_call_id, content} PER result (fan-out), since
 *       OpenAI requires one tool message per tool_call_id.
 *
 * Replaces the former lossy "[Called tools: ...]" text summary so a Claude-stored
 * history sent to an OpenAI/OpenRouter endpoint keeps native function-calling shape. */
int convert_claude_tool_to_openai(struct json_object *msg, struct json_object *out_array) {
   struct json_object *content_obj;
   if (!json_object_object_get_ex(msg, "content", &content_obj) ||
       json_object_get_type(content_obj) != json_type_array) {
      return 0;
   }

   int arr_len = json_object_array_length(content_obj);
   bool has_tool_use = false;
   bool has_tool_result = false;
   for (int i = 0; i < arr_len; i++) {
      struct json_object *elem = json_object_array_get_idx(content_obj, i);
      struct json_object *type_obj;
      if (elem && json_object_object_get_ex(elem, "type", &type_obj)) {
         const char *t = json_object_get_string(type_obj);
         if (strcmp(t, "tool_use") == 0) {
            has_tool_use = true;
         } else if (strcmp(t, "tool_result") == 0) {
            has_tool_result = true;
         }
      }
   }
   if (!has_tool_use && !has_tool_result) {
      return 0; /* no tool blocks — caller handles images / passthrough */
   }

   int appended = 0;

   /* Assistant turn: collect text + tool_use blocks into one OpenAI assistant msg. */
   if (has_tool_use) {
      struct json_object *asst = json_object_new_object();
      json_object_object_add(asst, "role", json_object_new_string("assistant"));
      struct json_object *tool_calls = json_object_new_array();

      char text_buf[CLAUDE_TOOL_TEXT_BUF_MAX] = "";
      size_t toff = 0;
      for (int i = 0; i < arr_len; i++) {
         struct json_object *elem = json_object_array_get_idx(content_obj, i);
         struct json_object *type_obj;
         if (!elem || !json_object_object_get_ex(elem, "type", &type_obj)) {
            continue;
         }
         const char *t = json_object_get_string(type_obj);
         if (strcmp(t, "text") == 0) {
            struct json_object *txt;
            if (json_object_object_get_ex(elem, "text", &txt)) {
               const char *s = json_object_get_string(txt);
               size_t rem = (toff < sizeof(text_buf)) ? sizeof(text_buf) - toff : 0;
               if (s && *s && rem > 1) {
                  int w = snprintf(text_buf + toff, rem, "%s%s", toff ? "\n\n" : "", s);
                  if (w > 0) {
                     toff += (size_t)w;
                     if (toff >= sizeof(text_buf)) {
                        toff = sizeof(text_buf) - 1;
                     }
                  }
               }
            }
         } else if (strcmp(t, "tool_use") == 0) {
            struct json_object *id_obj, *name_obj, *input_obj;
            /* No id → no OpenAI tool_call_id can pair with a result; skip to avoid
             * emitting an unpairable tool_calls entry (a hard API error). */
            if (!json_object_object_get_ex(elem, "id", &id_obj) ||
                !json_object_get_string(id_obj) || json_object_get_string(id_obj)[0] == '\0') {
               OLOG_WARNING("OpenAI: dropping Claude tool_use with no id during conversion");
               continue;
            }
            struct json_object *tc = json_object_new_object();
            json_object_object_add(tc, "id", json_object_get(id_obj));
            json_object_object_add(tc, "type", json_object_new_string("function"));
            struct json_object *fn = json_object_new_object();
            if (json_object_object_get_ex(elem, "name", &name_obj)) {
               json_object_object_add(fn, "name", json_object_get(name_obj));
            }
            /* OpenAI arguments is a JSON STRING; Claude input is an object. */
            const char *args = "{}";
            if (json_object_object_get_ex(elem, "input", &input_obj)) {
               args = json_object_to_json_string(input_obj);
            }
            json_object_object_add(fn, "arguments", json_object_new_string(args));
            json_object_object_add(tc, "function", fn);
            json_object_array_add(tool_calls, tc);
         }
         /* "thinking" blocks are dropped — not representable in OpenAI history. */
      }
      json_object_object_add(asst, "content", json_object_new_string(text_buf));
      json_object_object_add(asst, "tool_calls", tool_calls);
      json_object_array_add(out_array, asst);
      appended++;
   }

   /* Result turn: fan out each Claude tool_result block to its own OpenAI tool msg. */
   if (has_tool_result) {
      for (int i = 0; i < arr_len; i++) {
         struct json_object *elem = json_object_array_get_idx(content_obj, i);
         struct json_object *type_obj;
         if (!elem || !json_object_object_get_ex(elem, "type", &type_obj) ||
             strcmp(json_object_get_string(type_obj), "tool_result") != 0) {
            continue;
         }
         struct json_object *tuid;
         /* No tool_use_id → an unpairable tool result; skip it (a hard API error). */
         if (!json_object_object_get_ex(elem, "tool_use_id", &tuid) ||
             !json_object_get_string(tuid) || json_object_get_string(tuid)[0] == '\0') {
            OLOG_WARNING(
                "OpenAI: dropping Claude tool_result with no tool_use_id during conversion");
            continue;
         }
         struct json_object *tool_msg = json_object_new_object();
         json_object_object_add(tool_msg, "role", json_object_new_string("tool"));
         json_object_object_add(tool_msg, "tool_call_id", json_object_get(tuid));
         /* Claude tool_result content may be a string or an array of blocks. */
         struct json_object *content_obj;
         const char *result = "";
         if (json_object_object_get_ex(elem, "content", &content_obj)) {
            result = (json_object_get_type(content_obj) == json_type_string)
                         ? json_object_get_string(content_obj)
                         : json_object_to_json_string(content_obj);
         }
         json_object_object_add(tool_msg, "content", json_object_new_string(result));
         json_object_array_add(out_array, tool_msg);
         appended++;
      }
   }

   return appended;
}

static struct json_object *convert_content_block_to_openai(struct json_object *block) {
   if (!block) {
      return NULL;
   }

   struct json_object *type_obj;
   if (!json_object_object_get_ex(block, "type", &type_obj)) {
      return json_object_get(block);
   }

   const char *block_type = json_object_get_string(type_obj);
   if (!block_type || strcmp(block_type, "image") != 0) {
      return json_object_get(block);
   }

   struct json_object *source_obj;
   if (!json_object_object_get_ex(block, "source", &source_obj)) {
      OLOG_WARNING("OpenAI: Claude image block missing source field");
      return NULL;
   }

   struct json_object *media_type_obj, *data_obj;
   const char *media_type = "image/jpeg";
   const char *data = NULL;

   if (json_object_object_get_ex(source_obj, "media_type", &media_type_obj)) {
      media_type = json_object_get_string(media_type_obj);
   }
   if (json_object_object_get_ex(source_obj, "data", &data_obj)) {
      data = json_object_get_string(data_obj);
   }

   if (!data) {
      OLOG_WARNING("OpenAI: Claude image block missing data");
      return NULL;
   }

   size_t url_len = strlen("data:") + strlen(media_type) + strlen(";base64,") + strlen(data) + 1;
   char *data_url = malloc(url_len);
   if (!data_url) {
      return NULL;
   }
   snprintf(data_url, url_len, "data:%s;base64,%s", media_type, data);

   struct json_object *image_obj = json_object_new_object();
   json_object_object_add(image_obj, "type", json_object_new_string("image_url"));

   struct json_object *url_obj = json_object_new_object();
   json_object_object_add(url_obj, "url", json_object_new_string(data_url));
   json_object_object_add(image_obj, "image_url", url_obj);

   free(data_url);

   OLOG_INFO("OpenAI: Converted Claude image to OpenAI image_url (media_type=%s)", media_type);
   return image_obj;
}

static bool is_vision_content_block(struct json_object *block) {
   struct json_object *type_obj;
   if (!json_object_object_get_ex(block, "type", &type_obj)) {
      return false;
   }
   const char *type_str = json_object_get_string(type_obj);
   return (strcmp(type_str, "image_url") == 0 || strcmp(type_str, "image") == 0);
}

static bool is_claude_image_block(struct json_object *block) {
   struct json_object *type_obj;
   if (!json_object_object_get_ex(block, "type", &type_obj)) {
      return false;
   }
   const char *type_str = json_object_get_string(type_obj);
   return (strcmp(type_str, "image") == 0);
}

/* ── History filters ────────────────────────────────────────────────────── */

/* A json object used as a string set (key present == member); value is unused. */
static bool id_set_contains(struct json_object *set, const char *id) {
   if (!set || !id) {
      return false;
   }
   struct json_object *unused;
   return json_object_object_get_ex(set, id, &unused);
}

/* Returns true if `content` is missing/NULL/empty. */
static bool assistant_content_empty(struct json_object *msg) {
   struct json_object *content_obj = NULL;
   json_object_object_get_ex(msg, "content", &content_obj);
   const char *s = content_obj ? json_object_get_string(content_obj) : NULL;
   return (s == NULL || s[0] == '\0');
}

/* Drops orphaned tool-related messages from a restored history so the request is
 * valid on OpenAI/OpenRouter, which reject ANY unpaired tool block.  Two orphan
 * directions are handled (a partial save — e.g. the assistant tool-call row persisted
 * but a role:tool result row failed — produces either):
 *   - forward:  a role:tool result with no matching assistant tool_call id;
 *   - reverse:  an assistant tool_calls[] entry with no matching role:tool result.
 * A legacy role:tool message with no tool_call_id field at all is degraded to a text
 * summary (older data predating E2's structured columns).  Returns a new (owned) array
 * when filtering was needed, otherwise a +1 ref to the input. */
static struct json_object *filter_orphaned_tool_messages(struct json_object *history) {
   int len = json_object_array_length(history);

   /* Pass 1: collect the set of result ids (from role:tool) and call ids (from
    * assistant tool_calls[]) so each side can be checked against the other. */
   struct json_object *result_ids = json_object_new_object();
   struct json_object *call_ids = json_object_new_object();

   for (int i = 0; i < len; i++) {
      struct json_object *msg = json_object_array_get_idx(history, i);
      struct json_object *role_obj;
      if (!json_object_object_get_ex(msg, "role", &role_obj)) {
         continue;
      }
      const char *role = json_object_get_string(role_obj);

      if (strcmp(role, "tool") == 0) {
         struct json_object *tcid_obj;
         if (json_object_object_get_ex(msg, "tool_call_id", &tcid_obj)) {
            const char *tcid = json_object_get_string(tcid_obj);
            if (tcid) {
               json_object_object_add(result_ids, tcid, NULL);
            }
         }
      } else if (strcmp(role, "assistant") == 0) {
         struct json_object *tool_calls_obj;
         if (json_object_object_get_ex(msg, "tool_calls", &tool_calls_obj) &&
             json_object_is_type(tool_calls_obj, json_type_array)) {
            int tc_len = json_object_array_length(tool_calls_obj);
            for (int j = 0; j < tc_len; j++) {
               struct json_object *id_obj;
               if (json_object_object_get_ex(json_object_array_get_idx(tool_calls_obj, j), "id",
                                             &id_obj)) {
                  const char *id = json_object_get_string(id_obj);
                  if (id) {
                     json_object_object_add(call_ids, id, NULL);
                  }
               }
            }
         }
      }
   }

   /* Pass 2: decide whether any message is an orphan (and so needs rebuilding). */
   bool needs_filtering = false;
   for (int i = 0; i < len && !needs_filtering; i++) {
      struct json_object *msg = json_object_array_get_idx(history, i);
      struct json_object *role_obj;
      if (!json_object_object_get_ex(msg, "role", &role_obj)) {
         continue;
      }
      const char *role = json_object_get_string(role_obj);

      if (strcmp(role, "tool") == 0) {
         struct json_object *tcid_obj;
         if (!json_object_object_get_ex(msg, "tool_call_id", &tcid_obj) ||
             !id_set_contains(call_ids, json_object_get_string(tcid_obj))) {
            needs_filtering = true;
         }
      } else if (strcmp(role, "assistant") == 0) {
         struct json_object *tool_calls_obj;
         bool has_tool_calls = json_object_object_get_ex(msg, "tool_calls", &tool_calls_obj) &&
                               json_object_is_type(tool_calls_obj, json_type_array);
         if (!has_tool_calls) {
            if (assistant_content_empty(msg)) {
               needs_filtering = true;
            }
         } else {
            int tc_len = json_object_array_length(tool_calls_obj);
            for (int j = 0; j < tc_len; j++) {
               struct json_object *id_obj;
               json_object_object_get_ex(json_object_array_get_idx(tool_calls_obj, j), "id",
                                         &id_obj);
               if (!id_obj || !id_set_contains(result_ids, json_object_get_string(id_obj))) {
                  needs_filtering = true;
                  break;
               }
            }
         }
      }
   }

   if (!needs_filtering) {
      json_object_put(result_ids);
      json_object_put(call_ids);
      return json_object_get(history);
   }

   OLOG_WARNING("OpenAI: Filtering orphaned tool messages from restored conversation");

   struct json_object *filtered = json_object_new_array();
   int orphan_count = 0;

   for (int i = 0; i < len; i++) {
      struct json_object *msg = json_object_array_get_idx(history, i);
      struct json_object *role_obj;

      if (!json_object_object_get_ex(msg, "role", &role_obj)) {
         json_object_array_add(filtered, json_object_get(msg));
         continue;
      }
      const char *role = json_object_get_string(role_obj);

      if (strcmp(role, "tool") == 0) {
         struct json_object *tcid_obj;
         if (!json_object_object_get_ex(msg, "tool_call_id", &tcid_obj)) {
            /* Legacy result with no id column — degrade to a text summary. */
            struct json_object *content_obj;
            if (json_object_object_get_ex(msg, "content", &content_obj)) {
               const char *content = json_object_get_string(content_obj);
               if (content && content[0] != '\0') {
                  struct json_object *summary_msg = json_object_new_object();
                  json_object_object_add(summary_msg, "role", json_object_new_string("assistant"));

                  char summary[2048];
                  snprintf(summary, sizeof(summary), "[Previous tool result: %.1900s%s]", content,
                           strlen(content) > 1900 ? "..." : "");
                  json_object_object_add(summary_msg, "content", json_object_new_string(summary));
                  json_object_array_add(filtered, summary_msg);
               }
            }
            orphan_count++;
            continue;
         }
         if (!id_set_contains(call_ids, json_object_get_string(tcid_obj))) {
            /* Result with no surviving call — drop (would be a hard API error). */
            orphan_count++;
            continue;
         }
         json_object_array_add(filtered, json_object_get(msg));
      } else if (strcmp(role, "assistant") == 0) {
         struct json_object *tool_calls_obj;
         bool has_tool_calls = json_object_object_get_ex(msg, "tool_calls", &tool_calls_obj) &&
                               json_object_is_type(tool_calls_obj, json_type_array);

         if (!has_tool_calls) {
            if (assistant_content_empty(msg)) {
               orphan_count++;
               continue;
            }
            json_object_array_add(filtered, json_object_get(msg));
            continue;
         }

         /* Keep only tool_calls whose result survived. */
         int tc_len = json_object_array_length(tool_calls_obj);
         struct json_object *kept_calls = json_object_new_array();
         for (int j = 0; j < tc_len; j++) {
            struct json_object *call = json_object_array_get_idx(tool_calls_obj, j);
            struct json_object *id_obj;
            json_object_object_get_ex(call, "id", &id_obj);
            if (id_obj && id_set_contains(result_ids, json_object_get_string(id_obj))) {
               json_object_array_add(kept_calls, json_object_get(call));
            }
         }

         if (json_object_array_length(kept_calls) == tc_len) {
            /* Nothing dropped — keep the original message verbatim. */
            json_object_put(kept_calls);
            json_object_array_add(filtered, json_object_get(msg));
         } else if (json_object_array_length(kept_calls) > 0) {
            /* Some calls survived — rebuild with the filtered tool_calls. */
            struct json_object *rebuilt = json_object_new_object();
            json_object_object_add(rebuilt, "role", json_object_new_string("assistant"));
            struct json_object *content_obj;
            if (json_object_object_get_ex(msg, "content", &content_obj)) {
               json_object_object_add(rebuilt, "content", json_object_get(content_obj));
            }
            json_object_object_add(rebuilt, "tool_calls", kept_calls);
            json_object_array_add(filtered, rebuilt);
            orphan_count += (tc_len - json_object_array_length(kept_calls));
         } else {
            /* No call survived — keep any text as a plain assistant turn, else drop. */
            json_object_put(kept_calls);
            if (!assistant_content_empty(msg)) {
               struct json_object *text_only = json_object_new_object();
               json_object_object_add(text_only, "role", json_object_new_string("assistant"));
               struct json_object *content_obj;
               json_object_object_get_ex(msg, "content", &content_obj);
               json_object_object_add(text_only, "content", json_object_get(content_obj));
               json_object_array_add(filtered, text_only);
            }
            orphan_count++;
         }
      } else {
         json_object_array_add(filtered, json_object_get(msg));
      }
   }

   if (orphan_count > 0) {
      OLOG_INFO("OpenAI: Filtered %d orphaned tool-related messages", orphan_count);
   }

   json_object_put(result_ids);
   json_object_put(call_ids);
   return filtered;
}

static struct json_object *convert_claude_tool_messages(struct json_object *history) {
   int len = json_object_array_length(history);

   bool needs_conversion = false;
   for (int i = 0; i < len && !needs_conversion; i++) {
      struct json_object *msg = json_object_array_get_idx(history, i);
      struct json_object *content_obj;
      if (json_object_object_get_ex(msg, "content", &content_obj) &&
          json_object_get_type(content_obj) == json_type_array) {
         int arr_len = json_object_array_length(content_obj);
         for (int j = 0; j < arr_len; j++) {
            struct json_object *elem = json_object_array_get_idx(content_obj, j);
            struct json_object *type_obj;
            if (json_object_object_get_ex(elem, "type", &type_obj)) {
               const char *type_str = json_object_get_string(type_obj);
               if (strcmp(type_str, "tool_use") == 0 || strcmp(type_str, "tool_result") == 0 ||
                   strcmp(type_str, "image") == 0) {
                  needs_conversion = true;
                  break;
               }
            }
         }
      }
   }

   if (!needs_conversion) {
      return json_object_get(history);
   }

   struct json_object *converted = json_object_new_array();

   for (int i = 0; i < len; i++) {
      struct json_object *msg = json_object_array_get_idx(history, i);

      if (convert_claude_tool_to_openai(msg, converted) > 0) {
         /* Structured OpenAI tool message(s) appended in place — nothing more to do. */
      } else {
         struct json_object *content_obj;
         if (json_object_object_get_ex(msg, "content", &content_obj) &&
             json_object_get_type(content_obj) == json_type_array) {
            int arr_len = json_object_array_length(content_obj);
            bool has_claude_images = false;
            for (int j = 0; j < arr_len && !has_claude_images; j++) {
               struct json_object *elem = json_object_array_get_idx(content_obj, j);
               if (is_claude_image_block(elem)) {
                  has_claude_images = true;
               }
            }

            if (has_claude_images) {
               struct json_object *new_msg = json_object_new_object();
               struct json_object *role_obj;
               if (json_object_object_get_ex(msg, "role", &role_obj)) {
                  json_object_object_add(new_msg, "role", json_object_get(role_obj));
               }

               struct json_object *new_content = json_object_new_array();
               for (int j = 0; j < arr_len; j++) {
                  struct json_object *elem = json_object_array_get_idx(content_obj, j);
                  struct json_object *converted_elem = convert_content_block_to_openai(elem);
                  if (converted_elem) {
                     json_object_array_add(new_content, converted_elem);
                  }
               }
               json_object_object_add(new_msg, "content", new_content);
               json_object_array_add(converted, new_msg);
            } else {
               json_object_array_add(converted, json_object_get(msg));
            }
         } else {
            json_object_array_add(converted, json_object_get(msg));
         }
      }
   }

   return converted;
}

static struct json_object *strip_vision_content(struct json_object *history) {
   int len = json_object_array_length(history);

   bool has_vision = false;
   for (int i = 0; i < len && !has_vision; i++) {
      struct json_object *msg = json_object_array_get_idx(history, i);
      struct json_object *content_obj;
      if (json_object_object_get_ex(msg, "content", &content_obj) &&
          json_object_get_type(content_obj) == json_type_array) {
         int arr_len = json_object_array_length(content_obj);
         for (int j = 0; j < arr_len; j++) {
            struct json_object *elem = json_object_array_get_idx(content_obj, j);
            if (is_vision_content_block(elem)) {
               has_vision = true;
               break;
            }
         }
      }
   }

   if (!has_vision) {
      return json_object_get(history);
   }

   OLOG_INFO("Stripping vision content from history (target LLM doesn't support vision)");

   struct json_object *sanitized = json_object_new_array();

   for (int i = 0; i < len; i++) {
      struct json_object *msg = json_object_array_get_idx(history, i);
      struct json_object *content_obj;

      if (json_object_object_get_ex(msg, "content", &content_obj) &&
          json_object_get_type(content_obj) == json_type_array) {
         int arr_len = json_object_array_length(content_obj);
         char text_buffer[CLAUDE_TOOL_TEXT_BUF_MAX] = "";
         size_t text_len = 0;
         bool found_image = false;

         for (int j = 0; j < arr_len; j++) {
            struct json_object *elem = json_object_array_get_idx(content_obj, j);
            struct json_object *type_obj;
            if (json_object_object_get_ex(elem, "type", &type_obj)) {
               const char *type_str = json_object_get_string(type_obj);
               if (strcmp(type_str, "text") == 0) {
                  struct json_object *text_obj;
                  if (json_object_object_get_ex(elem, "text", &text_obj)) {
                     const char *text = json_object_get_string(text_obj);
                     if (text && text_len < sizeof(text_buffer) - 1) {
                        if (text_len > 0) {
                           text_buffer[text_len++] = ' ';
                        }
                        size_t copy_len = strlen(text);
                        if (text_len + copy_len >= sizeof(text_buffer)) {
                           copy_len = sizeof(text_buffer) - text_len - 1;
                        }
                        memcpy(text_buffer + text_len, text, copy_len);
                        text_len += copy_len;
                        text_buffer[text_len] = '\0';
                     }
                  }
               } else if (is_vision_content_block(elem)) {
                  found_image = true;
               }
            }
         }

         struct json_object *new_msg = json_object_new_object();
         struct json_object *role_obj;
         if (json_object_object_get_ex(msg, "role", &role_obj)) {
            json_object_object_add(new_msg, "role", json_object_get(role_obj));
         }

         if (found_image) {
            if (text_len > 0) {
               char combined[8300];
               snprintf(combined, sizeof(combined), "%s [An image was shared earlier]",
                        text_buffer);
               json_object_object_add(new_msg, "content", json_object_new_string(combined));
            } else {
               json_object_object_add(new_msg, "content",
                                      json_object_new_string("[An image was shared here]"));
            }
         } else {
            json_object_object_add(new_msg, "content", json_object_new_string(text_buffer));
         }

         json_object_array_add(sanitized, new_msg);
      } else {
         json_object_array_add(sanitized, json_object_get(msg));
      }
   }

   return sanitized;
}

/* ── Public entry point ─────────────────────────────────────────────────── */

json_object *llm_openai_prepare_chat_history(struct json_object *conversation_history) {
   json_object *filtered = filter_orphaned_tool_messages(conversation_history);
   json_object *converted = convert_claude_tool_messages(filtered);
   json_object_put(filtered);

   if (!is_vision_enabled_for_current_llm()) {
      json_object *sanitized = strip_vision_content(converted);
      json_object_put(converted);
      converted = sanitized;
   }

   return converted;
}
