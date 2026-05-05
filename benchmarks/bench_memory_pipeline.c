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
 * Memory-pipeline mode for bench_retrieval.
 *
 * Drives DAWN's production extraction pipeline end-to-end against a LoCoMo
 * conversation: ingests dialogs as messages with per-dia_id msg_ids, calls
 * memory_trigger_extraction() at each session boundary, polls until done,
 * dumps the resulting facts with v40 provenance.
 *
 * Phase 0 entry point. The full --memory-pipeline JSON protocol (conv_create,
 * add_message, extract, query_memory, reset_memory, snapshot_*) lands in
 * Phase 1; this file provides only the smoke-test path for now.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include "bench_memory_pipeline.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <json-c/json.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "bench_memory_schema.h"
#include "config/dawn_config.h"
#include "memory/memory_db.h"
#include "memory/memory_embeddings.h"
#include "memory/memory_extraction.h"

#define BENCH_MP_USER_ID 1
#define BENCH_MP_POLL_USEC 250000 /* 250 ms */
#define BENCH_MP_TIMEOUT_SEC 600  /* 10 min per session */
#define BENCH_MP_FACT_LIST_CAP 500
#define BENCH_MP_DIA_MAP_CAP 4096

extern dawn_config_t g_config;
extern secrets_config_t g_secrets;
extern auth_db_state_t s_db;

typedef struct {
   char dia_id[32];
   int64_t msg_id;
} dia_map_entry_t;

static dia_map_entry_t s_dia_map[BENCH_MP_DIA_MAP_CAP];
static int s_dia_map_count = 0;

static void dia_map_clear(void) {
   s_dia_map_count = 0;
}

static void dia_map_add(const char *dia_id, int64_t msg_id) {
   if (s_dia_map_count >= BENCH_MP_DIA_MAP_CAP)
      return;
   snprintf(s_dia_map[s_dia_map_count].dia_id, sizeof(s_dia_map[0].dia_id), "%s", dia_id);
   s_dia_map[s_dia_map_count].msg_id = msg_id;
   s_dia_map_count++;
}

/* =============================================================================
 * Init / teardown — opens :memory: SQLite + applies the bench memory DDL.
 * Must run instead of (not alongside) the existing bench setup_db(), which
 * creates an incompatible users-table schema.
 * ============================================================================= */

/* Use auth_db_init() so we get full production schema + prepared statements.
 * Pure :memory: would skip the prepared-statements step, breaking conv_db_*
 * and memory_db_* helpers.  Tmpfile is cheap, gets unlinked on teardown. */
static char s_bench_db_path[64] = "";

int bench_mp_init(void) {
   /* mkstemp gives us a unique writable path; close the fd immediately so
    * sqlite can open it. */
   snprintf(s_bench_db_path, sizeof(s_bench_db_path), "/tmp/bench_mp_XXXXXX.db");
   int fd = mkstemps(s_bench_db_path, 3);
   if (fd < 0) {
      fprintf(stderr, "bench_mp_init: mkstemps failed\n");
      return 1;
   }
   close(fd);
   /* Remove the empty file so auth_db_init creates a fresh DB without
    * tripping the permission-fix path. */
   unlink(s_bench_db_path);

   if (auth_db_init(s_bench_db_path) != AUTH_DB_SUCCESS) {
      fprintf(stderr, "bench_mp_init: auth_db_init(%s) failed\n", s_bench_db_path);
      return 1;
   }

   /* Insert benchmark user — auth_db_init doesn't seed users.  Use direct
    * sqlite3_exec since auth_db's user-creation path goes through password
    * hashing we don't need. */
   char *errmsg = NULL;
   const char *insert_user =
       "INSERT OR IGNORE INTO users (id, username, password_hash, created_at) "
       "VALUES (1, 'benchmark', '', strftime('%s','now'))";
   if (sqlite3_exec(s_db.db, insert_user, NULL, NULL, &errmsg) != SQLITE_OK) {
      fprintf(stderr, "bench_mp_init: insert user failed: %s\n", errmsg ? errmsg : "?");
      sqlite3_free(errmsg);
      return 1;
   }

   /* The BENCH_MEMORY_DDL header is retained as a documented subset for the
    * eventual --memory-pipeline JSON-protocol mode; it's not exec'd here
    * because auth_db_init already applied the production schema (superset). */
   (void)BENCH_MEMORY_DDL;
   return 0;
}

void bench_mp_teardown(void) {
   if (s_db.db) {
      sqlite3_close(s_db.db);
      s_db.db = NULL;
   }
   s_db.initialized = false;
   if (s_bench_db_path[0])
      unlink(s_bench_db_path);
}

