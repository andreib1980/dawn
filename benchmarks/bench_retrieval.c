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
 * Retrieval Benchmark Binary
 *
 * Standalone tool that exercises DAWN's embedding engine and document
 * search scoring via a JSON-lines protocol on stdin/stdout. Used by
 * the Python benchmark orchestrator (run_benchmark.py) to evaluate
 * retrieval quality on LongMemEval, LoCoMo, and ConvoMem datasets.
 *
 * Usage:
 *   bench_retrieval --provider onnx
 *   bench_retrieval --provider ollama --model all-minilm --endpoint http://localhost:11434
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <ctype.h>
#include <getopt.h>
#include <json-c/json.h>
#include <math.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db_internal.h"
#include "config/dawn_config.h"
#include "core/embedding_engine.h"
#include "tools/document_db.h"
#include "tools/time_query_parser.h"

/* =============================================================================
 * Constants
 * ============================================================================= */

#define BENCH_USER_ID 1
#define BENCH_MAX_CHUNKS 10000
#define BENCH_KEYWORD_BOOST 0.15f
#define BENCH_MAX_LINE 1048576 /* 1 MB max JSON line */

/* =============================================================================
 * Extern Globals (defined in bench_retrieval_stub.c)
 * ============================================================================= */

extern dawn_config_t g_config;
extern secrets_config_t g_secrets;
extern auth_db_state_t s_db;

/* When true, skip keyword boosting (pure cosine baseline) */
static bool s_no_keyword_boost = false;

/* Temporal-boost weight (v35).  When the query contains a parsed temporal
 * expression and a chunk has a non-zero created_at, the chunk's score gets
 * `s_temporal_weight * proximity` added.  proximity is Gaussian decay in
 * [0,1] from time_query_parser.  0 disables the boost entirely. */
static float s_temporal_weight = 0.0f;

/* When set, anchors "yesterday"/"last week"/etc to this timestamp (Unix sec)
 * instead of wall-clock now().  Useful for replaying historical datasets where
 * queries' "now" should match the dataset's era, not the benchmark run time. */
static int64_t s_now_override = 0;

/* When set, restrict query results to chunks whose doc has this category (v34).
 * Empty = no filter (default).  Categories are supplied per-add via the optional
 * "category" field in the add command. */
#define BENCH_MAX_DOC_CATEGORIES 32
static char s_category_filter[32] = "";
static struct {
   int64_t doc_id;
   char category[32];
} s_doc_categories[BENCH_MAX_CHUNKS];
static int s_doc_categories_count = 0;

static void doc_category_set(int64_t doc_id, const char *category) {
   if (s_doc_categories_count >= BENCH_MAX_CHUNKS)
      return;
   s_doc_categories[s_doc_categories_count].doc_id = doc_id;
   snprintf(s_doc_categories[s_doc_categories_count].category, BENCH_MAX_DOC_CATEGORIES, "%s",
            category ? category : "");
   s_doc_categories_count++;
}

static const char *doc_category_get(int64_t doc_id) {
   for (int i = 0; i < s_doc_categories_count; i++) {
      if (s_doc_categories[i].doc_id == doc_id)
         return s_doc_categories[i].category;
   }
   return "";
}

/* =============================================================================
 * Database Setup (mirrors tests/test_document_db.c)
 * ============================================================================= */

/* clang-format off */
static const char *DDL =
   "CREATE TABLE IF NOT EXISTS users ("
   "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
   "  username TEXT UNIQUE NOT NULL"
   ");"
   "INSERT INTO users (id, username) VALUES (1, 'benchmark');"
   "CREATE TABLE IF NOT EXISTS documents ("
   "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
   "  user_id INTEGER,"
   "  filename TEXT NOT NULL,"
   "  filepath TEXT NOT NULL,"
   "  filetype TEXT NOT NULL,"
   "  file_hash TEXT NOT NULL,"
   "  num_chunks INTEGER NOT NULL,"
   "  is_global INTEGER DEFAULT 0,"
   "  created_at INTEGER NOT NULL,"
   "  FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE"
   ");"
   "CREATE TABLE IF NOT EXISTS document_chunks ("
   "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
   "  document_id INTEGER NOT NULL,"
   "  chunk_index INTEGER NOT NULL,"
   "  text TEXT NOT NULL,"
   "  embedding BLOB NOT NULL,"
   "  embedding_norm REAL NOT NULL,"
   "  created_at INTEGER NOT NULL DEFAULT 0,"
   "  FOREIGN KEY(document_id) REFERENCES documents(id) ON DELETE CASCADE"
   ");";
