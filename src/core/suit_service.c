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
 * Suit-telemetry service — MQTT ingest of the AURA helmet + SPARK armor feeds
 * (republished by MIRAGE) into an in-memory latest-value cache with per-
 * subsystem TTL staleness.  See include/core/suit_service.h.
 */

#include "core/suit_service.h"

#include <json-c/json.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "dawn_error.h"
#include "logging.h"

/* =============================================================================
 * State
 * ============================================================================= */

static suit_service_cfg_t s_cfg;
/* Set by main-thread init/shutdown, read by the MQTT + worker threads.  Atomic
 * so the cross-thread reads are well-defined. */
static atomic_bool s_active = false;

/* Guards the live cache.  Touched by the MQTT thread (ingest) and worker threads
 * (snapshot).  A single leaf lock — critical sections copy out, then process. */
static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;

/* One armor piece as held in the cache. */
typedef struct {
   char name[SUIT_PIECE_NAME_LEN];
   double temp, voltage;
   bool have_temp, have_voltage;
   time_t last_seen;
} armor_slot_t;

/* Latest live values.  Each helmet subsystem carries its own last_seen because
 * they arrive at different cadences (Motion streams; Enviro/GPS are slow), so a
 * single feed timestamp would mask a stalled Enviro behind fresh Motion. */
static struct {
   bool ever_seen;

   bool have_enviro;
   time_t enviro_last_seen;
   double temp, humidity, air_quality, tvoc_ppb, eco2_ppm, co2_ppm, heat_index_c, dew_point;
   char air_quality_desc[16];
   char co2_quality_desc[16];

   bool have_motion;
   time_t motion_last_seen;
   double heading, pitch, roll;

   bool have_gps;
   time_t gps_last_seen;
   int fix, quality, satellites;
   double latitude, longitude, altitude, speed;

   int armor_count;
   armor_slot_t armor[SUIT_MAX_ARMOR_PIECES];
} s_cache;

/* =============================================================================
 * JSON helpers (mirror stat_service.c — untrusted broker input)
 * ============================================================================= */

/* Extract a finite double.  Rejects a missing key AND non-finite values
 * (inf/nan) so they never reach the double->int casts in the tool formatter. */
static bool json_get_double(struct json_object *root, const char *key, double *out) {
   struct json_object *v;
   if (json_object_object_get_ex(root, key, &v)) {
      double d = json_object_get_double(v);
      if (!isfinite(d)) {
         return false;
      }
      *out = d;
      return true;
   }
   return false;
}

static bool json_get_int(struct json_object *root, const char *key, int *out) {
   struct json_object *v;
   if (json_object_object_get_ex(root, key, &v)) {
      *out = json_object_get_int(v);
      return true;
   }
   return false;
}

/* Copy a string field, replacing control characters with spaces.  These fields
 * come from untrusted MQTT and are surfaced into the LLM tool result, so strip
 * newlines/control bytes that could break formatting or inject structure. */
static void json_get_str(struct json_object *root, const char *key, char *out, size_t out_sz) {
   struct json_object *v;
   if (!json_object_object_get_ex(root, key, &v)) {
      return;
   }
   const char *s = json_object_get_string(v);
   snprintf(out, out_sz, "%s", s ? s : "");
   for (char *p = out; *p; p++) {
      unsigned char c = (unsigned char)*p;
      if (c < 0x20 || c == 0x7F) {
         *p = ' ';
      }
   }
}

/* Mark that fresh suit telemetry arrived.  Caller holds s_mutex. */
static void mark_seen_locked(void) {
   s_cache.ever_seen = true;
}

/* =============================================================================
 * Ingest (helmet AURA)
 * ============================================================================= */

