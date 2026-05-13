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
 * URL Fetcher detector helpers — pure functions that classify content as
 * Chromium error pages vs. legitimate articles. Lives in its own translation
 * unit so the unit test suite can link against it without pulling in
 * url_fetcher.c's libcurl/json-c/memory-filter transitive deps.
 */

#ifndef URL_FETCHER_DETECT_H
#define URL_FETCHER_DETECT_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Count how many distinct Chromium / network error-page signatures
 *        appear in @p text. Case-insensitive substring match.
 *
 * Pure function. NULL-safe.
 */
int count_chromium_error_signatures(const char *text);

/**
 * @brief True if @p html contains a strong article-structure marker
 *        (`<article` or `<main`). Used as the "this is a real page, don't
 *        flag it as an error page" arm of the combined FlareSolverr
 *        error-page detection gate (SEC-H2).
 *
 * `<h1>` is intentionally NOT checked: Chromium's network error template
 * uses `<h1>This site can't be reached</h1>` for the headline, which would
 * make every DNS-failed FlareSolverr response look like a real article.
 *
 * Pure function. NULL-safe.
 */
bool html_has_article_markers(const char *html);

/**
 * @brief Metrics computed by markdown_is_chrome_dominated() — exposed so
 *        callers can log the values that drove the decision. All fields
 *        are zero-initialized when the input is NULL or empty.
 */
typedef struct {
   size_t total_bytes;       /* Length of input in bytes */
   int link_tail_count;      /* Count of "](http" occurrences (markdown link tails) */
   int paragraph_count;      /* Count of runs of 200+ contiguous non-newline prose chars */
   double link_density;      /* link_tail_count per KB (link_tail_count * 1024 / total_bytes) */
   double paragraph_density; /* paragraph_count per KB */
} chrome_metrics_t;

/**
 * @brief Heuristic: is this markdown dominated by site chrome (nav menus,
 *        link lists, headers) rather than article prose?
 *
 * Signals combined: high link-tail density AND low paragraph density AND
 * sufficient sample size. Tuned for high precision (>95%) over recall —
 * we'd rather miss some chrome cases than wrap a legitimate article in
 * the "snippet preferred" path.
 *
 * The caller uses this AND the presence of a cached search snippet for the
 * same URL to decide whether to substitute the snippet for the full fetch.
 * If we return true but no snippet is available, the caller keeps the raw
 * fetch (degraded gracefully — never worse than today).
 *
 * Pure function. NULL-safe.
 *
 * @param markdown  Heap or static buffer to inspect.
 * @param len       Length in bytes. Pass 0 to skip the size-floor check.
 * @param out_metrics Optional out param for diagnostics. May be NULL.
 */
bool markdown_is_chrome_dominated(const char *markdown, size_t len, chrome_metrics_t *out_metrics);

#ifdef __cplusplus
}
#endif

#endif /* URL_FETCHER_DETECT_H */
