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
 */

#include <string.h>

#include "dawn_error.h"
#include "llm/llm_runtime_identity.h"
#include "unity.h"

void setUp(void) {
}

void tearDown(void) {
}

static void test_formats_ollama_identity_exactly(void) {
   char output[512];

   TEST_ASSERT_EQUAL_INT(SUCCESS, llm_runtime_identity_format("Ollama", "gemma4:12b", output,
                                                              sizeof(output)));

   TEST_ASSERT_NOT_NULL(strstr(output, "provider is \"Ollama\""));
   TEST_ASSERT_NOT_NULL(strstr(output, "model identifier is \"gemma4:12b\""));
   TEST_ASSERT_NOT_NULL(strstr(output, "Never guess"));
}

static void test_formats_openai_identity_exactly(void) {
   char output[512];

   TEST_ASSERT_EQUAL_INT(SUCCESS, llm_runtime_identity_format("OpenAI", "gpt-5-mini", output,
                                                              sizeof(output)));

   TEST_ASSERT_NOT_NULL(strstr(output, "provider is \"OpenAI\""));
   TEST_ASSERT_NOT_NULL(strstr(output, "model identifier is \"gpt-5-mini\""));
}

static void test_preserves_provider_and_model_verbatim(void) {
   char output[512];

   TEST_ASSERT_EQUAL_INT(SUCCESS, llm_runtime_identity_format("OpenRouter", "openai/gpt-5.4",
                                                              output, sizeof(output)));

   TEST_ASSERT_NOT_NULL(strstr(output, "\"OpenRouter\""));
   TEST_ASSERT_NOT_NULL(strstr(output, "\"openai/gpt-5.4\""));
}

static void test_rejects_missing_provider(void) {
   char output[512] = "stale";

   TEST_ASSERT_EQUAL_INT(FAILURE,
                         llm_runtime_identity_format("", "gemma4:12b", output, sizeof(output)));
   TEST_ASSERT_EQUAL_STRING("", output);
}

static void test_rejects_missing_model(void) {
   char output[512] = "stale";

   TEST_ASSERT_EQUAL_INT(FAILURE,
                         llm_runtime_identity_format("OpenAI", NULL, output, sizeof(output)));
   TEST_ASSERT_EQUAL_STRING("", output);
}

static void test_rejects_truncation(void) {
   char output[32] = "stale";

   TEST_ASSERT_EQUAL_INT(FAILURE, llm_runtime_identity_format("OpenAI", "gpt-5-mini", output,
                                                              sizeof(output)));
   TEST_ASSERT_EQUAL_STRING("", output);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_formats_ollama_identity_exactly);
   RUN_TEST(test_formats_openai_identity_exactly);
   RUN_TEST(test_preserves_provider_and_model_verbatim);
   RUN_TEST(test_rejects_missing_provider);
   RUN_TEST(test_rejects_missing_model);
   RUN_TEST(test_rejects_truncation);
   return UNITY_END();
}