/* --- Enviro: air quality, CO2, heat --- */
static void ingest_enviro(struct json_object *root) {
   double temp = 0, humidity = 0, aq = 0, tvoc = 0, eco2 = 0, co2 = 0, heat = 0, dew = 0;
   bool has_temp = json_get_double(root, "temp", &temp);
   bool has_hum = json_get_double(root, "humidity", &humidity);
   bool has_aq = json_get_double(root, "air_quality", &aq);
   bool has_tvoc = json_get_double(root, "tvoc_ppb", &tvoc);
   bool has_eco2 = json_get_double(root, "eco2_ppm", &eco2);
   bool has_co2 = json_get_double(root, "co2_ppm", &co2);
   bool has_heat = json_get_double(root, "heat_index_c", &heat);
   bool has_dew = json_get_double(root, "dew_point", &dew);
   char aq_desc[16] = "", co2_desc[16] = "";
   json_get_str(root, "air_quality_description", aq_desc, sizeof(aq_desc));
   json_get_str(root, "co2_quality", co2_desc, sizeof(co2_desc));
   time_t now = time(NULL);

   pthread_mutex_lock(&s_mutex);
   s_cache.have_enviro = true;
   s_cache.enviro_last_seen = now;
   if (has_temp)
      s_cache.temp = temp;
   if (has_hum)
      s_cache.humidity = humidity;
   if (has_aq)
      s_cache.air_quality = aq;
   if (has_tvoc)
      s_cache.tvoc_ppb = tvoc;
   if (has_eco2)
      s_cache.eco2_ppm = eco2;
   if (has_co2)
      s_cache.co2_ppm = co2;
   if (has_heat)
      s_cache.heat_index_c = heat;
   if (has_dew)
      s_cache.dew_point = dew;
   if (aq_desc[0])
      snprintf(s_cache.air_quality_desc, sizeof(s_cache.air_quality_desc), "%s", aq_desc);
   if (co2_desc[0])
      snprintf(s_cache.co2_quality_desc, sizeof(s_cache.co2_quality_desc), "%s", co2_desc);
   mark_seen_locked();
   pthread_mutex_unlock(&s_mutex);
}

/* --- Motion: orientation --- */
static void ingest_motion(struct json_object *root) {
   double heading = 0, pitch = 0, roll = 0;
   bool has_h = json_get_double(root, "heading", &heading);
   bool has_p = json_get_double(root, "pitch", &pitch);
   bool has_r = json_get_double(root, "roll", &roll);
   time_t now = time(NULL);

   pthread_mutex_lock(&s_mutex);
   s_cache.have_motion = true;
   s_cache.motion_last_seen = now;
   if (has_h)
      s_cache.heading = heading;
   if (has_p)
      s_cache.pitch = pitch;
   if (has_r)
      s_cache.roll = roll;
   mark_seen_locked();
   pthread_mutex_unlock(&s_mutex);
}

/* --- GPS: position --- */
static void ingest_gps(struct json_object *root) {
   int fix = 0, quality = 0, sats = 0;
   json_get_int(root, "fix", &fix);
   json_get_int(root, "quality", &quality);
   json_get_int(root, "satellites", &sats);
   double lat = 0, lon = 0, alt = 0, speed = 0;
   json_get_double(root, "latitude", &lat);
   json_get_double(root, "longitude", &lon);
   json_get_double(root, "altitude", &alt);
   json_get_double(root, "speed", &speed);
   time_t now = time(NULL);

   pthread_mutex_lock(&s_mutex);
   s_cache.have_gps = true;
   s_cache.gps_last_seen = now;
   s_cache.fix = fix;
   s_cache.quality = quality;
   s_cache.satellites = sats;
   s_cache.latitude = lat;
   s_cache.longitude = lon;
   s_cache.altitude = alt;
   s_cache.speed = speed;
   mark_seen_locked();
   pthread_mutex_unlock(&s_mutex);
}