/* clang-format on */

static int prepare_statements(void) {
   int rc;

   rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO documents (user_id, filename, filepath, filetype, file_hash, "
       "num_chunks, is_global, created_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
       -1, &s_db.stmt_doc_create, NULL);
   if (rc != SQLITE_OK)
      return -1;

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT id, user_id, filename, filepath, filetype, file_hash, "
                           "num_chunks, is_global, created_at FROM documents WHERE id = ?",
                           -1, &s_db.stmt_doc_get, NULL);
   if (rc != SQLITE_OK)
      return -1;

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT id FROM documents WHERE file_hash = ? "
                           "AND (user_id = ? OR is_global = 1)",
                           -1, &s_db.stmt_doc_get_by_hash, NULL);
   if (rc != SQLITE_OK)
      return -1;

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT id, user_id, filename, filepath, filetype, file_hash, "
                           "num_chunks, is_global, created_at FROM documents "
                           "WHERE user_id = ? OR is_global = 1 ORDER BY created_at DESC "
                           "LIMIT ? OFFSET ?",
                           -1, &s_db.stmt_doc_list, NULL);
   if (rc != SQLITE_OK)
      return -1;

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT d.id, d.user_id, d.filename, d.filepath, d.filetype, "
                           "d.file_hash, d.num_chunks, d.is_global, d.created_at, "
                           "COALESCE(u.username, '') FROM documents d "
                           "LEFT JOIN users u ON d.user_id = u.id "
                           "ORDER BY d.created_at DESC LIMIT ? OFFSET ?",
                           -1, &s_db.stmt_doc_list_all, NULL);
   if (rc != SQLITE_OK)
      return -1;

   rc = sqlite3_prepare_v2(s_db.db, "UPDATE documents SET is_global = ? WHERE id = ?", -1,
                           &s_db.stmt_doc_update_global, NULL);
   if (rc != SQLITE_OK)
      return -1;

   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM documents WHERE id = ?", -1, &s_db.stmt_doc_delete,
                           NULL);
   if (rc != SQLITE_OK)
      return -1;

   rc = sqlite3_prepare_v2(s_db.db, "SELECT COUNT(*) FROM documents WHERE user_id = ?", -1,
                           &s_db.stmt_doc_count_user, NULL);
   if (rc != SQLITE_OK)
      return -1;

   /* Bench mirrors auth_db_core.c statements — must include v35 created_at column. */
   rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO document_chunks (document_id, chunk_index, text, embedding, "
       "embedding_norm, created_at) VALUES (?, ?, ?, ?, ?, ?)",
       -1, &s_db.stmt_doc_chunk_create, NULL);
   if (rc != SQLITE_OK)
      return -1;

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT c.id, c.chunk_index, c.text, c.embedding, c.embedding_norm, "
                           "d.id, d.filename, d.filetype, c.created_at "
                           "FROM document_chunks c JOIN documents d ON c.document_id = d.id "
                           "WHERE d.user_id = ? OR d.is_global = 1 "
                           "LIMIT ?",
                           -1, &s_db.stmt_doc_chunk_search, NULL);
   if (rc != SQLITE_OK)
      return -1;

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT id, user_id, filename, filepath, filetype, file_hash, "
                           "num_chunks, is_global, created_at "
                           "FROM documents "
                           "WHERE (user_id = ? OR is_global = 1) "
                           "AND filename LIKE ? ESCAPE '\\' COLLATE NOCASE "
                           "ORDER BY CASE WHEN LOWER(filename) = LOWER(?) "
                           "THEN 0 ELSE 1 END, created_at DESC LIMIT 1",
                           -1, &s_db.stmt_doc_find_by_name, NULL);
   if (rc != SQLITE_OK)
      return -1;

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT chunk_index, text FROM document_chunks "
                           "WHERE document_id = ? ORDER BY chunk_index LIMIT ? OFFSET ?",
                           -1, &s_db.stmt_doc_chunk_read, NULL);
   if (rc != SQLITE_OK)
      return -1;

   return 0;
}

