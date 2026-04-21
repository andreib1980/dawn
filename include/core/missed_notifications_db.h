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
 * Missed Notifications DB Layer - persists scheduler notifications that
 * could not be delivered (no connected clients for the target user) and
 * replays them when the user reconnects.
 */

#ifndef MISSED_NOTIFICATIONS_DB_H
#define MISSED_NOTIFICATIONS_DB_H

#include <stdint.h>
#include <time.h>

#include "core/scheduler_db.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Retention period: missed notifications older than this are auto-expired. */
#define MISSED_NOTIF_EXPIRE_SEC (24 * 60 * 60)

/* Maximum queued missed notifications per user (prevents unbounded growth). */
#define MISSED_NOTIF_MAX_PER_USER 100

/* Maximum missed notifications delivered per reconnect (UI-facing batch size).
 * The rest remain in the queue and are delivered on the next reconnect. */
#define MISSED_NOTIF_DELIVERY_BATCH 32

typedef struct {
   int64_t id;
   int user_id;
   int64_t event_id;
   char event_type[16];
   char status[16];
   char name[SCHED_NAME_MAX];
   char message[SCHED_MESSAGE_MAX];
   time_t fire_at;
   int64_t conversation_id;
   time_t created_at;
} missed_notif_t;

/**
 * @brief Insert a missed notification for a user.
 *
 * Enforces a per-user cap (MISSED_NOTIF_MAX_PER_USER) — if the user already
 * has that many queued, the insert is skipped and a warning is logged.
 *
 * @param user_id          Target user (must be > 0)
 * @param event_id         Original scheduler event ID
 * @param event_type       String form (from sched_event_type_to_str)
 * @param status           String form (from sched_status_to_str)
 * @param name             Event name
 * @param message          Notification text
 * @param fire_at          Original fire time
 * @param conversation_id  Briefing conversation ID (0 for non-briefings)
 * @return SUCCESS on insert, FAILURE on error or cap exceeded
 */
int missed_notif_insert(int user_id,
                        int64_t event_id,
                        const char *event_type,
                        const char *status,
                        const char *name,
                        const char *message,
                        time_t fire_at,
                        int64_t conversation_id);

/**
 * @brief Load missed notifications for a user (oldest first).
 *
 * @param user_id    Target user
 * @param max_count  Maximum rows to return (also used as SQL LIMIT)
 * @param out        Output buffer (at least max_count entries)
 * @return Number of rows filled, or -1 on error
 */
int missed_notif_get_for_user(int user_id, int max_count, missed_notif_t *out);

/**
 * @brief Delete a single missed notification, enforcing user ownership.
 *
 * The DELETE includes `AND user_id = ?` so users cannot remove rows that
 * belong to other users even if they guess the ID.
 *
 * @param id       Missed notification ID
 * @param user_id  Authenticated user ID
 * @return SUCCESS on delete (including no-op if not found / not owner), FAILURE on DB error
 */
int missed_notif_delete_by_user(int64_t id, int user_id);

/**
 * @brief Delete all missed notifications for a user.
 *
 * @param user_id  User whose queue to clear
 * @return Number of rows deleted, or -1 on error
 */
int missed_notif_delete_all_for_user(int user_id);

/**
 * @brief Delete missed notifications older than max_age_sec.
 *
 * Deletes in batches of MISSED_NOTIF_EXPIRE_BATCH to avoid long DB locks.
 *
 * @param max_age_sec  Rows with created_at older than now - max_age_sec are deleted
 * @return Total number of rows deleted, or -1 on error
 */
int missed_notif_expire(int max_age_sec);

#ifdef __cplusplus
}
#endif

#endif /* MISSED_NOTIFICATIONS_DB_H */
