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
 * Unit tests for the STAT telemetry service: ingest → accumulator fold →
 * history flush → SQL aggregation round-trip, plus staleness and topic gating.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "core/stat_db.h"
#include "core/stat_service.h"
#include "unity.h"

#define TEST_DB "/tmp/dawn_stat_test.db"

static void unlink_db(void) {
   unlink(TEST_DB);
   unlink(TEST_DB "-wal");
   unlink(TEST_DB "-shm");
}

void setUp(void) {
   unlink_db();
   stat_service_cfg_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.enabled = true;
   snprintf(cfg.db_path, sizeof(cfg.db_path), "%s", TEST_DB);
   cfg.stale_after_sec = 30;
   cfg.history_retention_days = 30;
   TEST_ASSERT_EQUAL_INT(0, stat_service_init(&cfg));
}

void tearDown(void) {
   stat_service_shutdown();
   unlink_db();
}

static void feed_system(double cpu, double mem, double temp) {
   char msg[256];
   snprintf(msg, sizeof(msg),
            "{\"device\":\"stat\",\"msg_type\":\"telemetry\",\"type\":\"SystemMetrics\","
            "\"cpu_usage\":%.2f,\"memory_usage\":%.2f,\"system_temp\":%.2f}",
            cpu, mem, temp);
   int consumed = stat_service_handle_mqtt("stat/telemetry", msg, (int)strlen(msg));
   TEST_ASSERT_EQUAL_INT(1, consumed);
}

static void feed_battery(double level, double voltage, double power, double temp) {
   char msg[384];
   snprintf(msg, sizeof(msg),
            "{\"device\":\"stat\",\"msg_type\":\"telemetry\",\"type\":\"BatteryStatus\","
            "\"voltage\":%.2f,\"current\":1.20,\"power\":%.2f,\"battery_level\":%.2f,"
            "\"temperature\":%.2f,\"charging_state\":\"discharging\","
            "\"battery_status\":\"OK\",\"time_remaining_min\":200.0,"
            "\"critical_fault_count\":0,\"warning_fault_count\":0,\"info_fault_count\":0}",
            voltage, power, level, temp);
   int consumed = stat_service_handle_mqtt("stat/telemetry", msg, (int)strlen(msg));
   TEST_ASSERT_EQUAL_INT(1, consumed);
}

static void feed_fan(int rpm) {
   char msg[192];
   snprintf(msg, sizeof(msg),
            "{\"device\":\"stat\",\"msg_type\":\"telemetry\",\"type\":\"Fan\","
            "\"rpm\":%d,\"load\":30,\"pwm\":100}",
            rpm);
   int consumed = stat_service_handle_mqtt("stat/telemetry", msg, (int)strlen(msg));
   TEST_ASSERT_EQUAL_INT(1, consumed);
}

/* Foreign topics must not be consumed (return 0 → normal dispatch continues). */
void test_topic_gating(void) {
   int consumed = stat_service_handle_mqtt("echo/events", "{}", 2);
   TEST_ASSERT_EQUAL_INT(0, consumed);
}

/* Before any message: no data, stale. */
void test_snapshot_never_seen(void) {
   stat_snapshot_t s;
   stat_service_get_snapshot(&s);
   TEST_ASSERT_FALSE(s.ever_seen);
   TEST_ASSERT_TRUE(s.stale);
   TEST_ASSERT_FALSE(s.have_system);
}

/* Live cache reflects the latest values of each family. */
void test_live_cache_latest(void) {
   feed_system(10.0, 40.0, 45.0);
   feed_system(20.0, 42.0, 50.0);
   feed_battery(80.0, 14.6, 17.5, 30.0);
   feed_fan(2000);

   stat_snapshot_t s;
   stat_service_get_snapshot(&s);
   TEST_ASSERT_TRUE(s.ever_seen);
   TEST_ASSERT_TRUE(s.stat_online);
   TEST_ASSERT_FALSE(s.stale);
   TEST_ASSERT_TRUE(s.have_system);
   TEST_ASSERT_EQUAL_DOUBLE(20.0, s.cpu_usage); /* latest, not first */
   TEST_ASSERT_EQUAL_DOUBLE(50.0, s.system_temp);
   TEST_ASSERT_TRUE(s.have_battery);
   TEST_ASSERT_EQUAL_DOUBLE(80.0, s.batt_level);
   TEST_ASSERT_EQUAL_STRING("discharging", s.charging_state);
   TEST_ASSERT_TRUE(s.have_fan);
   TEST_ASSERT_EQUAL_INT(2000, s.fan_rpm);
}

