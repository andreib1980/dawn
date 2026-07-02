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
 * STAT telemetry service — MQTT ingest, in-memory latest-value cache (TTL
 * staleness), and rollup accumulators flushed to stat_db by the maintenance
 * thread.  See include/tools/stat_service.h.
 */

#include "core/stat_service.h"

#include <json-c/json.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "core/stat_db.h"
#include "dawn_error.h"
#include "logging.h"

/* =============================================================================
 * State
 * ============================================================================= */

static stat_service_cfg_t s_cfg;
/* Set by main-thread init/shutdown, read by the MQTT + maintenance threads.
 * Atomic so the cross-thread reads are well-defined rather than relying on the
 * (currently true) lifecycle invariant that it's only written before those
 * threads start and after they are joined. */
static atomic_bool s_active = false;

/* Guards both the live cache and the rollup accumulators.  Touched by the MQTT
 * thread (ingest), a worker thread (snapshot), and the maintenance thread
 * (flush).  A single leaf lock — critical sections copy out, then process. */
static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Latest live values. */
static struct {
   bool online;
   bool ever_seen;
   time_t last_seen;

   bool have_system;
   double cpu_usage, memory_usage, system_temp;

   bool have_fan;
   int fan_rpm, fan_load, fan_pwm;

   bool have_battery;
   double batt_voltage, batt_current, batt_power, batt_level, batt_temp, time_remaining_min;
   char charging_state[24];
   char battery_status[24];
   char status_reason[96];
   int crit_faults, warn_faults, info_faults;
} s_cache;

/* Rollup accumulators for the current bucket (per family). */
static struct {
   time_t started;
   long sys_n;
   double cpu_sum, cpu_max;
   double mem_sum, mem_max;
   double temp_sum, temp_min, temp_max;
   long batt_n;
   double batt_sum, batt_min, batt_max; /* battery_level (%) */
   double batt_v_sum, batt_p_sum, batt_temp_max;
   long fan_n;
   double fan_sum, fan_max; /* rpm */
} s_accum;

/* =============================================================================
 * Helpers (all callers hold s_mutex unless noted)
 * ============================================================================= */

static void reset_accum_locked(time_t now) {
   memset(&s_accum, 0, sizeof(s_accum));
   s_accum.started = now;
}

/* Mark that fresh telemetry arrived.  Caller holds s_mutex. */
static void mark_seen_locked(time_t now) {
   s_cache.ever_seen = true;
   s_cache.online = true; /* receiving telemetry implies STAT is alive */
   s_cache.last_seen = now;
}

/* Extract a finite double.  Rejects a missing key AND non-finite values
 * (inf/nan) from an untrusted broker publisher, so they never enter the
 * accumulators, the DB, or the double->int cast in the tool's formatter. */
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

/* --- SystemMetrics: cpu_usage, memory_usage, system_temp --- */
static void ingest_system(struct json_object *root) {
   double cpu = 0, mem = 0, temp = 0;
   bool has_cpu = json_get_double(root, "cpu_usage", &cpu);
   bool has_mem = json_get_double(root, "memory_usage", &mem);
   bool has_temp = json_get_double(root, "system_temp", &temp);

   time_t now = time(NULL);

   pthread_mutex_lock(&s_mutex);
   s_cache.have_system = true;
   if (has_cpu)
      s_cache.cpu_usage = cpu;
   if (has_mem)
      s_cache.memory_usage = mem;
   if (has_temp)
      s_cache.system_temp = temp;

   /* Fold only a complete sample — the three share sys_n as their divisor, so a
    * partial/garbage SystemMetrics would skew the bucket average. */
   if (has_cpu && has_mem && has_temp) {
      bool first = (s_accum.sys_n == 0);
      s_accum.cpu_sum += cpu;
      s_accum.mem_sum += mem;
      s_accum.temp_sum += temp;
      s_accum.cpu_max = first ? cpu : (cpu > s_accum.cpu_max ? cpu : s_accum.cpu_max);
      s_accum.mem_max = first ? mem : (mem > s_accum.mem_max ? mem : s_accum.mem_max);
      s_accum.temp_max = first ? temp : (temp > s_accum.temp_max ? temp : s_accum.temp_max);
      s_accum.temp_min = first ? temp : (temp < s_accum.temp_min ? temp : s_accum.temp_min);
      s_accum.sys_n++;
   }
   mark_seen_locked(now);
   pthread_mutex_unlock(&s_mutex);
}

