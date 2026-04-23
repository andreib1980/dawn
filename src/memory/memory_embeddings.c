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
 * Memory Embeddings Core
 *
 * Provider abstraction, math utilities, in-memory cache, hybrid search,
 * and background backfill for semantic memory search.
 */

#include "memory/memory_embeddings.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define AUTH_DB_INTERNAL_ALLOWED /* needed for direct sqlite access in category backfill */

#include "auth/auth_db_internal.h"
#include "config/dawn_config.h"
#include "core/embedding_engine.h"
#include "logging.h"
#include "memory/memory_db.h"
#include "memory/memory_types.h"
#include "tools/time_query_parser.h"

/* In-memory embedding cache for fast cosine search.
 * created_ats added in #3 — per-fact origin timestamps used by temporal-query
 * scoring.  Loaded together with embeddings so scoring stays single-pass. */
static struct {
   pthread_mutex_t mutex;
   int user_id;
   int64_t *ids;
   float *embeddings; /* flat: count * dims */
   float *norms;
   int64_t *created_ats; /* per-fact created_at, parallel to ids */
   int count;
   int capacity;
   int dims;
   bool valid;
   atomic_bool dirty; /* set by backfill after each store */
} s_cache = {
   .mutex = PTHREAD_MUTEX_INITIALIZER,
};

/* Entity embedding cache (separate from fact cache) */
#define ENTITY_CACHE_CAP 500

static struct {
   pthread_mutex_t mutex;
   int user_id;
   int64_t *ids;
   char (*names)[MEMORY_ENTITY_NAME_MAX];
   char (*types)[MEMORY_ENTITY_TYPE_MAX];
   float *embeddings; /* flat: count * dims */
   float *norms;
   int count;
   int dims;
   bool valid;
   atomic_bool dirty;
} s_entity_cache = {
   .mutex = PTHREAD_MUTEX_INITIALIZER,
};

/* Forward declaration */
static void entity_cache_free(void);

/* Backfill thread state */
static pthread_t s_backfill_thread;
static atomic_bool s_backfill_running;
static atomic_bool s_backfill_shutdown;
static int s_backfill_user_id;

/* =============================================================================
 * Category Centroid Backfill (v34)
 *
 * One-shot per-user pass that classifies existing facts using their already-cached
 * embeddings.  Runs after the embedding backfill completes (centroids are useless
 * without populated fact embeddings).  Gated by users.categories_backfilled_at:
 * non-zero = already done, skip.
 * ============================================================================= */

/* Calibrated against live DAWN memory (1122 facts, all-MiniLM-L6-v2-int8):
 *   0.45 → 0.3%  classified (almost nothing — model compresses cosines heavily)
 *   0.30 → 7.4% classified (too conservative)
 *   0.25 → 15.7% classified — reasonable working default.
 *
 * MiniLM typically produces cosines in [0.10, 0.45] even for strongly related
 * content.  Non-match cosines cluster around 0.10-0.18, so 0.25 still separates
 * signal from noise while catching directionally-correct matches.  Going lower
 * (0.20) mostly inflates already-dominant categories rather than bringing in
 * the sparser ones — those are seed-quality issues, not threshold issues. */
#define CATEGORY_BACKFILL_THRESHOLD 0.25f
#define CATEGORY_BACKFILL_BATCH_SIZE 25
#define CATEGORY_BACKFILL_FETCH 200

/* Seed phrases per category.  Used to compute one centroid per category at
 * backfill time — embed each seed, average per category, store as the
 * comparison vector for cosine classification.  "general" is intentionally
 * absent: it's the fallback when no other centroid scores above the threshold. */
static const struct {
   const char *category;
   const char *seeds[6]; /* NULL-terminated, ~5 phrases each */
} CATEGORY_SEEDS[] = {
   { "personal",
     { "I was born in 1985", "my full name is Alex Smith", "I grew up in Texas",
       "I am 38 years old", "my middle name is Lee", NULL } },
   { "professional",
     { "I work as a software engineer", "I graduated from MIT", "my company is Acme Corp",
       "I am a senior developer", "I have a Python certification", NULL } },
   { "relationships",
     { "my wife's name is Jane", "my son is named Sam", "my best friend is Bob",
       "my mother lives in Ohio", "I have two siblings", NULL } },
   { "health",
     { "I am allergic to peanuts", "I take metformin daily", "I follow a vegetarian diet",
       "I have asthma", "I work out three times a week", NULL } },
   { "interests",
     { "I love science fiction novels", "I play guitar in a band", "I enjoy hiking on weekends",
       "I follow the Lakers", "I am learning Spanish", NULL } },
   { "practical",
     { "my home address is 123 Main St", "my car is a Honda Civic",
       "my router is in the office closet", "my office is on the third floor",
       "I have an Amazon Prime account", NULL } },
   { "preferences",
     { "I prefer dark mode", "I like concise responses", "I prefer Celsius over Fahrenheit",
       "I dislike interruptions", "I like formal language", NULL } },
};
#define CATEGORY_SEEDS_COUNT ((int)(sizeof(CATEGORY_SEEDS) / sizeof(CATEGORY_SEEDS[0])))

