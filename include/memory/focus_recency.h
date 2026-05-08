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
 * Recency-decay helper shared by every focus-source adapter that wants
 * the same uniform decay shape across all sources.  Phase 1d extraction
 * from memory_focus_adapters.c — kept framework-scoped (NOT
 * memory-subsystem-scoped) so L3 adapters can consume it without
 * pulling memory headers.
 *
 * Layer: 2.  Pure libc — depends only on <math.h> + <time.h>.  L3
 * adapters (document, calendar) include this directly without crossing
 * a layer boundary.
 *
 * Note on physical location: this header lives next to `focus_source.h`
 * because the framework already owns `include/memory/focus_*.h` for the
 * same architectural reason — adapters polymorphically register against
 * the framework, so the shared helpers travel with the framework, not
 * with their primary consumer.
 */

#ifndef MEMORY_FOCUS_RECENCY_H
#define MEMORY_FOCUS_RECENCY_H

#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Recency decay half-life applied uniformly across adapters in v1.
 * TODO(1j): per-source tuning + config exposure once bench evidence
 * justifies the additional knobs.  Calendar uses an asymmetric formula
 * (past vs future) defined locally in its adapter — this constant is
 * the single-half-life default for everything else (memory facts /
 * entities / relations / summaries / document chunks / future email). */
#define FOCUS_RECENCY_HALF_LIFE_SECONDS (14 * 86400)

/**
 * @brief Uniform exponential recency-decay score.
 *
 * Returns 0.5 ** (age / FOCUS_RECENCY_HALF_LIFE_SECONDS), clamped to
 * [0, 1].  Items in the future (clock skew) collapse to 1.0.  Returns
 * 0.0 for `item_ts <= 0` (no creation time recorded) so the ranker
 * doesn't credit unknowns.  Never returns FOCUS_SCORE_NA.
 *
 * Identical math to the pre-extraction `compute_recency_score()` in
 * `memory_focus_adapters.c` — extraction is behavior-preserving.
 *
 * @param item_ts  Item creation / event time (epoch seconds)
 * @param now      Reference time the score is computed against
 * @return 0.0 .. 1.0
 */
float focus_recency_decay_uniform(time_t item_ts, time_t now);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_FOCUS_RECENCY_H */
