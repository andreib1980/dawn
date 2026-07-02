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
 * STAT telemetry service — subscribes (passively, via mosquitto_comms) to the
 * external STAT daemon's MQTT telemetry, caches the latest values in memory
 * (mutex-guarded, TTL-stale), and folds every sample into rollup accumulators
 * that the maintenance thread flushes to stat_db.  Modeled on phone_service.c
 * (external MQTT daemon -> cache -> tool) + component_status.c (TTL staleness).
 */

#ifndef STAT_SERVICE_H
#define STAT_SERVICE_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STAT_TELEMETRY_TOPIC_DEFAULT "stat/telemetry"
#define STAT_STATUS_TOPIC_DEFAULT "stat/status"

/** Resolved configuration handed to the service by the tool at init. */
typedef struct {
   bool enabled;
   char telemetry_topic[128];
   char status_topic[128];
   char db_path[256];
   int stale_after_sec;        /* live reads older than this report "stale" */
   int history_retention_days; /* prune buckets older than this */
} stat_service_cfg_t;

/** A consistent copy-out of the live cache (see stat_service_get_snapshot). */
typedef struct {
   bool stat_online; /* last stat/status said "online" */
   bool ever_seen;   /* any telemetry received since boot */
   bool stale;       /* (now - last_seen) > stale_after_sec, or never seen */
   int age_sec;      /* seconds since last telemetry (0 if never) */
   time_t last_seen; /* wall-clock receipt of last telemetry */

   bool have_system;
   double cpu_usage, memory_usage, system_temp;

   bool have_fan;
   int fan_rpm, fan_load, fan_pwm;

   bool have_battery;
   double batt_voltage, batt_current, batt_power, batt_level, batt_temp, time_remaining_min;
   char charging_state[24];
   char battery_status[24]; /* OK / WARNING / CRITICAL */
   char status_reason[96];
   int crit_faults, warn_faults, info_faults;
} stat_snapshot_t;

/**
 * @brief Initialize the service (from the tool `.init`, before MQTT connects).
 *
 * Copies @p cfg, inits the cache, and opens stat_db if enabled.  Does NOT touch
 * MQTT (subscriptions are centralized in mosquitto_comms.c on_connect).  Returns
 * SUCCESS even if the DB fails to open (live cache still works; history off) so
 * tool registration is never aborted.
 */
int stat_service_init(const stat_service_cfg_t *cfg);

/** @brief Flush the final partial bucket, close stat_db, free state. */
void stat_service_shutdown(void);

/** @brief True when enabled and initialized (gate subscribe/dispatch on this). */
bool stat_service_is_active(void);

/** @brief Configured telemetry topic (for the on_connect subscribe). */
const char *stat_service_telemetry_topic(void);

/** @brief Configured status topic (retained/LWT; for the on_connect subscribe). */
const char *stat_service_status_topic(void);

/**
 * @brief Consume an MQTT message if @p topic is one of ours.
 *
 * Called from on_message ABOVE the generic INFO log so STAT's multi-Hz firehose
 * does not flood the log.  Parses length-bounded (payload may not be
 * NUL-terminated) and updates the cache + accumulators under the mutex.
 *
 * @return 1 if the message was ours (consumed), 0 otherwise.
 */
int stat_service_handle_mqtt(const char *topic, const char *payload, int payload_len);

/** @brief Copy the live cache out (mutex-guarded); computes stale/age vs now. */
void stat_service_get_snapshot(stat_snapshot_t *out);

/**
 * @brief Flush accumulators to one history bucket + prune + checkpoint.
 *
 * Called from the auth_maintenance loop.  Copies+resets accumulators under the
 * cache mutex, then writes/prunes via stat_db (no lock nesting).  No-op when the
 * bucket had zero samples.  Failure-isolated: always returns, never propagates.
 */
int stat_history_flush(void);

#ifdef __cplusplus
}
#endif

#endif /* STAT_SERVICE_H */
