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

#ifdef __cplusplus
}
#endif

#endif /* URL_FETCHER_DETECT_H */
