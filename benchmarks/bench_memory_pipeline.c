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

#include <fcntl.h>
#include <inttypes.h>
#include <json-c/json.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "bench_memory_schema.h"
#include "config/dawn_config.h"
#include "memory/memory_db.h"
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
      char *fact_text_esc = json_object_to_json_string(json_object_new_string(facts[i].fact_text));
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
