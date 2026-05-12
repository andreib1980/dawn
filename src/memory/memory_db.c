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
 * Memory Database Implementation
 *
 * CRUD operations for facts, preferences, and summaries.
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include "memory/memory_db.h"

#include <ctype.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db_internal.h"
#include "config/dawn_config.h"
#include "logging.h"
#include "memory/memory_embeddings.h"
#include "memory/memory_similarity.h"

/* Forward declarations for helpers used across sections */
static void populate_entity_from_row(sqlite3_stmt *stmt, memory_entity_t *entity);

/* Bind provenance columns to sequential params at positions (base), (base+1), (base+2).
 * Emits SQLITE_NULL for all three when prov is NULL or prov->conv_id <= 0. */
static void bind_provenance(sqlite3_stmt *stmt, int base, const memory_provenance_t *prov) {
   if (prov && prov->conv_id > 0) {
      sqlite3_bind_int64(stmt, base, prov->conv_id);
      sqlite3_bind_int64(stmt, base + 1, prov->msg_id_start);
      sqlite3_bind_int64(stmt, base + 2, prov->msg_id_end);
   } else {
      sqlite3_bind_null(stmt, base);
      sqlite3_bind_null(stmt, base + 1);
      sqlite3_bind_null(stmt, base + 2);
   }
}

/* Canonical fact category labels (v34).  Single source of truth referenced by
 * extraction allowlist (memory_extraction.c), tool enum_values (memory_tool.c),
 * and centroid seed table (memory_embeddings.c).  See memory_types.h. */
const char *const MEMORY_FACT_CATEGORIES[] = { "personal",    "professional", "relationships",
                                               "health",      "interests",    "practical",
                                               "preferences", "general",      NULL };
const int MEMORY_FACT_CATEGORY_COUNT = 8;

/* =============================================================================
 * Helper: Build LIKE pattern with wildcards
 * ============================================================================= */

static void build_like_pattern(const char *keywords, char *out_pattern, size_t max_len) {
   if (!keywords || !out_pattern || max_len < 4) {
      if (out_pattern && max_len > 0) {
         out_pattern[0] = '\0';
      }
      return;
   }

   /* Build pattern with escaped LIKE metacharacters (%, _, \) */
   size_t out_idx = 0;
   out_pattern[out_idx++] = '%';

   for (size_t i = 0; keywords[i] != '\0' && out_idx < max_len - 3; i++) {
      char c = keywords[i];
      /* Escape LIKE metacharacters with backslash */
      if (c == '%' || c == '_' || c == '\\') {
         if (out_idx < max_len - 4) {
            out_pattern[out_idx++] = '\\';
            out_pattern[out_idx++] = c;
         }
      } else {
         out_pattern[out_idx++] = c;
      }
   }

   out_pattern[out_idx++] = '%';
   out_pattern[out_idx] = '\0';
}

/* =============================================================================
 * Helper: Populate fact from statement row
 * ============================================================================= */

/* Reads from SELECTs ordered: id, user_id, fact_text, confidence, source,
 * created_at, last_accessed, access_count, superseded_by, category.
 * category appended last (col 9, v34) so existing column indices are preserved. */
static void populate_fact_from_row(sqlite3_stmt *stmt, memory_fact_t *fact) {
   fact->id = sqlite3_column_int64(stmt, 0);
   fact->user_id = sqlite3_column_int(stmt, 1);

   const char *text = (const char *)sqlite3_column_text(stmt, 2);
   if (text) {
      strncpy(fact->fact_text, text, MEMORY_FACT_TEXT_MAX - 1);
      fact->fact_text[MEMORY_FACT_TEXT_MAX - 1] = '\0';
   }

   fact->confidence = (float)sqlite3_column_double(stmt, 3);

   const char *source = (const char *)sqlite3_column_text(stmt, 4);
   if (source) {
      strncpy(fact->source, source, MEMORY_SOURCE_MAX - 1);
      fact->source[MEMORY_SOURCE_MAX - 1] = '\0';
   }

   fact->created_at = (time_t)sqlite3_column_int64(stmt, 5);
   fact->last_accessed = (time_t)sqlite3_column_int64(stmt, 6);
   fact->access_count = sqlite3_column_int(stmt, 7);
   fact->superseded_by = sqlite3_column_int64(stmt, 8);

   const char *category = (const char *)sqlite3_column_text(stmt, 9);
   if (category) {
      strncpy(fact->category, category, MEMORY_CATEGORY_MAX - 1);
      fact->category[MEMORY_CATEGORY_MAX - 1] = '\0';
   } else {
      strncpy(fact->category, "general", MEMORY_CATEGORY_MAX - 1);
      fact->category[MEMORY_CATEGORY_MAX - 1] = '\0';
   }
}

/* =============================================================================
 * Helper: Populate preference from statement row
 * ============================================================================= */

static void populate_pref_from_row(sqlite3_stmt *stmt, memory_preference_t *pref) {
   pref->id = sqlite3_column_int64(stmt, 0);
   pref->user_id = sqlite3_column_int(stmt, 1);

   const char *cat = (const char *)sqlite3_column_text(stmt, 2);
   if (cat) {
      strncpy(pref->category, cat, MEMORY_CATEGORY_MAX - 1);
      pref->category[MEMORY_CATEGORY_MAX - 1] = '\0';
   }

   const char *val = (const char *)sqlite3_column_text(stmt, 3);
   if (val) {
      strncpy(pref->value, val, MEMORY_PREF_VALUE_MAX - 1);
      pref->value[MEMORY_PREF_VALUE_MAX - 1] = '\0';
   }

   pref->confidence = (float)sqlite3_column_double(stmt, 4);

   const char *source = (const char *)sqlite3_column_text(stmt, 5);
   if (source) {
      strncpy(pref->source, source, MEMORY_SOURCE_MAX - 1);
      pref->source[MEMORY_SOURCE_MAX - 1] = '\0';
   }

   pref->created_at = (time_t)sqlite3_column_int64(stmt, 6);
   pref->updated_at = (time_t)sqlite3_column_int64(stmt, 7);
   pref->reinforcement_count = sqlite3_column_int(stmt, 8);
}

/* =============================================================================
 * Helper: Populate summary from statement row
 * ============================================================================= */

static void populate_summary_from_row(sqlite3_stmt *stmt, memory_summary_t *summary) {
   summary->id = sqlite3_column_int64(stmt, 0);
   summary->user_id = sqlite3_column_int(stmt, 1);

   const char *sid = (const char *)sqlite3_column_text(stmt, 2);
   if (sid) {
      strncpy(summary->session_id, sid, MEMORY_SESSION_ID_MAX - 1);
      summary->session_id[MEMORY_SESSION_ID_MAX - 1] = '\0';
   }

   const char *sum = (const char *)sqlite3_column_text(stmt, 3);
   if (sum) {
      strncpy(summary->summary, sum, MEMORY_SUMMARY_MAX - 1);
      summary->summary[MEMORY_SUMMARY_MAX - 1] = '\0';
   }

   const char *topics = (const char *)sqlite3_column_text(stmt, 4);
   if (topics) {
      strncpy(summary->topics, topics, MEMORY_TOPICS_MAX - 1);
      summary->topics[MEMORY_TOPICS_MAX - 1] = '\0';
   }

   const char *sentiment = (const char *)sqlite3_column_text(stmt, 5);
   if (sentiment) {
      strncpy(summary->sentiment, sentiment, MEMORY_SENTIMENT_MAX - 1);
      summary->sentiment[MEMORY_SENTIMENT_MAX - 1] = '\0';
   }

   summary->created_at = (time_t)sqlite3_column_int64(stmt, 6);
   summary->message_count = sqlite3_column_int(stmt, 7);
   summary->duration_seconds = sqlite3_column_int(stmt, 8);
   summary->consolidated = sqlite3_column_int(stmt, 9) != 0;
}

/* =============================================================================
 * Fact Operations
 * ============================================================================= */

int memory_db_fact_create_at(int user_id,
                             const char *fact_text,
                             float confidence,
                             const char *source,
                             const char *category,
                             const memory_provenance_t *prov,
                             int64_t created_at_override,
                             int64_t *id_out) {
   if (id_out)
      *id_out = 0;
   if (!fact_text || !source) {
      return MEMORY_DB_FAILURE;
   }

   /* category may be NULL — schema default 'general' applies via the column DEFAULT,
    * but we explicitly bind to keep the SQL pure (no default-fallback ambiguity
    * across SQLite versions). */
   const char *cat = (category && *category) ? category : "general";

   /* 0 sentinel = "use NOW()".  Callers that want extraction-time temporal
    * fidelity pass the source conversation's created_at; everyone else gets
    * the legacy time(NULL) behavior.  int64_t matches anchor_date /
    * valid_from / on-disk SQLite column type for consistency. */
   const int64_t created_at = (created_at_override > 0) ? created_at_override : (int64_t)time(NULL);

   /* Compute normalized hash for deduplication */
   uint32_t normalized_hash = memory_normalize_and_hash(fact_text);

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_memory_fact_create;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, fact_text, -1, SQLITE_STATIC);
   sqlite3_bind_double(stmt, 3, confidence);
   sqlite3_bind_text(stmt, 4, source, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 5, cat, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int64(stmt, 6, created_at);
   sqlite3_bind_int64(stmt, 7, (int64_t)normalized_hash);
   bind_provenance(stmt, 8, prov);

   int rc = sqlite3_step(stmt);
   sqlite3_reset(stmt);

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("memory_db: fact_create failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   int64_t id = sqlite3_last_insert_rowid(s_db.db);
   AUTH_DB_UNLOCK();

   if (id_out)
      *id_out = id;

   OLOG_INFO("memory_db: created fact %ld for user %d (hash=%u, category=%s)", (long)id, user_id,
             normalized_hash, cat);
   return MEMORY_DB_SUCCESS;
}

int memory_db_fact_create(int user_id,
                          const char *fact_text,
                          float confidence,
                          const char *source,
                          const char *category,
                          const memory_provenance_t *prov,
                          int64_t *id_out) {
   return memory_db_fact_create_at(user_id, fact_text, confidence, source, category, prov,
                                   /*created_at_override*/ 0, id_out);
}

/* Category-filtered keyword search.  Pre-filters at the SQL level so the embedding
 * cache and hybrid scoring downstream only see facts in the requested category. */
int memory_db_fact_search_by_category(int user_id,
                                      const char *keywords,
                                      const char *category,
                                      memory_fact_t *out_facts,
                                      int max_facts,
                                      int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!keywords || !category || !out_facts || max_facts <= 0) {
      return MEMORY_DB_FAILURE;
   }

   char pattern[MEMORY_FACT_TEXT_MAX];
   build_like_pattern(keywords, pattern, sizeof(pattern));

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_memory_fact_search_by_category;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, category, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 3, pattern, -1, SQLITE_STATIC);
   sqlite3_bind_int(stmt, 4, max_facts);

   int count = 0;
   while (count < max_facts && sqlite3_step(stmt) == SQLITE_ROW) {
      populate_fact_from_row(stmt, &out_facts[count]);
      count++;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = count;
   return MEMORY_DB_SUCCESS;
}

/* Per-fact category UPDATE used by the centroid backfill pass (v34).
 * Caller batches these inside a transaction to amortize lock cost. */
int memory_db_fact_update_category(int64_t fact_id, const char *category) {
   if (!category || !*category)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_memory_fact_update_category;
   sqlite3_reset(stmt);
   sqlite3_bind_text(stmt, 1, category, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int64(stmt, 2, fact_id);

   int rc = sqlite3_step(stmt);
   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();

   return (rc == SQLITE_DONE) ? MEMORY_DB_SUCCESS : MEMORY_DB_FAILURE;
}

int memory_db_fact_list_general(int user_id,
                                int64_t cursor_id,
                                memory_fact_t *out_facts,
                                int max_facts,
                                int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out_facts || max_facts <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_memory_fact_list_general;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, cursor_id);
   sqlite3_bind_int(stmt, 3, max_facts);

   int count = 0;
   while (count < max_facts && sqlite3_step(stmt) == SQLITE_ROW) {
      populate_fact_from_row(stmt, &out_facts[count]);
      count++;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = count;
   return MEMORY_DB_SUCCESS;
}

int memory_db_fact_count_general(int user_id, int *count_out) {
   if (count_out)
      *count_out = 0;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_memory_fact_count_general;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);

   int count = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      count = sqlite3_column_int(stmt, 0);
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = count;
   return MEMORY_DB_SUCCESS;
}

int memory_db_fact_get(int64_t fact_id, memory_fact_t *out_fact) {
   if (!out_fact) {
      return MEMORY_DB_FAILURE;
   }

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = s_db.stmt_memory_fact_get;
   sqlite3_reset(stmt);
   sqlite3_bind_int64(stmt, 1, fact_id);

   int result = MEMORY_DB_NOT_FOUND;
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      populate_fact_from_row(stmt, out_fact);
      result = MEMORY_DB_SUCCESS;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   return result;
}