/* --- Fan: rpm, load, pwm --- */
static void ingest_fan(struct json_object *root) {
   int rpm = 0, load = 0, pwm = 0;
   bool has_rpm = json_get_int(root, "rpm", &rpm);
   json_get_int(root, "load", &load);
   json_get_int(root, "pwm", &pwm);
   time_t now = time(NULL);

   pthread_mutex_lock(&s_mutex);
   s_cache.have_fan = true;
   s_cache.fan_rpm = rpm;
   s_cache.fan_load = load;
   s_cache.fan_pwm = pwm;

   if (has_rpm) {
      double r = (double)rpm;
      bool first = (s_accum.fan_n == 0);
      s_accum.fan_sum += r;
      s_accum.fan_max = first ? r : (r > s_accum.fan_max ? r : s_accum.fan_max);
      s_accum.fan_n++;
   }
   mark_seen_locked(now);
   pthread_mutex_unlock(&s_mutex);
}

/* --- Unified BatteryStatus --- */
static void ingest_battery(struct json_object *root) {
   double voltage = 0, current = 0, power = 0, level = 0, temp = 0, remaining = 0;
   json_get_double(root, "voltage", &voltage);
   json_get_double(root, "current", &current);
   json_get_double(root, "power", &power);
   bool has_level = json_get_double(root, "battery_level", &level);
   json_get_double(root, "temperature", &temp);
   json_get_double(root, "time_remaining_min", &remaining);
   int cf = 0, wf = 0, inf = 0;
   json_get_int(root, "critical_fault_count", &cf);
   json_get_int(root, "warning_fault_count", &wf);
   json_get_int(root, "info_fault_count", &inf);
   char charging[24] = "", status[24] = "", reason[96] = "";
   json_get_str(root, "charging_state", charging, sizeof(charging));
   json_get_str(root, "battery_status", status, sizeof(status));
   json_get_str(root, "status_reason", reason, sizeof(reason));
   time_t now = time(NULL);

   pthread_mutex_lock(&s_mutex);
   s_cache.have_battery = true;
   s_cache.batt_voltage = voltage;
   s_cache.batt_current = current;
   s_cache.batt_power = power;
   s_cache.batt_level = level;
   s_cache.batt_temp = temp;
   s_cache.time_remaining_min = remaining;
   s_cache.crit_faults = cf;
   s_cache.warn_faults = wf;
   s_cache.info_faults = inf;
   snprintf(s_cache.charging_state, sizeof(s_cache.charging_state), "%s", charging);
   snprintf(s_cache.battery_status, sizeof(s_cache.battery_status), "%s", status);
   snprintf(s_cache.status_reason, sizeof(s_cache.status_reason), "%s", reason);

   /* battery_level is the divisor-bearing metric; fold the bucket only when it's
    * present so a level-less BatteryStatus can't skew the average. */
   if (has_level) {
      bool first = (s_accum.batt_n == 0);
      s_accum.batt_sum += level;
      s_accum.batt_v_sum += voltage;
      s_accum.batt_p_sum += power;
      s_accum.batt_max = first ? level : (level > s_accum.batt_max ? level : s_accum.batt_max);
      s_accum.batt_min = first ? level : (level < s_accum.batt_min ? level : s_accum.batt_min);
      s_accum.batt_temp_max = first ? temp
                                    : (temp > s_accum.batt_temp_max ? temp : s_accum.batt_temp_max);
      s_accum.batt_n++;
   }
   mark_seen_locked(now);
   pthread_mutex_unlock(&s_mutex);
}

