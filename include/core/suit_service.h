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
 * Suit-telemetry service — subscribes (passively, via mosquitto_comms) to the
 * AURA helmet (helmet/telemetry) and SPARK armor (armor/telemetry) feeds that
 * MIRAGE republishes, caches the latest values in memory (mutex-guarded, per-
 * subsystem TTL staleness), and exposes a snapshot to the suit_status tool.
 * Modeled on stat_service.c (external MQTT source -> cache -> tool).  Live cache
 * only; no history store (that is a deferred follow-up, unlike stat).  This is
 * the SAGE (proactive attention) PRE phase — see docs/PROACTIVE_ATTENTION_DESIGN.md.
 */

#ifndef SUIT_SERVICE_H
#define SUIT_SERVICE_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SUIT_HELMET_TOPIC_DEFAULT "helmet/telemetry"
#define SUIT_ARMOR_TOPIC_DEFAULT "armor/telemetry"

/* Default staleness window (seconds).  Mirror the value in suit_tool.c's config
 * initializer and the test fixture if this changes — the tool layer owns config
 * and can't reach the service fallback below at compile time. */
#define SUIT_STALE_AFTER_SEC_DEFAULT 30

/* Bounded roster of armor pieces tracked at once.  SPARK armor is a handful of
 * pieces (gauntlets, chest, boots, ...); 16 is comfortable headroom. */
#define SUIT_MAX_ARMOR_PIECES 16
#define SUIT_PIECE_NAME_LEN 32

/** Resolved configuration handed to the service by the tool at init. */
typedef struct {
   bool enabled;
   char helmet_topic[128];
   char armor_topic[128];
   int stale_after_sec; /* live reads older than this report "stale" */
} suit_service_cfg_t;

/** One armor piece's latest telemetry (copied out of the cache). */
typedef struct {
   char name[SUIT_PIECE_NAME_LEN];
   double temp;    /* last reported piece temperature (deg C) */
   double voltage; /* last reported piece voltage (V) */
   bool have_temp;
   bool have_voltage;
   int age_sec; /* seconds since this piece last reported */
   bool stale;  /* age_sec > stale_after_sec (piece believed offline) */
} suit_armor_piece_t;

/** A consistent copy-out of the live cache (see suit_service_get_snapshot). */
typedef struct {
   bool ever_seen; /* any suit telemetry (helmet or armor) received since boot */

   /* Helmet environment (AURA "Enviro"): air quality, CO2, heat. */
   bool have_enviro;
   int enviro_age_sec;
   bool enviro_stale;
   double temp, humidity, air_quality, tvoc_ppb, eco2_ppm, co2_ppm, heat_index_c, dew_point;
   char air_quality_desc[16];
   char co2_quality_desc[16];

   /* Helmet orientation (AURA "Motion"). */
   bool have_motion;
   int motion_age_sec;
   bool motion_stale;
   double heading, pitch, roll;

   /* Helmet position (AURA "GPS"). */
   bool have_gps;
   int gps_age_sec;
   bool gps_stale;
   int fix, quality, satellites;
   double latitude, longitude, altitude, speed;

   /* SPARK armor roster. */
   int armor_count;
   suit_armor_piece_t armor[SUIT_MAX_ARMOR_PIECES];
} suit_snapshot_t;

/**
 * @brief Initialize the service (from the tool `.init`, before MQTT connects).
 *
 * Copies @p cfg and clears the cache.  Does NOT touch MQTT (subscriptions are
 * centralized in mosquitto_comms.c on_connect).  Returns SUCCESS even when
 * disabled so tool registration is never aborted.
 */
int suit_service_init(const suit_service_cfg_t *cfg);

/** @brief Free state (no-op beyond marking inactive; no DB to close). */
void suit_service_shutdown(void);

/** @brief True when enabled and initialized (gate subscribe/dispatch on this). */
bool suit_service_is_active(void);

/** @brief Configured helmet telemetry topic (for the on_connect subscribe). */
const char *suit_service_helmet_topic(void);

/** @brief Configured armor telemetry topic (for the on_connect subscribe). */
const char *suit_service_armor_topic(void);

/**
 * @brief Consume an MQTT message if @p topic is one of ours.
 *
 * Called from on_message ABOVE the generic INFO log so the suit feed does not
 * flood the log.  Parses length-bounded (payload may not be NUL-terminated) and
 * updates the cache under the mutex.
 *
 * @return 1 if the message was ours (consumed), 0 otherwise.
 */
int suit_service_handle_mqtt(const char *topic, const char *payload, int payload_len);

/** @brief Copy the live cache out (mutex-guarded); computes stale/age vs now. */
void suit_service_get_snapshot(suit_snapshot_t *out);

#ifdef __cplusplus
}
#endif

#endif /* SUIT_SERVICE_H */