/* =============================================================================
 * Wait for extraction to complete on the given user, with timeout.
 * Mirrors src/memory/memory_recovery.c:wait_for_user_extraction() but with
 * a tighter 250ms poll since the bench is single-purpose.
 * ============================================================================= */

static int wait_for_extraction(int user_id, int timeout_sec) {
   time_t deadline = time(NULL) + timeout_sec;
   while (memory_extraction_in_progress(user_id)) {
      if (time(NULL) >= deadline) {
         fprintf(stderr, "bench_mp: extraction timed out after %ds\n", timeout_sec);
         return 1;
      }
      usleep(BENCH_MP_POLL_USEC);
   }
   return 0;
}

/* =============================================================================
 * Build conversation_history JSON from messages added so far.
 * Each entry: {role, content, id}.  Extraction's internal filter
 * (memory_extraction.c:1198-1217) skips messages with id <= last_msg_id,
 * so passing the full array is correct for incremental per-session runs.
 * ============================================================================= */

static struct json_object *build_history_for_conv(int64_t conv_id, int *count_out) {
   sqlite3_stmt *stmt = NULL;
   const char *sql =
       "SELECT id, role, content FROM messages WHERE conversation_id = ? ORDER BY id ASC";
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      fprintf(stderr, "bench_mp: prepare history failed: %s\n", sqlite3_errmsg(s_db.db));
      return NULL;
   }
   sqlite3_bind_int64(stmt, 1, conv_id);

   struct json_object *arr = json_object_new_array();
   int n = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW) {
      int64_t msg_id = sqlite3_column_int64(stmt, 0);
      const char *role = (const char *)sqlite3_column_text(stmt, 1);
      const char *content = (const char *)sqlite3_column_text(stmt, 2);
      struct json_object *msg = json_object_new_object();
      json_object_object_add(msg, "role", json_object_new_string(role ? role : ""));
      json_object_object_add(msg, "content", json_object_new_string(content ? content : ""));
      json_object_object_add(msg, "id", json_object_new_int64(msg_id));
      json_object_array_add(arr, msg);
      n++;
   }
   sqlite3_finalize(stmt);
   if (count_out)
      *count_out = n;
   return arr;
}

/* =============================================================================
 * Smoke-test runner.  Reads LoCoMo JSON, picks conv_idx, ingests dialogs as
 * messages, fires extraction at each session boundary, dumps facts at end.
 * ============================================================================= */

static const char *role_for_speaker(const char *speaker, const char *first_speaker) {
   /* LoCoMo has two human speakers; DAWN's prompt wants user/assistant.
    * Map first-introduced speaker to "user", the second to "assistant". */
   if (!speaker || !first_speaker)
      return "user";
   return strcmp(speaker, first_speaker) == 0 ? "user" : "assistant";
}

