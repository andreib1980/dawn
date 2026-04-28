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
 * Scheduler Tool - LLM tool for creating/managing timers, alarms, reminders
 *
 * Actions: create, list, cancel, query, snooze, dismiss
 * The "details" parameter is a JSON string with action-specific fields.
 */

#include "tools/scheduler_tool.h"

#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "config/dawn_config.h"
#include "core/scheduler.h"
#include "core/scheduler_db.h"
#include "core/session_manager.h"
#include "logging.h"
#include "tools/tool_registry.h"

/* =============================================================================
 * Constants
 * ============================================================================= */

#define RESULT_BUF_SIZE 2048
#define MAX_DURATION_MINUTES 43200 /* 30 days */
#define MAX_SNOOZE_MINUTES 120

/* =============================================================================
 * Forward Declarations
 * ============================================================================= */

static char *scheduler_tool_callback(const char *action, char *value, int *should_respond);
static int scheduler_tool_init(void);
static void scheduler_tool_cleanup(void);

/* =============================================================================
 * ISO 8601 Parser
 * ============================================================================= */

/**
 * @brief Parse timezone offset from ISO 8601 suffix
 *
 * Handles 'Z' (UTC), '+HH:MM', '-HH:MM' suffixes.
 *
 * @param suffix Pointer to the timezone part of the string (after seconds)
 * @param offset_sec Output: offset from UTC in seconds (e.g., -18000 for -05:00)
 * @return true if a timezone suffix was found and parsed, false if local time
 */
static bool parse_tz_offset(const char *suffix, int *offset_sec) {
   if (!suffix || !suffix[0])
      return false;

   if (suffix[0] == 'Z' || suffix[0] == 'z') {
      *offset_sec = 0;
      return true;
   }

   if (suffix[0] == '+' || suffix[0] == '-') {
      int tz_h = 0, tz_m = 0;
      if (sscanf(suffix + 1, "%d:%d", &tz_h, &tz_m) >= 1) {
         *offset_sec = (tz_h * 3600 + tz_m * 60);
         if (suffix[0] == '-')
            *offset_sec = -*offset_sec;
         return true;
      }
   }

   return false;
}

/**
 * @brief Parse ISO 8601 datetime string to Unix timestamp (thread-safe)
 *
 * Supports formats:
 * - "2026-02-19T15:30:00"        (local time, uses process-wide TZ)
 * - "2026-02-19T15:30:00Z"       (UTC)
 * - "2026-02-19T15:30:00-05:00"  (with timezone offset)
 * - "15:30" or "07:00"           (time only, assume today or tomorrow)
 *
 * Thread-safe: relies on process-wide TZ set once at startup from
 * g_config.localization.timezone. Explicit timezone offsets are parsed
 * arithmetically.
 *
 * @param iso_str Input string
 * @return Unix timestamp, or -1 on error
 */
