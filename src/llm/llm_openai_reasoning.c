/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * By contributing to this project, you agree to license your contributions
 * under the GPLv3 (or any later version) or any future licenses chosen by
 * the project author(s).
 *
 * Local OpenAI-compatible reasoning request parameters.
 */

#include <string.h>

#include "llm/llm_local_provider.h"
#include "llm/llm_openai_internal.h"
#include "llm/llm_tools.h"
#include "logging.h"

void llm_openai_add_local_thinking_params(json_object *root) {
   const char *thinking_mode = llm_get_current_thinking_mode();
   bool thinking_enabled = strcmp(thinking_mode, "disabled") != 0 && !llm_tools_suppressed();
   local_provider_t provider = llm_local_get_provider();

   if (provider == LOCAL_PROVIDER_OLLAMA) {
      const char *effort = "none";

      if (thinking_enabled) {
         effort = llm_get_current_reasoning_effort();

         if (!effort || effort[0] == '\0') {
            effort = "medium";
         } else if (strcmp(effort, "xhigh") == 0) {
            effort = "high";
         } else if (strcmp(effort, "low") != 0 && strcmp(effort, "medium") != 0 &&
                    strcmp(effort, "high") != 0) {
            effort = "medium";
         }
      }

      json_object_object_add(root, "reasoning_effort", json_object_new_string(effort));

      OLOG_INFO("Local LLM (Ollama OpenAI compatibility): reasoning_effort='%s'", effort);
      return;
   }

   if (thinking_enabled) {
      json_object *thinking = json_object_new_object();
      json_object_object_add(thinking, "type", json_object_new_string("enabled"));

      int budget = llm_get_effective_budget_tokens();
      json_object_object_add(thinking, "budget_tokens", json_object_new_int(budget));
      json_object_object_add(root, "thinking", thinking);
      json_object_object_add(root, "thinking_forced_open", json_object_new_boolean(1));

      json_object *template_kwargs = json_object_new_object();
      json_object_object_add(template_kwargs, "enable_thinking", json_object_new_boolean(1));
      json_object_object_add(root, "chat_template_kwargs", template_kwargs);

      OLOG_INFO("Local LLM (llama.cpp): Extended thinking enabled "
                "(budget: %d tokens)",
                budget);
   } else if (strcmp(thinking_mode, "disabled") == 0) {
      json_object_object_add(root, "reasoning_budget", json_object_new_int(0));

      json_object *template_kwargs = json_object_new_object();
      json_object_object_add(template_kwargs, "enable_thinking", json_object_new_boolean(0));
      json_object_object_add(root, "chat_template_kwargs", template_kwargs);

      OLOG_INFO("Local LLM (llama.cpp): Reasoning explicitly disabled");
   }
}