int bench_mp_run_smoke(const char *locomo_path, int conv_idx) {
   /* 1. Load LoCoMo JSON */
   struct json_object *root = json_object_from_file(locomo_path);
   if (!root || !json_object_is_type(root, json_type_array)) {
      fprintf(stderr, "bench_mp: failed to load LoCoMo JSON from %s\n", locomo_path);
      if (root)
         json_object_put(root);
      return 1;
   }
   if (conv_idx < 0 || conv_idx >= (int)json_object_array_length(root)) {
      fprintf(stderr, "bench_mp: conv_idx %d out of range (have %d)\n", conv_idx,
              (int)json_object_array_length(root));
      json_object_put(root);
      return 1;
   }

   struct json_object *entry = json_object_array_get_idx(root, conv_idx);
   struct json_object *conv = NULL;
   if (!json_object_object_get_ex(entry, "conversation", &conv))
      conv = entry;

   /* 2. Create DAWN conversation */
   int64_t conv_id = 0;
   if (conv_db_create_with_origin(BENCH_MP_USER_ID, "smoke", "voice", &conv_id) !=
       AUTH_DB_SUCCESS) {
      fprintf(stderr, "bench_mp: conv_db_create_with_origin failed\n");
      json_object_put(root);
      return 1;
   }
   fprintf(stderr, "bench_mp: created conv_id=%" PRId64 "\n", conv_id);

   /* 3. Iterate sessions; ingest dialogs; trigger extraction at each boundary */
   dia_map_clear();
   char first_speaker[64] = "";
   int total_msgs = 0;
   int total_extractions = 0;
   time_t total_start = time(NULL);

   for (int session_n = 1;; session_n++) {
      char key[32];
      snprintf(key, sizeof(key), "session_%d", session_n);
      struct json_object *session = NULL;
      if (!json_object_object_get_ex(conv, key, &session))
         break;
      if (!json_object_is_type(session, json_type_array))
         continue;

      /* Ingest all dialogs in this session */
      size_t n_dialogs = json_object_array_length(session);
      for (size_t i = 0; i < n_dialogs; i++) {
         struct json_object *d = json_object_array_get_idx(session, i);
         struct json_object *dia_id_obj = NULL, *speaker_obj = NULL, *text_obj = NULL;
         json_object_object_get_ex(d, "dia_id", &dia_id_obj);
         json_object_object_get_ex(d, "speaker", &speaker_obj);
         json_object_object_get_ex(d, "text", &text_obj);
         const char *dia_id = dia_id_obj ? json_object_get_string(dia_id_obj) : "";
         const char *speaker = speaker_obj ? json_object_get_string(speaker_obj) : "?";
         const char *text = text_obj ? json_object_get_string(text_obj) : "";

         if (first_speaker[0] == '\0')
            snprintf(first_speaker, sizeof(first_speaker), "%s", speaker);
         const char *role = role_for_speaker(speaker, first_speaker);

         char content[2048];
         snprintf(content, sizeof(content), "%s said, \"%s\"", speaker, text);

         int64_t msg_id = 0;
         if (conv_db_add_message_ex(conv_id, BENCH_MP_USER_ID, role, content, &msg_id) !=
             AUTH_DB_SUCCESS) {
            fprintf(stderr, "bench_mp: conv_db_add_message_ex failed for %s\n", dia_id);
            continue;
         }
         dia_map_add(dia_id, msg_id);
         total_msgs++;

         /* Print mapping to stderr so the smoke caller can sanity-check */
         fprintf(stderr, "  %s -> msg_id=%" PRId64 "\n", dia_id, msg_id);
      }

      /* Trigger extraction at session boundary.  Build fresh history each time;
       * memory_extraction filters internally on last_extracted_msg_id. */
      int message_count = 0;
      struct json_object *history = build_history_for_conv(conv_id, &message_count);
      if (!history) {
         fprintf(stderr, "bench_mp: build_history failed\n");
         json_object_put(root);
         return 1;
      }

      char session_id_buf[64];
      snprintf(session_id_buf, sizeof(session_id_buf), "smoke_conv%d_s%d", conv_idx, session_n);

      time_t t_start = time(NULL);
      fprintf(stderr, "bench_mp: triggering extraction at session %d (%d total msgs)\n", session_n,
              message_count);

      int rc = memory_trigger_extraction(BENCH_MP_USER_ID, conv_id, session_id_buf, history,
                                         message_count, 0, NULL);
      json_object_put(history);

      if (rc != 0) {
         fprintf(stderr, "bench_mp: memory_trigger_extraction returned %d at session %d\n", rc,
                 session_n);
         /* Some return codes are benign (no new messages, too few messages) — keep going */
         continue;
      }

      if (wait_for_extraction(BENCH_MP_USER_ID, BENCH_MP_TIMEOUT_SEC) != 0) {
         fprintf(stderr, "bench_mp: extraction stuck at session %d\n", session_n);
         json_object_put(root);
         return 1;
      }

      total_extractions++;
      time_t elapsed = time(NULL) - t_start;
      fprintf(stderr, "bench_mp: session %d extraction done (%lds)\n", session_n, (long)elapsed);
   }

   /* 4. Dump facts with provenance */
   memory_fact_t *facts = calloc(BENCH_MP_FACT_LIST_CAP, sizeof(*facts));
   if (!facts) {
      fprintf(stderr, "bench_mp: calloc failed\n");
      json_object_put(root);
      return 1;
   }
   int fact_count = 0;
   if (memory_db_fact_list(BENCH_MP_USER_ID, facts, BENCH_MP_FACT_LIST_CAP, 0, &fact_count) !=
       MEMORY_DB_SUCCESS) {
      fprintf(stderr, "bench_mp: memory_db_fact_list failed\n");
      free(facts);
      json_object_put(root);
      return 1;
   }

   /* Batch-load provenance */
   int64_t *fact_ids = calloc((size_t)fact_count, sizeof(int64_t));
   int64_t *prov_conv_ids = calloc((size_t)fact_count, sizeof(int64_t));
   int64_t *prov_starts = calloc((size_t)fact_count, sizeof(int64_t));
   int64_t *prov_ends = calloc((size_t)fact_count, sizeof(int64_t));
   if (!fact_ids || !prov_conv_ids || !prov_starts || !prov_ends) {
      fprintf(stderr, "bench_mp: provenance calloc failed\n");
      free(facts);
      free(fact_ids);
      free(prov_conv_ids);
      free(prov_starts);
      free(prov_ends);
      json_object_put(root);
      return 1;
   }
   for (int i = 0; i < fact_count; i++)
      fact_ids[i] = facts[i].id;
   memory_db_facts_get_sources(BENCH_MP_USER_ID, fact_ids, fact_count, prov_conv_ids, prov_starts,
                               prov_ends);

   /* Output JSON summary */
   time_t total_elapsed = time(NULL) - total_start;
   printf("{\n");
   printf("  \"mode\": \"memory-pipeline-smoke\",\n");
   printf("  \"conv_idx\": %d,\n", conv_idx);
   printf("  \"conv_id\": %" PRId64 ",\n", conv_id);
   printf("  \"total_messages\": %d,\n", total_msgs);
   printf("  \"total_extractions\": %d,\n", total_extractions);
   printf("  \"total_seconds\": %ld,\n", (long)total_elapsed);
   printf("  \"extraction_provider\": \"%s\",\n", g_config.memory.extraction_provider);
   printf("  \"extraction_model\": \"%s\",\n", g_config.memory.extraction_model);
   printf("  \"fact_count\": %d,\n", fact_count);
   printf("  \"facts\": [\n");
   for (int i = 0; i < fact_count; i++) {
      const char *fact_text_esc = json_object_to_json_string(
          json_object_new_string(facts[i].fact_text));
      printf("    {\"id\": %" PRId64 ", \"text\": %s, \"category\": \"%s\", "
             "\"conv_id\": %" PRId64 ", \"msg_start\": %" PRId64 ", \"msg_end\": %" PRId64 "}%s\n",
             facts[i].id, fact_text_esc, facts[i].category, prov_conv_ids[i], prov_starts[i],
             prov_ends[i], i + 1 < fact_count ? "," : "");
   }
   printf("  ]\n");
   printf("}\n");

   free(facts);
   free(fact_ids);
   free(prov_conv_ids);
   free(prov_starts);
   free(prov_ends);
   json_object_put(root);
   return 0;
}

