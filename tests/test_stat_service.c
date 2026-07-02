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

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_topic_gating);
   RUN_TEST(test_snapshot_never_seen);
   RUN_TEST(test_live_cache_latest);
   RUN_TEST(test_history_aggregation);
   RUN_TEST(test_empty_bucket_skipped);
   RUN_TEST(test_multi_bucket);
   return UNITY_END();
}
