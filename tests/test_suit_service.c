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
 * Unit tests for the suit-telemetry service: helmet (Enviro/Motion/GPS) +
 * armor ingest, latest-value cache, per-subsystem staleness, armor-roster
 * upsert, and topic gating.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "core/suit_service.h"
#include "unity.h"

void setUp(void) {
   suit_service_cfg_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.enabled = true;
   snprintf(cfg.helmet_topic, sizeof(cfg.helmet_topic), "%s", SUIT_HELMET_TOPIC_DEFAULT);
   snprintf(cfg.armor_topic, sizeof(cfg.armor_topic), "%s", SUIT_ARMOR_TOPIC_DEFAULT);
   cfg.stale_after_sec = SUIT_STALE_AFTER_SEC_DEFAULT;
   TEST_ASSERT_EQUAL_INT(0, suit_service_init(&cfg));
}

void tearDown(void) {
   suit_service_shutdown();
}

static void feed_helmet(const char *json) {
   int consumed = suit_service_handle_mqtt("helmet/telemetry", json, (int)strlen(json));
   TEST_ASSERT_EQUAL_INT(1, consumed);
}

static void feed_armor(const char *json) {
   int consumed = suit_service_handle_mqtt("armor/telemetry", json, (int)strlen(json));
   TEST_ASSERT_EQUAL_INT(1, consumed);
}

static void feed_enviro(double co2, double temp) {
   char msg[384];
   snprintf(msg, sizeof(msg),
            "{\"device\":\"aura\",\"msg_type\":\"telemetry\",\"type\":\"Enviro\","
            "\"co2_ppm\":%.1f,\"eco2_ppm\":450.0,\"tvoc_ppb\":120.0,\"air_quality\":88.0,"
            "\"air_quality_description\":\"Good\",\"co2_quality\":\"Normal\","
            "\"temp\":%.1f,\"humidity\":40.0,\"heat_index_c\":%.1f,\"dew_point\":10.0}",
            co2, temp, temp);
   feed_helmet(msg);
}

/* Foreign topics must not be consumed (return 0 → normal dispatch continues). */
void test_topic_gating(void) {
   TEST_ASSERT_EQUAL_INT(0, suit_service_handle_mqtt("stat/telemetry", "{}", 2));
   TEST_ASSERT_EQUAL_INT(0, suit_service_handle_mqtt("echo/events", "{}", 2));
}

/* Before any message: nothing seen, every subsystem stale. */
void test_snapshot_never_seen(void) {
   suit_snapshot_t s;
   suit_service_get_snapshot(&s);
   TEST_ASSERT_FALSE(s.ever_seen);
   TEST_ASSERT_FALSE(s.have_enviro);
   TEST_ASSERT_TRUE(s.enviro_stale);
   TEST_ASSERT_EQUAL_INT(0, s.armor_count);
}

/* Enviro ingest reflects the latest values (CO2 is the PRE headline). */
void test_enviro_latest(void) {
   feed_enviro(600.0, 24.0);
   feed_enviro(750.0, 25.5); /* latest wins */

   suit_snapshot_t s;
   suit_service_get_snapshot(&s);
   TEST_ASSERT_TRUE(s.ever_seen);
   TEST_ASSERT_TRUE(s.have_enviro);
   TEST_ASSERT_FALSE(s.enviro_stale);
   TEST_ASSERT_EQUAL_DOUBLE(750.0, s.co2_ppm);
   TEST_ASSERT_EQUAL_DOUBLE(25.5, s.temp);
   TEST_ASSERT_EQUAL_STRING("Good", s.air_quality_desc);
   TEST_ASSERT_EQUAL_STRING("Normal", s.co2_quality_desc);
}

/* Motion + GPS parse into their own subsystems; fix gates position validity. */
void test_motion_and_gps(void) {
   feed_helmet("{\"type\":\"Motion\",\"heading\":270.0,\"pitch\":3.0,\"roll\":-2.0}");
   feed_helmet("{\"type\":\"GPS\",\"fix\":1,\"quality\":2,\"satellites\":9,"
               "\"latitude\":34.05,\"longitude\":-118.24,\"altitude\":89.0,\"speed\":1.4}");

   suit_snapshot_t s;
   suit_service_get_snapshot(&s);
   TEST_ASSERT_TRUE(s.have_motion);
   TEST_ASSERT_EQUAL_DOUBLE(270.0, s.heading);
   TEST_ASSERT_TRUE(s.have_gps);
   TEST_ASSERT_EQUAL_INT(1, s.fix);
   TEST_ASSERT_EQUAL_INT(9, s.satellites);
   TEST_ASSERT_EQUAL_DOUBLE(34.05, s.latitude);
}