static void setup_db(void) {
   int rc = sqlite3_open(":memory:", &s_db.db);
   if (rc != SQLITE_OK) {
      fprintf(stderr, "Failed to open in-memory DB: %s\n", sqlite3_errmsg(s_db.db));
      exit(1);
   }

   sqlite3_exec(s_db.db, "PRAGMA foreign_keys = ON", NULL, NULL, NULL);

   char *errmsg = NULL;
   rc = sqlite3_exec(s_db.db, DDL, NULL, NULL, &errmsg);
   if (rc != SQLITE_OK) {
      fprintf(stderr, "DDL failed: %s\n", errmsg);
      sqlite3_free(errmsg);
      exit(1);
   }

   if (prepare_statements() != 0) {
      fprintf(stderr, "Prepare statements failed: %s\n", sqlite3_errmsg(s_db.db));
      exit(1);
   }

   s_db.initialized = true;
}

static void teardown_db(void) {
   s_db.initialized = false;

   if (s_db.stmt_doc_create)
      sqlite3_finalize(s_db.stmt_doc_create);
   if (s_db.stmt_doc_get)
      sqlite3_finalize(s_db.stmt_doc_get);
   if (s_db.stmt_doc_get_by_hash)
      sqlite3_finalize(s_db.stmt_doc_get_by_hash);
   if (s_db.stmt_doc_list)
      sqlite3_finalize(s_db.stmt_doc_list);
   if (s_db.stmt_doc_list_all)
      sqlite3_finalize(s_db.stmt_doc_list_all);
   if (s_db.stmt_doc_update_global)
      sqlite3_finalize(s_db.stmt_doc_update_global);
   if (s_db.stmt_doc_delete)
      sqlite3_finalize(s_db.stmt_doc_delete);
   if (s_db.stmt_doc_count_user)
      sqlite3_finalize(s_db.stmt_doc_count_user);
   if (s_db.stmt_doc_chunk_create)
      sqlite3_finalize(s_db.stmt_doc_chunk_create);
   if (s_db.stmt_doc_chunk_search)
      sqlite3_finalize(s_db.stmt_doc_chunk_search);
   if (s_db.stmt_doc_find_by_name)
      sqlite3_finalize(s_db.stmt_doc_find_by_name);
   if (s_db.stmt_doc_chunk_read)
      sqlite3_finalize(s_db.stmt_doc_chunk_read);

   if (s_db.db) {
      sqlite3_close(s_db.db);
      s_db.db = NULL;
   }
}

/* =============================================================================
 * Keyword Scoring (from src/tools/document_search.c)
 * ============================================================================= */

static bool contains_keyword(const char *text, const char *keyword, int keyword_len) {
   if (!text || !keyword || keyword_len <= 0)
      return false;

   for (const char *p = text; *p; p++) {
      if (tolower((unsigned char)*p) == tolower((unsigned char)keyword[0])) {
         bool match = true;
         int matched = 1;
         for (int i = 1; i < keyword_len && p[i]; i++) {
            if (tolower((unsigned char)p[i]) != tolower((unsigned char)keyword[i])) {
               match = false;
               break;
            }
            matched++;
         }
         if (match && matched == keyword_len &&
             (p[keyword_len] == '\0' || !isalnum((unsigned char)p[keyword_len])))
            return true;
      }
   }
   return false;
}

typedef struct {
   const char *words[32];
   int lengths[32];
   int count;
} query_words_t;

static void tokenize_query(const char *query, query_words_t *qw) {
   qw->count = 0;
   const char *p = query;

   while (*p && qw->count < 32) {
      while (*p && !isalnum((unsigned char)*p))
         p++;
      if (!*p)
         break;
      const char *start = p;
      while (*p && isalnum((unsigned char)*p))
         p++;
      int len = (int)(p - start);
      if (len >= 3) {
         qw->words[qw->count] = start;
         qw->lengths[qw->count] = len;
         qw->count++;
      }
   }
}

static float keyword_score(const char *text, const query_words_t *qw) {
   if (qw->count == 0)
      return 0.0f;

   int matches = 0;
   for (int i = 0; i < qw->count; i++) {
      if (contains_keyword(text, qw->words[i], qw->lengths[i]))
         matches++;
   }
   return (float)matches / (float)qw->count;
}