int memory_db_fact_list(int user_id,
                        memory_fact_t *out_facts,
                        int max_facts,
                        int offset,
                        int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out_facts || max_facts <= 0) {
      return MEMORY_DB_FAILURE;
   }

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_memory_fact_list;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int(stmt, 2, max_facts);
   sqlite3_bind_int(stmt, 3, offset);

   int count = 0;
   while (count < max_facts && sqlite3_step(stmt) == SQLITE_ROW) {
      populate_fact_from_row(stmt, &out_facts[count]);
      count++;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = count;
   return MEMORY_DB_SUCCESS;
}

int memory_db_fact_search(int user_id,
                          const char *keywords,
                          memory_fact_t *out_facts,
                          int max_facts,
                          int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!keywords || !out_facts || max_facts <= 0) {
      return MEMORY_DB_FAILURE;
   }

   char pattern[MEMORY_FACT_TEXT_MAX];
   build_like_pattern(keywords, pattern, sizeof(pattern));

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_memory_fact_search;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, pattern, -1, SQLITE_STATIC);
   sqlite3_bind_int(stmt, 3, max_facts);

   int count = 0;
   while (count < max_facts && sqlite3_step(stmt) == SQLITE_ROW) {
      populate_fact_from_row(stmt, &out_facts[count]);
      count++;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = count;
   return MEMORY_DB_SUCCESS;
}

int memory_db_fact_update_access(int64_t fact_id, int user_id) {
   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = s_db.stmt_memory_fact_update_access;
   sqlite3_reset(stmt);
   sqlite3_bind_int64(stmt, 1, (int64_t)time(NULL));
   sqlite3_bind_double(stmt, 2, (double)g_config.memory.access_reinforcement_boost);
   sqlite3_bind_int64(stmt, 3, fact_id);
   sqlite3_bind_int(stmt, 4, user_id);

   int rc = sqlite3_step(stmt);
   sqlite3_reset(stmt);

   AUTH_DB_UNLOCK();
   return (rc == SQLITE_DONE) ? MEMORY_DB_SUCCESS : MEMORY_DB_FAILURE;
}

int memory_db_fact_update_confidence(int64_t fact_id, float confidence) {
   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = s_db.stmt_memory_fact_update_confidence;
   sqlite3_reset(stmt);
   sqlite3_bind_double(stmt, 1, confidence);
   sqlite3_bind_int64(stmt, 2, fact_id);

   int rc = sqlite3_step(stmt);
   sqlite3_reset(stmt);

   AUTH_DB_UNLOCK();
   return (rc == SQLITE_DONE) ? MEMORY_DB_SUCCESS : MEMORY_DB_FAILURE;
}

int memory_db_fact_supersede(int64_t old_fact_id, int64_t new_fact_id) {
   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = s_db.stmt_memory_fact_supersede;
   sqlite3_reset(stmt);
   sqlite3_bind_int64(stmt, 1, new_fact_id);
   sqlite3_bind_int64(stmt, 2, old_fact_id);

   int rc = sqlite3_step(stmt);
   sqlite3_reset(stmt);

   AUTH_DB_UNLOCK();
   return (rc == SQLITE_DONE) ? MEMORY_DB_SUCCESS : MEMORY_DB_FAILURE;
}

int memory_db_fact_delete(int64_t fact_id, int user_id) {
   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = s_db.stmt_memory_fact_delete;
   sqlite3_reset(stmt);
   sqlite3_bind_int64(stmt, 1, fact_id);
   sqlite3_bind_int(stmt, 2, user_id);

   int rc = sqlite3_step(stmt);
   int changes = sqlite3_changes(s_db.db);
   sqlite3_reset(stmt);

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      return MEMORY_DB_FAILURE;
   }
   return (changes > 0) ? MEMORY_DB_SUCCESS : MEMORY_DB_NOT_FOUND;
}

int memory_db_fact_find_similar(int user_id,
                                const char *fact_text,
                                memory_fact_t *out_facts,
                                int max_facts,
                                int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!fact_text || !out_facts || max_facts <= 0) {
      return MEMORY_DB_FAILURE;
   }

   /* Extract key words from fact for similarity search */
   char pattern[MEMORY_FACT_TEXT_MAX];
   build_like_pattern(fact_text, pattern, sizeof(pattern));

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_memory_fact_find_similar;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, pattern, -1, SQLITE_STATIC);

   int count = 0;
   while (count < max_facts && sqlite3_step(stmt) == SQLITE_ROW) {
      out_facts[count].id = sqlite3_column_int64(stmt, 0);
      const char *text = (const char *)sqlite3_column_text(stmt, 1);
      if (text) {
         strncpy(out_facts[count].fact_text, text, MEMORY_FACT_TEXT_MAX - 1);
         out_facts[count].fact_text[MEMORY_FACT_TEXT_MAX - 1] = '\0';
      }
      out_facts[count].confidence = (float)sqlite3_column_double(stmt, 2);
      count++;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = count;
   return MEMORY_DB_SUCCESS;
}

int memory_db_fact_find_by_hash(int user_id,
                                uint32_t hash,
                                memory_fact_t *out_facts,
                                int max_facts,
                                int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out_facts || max_facts <= 0 || hash == 0) {
      return MEMORY_DB_FAILURE;
   }

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_memory_fact_find_by_hash;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, (int64_t)hash);

   int count = 0;
   while (count < max_facts && sqlite3_step(stmt) == SQLITE_ROW) {
      out_facts[count].id = sqlite3_column_int64(stmt, 0);
      const char *text = (const char *)sqlite3_column_text(stmt, 1);
      if (text) {
         strncpy(out_facts[count].fact_text, text, MEMORY_FACT_TEXT_MAX - 1);
         out_facts[count].fact_text[MEMORY_FACT_TEXT_MAX - 1] = '\0';
      }
      out_facts[count].confidence = (float)sqlite3_column_double(stmt, 2);
      count++;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = count;
   return MEMORY_DB_SUCCESS;
}

int memory_db_fact_prune_superseded(int user_id, int retention_days, int *count_out) {
   if (count_out)
      *count_out = 0;

   AUTH_DB_LOCK_OR_FAIL();

   time_t cutoff = time(NULL) - (retention_days * 24 * 60 * 60);

   sqlite3_stmt *stmt = s_db.stmt_memory_fact_prune_superseded;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, (int64_t)cutoff);

   int rc = sqlite3_step(stmt);
   int deleted = sqlite3_changes(s_db.db);
   sqlite3_reset(stmt);

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("memory_db: prune_superseded failed: %s", sqlite3_errmsg(s_db.db));
      return MEMORY_DB_FAILURE;
   }

   if (count_out)
      *count_out = deleted;
   if (deleted > 0) {
      OLOG_INFO("memory_db: pruned %d superseded facts for user %d", deleted, user_id);
   }
   return MEMORY_DB_SUCCESS;
}

int memory_db_fact_prune_stale(int user_id, int stale_days, float min_confidence, int *count_out) {
   if (count_out)
      *count_out = 0;

   AUTH_DB_LOCK_OR_FAIL();

   time_t cutoff = time(NULL) - (stale_days * 24 * 60 * 60);

   sqlite3_stmt *stmt = s_db.stmt_memory_fact_prune_stale;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, (int64_t)cutoff);
   sqlite3_bind_double(stmt, 3, min_confidence);

   int rc = sqlite3_step(stmt);
   int deleted = sqlite3_changes(s_db.db);
   sqlite3_reset(stmt);

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("memory_db: prune_stale failed: %s", sqlite3_errmsg(s_db.db));
      return MEMORY_DB_FAILURE;
   }

   if (count_out)
      *count_out = deleted;
   if (deleted > 0) {
      OLOG_INFO("memory_db: pruned %d stale facts for user %d", deleted, user_id);
   }
   return MEMORY_DB_SUCCESS;
}

int memory_db_facts_delete_by_patterns(int user_id,
                                       const char *const *patterns,
                                       int n_patterns,
                                       bool dry_run,
                                       int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!patterns || n_patterns <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_FAIL();

   /* Build a single statement: SELECT count first (for both modes — surface
    * the would-delete count even on dry-run), then conditionally DELETE.
    * Patterns OR'd via repeated `fact_text LIKE ?` clauses, all under the
    * same user_id scope.  ESCAPE '\\' lets a future caller embed literal
    * %/_ via backslash without surprise.
    *
    * Cap at MAX_DELETE_PATTERNS to keep the SQL prepared-statement size
    * bounded and prevent a malicious admin client from constructing a
    * multi-MB query.  64 is well above what any caller actually needs. */
   const int MAX_DELETE_PATTERNS = 64;
   if (n_patterns > MAX_DELETE_PATTERNS) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   /* Construct WHERE clause: " AND (fact_text LIKE ? ESCAPE '\\' OR ... )"
    * Worst case at MAX=64: ~30 bytes/pattern × 64 + base = ~2 KB.  Use a
    * fixed buffer comfortably larger. */
   char where_buf[4096];
   int off = snprintf(where_buf, sizeof(where_buf), "(");
   for (int i = 0; i < n_patterns; i++) {
      int n = snprintf(where_buf + off, sizeof(where_buf) - off, "%sfact_text LIKE ? ESCAPE '\\'",
                       i == 0 ? "" : " OR ");
      if (n < 0 || n >= (int)(sizeof(where_buf) - off)) {
         AUTH_DB_UNLOCK();
         return MEMORY_DB_FAILURE;
      }
      off += n;
   }
   if (off + 2 >= (int)sizeof(where_buf)) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   where_buf[off++] = ')';
   where_buf[off] = '\0';

   /* COUNT pass — needed for both --dry-run report and post-DELETE
    * confirmation; SQLite's `changes()` after DELETE would give us the
    * same number on the actual-delete path, but running COUNT first lets
    * the dry-run path skip the DELETE entirely. */
   char sql[5120];
   snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM memory_facts WHERE user_id = ? AND %s",
            where_buf);
   sqlite3_stmt *count_stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &count_stmt, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(count_stmt, 1, user_id);
   for (int i = 0; i < n_patterns; i++) {
      sqlite3_bind_text(count_stmt, 2 + i, patterns[i], -1, SQLITE_STATIC);
   }

   int matched = 0;
   if (sqlite3_step(count_stmt) == SQLITE_ROW)
      matched = sqlite3_column_int(count_stmt, 0);
   sqlite3_finalize(count_stmt);

   if (dry_run || matched == 0) {
      AUTH_DB_UNLOCK();
      if (count_out)
         *count_out = matched;
      return MEMORY_DB_SUCCESS;
   }

   /* DELETE pass.  Run in an explicit transaction so a mid-statement
    * failure does not leave the table partially trimmed. */
   snprintf(sql, sizeof(sql), "DELETE FROM memory_facts WHERE user_id = ? AND %s", where_buf);
   sqlite3_stmt *del_stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &del_stmt, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_exec(s_db.db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
   sqlite3_bind_int(del_stmt, 1, user_id);
   for (int i = 0; i < n_patterns; i++) {
      sqlite3_bind_text(del_stmt, 2 + i, patterns[i], -1, SQLITE_STATIC);
   }
   int rc = sqlite3_step(del_stmt);
   int deleted = sqlite3_changes(s_db.db);
   sqlite3_finalize(del_stmt);

   if (rc != SQLITE_DONE) {
      sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_exec(s_db.db, "COMMIT", NULL, NULL, NULL);
   AUTH_DB_UNLOCK();

   /* Cache invalidation — embedding cache holds (id, embedding) pairs that
    * are now stale.  Mark dirty so the next access reloads. */
   memory_embeddings_invalidate_cache();

   if (count_out)
      *count_out = deleted;
   OLOG_INFO("memory_db: cleanup_meta_facts deleted %d rows for user %d (n_patterns=%d)", deleted,
             user_id, n_patterns);
   return MEMORY_DB_SUCCESS;
}

/* =============================================================================
 * Date-Filtered Queries
 * ============================================================================= */

int memory_db_fact_search_since(int user_id,
                                const char *keywords,
                                time_t since_ts,
                                memory_fact_t *out_facts,
                                int max_facts,
                                int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!keywords || !out_facts || max_facts <= 0) {
      return MEMORY_DB_FAILURE;
   }

   char pattern[MEMORY_FACT_TEXT_MAX];
   build_like_pattern(keywords, pattern, sizeof(pattern));

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_memory_fact_search_since;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, pattern, -1, SQLITE_STATIC);
   sqlite3_bind_int64(stmt, 3, (int64_t)since_ts);
   sqlite3_bind_int(stmt, 4, max_facts);

   int count = 0;
   while (count < max_facts && sqlite3_step(stmt) == SQLITE_ROW) {
      populate_fact_from_row(stmt, &out_facts[count]);
      count++;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = count;
   return MEMORY_DB_SUCCESS;
}

int memory_db_summary_search_since(int user_id,
                                   const char *keywords,
                                   time_t since_ts,
                                   memory_summary_t *out_summaries,
                                   int max_summaries,
                                   int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!keywords || !out_summaries || max_summaries <= 0) {
      return MEMORY_DB_FAILURE;
   }

   char pattern[MEMORY_SUMMARY_MAX];
   build_like_pattern(keywords, pattern, sizeof(pattern));

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_memory_summary_search_since;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, pattern, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 3, pattern, -1, SQLITE_STATIC);
   sqlite3_bind_int64(stmt, 4, (int64_t)since_ts);
   sqlite3_bind_int(stmt, 5, max_summaries);

   int count = 0;
   while (count < max_summaries && sqlite3_step(stmt) == SQLITE_ROW) {
      populate_summary_from_row(stmt, &out_summaries[count]);
      count++;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = count;
   return MEMORY_DB_SUCCESS;
}