/* Accumulator fold + SQL aggregation: exact min/max, weighted avg, counts. */
void test_history_aggregation(void) {
   feed_system(10.0, 40.0, 45.0);
   feed_system(20.0, 42.0, 50.0); /* peak temp */
   feed_system(30.0, 44.0, 40.0); /* min temp */
   feed_battery(80.0, 14.6, 17.5, 30.0);
   feed_battery(78.0, 14.5, 18.0, 31.0); /* min level */
   feed_fan(2000);

   TEST_ASSERT_EQUAL_INT(0 /*SUCCESS*/, stat_history_flush());

   stat_history_agg_t agg;
   TEST_ASSERT_EQUAL_INT(0, stat_db_history(0, (int64_t)time(NULL) + 1000, &agg));
   TEST_ASSERT_TRUE(agg.have_data);
   TEST_ASSERT_EQUAL_INT(1, agg.bucket_count);
   TEST_ASSERT_EQUAL_INT(3, agg.total_sys);
   TEST_ASSERT_EQUAL_INT(2, agg.total_batt);
   TEST_ASSERT_EQUAL_INT(1, agg.total_fan);

   /* Temperature: min 40, max 50, avg (45+50+40)/3 = 45 */
   TEST_ASSERT_EQUAL_DOUBLE(40.0, agg.temp_min);
   TEST_ASSERT_EQUAL_DOUBLE(50.0, agg.temp_max);
   TEST_ASSERT_DOUBLE_WITHIN(0.01, 45.0, agg.temp_avg);
   /* CPU: avg (10+20+30)/3 = 20, peak 30 */
   TEST_ASSERT_DOUBLE_WITHIN(0.01, 20.0, agg.cpu_avg);
   TEST_ASSERT_EQUAL_DOUBLE(30.0, agg.cpu_max);
   /* Battery: min 78, max 80, avg 79 */
   TEST_ASSERT_EQUAL_DOUBLE(78.0, agg.batt_min);
   TEST_ASSERT_EQUAL_DOUBLE(80.0, agg.batt_max);
   TEST_ASSERT_DOUBLE_WITHIN(0.01, 79.0, agg.batt_avg);
   /* Fan */
   TEST_ASSERT_EQUAL_DOUBLE(2000.0, agg.fan_max);
}

/* An empty bucket (no samples since last flush) must NOT write a row. */
void test_empty_bucket_skipped(void) {
   feed_system(10.0, 40.0, 45.0);
   TEST_ASSERT_EQUAL_INT(0, stat_history_flush()); /* writes 1 bucket */
   TEST_ASSERT_EQUAL_INT(0, stat_history_flush()); /* no samples → no row */

   stat_history_agg_t agg;
   TEST_ASSERT_EQUAL_INT(0, stat_db_history(0, (int64_t)time(NULL) + 1000, &agg));
   TEST_ASSERT_EQUAL_INT(1, agg.bucket_count); /* still just the one */
}

/* Two populated buckets aggregate together across the window. */
void test_multi_bucket(void) {
   feed_system(10.0, 40.0, 60.0);
   TEST_ASSERT_EQUAL_INT(0, stat_history_flush());
   feed_system(20.0, 40.0, 70.0); /* higher peak in second bucket */
   TEST_ASSERT_EQUAL_INT(0, stat_history_flush());

   stat_history_agg_t agg;
   TEST_ASSERT_EQUAL_INT(0, stat_db_history(0, (int64_t)time(NULL) + 1000, &agg));
   TEST_ASSERT_EQUAL_INT(2, agg.bucket_count);
   TEST_ASSERT_EQUAL_INT(2, agg.total_sys);
   TEST_ASSERT_EQUAL_DOUBLE(70.0, agg.temp_max);
   TEST_ASSERT_EQUAL_DOUBLE(60.0, agg.temp_min);
}

/* ===== stat_db_series() — the chartable trend reader ===== */

/* Insert a SystemMetrics-family bucket at an explicit time (weight = count). */
static void insert_sys_bucket(int64_t t,
                              double temp_avg,
                              double temp_min,
                              double temp_max,
                              double cpu,
                              int count) {
   stat_bucket_row_t r;
   memset(&r, 0, sizeof(r));
   r.bucket_start = t;
   r.have_sys = true;
   r.sys_count = count;
   r.temp_avg = temp_avg;
   r.temp_min = temp_min;
   r.temp_max = temp_max;
   r.cpu_avg = cpu;
   r.cpu_max = cpu;
   r.mem_avg = 40.0;
   r.mem_max = 40.0;
   TEST_ASSERT_EQUAL_INT(0, stat_db_insert_bucket(&r));
}

