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
 * Standalone implementations of LCM Phase 1 compaction helpers for unit testing.
 * These mirror the static functions in llm_context.c but compile without its
 * heavy dependency chain (curl, LLM interface, TTS, session manager, etc.).
 */

#define DAWN_TESTING

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "llm/llm_context.h"

/* ---- calculate_compaction_target (mirrors llm_context.c static) ---- */

int llm_context_calculate_compaction_target(int context_size, float threshold) {
   if (threshold < 0.25f)
      threshold = 0.25f;
   float target_ratio = threshold - 0.20f;
   float floor_ratio = 0.30f;
   if (target_ratio < floor_ratio)
      target_ratio = floor_ratio;
   return (int)(context_size * target_ratio);
}

/* ---- estimate_tokens_range (mirrors llm_context.c static) ---- */

int llm_context_estimate_tokens_range(struct json_object *history, int start_idx, int end_idx) {
   if (!history || !json_object_is_type(history, json_type_array))
      return 0;

   if (start_idx < 0)
      start_idx = 0;
   int len = json_object_array_length(history);
   if (end_idx > len)
      end_idx = len;
   if (start_idx >= end_idx)
      return 0;

   size_t total_chars = 0;

   for (int i = start_idx; i < end_idx; i++) {
      struct json_object *msg = json_object_array_get_idx(history, i);
      struct json_object *content_obj = NULL;

      if (json_object_object_get_ex(msg, "content", &content_obj)) {
         if (json_object_is_type(content_obj, json_type_string)) {
            const char *content = json_object_get_string(content_obj);
            if (content)
               total_chars += strlen(content);
         } else if (json_object_is_type(content_obj, json_type_array)) {
            int content_len = json_object_array_length(content_obj);
            for (int j = 0; j < content_len; j++) {
               struct json_object *part = json_object_array_get_idx(content_obj, j);
               struct json_object *text_obj = NULL;
               if (json_object_object_get_ex(part, "text", &text_obj)) {
                  const char *text = json_object_get_string(text_obj);
                  if (text)
                     total_chars += strlen(text);
               }
               struct json_object *type_obj = NULL;
               if (json_object_object_get_ex(part, "type", &type_obj)) {
                  const char *type = json_object_get_string(type_obj);
                  if (type && (strcmp(type, "image_url") == 0 || strcmp(type, "image") == 0))
                     total_chars += 3000;
               }
            }
         }
      }
      total_chars += 20;
   }

   size_t tokens = total_chars / 4;
   return (tokens > (size_t)INT_MAX) ? INT_MAX : (int)tokens;
}

/* ---- compact_deterministic (mirrors llm_context.c static) ---- */

#define COMPACT_DET_MAX_MSGS 200
#define COMPACT_DET_SNIPPET 80

char *llm_context_compact_deterministic(struct json_object *to_summarize, int token_budget) {
   int buf_size = token_budget * 4 + 128;
   char buf[LLM_CONTEXT_SUMMARY_TARGET_L3 * 4 + 128];
   if (buf_size > (int)sizeof(buf))
      buf_size = (int)sizeof(buf);

   int offset = 0;
   int written = snprintf(buf, buf_size, "[Conversation summary (truncated)]\n");
   if (written > 0)
      offset = written;

   int msg_count = json_object_array_length(to_summarize);
   int processed = 0;

   for (int i = 0; i < msg_count && processed < COMPACT_DET_MAX_MSGS; i++) {
      int remaining = buf_size - offset - 1;
      if (remaining < 40)
         break;

      struct json_object *msg = json_object_array_get_idx(to_summarize, i);
      struct json_object *role_obj = NULL;
      const char *role = "unknown";
      if (json_object_object_get_ex(msg, "role", &role_obj))
         role = json_object_get_string(role_obj);

      struct json_object *content_obj = NULL;
      const char *content = NULL;
      if (json_object_object_get_ex(msg, "content", &content_obj) &&
          json_object_is_type(content_obj, json_type_string)) {
         content = json_object_get_string(content_obj);
      }
      if (!content || content[0] == '\0')
         continue;

      int snippet_len = COMPACT_DET_SNIPPET;
      int content_len = (int)strlen(content);
      if (snippet_len > content_len)
         snippet_len = content_len;

      written = snprintf(buf + offset, remaining, "- %s: %.*s%s\n", role, snippet_len, content,
                         (content_len > snippet_len) ? "..." : "");
      if (written >= remaining) {
         buf[offset] = '\0';
         break;
      }
      offset += written;
      processed++;
   }

   return strdup(buf);
}
