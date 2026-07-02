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
 * the project author(s).
 *
 * STAT telemetry history store — self-contained SQLite DB (own handle/mutex/WAL).
 * Modeled on src/audio/music_db.c (own handle) + src/tools/phone_db.c
 * (ready-flag-guarded, prepare-per-call).  Write path is cold (~1 insert /
 * 15 min) so statements are prepared per-call rather than cached — this keeps
 * the subsystem decoupled from auth_db's cached-statement teardown.
 */

#include "core/stat_db.h"

#include <assert.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "core/path_utils.h"
#include "dawn_error.h"
#include "logging.h"

/* Own DB state — NOT the shared auth.db handle. */
static struct {
   sqlite3 *db;
   pthread_mutex_t mutex;
   bool ready;
} s_stat_db = { NULL, PTHREAD_MUTEX_INITIALIZER, false };

static const char *SQL_CREATE_TABLE =
    "CREATE TABLE IF NOT EXISTS stat_samples ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  bucket_start INTEGER NOT NULL,"
    "  cpu_usage_avg REAL, cpu_usage_max REAL,"
    "  mem_usage_avg REAL, mem_usage_max REAL,"
    "  system_temp_avg REAL, system_temp_min REAL, system_temp_max REAL,"
    "  batt_level_avg REAL, batt_level_min REAL, batt_level_max REAL,"
    "  batt_voltage_avg REAL, batt_power_avg REAL, batt_temp_max REAL,"
    "  fan_rpm_avg REAL, fan_rpm_max REAL,"
    "  sys_count INTEGER NOT NULL, batt_count INTEGER NOT NULL, fan_count INTEGER NOT NULL"
    ")";

static const char *SQL_CREATE_INDEX =
    "CREATE INDEX IF NOT EXISTS idx_stat_samples_time ON stat_samples(bucket_start DESC)";

int stat_db_init(const char *db_path) {
   if (!db_path || !db_path[0]) {
      OLOG_ERROR("stat_db: no db_path provided");
      return FAILURE;
   }

   pthread_mutex_lock(&s_stat_db.mutex);
   if (s_stat_db.ready) {
      pthread_mutex_unlock(&s_stat_db.mutex);
      return SUCCESS;
   }

   char expanded[512];
   if (!path_expand_tilde(db_path, expanded, sizeof(expanded))) {
      OLOG_ERROR("stat_db: failed to expand path: %s", db_path);
      pthread_mutex_unlock(&s_stat_db.mutex);
      return FAILURE;
   }
   if (!path_ensure_parent_dir(expanded)) {
      OLOG_ERROR("stat_db: failed to create directory for: %s", expanded);
      pthread_mutex_unlock(&s_stat_db.mutex);
      return FAILURE;
   }

   int rc = sqlite3_open(expanded, &s_stat_db.db);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("stat_db: open '%s' failed: %s", expanded, sqlite3_errmsg(s_stat_db.db));
      sqlite3_close(s_stat_db.db);
      s_stat_db.db = NULL;
      pthread_mutex_unlock(&s_stat_db.mutex);
      return FAILURE;
   }

   char *errmsg = NULL;
   sqlite3_exec(s_stat_db.db, "PRAGMA journal_mode=WAL", NULL, NULL, &errmsg);
   if (errmsg) {
      OLOG_WARNING("stat_db: WAL pragma: %s", errmsg);
      sqlite3_free(errmsg);
      errmsg = NULL;
   }

   if (sqlite3_exec(s_stat_db.db, SQL_CREATE_TABLE, NULL, NULL, &errmsg) != SQLITE_OK ||
       sqlite3_exec(s_stat_db.db, SQL_CREATE_INDEX, NULL, NULL, &errmsg) != SQLITE_OK) {
      OLOG_ERROR("stat_db: schema create failed: %s", errmsg ? errmsg : "?");
      sqlite3_free(errmsg);
      sqlite3_close(s_stat_db.db);
      s_stat_db.db = NULL;
      pthread_mutex_unlock(&s_stat_db.mutex);
      return FAILURE;
   }

   s_stat_db.ready = true;
   pthread_mutex_unlock(&s_stat_db.mutex);
   OLOG_INFO("stat_db: telemetry history ready at %s", expanded);
   return SUCCESS;
}

void stat_db_shutdown(void) {
   pthread_mutex_lock(&s_stat_db.mutex);
   if (s_stat_db.db) {
      sqlite3_wal_checkpoint_v2(s_stat_db.db, NULL, SQLITE_CHECKPOINT_TRUNCATE, NULL, NULL);
      sqlite3_close(s_stat_db.db);
      s_stat_db.db = NULL;
   }
   s_stat_db.ready = false;
   pthread_mutex_unlock(&s_stat_db.mutex);
}

bool stat_db_is_ready(void) {
   pthread_mutex_lock(&s_stat_db.mutex);
   bool r = s_stat_db.ready;
   pthread_mutex_unlock(&s_stat_db.mutex);
   return r;
}