/* Insert a BatteryStatus-family bucket at an explicit time. */
static void insert_batt_bucket(int64_t t, double level, double power) {
   stat_bucket_row_t r;
   memset(&r, 0, sizeof(r));
   r.bucket_start = t;
   r.have_batt = true;
   r.batt_count = 1;
   r.batt_avg = level;
   r.batt_min = level;
   r.batt_max = level;
   r.batt_v_avg = 14.5;
   r.batt_p_avg = power;
   r.batt_temp_max = 30.0;
   TEST_ASSERT_EQUAL_INT(0, stat_db_insert_bucket(&r));
}

/* Short window (4 native buckets) → no grouping. */
void test_series_short_window(void) {
   for (int i = 0; i < 4; i++) {
      insert_sys_bucket(1000 + i * 900, 45.0, 45.0, 45.0, 10.0, 1);
   }
   stat_series_t s;
   TEST_ASSERT_EQUAL_INT(0, stat_db_series(1000, 1000 + 3 * 900, STAT_METRIC_TEMP, &s));
   TEST_ASSERT_EQUAL_INT(4, s.count);
   TEST_ASSERT_EQUAL_INT(900, s.group_secs);
   TEST_ASSERT_TRUE(s.has_min);
   TEST_ASSERT_TRUE(s.has_max);
   TEST_ASSERT_TRUE(s.points[0].have_avg);
   TEST_ASSERT_TRUE(s.points[0].have_min);
}

/* H1: a non-aligned span ("today" mid-afternoon, 47,220s) must not blow the cap.
 * 53 native buckets → ceil(47220/43200)=2 → group_secs 1800 → 27 groups. */
void test_series_span_47220_cap(void) {
   int64_t base = 100000;
   for (int i = 0; i < 53; i++) {
      insert_sys_bucket(base + i * 900, 45.0, 45.0, 45.0, 10.0, 1);
   }
   stat_series_t s;
   TEST_ASSERT_EQUAL_INT(0, stat_db_series(base, base + 47220, STAT_METRIC_TEMP, &s));
   TEST_ASSERT_EQUAL_INT(1800, s.group_secs);
   TEST_ASSERT_TRUE(s.count <= STAT_SERIES_MAX_POINTS);
   TEST_ASSERT_EQUAL_INT(27, s.count);
}

/* H2: an aligned end (span 43,200) with a bucket AT end can yield a 49th group;
 * the +1 array slot + capped loop must hold it with no overrun. */
void test_series_aligned_end_no_overrun(void) {
   int64_t base = 200000;
   for (int i = 0; i <= 48; i++) { /* 49 buckets, last exactly at base+43200 */
      insert_sys_bucket(base + i * 900, 45.0, 45.0, 45.0, 10.0, 1);
   }
   stat_series_t s;
   TEST_ASSERT_EQUAL_INT(0, stat_db_series(base, base + 43200, STAT_METRIC_TEMP, &s));
   TEST_ASSERT_EQUAL_INT(900, s.group_secs);
   TEST_ASSERT_TRUE(s.count <= STAT_SERIES_MAX_POINTS + 1);
   TEST_ASSERT_EQUAL_INT(49, s.count);
}

/* 7-day span (672 native buckets) → ceil(604800/43200)=14 → group_secs 12600 → 48 groups. */
void test_series_7d_downsample(void) {
   int64_t base = 1000000;
   for (int i = 0; i < 672; i++) {
      insert_sys_bucket(base + i * 900, 50.0, 50.0, 50.0, 10.0, 1);
   }
   stat_series_t s;
   TEST_ASSERT_EQUAL_INT(0, stat_db_series(base, base + 604800, STAT_METRIC_TEMP, &s));
   TEST_ASSERT_EQUAL_INT(12600, s.group_secs);
   TEST_ASSERT_TRUE(s.count <= STAT_SERIES_MAX_POINTS);
}

