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
 * @file memory_focus_adapters.h
 * @brief Memory focus-source adapters — Phase 1c of Dynamic Context Injection.
 *
 * Wires the four memory-side adapters (facts, entities, relations,
 * summaries) into the focus-source framework.  Adapters are registered
 * via `memory_focus_adapters_register_all()`; daemon-startup wiring
 * lands in Phase 1e (this header is currently called only from tests).
 *
 * Layer: 2.  All adapter `query` callbacks operate strictly on
 * memory-subsystem APIs; no Layer 3 dependencies.
 *
 * Cross-user isolation: every adapter scopes its `memory_db_*` calls
 * by the `user_id` parameter passed through `focus_source_query_fn`.
 * The fact and vector-only paths additionally post-check
 * `memory_fact_t.user_id` against the requested user — defense in depth
 * against any future regression in the underlying retriever's scoping.
 */

#ifndef MEMORY_FOCUS_ADAPTERS_H
#define MEMORY_FOCUS_ADAPTERS_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register all four memory adapters (fact, entity, relation,
 *        summary) into the focus-source framework.
 *
 * Idempotent in spirit but NOT in implementation: re-calling without
 * an intervening `memory_focus_adapters_unregister_all()` returns
 * FAILURE on the duplicate-register path of the second adapter.  Tests
 * call `focus_unregister_all()` (test-only) in `setUp()` so each
 * register cycle starts from a clean registry.
 *
 * @return SUCCESS if all four register; FAILURE on the first failure
 *         (subsequent adapters are NOT registered after the first
 *         failure — caller is expected to surface the error).
 */
int memory_focus_adapters_register_all(void);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_FOCUS_ADAPTERS_H */
