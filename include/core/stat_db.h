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
 * STAT telemetry history — self-contained SQLite store (own handle/mutex/WAL,
 * modeled on src/audio/music_db.c).  Holds downsampled telemetry buckets so the
 * assistant can answer "how hot did it get overnight?".  Deliberately NOT part
 * of the shared auth.db: telemetry is host-scoped (not per-user) and keeping it
 * in its own file makes the whole STAT subsystem cleanly removable.
 */

#ifndef STAT_DB_H
#define STAT_DB_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * One downsampled telemetry bucket to persist.  A family with no samples in the
 * bucket sets its `have_*` flag false; those columns are stored as SQL NULL.
 */
typedef struct {
   int64_t bucket_start; /* unix epoch seconds — start of the accumulation window */

   bool have_sys; /* SystemMetrics samples present */
   double cpu_avg, cpu_max;
   double mem_avg, mem_max;
   double temp_avg, temp_min, temp_max;

   bool have_batt;                      /* unified BatteryStatus samples present */
   double batt_avg, batt_min, batt_max; /* battery_level (%) */
   double batt_v_avg, batt_p_avg, batt_temp_max;

   bool have_fan;           /* Fan samples present */
   double fan_avg, fan_max; /* rpm */

   int sys_count, batt_count, fan_count;
} stat_bucket_row_t;

/**
 * Aggregated history over a time window (computed in SQL — the output size is
 * fixed regardless of how many buckets the window spans).  Averages are
 * sample-count weighted across buckets; peak-bucket timestamps let the tool
 * report "peaked ~03:12".
 */
typedef struct {
   bool have_data;
   int64_t covered_start, covered_end; /* actual bucket_start range returned */
   int bucket_count;
   long total_sys, total_batt, total_fan;

   double cpu_avg, cpu_max;
   double mem_avg, mem_max;
   double temp_min, temp_avg, temp_max;
   int64_t temp_peak_bucket; /* bucket_start of the hottest bucket (0 if none) */
   double batt_min, batt_avg, batt_max;
   int64_t batt_min_bucket; /* bucket_start of the lowest-charge bucket (0 if none) */
   double fan_avg, fan_max;
} stat_history_agg_t;

/**
 * @brief Open (or create) the telemetry history DB at @p db_path.
 *
 * Opens its own SQLite handle in WAL mode and creates `stat_samples` + index
 * if absent.  Safe to call once at service init; idempotent.
 *
 * @return SUCCESS on success, FAILURE on open/schema error (history disabled,
 *         but the live cache still works).
 */
int stat_db_init(const char *db_path);

/** @brief Final WAL checkpoint + close the handle. */
void stat_db_shutdown(void);

/** @brief True once the DB is open and the schema exists. */
bool stat_db_is_ready(void);

/** @brief Insert one downsampled bucket row. @return SUCCESS/FAILURE. */
int stat_db_insert_bucket(const stat_bucket_row_t *row);

/**
 * @brief Delete buckets older than @p retention_days.
 * @param deleted_out  Optional; set to the number of rows removed.
 * @return SUCCESS/FAILURE.
 */
int stat_db_prune(int retention_days, int *deleted_out);

/** @brief Passive (non-blocking) WAL checkpoint on the stat.db handle. */
int stat_db_checkpoint(void);

/**
 * @brief Aggregate all buckets in [start_ts, end_ts] into @p out.
 *
 * Averages are sample-weighted; min/max are exact.  @p out->have_data is false
 * when the window contains no rows.
 *
 * @return SUCCESS on a successful query (even with zero rows), FAILURE on error.
 */
int stat_db_history(int64_t start_ts, int64_t end_ts, stat_history_agg_t *out);

#ifdef __cplusplus
}
#endif

#endif /* STAT_DB_H */