/* Weighted average + exact min/max across a downsampled group. */
void test_series_weighted_group(void) {
   int64_t base = 300000;
   /* Two buckets share group 0 (group_secs will be 1800): weights 1 and 3. */
   insert_sys_bucket(base + 0, 40.0, 38.0, 42.0, 10.0, 1);
   insert_sys_bucket(base + 900, 50.0, 48.0, 52.0, 10.0, 3);
   /* A distant bucket stretches the span so group_secs becomes 1800. */
   insert_sys_bucket(base + 50000, 60.0, 60.0, 60.0, 10.0, 1);

   stat_series_t s;
   TEST_ASSERT_EQUAL_INT(0, stat_db_series(base, base + 50000, STAT_METRIC_TEMP, &s));
   TEST_ASSERT_EQUAL_INT(1800, s.group_secs);
   /* group 0: weighted avg (40*1 + 50*3)/4 = 47.5, min 38, max 52 */
   TEST_ASSERT_TRUE(s.points[0].have_avg);
   TEST_ASSERT_DOUBLE_WITHIN(0.01, 47.5, s.points[0].avg);
   TEST_ASSERT_EQUAL_DOUBLE(38.0, s.points[0].min);
   TEST_ASSERT_EQUAL_DOUBLE(52.0, s.points[0].max);
}

/* H3: a group with no samples for the metric's family is a GAP (have_avg false),
 * never a spurious 0.0. */
void test_series_null_gap_not_zero(void) {
   int64_t base = 400000;
   insert_sys_bucket(base, 45.0, 45.0, 45.0, 10.0, 1);       /* sys only */
   insert_batt_bucket(base, 80.0, 15.0);                     /* + battery in group 0 */
   insert_sys_bucket(base + 900, 46.0, 46.0, 46.0, 10.0, 1); /* group 1: sys only, NO battery */

   stat_series_t s;
   TEST_ASSERT_EQUAL_INT(0, stat_db_series(base, base + 900, STAT_METRIC_BATTERY, &s));
   TEST_ASSERT_EQUAL_INT(2, s.count);
   TEST_ASSERT_TRUE(s.points[0].have_avg); /* battery present */
   TEST_ASSERT_EQUAL_DOUBLE(80.0, s.points[0].avg);
   TEST_ASSERT_FALSE(s.points[1].have_avg); /* battery absent → gap, not 0 */
}

/* Power is an avg-only metric: no min/max columns. */
void test_series_power_avg_only(void) {
   insert_batt_bucket(500000, 80.0, 17.5);
   stat_series_t s;
   TEST_ASSERT_EQUAL_INT(0, stat_db_series(500000, 500000, STAT_METRIC_POWER, &s));
   TEST_ASSERT_EQUAL_INT(1, s.count);
   TEST_ASSERT_FALSE(s.has_min);
   TEST_ASSERT_FALSE(s.has_max);
   TEST_ASSERT_TRUE(s.points[0].have_avg);
   TEST_ASSERT_FALSE(s.points[0].have_min);
   TEST_ASSERT_DOUBLE_WITHIN(0.01, 17.5, s.points[0].avg);
}

/* CPU stores avg + max (no min). */
void test_series_cpu_avg_max(void) {
   insert_sys_bucket(600000, 45.0, 45.0, 45.0, 22.0, 1);
   stat_series_t s;
   TEST_ASSERT_EQUAL_INT(0, stat_db_series(600000, 600000, STAT_METRIC_CPU, &s));
   TEST_ASSERT_FALSE(s.has_min);
   TEST_ASSERT_TRUE(s.has_max);
   TEST_ASSERT_TRUE(s.points[0].have_avg);
   TEST_ASSERT_TRUE(s.points[0].have_max);
   TEST_ASSERT_FALSE(s.points[0].have_min);
}

/* Empty window → zero points (tool surfaces the error-mark path). */
void test_series_empty_window(void) {
   stat_series_t s;
   TEST_ASSERT_EQUAL_INT(0, stat_db_series(900000000, 900000900, STAT_METRIC_TEMP, &s));
   TEST_ASSERT_EQUAL_INT(0, s.count);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_topic_gating);
   RUN_TEST(test_snapshot_never_seen);
   RUN_TEST(test_live_cache_latest);
   RUN_TEST(test_history_aggregation);
   RUN_TEST(test_empty_bucket_skipped);
   RUN_TEST(test_multi_bucket);
   RUN_TEST(test_series_short_window);
   RUN_TEST(test_series_span_47220_cap);
   RUN_TEST(test_series_aligned_end_no_overrun);
   RUN_TEST(test_series_7d_downsample);
   RUN_TEST(test_series_weighted_group);
   RUN_TEST(test_series_null_gap_not_zero);
   RUN_TEST(test_series_power_avg_only);
   RUN_TEST(test_series_cpu_avg_max);
   RUN_TEST(test_series_empty_window);
   return UNITY_END();
}