/* Build per-category centroid embeddings by averaging the seed phrase embeddings.
 * Returns malloc'd buffer of CATEGORY_SEEDS_COUNT * dims floats; caller frees.
 * Returns NULL on failure. */
static float *build_category_centroids(int *out_dims) {
   if (!embedding_engine_available())
      return NULL;

   int dims = 0;
   /* Probe dimensions with the first seed of the first category. */
   float probe[MAX_EMBEDDING_DIMS];
   if (embedding_engine_embed(CATEGORY_SEEDS[0].seeds[0], probe, MAX_EMBEDDING_DIMS, &dims) != 0 ||
       dims <= 0) {
      OLOG_ERROR("memory_embeddings: centroid build failed (probe embed)");
      return NULL;
   }

   float *centroids = calloc((size_t)CATEGORY_SEEDS_COUNT * dims, sizeof(float));
   if (!centroids) {
      OLOG_ERROR("memory_embeddings: centroid alloc failed");
      return NULL;
   }

   for (int c = 0; c < CATEGORY_SEEDS_COUNT; c++) {
      int seed_count = 0;
      float *cent = centroids + (size_t)c * dims;
      for (int s = 0; CATEGORY_SEEDS[c].seeds[s]; s++) {
         float emb[MAX_EMBEDDING_DIMS];
         int sd = 0;
         if (embedding_engine_embed(CATEGORY_SEEDS[c].seeds[s], emb, MAX_EMBEDDING_DIMS, &sd) ==
                 0 &&
             sd == dims) {
            for (int d = 0; d < dims; d++)
               cent[d] += emb[d];
            seed_count++;
         }
      }
      if (seed_count == 0) {
         OLOG_WARNING("memory_embeddings: no seeds embedded for category '%s'",
                      CATEGORY_SEEDS[c].category);
         /* Leave as zero vector — cosine will produce 0, no false matches. */
         continue;
      }
      /* Average then re-normalize so cosine is well-defined. */
      float norm_sq = 0.0f;
      for (int d = 0; d < dims; d++) {
         cent[d] /= (float)seed_count;
         norm_sq += cent[d] * cent[d];
      }
      float n = sqrtf(norm_sq);
      if (n > 1e-6f) {
         for (int d = 0; d < dims; d++)
            cent[d] /= n;
      }
      /* Health check: log the final centroid norm.  A healthy unit vector prints
       * ~1.0; zeros or near-zero mean all embeds failed for this category. */
      float check_sq = 0.0f;
      for (int d = 0; d < dims; d++)
         check_sq += cent[d] * cent[d];
      OLOG_INFO("memory_embeddings: centroid[%s] seeds=%d norm=%.4f", CATEGORY_SEEDS[c].category,
                seed_count, sqrtf(check_sq));
   }

   *out_dims = dims;
   return centroids;
}

/* Classify a single fact embedding against the centroids.  Returns the winning
 * category name (one of MEMORY_FACT_CATEGORIES) or "general" when no centroid
 * scores above the threshold. */
static const char *classify_fact_embedding(const float *fact_emb,
                                           const float *centroids,
                                           int dims,
                                           float threshold) {
   if (!fact_emb || !centroids || dims <= 0)
      return "general";

   float best_score = -1.0f;
   int best_idx = -1;

   /* Re-normalize the fact embedding once for cosine comparison.  We don't
    * trust the cached norm here because we want pure cosine = dot product
    * of unit vectors, and centroids are unit vectors. */
   float fact_norm_sq = 0.0f;
   for (int d = 0; d < dims; d++)
      fact_norm_sq += fact_emb[d] * fact_emb[d];
   float fact_norm = sqrtf(fact_norm_sq);
   if (fact_norm < 1e-6f)
      return "general";

   for (int c = 0; c < CATEGORY_SEEDS_COUNT; c++) {
      const float *cent = centroids + (size_t)c * dims;
      float dot = 0.0f;
      for (int d = 0; d < dims; d++)
         dot += fact_emb[d] * cent[d];
      float cosine = dot / fact_norm;
      if (cosine > best_score) {
         best_score = cosine;
         best_idx = c;
      }
   }

   if (best_idx < 0 || best_score < threshold)
      return "general";
   return CATEGORY_SEEDS[best_idx].category;
}

/* Read a user's flag.  Returns 0 if not yet backfilled, non-zero (timestamp)
 * if already done.  Errors return -1 (caller should treat as "skip and retry
 * next time"). */
