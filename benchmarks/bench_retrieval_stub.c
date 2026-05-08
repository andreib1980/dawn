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
 * the project author(s). Contributions include any modifications,
 * enhancements, or additions to the project. These contributions become
 * part of the project and are adopted by the project author(s).
 *
 * Stub providing globals for bench_retrieval standalone binary.
 * Allows document_db.c and embedding_engine.c to link without the
 * full daemon. Config fields are populated by main() from CLI args.
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "auth/auth_db_internal.h"
#include "config/dawn_config.h"

/* g_config and g_secrets are now defined by src/config/config_defaults.c
 * (linked into the bench for memory-pipeline mode). Pre-memory-pipeline the
 * bench used its own copies here.
 *
 * s_db is now defined by src/auth/auth_db_core.c. */

/* =============================================================================
 * Daemon-only stubs for memory-pipeline mode
 *
 * These symbols are referenced by memory_extraction.c, llm_*.c, and friends
 * when linked into the bench. The bench never drives the codepaths that need
 * real implementations (sessions, WebUI broadcasts, metrics) — these are
 * no-op shims so the link succeeds.
 *
 * If you're adding a new stub here and the function returns a session_t*
 * or buffer pointer, return NULL — callers null-check.
 * ============================================================================= */

/* session_manager.h has multiple inline definitions for these depending on
 * #ifdef gating; bench links against the non-inline-fallback path which
 * leaves the symbols undefined.  Define them as no-ops with sane returns. */
#include "core/session_manager.h"

session_t *session_get_command_context(void) {
   return NULL;
}
void session_set_command_context(session_t *session) {
   (void)session;
}
void session_get_llm_config(session_t *session, session_llm_config_t *config) {
   (void)session;
   if (config)
      memset(config, 0, sizeof(*config));
}

/* WebUI broadcasts — no-op when WebUI isn't built into the bench */
void webui_send_error(session_t *s, const char *code, const char *message) {
   (void)s;
   (void)code;
   (void)message;
}
void webui_send_thinking_start(session_t *s, const char *provider) {
   (void)s;
   (void)provider;
}
void webui_send_thinking_delta(session_t *s, const char *text) {
   (void)s;
   (void)text;
}
void webui_send_thinking_end(session_t *s, bool has_content) {
   (void)s;
   (void)has_content;
}
void webui_send_reasoning_summary(session_t *s, int reasoning_tokens) {
   (void)s;
   (void)reasoning_tokens;
}
void webui_broadcast_memory_notice(int user_id, const char *level, const char *message) {
   (void)user_id;
   (void)level;
   (void)message;
}

/* Metrics — recorded by daemon for observability; bench discards */
void metrics_record_llm_query(int duration_ms, bool is_cloud, bool success) {
   (void)duration_ms;
   (void)is_cloud;
   (void)success;
}
void metrics_record_llm_tokens(const char *provider,
                               int input_tokens,
                               int output_tokens,
                               int cached_tokens) {
   (void)provider;
   (void)input_tokens;
   (void)output_tokens;
   (void)cached_tokens;
}
void metrics_record_llm_ttft(int ttft_ms) {
   (void)ttft_ms;
}

/* TTS — bench has no audio output */
int text_to_speech(const char *text) {
   (void)text;
   return 0;
}

/* is_vision_enabled_for_current_llm is provided by linked llm_command_parser.c */

/* config_get / config_get_secrets are provided by config_defaults.c (linked) */

/* =============================================================================
 * Second batch — symbols pulled in by llm_*.c TUs that the bench links but
 * never executes (streaming / tool loop / context compaction paths).
 * ============================================================================= */

/* Session lifecycle — return NULL or no-op since the bench has no live sessions */
session_t *session_get(uint32_t session_id) {
   (void)session_id;
   return NULL;
}
void session_retain(session_t *session) {
   (void)session;
}
void session_release(session_t *session) {
   (void)session;
}
void session_record_query(session_t *session,
                          const char *provider,
                          uint64_t tokens_in,
                          uint64_t tokens_out,
                          uint64_t tokens_cached,
                          double llm_ttft_ms,
                          double llm_total_ms,
                          bool is_error) {
   (void)session;
   (void)provider;
   (void)tokens_in;
   (void)tokens_out;
   (void)tokens_cached;
   (void)llm_ttft_ms;
   (void)llm_total_ms;
   (void)is_error;
}

/* WebUI server entry points the daemon uses; bench is headless */
void webui_broadcast_conversation_renamed(int user_id, int64_t conv_id, const char *title) {
   (void)user_id;
   (void)conv_id;
   (void)title;
}
int64_t webui_get_active_conversation_id(int user_id) {
   (void)user_id;
   return 0;
}
void webui_send_compaction_complete(session_t *s) {
   (void)s;
}
void webui_send_metrics_update(session_t *s) {
   (void)s;
}
void webui_send_session_json(session_t *s, const char *json) {
   (void)s;
   (void)json;
}
void webui_send_state_with_detail(session_t *s, const char *state, const char *detail) {
   (void)s;
   (void)state;
   (void)detail;
}

/* Daemon metrics — recorded for observability, bench discards */
void metrics_log_activity(const char *event, const char *detail) {
   (void)event;
   (void)detail;
}
void metrics_record_fallback(void) {
}
void metrics_record_llm_total_time(int duration_ms, bool is_cloud) {
   (void)duration_ms;
   (void)is_cloud;
}
void metrics_update_llm_config(const char *provider, const char *model) {
   (void)provider;
   (void)model;
}

/* Command execution — only used by tool loop / direct mode; bench never gets there */
void cmd_exec_result_free(void *result) {
   (void)result;
}
char *command_execute(const char *json, int *should_respond) {
   (void)json;
   if (should_respond)
      *should_respond = 0;
   return NULL;
}
char *command_execute_json(const char *json, int *should_respond) {
   (void)json;
   if (should_respond)
      *should_respond = 0;
   return NULL;
}
void command_execute_mqtt_direct(const char *json) {
   (void)json;
}
char *command_execute_sync(const char *json) {
   (void)json;
   return NULL;
}

/* TOML helpers are provided by linked src/tools/toml.c */
/* config_get is provided by linked config_defaults.c */

struct mosquitto;
struct mosquitto *worker_pool_get_mosq(void) {
   return NULL;
}
bool component_status_is_hud_online(void) {
   return false;
}
void session_manager_refresh_all_prompts(void) {
}

/* Tool device-type lookup — registry has empty table in bench */
struct device_type_def;
const struct device_type_def *device_type_get_def(const char *name) {
   (void)name;
   return NULL;
}

/* llm_silent_observe.c brings full SSE + audit-log dependency cone.  The
 * bench never invokes silent observation, but config_validate.c calls
 * llm_silent_observe_provider_is_valid() when validating a freshly-loaded
 * dawn.toml.  Stub returns true so config_load passes; the bench's loaded
 * config never executes silent_observe paths. */
bool llm_silent_observe_provider_is_valid(const char *provider) {
   (void)provider;
   return true;
}