/* =============================================================================
 * JSON-protocol dispatcher (Phase 1).
 *
 * Each handler reads its inputs from `cmd` and writes a single JSON line to
 * stdout — same wire convention as handle_add/handle_query/handle_reset in
 * bench_retrieval.c.  Returns 1 if the command was recognised, 0 otherwise.
 * ============================================================================= */

static void respond_error(const char *message) {
   fprintf(stdout, "{\"status\":\"error\",\"message\":\"%s\"}\n", message ? message : "?");
   fflush(stdout);
}

/* dia_id → msg_id lookup over the in-process map. */
static int64_t dia_map_lookup(const char *dia_id) {
   if (!dia_id)
      return 0;
   for (int i = 0; i < s_dia_map_count; i++) {
      if (strcmp(s_dia_map[i].dia_id, dia_id) == 0)
         return s_dia_map[i].msg_id;
   }
   return 0;
}

/* Reverse: msg_id → dia_id (used by query_memory to compute covered_dia_ids
 * for retrieved facts whose provenance range covers stored msg_ids). */
static const char *dia_map_reverse(int64_t msg_id) {
   for (int i = 0; i < s_dia_map_count; i++) {
      if (s_dia_map[i].msg_id == msg_id)
         return s_dia_map[i].dia_id;
   }
   return NULL;
}

static int handle_conv_create(struct json_object *cmd) {
   const char *title = "bench";
   struct json_object *title_obj = NULL;
   if (json_object_object_get_ex(cmd, "title", &title_obj))
      title = json_object_get_string(title_obj);

   int64_t conv_id = 0;
   if (conv_db_create_with_origin(BENCH_MP_USER_ID, title, "voice", &conv_id) != AUTH_DB_SUCCESS) {
      respond_error("conv_db_create_with_origin failed");
      return 1;
   }

   /* Optional anchor_date override (v42).  When the bench passes a LoCoMo
    * session_X_date_time epoch, the extraction prompt resolves relative
    * temporal phrases against it instead of bench wall-clock. */
   struct json_object *anchor_obj = NULL;
   if (json_object_object_get_ex(cmd, "anchor_date", &anchor_obj)) {
      int64_t anchor_ts = json_object_get_int64(anchor_obj);
      if (anchor_ts > 0) {
         conv_db_force_anchor_date_unsafe(conv_id, BENCH_MP_USER_ID, anchor_ts);
      }
   }

   fprintf(stdout, "{\"status\":\"ok\",\"conv_id\":%" PRId64 "}\n", conv_id);
   fflush(stdout);
   return 1;
}