/* An unknown helmet "type" is consumed but changes nothing. */
void test_unknown_type_ignored(void) {
   feed_helmet("{\"type\":\"Bogus\",\"foo\":1}");
   suit_snapshot_t s;
   suit_service_get_snapshot(&s);
   TEST_ASSERT_FALSE(s.have_enviro);
   TEST_ASSERT_FALSE(s.have_motion);
   TEST_ASSERT_FALSE(s.have_gps);
   /* ever_seen only flips when a recognized subsystem folds a sample. */
   TEST_ASSERT_FALSE(s.ever_seen);
}

/* Non-finite numbers from a misbehaving publisher are rejected, not cached. */
void test_nonfinite_rejected(void) {
   feed_enviro(600.0, 24.0);
   feed_helmet("{\"type\":\"Enviro\",\"co2_ppm\":1e999}"); /* inf → rejected, keep 600 */
   suit_snapshot_t s;
   suit_service_get_snapshot(&s);
   TEST_ASSERT_EQUAL_DOUBLE(600.0, s.co2_ppm);
}

/* Armor roster upserts by piece name; temp/voltage tracked per piece. */
void test_armor_roster_upsert(void) {
   feed_armor("{\"type\":\"Armor\",\"piece\":\"left_gauntlet\",\"temp\":38.0,\"voltage\":11.9}");
   feed_armor("{\"type\":\"Armor\",\"piece\":\"chest\",\"temp\":41.0,\"voltage\":12.1}");
   feed_armor("{\"type\":\"Armor\",\"piece\":\"left_gauntlet\",\"temp\":39.5,\"voltage\":11.7}");

   suit_snapshot_t s;
   suit_service_get_snapshot(&s);
   TEST_ASSERT_EQUAL_INT(2, s.armor_count); /* two distinct pieces */

   const suit_armor_piece_t *lg = NULL;
   for (int i = 0; i < s.armor_count; i++) {
      if (strcmp(s.armor[i].name, "left_gauntlet") == 0) {
         lg = &s.armor[i];
      }
   }
   TEST_ASSERT_NOT_NULL(lg);
   TEST_ASSERT_TRUE(lg->have_temp);
   TEST_ASSERT_EQUAL_DOUBLE(39.5, lg->temp); /* latest, not first */
   TEST_ASSERT_FALSE(lg->stale);
}

/* An armor message with no piece name is ignored (not addressable). */
void test_armor_nameless_ignored(void) {
   feed_armor("{\"type\":\"Armor\",\"temp\":40.0}");
   suit_snapshot_t s;
   suit_service_get_snapshot(&s);
   TEST_ASSERT_EQUAL_INT(0, s.armor_count);
}

/* Roster is bounded: extra pieces beyond the cap are dropped, not overflowing. */
void test_armor_roster_cap(void) {
   char msg[128];
   for (int i = 0; i < SUIT_MAX_ARMOR_PIECES + 5; i++) {
      snprintf(msg, sizeof(msg), "{\"type\":\"Armor\",\"piece\":\"piece_%d\",\"temp\":30.0}", i);
      feed_armor(msg);
   }
   suit_snapshot_t s;
   suit_service_get_snapshot(&s);
   TEST_ASSERT_EQUAL_INT(SUIT_MAX_ARMOR_PIECES, s.armor_count);
}

/* Empty payload on our topic is consumed but ingests nothing. */
void test_empty_payload_consumed(void) {
   TEST_ASSERT_EQUAL_INT(1, suit_service_handle_mqtt("helmet/telemetry", "", 0));
   suit_snapshot_t s;
   suit_service_get_snapshot(&s);
   TEST_ASSERT_FALSE(s.ever_seen);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_topic_gating);
   RUN_TEST(test_snapshot_never_seen);
   RUN_TEST(test_enviro_latest);
   RUN_TEST(test_motion_and_gps);
   RUN_TEST(test_unknown_type_ignored);
   RUN_TEST(test_nonfinite_rejected);
   RUN_TEST(test_armor_roster_upsert);
   RUN_TEST(test_armor_nameless_ignored);
   RUN_TEST(test_armor_roster_cap);
   RUN_TEST(test_empty_payload_consumed);
   return UNITY_END();
}
