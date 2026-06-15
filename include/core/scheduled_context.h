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
 * Scheduled-origin context — a thread-local marker the scheduler sets around
 * a scheduled tool-callback invocation so tools can (a) recover the owning
 * user_id (the scheduler thread has no session, so session_get_command_context
 * returns NULL and tools would otherwise fall back to user 1) and (b) gate
 * which actions may run unattended.  Layer 1 / Foundation: libc only, no DAWN
 * state, always compiled.
 */
#ifndef SCHEDULED_CONTEXT_H
#define SCHEDULED_CONTEXT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Mark the current thread as executing on behalf of @p user_id from a
 *        scheduled (briefing/task) context.  Pair with scheduled_context_clear.
 *
 * @param user_id Owning user (> 0).
 */
void scheduled_context_set(int user_id);

/**
 * @brief Clear the scheduled-origin marker for the current thread.
 */
void scheduled_context_clear(void);

/**
 * @brief Query whether the current thread is in a scheduled-origin scope.
 *
 * @param user_id_out  If non-NULL, set to the scheduled user_id (0 when not in
 *                     a scheduled scope).
 * @return true if currently executing in a scheduled-origin scope.
 */
bool scheduled_context_get(int *user_id_out);

#ifdef __cplusplus
}
#endif

#endif /* SCHEDULED_CONTEXT_H */