static int handle_add_message(struct json_object *cmd) {
   struct json_object *conv_obj = NULL, *dia_obj = NULL, *role_obj = NULL, *content_obj = NULL;
   if (!json_object_object_get_ex(cmd, "conv_id", &conv_obj) ||
       !json_object_object_get_ex(cmd, "dia_id", &dia_obj) ||
       !json_object_object_get_ex(cmd, "role", &role_obj) ||
       !json_object_object_get_ex(cmd, "content", &content_obj)) {
      respond_error("add_message: missing conv_id/dia_id/role/content");
      return 1;
   }
   int64_t conv_id = json_object_get_int64(conv_obj);
   const char *dia_id = json_object_get_string(dia_obj);
   const char *role = json_object_get_string(role_obj);
   const char *content = json_object_get_string(content_obj);

   int64_t msg_id = 0;
   if (conv_db_add_message_ex(conv_id, BENCH_MP_USER_ID, role, content, &msg_id) !=
       AUTH_DB_SUCCESS) {
      respond_error("conv_db_add_message_ex failed");
      return 1;
   }
   dia_map_add(dia_id, msg_id);
   fprintf(stdout, "{\"status\":\"ok\",\"msg_id\":%" PRId64 ",\"dia_id\":\"%s\"}\n", msg_id,
           dia_id);
   fflush(stdout);
   return 1;
}

/* Lightweight COUNT(*) helper — memory_db_fact_list with max=0 evaluates as
 * SQL LIMIT 0 and always returns count=0, so we can't use it for counting. */
static int count_table_for_user(const char *table, int user_id) {
   char sql[128];
   snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s WHERE user_id = ?", table);
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return 0;
   sqlite3_bind_int(stmt, 1, user_id);
   int count = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW)
      count = sqlite3_column_int(stmt, 0);
   sqlite3_finalize(stmt);
   return count;
}

static int handle_extract(struct json_object *cmd) {
   struct json_object *conv_obj = NULL, *sess_obj = NULL, *to_obj = NULL;
   if (!json_object_object_get_ex(cmd, "conv_id", &conv_obj) ||
       !json_object_object_get_ex(cmd, "session_id", &sess_obj)) {
      respond_error("extract: missing conv_id/session_id");
      return 1;
   }
   int64_t conv_id = json_object_get_int64(conv_obj);
   const char *session_id = json_object_get_string(sess_obj);
   int timeout_sec = BENCH_MP_TIMEOUT_SEC;
   if (json_object_object_get_ex(cmd, "timeout_ms", &to_obj)) {
      int ms = json_object_get_int(to_obj);
      if (ms > 0)
         timeout_sec = ms / 1000;
   }

   /* Optional per-session anchor override (v42).  LoCoMo conversations span
    * months across sessions, so each session's extraction needs its own anchor
    * for relative-phrase resolution.  Bench harness sends session_X_date_time
    * here; we update the conversation row before triggering extraction.
    *
    * Note: this overwrites the row's anchor_date per session — the value is
    * NOT preserved across sessions in the row.  A bench abort mid-conversation
    * leaves anchor_date set to the last-completed session.  Cache-miss reruns
    * recreate the conversation cleanly so this is benign in practice. */
   struct json_object *anchor_obj = NULL;
   if (json_object_object_get_ex(cmd, "anchor_date", &anchor_obj)) {
      int64_t anchor_ts = json_object_get_int64(anchor_obj);
      if (anchor_ts > 0) {
         conv_db_force_anchor_date_unsafe(conv_id, BENCH_MP_USER_ID, anchor_ts);
      }
   }

   int facts_before = count_table_for_user("memory_facts", BENCH_MP_USER_ID);
   int entities_before = count_table_for_user("memory_entities", BENCH_MP_USER_ID);

   int message_count = 0;
   struct json_object *history = build_history_for_conv(conv_id, &message_count);
   if (!history) {
      respond_error("build_history_for_conv failed");
      return 1;
   }

   time_t t_start = time(NULL);
   int rc = memory_trigger_extraction(BENCH_MP_USER_ID, conv_id, session_id, history, message_count,
                                      0, NULL);
   json_object_put(history);

   if (rc != 0) {
      /* Skip codes are benign (no new messages, too few). Report 0/0/0 deltas. */
      fprintf(stdout,
              "{\"status\":\"ok\",\"duration_ms\":0,\"facts_added\":0,"
              "\"entities_added\":0,\"trigger_rc\":%d}\n",
              rc);
      fflush(stdout);
      return 1;
   }

   if (wait_for_extraction(BENCH_MP_USER_ID, timeout_sec) != 0) {
      respond_error("extraction timed out");
      return 1;
   }

   int facts_after = count_table_for_user("memory_facts", BENCH_MP_USER_ID);
   int entities_after = count_table_for_user("memory_entities", BENCH_MP_USER_ID);

   long duration_ms = (long)(time(NULL) - t_start) * 1000;
   fprintf(stdout,
           "{\"status\":\"ok\",\"duration_ms\":%ld,\"facts_added\":%d,"
           "\"entities_added\":%d,\"facts_total\":%d,\"entities_total\":%d}\n",
           duration_ms, facts_after - facts_before, entities_after - entities_before, facts_after,
           entities_after);
   fflush(stdout);
   return 1;
}

