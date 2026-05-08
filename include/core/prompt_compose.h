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
 * Pure-function prompt composer — Phase 1e of Dynamic Context Injection.
 *
 * Lifted out of session_manager.c so the composer logic can be unit-
 * tested without dragging the full session-manager runtime (auth_db,
 * conv_db, satellite_db, json-c, ...) into the test link.  The
 * functions declared here are still re-exposed via session_manager.h
 * with the public names `composed_prompt_free` and
 * `session_manager_compose_prompt_string` — production callers SHOULD
 * use those.  This header is effectively internal (used only by the
 * extracted .c, the session_manager.c wrappers, and tests/test_prompt_builder*).
 * Future-proofing note: keep the call set narrow so the L1 layer cut
 * stays clean.  If a 1f optimization wants block-level access (e.g.,
 * caching base_prompt across PER_TURN refreshes), thread the access
 * through the structured `session_prompt_builder_t` registration
 * instead of growing this header's surface.
 *
 * Layer: 1.  Depends only on libc + composed_prompt_t (declared in
 * session_manager.h).
 */

#ifndef CORE_PROMPT_COMPOSE_H
#define CORE_PROMPT_COMPOSE_H

#include "core/session_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Free the heap allocations inside a composed_prompt_t.
 *
 * Idempotent on a zeroed / already-freed struct; no-op on NULL.
 * Resets all three pointers to NULL so the struct is safe to reuse.
 *
 * Public alias: `composed_prompt_free` in session_manager.h.
 *
 * @param p Composed prompt to free (NULL-safe).
 */
void prompt_compose_free(composed_prompt_t *p);

/**
 * @brief Flatten a composed_prompt_t into a single allocated string.
 *
 * Concatenates `base_prompt + memory_block + focus_block` with the
 * existing `--- USER MEMORY ---` framing identity for the memory
 * section (which arrives pre-framed from `memory_build_context`)
 * and a parallel `--- TURN CONTEXT ---` framing for the focus
 * section.  NULL or empty `focus_block` omits the focus section
 * entirely (preserves byte-identical pre-1e output when the
 * feature is disabled).
 *
 * Public alias: `session_manager_compose_prompt_string` in
 * session_manager.h.
 *
 * @param blocks Composed prompt; NULL or NULL `base_prompt` → NULL.
 * @return Caller-owned string; NULL on bad input or OOM.
 */
char *prompt_compose_to_string(const composed_prompt_t *blocks);

#ifdef __cplusplus
}
#endif

#endif /* CORE_PROMPT_COMPOSE_H */