static void ingest_helmet(struct json_object *root) {
   struct json_object *j_type;
   if (!json_object_object_get_ex(root, "type", &j_type)) {
      return;
   }
   const char *type = json_object_get_string(j_type);
   if (!type) {
      return;
   }
   if (strcmp(type, "Enviro") == 0) {
      ingest_enviro(root);
   } else if (strcmp(type, "Motion") == 0) {
      ingest_motion(root);
   } else if (strcmp(type, "GPS") == 0) {
      ingest_gps(root);
   }
}

/* =============================================================================
 * Ingest (armor SPARK)
 * ============================================================================= */

static void ingest_armor(struct json_object *root) {
   char piece[SUIT_PIECE_NAME_LEN] = "";
   json_get_str(root, "piece", piece, sizeof(piece));
   if (!piece[0]) {
      return; /* a piece with no name is not addressable */
   }
   double temp = 0, voltage = 0;
   bool has_temp = json_get_double(root, "temp", &temp);
   bool has_voltage = json_get_double(root, "voltage", &voltage);
   time_t now = time(NULL);

   pthread_mutex_lock(&s_mutex);
   /* Upsert by name. */
   armor_slot_t *slot = NULL;
   for (int i = 0; i < s_cache.armor_count; i++) {
      if (strcmp(s_cache.armor[i].name, piece) == 0) {
         slot = &s_cache.armor[i];
         break;
      }
   }
   if (!slot && s_cache.armor_count < SUIT_MAX_ARMOR_PIECES) {
      slot = &s_cache.armor[s_cache.armor_count++];
      memset(slot, 0, sizeof(*slot));
      snprintf(slot->name, sizeof(slot->name), "%s", piece);
   }
   if (slot) {
      slot->last_seen = now;
      if (has_temp) {
         slot->temp = temp;
         slot->have_temp = true;
      }
      if (has_voltage) {
         slot->voltage = voltage;
         slot->have_voltage = true;
      }
      mark_seen_locked();
   }
   pthread_mutex_unlock(&s_mutex);

   if (!slot) {
      OLOG_WARNING("suit_service: armor roster full (%d) — dropping piece '%s'",
                   SUIT_MAX_ARMOR_PIECES, piece);
   }
}

/* =============================================================================
 * Public API
 * ============================================================================= */

int suit_service_handle_mqtt(const char *topic, const char *payload, int payload_len) {
   if (!s_active || !topic) {
      return 0;
   }
   bool is_helmet = (strcmp(topic, s_cfg.helmet_topic) == 0);
   bool is_armor = (strcmp(topic, s_cfg.armor_topic) == 0);
   if (!is_helmet && !is_armor) {
      return 0; /* not ours — let the normal dispatch + log run */
   }
   if (!payload || payload_len <= 0) {
      return 1; /* ours but empty */
   }

   /* Length-bounded parse — mosquitto payloads aren't guaranteed NUL-terminated. */
   struct json_tokener *tok = json_tokener_new();
   if (!tok) {
      return 1;
   }
   struct json_object *root = json_tokener_parse_ex(tok, payload, payload_len);
   json_tokener_free(tok);
   if (!root) {
      return 1;
   }

   if (is_helmet) {
      ingest_helmet(root);
   } else {
      ingest_armor(root);
   }
   json_object_put(root);
   return 1;
}

/* Compute age/stale for one subsystem timestamp against now (caller passes the
 * copied-out last_seen and have flag). */
static void compute_stale(bool have, time_t last_seen, time_t now, int *age_out, bool *stale_out) {
   if (!have) {
      *age_out = 0;
      *stale_out = true;
      return;
   }
   *age_out = (int)(now - last_seen);
   *stale_out = (*age_out > s_cfg.stale_after_sec);
}