/* Bind a REAL column, or NULL when the family had no samples this bucket. */
static void bind_opt_real(sqlite3_stmt *stmt, int idx, bool present, double val) {
   if (present) {
      sqlite3_bind_double(stmt, idx, val);
   } else {
      sqlite3_bind_null(stmt, idx);
   }
}

int stat_db_insert_bucket(const stat_bucket_row_t *row) {
   if (!row) {
      return FAILURE;
   }

   pthread_mutex_lock(&s_stat_db.mutex);
   if (!s_stat_db.ready) {
      pthread_mutex_unlock(&s_stat_db.mutex);
      return FAILURE;
   }

   static const char *sql = "INSERT INTO stat_samples ("
                            " bucket_start,"
                            " cpu_usage_avg, cpu_usage_max, mem_usage_avg, mem_usage_max,"
                            " system_temp_avg, system_temp_min, system_temp_max,"
                            " batt_level_avg, batt_level_min, batt_level_max,"
                            " batt_voltage_avg, batt_power_avg, batt_temp_max,"
                            " fan_rpm_avg, fan_rpm_max,"
                            " sys_count, batt_count, fan_count"
                            ") VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";

   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_stat_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      OLOG_ERROR("stat_db: insert prepare failed: %s", sqlite3_errmsg(s_stat_db.db));
      pthread_mutex_unlock(&s_stat_db.mutex);
      return FAILURE;
   }

   int i = 1;
   sqlite3_bind_int64(stmt, i++, row->bucket_start);
   bind_opt_real(stmt, i++, row->have_sys, row->cpu_avg);
   bind_opt_real(stmt, i++, row->have_sys, row->cpu_max);
   bind_opt_real(stmt, i++, row->have_sys, row->mem_avg);
   bind_opt_real(stmt, i++, row->have_sys, row->mem_max);
   bind_opt_real(stmt, i++, row->have_sys, row->temp_avg);
   bind_opt_real(stmt, i++, row->have_sys, row->temp_min);
   bind_opt_real(stmt, i++, row->have_sys, row->temp_max);
   bind_opt_real(stmt, i++, row->have_batt, row->batt_avg);
   bind_opt_real(stmt, i++, row->have_batt, row->batt_min);
   bind_opt_real(stmt, i++, row->have_batt, row->batt_max);
   bind_opt_real(stmt, i++, row->have_batt, row->batt_v_avg);
   bind_opt_real(stmt, i++, row->have_batt, row->batt_p_avg);
   bind_opt_real(stmt, i++, row->have_batt, row->batt_temp_max);
   bind_opt_real(stmt, i++, row->have_fan, row->fan_avg);
   bind_opt_real(stmt, i++, row->have_fan, row->fan_max);
   sqlite3_bind_int(stmt, i++, row->sys_count);
   sqlite3_bind_int(stmt, i++, row->batt_count);
   sqlite3_bind_int(stmt, i++, row->fan_count);

   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   pthread_mutex_unlock(&s_stat_db.mutex);

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("stat_db: insert failed (rc=%d)", rc);
      return FAILURE;
   }
   return SUCCESS;
}

int stat_db_prune(int retention_days, int *deleted_out) {
   if (deleted_out) {
      *deleted_out = 0;
   }
   if (retention_days <= 0) {
      return SUCCESS; /* retention disabled */
   }

   pthread_mutex_lock(&s_stat_db.mutex);
   if (!s_stat_db.ready) {
      pthread_mutex_unlock(&s_stat_db.mutex);
      return FAILURE;
   }

   int64_t cutoff = (int64_t)time(NULL) - (int64_t)retention_days * 86400;
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_stat_db.db, "DELETE FROM stat_samples WHERE bucket_start < ?", -1,
                          &stmt, NULL) != SQLITE_OK) {
      OLOG_ERROR("stat_db: prune prepare failed: %s", sqlite3_errmsg(s_stat_db.db));
      pthread_mutex_unlock(&s_stat_db.mutex);
      return FAILURE;
   }
   sqlite3_bind_int64(stmt, 1, cutoff);
   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   int changes = sqlite3_changes(s_stat_db.db);
   pthread_mutex_unlock(&s_stat_db.mutex);

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("stat_db: prune failed (rc=%d)", rc);
      return FAILURE;
   }
   if (deleted_out) {
      *deleted_out = changes;
   }
   return SUCCESS;
}

int stat_db_checkpoint(void) {
   pthread_mutex_lock(&s_stat_db.mutex);
   if (!s_stat_db.ready) {
      pthread_mutex_unlock(&s_stat_db.mutex);
      return FAILURE;
   }
   sqlite3_wal_checkpoint_v2(s_stat_db.db, NULL, SQLITE_CHECKPOINT_PASSIVE, NULL, NULL);
   pthread_mutex_unlock(&s_stat_db.mutex);
   return SUCCESS;
}

