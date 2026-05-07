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
 * Focus-source framework — TEST-ONLY internal API surface.
 *
 * Included only by `tests/test_focus_source.c`.  Production code MUST
 * NOT include this header — the registry is intended to be append-only
 * after daemon init, and the reset hook here exists solely so each
 * Unity test starts from an empty registry.
 */

#ifndef MEMORY_FOCUS_SOURCE_INTERNAL_H
#define MEMORY_FOCUS_SOURCE_INTERNAL_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Drop every registered adapter so the next `focus_register_source()`
 *        starts from an empty registry.
 *
 * NEVER call from production code.  MUST be called from a single-threaded
 * context — the registry mutex protects the reset itself, but
 * `focus_compose()` is documented as lock-free for iteration after
 * init.  Calling this concurrently with an in-flight `focus_compose()`
 * is a data race; ThreadSanitizer would flag it.  Test harnesses serialize
 * this through Unity's `setUp()` so the constraint is naturally honored.
 */
void focus_unregister_all(void);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_FOCUS_SOURCE_INTERNAL_H */