static int handle_query_memory(struct json_object *cmd) {
   struct json_object *text_obj = NULL, *topk_obj = NULL;
   if (!json_object_object_get_ex(cmd, "text", &text_obj)) {
      respond_error("query_memory: missing text");
      return 1;
   }
   const char *query_text = json_object_get_string(text_obj);
   int top_k = 10;
   if (json_object_object_get_ex(cmd, "top_k", &topk_obj))
      top_k = json_object_get_int(topk_obj);
   if (top_k <= 0)
      top_k = 10;
   if (top_k > BENCH_MP_FACT_LIST_CAP)
      top_k = BENCH_MP_FACT_LIST_CAP;

   /* Hybrid search.  Pass NULL keyword inputs since LoCoMo questions are
    * typically full sentences — pure cosine + temporal works as a clean Phase 1
    * baseline; pre-keyword scoring can be added later if the orchestrator
    * benefits from it. */
   embedding_search_result_t *results = calloc((size_t)top_k, sizeof(*results));
   if (!results) {
      respond_error("alloc failed");
      return 1;
   }
   int n_results = memory_embeddings_hybrid_search(BENCH_MP_USER_ID, query_text, NULL, NULL, 0, 0,
                                                   results, top_k);
   if (n_results < 0)
      n_results = 0;

   /* Batch-load provenance + per-fact text */
   int64_t *fact_ids = calloc((size_t)n_results, sizeof(int64_t));
   int64_t *prov_convs = calloc((size_t)n_results, sizeof(int64_t));
   int64_t *prov_starts = calloc((size_t)n_results, sizeof(int64_t));
   int64_t *prov_ends = calloc((size_t)n_results, sizeof(int64_t));
   if (n_results > 0 && (!fact_ids || !prov_convs || !prov_starts || !prov_ends)) {
      free(results);
      free(fact_ids);
      free(prov_convs);
      free(prov_starts);
      free(prov_ends);
      respond_error("alloc failed");
      return 1;
   }
   for (int i = 0; i < n_results; i++)
      fact_ids[i] = results[i].fact_id;
   if (n_results > 0)
      memory_db_facts_get_sources(BENCH_MP_USER_ID, fact_ids, n_results, prov_convs, prov_starts,
                                  prov_ends);

   /* Emit results.  covered_dia_ids[]: for each retrieved fact, every dia_id
    * whose msg_id is in [msg_start, msg_end].  Empty list if range is zeroed. */
   fprintf(stdout, "{\"status\":\"ok\",\"results\":[");
   for (int i = 0; i < n_results; i++) {
      memory_fact_t fact = { 0 };
      memory_db_fact_get(results[i].fact_id, &fact);
      struct json_object *text_str = json_object_new_string(fact.fact_text);
      const char *text_json = json_object_to_json_string(text_str);

      fprintf(stdout, "%s{\"fact_id\":%" PRId64 ",\"score\":%.6f,\"text\":%s,", i ? "," : "",
              results[i].fact_id, (double)results[i].score, text_json);
      fprintf(stdout, "\"conv_id\":%" PRId64 ",\"msg_start\":%" PRId64 ",\"msg_end\":%" PRId64 ",",
              prov_convs[i], prov_starts[i], prov_ends[i]);
      fprintf(stdout, "\"covered_dia_ids\":[");
      bool first = true;
      if (prov_starts[i] > 0 && prov_ends[i] >= prov_starts[i]) {
         for (int64_t mid = prov_starts[i]; mid <= prov_ends[i]; mid++) {
            const char *did = dia_map_reverse(mid);
            if (!did)
               continue;
            fprintf(stdout, "%s\"%s\"", first ? "" : ",", did);
            first = false;
         }
      }
      fprintf(stdout, "]}");
      json_object_put(text_str);
   }
   fprintf(stdout, "]}\n");
   fflush(stdout);

   free(results);
   free(fact_ids);
   free(prov_convs);
   free(prov_starts);
   free(prov_ends);
   return 1;
}

static int handle_reset_memory(struct json_object *cmd) {
   (void)cmd;
   /* Drop facts/preferences/relations/entities/summaries for the bench user.
    * Conversations are kept for the run; orchestrator drives reset between
    * LoCoMo conversations by creating new conv_ids — that's cheap and avoids
    * having to mass-delete via raw SQL. */
   int rc = memory_db_delete_user_memories(BENCH_MP_USER_ID);
   dia_map_clear();
   fprintf(stdout, "{\"status\":\"ok\",\"rc\":%d}\n", rc);
   fflush(stdout);
   return 1;
}

