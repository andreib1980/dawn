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
 * Regression tests for suppressing streamed reasoning when thinking is disabled.
 */

#include <stdbool.h>

#include "core/session_manager.h"
#include "llm/llm_context.h"
#include "llm/llm_tools.h"
#include "ui/metrics.h"
#include "webui/webui_server.h"

static const char *current_thinking_mode = "disabled";
static int usage_calls;
static uint32_t usage_session_id;
static int usage_prompt_tokens;
static int usage_completion_tokens;
static int usage_cached_tokens;

void test_set_thinking_mode(const char *mode) {
   current_thinking_mode = mode;
}

void test_reset_usage(void) {
   usage_calls = 0;
   usage_session_id = 0;
   usage_prompt_tokens = 0;
   usage_completion_tokens = 0;
   usage_cached_tokens = 0;
}

int test_get_usage_calls(void) {
   return usage_calls;
}

uint32_t test_get_usage_session_id(void) {
   return usage_session_id;
}

int test_get_usage_prompt_tokens(void) {
   return usage_prompt_tokens;
}

int test_get_usage_completion_tokens(void) {
   return usage_completion_tokens;
}

int test_get_usage_cached_tokens(void) {
   return usage_cached_tokens;
}

void llm_context_update_usage(uint32_t session_id,
                              int prompt_tokens,
                              int completion_tokens,
                              int cached_tokens) {
   usage_calls++;
   usage_session_id = session_id;
   usage_prompt_tokens = prompt_tokens;
   usage_completion_tokens = completion_tokens;
   usage_cached_tokens = cached_tokens;
}

const char *llm_get_current_thinking_mode(void) {
   return current_thinking_mode;
}

void metrics_record_llm_ttft(double ttft_ms) {
   (void)ttft_ms;
}

void metrics_record_llm_tokens(llm_type_t type,
                               cloud_provider_t cloud_provider,
                               int input_tokens,
                               int output_tokens,
                               int cached_tokens) {
   (void)type;
   (void)cloud_provider;
   (void)input_tokens;
   (void)output_tokens;
   (void)cached_tokens;
}

void webui_send_thinking_start(struct session *session, const char *provider) {
   (void)session;
   (void)provider;
}

void webui_send_thinking_delta(struct session *session, const char *text) {
   (void)session;
   (void)text;
}

void webui_send_thinking_end(struct session *session, bool has_content) {
   (void)session;
   (void)has_content;
}

void webui_send_session_json(struct session *session, const char *json_str) {
   (void)session;
   (void)json_str;
}

void webui_send_metrics_update(struct session *session,
                               const char *state,
                               int ttft_ms,
                               float token_rate,
                               int context_percent) {
   (void)session;
   (void)state;
   (void)ttft_ms;
   (void)token_rate;
   (void)context_percent;
}
