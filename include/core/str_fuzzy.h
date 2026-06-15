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
 * Lightweight fuzzy name matcher — shared by surfaces that resolve a
 * user-typed name (Home Assistant entities, Discord channels, ...) against
 * a list of candidate names.  Layer 1 / Foundation: depends only on libc,
 * no DAWN state.  Extracted from the byte-identical helper that previously
 * lived static in homeassistant_service.c.
 */

#ifndef STR_FUZZY_H
#define STR_FUZZY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Score tiers returned by str_fuzzy_score(). */
#define STR_FUZZY_SCORE_EXACT 100      /* candidate == needle */
#define STR_FUZZY_SCORE_CONTAINS 80    /* candidate contains needle as substring */
#define STR_FUZZY_SCORE_TOKEN_BONUS 20 /* per whitespace-delimited needle token found */

/**
 * @brief Lowercase @p src into @p dst (ASCII tolower), bounded by @p max_len.
 *
 * Always NUL-terminates within @p max_len.  Intended to pre-normalize both
 * candidate and needle strings before scoring.
 *
 * @param dst     Output buffer.
 * @param src     Source string.
 * @param max_len Size of @p dst (must be >= 1).
 */
void str_fuzzy_tolower(char *dst, const char *src, size_t max_len);

/**
 * @brief Score how well @p needle_lower matches @p haystack_lower.
 *
 * Both arguments MUST already be lowercased (see str_fuzzy_tolower()).
 * Tiered, allocation-free scoring:
 *   - 100  exact match
 *   -  80  haystack contains needle as a substring
 *   - +20  per whitespace-delimited needle token found in haystack
 *
 * The tiers are not mutually exclusive in spirit but return early: an exact
 * or substring hit short-circuits before token scoring.  Callers compare
 * scores across candidates and apply their own acceptance threshold.
 *
 * @param haystack_lower Candidate name, lowercased.
 * @param needle_lower   User-supplied query, lowercased.
 * @return Match score (0 = no overlap; higher is better).
 */
int str_fuzzy_score(const char *haystack_lower, const char *needle_lower);

#ifdef __cplusplus
}
#endif

#endif /* STR_FUZZY_H */