void suit_service_get_snapshot(suit_snapshot_t *out) {
   if (!out) {
      return;
   }
   memset(out, 0, sizeof(*out));
   time_t now = time(NULL);

   pthread_mutex_lock(&s_mutex);
   out->ever_seen = s_cache.ever_seen;

   out->have_enviro = s_cache.have_enviro;
   compute_stale(s_cache.have_enviro, s_cache.enviro_last_seen, now, &out->enviro_age_sec,
                 &out->enviro_stale);
   out->temp = s_cache.temp;
   out->humidity = s_cache.humidity;
   out->air_quality = s_cache.air_quality;
   out->tvoc_ppb = s_cache.tvoc_ppb;
   out->eco2_ppm = s_cache.eco2_ppm;
   out->co2_ppm = s_cache.co2_ppm;
   out->heat_index_c = s_cache.heat_index_c;
   out->dew_point = s_cache.dew_point;
   snprintf(out->air_quality_desc, sizeof(out->air_quality_desc), "%s", s_cache.air_quality_desc);
   snprintf(out->co2_quality_desc, sizeof(out->co2_quality_desc), "%s", s_cache.co2_quality_desc);

   out->have_motion = s_cache.have_motion;
   compute_stale(s_cache.have_motion, s_cache.motion_last_seen, now, &out->motion_age_sec,
                 &out->motion_stale);
   out->heading = s_cache.heading;
   out->pitch = s_cache.pitch;
   out->roll = s_cache.roll;

   out->have_gps = s_cache.have_gps;
   compute_stale(s_cache.have_gps, s_cache.gps_last_seen, now, &out->gps_age_sec, &out->gps_stale);
   out->fix = s_cache.fix;
   out->quality = s_cache.quality;
   out->satellites = s_cache.satellites;
   out->latitude = s_cache.latitude;
   out->longitude = s_cache.longitude;
   out->altitude = s_cache.altitude;
   out->speed = s_cache.speed;

   out->armor_count = s_cache.armor_count;
   for (int i = 0; i < s_cache.armor_count; i++) {
      const armor_slot_t *slot = &s_cache.armor[i];
      suit_armor_piece_t *dst = &out->armor[i];
      snprintf(dst->name, sizeof(dst->name), "%s", slot->name);
      dst->temp = slot->temp;
      dst->voltage = slot->voltage;
      dst->have_temp = slot->have_temp;
      dst->have_voltage = slot->have_voltage;
      compute_stale(true, slot->last_seen, now, &dst->age_sec, &dst->stale);
   }
   pthread_mutex_unlock(&s_mutex);
}

bool suit_service_is_active(void) {
   return s_active;
}

const char *suit_service_helmet_topic(void) {
   return s_cfg.helmet_topic;
}

const char *suit_service_armor_topic(void) {
   return s_cfg.armor_topic;
}

int suit_service_init(const suit_service_cfg_t *cfg) {
   if (!cfg) {
      return FAILURE;
   }
   s_cfg = *cfg;
   if (!s_cfg.helmet_topic[0]) {
      snprintf(s_cfg.helmet_topic, sizeof(s_cfg.helmet_topic), "%s", SUIT_HELMET_TOPIC_DEFAULT);
   }
   if (!s_cfg.armor_topic[0]) {
      snprintf(s_cfg.armor_topic, sizeof(s_cfg.armor_topic), "%s", SUIT_ARMOR_TOPIC_DEFAULT);
   }
   if (s_cfg.stale_after_sec <= 0) {
      s_cfg.stale_after_sec = SUIT_STALE_AFTER_SEC_DEFAULT;
   }

   pthread_mutex_lock(&s_mutex);
   memset(&s_cache, 0, sizeof(s_cache));
   pthread_mutex_unlock(&s_mutex);

   if (!s_cfg.enabled) {
      OLOG_INFO("suit_service: disabled by config");
      s_active = false;
      return SUCCESS;
   }

   s_active = true;
   OLOG_INFO("suit_service: active (helmet='%s', armor='%s')", s_cfg.helmet_topic,
             s_cfg.armor_topic);
   return SUCCESS;
}

void suit_service_shutdown(void) {
   s_active = false;
}