static time_t parse_iso8601(const char *iso_str) {
   if (!iso_str || !iso_str[0])
      return -1;

   struct tm tm_info;
   memset(&tm_info, 0, sizeof(tm_info));
   tm_info.tm_isdst = -1; /* Let mktime determine DST */

   /* Check for time-only format (HH:MM) */
   if (strlen(iso_str) <= 5 && strchr(iso_str, ':')) {
      int hour = 0, min = 0;
      if (sscanf(iso_str, "%d:%d", &hour, &min) != 2)
         return -1;
      if (hour < 0 || hour > 23 || min < 0 || min > 59)
         return -1;

      time_t now = time(NULL);
      localtime_r(&now, &tm_info);
      tm_info.tm_hour = hour;
      tm_info.tm_min = min;
      tm_info.tm_sec = 0;

      time_t result = mktime(&tm_info);
      /* If time already passed today, use tomorrow */
      if (result <= now)
         result += 86400;
      return result;
   }

   /* Full ISO 8601 */
   int year, month, day, hour = 0, min = 0, sec = 0;
   int parsed = sscanf(iso_str, "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &min, &sec);
   if (parsed < 3)
      return -1;

   tm_info.tm_year = year - 1900;
   tm_info.tm_mon = month - 1;
   tm_info.tm_mday = day;
   tm_info.tm_hour = hour;
   tm_info.tm_min = min;
   tm_info.tm_sec = sec;

   /* Check for timezone suffix after the time portion */
   const char *tz_start = iso_str;
   /* Skip past the parsed datetime to find timezone suffix */
   const char *t_pos = strchr(iso_str, 'T');
   if (t_pos) {
      tz_start = t_pos + 1;
      /* Skip past HH:MM:SS digits */
      while (*tz_start && (*tz_start == ':' || (*tz_start >= '0' && *tz_start <= '9')))
         tz_start++;
   } else {
      tz_start = iso_str + strlen(iso_str); /* No T, no timezone */
   }

   int tz_offset_sec = 0;
   if (parse_tz_offset(tz_start, &tz_offset_sec)) {
      /* Timezone-aware: interpret as UTC + offset, convert to local */
      /* First compute as if local time, then adjust */
      time_t local_result = mktime(&tm_info);
      if (local_result == (time_t)-1)
         return -1;

      /* Get the local timezone offset at this time */
      struct tm local_tm;
      localtime_r(&local_result, &local_tm);
      long local_offset = local_tm.tm_gmtoff; /* seconds east of UTC */

      /* The input time is at tz_offset_sec from UTC.
       * Convert: utc_time = input_time - tz_offset_sec
       *          local_time = utc_time + local_offset
       *          result = mktime_result + (local_offset - tz_offset_sec)
       * But mktime already assumed local, so adjust the difference. */
      return local_result + (local_offset - tz_offset_sec);
   }

   /* No timezone suffix: interpret as local time (uses process TZ) */
   return mktime(&tm_info);
}

/* =============================================================================
 * JSON Helpers
 * ============================================================================= */

static const char *json_get_string(struct json_object *obj, const char *key) {
   struct json_object *val = NULL;
   if (!json_object_object_get_ex(obj, key, &val))
      return NULL;
   return json_object_get_string(val);
}

static int json_get_int(struct json_object *obj, const char *key, int default_val) {
   struct json_object *val = NULL;
   if (!json_object_object_get_ex(obj, key, &val))
      return default_val;
   return json_object_get_int(val);
}

static bool json_get_bool(struct json_object *obj, const char *key, bool default_val) {
   struct json_object *val = NULL;
   if (!json_object_object_get_ex(obj, key, &val))
      return default_val;
   return json_object_get_boolean(val);
}

/* =============================================================================
 * Action Handlers
 * ============================================================================= */

static char *handle_create(struct json_object *details,
                           int user_id,
                           const char *source_uuid,
                           const char *source_location,
                           sched_source_type_t source_client_type) {
   char result[RESULT_BUF_SIZE];

   const char *type_str = json_get_string(details, "type");
   if (!type_str) {
      snprintf(result, sizeof(result),
               "Error: 'type' is required (timer, alarm, reminder, task, briefing)");
      return strdup(result);
   }

   sched_event_type_t type = sched_event_type_from_str(type_str);

   /* Build event (limits checked atomically during insert) */
   sched_event_t event;
   memset(&event, 0, sizeof(event));
   event.user_id = user_id;
   event.event_type = type;
   event.status = SCHED_STATUS_PENDING;
   event.recurrence = SCHED_RECUR_ONCE;

   /* Name */
   const char *name = json_get_string(details, "name");
   if (name) {
      strncpy(event.name, name, SCHED_NAME_MAX - 1);
   } else {
      /* Auto-generate name */
      snprintf(event.name, SCHED_NAME_MAX, "%s", type_str);
   }

   /* Message (for reminders) */
   const char *message = json_get_string(details, "message");
   if (message)
      strncpy(event.message, message, SCHED_MESSAGE_MAX - 1);

   /* Fire time */
   int duration_min = json_get_int(details, "duration_minutes", 0);
   const char *fire_at_str = json_get_string(details, "fire_at");

   /* Validate duration_minutes range (shared by all types) */
   if (duration_min > MAX_DURATION_MINUTES) {
      snprintf(result, sizeof(result), "Error: duration cannot exceed %d minutes (30 days)",
               MAX_DURATION_MINUTES);
      return strdup(result);
   }

   if (duration_min > 0) {
      /* Any type can use duration_minutes as a relative offset */
      event.fire_at = time(NULL) + (time_t)duration_min * 60;
      event.duration_sec = duration_min * 60;
   } else if (fire_at_str) {
      /* Absolute time via ISO 8601 */
      time_t fire_time = parse_iso8601(fire_at_str);
      if (fire_time <= 0) {
         snprintf(result, sizeof(result), "Error: invalid fire_at format '%s'", fire_at_str);
         return strdup(result);
      }

      /* Must be in the future */
      if (fire_time <= time(NULL)) {
         snprintf(result, sizeof(result), "Error: fire_at must be in the future");
         return strdup(result);
      }

      /* Must be within 1 year */
      if (fire_time > time(NULL) + 365 * 86400) {
         snprintf(result, sizeof(result), "Error: fire_at must be within 1 year");
         return strdup(result);
      }

      event.fire_at = fire_time;

      /* Store original time for recurring alarms */
      const char *time_only = strchr(fire_at_str, 'T');
      if (time_only) {
         time_only++; /* Skip 'T' */
         strncpy(event.original_time, time_only, SCHED_ORIGINAL_TIME_MAX - 1);
      } else if (strlen(fire_at_str) <= 5) {
         strncpy(event.original_time, fire_at_str, SCHED_ORIGINAL_TIME_MAX - 1);
      }
   } else {
      /* Neither provided */
      if (type == SCHED_EVENT_TIMER) {
         snprintf(result, sizeof(result), "Error: 'duration_minutes' is required for timers");
      } else {
         snprintf(result, sizeof(result),
                  "Error: 'fire_at' (ISO 8601) or 'duration_minutes' is required for %s", type_str);
      }
      return strdup(result);
   }

   /* Recurrence */
   const char *recur = json_get_string(details, "recurrence");
   if (recur)
      event.recurrence = sched_recurrence_from_str(recur);

   const char *recur_days = json_get_string(details, "recurrence_days");
   if (recur_days) {
      /* Validate CSV of day names */
      static const char *valid_days[] = { "sun", "mon", "tue", "wed", "thu", "fri", "sat" };
      char buf[SCHED_RECURRENCE_DAYS_MAX];
      strncpy(buf, recur_days, sizeof(buf) - 1);
      buf[sizeof(buf) - 1] = '\0';

      bool valid = true;
      char *saveptr = NULL;
      char *tok = strtok_r(buf, ",", &saveptr);
      int day_count = 0;
      uint8_t seen = 0; /* bitmask for duplicate detection */
      while (tok && valid) {
         /* Trim whitespace */
         while (*tok == ' ')
            tok++;
         bool found = false;
         for (int d = 0; d < 7; d++) {
            if (strcasecmp(tok, valid_days[d]) == 0) {
               if (seen & (1 << d)) {
                  valid = false; /* duplicate */
               } else {
                  seen |= (1 << d);
                  found = true;
                  day_count++;
               }
               break;
            }
         }
         if (!found)
            valid = false;
         tok = strtok_r(NULL, ",", &saveptr);
      }
      if (!valid || day_count == 0) {
         snprintf(result, sizeof(result),
                  "Error: invalid recurrence_days '%s'. Use CSV of: sun,mon,tue,wed,thu,fri,sat",
                  recur_days);
         return strdup(result);
      }
      strncpy(event.recurrence_days, recur_days, SCHED_RECURRENCE_DAYS_MAX - 1);
   }

   /* Source info */
   if (source_uuid)
      strncpy(event.source_uuid, source_uuid, SCHED_UUID_MAX - 1);
   if (source_location)
      strncpy(event.source_location, source_location, SCHED_LOCATION_MAX - 1);
   event.source_client_type = source_client_type;

   /* Announce all */
   event.announce_all = json_get_bool(details, "announce_all", false);

   /* Tool scheduling (Phase 5) */
   const char *tool_name = json_get_string(details, "tool_name");
   if ((type == SCHED_EVENT_TASK || type == SCHED_EVENT_BRIEFING) && !tool_name) {
      snprintf(result, sizeof(result),
               "Error: 'tool_name' is required for scheduled %s. "
               "System shutdown is not available as a schedulable tool.",
               type == SCHED_EVENT_BRIEFING ? "briefings" : "tasks");
      return strdup(result);
   }
   if (tool_name) {
      /* Validate tool exists and is schedulable */
      const tool_metadata_t *meta = tool_registry_find(tool_name);
      if (!meta) {
         snprintf(result, sizeof(result), "Error: unknown tool '%s'", tool_name);
         return strdup(result);
      }
      if (!(meta->capabilities & TOOL_CAP_SCHEDULABLE)) {
         snprintf(result, sizeof(result), "Error: tool '%s' is not schedulable", tool_name);
         return strdup(result);
      }
      strncpy(event.tool_name, tool_name, SCHED_TOOL_NAME_MAX - 1);
   }
   const char *tool_action = json_get_string(details, "tool_action");
   if (tool_action)
      strncpy(event.tool_action, tool_action, SCHED_TOOL_NAME_MAX - 1);
   const char *tool_value = json_get_string(details, "tool_value");
   if (tool_value) {
      if (strlen(tool_value) >= SCHED_TOOL_VALUE_MAX) {
         snprintf(result, sizeof(result),
                  "Error: tool_value too long (%zu bytes, max %d). "
                  "Shorten the content and retry.",
                  strlen(tool_value), SCHED_TOOL_VALUE_MAX - 1);
         return strdup(result);
      }
      strncpy(event.tool_value, tool_value, SCHED_TOOL_VALUE_MAX - 1);
   }

   /* Atomic limit check + insert */
   int64_t id = 0;
   int insert_rc = scheduler_db_insert_checked(&event, g_config.scheduler.max_events_per_user,
                                               g_config.scheduler.max_events_total, &id);
   if (insert_rc == SCHED_DB_USER_LIMIT) {
      snprintf(result, sizeof(result),
               "Error: maximum events per user reached (%d). Cancel some events first.",
               g_config.scheduler.max_events_per_user);
      return strdup(result);
   }
   if (insert_rc == SCHED_DB_GLOBAL_LIMIT) {
      snprintf(result, sizeof(result), "Error: maximum total events reached (%d).",
               g_config.scheduler.max_events_total);
      return strdup(result);
   }
   if (insert_rc != SCHED_DB_SUCCESS) {
      snprintf(result, sizeof(result), "Error: failed to create event");
      return strdup(result);
   }

   /* Notify scheduler thread */
   scheduler_notify_new_event();

   /* Format response with current time + fire time so the LLM can relay accurately */
   time_t now = time(NULL);
   struct tm now_tm, fire_tm;
   localtime_r(&now, &now_tm);
   localtime_r(&event.fire_at, &fire_tm);

   char now_str[64], fire_str[64];
   strftime(now_str, sizeof(now_str), "%I:%M %p", &now_tm);
   strftime(fire_str, sizeof(fire_str), "%I:%M %p on %b %d", &fire_tm);

   if (type == SCHED_EVENT_TIMER) {
      int hours = duration_min / 60;
      int mins = duration_min % 60;
      char dur_str[64];
      if (hours > 0 && mins > 0)
         snprintf(dur_str, sizeof(dur_str), "%d hour%s and %d minute%s", hours,
                  hours == 1 ? "" : "s", mins, mins == 1 ? "" : "s");
      else if (hours > 0)
         snprintf(dur_str, sizeof(dur_str), "%d hour%s", hours, hours == 1 ? "" : "s");
      else
         snprintf(dur_str, sizeof(dur_str), "%d minute%s", mins, mins == 1 ? "" : "s");
      snprintf(result, sizeof(result), "%s timer set for %s (fires at %s). Current time: %s.",
               event.name, dur_str, fire_str, now_str);
   } else {
      snprintf(result, sizeof(result), "%s '%s' set for %s. Current time: %s.", type_str,
               event.name, fire_str, now_str);
   }

   return strdup(result);
}

static char *handle_list(struct json_object *details, int user_id) {
   const char *type_str = json_get_string(details, "type");
   int type_filter = type_str ? (int)sched_event_type_from_str(type_str) : -1;

   sched_event_t events[SCHED_MAX_RESULTS];
   int count = scheduler_db_list_user_events(user_id, type_filter, events, SCHED_MAX_RESULTS);

   if (count == 0) {
      if (type_str)
         return strdup("No active events of that type.");
      return strdup("No active timers, alarms, or reminders.");
   }

   /* Build response */
   char result[RESULT_BUF_SIZE];
   int pos = 0;
   pos += snprintf(result + pos, sizeof(result) - (size_t)pos, "Active events (%d):\n", count);

   for (int i = 0; i < count && pos < (int)sizeof(result) - 100; i++) {
      sched_event_t *e = &events[i];
      const char *type = sched_event_type_to_str(e->event_type);

      if (e->event_type == SCHED_EVENT_TIMER) {
         /* Show time remaining */
         int remaining = (int)(e->fire_at - time(NULL));
         if (remaining < 0)
            remaining = 0;
         int rm = remaining / 60;
         int rs = remaining % 60;
         pos += snprintf(result + pos, sizeof(result) - (size_t)pos,
                         "- [%s] %s: %dm %ds remaining\n", type, e->name, rm, rs);
      } else {
         struct tm fire_tm;
         localtime_r(&e->fire_at, &fire_tm);
         char time_str[32];
         strftime(time_str, sizeof(time_str), "%I:%M %p %b %d", &fire_tm);
         pos += snprintf(result + pos, sizeof(result) - (size_t)pos, "- [%s] %s: %s", type, e->name,
                         time_str);
         if (e->recurrence != SCHED_RECUR_ONCE) {
            pos += snprintf(result + pos, sizeof(result) - (size_t)pos, " (%s)",
                            sched_recurrence_to_str(e->recurrence));
         }
         pos += snprintf(result + pos, sizeof(result) - (size_t)pos, "\n");
      }
   }

   return strdup(result);
}

static char *handle_cancel(struct json_object *details, int user_id) {
   char result[RESULT_BUF_SIZE];

   /* Try by event_id first */
   int64_t event_id = (int64_t)json_get_int(details, "event_id", 0);
   const char *name = json_get_string(details, "name");

   sched_event_t event;

   if (event_id > 0) {
      if (scheduler_db_get(event_id, &event) != 0) {
         snprintf(result, sizeof(result), "Error: event not found");
         return strdup(result);
      }
      if (event.user_id != user_id) {
         snprintf(result, sizeof(result), "Error: event not found");
         return strdup(result);
      }
   } else if (name) {
      if (scheduler_db_find_by_name(user_id, name, &event) != 0) {
         snprintf(result, sizeof(result), "No active event named '%s' found.", name);
         return strdup(result);
      }
      event_id = event.id;
   } else {
      snprintf(result, sizeof(result), "Error: 'event_id' or 'name' required to cancel");
      return strdup(result);
   }

   if (scheduler_db_cancel(event_id) == 0) {
      snprintf(result, sizeof(result), "Cancelled %s '%s'.",
               sched_event_type_to_str(event.event_type), event.name);
   } else {
      snprintf(result, sizeof(result), "Could not cancel '%s' (may have already fired).",
               event.name);
   }

   return strdup(result);
}

static char *handle_query(struct json_object *details, int user_id) {
   char result[RESULT_BUF_SIZE];

   const char *name = json_get_string(details, "name");
   int64_t event_id = (int64_t)json_get_int(details, "event_id", 0);

   sched_event_t event;

   if (event_id > 0) {
      if (scheduler_db_get(event_id, &event) != 0 || event.user_id != user_id) {
         snprintf(result, sizeof(result), "Event not found.");
         return strdup(result);
      }
   } else if (name) {
      if (scheduler_db_find_by_name(user_id, name, &event) != 0) {
         snprintf(result, sizeof(result), "No active event named '%s' found.", name);
         return strdup(result);
      }
   } else {
      snprintf(result, sizeof(result), "Error: 'event_id' or 'name' required to query");
      return strdup(result);
   }

   if (event.event_type == SCHED_EVENT_TIMER) {
      int remaining = (int)(event.fire_at - time(NULL));
      if (remaining < 0)
         remaining = 0;
      int rh = remaining / 3600;
      int rm = (remaining % 3600) / 60;
      int rs = remaining % 60;

      if (rh > 0) {
         snprintf(result, sizeof(result), "%s has %d hour%s, %d minute%s, and %d second%s left.",
                  event.name, rh, rh == 1 ? "" : "s", rm, rm == 1 ? "" : "s", rs,
                  rs == 1 ? "" : "s");
      } else if (rm > 0) {
         snprintf(result, sizeof(result), "%s has %d minute%s and %d second%s left.", event.name,
                  rm, rm == 1 ? "" : "s", rs, rs == 1 ? "" : "s");
      } else {
         snprintf(result, sizeof(result), "%s has %d second%s left.", event.name, rs,
                  rs == 1 ? "" : "s");
      }
   } else {
      struct tm fire_tm;
      localtime_r(&event.fire_at, &fire_tm);
      char time_str[32];
      strftime(time_str, sizeof(time_str), "%I:%M %p on %b %d", &fire_tm);
      snprintf(result, sizeof(result), "%s '%s' is set for %s. Status: %s.",
               sched_event_type_to_str(event.event_type), event.name, time_str,
               sched_status_to_str(event.status));
   }

   return strdup(result);
}

static char *handle_snooze(struct json_object *details) {
   int64_t event_id = (int64_t)json_get_int(details, "event_id", 0);
   int snooze_min = json_get_int(details, "snooze_minutes", 0);

   if (snooze_min < 0 || snooze_min > MAX_SNOOZE_MINUTES)
      snooze_min = 0;

   int result = scheduler_snooze(event_id, snooze_min);
   if (result == 0) {
      int actual_min = snooze_min > 0 ? snooze_min : g_config.scheduler.default_snooze_minutes;
      char buf[128];
      snprintf(buf, sizeof(buf), "Snoozed for %d minute%s.", actual_min,
               actual_min == 1 ? "" : "s");
      return strdup(buf);
   }

   return strdup("No alarm is currently ringing to snooze.");
}

static char *handle_dismiss(struct json_object *details) {
   int64_t event_id = (int64_t)json_get_int(details, "event_id", 0);

   int result = scheduler_dismiss(event_id);
   if (result == 0)
      return strdup("Alarm dismissed.");

   return strdup("No alarm is currently ringing to dismiss.");
}

/* =============================================================================
 * Tool Callback
 * ============================================================================= */

static char *scheduler_tool_callback(const char *action, char *value, int *should_respond) {
   *should_respond = 1;

   if (!action || !action[0])
      return strdup("Error: action is required");

   /* Parse details JSON */
   struct json_object *details = NULL;
   if (value && value[0]) {
      details = json_tokener_parse(value);
      if (!details) {
         return strdup("Error: invalid JSON in details parameter");
      }
   } else {
      details = json_object_new_object();
   }

   /* Get user context */
   int user_id = 1; /* Default */
   const char *source_uuid = NULL;
   const char *source_location = NULL;
   sched_source_type_t source_client_type = SCHED_SOURCE_LOCAL;

#ifdef ENABLE_MULTI_CLIENT
   session_t *ctx = session_get_command_context();
   if (ctx) {
      user_id = ctx->metrics.user_id > 0 ? ctx->metrics.user_id : 1;
      if (ctx->type == SESSION_TYPE_DAP2) {
         source_uuid = ctx->identity.uuid;
         source_location = ctx->identity.location;
         source_client_type = SCHED_SOURCE_DAP2;
      } else if (ctx->type == SESSION_TYPE_WEBUI) {
         source_client_type = SCHED_SOURCE_WEBUI;
      }
   }
#endif

   char *result = NULL;

   if (strcmp(action, "create") == 0) {
      result = handle_create(details, user_id, source_uuid, source_location, source_client_type);
   } else if (strcmp(action, "list") == 0) {
      result = handle_list(details, user_id);
   } else if (strcmp(action, "cancel") == 0) {
      result = handle_cancel(details, user_id);
   } else if (strcmp(action, "query") == 0) {
      result = handle_query(details, user_id);
   } else if (strcmp(action, "snooze") == 0) {
      result = handle_snooze(details);
   } else if (strcmp(action, "dismiss") == 0) {
      result = handle_dismiss(details);
   } else {
      char buf[256];
      snprintf(buf, sizeof(buf),
               "Error: unknown action '%s'. Valid: create, list, cancel, "
               "query, snooze, dismiss",
               action);
      result = strdup(buf);
   }

   json_object_put(details);
   return result;
}

/* =============================================================================
 * Tool Lifecycle
 * ============================================================================= */

static int scheduler_tool_init(void) {
   return scheduler_init();
}

static void scheduler_tool_cleanup(void) {
   scheduler_shutdown();
}

/* =============================================================================
 * Tool Parameter Definition
 * ============================================================================= */

static const treg_param_t scheduler_params[] = {
   {
       .name = "action",
       .description = "The scheduler action: 'create' (new event), 'list' (show active events), "
                      "'cancel' (cancel by name/id), 'query' (check status/time remaining), "
                      "'snooze' (snooze ringing alarm), 'dismiss' (dismiss ringing alarm)",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = true,
       .maps_to = TOOL_MAPS_TO_ACTION,
       .enum_values = { "create", "list", "cancel", "query", "snooze", "dismiss" },
       .enum_count = 6,
   },
   {
       .name = "details",
       .description =
           "JSON object with action-specific fields. "
           "For 'create': {type (timer|alarm|reminder|briefing), name (optional), "
           "fire_at (ISO 8601 absolute time — PREFERRED for alarms/reminders at specific times, "
           "e.g. '2026-03-19T07:00:00'), "
           "duration_minutes (1-43200, relative offset from now — use for timers or "
           "'in X minutes' requests only), "
           "message (for reminders, max 512 chars), recurrence (once|daily|weekdays|weekends|"
           "weekly|custom), recurrence_days (csv: mon,tue,...), announce_all (bool)}. "
           "Type 'task' is ONLY for scheduling execution of other registered tools and "
           "requires tool_name (must be a valid registered tool), tool_action, tool_value. "
           "Type 'briefing' is like 'task' but summarizes the tool output via LLM, speaks the "
           "summary, and creates a conversation the user can continue (e.g., weather briefing). "
           "Do NOT use type 'task' for arbitrary system operations like shutdown or reboot. "
           "For 'list': {type (optional filter: timer|alarm|reminder|task|briefing)}. "
           "For 'cancel'/'query': {name or event_id}. "
           "For 'snooze': {event_id (optional), snooze_minutes (1-120, optional)}. "
           "For 'dismiss': {event_id (optional)}.",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_VALUE,
   },
};

/* =============================================================================
 * Tool Metadata
 * ============================================================================= */

static const tool_metadata_t scheduler_metadata = {
   .name = "scheduler",
   .device_string = "scheduler",
   .topic = "dawn",
   .aliases = { "timer", "alarm", "reminder", "schedule" },
   .alias_count = 4,

   .description = "Manage timers, alarms, reminders, scheduled tasks, and briefings. "
                  "Set timers with duration ('set a 10 minute timer'), "
                  "alarms at specific times ('set an alarm for 7 AM'), "
                  "reminders with messages ('remind me to call Mom at 3pm'), "
                  "schedule tool execution ('turn off lights at midnight'), "
                  "or briefings that summarize tool output via LLM ('weather briefing at 7am'). "
                  "Query time remaining, list active events, cancel, snooze, or dismiss.",
   .params = scheduler_params,
   .param_count = 2,

   .device_type = TOOL_DEVICE_TYPE_TRIGGER,
   .capabilities = TOOL_CAP_NONE,
   .is_getter = false,
   .default_local = true,
   .default_remote = true,

   .init = scheduler_tool_init,
   .cleanup = scheduler_tool_cleanup,
   .callback = scheduler_tool_callback,
};

/* =============================================================================
 * Registration
 * ============================================================================= */

int scheduler_tool_register(void) {
   return tool_registry_register(&scheduler_metadata);
}