/* =============================================================================
 * Scoring Helpers
 * ============================================================================= */

typedef struct {
   int index;
   float score;
} scored_chunk_t;

static int score_compare(const void *a, const void *b) {
   float sa = ((const scored_chunk_t *)a)->score;
   float sb = ((const scored_chunk_t *)b)->score;
   if (sb > sa)
      return 1;
   if (sb < sa)
      return -1;
   return 0;
}

/* =============================================================================
 * Command: add
 * ============================================================================= */

static uint64_t s_add_counter = 0;

static int handle_add(struct json_object *cmd) {
   struct json_object *id_obj = NULL;
   struct json_object *text_obj = NULL;

   if (!json_object_object_get_ex(cmd, "id", &id_obj) ||
       !json_object_object_get_ex(cmd, "text", &text_obj)) {
      fprintf(stdout, "{\"status\":\"error\",\"message\":\"missing id or text\"}\n");
      fflush(stdout);
      return -1;
   }

   const char *id = json_object_get_string(id_obj);
   const char *text = json_object_get_string(text_obj);
   if (!id || !text) {
      fprintf(stdout, "{\"status\":\"error\",\"message\":\"null id or text\"}\n");
      fflush(stdout);
      return -1;
   }

   int dims = embedding_engine_dims();
   float *embedding = malloc((size_t)dims * sizeof(float));
   if (!embedding) {
      fprintf(stdout, "{\"status\":\"error\",\"message\":\"alloc failed\"}\n");
      fflush(stdout);
      return -1;
   }

   int out_dims = 0;
   if (embedding_engine_embed(text, embedding, dims, &out_dims) != 0 || out_dims != dims) {
      free(embedding);
      fprintf(stdout, "{\"status\":\"error\",\"message\":\"embedding failed\"}\n");
      fflush(stdout);
      return -1;
   }

   float norm = embedding_engine_l2_norm(embedding, out_dims);

   /* Unique hash from counter */
   char hash[DOC_HASH_MAX];
   snprintf(hash, sizeof(hash), "%064llx", (unsigned long long)++s_add_counter);

   /* Truncate text if needed for chunk storage */
   char chunk_text[DOC_CHUNK_TEXT_MAX];
   snprintf(chunk_text, sizeof(chunk_text), "%s", text);

   int64_t doc_id = document_db_create(BENCH_USER_ID, id, id, "bench", hash, 1, false);
   if (doc_id < 0) {
      free(embedding);
      fprintf(stdout, "{\"status\":\"error\",\"message\":\"db create failed\"}\n");
      fflush(stdout);
      return -1;
   }

   /* Optional per-add category for filtered-retrieval benchmarks (v34) */
   struct json_object *cat_obj = NULL;
   if (json_object_object_get_ex(cmd, "category", &cat_obj)) {
      const char *cat = json_object_get_string(cat_obj);
      if (cat && *cat)
         doc_category_set(doc_id, cat);
   }

   /* Optional per-add created_at for temporal-scoring benchmarks (v35).
    * Orchestrator passes session timestamps for LoCoMo. 0 = unknown. */
   int64_t created_at = 0;
   struct json_object *ts_obj = NULL;
   if (json_object_object_get_ex(cmd, "created_at", &ts_obj)) {
      created_at = (int64_t)json_object_get_int64(ts_obj);
   }

   int64_t chunk_id = document_db_chunk_create(doc_id, 0, chunk_text, embedding, out_dims, norm,
                                               created_at);
   free(embedding);

   if (chunk_id < 0) {
      fprintf(stdout, "{\"status\":\"error\",\"message\":\"chunk create failed\"}\n");
      fflush(stdout);
      return -1;
   }

   fprintf(stdout, "{\"status\":\"ok\",\"doc_id\":%lld}\n", (long long)doc_id);
   fflush(stdout);
   return 0;
}

/* =============================================================================
 * Command: query
 * ============================================================================= */