static void ingest_telemetry(struct json_object *root) {
   struct json_object *j_type;
   if (!json_object_object_get_ex(root, "type", &j_type)) {
      return;
   }
   const char *type = json_object_get_string(j_type);
   if (!type) {
      return;
   }

   /* Each ingest_* marks last_seen/online within its own critical section
    * (mark_seen_locked), so there's a single lock acquisition per message. */
   if (strcmp(type, "SystemMetrics") == 0) {
      ingest_system(root);
   } else if (strcmp(type, "Fan") == 0) {
      ingest_fan(root);
   } else if (strcmp(type, "BatteryStatus") == 0) {
      ingest_battery(root);
   }
   /* else Battery(raw)/SystemPower/BatteryHealth — not in the headline set */
}

static void ingest_status(struct json_object *root) {
   char status[16] = "";
   json_get_str(root, "status", status, sizeof(status));
   if (!status[0]) {
      return;
   }
   pthread_mutex_lock(&s_mutex);
   s_cache.online = (strcmp(status, "online") == 0);
   pthread_mutex_unlock(&s_mutex);
}

/* =============================================================================
 * Public API
 * ============================================================================= */

int stat_service_handle_mqtt(const char *topic, const char *payload, int payload_len) {
   if (!s_active || !topic) {
      return 0;
   }
   bool is_tel = (strcmp(topic, s_cfg.telemetry_topic) == 0);
   bool is_stat = (strcmp(topic, s_cfg.status_topic) == 0);
   if (!is_tel && !is_stat) {
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

   if (is_stat) {
      ingest_status(root);
   } else {
      ingest_telemetry(root);
   }
   json_object_put(root);
   return 1;
}

void stat_service_get_snapshot(stat_snapshot_t *out) {
   if (!out) {
      return;
   }
   memset(out, 0, sizeof(*out));

   pthread_mutex_lock(&s_mutex);
   out->stat_online = s_cache.online;
   out->ever_seen = s_cache.ever_seen;
   out->last_seen = s_cache.last_seen;
   out->have_system = s_cache.have_system;
   out->cpu_usage = s_cache.cpu_usage;
   out->memory_usage = s_cache.memory_usage;
   out->system_temp = s_cache.system_temp;
   out->have_fan = s_cache.have_fan;
   out->fan_rpm = s_cache.fan_rpm;
   out->fan_load = s_cache.fan_load;
   out->fan_pwm = s_cache.fan_pwm;
   out->have_battery = s_cache.have_battery;
   out->batt_voltage = s_cache.batt_voltage;
   out->batt_current = s_cache.batt_current;
   out->batt_power = s_cache.batt_power;
   out->batt_level = s_cache.batt_level;
   out->batt_temp = s_cache.batt_temp;
   out->time_remaining_min = s_cache.time_remaining_min;
   out->crit_faults = s_cache.crit_faults;
   out->warn_faults = s_cache.warn_faults;
   out->info_faults = s_cache.info_faults;
   snprintf(out->charging_state, sizeof(out->charging_state), "%s", s_cache.charging_state);
   snprintf(out->battery_status, sizeof(out->battery_status), "%s", s_cache.battery_status);
   snprintf(out->status_reason, sizeof(out->status_reason), "%s", s_cache.status_reason);
   pthread_mutex_unlock(&s_mutex);

   time_t now = time(NULL);
   if (!out->ever_seen) {
      out->stale = true;
      out->age_sec = 0;
   } else {
      out->age_sec = (int)(now - out->last_seen);
      out->stale = (out->age_sec > s_cfg.stale_after_sec);
   }
}

int stat_history_flush(void) {
   if (!s_active) {
      return SUCCESS;
   }

   stat_bucket_row_t row;
   memset(&row, 0, sizeof(row));

   pthread_mutex_lock(&s_mutex);
   long sys_n = s_accum.sys_n, batt_n = s_accum.batt_n, fan_n = s_accum.fan_n;
   if (sys_n == 0 && batt_n == 0 && fan_n == 0) {
      /* Empty bucket (STAT offline) — re-anchor the window, write nothing. */
      s_accum.started = time(NULL);
      pthread_mutex_unlock(&s_mutex);
      return SUCCESS;
   }

   row.bucket_start = (int64_t)s_accum.started;
   row.sys_count = (int)sys_n;
   row.batt_count = (int)batt_n;
   row.fan_count = (int)fan_n;
   if (sys_n > 0) {
      row.have_sys = true;
      row.cpu_avg = s_accum.cpu_sum / (double)sys_n;
      row.cpu_max = s_accum.cpu_max;
      row.mem_avg = s_accum.mem_sum / (double)sys_n;
      row.mem_max = s_accum.mem_max;
      row.temp_avg = s_accum.temp_sum / (double)sys_n;
      row.temp_min = s_accum.temp_min;
      row.temp_max = s_accum.temp_max;
   }
   if (batt_n > 0) {
      row.have_batt = true;
      row.batt_avg = s_accum.batt_sum / (double)batt_n;
      row.batt_min = s_accum.batt_min;
      row.batt_max = s_accum.batt_max;
      row.batt_v_avg = s_accum.batt_v_sum / (double)batt_n;
      row.batt_p_avg = s_accum.batt_p_sum / (double)batt_n;
      row.batt_temp_max = s_accum.batt_temp_max;
   }
   if (fan_n > 0) {
      row.have_fan = true;
      row.fan_avg = s_accum.fan_sum / (double)fan_n;
      row.fan_max = s_accum.fan_max;
   }
   reset_accum_locked(time(NULL));
   pthread_mutex_unlock(&s_mutex);

   /* DB work outside the cache lock (no lock nesting). */
   if (stat_db_insert_bucket(&row) != SUCCESS) {
      OLOG_WARNING("stat_service: history bucket write failed");
      return FAILURE;
   }
   int deleted = 0;
   if (stat_db_prune(s_cfg.history_retention_days, &deleted) == SUCCESS && deleted > 0) {
      OLOG_INFO("stat_service: pruned %d old telemetry bucket(s)", deleted);
   }
   stat_db_checkpoint();
   return SUCCESS;
}

bool stat_service_is_active(void) {
   return s_active;
}

const char *stat_service_telemetry_topic(void) {
   return s_cfg.telemetry_topic;
}

const char *stat_service_status_topic(void) {
   return s_cfg.status_topic;
}

int stat_service_init(const stat_service_cfg_t *cfg) {
   if (!cfg) {
      return FAILURE;
   }
   s_cfg = *cfg;
   if (!s_cfg.telemetry_topic[0]) {
      snprintf(s_cfg.telemetry_topic, sizeof(s_cfg.telemetry_topic), "%s",
               STAT_TELEMETRY_TOPIC_DEFAULT);
   }
   if (!s_cfg.status_topic[0]) {
      snprintf(s_cfg.status_topic, sizeof(s_cfg.status_topic), "%s", STAT_STATUS_TOPIC_DEFAULT);
   }
   if (s_cfg.stale_after_sec <= 0) {
      s_cfg.stale_after_sec = 30;
   }

   memset(&s_cache, 0, sizeof(s_cache));
   reset_accum_locked(time(NULL));

   if (!s_cfg.enabled) {
      OLOG_INFO("stat_service: disabled by config");
      s_active = false;
      return SUCCESS;
   }

   /* History is best-effort: a DB open failure disables history, not the tool. */
   if (stat_db_init(s_cfg.db_path) != SUCCESS) {
      OLOG_WARNING("stat_service: history DB unavailable — live telemetry only");
   }

   s_active = true;
   OLOG_INFO("stat_service: active (telemetry='%s', status='%s')", s_cfg.telemetry_topic,
             s_cfg.status_topic);
   return SUCCESS;
}

void stat_service_shutdown(void) {
   if (s_active) {
      stat_history_flush(); /* persist the final partial bucket before closing */
   }
   stat_db_shutdown();
   s_active = false;
}