/* =============================================================================
 * Snapshot save / load (Phase 2.1).
 *
 * Caches the post-extraction memory state (facts, entities, relations,
 * summaries, preferences, conversations, messages) plus the in-process
 * dia_id<->msg_id map.  Lets a re-run skip the expensive extraction LLM calls
 * and jump straight to query_memory.
 *
 * snapshot_save  {db_path, map_path}  → VACUUM INTO db_path; write map_path JSON
 * snapshot_load  {db_path, map_path}  → shutdown DB, copy db_path over bench
 *                                       tmpfile, re-init, load map_path JSON
 *
 * The orchestrator (Python) is responsible for choosing the cache key and
 * paths so we don't have to plumb hashing into C.  We just save/restore.
 * ============================================================================= */

static int file_exists(const char *path) {
   struct stat st;
   return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* messages table has no user_id column; count via join on conversations. */
static int count_messages_for_user(int user_id) {
   const char *sql = "SELECT COUNT(*) FROM messages m "
                     "JOIN conversations c ON c.id = m.conversation_id "
                     "WHERE c.user_id = ?";
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return 0;
   sqlite3_bind_int(stmt, 1, user_id);
   int count = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW)
      count = sqlite3_column_int(stmt, 0);
   sqlite3_finalize(stmt);
   return count;
}

/* Counts per BENCH_MP_USER_ID.  Used in snapshot responses for the orchestrator
 * to verify a cache load actually populated the expected tables. */
static void counts_for_response(int *facts_out, int *entities_out, int *convs_out, int *msgs_out) {
   if (facts_out)
      *facts_out = count_table_for_user("memory_facts", BENCH_MP_USER_ID);
   if (entities_out)
      *entities_out = count_table_for_user("memory_entities", BENCH_MP_USER_ID);
   if (convs_out)
      *convs_out = count_table_for_user("conversations", BENCH_MP_USER_ID);
   if (msgs_out)
      *msgs_out = count_messages_for_user(BENCH_MP_USER_ID);
}

/* Copy file via streaming read/write.  Used by snapshot_load to overwrite the
 * bench tmpfile with the cached snapshot before re-initializing auth_db.
 * Returns 0 on success, 1 on failure. */
static int copy_file(const char *src, const char *dst) {
   FILE *in = fopen(src, "rb");
   if (!in)
      return 1;
   FILE *out = fopen(dst, "wb");
   if (!out) {
      fclose(in);
      return 1;
   }
   char buf[64 * 1024];
   size_t n;
   int rc = 0;
   while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
      if (fwrite(buf, 1, n, out) != n) {
         rc = 1;
         break;
      }
   }
   if (ferror(in))
      rc = 1;
   fclose(in);
   if (fclose(out) != 0)
      rc = 1;
   return rc;
}

static int handle_snapshot_save(struct json_object *cmd) {
   struct json_object *db_obj = NULL, *map_obj = NULL;
   if (!json_object_object_get_ex(cmd, "db_path", &db_obj) ||
       !json_object_object_get_ex(cmd, "map_path", &map_obj)) {
      respond_error("snapshot_save: missing db_path/map_path");
      return 1;
   }
   const char *db_path = json_object_get_string(db_obj);
   const char *map_path = json_object_get_string(map_obj);
   if (!db_path || !*db_path || !map_path || !*map_path) {
      respond_error("snapshot_save: empty db_path/map_path");
      return 1;
   }

   /* VACUUM INTO is atomic and produces a clean single-file DB (no -wal/-shm
    * sidecars).  Force overwrite by unlinking first; SQLite returns
    * SQLITE_ERROR if the destination exists. */
   unlink(db_path);

   /* SQL-quote the path: bind args aren't allowed in VACUUM INTO. */
   sqlite3_stmt *stmt = NULL;
   const char *sql = "VACUUM INTO ?";
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      respond_error("snapshot_save: prepare VACUUM INTO failed");
      return 1;
   }
   sqlite3_bind_text(stmt, 1, db_path, -1, SQLITE_STATIC);
   int step_rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   if (step_rc != SQLITE_DONE) {
      char err[256];
      snprintf(err, sizeof(err), "snapshot_save: VACUUM INTO failed: %s", sqlite3_errmsg(s_db.db));
      respond_error(err);
      return 1;
   }

   /* Persist dia_map alongside.  JSON: [{dia_id, msg_id}, ...] */
   struct json_object *arr = json_object_new_array();
   for (int i = 0; i < s_dia_map_count; i++) {
      struct json_object *e = json_object_new_object();
      json_object_object_add(e, "dia_id", json_object_new_string(s_dia_map[i].dia_id));
      json_object_object_add(e, "msg_id", json_object_new_int64(s_dia_map[i].msg_id));
      json_object_array_add(arr, e);
   }
   int wrote = json_object_to_file_ext(map_path, arr, JSON_C_TO_STRING_PLAIN);
   json_object_put(arr);
   if (wrote != 0) {
      unlink(db_path);
      respond_error("snapshot_save: write map_path failed");
      return 1;
   }

   int facts = 0, entities = 0, convs = 0, msgs = 0;
   counts_for_response(&facts, &entities, &convs, &msgs);
   fprintf(stdout,
           "{\"status\":\"ok\",\"facts\":%d,\"entities\":%d,\"conversations\":%d,"
           "\"messages\":%d,\"dia_count\":%d}\n",
           facts, entities, convs, msgs, s_dia_map_count);
   fflush(stdout);
   return 1;
}