static int handle_query(struct json_object *cmd) {
   struct json_object *text_obj = NULL;
   struct json_object *topk_obj = NULL;

   if (!json_object_object_get_ex(cmd, "text", &text_obj)) {
      fprintf(stdout, "{\"status\":\"error\",\"message\":\"missing text\"}\n");
      fflush(stdout);
      return -1;
   }

   const char *query_text = json_object_get_string(text_obj);
   int top_k = 10;
   if (json_object_object_get_ex(cmd, "top_k", &topk_obj))
      top_k = json_object_get_int(topk_obj);

   int dims = embedding_engine_dims();

   /* Embed the query */
   float *query_vec = malloc((size_t)dims * sizeof(float));
   if (!query_vec) {
      fprintf(stdout, "{\"status\":\"error\",\"message\":\"alloc failed\"}\n");
      fflush(stdout);
      return -1;
   }

   int out_dims = 0;
   if (embedding_engine_embed(query_text, query_vec, dims, &out_dims) != 0) {
      free(query_vec);
      fprintf(stdout, "{\"status\":\"error\",\"message\":\"query embed failed\"}\n");
      fflush(stdout);
      return -1;
   }
   float query_norm = embedding_engine_l2_norm(query_vec, dims);

   /* Load all chunks */
   int max_chunks = BENCH_MAX_CHUNKS;
   document_chunk_t *chunks = calloc((size_t)max_chunks, sizeof(document_chunk_t));
   float *emb_buf = malloc((size_t)max_chunks * (size_t)dims * sizeof(float));
   if (!chunks || !emb_buf) {
      free(query_vec);
      free(chunks);
      free(emb_buf);
      fprintf(stdout, "{\"status\":\"error\",\"message\":\"alloc failed\"}\n");
      fflush(stdout);
      return -1;
   }

   int chunk_count = document_db_chunk_search_load(BENCH_USER_ID, chunks, emb_buf, dims,
                                                   max_chunks);
   if (chunk_count <= 0) {
      free(query_vec);
      free(chunks);
      free(emb_buf);
      fprintf(stdout, "{\"results\":[]}\n");
      fflush(stdout);
      return 0;
   }

   /* Score all chunks — cosine similarity */
   scored_chunk_t *scores = malloc((size_t)chunk_count * sizeof(scored_chunk_t));
   if (!scores) {
      free(query_vec);
      free(chunks);
      free(emb_buf);
      fprintf(stdout, "{\"status\":\"error\",\"message\":\"alloc failed\"}\n");
      fflush(stdout);
      return -1;
   }

   /* Per-query category override falls back to CLI-set s_category_filter. */
   const char *q_category_filter = s_category_filter;
   struct json_object *qcat_obj = NULL;
   if (json_object_object_get_ex(cmd, "category", &qcat_obj)) {
      const char *q = json_object_get_string(qcat_obj);
      if (q && *q)
         q_category_filter = q;
   }

   /* Parse the query for temporal expressions ONCE.  When --temporal-weight is
    * 0 (default), we skip the work entirely so baseline runs are unaffected.
    * Per-call "now" override (cmd.now) takes precedence over CLI --now;
    * LongMemEval needs this because each question has its own question_date. */
   time_query_t tq = { 0 };
   if (s_temporal_weight > 0.0f) {
      int64_t now_ts = (int64_t)time(NULL);
      if (s_now_override > 0)
         now_ts = s_now_override;
      struct json_object *now_obj = NULL;
      if (json_object_object_get_ex(cmd, "now", &now_obj)) {
         int64_t per_call = (int64_t)json_object_get_int64(now_obj);
         if (per_call > 0)
            now_ts = per_call;
      }
      time_query_parse(query_text, now_ts, &tq);
   }

   for (int i = 0; i < chunk_count; i++) {
      /* Pre-filter by category when set: any chunk whose doc has a different
       * category gets a sentinel score so it sorts to the bottom and never
       * appears in top_k.  Same-category and unlabeled docs proceed normally. */
      if (q_category_filter && *q_category_filter) {
         const char *chunk_cat = doc_category_get(chunks[i].document_id);
         if (chunk_cat[0] && strcmp(chunk_cat, q_category_filter) != 0) {
            scores[i].index = i;
            scores[i].score = -1.0f;
            continue;
         }
      }

      float cosine = embedding_engine_cosine_with_norms(query_vec, chunks[i].embedding, dims,
                                                        query_norm, chunks[i].embedding_norm);

      /* Additive temporal boost: chunks dated near the query's referenced point
       * get a bump.  Additive (not multiplicative) so a chunk with great cosine
       * but no timestamp doesn't get penalized — it just doesn't get the bonus. */
      if (tq.found && chunks[i].created_at > 0) {
         cosine += s_temporal_weight * time_query_proximity(&tq, chunks[i].created_at);
      }

      scores[i].index = i;
      scores[i].score = cosine;
   }

   free(query_vec);

   /* Sort by cosine descending */
   qsort(scores, (size_t)chunk_count, sizeof(scored_chunk_t), score_compare);

   /* Apply keyword boosting to top 50 (skip in raw mode) */
   if (!s_no_keyword_boost) {
      query_words_t qw;
      tokenize_query(query_text, &qw);

      int top_n = chunk_count < 50 ? chunk_count : 50;
      for (int i = 0; i < top_n; i++) {
         float kw = keyword_score(chunks[scores[i].index].text, &qw);
         scores[i].score += kw * BENCH_KEYWORD_BOOST;
      }

      /* Re-sort top candidates */
      qsort(scores, (size_t)top_n, sizeof(scored_chunk_t), score_compare);
   }

   /* Build JSON result array */
   int result_count = top_k < chunk_count ? top_k : chunk_count;
   struct json_object *results_arr = json_object_new_array();

   for (int i = 0; i < result_count; i++) {
      struct json_object *entry = json_object_new_object();
      json_object_object_add(entry, "id",
                             json_object_new_string(chunks[scores[i].index].doc_filename));
      json_object_object_add(entry, "score", json_object_new_double((double)scores[i].score));
      json_object_object_add(entry, "rank", json_object_new_int(i + 1));
      json_object_array_add(results_arr, entry);
   }

   struct json_object *response = json_object_new_object();
   json_object_object_add(response, "results", results_arr);

   fprintf(stdout, "%s\n", json_object_to_json_string_ext(response, JSON_C_TO_STRING_PLAIN));
   fflush(stdout);

   json_object_put(response);
   free(scores);
   free(chunks);
   free(emb_buf);
   return 0;
}