int memory_db_fact_list_since(int user_id,
                              time_t since_ts,
                              memory_fact_t *out_facts,
                              int max_facts,
                              int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out_facts || max_facts <= 0) {
      return MEMORY_DB_FAILURE;
   }

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_memory_fact_list_since;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, (int64_t)since_ts);
   sqlite3_bind_int(stmt, 3, max_facts);

   int count = 0;
   while (count < max_facts && sqlite3_step(stmt) == SQLITE_ROW) {
      populate_fact_from_row(stmt, &out_facts[count]);
      count++;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = count;
   return MEMORY_DB_SUCCESS;
}

int memory_db_summary_list_since(int user_id,
                                 time_t since_ts,
                                 memory_summary_t *out_summaries,
                                 int max_summaries,
                                 int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out_summaries || max_summaries <= 0) {
      return MEMORY_DB_FAILURE;
   }

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_memory_summary_list_since;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, (int64_t)since_ts);
   sqlite3_bind_int(stmt, 3, max_summaries);

   int count = 0;
   while (count < max_summaries && sqlite3_step(stmt) == SQLITE_ROW) {
      populate_summary_from_row(stmt, &out_summaries[count]);
      count++;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = count;
   return MEMORY_DB_SUCCESS;
}

/* =============================================================================
 * Preference Operations
 * ============================================================================= */

int memory_db_pref_upsert(int user_id,
                          const char *category,
                          const char *value,
                          float confidence,
                          const char *source,
                          const memory_provenance_t *prov) {
   if (!category || !value || !source) {
      return MEMORY_DB_FAILURE;
   }

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   time_t now = time(NULL);
   sqlite3_stmt *stmt = s_db.stmt_memory_pref_upsert;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, category, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 3, value, -1, SQLITE_STATIC);
   sqlite3_bind_double(stmt, 4, confidence);
   sqlite3_bind_text(stmt, 5, source, -1, SQLITE_STATIC);
   sqlite3_bind_int64(stmt, 6, (int64_t)now);
   sqlite3_bind_int64(stmt, 7, (int64_t)now);
   bind_provenance(stmt, 8, prov);

   int rc = sqlite3_step(stmt);
   sqlite3_reset(stmt);

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("memory_db: pref_upsert failed: %s", sqlite3_errmsg(s_db.db));
      return MEMORY_DB_FAILURE;
   }

   OLOG_INFO("memory_db: upserted preference %s=%s for user %d", category, value, user_id);
   return MEMORY_DB_SUCCESS;
}

int memory_db_pref_get(int user_id, const char *category, memory_preference_t *out_pref) {
   if (!category || !out_pref) {
      return MEMORY_DB_FAILURE;
   }

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = s_db.stmt_memory_pref_get;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, category, -1, SQLITE_STATIC);

   int result = MEMORY_DB_NOT_FOUND;
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      populate_pref_from_row(stmt, out_pref);
      result = MEMORY_DB_SUCCESS;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   return result;
}

int memory_db_pref_list(int user_id,
                        memory_preference_t *out_prefs,
                        int max_prefs,
                        int offset,
                        int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out_prefs || max_prefs <= 0) {
      return MEMORY_DB_FAILURE;
   }

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_memory_pref_list;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int(stmt, 2, max_prefs);
   sqlite3_bind_int(stmt, 3, offset);

   int count = 0;
   while (count < max_prefs && sqlite3_step(stmt) == SQLITE_ROW) {
      populate_pref_from_row(stmt, &out_prefs[count]);
      count++;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = count;
   return MEMORY_DB_SUCCESS;
}

int memory_db_pref_search(int user_id,
                          const char *keywords,
                          memory_preference_t *out_prefs,
                          int max_prefs,
                          int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!keywords || !out_prefs || max_prefs <= 0) {
      return MEMORY_DB_FAILURE;
   }

   char pattern[256];
   build_like_pattern(keywords, pattern, sizeof(pattern));

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_memory_pref_search;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, pattern, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 3, pattern, -1, SQLITE_STATIC);
   sqlite3_bind_int(stmt, 4, max_prefs);

   int count = 0;
   while (count < max_prefs && sqlite3_step(stmt) == SQLITE_ROW) {
      populate_pref_from_row(stmt, &out_prefs[count]);
      count++;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = count;
   return MEMORY_DB_SUCCESS;
}

int memory_db_pref_delete(int user_id, const char *category) {
   if (!category) {
      return MEMORY_DB_FAILURE;
   }

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = s_db.stmt_memory_pref_delete;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, category, -1, SQLITE_STATIC);

   int rc = sqlite3_step(stmt);
   int changes = sqlite3_changes(s_db.db);
   sqlite3_reset(stmt);

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      return MEMORY_DB_FAILURE;
   }
   return (changes > 0) ? MEMORY_DB_SUCCESS : MEMORY_DB_NOT_FOUND;
}

/* =============================================================================
 * Summary Operations
 * ============================================================================= */

int memory_db_summary_create_at(int user_id,
                                const char *session_id,
                                const char *summary,
                                const char *topics,
                                const char *sentiment,
                                int message_count,
                                int duration_seconds,
                                const memory_provenance_t *prov,
                                int64_t created_at_override,
                                int64_t *id_out) {
   if (id_out)
      *id_out = 0;
   if (!session_id || !summary) {
      return MEMORY_DB_FAILURE;
   }

   /* 0 sentinel = "use NOW()".  See memory_db_fact_create_at comment. */
   const int64_t created_at = (created_at_override > 0) ? created_at_override : (int64_t)time(NULL);

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_memory_summary_create;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, session_id, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 3, summary, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 4, topics ? topics : "", -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 5, sentiment ? sentiment : "neutral", -1, SQLITE_STATIC);
   sqlite3_bind_int64(stmt, 6, created_at);
   sqlite3_bind_int(stmt, 7, message_count);
   sqlite3_bind_int(stmt, 8, duration_seconds);
   bind_provenance(stmt, 9, prov);

   int rc = sqlite3_step(stmt);
   sqlite3_reset(stmt);

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("memory_db: summary_create failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   int64_t id = sqlite3_last_insert_rowid(s_db.db);
   AUTH_DB_UNLOCK();

   if (id_out)
      *id_out = id;

   OLOG_INFO("memory_db: created summary %ld for user %d", (long)id, user_id);
   return MEMORY_DB_SUCCESS;
}

int memory_db_summary_create(int user_id,
                             const char *session_id,
                             const char *summary,
                             const char *topics,
                             const char *sentiment,
                             int message_count,
                             int duration_seconds,
                             const memory_provenance_t *prov,
                             int64_t *id_out) {
   return memory_db_summary_create_at(user_id, session_id, summary, topics, sentiment,
                                      message_count, duration_seconds, prov,
                                      /*created_at_override*/ 0, id_out);
}

int memory_db_summary_list(int user_id,
                           memory_summary_t *out_summaries,
                           int max_summaries,
                           int offset,
                           int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out_summaries || max_summaries <= 0) {
      return MEMORY_DB_FAILURE;
   }

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_memory_summary_list;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int(stmt, 2, max_summaries);
   sqlite3_bind_int(stmt, 3, offset);

   int count = 0;
   while (count < max_summaries && sqlite3_step(stmt) == SQLITE_ROW) {
      populate_summary_from_row(stmt, &out_summaries[count]);
      count++;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = count;
   return MEMORY_DB_SUCCESS;
}

int memory_db_summary_mark_consolidated(int64_t summary_id) {
   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = s_db.stmt_memory_summary_mark_consolidated;
   sqlite3_reset(stmt);
   sqlite3_bind_int64(stmt, 1, summary_id);

   int rc = sqlite3_step(stmt);
   sqlite3_reset(stmt);

   AUTH_DB_UNLOCK();
   return (rc == SQLITE_DONE) ? MEMORY_DB_SUCCESS : MEMORY_DB_FAILURE;
}

int memory_db_summary_search(int user_id,
                             const char *keywords,
                             memory_summary_t *out_summaries,
                             int max_summaries,
                             int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!keywords || !out_summaries || max_summaries <= 0) {
      return MEMORY_DB_FAILURE;
   }

   char pattern[MEMORY_SUMMARY_MAX];
   build_like_pattern(keywords, pattern, sizeof(pattern));

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_memory_summary_search;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, pattern, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 3, pattern, -1, SQLITE_STATIC);
   sqlite3_bind_int(stmt, 4, max_summaries);

   int count = 0;
   while (count < max_summaries && sqlite3_step(stmt) == SQLITE_ROW) {
      populate_summary_from_row(stmt, &out_summaries[count]);
      count++;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = count;
   return MEMORY_DB_SUCCESS;
}

int memory_db_summary_delete(int64_t summary_id, int user_id) {
   if (summary_id <= 0 || user_id <= 0) {
      return MEMORY_DB_FAILURE;
   }

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM memory_summaries WHERE id = ? AND user_id = ?",
                               -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("memory_db: summary_delete prepare failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   sqlite3_bind_int64(stmt, 1, summary_id);
   sqlite3_bind_int(stmt, 2, user_id);

   rc = sqlite3_step(stmt);
   int changes = sqlite3_changes(s_db.db);
   sqlite3_finalize(stmt);

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      return MEMORY_DB_FAILURE;
   }
   return (changes > 0) ? MEMORY_DB_SUCCESS : MEMORY_DB_NOT_FOUND;
}

/* =============================================================================
 * Summary Embedding Operations (v45 — semantic summary adapter)
 *
 * Norms are recomputed inside the scan rather than persisted alongside the
 * blob.  At the per-user summary scale (hundreds), a one-shot norm pass
 * over each row's float vector is cheaper than the schema churn an extra
 * column would cost, and keeps memory_db_summary_update_embedding
 * symmetric with how the recompute worker writes (vector-only).
 * ============================================================================= */

int memory_db_summary_update_embedding(int user_id,
                                       int64_t summary_id,
                                       const float *embedding,
                                       int dims) {
   if (!embedding || dims <= 0 || user_id <= 0 || summary_id <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_memory_summary_update_embedding);
   sqlite3_bind_blob(s_db.stmt_memory_summary_update_embedding, 1, embedding,
                     dims * (int)sizeof(float), SQLITE_TRANSIENT);
   sqlite3_bind_int64(s_db.stmt_memory_summary_update_embedding, 2, summary_id);
   sqlite3_bind_int(s_db.stmt_memory_summary_update_embedding, 3, user_id);

   int rc = sqlite3_step(s_db.stmt_memory_summary_update_embedding);
   sqlite3_reset(s_db.stmt_memory_summary_update_embedding);

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("memory_db: summary_update_embedding failed for id %ld: %s", (long)summary_id,
                 sqlite3_errmsg(s_db.db));
      return MEMORY_DB_FAILURE;
   }
   return MEMORY_DB_SUCCESS;
}

/* Inline cosine helper.  Inputs are assumed non-NULL with identical dims;
 * callers validate before invoking.  Returns 0.0f if either norm is zero. */
static float summary_cosine(const float *a, float a_norm, const float *b, float b_norm, int dims) {
   if (a_norm <= 0.0f || b_norm <= 0.0f)
      return 0.0f;
   float dot = 0.0f;
   for (int i = 0; i < dims; i++) {
      dot += a[i] * b[i];
   }
   return dot / (a_norm * b_norm);
}

/* Compile-time top-K cap.  Heap entries hold only {id, score} (12 bytes),
 * so the entire ranking buffer is ~192 bytes regardless of K — safe on
 * 256 KB pthread stacks.  Full memory_summary_t rows (~3 KB each) are
 * fetched in a second pass for the K survivors only, instead of being
 * copied into a heap slot at every admission.  This both shrinks the
 * stack footprint (~154 KB → 192 B) and skips the strncpy storm under
 * the auth_db lock for non-survivor rows. */
#define MEMORY_SUMMARY_SEMANTIC_TOPK_CAP 16

typedef struct {
   int64_t id;
   float score;
} summary_id_score_t;