static int handle_snapshot_load(struct json_object *cmd) {
   struct json_object *db_obj = NULL, *map_obj = NULL;
   if (!json_object_object_get_ex(cmd, "db_path", &db_obj) ||
       !json_object_object_get_ex(cmd, "map_path", &map_obj)) {
      respond_error("snapshot_load: missing db_path/map_path");
      return 1;
   }
   const char *db_path = json_object_get_string(db_obj);
   const char *map_path = json_object_get_string(map_obj);
   if (!db_path || !*db_path || !map_path || !*map_path) {
      respond_error("snapshot_load: empty db_path/map_path");
      return 1;
   }
   if (!file_exists(db_path) || !file_exists(map_path)) {
      respond_error("snapshot_load: snapshot files not found");
      return 1;
   }

   /* Tear down current DB cleanly (finalizes all 43 prepared statements,
    * checkpoints WAL, closes the handle).  s_db.initialized goes false. */
   auth_db_shutdown();

   /* Replace the bench tmpfile with the cached snapshot. */
   unlink(s_bench_db_path);
   if (copy_file(db_path, s_bench_db_path) != 0) {
      respond_error("snapshot_load: copy snapshot failed");
      return 1;
   }

   /* Re-init auth_db.  This restores all prepared statements pointing at the
    * loaded data and re-applies any forward schema migrations (the cached DB
    * may be from an older bench commit). */
   if (auth_db_init(s_bench_db_path) != AUTH_DB_SUCCESS) {
      respond_error("snapshot_load: auth_db_init failed");
      return 1;
   }

   /* Restore dia_map. */
   dia_map_clear();
   struct json_object *arr = json_object_from_file(map_path);
   if (!arr || !json_object_is_type(arr, json_type_array)) {
      if (arr)
         json_object_put(arr);
      respond_error("snapshot_load: parse map_path failed");
      return 1;
   }
   size_t n = json_object_array_length(arr);
   for (size_t i = 0; i < n; i++) {
      struct json_object *e = json_object_array_get_idx(arr, i);
      struct json_object *did = NULL, *mid = NULL;
      if (!json_object_object_get_ex(e, "dia_id", &did) ||
          !json_object_object_get_ex(e, "msg_id", &mid))
         continue;
      const char *dia_id = json_object_get_string(did);
      int64_t msg_id = json_object_get_int64(mid);
      if (dia_id && *dia_id)
         dia_map_add(dia_id, msg_id);
   }
   json_object_put(arr);

   int facts = 0, entities = 0, convs = 0, msgs = 0;
   counts_for_response(&facts, &entities, &convs, &msgs);
   fprintf(stdout,
           "{\"status\":\"ok\",\"facts\":%d,\"entities\":%d,\"conversations\":%d,"
           "\"messages\":%d,\"dia_count\":%d}\n",
           facts, entities, convs, msgs, s_dia_map_count);
   fflush(stdout);
   return 1;
}

int bench_mp_dispatch(struct json_object *cmd) {
   struct json_object *cmd_obj = NULL;
   if (!json_object_object_get_ex(cmd, "cmd", &cmd_obj))
      return 0;
   const char *cmd_str = json_object_get_string(cmd_obj);
   if (!cmd_str)
      return 0;

   if (strcmp(cmd_str, "conv_create") == 0)
      return handle_conv_create(cmd);
   if (strcmp(cmd_str, "add_message") == 0)
      return handle_add_message(cmd);
   if (strcmp(cmd_str, "extract") == 0)
      return handle_extract(cmd);
   if (strcmp(cmd_str, "query_memory") == 0)
      return handle_query_memory(cmd);
   if (strcmp(cmd_str, "reset_memory") == 0)
      return handle_reset_memory(cmd);
   if (strcmp(cmd_str, "snapshot_save") == 0)
      return handle_snapshot_save(cmd);
   if (strcmp(cmd_str, "snapshot_load") == 0)
      return handle_snapshot_load(cmd);

   return 0;
}