/* =============================================================================
 * Command: reset
 * ============================================================================= */

static int handle_reset(void) {
   pthread_mutex_lock(&s_db.mutex);
   sqlite3_exec(s_db.db, "DELETE FROM document_chunks", NULL, NULL, NULL);
   sqlite3_exec(s_db.db, "DELETE FROM documents", NULL, NULL, NULL);
   pthread_mutex_unlock(&s_db.mutex);

   s_add_counter = 0;
   s_doc_categories_count = 0;

   fprintf(stdout, "{\"status\":\"ok\"}\n");
   fflush(stdout);
   return 0;
}

/* =============================================================================
 * Command Loop
 * ============================================================================= */

static void command_loop(void) {
   char *line = malloc(BENCH_MAX_LINE);
   if (!line) {
      fprintf(stderr, "Failed to allocate line buffer\n");
      return;
   }

   while (fgets(line, BENCH_MAX_LINE, stdin)) {
      /* Strip trailing newline */
      size_t len = strlen(line);
      while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
         line[--len] = '\0';

      if (len == 0)
         continue;

      struct json_object *cmd = json_tokener_parse(line);
      if (!cmd) {
         fprintf(stdout, "{\"status\":\"error\",\"message\":\"invalid JSON\"}\n");
         fflush(stdout);
         continue;
      }

      struct json_object *cmd_obj = NULL;
      if (!json_object_object_get_ex(cmd, "cmd", &cmd_obj)) {
         fprintf(stdout, "{\"status\":\"error\",\"message\":\"missing cmd field\"}\n");
         fflush(stdout);
         json_object_put(cmd);
         continue;
      }

      const char *cmd_str = json_object_get_string(cmd_obj);

      if (strcmp(cmd_str, "add") == 0) {
         handle_add(cmd);
      } else if (strcmp(cmd_str, "query") == 0) {
         handle_query(cmd);
      } else if (strcmp(cmd_str, "reset") == 0) {
         handle_reset();
      } else if (strcmp(cmd_str, "quit") == 0) {
         json_object_put(cmd);
         break;
      } else {
         fprintf(stdout, "{\"status\":\"error\",\"message\":\"unknown cmd: %s\"}\n", cmd_str);
         fflush(stdout);
      }

      json_object_put(cmd);
   }

   free(line);
}

/* =============================================================================
 * CLI Parsing
 * ============================================================================= */

