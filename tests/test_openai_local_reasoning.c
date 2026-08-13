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
 * Regression tests for local OpenAI-compatible reasoning controls.
 */

#include <json-c/json.h>
#include <stdbool.h>
#include <string.h>
#include <unity.h>

#include "llm/llm_local_provider.h"
#include "llm/llm_openai_internal.h"

static const char *thinking_mode;
static const char *reasoning_effort;
static bool tools_suppressed;
static local_provider_t local_provider;

const char *llm_get_current_thinking_mode(void) {
   return thinking_mode;
}

const char *llm_get_current_reasoning_effort(void) {
   return reasoning_effort;
}

bool llm_tools_suppressed(void) {
   return tools_suppressed;
}

int llm_get_effective_budget_tokens(void) {
   return 1024;
}

local_provider_t llm_local_get_provider(void) {
   return local_provider;
}

void setUp(void) {
   thinking_mode = "disabled";
   reasoning_effort = "medium";
   tools_suppressed = false;
   local_provider = LOCAL_PROVIDER_OLLAMA;
}

void tearDown(void) {
}

static const char *get_string(json_object *root, const char *name) {
   json_object *value = NULL;

   TEST_ASSERT_TRUE(json_object_object_get_ex(root, name, &value));

   return json_object_get_string(value);
}

static void test_ollama_disabled_uses_none(void) {
   json_object *root = json_object_new_object();

   llm_openai_add_local_thinking_params(root);

   TEST_ASSERT_EQUAL_STRING("none", get_string(root, "reasoning_effort"));
   TEST_ASSERT_FALSE(json_object_object_get_ex(root, "think", NULL));

   json_object_put(root);
}

static void test_ollama_enabled_preserves_supported_effort(void) {
   thinking_mode = "enabled";
   reasoning_effort = "low";
   json_object *root = json_object_new_object();

   llm_openai_add_local_thinking_params(root);

   TEST_ASSERT_EQUAL_STRING("low", get_string(root, "reasoning_effort"));

   json_object_put(root);
}

static void test_ollama_xhigh_clamps_to_high(void) {
   thinking_mode = "enabled";
   reasoning_effort = "xhigh";
   json_object *root = json_object_new_object();

   llm_openai_add_local_thinking_params(root);

   TEST_ASSERT_EQUAL_STRING("high", get_string(root, "reasoning_effort"));

   json_object_put(root);
}

static void test_suppressed_tools_disable_ollama_reasoning(void) {
   thinking_mode = "enabled";
   tools_suppressed = true;
   json_object *root = json_object_new_object();

   llm_openai_add_local_thinking_params(root);

   TEST_ASSERT_EQUAL_STRING("none", get_string(root, "reasoning_effort"));

   json_object_put(root);
}

static void test_llama_cpp_disabled_contract_is_preserved(void) {
   local_provider = LOCAL_PROVIDER_LLAMA_CPP;
   json_object *root = json_object_new_object();
   json_object *reasoning_budget = NULL;
   json_object *template_kwargs = NULL;
   json_object *enable_thinking = NULL;

   llm_openai_add_local_thinking_params(root);

   TEST_ASSERT_TRUE(json_object_object_get_ex(root, "reasoning_budget", &reasoning_budget));
   TEST_ASSERT_EQUAL_INT(0, json_object_get_int(reasoning_budget));

   TEST_ASSERT_TRUE(json_object_object_get_ex(root, "chat_template_kwargs", &template_kwargs));
   TEST_ASSERT_TRUE(
       json_object_object_get_ex(template_kwargs, "enable_thinking", &enable_thinking));
   TEST_ASSERT_FALSE(json_object_get_boolean(enable_thinking));

   json_object_put(root);
}

int main(void) {
   UNITY_BEGIN();

   RUN_TEST(test_ollama_disabled_uses_none);
   RUN_TEST(test_ollama_enabled_preserves_supported_effort);
   RUN_TEST(test_ollama_xhigh_clamps_to_high);
   RUN_TEST(test_suppressed_tools_disable_ollama_reasoning);
   RUN_TEST(test_llama_cpp_disabled_contract_is_preserved);

   return UNITY_END();
}