int memory_db_summary_search_semantic(int user_id,
                                      const float *query_vec,
                                      int query_dims,
                                      time_t since_ts,
                                      int max_summaries,
                                      int max_scan,
                                      memory_summary_t *out_summaries,
                                      float *out_scores,
                                      int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!query_vec || query_dims <= 0 || max_summaries <= 0 || !out_summaries || !out_scores ||
       user_id <= 0)
      return MEMORY_DB_FAILURE;
   if (max_scan <= 0)
      max_scan = MEMORY_SUMMARY_SEMANTIC_SCAN_CAP_DEFAULT;
   if (max_summaries > MEMORY_SUMMARY_SEMANTIC_TOPK_CAP)
      max_summaries = MEMORY_SUMMARY_SEMANTIC_TOPK_CAP;

   float q_norm = memory_embeddings_l2_norm(query_vec, query_dims);
   if (q_norm <= 0.0f) {
      /* Zero-norm query → nothing to compare against. */
      return MEMORY_DB_SUCCESS;
   }

   const int expected_bytes = query_dims * (int)sizeof(float);

   /* Top-K ranking buffer — id + score only.  Min-heap semantics via
    * linear scan to find evictee (K ≤ 16, sub-microsecond).  Sorted to
    * descending order after the scan completes. */
   summary_id_score_t heap[MEMORY_SUMMARY_SEMANTIC_TOPK_CAP];
   int heap_n = 0;

   AUTH_DB_LOCK_OR_FAIL();

   /* PASS 1 — cosine-rank scan.  The scan SQL was reduced to (id, embedding)
    * only, so SQLite doesn't materialise the long summary / topics text for
    * every row when we're just doing cosine.  Full rows are fetched in
    * pass 2 for survivors only. */
   sqlite3_reset(s_db.stmt_memory_summary_scan_embeddings);
   sqlite3_bind_int(s_db.stmt_memory_summary_scan_embeddings, 1, user_id);
   sqlite3_bind_int64(s_db.stmt_memory_summary_scan_embeddings, 2, (sqlite3_int64)since_ts);
   sqlite3_bind_int(s_db.stmt_memory_summary_scan_embeddings, 3, max_scan);

   int rows_seen = 0;
   while (rows_seen < max_scan &&
          sqlite3_step(s_db.stmt_memory_summary_scan_embeddings) == SQLITE_ROW) {
      rows_seen++;

      int blob_bytes = sqlite3_column_bytes(s_db.stmt_memory_summary_scan_embeddings, 1);
      if (blob_bytes != expected_bytes) {
         /* Dimension mismatch — recompute worker will catch up. */
         continue;
      }
      const void *blob = sqlite3_column_blob(s_db.stmt_memory_summary_scan_embeddings, 1);
      if (!blob)
         continue;

      const float *row_vec = (const float *)blob;
      float row_norm = memory_embeddings_l2_norm(row_vec, query_dims);
      float score = summary_cosine(query_vec, q_norm, row_vec, row_norm, query_dims);

      int min_idx = 0;
      if (heap_n >= max_summaries) {
         for (int i = 1; i < heap_n; i++) {
            if (heap[i].score < heap[min_idx].score)
               min_idx = i;
         }
         if (score <= heap[min_idx].score)
            continue;
      } else {
         min_idx = heap_n;
         heap_n++;
      }

      heap[min_idx].id = sqlite3_column_int64(s_db.stmt_memory_summary_scan_embeddings, 0);
      heap[min_idx].score = score;
   }

   sqlite3_reset(s_db.stmt_memory_summary_scan_embeddings);

   if (heap_n <= 0) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_SUCCESS;
   }

   /* Sort survivors by descending score before the row-fetch pass so the
    * out_summaries[] array lands in the order the caller expects. */
   for (int i = 1; i < heap_n; i++) {
      summary_id_score_t tmp = heap[i];
      int j = i - 1;
      while (j >= 0 && heap[j].score < tmp.score) {
         heap[j + 1] = heap[j];
         j--;
      }
      heap[j + 1] = tmp;
   }

   /* PASS 2 — fetch the full row for each survivor.  Inline-prepared
    * one-shot statement keeps the change scoped to this function; per-call
    * prepare cost (~10 μs) is negligible against the cosine scan that
    * already ran.  The user_id bind is defense-in-depth — the scan already
    * filtered to user_id, but this guards against a re-bound id mismatch.
    *
    * If a row was deleted between pass 1 and pass 2 (we hold the auth_db
    * lock across both, so this can't happen in practice — but the code is
    * tolerant), we simply produce fewer survivors than the heap held. */
   sqlite3_stmt *fetch = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "SELECT id, user_id, session_id, summary, topics, sentiment, "
                               "       created_at, message_count, duration_seconds, consolidated "
                               "FROM memory_summaries WHERE id = ? AND user_id = ?",
                               -1, &fetch, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("memory_db: summary_search_semantic prepare fetch failed: %s",
                 sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   int produced = 0;
   for (int i = 0; i < heap_n; i++) {
      sqlite3_reset(fetch);
      sqlite3_bind_int64(fetch, 1, heap[i].id);
      sqlite3_bind_int(fetch, 2, user_id);

      if (sqlite3_step(fetch) != SQLITE_ROW) {
         /* Row vanished between passes — skip and continue. */
         continue;
      }

      memory_summary_t *s = &out_summaries[produced];
      memset(s, 0, sizeof(*s));
      s->id = sqlite3_column_int64(fetch, 0);
      s->user_id = sqlite3_column_int(fetch, 1);
      const unsigned char *sid = sqlite3_column_text(fetch, 2);
      if (sid)
         strncpy(s->session_id, (const char *)sid, sizeof(s->session_id) - 1);
      const unsigned char *txt = sqlite3_column_text(fetch, 3);
      if (txt)
         strncpy(s->summary, (const char *)txt, sizeof(s->summary) - 1);
      const unsigned char *topics = sqlite3_column_text(fetch, 4);
      if (topics)
         strncpy(s->topics, (const char *)topics, sizeof(s->topics) - 1);
      const unsigned char *sent = sqlite3_column_text(fetch, 5);
      if (sent)
         strncpy(s->sentiment, (const char *)sent, sizeof(s->sentiment) - 1);
      s->created_at = (time_t)sqlite3_column_int64(fetch, 6);
      s->message_count = sqlite3_column_int(fetch, 7);
      s->duration_seconds = sqlite3_column_int(fetch, 8);
      s->consolidated = (sqlite3_column_int(fetch, 9) != 0);

      out_scores[produced] = heap[i].score;
      produced++;
   }

   sqlite3_finalize(fetch);
   AUTH_DB_UNLOCK();

   if (count_out)
      *count_out = produced;
   return MEMORY_DB_SUCCESS;
}

int memory_db_summary_list_without_embedding(int user_id,
                                             int expected_dims,
                                             int64_t *out_ids,
                                             char (*out_texts)[MEMORY_SUMMARY_MAX],
                                             int max_count,
                                             int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out_ids || !out_texts || max_count <= 0 || expected_dims <= 0 || user_id <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_memory_summary_list_without_embedding);
   sqlite3_bind_int(s_db.stmt_memory_summary_list_without_embedding, 1, user_id);
   sqlite3_bind_int(s_db.stmt_memory_summary_list_without_embedding, 2, expected_dims);
   sqlite3_bind_int(s_db.stmt_memory_summary_list_without_embedding, 3, max_count);

   int n = 0;
   while (n < max_count &&
          sqlite3_step(s_db.stmt_memory_summary_list_without_embedding) == SQLITE_ROW) {
      out_ids[n] = sqlite3_column_int64(s_db.stmt_memory_summary_list_without_embedding, 0);
      const unsigned char *txt = sqlite3_column_text(
          s_db.stmt_memory_summary_list_without_embedding, 1);
      if (txt) {
         strncpy(out_texts[n], (const char *)txt, MEMORY_SUMMARY_MAX - 1);
         out_texts[n][MEMORY_SUMMARY_MAX - 1] = '\0';
      } else {
         out_texts[n][0] = '\0';
      }
      n++;
   }

   sqlite3_reset(s_db.stmt_memory_summary_list_without_embedding);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = n;
   return MEMORY_DB_SUCCESS;
}

/* =============================================================================
 * Utility Operations
 * ============================================================================= */

