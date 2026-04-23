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
 * Time Query Parser — extract temporal references from natural-language
 * search queries.  Used by retrieval to boost facts/chunks whose created_at
 * is near the user's referenced point in time.
 *
 * Recognized expressions (case-insensitive, looked up anywhere in query):
 *   absolute year         "in 2020", "during 2021", "back in 2019"
 *   month + year          "September 2022", "in May 2020", "January 2019"
 *   season + year         "summer 2021", "winter 2020", "fall 2022"
 *   relative durations    "last week", "last month", "last year"
 *   today/yesterday       "today", "yesterday"
 *   recently              "recently", "the other day"
 *
 * The first match wins (top-down precedence: absolute date > relative > vague).
 *
 * Returns a target timestamp (midpoint of the referenced window) plus the
 * window width so callers can score by Gaussian decay or hard-window match.
 */

#ifndef TIME_QUERY_PARSER_H
#define TIME_QUERY_PARSER_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
   TQP_NONE = 0, /* No temporal reference found */
   TQP_ABSOLUTE, /* "in 2020", "September 2022", "summer 2021" */
   TQP_RELATIVE, /* "last week", "yesterday", "last month" */
   TQP_VAGUE     /* "recently", "the other day" */
} time_query_kind_t;

typedef struct {
   bool found;             /* True if any temporal expression was extracted */
   time_query_kind_t kind; /* Specificity classification */
   int64_t target_ts;      /* Unix timestamp (UTC) at the midpoint of the ref window */
   int64_t window_seconds; /* Half-width of the window in seconds (used for decay scaling) */
   char matched[64];       /* The substring that matched (for logging/debugging) */
} time_query_t;

/**
 * @brief Parse a natural-language query for temporal expressions.
 *
 * Scans the query for date references and returns the first match.  Caller
 * checks `out->found` to decide whether to apply temporal scoring.
 *
 * `now_ts` is supplied (rather than reading time(NULL) internally) so unit
 * tests can pin the "current time" deterministically and so callers can
 * evaluate queries against a non-current reference point.
 *
 * @param query    Lower- or mixed-case query string.  Not modified.
 * @param now_ts   Reference "now" (Unix seconds, UTC).  Used to anchor
 *                 relative expressions ("last week", "yesterday").
 * @param out      Caller-allocated.  Always initialized; check `found`.
 *
 * @return SUCCESS (0) on any input; FAILURE only on NULL inputs.
 */
int time_query_parse(const char *query, int64_t now_ts, time_query_t *out);

/**
 * @brief Compute a [0,1] proximity score given a chunk's timestamp.
 *
 * Uses Gaussian decay: score = exp(-(distance / window)^2 / 2).  At distance
 * = 0 the score is 1.0; at distance = window it falls to ~0.6; at 3*window
 * it's ~0.01.  Returns 0.0 when `tq->found` is false.
 *
 * @param tq          Output of time_query_parse.
 * @param chunk_ts    Chunk's created_at (Unix seconds).  Pass 0 for unknown,
 *                    which yields 0.0 (no boost).
 * @return            Proximity score in [0.0, 1.0].
 */
float time_query_proximity(const time_query_t *tq, int64_t chunk_ts);

#ifdef __cplusplus
}
#endif

#endif /* TIME_QUERY_PARSER_H */
