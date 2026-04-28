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
 * Scheduler - Background thread for timers, alarms, reminders, and tasks
 *
 * Uses pthread_cond_timedwait with CLOCK_MONOTONIC for efficient scheduling.
 * Wakes only when the next event is due or when notified of new events.
 */

#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stdbool.h>
#include <stdint.h>

#include "core/scheduler_db.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Lifecycle
 * ============================================================================= */

/**
 * @brief Initialize the scheduler subsystem
 *
 * Creates the condvar with CLOCK_MONOTONIC, generates alarm chime PCM,
 * and starts the background scheduler thread.
 *
 * @return 0 on success, non-zero on error
 */
int scheduler_init(void);

/**
 * @brief Shut down the scheduler
 *
 * Signals the scheduler thread to stop and waits for it to join.
 * Frees alarm sound buffers.
 */
void scheduler_shutdown(void);

/* =============================================================================
 * Event Notification
 * ============================================================================= */

/**
 * @brief Notify the scheduler that a new event was created
 *
 * Wakes the scheduler thread so it can recalculate its next wake time.
 * Call this after inserting a new event into the database.
 */
void scheduler_notify_new_event(void);

/* =============================================================================
 * Ringing State Queries
 * ============================================================================= */

/**
 * @brief Check if any alarm is currently ringing
 * @return true if an alarm is actively ringing
 */
bool scheduler_is_ringing(void);

/**
 * @brief Get the currently ringing event (if any)
 * @param event Output event struct (filled if ringing)
 * @return SUCCESS if ringing event found, FAILURE if nothing ringing
 */
int scheduler_get_ringing(sched_event_t *event);

/**
 * @brief Dismiss the currently ringing alarm
 * @param event_id Event ID to dismiss (0 = dismiss whatever is ringing)
 * @return SUCCESS or FAILURE if nothing to dismiss
 */
int scheduler_dismiss(int64_t event_id);

/**
 * @brief Snooze the currently ringing alarm
 * @param event_id Event ID to snooze (0 = snooze whatever is ringing)
 * @param snooze_minutes Minutes to snooze (0 = use default from config)
 * @return SUCCESS or FAILURE if nothing to snooze
 */
int scheduler_snooze(int64_t event_id, int snooze_minutes);

/* =============================================================================
 * Alarm Sound
 * ============================================================================= */

/**
 * @brief Stop the currently playing alarm sound
 *
 * Called from dismiss/snooze handlers to immediately stop the sound.
 */
void scheduler_stop_alarm_sound(void);

#ifdef __cplusplus
}
#endif

#endif /* SCHEDULER_H */
