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
 * Pure-function prompt composer — composes the two-segment
 * composed_prompt_t (stable_prefix + volatile_block) into either:
 *   (a) a flattened string for legacy callers that want one blob, or
 *   (b) one or two role:"system" messages pushed into a json-c history
 *       array — the shape the per-turn LLM dispatch uses so Anthropic
 *       can attach `cache_control` to the stable prefix only.
 *
 * Lifted out of session_manager.c so the composer logic can be unit-
 * tested without dragging the full session-manager runtime (auth_db,
 * conv_db, satellite_db, json-c init...) into the test link.  The
 * functions declared here are re-exposed via session_manager.h with
 * the public names `composed_prompt_free` and
 * `session_manager_compose_prompt_string` — production callers SHOULD
 * use those.
 *
 * Layer: 1.  Depends only on libc + json-c + composed_prompt_t (declared
 * in session_manager.h).
 */

#ifndef CORE_PROMPT_COMPOSE_H
#define CORE_PROMPT_COMPOSE_H

#include <stdint.h>

#include "core/session_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Free the heap allocations inside a composed_prompt_t.
 *
 * Idempotent on a zeroed / already-freed struct; no-op on NULL.
 * Resets both pointers to NULL so the struct is safe to reuse.
 *
 * Public alias: `composed_prompt_free` in session_manager.h.
 *
 * @param p Composed prompt to free (NULL-safe).
 */
void prompt_compose_free(composed_prompt_t *p);

/**
 * @brief Flatten a composed_prompt_t into a single allocated string.
 *
 * Concatenates `stable_prefix + volatile_block`.  Either segment may
 * be NULL or empty — the omitted segment contributes nothing (no
 * separator, no marker).  Used by legacy callers that need a single
 * `char *` (capability-refresh paths, WebUI system-prompt inspector).
 *
 * Returns NULL when BOTH segments are NULL/empty, or on OOM.
 *
 * Public alias: `session_manager_compose_prompt_string` in
 * session_manager.h.
 *
 * @param blocks Composed prompt; NULL → NULL.
 * @return Caller-owned string; NULL on bad input or OOM.
 */
char *prompt_compose_to_string(const composed_prompt_t *blocks);

/**
 * @brief FNV-1a hash of @p s.
 *
 * Exposed for session_manager's drift detection on the stable prefix.
 * Cheap (~5 ns/byte on Jetson) so calling on each per-turn refresh
 * adds no measurable cost.  Empty / NULL input → FNV-1a basis.
 *
 * @param s NUL-terminated string (NULL safe).
 * @return 32-bit FNV-1a hash.
 */
uint32_t prompt_compose_fnv1a(const char *s);

#ifdef __cplusplus
}
#endif

#endif /* CORE_PROMPT_COMPOSE_H */