static void print_usage(const char *prog) {
   fprintf(stderr,
           "Usage: %s [options]\n"
           "  --provider <onnx|ollama|openai>  Embedding provider (default: onnx)\n"
           "  --model <name>                   Model name for HTTP providers\n"
           "  --endpoint <url>                 Endpoint URL for HTTP providers\n"
           "  --api-key <key>                  API key for OpenAI provider\n"
           "  --no-keyword-boost               Disable keyword boosting (raw cosine only)\n"
           "  --category-filter <name>         Restrict queries to chunks whose doc has\n"
           "                                   this category (set per-add via JSON 'category'\n"
           "                                   field). Per-query 'category' overrides this.\n"
           "  --temporal-weight <float>        Temporal-boost weight (0 = disabled, default).\n"
           "                                   Adds w * proximity to chunk cosine when query\n"
           "                                   contains a parsed temporal expression and the\n"
           "                                   chunk has a non-zero created_at. Try 0.10–0.30.\n"
           "  --now <unix_ts>                  Pin 'now' for relative expressions (yesterday,\n"
           "                                   last week). Useful when replaying historical\n"
           "                                   datasets — defaults to wall-clock time().\n"
           "  --help                           Show this help\n",
           prog);
}

int main(int argc, char *argv[]) {
   const char *provider = "onnx";
   const char *model = "";
   const char *endpoint = "";
   const char *api_key = "";

   static struct option long_options[] = {
      { "provider", required_argument, 0, 'p' },
      { "model", required_argument, 0, 'm' },
      { "endpoint", required_argument, 0, 'e' },
      { "api-key", required_argument, 0, 'k' },
      { "no-keyword-boost", no_argument, 0, 'r' },
      { "category-filter", required_argument, 0, 'c' },
      { "temporal-weight", required_argument, 0, 't' },
      { "now", required_argument, 0, 'n' },
      { "help", no_argument, 0, 'h' },
      { 0, 0, 0, 0 },
   };

   int opt;
   while ((opt = getopt_long(argc, argv, "p:m:e:k:c:t:n:rh", long_options, NULL)) != -1) {
      switch (opt) {
         case 'p':
            provider = optarg;
            break;
         case 'm':
            model = optarg;
            break;
         case 'e':
            endpoint = optarg;
            break;
         case 'k':
            api_key = optarg;
            break;
         case 'r':
            s_no_keyword_boost = true;
            break;
         case 'c':
            snprintf(s_category_filter, sizeof(s_category_filter), "%s", optarg);
            break;
         case 't':
            s_temporal_weight = (float)atof(optarg);
            break;
         case 'n':
            s_now_override = (int64_t)atoll(optarg);
            break;
         case 'h':
            print_usage(argv[0]);
            return 0;
         default:
            print_usage(argv[0]);
            return 1;
      }
   }

   /* Populate config for embedding engine */
   memset(&g_config, 0, sizeof(g_config));
   memset(&g_secrets, 0, sizeof(g_secrets));

   snprintf(g_config.memory.embedding_provider, sizeof(g_config.memory.embedding_provider), "%s",
            provider);
   snprintf(g_config.memory.embedding_model, sizeof(g_config.memory.embedding_model), "%s", model);
   snprintf(g_config.memory.embedding_endpoint, sizeof(g_config.memory.embedding_endpoint), "%s",
            endpoint);
   snprintf(g_secrets.embedding_api_key, sizeof(g_secrets.embedding_api_key), "%s", api_key);

   /* Initialize database */
   setup_db();

   /* Initialize embedding engine */
   if (embedding_engine_init() != 0) {
      fprintf(stderr, "Failed to initialize embedding engine with provider '%s'\n", provider);
      teardown_db();
      return 1;
   }

   if (!embedding_engine_available()) {
      fprintf(stderr, "Embedding engine not available after init\n");
      teardown_db();
      return 1;
   }

   /* Signal readiness to orchestrator */
   fprintf(stdout,
           "{\"status\":\"ready\",\"dims\":%d,\"provider\":\"%s\","
           "\"mode\":\"%s\"}\n",
           embedding_engine_dims(), provider, s_no_keyword_boost ? "raw" : "hybrid");
   fflush(stdout);

   /* Process commands */
   command_loop();

   /* Cleanup */
   embedding_engine_cleanup();
   teardown_db();
   return 0;
}
