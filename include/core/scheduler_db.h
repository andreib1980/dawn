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
 * Scheduler Database Layer - CRUD operations for scheduled_events table
 *
 * Provides all SQLite operations for the scheduler. Uses the shared auth_db
 * database handle and prepared statements. All functions are thread-safe
 * via the auth_db mutex.
 */

#ifndef SCHEDULER_DB_H
#define SCHEDULER_DB_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "dawn_error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Constants
 * ============================================================================= */

/* Scheduler DB return codes */
#define SCHED_DB_SUCCESS SUCCESS
#define SCHED_DB_FAILURE FAILURE
#define SCHED_DB_USER_LIMIT 2
#define SCHED_DB_GLOBAL_LIMIT 3

#define SCHED_NAME_MAX 128
#define SCHED_MESSAGE_MAX 512
#define SCHED_UUID_MAX 37
#define SCHED_LOCATION_MAX 32
#define SCHED_TOOL_NAME_MAX 64
#define SCHED_TOOL_VALUE_MAX 2048
#define SCHED_RECURRENCE_DAYS_MAX 32
#define SCHED_ORIGINAL_TIME_MAX 6 /* HH:MM + null */
#define SCHED_MAX_RESULTS 50

/* =============================================================================
 * Enums (C enums, convert at DB boundary)
 * ============================================================================= */

typedef enum {
   SCHED_EVENT_TIMER,
   SCHED_EVENT_ALARM,
   SCHED_EVENT_REMINDER,
   SCHED_EVENT_TASK,
   SCHED_EVENT_BRIEFING,
} sched_event_type_t;

typedef enum {
   SCHED_STATUS_PENDING,
   SCHED_STATUS_RINGING,
   SCHED_STATUS_FIRED,
   SCHED_STATUS_CANCELLED,
   SCHED_STATUS_SNOOZED,
   SCHED_STATUS_MISSED,
   SCHED_STATUS_DISMISSED,
   SCHED_STATUS_TIMED_OUT,
} sched_status_t;

typedef enum {
   SCHED_RECUR_ONCE,
   SCHED_RECUR_DAILY,
   SCHED_RECUR_WEEKDAYS,
   SCHED_RECUR_WEEKENDS,
   SCHED_RECUR_WEEKLY,
   SCHED_RECUR_CUSTOM,
} sched_recurrence_t;

typedef enum {
   SCHED_SOURCE_LOCAL = 0, /**< Local mic — daemon speaker (default/legacy) */
   SCHED_SOURCE_WEBUI = 1, /**< WebUI browser session */
   SCHED_SOURCE_DAP2 = 2,  /**< DAP2 satellite */
} sched_source_type_t;

/* =============================================================================
 * Event Structure
 * ============================================================================= */

typedef struct {
   int64_t id;
   int user_id;
   sched_event_type_t event_type;
   sched_status_t status;
   char name[SCHED_NAME_MAX];
   char message[SCHED_MESSAGE_MAX];
   time_t fire_at;
   time_t created_at;
   int duration_sec;
   time_t snoozed_until;
   sched_recurrence_t recurrence;
   char recurrence_days[SCHED_RECURRENCE_DAYS_MAX];
   char original_time[SCHED_ORIGINAL_TIME_MAX];
   char source_uuid[SCHED_UUID_MAX];
   char source_location[SCHED_LOCATION_MAX];
   sched_source_type_t source_client_type;
   bool announce_all;
   char tool_name[SCHED_TOOL_NAME_MAX];
   char tool_action[SCHED_TOOL_NAME_MAX];
   char tool_value[SCHED_TOOL_VALUE_MAX];
   time_t fired_at;
   int snooze_count;
} sched_event_t;

/* =============================================================================
 * String Conversion Helpers
 * ============================================================================= */

const char *sched_event_type_to_str(sched_event_type_t type);
sched_event_type_t sched_event_type_from_str(const char *str);
const char *sched_status_to_str(sched_status_t status);
sched_status_t sched_status_from_str(const char *str);
const char *sched_recurrence_to_str(sched_recurrence_t recurrence);
sched_recurrence_t sched_recurrence_from_str(const char *str);

const char *sched_source_type_to_str(sched_source_type_t type);
sched_source_type_t sched_source_type_from_str(const char *str);

/* =============================================================================
 * CRUD Operations
 * ============================================================================= */

/**
 * @brief Insert a new scheduled event
 * @param event Event to insert (id and created_at are set by this function)
 * @param id_out Output: event ID on success
 * @return SCHED_DB_SUCCESS or SCHED_DB_FAILURE
 */
int scheduler_db_insert(sched_event_t *event, int64_t *id_out);