int memory_db_delete_user_memories(int user_id) {
   if (user_id <= 0) {
      return MEMORY_DB_FAILURE;
   }

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = NULL;
   int rc;

   /* Delete in FK-safe order: relations -> entities -> facts -> prefs -> summaries */

   /* Delete relations first (FK references entities) */
   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM memory_relations WHERE user_id = ?", -1, &stmt,
                           NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("memory_db: delete_user_memories (relations) prepare failed: %s",
                 sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   if (rc != SQLITE_DONE) {
      OLOG_ERROR("memory_db: delete_user_memories (relations) failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   /* Delete entities */
   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM memory_entities WHERE user_id = ?", -1, &stmt,
                           NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("memory_db: delete_user_memories (entities) prepare failed: %s",
                 sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   if (rc != SQLITE_DONE) {
      OLOG_ERROR("memory_db: delete_user_memories (entities) failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   /* Delete facts */
   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM memory_facts WHERE user_id = ?", -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("memory_db: delete_user_memories (facts) prepare failed: %s",
                 sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   if (rc != SQLITE_DONE) {
      OLOG_ERROR("memory_db: delete_user_memories (facts) failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   /* Delete preferences */
   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM memory_preferences WHERE user_id = ?", -1, &stmt,
                           NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("memory_db: delete_user_memories (prefs) prepare failed: %s",
                 sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   if (rc != SQLITE_DONE) {
      OLOG_ERROR("memory_db: delete_user_memories (prefs) failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   /* Delete summaries */
   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM memory_summaries WHERE user_id = ?", -1, &stmt,
                           NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("memory_db: delete_user_memories (summaries) prepare failed: %s",
                 sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   if (rc != SQLITE_DONE) {
      OLOG_ERROR("memory_db: delete_user_memories (summaries) failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   AUTH_DB_UNLOCK();

   /* Drop in-memory embedding caches.  Without this, hybrid_search keeps
    * returning the deleted facts/entities until another invalidation fires
    * (e.g., a fresh embed_and_store).  Acquired *after* releasing the auth_db
    * lock — embedding_cache mutex is a leaf lock per ARCHITECTURE.md. */
   memory_embeddings_invalidate_all();

   OLOG_INFO("memory_db: deleted all memories for user %d", user_id);
   return MEMORY_DB_SUCCESS;
}

int memory_db_get_stats(int user_id, memory_stats_t *out_stats) {
   if (!out_stats) {
      return MEMORY_DB_FAILURE;
   }

   memset(out_stats, 0, sizeof(memory_stats_t));

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   /* Combined stats query — single round-trip for all counts + date range */
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT "
       "(SELECT COUNT(*) FROM memory_facts WHERE user_id = ?1 AND superseded_by IS NULL), "
       "(SELECT MIN(created_at) FROM memory_facts WHERE user_id = ?1 AND superseded_by IS NULL), "
       "(SELECT MAX(created_at) FROM memory_facts WHERE user_id = ?1 AND superseded_by IS NULL), "
       "(SELECT COUNT(*) FROM memory_preferences WHERE user_id = ?1), "
       "(SELECT COUNT(*) FROM memory_summaries WHERE user_id = ?1), "
       "(SELECT COUNT(*) FROM memory_entities WHERE user_id = ?1)",
       -1, &stmt, NULL);

   if (rc == SQLITE_OK) {
      sqlite3_bind_int(stmt, 1, user_id);
      if (sqlite3_step(stmt) == SQLITE_ROW) {
         out_stats->fact_count = sqlite3_column_int(stmt, 0);
         out_stats->oldest_fact = (time_t)sqlite3_column_int64(stmt, 1);
         out_stats->newest_fact = (time_t)sqlite3_column_int64(stmt, 2);
         out_stats->pref_count = sqlite3_column_int(stmt, 3);
         out_stats->summary_count = sqlite3_column_int(stmt, 4);
         out_stats->entity_count = sqlite3_column_int(stmt, 5);
      }
      sqlite3_finalize(stmt);
   }

   AUTH_DB_UNLOCK();
   return MEMORY_DB_SUCCESS;
}

/* =============================================================================
 * Extraction Tracking
 * ============================================================================= */

int memory_db_get_last_extracted(int64_t conversation_id, int *count_out) {
   if (count_out)
      *count_out = 0;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_conv_get_last_extracted;
   sqlite3_reset(stmt);
   sqlite3_bind_int64(stmt, 1, conversation_id);

   int result = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      result = sqlite3_column_int(stmt, 0);
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = result;
   return MEMORY_DB_SUCCESS;
}

int memory_db_set_last_extracted(int64_t conversation_id, int message_count, int64_t last_msg_id) {
   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = s_db.stmt_conv_set_last_extracted;
   sqlite3_reset(stmt);
   /* Params: message_count, last_msg_id (x2 for CASE), conversation_id */
   sqlite3_bind_int(stmt, 1, message_count);
   sqlite3_bind_int64(stmt, 2, last_msg_id);
   sqlite3_bind_int64(stmt, 3, last_msg_id);
   sqlite3_bind_int64(stmt, 4, conversation_id);

   int rc = sqlite3_step(stmt);
   sqlite3_reset(stmt);

   AUTH_DB_UNLOCK();
   return (rc == SQLITE_DONE) ? MEMORY_DB_SUCCESS : MEMORY_DB_FAILURE;
}

/* memory_db_facts_get_sources moved to memory_db_provenance.c (Phase B). */

int memory_db_get_last_extracted_msg_id(int64_t conversation_id, int64_t *msg_id_out) {
   if (!msg_id_out)
      return MEMORY_DB_FAILURE;
   *msg_id_out = 0;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "SELECT last_extracted_msg_id FROM conversations WHERE id = ?", -1,
                               &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int64(stmt, 1, conversation_id);
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      *msg_id_out = sqlite3_column_int64(stmt, 0);
   }
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return MEMORY_DB_SUCCESS;
}

/* memory_db_fact_get_source moved to memory_db_provenance.c (Phase B). */

/* =============================================================================
 * Decay and Maintenance Operations (Phase 5)
 *
 * These use ad-hoc prepared statements (acceptable for once-daily execution).
 * ============================================================================= */

int memory_db_apply_fact_decay(int user_id,
                               float inferred_rate,
                               float explicit_rate,
                               float inferred_floor,
                               float explicit_floor,
                               int *count_out) {
   if (count_out)
      *count_out = 0;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(
       s_db.db,
       "UPDATE memory_facts SET confidence ="
       "  CASE WHEN source = 'explicit'"
       "    THEN MAX(?, confidence * powf(?, "
       "         (CAST(strftime('%s','now') AS REAL) - last_accessed) / 604800.0))"
       "    ELSE MAX(?, confidence * powf(?, "
       "         (CAST(strftime('%s','now') AS REAL) - last_accessed) / 604800.0))"
       "  END "
       "WHERE user_id = ? AND superseded_by IS NULL AND last_accessed IS NOT NULL",
       -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("memory_db: apply_fact_decay prepare failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   sqlite3_bind_double(stmt, 1, (double)explicit_floor);
   sqlite3_bind_double(stmt, 2, (double)explicit_rate);
   sqlite3_bind_double(stmt, 3, (double)inferred_floor);
   sqlite3_bind_double(stmt, 4, (double)inferred_rate);
   sqlite3_bind_int(stmt, 5, user_id);

   rc = sqlite3_step(stmt);
   int affected = sqlite3_changes(s_db.db);
   sqlite3_finalize(stmt);

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("memory_db: apply_fact_decay failed: %s", sqlite3_errmsg(s_db.db));
      return MEMORY_DB_FAILURE;
   }

   if (count_out)
      *count_out = affected;
   return MEMORY_DB_SUCCESS;
}

int memory_db_apply_pref_decay(int user_id, float pref_rate, float pref_floor, int *count_out) {
   if (count_out)
      *count_out = 0;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(
       s_db.db,
       "UPDATE memory_preferences SET confidence ="
       "  MAX(?, confidence * powf(?, "
       "      (CAST(strftime('%s','now') AS REAL) - updated_at) / 604800.0))"
       " WHERE user_id = ?",
       -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("memory_db: apply_pref_decay prepare failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   sqlite3_bind_double(stmt, 1, (double)pref_floor);
   sqlite3_bind_double(stmt, 2, (double)pref_rate);
   sqlite3_bind_int(stmt, 3, user_id);

   rc = sqlite3_step(stmt);
   int affected = sqlite3_changes(s_db.db);
   sqlite3_finalize(stmt);

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("memory_db: apply_pref_decay failed: %s", sqlite3_errmsg(s_db.db));
      return MEMORY_DB_FAILURE;
   }

   if (count_out)
      *count_out = affected;
   return MEMORY_DB_SUCCESS;
}

int memory_db_prune_low_confidence(int user_id, float threshold, int *count_out) {
   if (count_out)
      *count_out = 0;

   AUTH_DB_LOCK_OR_FAIL();

   /* Wrap audit log + delete in a transaction for consistency */
   sqlite3_exec(s_db.db, "BEGIN IMMEDIATE", NULL, NULL, NULL);

   /* Log facts that will be pruned (audit trail for irreversible operation) */
   sqlite3_stmt *log_stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "SELECT id, fact_text, confidence, source FROM memory_facts "
                               "WHERE user_id = ? AND confidence < ? AND superseded_by IS NULL",
                               -1, &log_stmt, NULL);
   if (rc == SQLITE_OK) {
      sqlite3_bind_int(log_stmt, 1, user_id);
      sqlite3_bind_double(log_stmt, 2, (double)threshold);

      while (sqlite3_step(log_stmt) == SQLITE_ROW) {
         OLOG_INFO("memory_decay: pruning fact %ld (%.2f, %s): %.60s",
                   (long)sqlite3_column_int64(log_stmt, 0), sqlite3_column_double(log_stmt, 2),
                   (const char *)sqlite3_column_text(log_stmt, 3),
                   (const char *)sqlite3_column_text(log_stmt, 1));
      }
      sqlite3_finalize(log_stmt);
   }

   /* Delete low-confidence facts */
   sqlite3_stmt *stmt = NULL;
   rc = sqlite3_prepare_v2(
       s_db.db,
       "DELETE FROM memory_facts WHERE user_id = ? AND confidence < ? AND superseded_by IS NULL",
       -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("memory_db: prune_low_confidence prepare failed: %s", sqlite3_errmsg(s_db.db));
      sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_double(stmt, 2, (double)threshold);

   rc = sqlite3_step(stmt);
   int deleted = sqlite3_changes(s_db.db);
   sqlite3_finalize(stmt);

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("memory_db: prune_low_confidence failed: %s", sqlite3_errmsg(s_db.db));
      sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   sqlite3_exec(s_db.db, "COMMIT", NULL, NULL, NULL);
   AUTH_DB_UNLOCK();

   if (count_out)
      *count_out = deleted;
   return MEMORY_DB_SUCCESS;
}

int memory_db_prune_old_summaries(int user_id, int retention_days, int *count_out) {
   if (count_out)
      *count_out = 0;

   AUTH_DB_LOCK_OR_FAIL();

   time_t cutoff = time(NULL) - ((time_t)retention_days * 24 * 60 * 60);

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "DELETE FROM memory_summaries WHERE user_id = ? AND created_at < ?",
                               -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("memory_db: prune_old_summaries prepare failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, (int64_t)cutoff);

   rc = sqlite3_step(stmt);
   int deleted = sqlite3_changes(s_db.db);
   sqlite3_finalize(stmt);

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("memory_db: prune_old_summaries failed: %s", sqlite3_errmsg(s_db.db));
      return MEMORY_DB_FAILURE;
   }

   if (count_out)
      *count_out = deleted;
   if (deleted > 0) {
      OLOG_INFO("memory_db: pruned %d old summaries for user %d", deleted, user_id);
   }
   return MEMORY_DB_SUCCESS;
}

int memory_db_get_all_user_ids(int *out_ids, int max_ids, int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out_ids || max_ids <= 0) {
      return MEMORY_DB_FAILURE;
   }

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "SELECT user_id FROM memory_facts "
                               "UNION SELECT user_id FROM memory_preferences",
                               -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("memory_db: get_all_user_ids prepare failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   int count = 0;
   while (count < max_ids && sqlite3_step(stmt) == SQLITE_ROW) {
      out_ids[count++] = sqlite3_column_int(stmt, 0);
   }

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = count;
   return MEMORY_DB_SUCCESS;
}

/* =============================================================================
 * Embedding Operations (Semantic Search)
 * ============================================================================= */

int memory_db_fact_update_embedding(int user_id,
                                    int64_t fact_id,
                                    const float *embedding,
                                    int dims,
                                    float norm) {
   if (!embedding || dims <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_memory_fact_update_embedding);
   sqlite3_bind_blob(s_db.stmt_memory_fact_update_embedding, 1, embedding,
                     dims * (int)sizeof(float), SQLITE_TRANSIENT);
   sqlite3_bind_double(s_db.stmt_memory_fact_update_embedding, 2, (double)norm);
   sqlite3_bind_int64(s_db.stmt_memory_fact_update_embedding, 3, fact_id);
   sqlite3_bind_int(s_db.stmt_memory_fact_update_embedding, 4, user_id);

   int rc = sqlite3_step(s_db.stmt_memory_fact_update_embedding);
   sqlite3_reset(s_db.stmt_memory_fact_update_embedding);

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("memory_db: update_embedding failed for fact %ld: %s", (long)fact_id,
                 sqlite3_errmsg(s_db.db));
      return MEMORY_DB_FAILURE;
   }

   return MEMORY_DB_SUCCESS;
}

int memory_db_fact_get_embeddings(int user_id,
                                  int expected_dims,
                                  int64_t *out_ids,
                                  float *out_embeddings,
                                  float *out_norms,
                                  int64_t *out_created_ats,
                                  int max_count,
                                  int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out_ids || !out_embeddings || !out_norms || max_count <= 0 || expected_dims <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_memory_fact_get_embeddings);
   sqlite3_bind_int(s_db.stmt_memory_fact_get_embeddings, 1, user_id);
   sqlite3_bind_int(s_db.stmt_memory_fact_get_embeddings, 2, max_count);

   int count = 0;
   int expected_bytes = expected_dims * (int)sizeof(float);

   while (count < max_count && sqlite3_step(s_db.stmt_memory_fact_get_embeddings) == SQLITE_ROW) {
      int blob_bytes = sqlite3_column_bytes(s_db.stmt_memory_fact_get_embeddings, 1);

      /* Skip dimension mismatches (model changed) */
      if (blob_bytes != expected_bytes)
         continue;

      out_ids[count] = sqlite3_column_int64(s_db.stmt_memory_fact_get_embeddings, 0);
      const void *blob = sqlite3_column_blob(s_db.stmt_memory_fact_get_embeddings, 1);
      if (blob) {
         memcpy(out_embeddings + count * expected_dims, blob, (size_t)expected_bytes);
      }
      out_norms[count] = (float)sqlite3_column_double(s_db.stmt_memory_fact_get_embeddings, 2);
      /* created_at appended col 3 (v35).  out_created_ats may be NULL when caller
       * doesn't need temporal scoring — keeps the function backward-tolerant. */
      if (out_created_ats) {
         out_created_ats[count] = sqlite3_column_int64(s_db.stmt_memory_fact_get_embeddings, 3);
      }
      count++;
   }

   sqlite3_reset(s_db.stmt_memory_fact_get_embeddings);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = count;
   return MEMORY_DB_SUCCESS;
}

int memory_db_fact_list_without_embedding(int user_id,
                                          int expected_dims,
                                          int64_t *out_ids,
                                          char out_texts[][512],
                                          int max_count,
                                          int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out_ids || !out_texts || max_count <= 0 || expected_dims <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_memory_fact_list_without_embedding);
   sqlite3_bind_int(s_db.stmt_memory_fact_list_without_embedding, 1, user_id);
   sqlite3_bind_int(s_db.stmt_memory_fact_list_without_embedding, 2, expected_dims);
   sqlite3_bind_int(s_db.stmt_memory_fact_list_without_embedding, 3, max_count);

   int count = 0;
   while (count < max_count &&
          sqlite3_step(s_db.stmt_memory_fact_list_without_embedding) == SQLITE_ROW) {
      out_ids[count] = sqlite3_column_int64(s_db.stmt_memory_fact_list_without_embedding, 0);
      const char *text = (const char *)sqlite3_column_text(
          s_db.stmt_memory_fact_list_without_embedding, 1);
      if (text) {
         strncpy(out_texts[count], text, 511);
         out_texts[count][511] = '\0';
      } else {
         out_texts[count][0] = '\0';
      }
      count++;
   }

   sqlite3_reset(s_db.stmt_memory_fact_list_without_embedding);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = count;
   return MEMORY_DB_SUCCESS;
}

/* =============================================================================
 * Entity Graph Operations
 * ============================================================================= */

void memory_make_canonical_name(const char *name, char *out, size_t size) {
   if (!name || !out || size == 0)
      return;

   size_t j = 0;
   for (size_t i = 0; name[i] != '\0' && j < size - 1; i++) {
      unsigned char c = (unsigned char)name[i];
      if (c >= 0x80) {
         /* Preserve multibyte UTF-8 as-is */
         out[j++] = (char)c;
      } else {
         out[j++] = (char)tolower(c);
      }
   }

   /* Trim trailing spaces */
   while (j > 0 && out[j - 1] == ' ') {
      j--;
   }

   out[j] = '\0';
}

int memory_db_entity_upsert(int user_id,
                            const char *name,
                            const char *entity_type,
                            const char *canonical_name,
                            bool *out_created,
                            int64_t *id_out) {
   if (id_out)
      *id_out = 0;
   if (!name || !entity_type || !canonical_name)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_memory_entity_upsert);
   sqlite3_bind_int(s_db.stmt_memory_entity_upsert, 1, user_id);
   sqlite3_bind_text(s_db.stmt_memory_entity_upsert, 2, name, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(s_db.stmt_memory_entity_upsert, 3, entity_type, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(s_db.stmt_memory_entity_upsert, 4, canonical_name, -1, SQLITE_TRANSIENT);

   int64_t entity_id = 0;
   int rc = sqlite3_step(s_db.stmt_memory_entity_upsert);
   if (rc == SQLITE_ROW) {
      entity_id = sqlite3_column_int64(s_db.stmt_memory_entity_upsert, 0);
      int mention_count = sqlite3_column_int(s_db.stmt_memory_entity_upsert, 1);
      if (out_created) {
         *out_created = (mention_count == 1);
      }
   } else {
      OLOG_ERROR("memory_db: entity upsert failed: %s", sqlite3_errmsg(s_db.db));
      sqlite3_reset(s_db.stmt_memory_entity_upsert);
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   sqlite3_reset(s_db.stmt_memory_entity_upsert);
   AUTH_DB_UNLOCK();
   if (id_out)
      *id_out = entity_id;
   return MEMORY_DB_SUCCESS;
}

int memory_db_entity_get_by_name(int user_id,
                                 const char *canonical_name,
                                 memory_entity_t *out_entity) {
   if (!canonical_name || !out_entity)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_memory_entity_get_by_name);
   sqlite3_bind_int(s_db.stmt_memory_entity_get_by_name, 1, user_id);
   sqlite3_bind_text(s_db.stmt_memory_entity_get_by_name, 2, canonical_name, -1, SQLITE_TRANSIENT);

   int rc = sqlite3_step(s_db.stmt_memory_entity_get_by_name);
   int result;
   if (rc == SQLITE_ROW) {
      populate_entity_from_row(s_db.stmt_memory_entity_get_by_name, out_entity);
      result = MEMORY_DB_SUCCESS;
   } else if (rc == SQLITE_DONE) {
      result = MEMORY_DB_NOT_FOUND;
   } else {
      OLOG_ERROR("memory_db: entity_get_by_name failed: %s", sqlite3_errmsg(s_db.db));
      result = MEMORY_DB_FAILURE;
   }

   sqlite3_reset(s_db.stmt_memory_entity_get_by_name);
   AUTH_DB_UNLOCK();
   return result;
}

int memory_db_entity_update_embedding(int64_t entity_id,
                                      int user_id,
                                      const float *embedding,
                                      int dims,
                                      float norm) {
   if (!embedding || dims <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_memory_entity_update_embedding);
   sqlite3_bind_blob(s_db.stmt_memory_entity_update_embedding, 1, embedding,
                     dims * (int)sizeof(float), SQLITE_TRANSIENT);
   sqlite3_bind_double(s_db.stmt_memory_entity_update_embedding, 2, (double)norm);
   sqlite3_bind_int64(s_db.stmt_memory_entity_update_embedding, 3, entity_id);
   sqlite3_bind_int(s_db.stmt_memory_entity_update_embedding, 4, user_id);

   int rc = sqlite3_step(s_db.stmt_memory_entity_update_embedding);
   sqlite3_reset(s_db.stmt_memory_entity_update_embedding);
   AUTH_DB_UNLOCK();

   return (rc == SQLITE_DONE) ? MEMORY_DB_SUCCESS : MEMORY_DB_FAILURE;
}

/* =============================================================================
 * Contradiction handling — lives in three coordinated locations:
 *
 *   (1) HERE: EXCLUSIVE_RELATIONS[] + CONTRADICTORY_PAIRS[] tables (compile-time
 *       data) and memory_db_relation_supersede() (atomic close + create
 *       transaction; out_old_fact_id surfaces the linked fact for the caller
 *       to propagate).
 *
 *   (2) src/memory/memory_extraction.c — process_extracted_relation()
 *       (around the fact_map build / find_fact_for_relation() call site,
 *       ~line 765 in current tree): receives the old fact_id from (1) and
 *       calls memory_db_fact_supersede() to mark the linked fact as stale,
 *       so the structured-relation supersede also retires the natural-
 *       language fact rendered from it.
 *
 * Adding a new exclusive relation or contradictory pair is a (1)-only edit.
 * Changing the supersede-propagation mechanism is a (1)+(2) edit; if a
 * fourth piece lands (e.g. inline LLM-based contradiction judgment for
 * subtle quantity / negation cases — see dawn/docs/TODO.md "LLM-based
 * contradiction judgment"), this whole cluster should move into a
 * dedicated memory_contradiction.{c,h} module.
 *
 * Exclusive relations have at-most-one open instance per (subject, relation).
 * When extraction stores a new such relation with a different object,
 * memory_db_relation_supersede() auto-closes the prior open row.
 * Non-exclusive relations (likes, knows, has_pet) skip the close branch.
 * ============================================================================= */
static const char *EXCLUSIVE_RELATIONS[] = {
   "works_at",      "lives_in",         "married_to", "attends_school",
   "owns_vehicle",  "born_in",          "born_on",    "favorite_color",
   "favorite_food", "primary_language", "email_is",   "phone_number_is",
};
static const int EXCLUSIVE_RELATIONS_COUNT = (int)(sizeof(EXCLUSIVE_RELATIONS) /
                                                   sizeof(EXCLUSIVE_RELATIONS[0]));

static const struct {
   const char *a;
   const char *b;
} CONTRADICTORY_PAIRS[] = {
   { "likes", "dislikes" },
   { "enjoys", "hates" },
   { "can", "cannot" },
   { "is", "is_not" },
};
static const int CONTRADICTORY_PAIRS_COUNT = (int)(sizeof(CONTRADICTORY_PAIRS) /
                                                   sizeof(CONTRADICTORY_PAIRS[0]));

static const char *contradictory_opposite(const char *relation) {
   if (!relation)
      return NULL;
   for (int i = 0; i < CONTRADICTORY_PAIRS_COUNT; i++) {
      if (strcmp(relation, CONTRADICTORY_PAIRS[i].a) == 0)
         return CONTRADICTORY_PAIRS[i].b;
      if (strcmp(relation, CONTRADICTORY_PAIRS[i].b) == 0)
         return CONTRADICTORY_PAIRS[i].a;
   }
   return NULL;
}

/* Public-named helper: alias surface (memory_db_alias.c, v43) consults this to
 * identify open exclusive relations during Stage 5 of the resolver cascade.
 * EXCLUSIVE_RELATIONS[] stays static here as the source-of-truth list. */
bool memory_db_relation_is_exclusive(const char *relation) {
   if (!relation)
      return false;
   for (int i = 0; i < EXCLUSIVE_RELATIONS_COUNT; i++) {
      if (strcmp(relation, EXCLUSIVE_RELATIONS[i]) == 0)
         return true;
   }
   return false;
}

/* Internal-name shim — keeps the existing call sites in this file unchanged
 * while the public name becomes the canonical entry point. */
static inline bool relation_is_exclusive(const char *relation) {
   return memory_db_relation_is_exclusive(relation);
}

int memory_db_relation_create(int user_id,
                              int64_t subject_entity_id,
                              const char *relation,
                              int64_t object_entity_id,
                              const char *object_value,
                              int64_t fact_id,
                              float confidence,
                              int64_t valid_from,
                              int64_t valid_to,
                              const memory_provenance_t *prov) {
   if (!relation)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_memory_relation_create);
   sqlite3_bind_int(s_db.stmt_memory_relation_create, 1, user_id);
   sqlite3_bind_int64(s_db.stmt_memory_relation_create, 2, subject_entity_id);
   sqlite3_bind_text(s_db.stmt_memory_relation_create, 3, relation, -1, SQLITE_TRANSIENT);

   if (object_entity_id > 0) {
      sqlite3_bind_int64(s_db.stmt_memory_relation_create, 4, object_entity_id);
   } else {
      sqlite3_bind_null(s_db.stmt_memory_relation_create, 4);
   }

   if (object_value) {
      sqlite3_bind_text(s_db.stmt_memory_relation_create, 5, object_value, -1, SQLITE_TRANSIENT);
   } else {
      sqlite3_bind_null(s_db.stmt_memory_relation_create, 5);
   }

   if (fact_id > 0) {
      sqlite3_bind_int64(s_db.stmt_memory_relation_create, 6, fact_id);
   } else {
      sqlite3_bind_null(s_db.stmt_memory_relation_create, 6);
   }

   sqlite3_bind_double(s_db.stmt_memory_relation_create, 7, (double)confidence);

   if (valid_from > 0) {
      sqlite3_bind_int64(s_db.stmt_memory_relation_create, 8, valid_from);
   } else {
      sqlite3_bind_null(s_db.stmt_memory_relation_create, 8);
   }
   if (valid_to > 0) {
      sqlite3_bind_int64(s_db.stmt_memory_relation_create, 9, valid_to);
   } else {
      sqlite3_bind_null(s_db.stmt_memory_relation_create, 9);
   }
   bind_provenance(s_db.stmt_memory_relation_create, 10, prov);

   int rc = sqlite3_step(s_db.stmt_memory_relation_create);
   sqlite3_reset(s_db.stmt_memory_relation_create);
   AUTH_DB_UNLOCK();

   return (rc == SQLITE_DONE) ? MEMORY_DB_SUCCESS : MEMORY_DB_FAILURE;
}

/* Transactional close-and-create.  For exclusive relations (works_at, lives_in, ...)
 * any existing open row with a different object is closed (valid_to = now()) before
 * the new row is inserted.  Both writes happen under one BEGIN IMMEDIATE so other
 * workers can never observe a state with zero open rows for the same (subject, relation).
 *
 * Non-exclusive relations skip the close branch — multiple open rows are valid
 * (a user can like many things, know many people). */
int memory_db_relation_supersede(int user_id,
                                 int64_t subject_entity_id,
                                 const char *relation,
                                 int64_t object_entity_id,
                                 const char *object_value,
                                 int64_t fact_id,
                                 float confidence,
                                 int64_t valid_from,
                                 int64_t valid_to,
                                 const memory_provenance_t *prov,
                                 int64_t *out_old_fact_id) {
   if (out_old_fact_id)
      *out_old_fact_id = 0;
   if (!relation)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_FAIL();

   /* Begin transaction.  Any failure rolls back and leaves the graph unchanged. */
   char *errmsg = NULL;
   int rc = sqlite3_exec(s_db.db, "BEGIN IMMEDIATE", NULL, NULL, &errmsg);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("memory_db: relation_supersede begin failed: %s", errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   /* Auto-close prior open exclusive relation, if any.  Idempotency check (object
    * mismatch) lives in the prepared statement's WHERE clause.
    *
    * Two orthogonal questions govern whether we close and, if so, at what time:
    *
    *   1. Does the new relation represent a currently-true state?
    *        Yes: valid_to == 0 (open-ended) OR valid_to > now().
    *        No:  valid_to is in the past (bounded historical slice).
    *      Only currently-true relations should supersede the existing open row.
    *      Ingesting a bounded historical fact ("Alice worked at Google 2018-2020")
    *      must NOT close the present-day Microsoft row — that would erase reality.
    *
    *   2. If we do close, at what boundary?
    *        - new valid_from when provided (avoids overlapping validity windows)
    *        - now() otherwise
    */
   int64_t now_ts = (int64_t)time(NULL);
   bool new_is_current = (valid_to <= 0) || (valid_to > now_ts);
   if (relation_is_exclusive(relation) && new_is_current) {
      int64_t close_time = (valid_from > 0) ? valid_from : now_ts;
      sqlite3_stmt *close_stmt = s_db.stmt_memory_relation_close_open;
      sqlite3_reset(close_stmt);
      sqlite3_bind_int64(close_stmt, 1, close_time);
      sqlite3_bind_int(close_stmt, 2, user_id);
      sqlite3_bind_int64(close_stmt, 3, subject_entity_id);
      sqlite3_bind_text(close_stmt, 4, relation, -1, SQLITE_TRANSIENT);
      if (object_entity_id > 0) {
         sqlite3_bind_int64(close_stmt, 5, object_entity_id);
      } else {
         sqlite3_bind_null(close_stmt, 5);
      }
      if (object_value) {
         sqlite3_bind_text(close_stmt, 6, object_value, -1, SQLITE_TRANSIENT);
      } else {
         sqlite3_bind_null(close_stmt, 6);
      }
      /* RETURNING fact_id yields SQLITE_ROW for each closed row */
      int64_t old_fact_id_local = 0;
      rc = sqlite3_step(close_stmt);
      if (rc == SQLITE_ROW) {
         old_fact_id_local = sqlite3_column_int64(close_stmt, 0);
         while (sqlite3_step(close_stmt) == SQLITE_ROW) {
         }
         rc = SQLITE_DONE;
      }
      sqlite3_reset(close_stmt);
      if (rc != SQLITE_DONE) {
         OLOG_ERROR("memory_db: relation_supersede close failed: %s", sqlite3_errmsg(s_db.db));
         sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
         AUTH_DB_UNLOCK();
         return MEMORY_DB_FAILURE;
      }
      int closed = sqlite3_changes(s_db.db);
      if (closed > 0) {
         OLOG_INFO("memory_db: closed %d superseded '%s' relation(s) for subject %ld", closed,
                   relation, (long)subject_entity_id);
         if (out_old_fact_id && old_fact_id_local > 0)
            *out_old_fact_id = old_fact_id_local;
      }
   }

   /* Close contradictory relation (e.g., likes↔dislikes) on the same
    * (subject, object) pair.  Only applies to currently-true new relations. */
   const char *opposite = contradictory_opposite(relation);
   if (opposite && new_is_current) {
      sqlite3_stmt *cstmt = NULL;
      int rc2 = sqlite3_prepare_v2(s_db.db,
                                   "UPDATE memory_relations SET valid_to = ? "
                                   "WHERE user_id = ? AND subject_entity_id = ? AND relation = ? "
                                   "  AND valid_to IS NULL "
                                   "  AND COALESCE(object_entity_id, 0) = COALESCE(?, 0) "
                                   "  AND COALESCE(object_value, '') = COALESCE(?, '') "
                                   "RETURNING fact_id",
                                   -1, &cstmt, NULL);
      if (rc2 != SQLITE_OK) {
         OLOG_ERROR("memory_db: prepare contradictory close failed: %s", sqlite3_errmsg(s_db.db));
         sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
         AUTH_DB_UNLOCK();
         return MEMORY_DB_FAILURE;
      } else {
         int64_t close_time = (valid_from > 0) ? valid_from : now_ts;
         sqlite3_bind_int64(cstmt, 1, close_time);
         sqlite3_bind_int(cstmt, 2, user_id);
         sqlite3_bind_int64(cstmt, 3, subject_entity_id);
         sqlite3_bind_text(cstmt, 4, opposite, -1, SQLITE_TRANSIENT);
         if (object_entity_id > 0)
            sqlite3_bind_int64(cstmt, 5, object_entity_id);
         else
            sqlite3_bind_null(cstmt, 5);
         if (object_value)
            sqlite3_bind_text(cstmt, 6, object_value, -1, SQLITE_TRANSIENT);
         else
            sqlite3_bind_null(cstmt, 6);

         rc2 = sqlite3_step(cstmt);
         if (rc2 == SQLITE_ROW) {
            int64_t contra_fact_id = sqlite3_column_int64(cstmt, 0);
            do {
               rc2 = sqlite3_step(cstmt);
            } while (rc2 == SQLITE_ROW);
            if (rc2 != SQLITE_DONE) {
               OLOG_ERROR("memory_db: contradictory close step failed: %s",
                          sqlite3_errmsg(s_db.db));
               sqlite3_finalize(cstmt);
               sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
               AUTH_DB_UNLOCK();
               return MEMORY_DB_FAILURE;
            }
            if (out_old_fact_id && *out_old_fact_id == 0 && contra_fact_id > 0)
               *out_old_fact_id = contra_fact_id;
            OLOG_INFO("memory_db: closed contradictory '%s' (opposite of '%s') "
                      "for subject %ld",
                      opposite, relation, (long)subject_entity_id);
         } else if (rc2 != SQLITE_DONE) {
            OLOG_ERROR("memory_db: contradictory close step failed: %s", sqlite3_errmsg(s_db.db));
            sqlite3_finalize(cstmt);
            sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
            AUTH_DB_UNLOCK();
            return MEMORY_DB_FAILURE;
         }
         sqlite3_finalize(cstmt);
      }
   }

   /* Insert the new row using the existing prepared statement.  Inlined here
    * because we hold the auth_db lock; can't call memory_db_relation_create which
    * would re-acquire it. */
   sqlite3_stmt *create_stmt = s_db.stmt_memory_relation_create;
   sqlite3_reset(create_stmt);
   sqlite3_bind_int(create_stmt, 1, user_id);
   sqlite3_bind_int64(create_stmt, 2, subject_entity_id);
   sqlite3_bind_text(create_stmt, 3, relation, -1, SQLITE_TRANSIENT);
   if (object_entity_id > 0) {
      sqlite3_bind_int64(create_stmt, 4, object_entity_id);
   } else {
      sqlite3_bind_null(create_stmt, 4);
   }
   if (object_value) {
      sqlite3_bind_text(create_stmt, 5, object_value, -1, SQLITE_TRANSIENT);
   } else {
      sqlite3_bind_null(create_stmt, 5);
   }
   if (fact_id > 0) {
      sqlite3_bind_int64(create_stmt, 6, fact_id);
   } else {
      sqlite3_bind_null(create_stmt, 6);
   }
   sqlite3_bind_double(create_stmt, 7, (double)confidence);
   if (valid_from > 0) {
      sqlite3_bind_int64(create_stmt, 8, valid_from);
   } else {
      sqlite3_bind_null(create_stmt, 8);
   }
   if (valid_to > 0) {
      sqlite3_bind_int64(create_stmt, 9, valid_to);
   } else {
      sqlite3_bind_null(create_stmt, 9);
   }
   bind_provenance(create_stmt, 10, prov);
   rc = sqlite3_step(create_stmt);
   sqlite3_reset(create_stmt);
   if (rc != SQLITE_DONE) {
      int xrc = sqlite3_extended_errcode(s_db.db);
      OLOG_ERROR("memory_db: relation_supersede insert failed: %s "
                 "(rc=%d xrc=%d user=%d subj=%lld rel='%s' obj_id=%lld obj_val='%s' "
                 "fact_id=%lld conv=%lld msg_range=[%lld..%lld] valid=[%lld..%lld])",
                 sqlite3_errmsg(s_db.db), rc, xrc, user_id, (long long)subject_entity_id, relation,
                 (long long)object_entity_id, object_value ? object_value : "(null)",
                 (long long)fact_id, prov ? (long long)prov->conv_id : 0LL,
                 prov ? (long long)prov->msg_id_start : 0LL,
                 prov ? (long long)prov->msg_id_end : 0LL, (long long)valid_from,
                 (long long)valid_to);
      sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   rc = sqlite3_exec(s_db.db, "COMMIT", NULL, NULL, &errmsg);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("memory_db: relation_supersede commit failed: %s", errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   AUTH_DB_UNLOCK();
   return MEMORY_DB_SUCCESS;
}

/* Reads SELECTs ordered: id, subject_entity_id, relation, object_entity_id,
 * object_name, confidence, valid_from, valid_to.  valid_* columns appended last
 * (cols 6, 7, v33).  COALESCE in SQL converts SQL NULL → 0 in C. */
static void populate_relation_from_row(sqlite3_stmt *stmt, memory_relation_t *rel) {
   rel->id = sqlite3_column_int64(stmt, 0);
   rel->subject_entity_id = sqlite3_column_int64(stmt, 1);

   const char *r = (const char *)sqlite3_column_text(stmt, 2);
   if (r) {
      strncpy(rel->relation, r, MEMORY_RELATION_MAX - 1);
      rel->relation[MEMORY_RELATION_MAX - 1] = '\0';
   }

   rel->object_entity_id = sqlite3_column_int64(stmt, 3);

   const char *obj_name = (const char *)sqlite3_column_text(stmt, 4);
   if (obj_name) {
      strncpy(rel->object_name, obj_name, MEMORY_ENTITY_NAME_MAX - 1);
      rel->object_name[MEMORY_ENTITY_NAME_MAX - 1] = '\0';
   }

   rel->confidence = (float)sqlite3_column_double(stmt, 5);

   /* COALESCE in SQL means 0 if NULL — caller treats 0 as "open-ended". */
   rel->valid_from = sqlite3_column_int64(stmt, 6);
   rel->valid_to = sqlite3_column_int64(stmt, 7);
}

int memory_db_relation_list_by_subject(int user_id,
                                       int64_t subject_entity_id,
                                       memory_relation_t *out,
                                       int max,
                                       int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out || max <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_memory_relation_list_by_subject);
   sqlite3_bind_int(s_db.stmt_memory_relation_list_by_subject, 1, user_id);
   sqlite3_bind_int64(s_db.stmt_memory_relation_list_by_subject, 2, subject_entity_id);
   sqlite3_bind_int(s_db.stmt_memory_relation_list_by_subject, 3, max);

   int count = 0;
   while (count < max && sqlite3_step(s_db.stmt_memory_relation_list_by_subject) == SQLITE_ROW) {
      populate_relation_from_row(s_db.stmt_memory_relation_list_by_subject, &out[count]);
      count++;
   }

   sqlite3_reset(s_db.stmt_memory_relation_list_by_subject);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = count;
   return MEMORY_DB_SUCCESS;
}

int memory_db_relation_list_by_object(int user_id,
                                      int64_t object_entity_id,
                                      memory_relation_t *out,
                                      int max,
                                      int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out || max <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_memory_relation_list_by_object);
   sqlite3_bind_int(s_db.stmt_memory_relation_list_by_object, 1, user_id);
   sqlite3_bind_int64(s_db.stmt_memory_relation_list_by_object, 2, object_entity_id);
   sqlite3_bind_int(s_db.stmt_memory_relation_list_by_object, 3, max);

   int count = 0;
   while (count < max && sqlite3_step(s_db.stmt_memory_relation_list_by_object) == SQLITE_ROW) {
      populate_relation_from_row(s_db.stmt_memory_relation_list_by_object, &out[count]);
      count++;
   }

   sqlite3_reset(s_db.stmt_memory_relation_list_by_object);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = count;
   return MEMORY_DB_SUCCESS;
}

/* As-of variant.  When as_of_ts == 0, defaults to "currently valid".  Used by
 * the entity-recall block in memory_callback when the LLM passes as_of, and as
 * the default temporal filter for current-state queries. */
int memory_db_relation_list_by_subject_at(int user_id,
                                          int64_t subject_entity_id,
                                          int64_t as_of_ts,
                                          memory_relation_t *out,
                                          int max,
                                          int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out || max <= 0)
      return MEMORY_DB_FAILURE;

   if (as_of_ts <= 0)
      as_of_ts = (int64_t)time(NULL);

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_memory_relation_list_by_subject_at;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, subject_entity_id);
   sqlite3_bind_int64(stmt, 3, as_of_ts);
   sqlite3_bind_int64(stmt, 4, as_of_ts);
   sqlite3_bind_int(stmt, 5, max);

   int count = 0;
   while (count < max && sqlite3_step(stmt) == SQLITE_ROW) {
      populate_relation_from_row(stmt, &out[count]);
      count++;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = count;
   return MEMORY_DB_SUCCESS;
}

int memory_db_relation_list_all_by_user(int user_id,
                                        memory_relation_t *out,
                                        int max,
                                        int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out || max <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_FAIL();

   /* Single query: all relations for this user, ordered by subject for grouping.
    * valid_from/valid_to appended last (v33) to match populate_relation_from_row order. */
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "SELECT r.id, r.subject_entity_id, r.relation, r.object_entity_id, "
                               "COALESCE(e.name, r.object_value) AS object_name, r.confidence, "
                               "COALESCE(r.valid_from, 0), COALESCE(r.valid_to, 0) "
                               "FROM memory_relations r "
                               "LEFT JOIN memory_entities e ON r.object_entity_id = e.id "
                               "WHERE r.user_id = ? ORDER BY r.subject_entity_id LIMIT ?",
                               -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int(stmt, 2, max);

   int count = 0;
   while (count < max && sqlite3_step(stmt) == SQLITE_ROW) {
      populate_relation_from_row(stmt, &out[count]);
      count++;
   }

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = count;
   return MEMORY_DB_SUCCESS;
}

static void populate_entity_from_row(sqlite3_stmt *stmt, memory_entity_t *entity) {
   entity->id = sqlite3_column_int64(stmt, 0);
   entity->user_id = sqlite3_column_int(stmt, 1);

   const char *name = (const char *)sqlite3_column_text(stmt, 2);
   if (name) {
      strncpy(entity->name, name, MEMORY_ENTITY_NAME_MAX - 1);
      entity->name[MEMORY_ENTITY_NAME_MAX - 1] = '\0';
   }

   const char *type = (const char *)sqlite3_column_text(stmt, 3);
   if (type) {
      strncpy(entity->entity_type, type, MEMORY_ENTITY_TYPE_MAX - 1);
      entity->entity_type[MEMORY_ENTITY_TYPE_MAX - 1] = '\0';
   }

   const char *cname = (const char *)sqlite3_column_text(stmt, 4);
   if (cname) {
      strncpy(entity->canonical_name, cname, MEMORY_ENTITY_NAME_MAX - 1);
      entity->canonical_name[MEMORY_ENTITY_NAME_MAX - 1] = '\0';
   }

   entity->mention_count = sqlite3_column_int(stmt, 5);
   entity->first_seen = (time_t)sqlite3_column_int64(stmt, 6);
   entity->last_seen = (time_t)sqlite3_column_int64(stmt, 7);
}

int memory_db_entity_list(int user_id, memory_entity_t *out, int max, int offset, int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out || max <= 0)
      return MEMORY_DB_FAILURE;

   /* Reuse entity_search statement with "%" pattern to match all entities */
   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_memory_entity_search);
   sqlite3_bind_int(s_db.stmt_memory_entity_search, 1, user_id);
   sqlite3_bind_text(s_db.stmt_memory_entity_search, 2, "%", -1, SQLITE_STATIC);
   sqlite3_bind_int(s_db.stmt_memory_entity_search, 3, max);
   sqlite3_bind_int(s_db.stmt_memory_entity_search, 4, offset);

   int count = 0;
   while (count < max && sqlite3_step(s_db.stmt_memory_entity_search) == SQLITE_ROW) {
      populate_entity_from_row(s_db.stmt_memory_entity_search, &out[count]);
      count++;
   }

   sqlite3_reset(s_db.stmt_memory_entity_search);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = count;
   return MEMORY_DB_SUCCESS;
}

int memory_db_entity_search(int user_id,
                            const char *keywords,
                            memory_entity_t *out,
                            int max,
                            int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!keywords || !out || max <= 0)
      return MEMORY_DB_FAILURE;

   char pattern[256];
   build_like_pattern(keywords, pattern, sizeof(pattern));

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_memory_entity_search);
   sqlite3_bind_int(s_db.stmt_memory_entity_search, 1, user_id);
   sqlite3_bind_text(s_db.stmt_memory_entity_search, 2, pattern, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(s_db.stmt_memory_entity_search, 3, max);
   sqlite3_bind_int(s_db.stmt_memory_entity_search, 4, 0); /* OFFSET — shared stmt has 4 params */

   int count = 0;
   while (count < max && sqlite3_step(s_db.stmt_memory_entity_search) == SQLITE_ROW) {
      populate_entity_from_row(s_db.stmt_memory_entity_search, &out[count]);
      count++;
   }

   sqlite3_reset(s_db.stmt_memory_entity_search);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = count;
   return MEMORY_DB_SUCCESS;
}

int memory_db_entity_set_photo(int user_id, int64_t entity_id, const char *photo_id) {
   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = s_db.stmt_memory_entity_set_photo;
   sqlite3_reset(stmt);

   if (photo_id && photo_id[0]) {
      sqlite3_bind_text(stmt, 1, photo_id, -1, SQLITE_TRANSIENT);
   } else {
      sqlite3_bind_null(stmt, 1);
   }
   sqlite3_bind_int64(stmt, 2, entity_id);
   sqlite3_bind_int(stmt, 3, user_id);

   int rc = sqlite3_step(stmt);
   int changes = sqlite3_changes(s_db.db);
   sqlite3_reset(stmt);

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("memory_db: entity_set_photo failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   AUTH_DB_UNLOCK();
   return (changes > 0) ? MEMORY_DB_SUCCESS : MEMORY_DB_NOT_FOUND;
}

int memory_db_entity_get_photo(int user_id,
                               int64_t entity_id,
                               char *out_photo_id,
                               size_t photo_id_size) {
   if (!out_photo_id || photo_id_size == 0) {
      return MEMORY_DB_FAILURE;
   }
   out_photo_id[0] = '\0';

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = s_db.stmt_memory_entity_get_photo;
   sqlite3_reset(stmt);
   sqlite3_bind_int64(stmt, 1, entity_id);
   sqlite3_bind_int(stmt, 2, user_id);

   int rc = sqlite3_step(stmt);
   if (rc == SQLITE_ROW) {
      const char *val = (const char *)sqlite3_column_text(stmt, 0);
      if (val) {
         snprintf(out_photo_id, photo_id_size, "%s", val);
      }
      sqlite3_reset(stmt);
      AUTH_DB_UNLOCK();
      return MEMORY_DB_SUCCESS;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();

   return (rc == SQLITE_DONE) ? MEMORY_DB_NOT_FOUND : MEMORY_DB_FAILURE;
}

int memory_db_entity_delete(int64_t entity_id, int user_id) {
   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   /* Delete relations where this entity is subject or object */
   sqlite3_stmt *rel_stmt = s_db.stmt_memory_relation_delete_by_entity;
   sqlite3_reset(rel_stmt);
   sqlite3_bind_int(rel_stmt, 1, user_id);
   sqlite3_bind_int64(rel_stmt, 2, entity_id);
   sqlite3_bind_int64(rel_stmt, 3, entity_id);
   int rel_rc = sqlite3_step(rel_stmt);
   sqlite3_reset(rel_stmt);
   if (rel_rc != SQLITE_DONE) {
      OLOG_ERROR("memory_db: relation delete failed for entity %ld: %s", (long)entity_id,
                 sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   /* Delete the entity itself */
   sqlite3_stmt *stmt = s_db.stmt_memory_entity_delete;
   sqlite3_reset(stmt);
   sqlite3_bind_int64(stmt, 1, entity_id);
   sqlite3_bind_int(stmt, 2, user_id);

   int rc = sqlite3_step(stmt);
   int changes = sqlite3_changes(s_db.db);
   sqlite3_reset(stmt);

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      return MEMORY_DB_FAILURE;
   }
   return (changes > 0) ? MEMORY_DB_SUCCESS : MEMORY_DB_NOT_FOUND;
}

int memory_db_entity_merge(int user_id, int64_t source_id, int64_t target_id) {
   if (source_id == target_id || source_id <= 0 || target_id <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   /* Verify both entities exist and belong to user */
   sqlite3_stmt *chk = NULL;
   int rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, mention_count, first_seen, last_seen FROM memory_entities "
       "WHERE id = ? AND user_id = ?",
       -1, &chk, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   sqlite3_bind_int64(chk, 1, source_id);
   sqlite3_bind_int(chk, 2, user_id);
   if (sqlite3_step(chk) != SQLITE_ROW) {
      sqlite3_finalize(chk);
      AUTH_DB_UNLOCK();
      return MEMORY_DB_NOT_FOUND;
   }
   int src_mentions = sqlite3_column_int(chk, 1);
   int64_t src_first_seen = sqlite3_column_int64(chk, 2);
   int64_t src_last_seen = sqlite3_column_int64(chk, 3);
   sqlite3_reset(chk);

   sqlite3_bind_int64(chk, 1, target_id);
   sqlite3_bind_int(chk, 2, user_id);
   if (sqlite3_step(chk) != SQLITE_ROW) {
      sqlite3_finalize(chk);
      AUTH_DB_UNLOCK();
      return MEMORY_DB_NOT_FOUND;
   }
   sqlite3_finalize(chk);

   /* Begin transaction */
   sqlite3_exec(s_db.db, "BEGIN IMMEDIATE", NULL, NULL, NULL);

   /* Helper macro: prepare, bind, step, finalize — ROLLBACK on any failure */
#define MERGE_EXEC(sql, bind_block)                         \
   do {                                                     \
      sqlite3_stmt *_s = NULL;                              \
      rc = sqlite3_prepare_v2(s_db.db, sql, -1, &_s, NULL); \
      if (rc != SQLITE_OK)                                  \
         goto merge_fail;                                   \
      bind_block;                                           \
      rc = sqlite3_step(_s);                                \
      sqlite3_finalize(_s);                                 \
      if (rc != SQLITE_DONE)                                \
         goto merge_fail;                                   \
   } while (0)

   /* Reassign relations: subject */
   MERGE_EXEC("UPDATE memory_relations SET subject_entity_id = ? "
              "WHERE subject_entity_id = ? AND user_id = ?",
              {
                 sqlite3_bind_int64(_s, 1, target_id);
                 sqlite3_bind_int64(_s, 2, source_id);
                 sqlite3_bind_int(_s, 3, user_id);
              });

   /* Reassign relations: object */
   MERGE_EXEC("UPDATE memory_relations SET object_entity_id = ? "
              "WHERE object_entity_id = ? AND user_id = ?",
              {
                 sqlite3_bind_int64(_s, 1, target_id);
                 sqlite3_bind_int64(_s, 2, source_id);
                 sqlite3_bind_int(_s, 3, user_id);
              });

   /* Reassign contacts */
   MERGE_EXEC("UPDATE contacts SET entity_id = ? "
              "WHERE entity_id = ? AND user_id = ?",
              {
                 sqlite3_bind_int64(_s, 1, target_id);
                 sqlite3_bind_int64(_s, 2, source_id);
                 sqlite3_bind_int(_s, 3, user_id);
              });

   /* Delete self-referencing relations (subject == object == target) */
   MERGE_EXEC("DELETE FROM memory_relations WHERE user_id = ? "
              "AND subject_entity_id = ? AND object_entity_id = ?",
              {
                 sqlite3_bind_int(_s, 1, user_id);
                 sqlite3_bind_int64(_s, 2, target_id);
                 sqlite3_bind_int64(_s, 3, target_id);
              });

   /* Deduplicate relations: keep highest confidence per unique relation tuple */
   MERGE_EXEC("DELETE FROM memory_relations WHERE id IN ("
              "  SELECT id FROM ("
              "    SELECT id, ROW_NUMBER() OVER ("
              "      PARTITION BY subject_entity_id, relation, object_entity_id, "
              "        COALESCE(object_value, '') "
              "      ORDER BY confidence DESC, id ASC"
              "    ) AS rn FROM memory_relations "
              "    WHERE user_id = ? AND (subject_entity_id = ? OR object_entity_id = ?)"
              "  ) WHERE rn > 1"
              ")",
              {
                 sqlite3_bind_int(_s, 1, user_id);
                 sqlite3_bind_int64(_s, 2, target_id);
                 sqlite3_bind_int64(_s, 3, target_id);
              });

   /* Deduplicate contacts: keep oldest per (entity_id, field_type, value) */
   MERGE_EXEC("DELETE FROM contacts WHERE id IN ("
              "  SELECT id FROM ("
              "    SELECT id, ROW_NUMBER() OVER ("
              "      PARTITION BY entity_id, field_type, value "
              "      ORDER BY id ASC"
              "    ) AS rn FROM contacts "
              "    WHERE user_id = ? AND entity_id = ?"
              "  ) WHERE rn > 1"
              ")",
              {
                 sqlite3_bind_int(_s, 1, user_id);
                 sqlite3_bind_int64(_s, 2, target_id);
              });

   /* Update target: absorb mention count and time range */
   MERGE_EXEC("UPDATE memory_entities SET "
              "mention_count = mention_count + ?, "
              "first_seen = MIN(first_seen, ?), "
              "last_seen = MAX(COALESCE(last_seen, 0), ?) "
              "WHERE id = ? AND user_id = ?",
              {
                 sqlite3_bind_int(_s, 1, src_mentions);
                 sqlite3_bind_int64(_s, 2, src_first_seen);
                 sqlite3_bind_int64(_s, 3, src_last_seen);
                 sqlite3_bind_int64(_s, 4, target_id);
                 sqlite3_bind_int(_s, 5, user_id);
              });

   /* Delete source entity */
   MERGE_EXEC("DELETE FROM memory_entities WHERE id = ? AND user_id = ?", {
      sqlite3_bind_int64(_s, 1, source_id);
      sqlite3_bind_int(_s, 2, user_id);
   });

#undef MERGE_EXEC

   sqlite3_exec(s_db.db, "COMMIT", NULL, NULL, NULL);
   AUTH_DB_UNLOCK();

   /* Invalidate the entity-embedding cache so subsequent reads don't return
    * phantom entries for the deleted source row.  The invalidator is an
    * atomic dirty-bit flip — no auth_db lock involvement, no self-deadlock
    * risk — so it fires safely after AUTH_DB_UNLOCK() per the contract in
    * docs/ENTITY_MERGE_DESIGN.md §12.  Rollback path skips invalidation
    * because ROLLBACK reverses the in-progress UPDATEs and the cache state
    * is consistent with the pre-merge DB. */
   memory_embeddings_invalidate_entity_cache();

   OLOG_INFO("memory_db: merged entity %lld into %lld for user %d", (long long)source_id,
             (long long)target_id, user_id);
   return MEMORY_DB_SUCCESS;

merge_fail:
   OLOG_ERROR("memory_db: entity merge failed at step rc=%d: %s", rc, sqlite3_errmsg(s_db.db));
   sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
   AUTH_DB_UNLOCK();
   return MEMORY_DB_FAILURE;
}

int memory_db_entity_get_embeddings(int user_id,
                                    bool include_aliases,
                                    int expected_dims,
                                    int64_t *out_ids,
                                    char out_names[][MEMORY_ENTITY_NAME_MAX],
                                    char out_types[][MEMORY_ENTITY_TYPE_MAX],
                                    float *out_embeddings,
                                    float *out_norms,
                                    int max,
                                    int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out_ids || !out_names || !out_types || !out_embeddings || !out_norms || max <= 0 ||
       expected_dims <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_FAIL();

   /* Bindings: 1 = user_id, 2 = include_aliases (0/1 — drives the
    * "(? = 1 OR canonical_id IS NULL)" filter in the prepared SQL),
    * 3 = LIMIT.  See auth_db_core.c prepare site for the v43 SQL shape. */
   sqlite3_reset(s_db.stmt_memory_entity_get_embeddings);
   sqlite3_bind_int(s_db.stmt_memory_entity_get_embeddings, 1, user_id);
   sqlite3_bind_int(s_db.stmt_memory_entity_get_embeddings, 2, include_aliases ? 1 : 0);
   sqlite3_bind_int(s_db.stmt_memory_entity_get_embeddings, 3, max);

   int count = 0;
   int expected_bytes = expected_dims * (int)sizeof(float);

   while (count < max && sqlite3_step(s_db.stmt_memory_entity_get_embeddings) == SQLITE_ROW) {
      int blob_bytes = sqlite3_column_bytes(s_db.stmt_memory_entity_get_embeddings, 3);
      if (blob_bytes != expected_bytes)
         continue;

      out_ids[count] = sqlite3_column_int64(s_db.stmt_memory_entity_get_embeddings, 0);

      const char *cname = (const char *)sqlite3_column_text(s_db.stmt_memory_entity_get_embeddings,
                                                            1);
      if (cname) {
         strncpy(out_names[count], cname, MEMORY_ENTITY_NAME_MAX - 1);
         out_names[count][MEMORY_ENTITY_NAME_MAX - 1] = '\0';
      } else {
         out_names[count][0] = '\0';
      }

      const char *etype = (const char *)sqlite3_column_text(s_db.stmt_memory_entity_get_embeddings,
                                                            2);
      if (etype) {
         strncpy(out_types[count], etype, MEMORY_ENTITY_TYPE_MAX - 1);
         out_types[count][MEMORY_ENTITY_TYPE_MAX - 1] = '\0';
      } else {
         out_types[count][0] = '\0';
      }

      const void *blob = sqlite3_column_blob(s_db.stmt_memory_entity_get_embeddings, 3);
      if (blob) {
         memcpy(out_embeddings + count * expected_dims, blob, (size_t)expected_bytes);
      }
      out_norms[count] = (float)sqlite3_column_double(s_db.stmt_memory_entity_get_embeddings, 4);
      count++;
   }

   sqlite3_reset(s_db.stmt_memory_entity_get_embeddings);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = count;
   return MEMORY_DB_SUCCESS;
}