static int64_t user_categories_backfilled_at(int user_id) {
   AUTH_DB_LOCK_OR_RETURN(-1);

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, "SELECT categories_backfilled_at FROM users WHERE id = ?",
                               -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return -1;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   int64_t ts = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      ts = sqlite3_column_int64(stmt, 0);
   }
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return ts;
}

static void user_set_categories_backfilled(int user_id, int64_t ts) {
   AUTH_DB_LOCK_OR_RETURN_VOID();

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "UPDATE users SET categories_backfilled_at = ? WHERE id = ?", -1,
                               &stmt, NULL);
   if (rc == SQLITE_OK) {
      sqlite3_bind_int64(stmt, 1, ts);
      sqlite3_bind_int(stmt, 2, user_id);
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);
   }
   AUTH_DB_UNLOCK();
}

/* Iterate user's facts with embeddings + general category, classify, batch-UPDATE
 * the assigned category.  Returns number of facts classified (assigned non-general)
 * or -1 on hard error.  Caller already verified embedding engine + flag state. */
static int categorize_user_facts(int user_id, const float *centroids, int dims) {
   if (!centroids || dims <= 0)
      return -1;

   int classified = 0;
   int touched = 0;
   int loops = 0;
   int64_t cursor_id = 0;      /* id-based pagination (prevents infinite loop) */
   bool sample_logged = false; /* one-time cosine-score sample for tuning */
   int per_cat_count[CATEGORY_SEEDS_COUNT] = { 0 }; /* assignment distribution */

   /* Hoist the per-batch scratch buffer (~1.6 MB for 200 rows × 2048-dim float
    * array) out of the loop to avoid repeated alloc+zero churn on the heap.
    * The buffer is reused across all batches; ids/dims overwritten each pass. */
   typedef struct {
      int64_t id;
      float emb[MAX_EMBEDDING_DIMS];
      int emb_dims;
   } row_t;
   row_t *rows = calloc(CATEGORY_BACKFILL_FETCH, sizeof(row_t));
   if (!rows)
      return -1;

   while (!atomic_load(&s_backfill_shutdown)) {
      /* Pull the next batch by id > cursor.  This is the fix for the infinite
       * loop that hit prod: if every fact classifies as 'general', the UPDATE
       * doesn't fire and WHERE category='general' keeps returning the same rows. */
      pthread_mutex_lock(&s_db.mutex);
      if (!s_db.initialized) {
         pthread_mutex_unlock(&s_db.mutex);
         free(rows);
         return -1;
      }
      sqlite3_stmt *stmt = NULL;
      int rc = sqlite3_prepare_v2(s_db.db,
                                  "SELECT id, embedding, embedding_norm FROM memory_facts "
                                  "WHERE user_id = ? AND superseded_by IS NULL "
                                  "  AND embedding IS NOT NULL AND id > ? "
                                  "  AND category = 'general' "
                                  "ORDER BY id ASC LIMIT ?",
                                  -1, &stmt, NULL);
      if (rc != SQLITE_OK) {
         AUTH_DB_UNLOCK();
         free(rows);
         return -1;
      }
      sqlite3_bind_int(stmt, 1, user_id);
      sqlite3_bind_int64(stmt, 2, cursor_id);
      sqlite3_bind_int(stmt, 3, CATEGORY_BACKFILL_FETCH);

      /* rows buffer is pre-allocated above the while loop — just reset count. */
      int batch_count = 0;
      int64_t batch_max_id = cursor_id;
      while (batch_count < CATEGORY_BACKFILL_FETCH && sqlite3_step(stmt) == SQLITE_ROW) {
         int64_t row_id = sqlite3_column_int64(stmt, 0);
         if (row_id > batch_max_id)
            batch_max_id = row_id;
         const void *blob = sqlite3_column_blob(stmt, 1);
         int blob_bytes = sqlite3_column_bytes(stmt, 1);
         int row_dims = blob_bytes / (int)sizeof(float);
         if (blob && row_dims == dims && row_dims <= MAX_EMBEDDING_DIMS) {
            rows[batch_count].id = row_id;
            memcpy(rows[batch_count].emb, blob, (size_t)blob_bytes);
            rows[batch_count].emb_dims = row_dims;
            batch_count++;
         }
         /* Skip facts with mismatched dims (different embedding model than centroids). */
      }
      cursor_id = batch_max_id; /* advance past this batch regardless of UPDATE results */
      sqlite3_finalize(stmt);
      AUTH_DB_UNLOCK();

      if (batch_count == 0)
         break;

      /* Phase 1: classify this batch WITHOUT holding the DB lock.  Rows already
       * have embeddings copied into stack-local storage, so dot products are
       * pure CPU work with no DB access.  Holding the lock across these loops
       * would block unrelated DB traffic (session writes, fact updates, etc.)
       * for ~1ms × batch_size. */
      const char *assigned_cat[CATEGORY_BACKFILL_FETCH]; /* parallel to rows[] */
      for (int i = 0; i < batch_count && !atomic_load(&s_backfill_shutdown); i++) {
         /* One-time diagnostic on the very first fact — logs raw cosines against
          * each centroid so the threshold can be tuned from real data.  Fires
          * once per user per backfill run. */
         if (!sample_logged) {
            float best = -1.0f;
            int best_idx = -1;
            for (int c = 0; c < CATEGORY_SEEDS_COUNT; c++) {
               const float *cent = centroids + (size_t)c * dims;
               float dot = 0.0f;
               for (int d = 0; d < dims; d++)
                  dot += rows[i].emb[d] * cent[d];
               OLOG_INFO("memory_embeddings: sample fact_id=%lld vs %s = %.4f",
                         (long long)rows[i].id, CATEGORY_SEEDS[c].category, dot);
               if (dot > best) {
                  best = dot;
                  best_idx = c;
               }
            }
            OLOG_INFO("memory_embeddings: sample best=%s @ %.4f (threshold=%.2f)",
                      best_idx >= 0 ? CATEGORY_SEEDS[best_idx].category : "(none)", best,
                      CATEGORY_BACKFILL_THRESHOLD);
            sample_logged = true;
         }

         assigned_cat[i] = classify_fact_embedding(rows[i].emb, centroids, dims,
                                                   CATEGORY_BACKFILL_THRESHOLD);
         touched++;
      }

      /* Phase 2: apply the UPDATEs under one transaction.  Lock held only for
       * the SQLite work, not the cosine math. */
      pthread_mutex_lock(&s_db.mutex);
      if (!s_db.initialized) {
         pthread_mutex_unlock(&s_db.mutex);
         free(rows);
         return -1;
      }
      char *errmsg = NULL;
      int begin_rc = sqlite3_exec(s_db.db, "BEGIN", NULL, NULL, &errmsg);
      if (begin_rc != SQLITE_OK) {
         OLOG_ERROR("memory_embeddings: BEGIN failed in category backfill: %s",
                    errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         pthread_mutex_unlock(&s_db.mutex);
         free(rows);
         return -1;
      }
      sqlite3_free(errmsg);
      errmsg = NULL;

      for (int i = 0; i < batch_count && !atomic_load(&s_backfill_shutdown); i++) {
         const char *cat = assigned_cat[i];
         if (strcmp(cat, "general") == 0)
            continue; /* Leave the column at its current 'general' value. */

         /* Inline UPDATE — already hold lock; can't call memory_db_fact_update_category. */
         sqlite3_stmt *upd_stmt = s_db.stmt_memory_fact_update_category;
         sqlite3_reset(upd_stmt);
         sqlite3_bind_text(upd_stmt, 1, cat, -1, SQLITE_TRANSIENT);
         sqlite3_bind_int64(upd_stmt, 2, rows[i].id);
         if (sqlite3_step(upd_stmt) == SQLITE_DONE) {
            classified++;
            /* Tally for the final distribution log */
            for (int c = 0; c < CATEGORY_SEEDS_COUNT; c++) {
               if (strcmp(cat, CATEGORY_SEEDS[c].category) == 0) {
                  per_cat_count[c]++;
                  break;
               }
            }
         }
         sqlite3_reset(upd_stmt);
      }

      int commit_rc = sqlite3_exec(s_db.db, "COMMIT", NULL, NULL, &errmsg);
      if (commit_rc != SQLITE_OK) {
         OLOG_ERROR("memory_embeddings: COMMIT failed in category backfill: %s — rolling back",
                    errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         errmsg = NULL;
         sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
         pthread_mutex_unlock(&s_db.mutex);
         free(rows);
         return -1;
      }
      sqlite3_free(errmsg);
      AUTH_DB_UNLOCK();

      /* If we got fewer than the fetch limit, no more unclassified facts remain. */
      if (batch_count < CATEGORY_BACKFILL_FETCH)
         break;

      /* Cooperative throttle between batches. */
      usleep(50000);

      if (++loops > 1000) {
         OLOG_WARNING("memory_embeddings: category backfill loop guard tripped");
         break;
      }
   }

   free(rows);

   OLOG_INFO("memory_embeddings: category backfill user=%d touched=%d assigned=%d (general=%d)",
             user_id, touched, classified, touched - classified);
   for (int c = 0; c < CATEGORY_SEEDS_COUNT; c++) {
      if (per_cat_count[c] > 0) {
         OLOG_INFO("memory_embeddings:   %s: %d", CATEGORY_SEEDS[c].category, per_cat_count[c]);
      }
   }
   return classified;
}

/* =============================================================================
 * Math Utilities — delegate to shared embedding engine
 * ============================================================================= */

float memory_embeddings_l2_norm(const float *vec, int dims) {
   return embedding_engine_l2_norm(vec, dims);
}

float memory_embeddings_cosine_with_norms(const float *a,
                                          const float *b,
                                          int dims,
                                          float norm_a,
                                          float norm_b) {
   return embedding_engine_cosine_with_norms(a, b, dims, norm_a, norm_b);
}

float memory_embeddings_cosine(const float *a, const float *b, int dims) {
   return embedding_engine_cosine(a, b, dims);
}

/* =============================================================================
 * Cache Management
 * ============================================================================= */

static void cache_free_data(void) {
   free(s_cache.ids);
   free(s_cache.embeddings);
   free(s_cache.norms);
   free(s_cache.created_ats);
   s_cache.ids = NULL;
   s_cache.embeddings = NULL;
   s_cache.norms = NULL;
   s_cache.created_ats = NULL;
   s_cache.count = 0;
   s_cache.capacity = 0;
   s_cache.valid = false;
}

static int cache_load(int user_id) {
   /* Already valid for this user? */
   if (s_cache.valid && s_cache.user_id == user_id && !atomic_load(&s_cache.dirty))
      return 0;

   cache_free_data();

   int dims = embedding_engine_dims();
   if (dims <= 0)
      return -1;

   /* Allocate for EMBEDDING_SEARCH_CAP entries */
   int cap = EMBEDDING_SEARCH_CAP;
   s_cache.ids = malloc(cap * sizeof(int64_t));
   s_cache.embeddings = malloc((size_t)cap * (size_t)dims * sizeof(float));
   s_cache.norms = malloc(cap * sizeof(float));
   s_cache.created_ats = malloc(cap * sizeof(int64_t));

   if (!s_cache.ids || !s_cache.embeddings || !s_cache.norms || !s_cache.created_ats) {
      cache_free_data();
      return -1;
   }

   int loaded = memory_db_fact_get_embeddings(user_id, dims, s_cache.ids, s_cache.embeddings,
                                              s_cache.norms, s_cache.created_ats, cap);
   if (loaded < 0) {
      cache_free_data();
      return -1;
   }

   s_cache.count = loaded;
   s_cache.capacity = cap;
   s_cache.dims = dims;
   s_cache.user_id = user_id;
   s_cache.valid = true;
   atomic_store(&s_cache.dirty, false);

   OLOG_INFO("memory_embeddings: loaded %d embeddings into cache for user %d", loaded, user_id);
   return 0;
}

void memory_embeddings_invalidate_cache(void) {
   atomic_store(&s_cache.dirty, true);
}

/* =============================================================================
 * Init / Cleanup — delegate provider management to shared embedding engine
 * ============================================================================= */

int memory_embeddings_init(void) {
   return embedding_engine_init();
}

void memory_embeddings_cleanup(void) {
   /* Stop backfill thread */
   if (atomic_load(&s_backfill_running)) {
      atomic_store(&s_backfill_shutdown, true);
      pthread_join(s_backfill_thread, NULL);
      atomic_store(&s_backfill_running, false);
   }

   /* Free caches */
   pthread_mutex_lock(&s_cache.mutex);
   cache_free_data();
   pthread_mutex_unlock(&s_cache.mutex);

   pthread_mutex_lock(&s_entity_cache.mutex);
   entity_cache_free();
   pthread_mutex_unlock(&s_entity_cache.mutex);

   /* Provider cleanup handled by embedding_engine_cleanup() in dawn.c shutdown */
}

bool memory_embeddings_available(void) {
   return embedding_engine_available();
}

int memory_embeddings_dims(void) {
   return embedding_engine_dims();
}

/* =============================================================================
 * Embedding Generation
 * ============================================================================= */

int memory_embeddings_embed(const char *text, float *out, int *out_dims) {
   if (!text || !out || !out_dims)
      return -1;

   return embedding_engine_embed(text, out, MAX_EMBEDDING_DIMS, out_dims);
}

int memory_embeddings_embed_and_store(int user_id, int64_t fact_id, const char *text) {
   if (!embedding_engine_available() || !text)
      return -1;

   float embedding[MAX_EMBEDDING_DIMS];
   int dims = 0;

   int rc = embedding_engine_embed(text, embedding, MAX_EMBEDDING_DIMS, &dims);
   if (rc != 0 || dims <= 0)
      return rc;

   float norm = memory_embeddings_l2_norm(embedding, dims);

   rc = memory_db_fact_update_embedding(user_id, fact_id, embedding, dims, norm);
   if (rc == MEMORY_DB_SUCCESS) {
      memory_embeddings_invalidate_cache();
   }
   return rc;
}

/* =============================================================================
 * Entity Embedding Support
 * ============================================================================= */

static void entity_cache_free(void) {
   free(s_entity_cache.ids);
   free(s_entity_cache.names);
   free(s_entity_cache.types);
   free(s_entity_cache.embeddings);
   free(s_entity_cache.norms);
   s_entity_cache.ids = NULL;
   s_entity_cache.names = NULL;
   s_entity_cache.types = NULL;
   s_entity_cache.embeddings = NULL;
   s_entity_cache.norms = NULL;
   s_entity_cache.count = 0;
   s_entity_cache.valid = false;
}

static int entity_cache_load(int user_id) {
   if (s_entity_cache.valid && s_entity_cache.user_id == user_id &&
       !atomic_load(&s_entity_cache.dirty))
      return 0;

   entity_cache_free();

   int dims = embedding_engine_dims();
   if (dims <= 0)
      return -1;

   s_entity_cache.ids = malloc(ENTITY_CACHE_CAP * sizeof(int64_t));
   s_entity_cache.names = malloc(ENTITY_CACHE_CAP * sizeof(*s_entity_cache.names));
   s_entity_cache.types = malloc(ENTITY_CACHE_CAP * sizeof(*s_entity_cache.types));
   s_entity_cache.embeddings = malloc((size_t)ENTITY_CACHE_CAP * (size_t)dims * sizeof(float));
   s_entity_cache.norms = malloc(ENTITY_CACHE_CAP * sizeof(float));

   if (!s_entity_cache.ids || !s_entity_cache.names || !s_entity_cache.types ||
       !s_entity_cache.embeddings || !s_entity_cache.norms) {
      entity_cache_free();
      return -1;
   }

   int loaded = memory_db_entity_get_embeddings(user_id, dims, s_entity_cache.ids,
                                                s_entity_cache.names, s_entity_cache.types,
                                                s_entity_cache.embeddings, s_entity_cache.norms,
                                                ENTITY_CACHE_CAP);
   if (loaded < 0) {
      entity_cache_free();
      return -1;
   }

   s_entity_cache.count = loaded;
   s_entity_cache.dims = dims;
   s_entity_cache.user_id = user_id;
   s_entity_cache.valid = true;
   atomic_store(&s_entity_cache.dirty, false);

   OLOG_INFO("memory_embeddings: loaded %d entity embeddings into cache for user %d", loaded,
             user_id);
   return 0;
}

int memory_embeddings_embed_and_store_entity(int64_t entity_id, int user_id, const char *text) {
   if (!embedding_engine_available() || !text)
      return -1;

   float embedding[MAX_EMBEDDING_DIMS];
   int dims = 0;

   int rc = embedding_engine_embed(text, embedding, MAX_EMBEDDING_DIMS, &dims);
   if (rc != 0 || dims <= 0)
      return rc;

   float norm = memory_embeddings_l2_norm(embedding, dims);

   return memory_db_entity_update_embedding(entity_id, user_id, embedding, dims, norm);
}

void memory_embeddings_invalidate_entity_cache(void) {
   atomic_store(&s_entity_cache.dirty, true);
}

int memory_embeddings_entity_search(int user_id,
                                    const char *query,
                                    const char *type_filter,
                                    int64_t *out_ids,
                                    char out_names[][MEMORY_ENTITY_NAME_MAX],
                                    char out_types[][MEMORY_ENTITY_TYPE_MAX],
                                    float *out_scores,
                                    int max_results) {
   if (!memory_embeddings_available() || !query || !out_ids || max_results <= 0)
      return 0;

   /* Embed the query */
   float query_emb[MAX_EMBEDDING_DIMS];
   int dims = 0;
   if (memory_embeddings_embed(query, query_emb, &dims) != 0 || dims != embedding_engine_dims())
      return 0;

   float query_norm = memory_embeddings_l2_norm(query_emb, dims);

   pthread_mutex_lock(&s_entity_cache.mutex);
   if (entity_cache_load(user_id) != 0) {
      pthread_mutex_unlock(&s_entity_cache.mutex);
      return 0;
   }

   /* Score all cached entities by cosine similarity */
   typedef struct {
      int idx;
      float score;
   } scored_t;
   scored_t scored[ENTITY_CACHE_CAP];
   int scored_count = 0;

   for (int i = 0; i < s_entity_cache.count; i++) {
      /* Apply type filter if specified */
      if (type_filter && type_filter[0] != '\0' &&
          strcmp(s_entity_cache.types[i], type_filter) != 0) {
         continue;
      }

      float cosine = memory_embeddings_cosine_with_norms(query_emb,
                                                         s_entity_cache.embeddings + i * dims, dims,
                                                         query_norm, s_entity_cache.norms[i]);

      if (cosine > 0.4f) {
         scored[scored_count].idx = i;
         scored[scored_count].score = cosine;
         scored_count++;
      }
   }

   /* Sort by score descending (insertion sort — small N) */
   for (int i = 1; i < scored_count; i++) {
      scored_t tmp = scored[i];
      int j = i - 1;
      while (j >= 0 && scored[j].score < tmp.score) {
         scored[j + 1] = scored[j];
         j--;
      }
      scored[j + 1] = tmp;
   }

   /* Copy top results */
   int result_count = scored_count > max_results ? max_results : scored_count;
   for (int i = 0; i < result_count; i++) {
      int idx = scored[i].idx;
      out_ids[i] = s_entity_cache.ids[idx];
      if (out_names) {
         strncpy(out_names[i], s_entity_cache.names[idx], MEMORY_ENTITY_NAME_MAX - 1);
         out_names[i][MEMORY_ENTITY_NAME_MAX - 1] = '\0';
      }
      if (out_types) {
         strncpy(out_types[i], s_entity_cache.types[idx], MEMORY_ENTITY_TYPE_MAX - 1);
         out_types[i][MEMORY_ENTITY_TYPE_MAX - 1] = '\0';
      }
      if (out_scores)
         out_scores[i] = scored[i].score;
   }

   pthread_mutex_unlock(&s_entity_cache.mutex);
   return result_count;
}

/* =============================================================================
 * Hybrid Search
 * ============================================================================= */

int memory_embeddings_hybrid_search(int user_id,
                                    const char *query,
                                    const int64_t *keyword_facts,
                                    const int *keyword_scores,
                                    int keyword_count,
                                    int token_count,
                                    embedding_search_result_t *out_results,
                                    int max_results) {
   if (!out_results || max_results <= 0)
      return 0;

   float kw_weight = g_config.memory.embedding_keyword_weight;
   float vec_weight = g_config.memory.embedding_vector_weight;
   float temporal_weight = g_config.memory.temporal_weight;

   /* Parse temporal expression in the query (only when feature is enabled).
    * Cost is a single-pass strstr scan; skipped entirely when weight is 0. */
   time_query_t tq = { 0 };
   if (temporal_weight > 0.0f && query) {
      time_query_parse(query, (int64_t)time(NULL), &tq);
   }

   /* If no embeddings available, return keyword results directly */
   if (!memory_embeddings_available() || !query) {
      int count = keyword_count > max_results ? max_results : keyword_count;
      for (int i = 0; i < count; i++) {
         out_results[i].fact_id = keyword_facts[i];
         out_results[i].score = (token_count > 0) ? (float)keyword_scores[i] / (float)token_count
                                                  : 1.0f;
      }
      return count;
   }

   /* Embed the query */
   float query_emb[MAX_EMBEDDING_DIMS];
   int dims = 0;
   if (memory_embeddings_embed(query, query_emb, &dims) != 0 || dims != embedding_engine_dims()) {
      /* Embedding failed — fall back to keyword only */
      int count = keyword_count > max_results ? max_results : keyword_count;
      for (int i = 0; i < count; i++) {
         out_results[i].fact_id = keyword_facts[i];
         out_results[i].score = (token_count > 0) ? (float)keyword_scores[i] / (float)token_count
                                                  : 1.0f;
      }
      return count;
   }

   float query_norm = memory_embeddings_l2_norm(query_emb, dims);

   /* Load cache under lock */
   pthread_mutex_lock(&s_cache.mutex);
   if (cache_load(user_id) != 0) {
      pthread_mutex_unlock(&s_cache.mutex);
      /* Cache load failed — keyword only */
      int count = keyword_count > max_results ? max_results : keyword_count;
      for (int i = 0; i < count; i++) {
         out_results[i].fact_id = keyword_facts[i];
         out_results[i].score = (token_count > 0) ? (float)keyword_scores[i] / (float)token_count
                                                  : 1.0f;
      }
      return count;
   }

   /* Build result set: start with keyword facts, add vector matches */
   /* Temporary merged set */
   int merged_cap = keyword_count + s_cache.count;
   if (merged_cap > EMBEDDING_SEARCH_CAP)
      merged_cap = EMBEDDING_SEARCH_CAP;

   /* Stack-allocated — EMBEDDING_SEARCH_CAP * 12 bytes = 24KB, well within 8MB stack */
   embedding_search_result_t merged[EMBEDDING_SEARCH_CAP];
   int merged_count = 0;

   /* Score all cached embeddings by vector similarity */
   for (int i = 0; i < s_cache.count && merged_count < merged_cap; i++) {
      float cosine = memory_embeddings_cosine_with_norms(query_emb, s_cache.embeddings + i * dims,
                                                         dims, query_norm, s_cache.norms[i]);

      /* Find keyword score for this fact (if any) */
      float kw_score = 0.0f;
      for (int k = 0; k < keyword_count; k++) {
         if (keyword_facts[k] == s_cache.ids[i]) {
            kw_score = (token_count > 0) ? (float)keyword_scores[k] / (float)token_count : 1.0f;
            break;
         }
      }

      float hybrid = kw_weight * kw_score + vec_weight * cosine;

      /* Additive temporal boost.  Additive (not multiplicative) so undated facts
       * aren't penalized — they simply forfeit the bonus. */
      if (tq.found && s_cache.created_ats[i] > 0) {
         hybrid += temporal_weight * time_query_proximity(&tq, s_cache.created_ats[i]);
      }

      if (hybrid > 0.01f) { /* Skip near-zero results */
         merged[merged_count].fact_id = s_cache.ids[i];
         merged[merged_count].score = hybrid;
         merged_count++;
      }
   }

   /* Add keyword-only results (facts without embeddings) */
   for (int k = 0; k < keyword_count; k++) {
      bool found = false;
      for (int m = 0; m < merged_count; m++) {
         if (merged[m].fact_id == keyword_facts[k]) {
            found = true;
            break;
         }
      }
      if (!found && merged_count < merged_cap) {
         float kw_score = (token_count > 0) ? (float)keyword_scores[k] / (float)token_count : 1.0f;
         merged[merged_count].fact_id = keyword_facts[k];
         merged[merged_count].score = kw_weight * kw_score; /* keyword only, no vector penalty */
         merged_count++;
      }
   }

   pthread_mutex_unlock(&s_cache.mutex);

   /* Sort by score descending (insertion sort — small N) */
   for (int i = 1; i < merged_count; i++) {
      embedding_search_result_t tmp = merged[i];
      int j = i - 1;
      while (j >= 0 && merged[j].score < tmp.score) {
         merged[j + 1] = merged[j];
         j--;
      }
      merged[j + 1] = tmp;
   }

   /* Copy top results */
   int result_count = merged_count > max_results ? max_results : merged_count;
   memcpy(out_results, merged, result_count * sizeof(embedding_search_result_t));

   return result_count;
}

/* =============================================================================
 * Background Backfill
 * ============================================================================= */

static void *backfill_thread_fn(void *arg) {
   (void)arg;
   int user_id = s_backfill_user_id;
   int dims = embedding_engine_dims();

   OLOG_INFO("memory_embeddings: backfill started for user %d", user_id);

   int total_embedded = 0;
   int batch_size = 50;

   while (!atomic_load(&s_backfill_shutdown)) {
      int64_t ids[50];
      char texts[50][512];

      int count = memory_db_fact_list_without_embedding(user_id, dims, ids, texts, batch_size);
      if (count <= 0)
         break;

      for (int i = 0; i < count && !atomic_load(&s_backfill_shutdown); i++) {
         if (texts[i][0] == '\0')
            continue;

         if (memory_embeddings_embed_and_store(user_id, ids[i], texts[i]) == 0) {
            total_embedded++;
         }

         /* Throttle: 50ms between embeddings to avoid hogging CPU */
         usleep(50000);
      }
   }

   OLOG_INFO("memory_embeddings: backfill complete, embedded %d facts", total_embedded);

   /* v34: after embedding backfill completes, run a one-shot category-classification
    * pass for this user if it hasn't been done yet.  Centroids are only useful
    * once fact embeddings are populated — that's why this lives at the tail of
    * the same thread rather than as a separate scheduled job.  Idempotent across
    * reboots via users.categories_backfilled_at timestamp. */
   if (!atomic_load(&s_backfill_shutdown) && embedding_engine_available()) {
      int64_t flag = user_categories_backfilled_at(user_id);
      if (flag == 0) {
         OLOG_INFO("memory_embeddings: starting category backfill for user %d", user_id);
         int dims = 0;
         float *centroids = build_category_centroids(&dims);
         if (centroids && dims > 0) {
            int classified = categorize_user_facts(user_id, centroids, dims);
            free(centroids);
            if (classified >= 0) {
               user_set_categories_backfilled(user_id, (int64_t)time(NULL));
            }
         } else {
            OLOG_WARNING("memory_embeddings: category centroid build failed, skipping backfill");
         }
      }
   }

   atomic_store(&s_backfill_running, false);
   return NULL;
}

void memory_embeddings_start_backfill(int user_id) {
   if (!memory_embeddings_available())
      return;

   if (atomic_load(&s_backfill_running)) {
      OLOG_INFO("memory_embeddings: backfill already running");
      return;
   }

   s_backfill_user_id = user_id;
   atomic_store(&s_backfill_shutdown, false);
   atomic_store(&s_backfill_running, true);

   if (pthread_create(&s_backfill_thread, NULL, backfill_thread_fn, NULL) != 0) {
      OLOG_ERROR("memory_embeddings: failed to create backfill thread");
      atomic_store(&s_backfill_running, false);
   }
}