/* Fetch the bucket_start of the row with the extreme value of a column.
 * @p order_desc true → hottest (MAX); false → lowest (MIN).  Returns 0 if none.
 * Caller holds s_stat_db.mutex.
 *
 * SECURITY: @p col is interpolated into the SQL text (column names can't be
 * bound parameters), so it MUST be a trusted internal literal — NEVER user- or
 * network-derived input.  Both call sites pass hardcoded column names; the
 * allowlist assert below pins that contract for any future caller. */
static int64_t query_extreme_bucket(int64_t start_ts,
                                    int64_t end_ts,
                                    const char *col,
                                    bool order_desc) {
   assert(col && (strcmp(col, "system_temp_max") == 0 || strcmp(col, "batt_level_min") == 0));

   char sql[256];
   snprintf(sql, sizeof(sql),
            "SELECT bucket_start FROM stat_samples "
            "WHERE bucket_start BETWEEN ? AND ? AND %s IS NOT NULL "
            "ORDER BY %s %s LIMIT 1",
            col, col, order_desc ? "DESC" : "ASC");

   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_stat_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      return 0;
   }
   sqlite3_bind_int64(stmt, 1, start_ts);
   sqlite3_bind_int64(stmt, 2, end_ts);
   int64_t bucket = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      bucket = sqlite3_column_int64(stmt, 0);
   }
   sqlite3_finalize(stmt);
   return bucket;
}

int stat_db_history(int64_t start_ts, int64_t end_ts, stat_history_agg_t *out) {
   if (!out) {
      return FAILURE;
   }
   memset(out, 0, sizeof(*out));

   pthread_mutex_lock(&s_stat_db.mutex);
   if (!s_stat_db.ready) {
      pthread_mutex_unlock(&s_stat_db.mutex);
      return FAILURE;
   }

   /* Sample-weighted averages (SUM(avg*count)/SUM(count)) + exact min/max. */
   static const char *sql =
       "SELECT COUNT(*), MIN(bucket_start), MAX(bucket_start),"
       " COALESCE(SUM(sys_count),0), COALESCE(SUM(batt_count),0), COALESCE(SUM(fan_count),0),"
       " SUM(cpu_usage_avg*sys_count), MAX(cpu_usage_max),"
       " SUM(mem_usage_avg*sys_count), MAX(mem_usage_max),"
       " MIN(system_temp_min), SUM(system_temp_avg*sys_count), MAX(system_temp_max),"
       " MIN(batt_level_min), SUM(batt_level_avg*batt_count), MAX(batt_level_max),"
       " SUM(fan_rpm_avg*fan_count), MAX(fan_rpm_max)"
       " FROM stat_samples WHERE bucket_start BETWEEN ? AND ?";

   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_stat_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      OLOG_ERROR("stat_db: history prepare failed: %s", sqlite3_errmsg(s_stat_db.db));
      pthread_mutex_unlock(&s_stat_db.mutex);
      return FAILURE;
   }
   sqlite3_bind_int64(stmt, 1, start_ts);
   sqlite3_bind_int64(stmt, 2, end_ts);

   if (sqlite3_step(stmt) == SQLITE_ROW) {
      out->bucket_count = sqlite3_column_int(stmt, 0);
      if (out->bucket_count > 0) {
         out->have_data = true;
         out->covered_start = sqlite3_column_int64(stmt, 1);
         out->covered_end = sqlite3_column_int64(stmt, 2);
         out->total_sys = (long)sqlite3_column_int64(stmt, 3);
         out->total_batt = (long)sqlite3_column_int64(stmt, 4);
         out->total_fan = (long)sqlite3_column_int64(stmt, 5);

         if (out->total_sys > 0) {
            out->cpu_avg = sqlite3_column_double(stmt, 6) / (double)out->total_sys;
            out->mem_avg = sqlite3_column_double(stmt, 8) / (double)out->total_sys;
            out->temp_avg = sqlite3_column_double(stmt, 11) / (double)out->total_sys;
         }
         out->cpu_max = sqlite3_column_double(stmt, 7);
         out->mem_max = sqlite3_column_double(stmt, 9);
         out->temp_min = sqlite3_column_double(stmt, 10);
         out->temp_max = sqlite3_column_double(stmt, 12);
         out->batt_min = sqlite3_column_double(stmt, 13);
         if (out->total_batt > 0) {
            out->batt_avg = sqlite3_column_double(stmt, 14) / (double)out->total_batt;
         }
         out->batt_max = sqlite3_column_double(stmt, 15);
         if (out->total_fan > 0) {
            out->fan_avg = sqlite3_column_double(stmt, 16) / (double)out->total_fan;
         }
         out->fan_max = sqlite3_column_double(stmt, 17);
      }
   }
   sqlite3_finalize(stmt);

   if (out->have_data) {
      if (out->total_sys > 0) {
         out->temp_peak_bucket = query_extreme_bucket(start_ts, end_ts, "system_temp_max", true);
      }
      if (out->total_batt > 0) {
         out->batt_min_bucket = query_extreme_bucket(start_ts, end_ts, "batt_level_min", false);
      }
   }

   pthread_mutex_unlock(&s_stat_db.mutex);
   return SUCCESS;
}