/**
 * @brief Atomically check limits and insert event (TOCTOU-safe)
 * @param event Event to insert
 * @param max_per_user Maximum events per user
 * @param max_total Maximum total events
 * @param id_out Output: event ID on success
 * @return SCHED_DB_SUCCESS, SCHED_DB_FAILURE, SCHED_DB_USER_LIMIT, or SCHED_DB_GLOBAL_LIMIT
 */
int scheduler_db_insert_checked(sched_event_t *event,
                                int max_per_user,
                                int max_total,
                                int64_t *id_out);

/**
 * @brief Get event by ID
 * @param id Event ID
 * @param event Output event struct
 * @return SUCCESS or FAILURE if not found
 */
int scheduler_db_get(int64_t id, sched_event_t *event);

/**
 * @brief Update event status
 * @param id Event ID
 * @param status New status
 * @return SUCCESS or FAILURE
 */
int scheduler_db_update_status(int64_t id, sched_status_t status);

/**
 * @brief Update event status with fired_at timestamp
 * @param id Event ID
 * @param status New status
 * @param fired_at Fired timestamp
 * @return SUCCESS or FAILURE
 */
int scheduler_db_update_status_fired(int64_t id, sched_status_t status, time_t fired_at);

/**
 * @brief Update fire_at for snooze (also updates snoozed_until and snooze_count)
 * @param id Event ID
 * @param new_fire_at New fire time
 * @return SUCCESS or FAILURE
 */
int scheduler_db_snooze(int64_t id, time_t new_fire_at);

/**
 * @brief Cancel an event (optimistic: only if still pending/snoozed)
 * @param id Event ID
 * @return SUCCESS if cancelled, FAILURE if already fired/cancelled
 */
int scheduler_db_cancel(int64_t id);

/**
 * @brief Dismiss a ringing event (optimistic: only if status='ringing')
 * @param id Event ID
 * @return SUCCESS if dismissed, FAILURE if already handled
 */
int scheduler_db_dismiss(int64_t id);

/* =============================================================================
 * Query Operations
 * ============================================================================= */

/**
 * @brief Get the next fire_at time for pending events
 * @return Next fire_at timestamp, or 0 if no pending events
 */
time_t scheduler_db_next_fire_time(void);

/**
 * @brief Get all events that should fire now (fire_at <= now, status=pending/snoozed)
 * @param events Output array
 * @param max_count Maximum events to return
 * @return Number of events found
 */
int scheduler_db_get_due_events(sched_event_t *events, int max_count);

/**
 * @brief List events for a user filtered by status and optional type
 * @param user_id User ID
 * @param type Event type filter (-1 for all types)
 * @param events Output array
 * @param max_count Maximum events to return
 * @return Number of events found
 */
int scheduler_db_list_user_events(int user_id, int type, sched_event_t *events, int max_count);

/**
 * @brief Find event by name for a user (case-insensitive)
 * @param user_id User ID
 * @param name Event name
 * @param event Output event struct
 * @return SUCCESS or FAILURE if not found
 */
int scheduler_db_find_by_name(int user_id, const char *name, sched_event_t *event);

/**
 * @brief Count pending events for a user
 * @param user_id User ID
 * @param count_out Output: event count on success
 * @return SCHED_DB_SUCCESS or SCHED_DB_FAILURE
 */
int scheduler_db_count_user_events(int user_id, int *count_out);

/**
 * @brief Count total pending events across all users
 * @param count_out Output: event count on success
 * @return SCHED_DB_SUCCESS or SCHED_DB_FAILURE
 */
int scheduler_db_count_total_events(int *count_out);

/**
 * @brief Get currently ringing events (status='ringing')
 * @param events Output array
 * @param max_count Maximum events to return
 * @return Number of events found
 */
int scheduler_db_get_ringing(sched_event_t *events, int max_count);

/**
 * @brief Get active timers for a specific satellite UUID
 * @param uuid Satellite UUID
 * @param events Output array
 * @param max_count Maximum events
 * @return Number of events found
 */
int scheduler_db_get_active_by_uuid(const char *uuid, sched_event_t *events, int max_count);

/**
 * @brief Clean up old fired/cancelled/missed events
 * @param retention_days Delete events older than this many days
 * @param deleted_out Output: number of events deleted on success
 * @return SCHED_DB_SUCCESS or SCHED_DB_FAILURE
 */
int scheduler_db_cleanup_old_events(int retention_days, int *deleted_out);

/**
 * @brief Get all pending/snoozed events that should have fired (for missed recovery)
 * @param events Output array
 * @param max_count Maximum events to return
 * @return Number of events found
 */
int scheduler_db_get_missed_events(sched_event_t *events, int max_count);

#ifdef __cplusplus
}
#endif

#endif /* SCHEDULER_DB_H */
