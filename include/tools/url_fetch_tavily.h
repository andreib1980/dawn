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
 * Tavily /extract adapter — opt-in alternative to FlareSolverr for fetching
 * JS-rendered / anti-bot-protected pages. POSTs to https://api.tavily.com/extract.
 *
 * SSRF posture: DAWN does NOT delegate internal-IP / SSRF decisions to
 * upstream. The adapter calls url_is_valid() AND url_is_blocked() on the
 * inbound URL BEFORE handing off to Tavily — same allowlist that gates the
 * direct libcurl fetch path.
 *
 * Thread Safety: Stateless. Each call uses its own CURL handle and its own
 * json-c root object. Safe to invoke from concurrent LLM tool worker threads.
 * No module-level mutex; do not introduce per-process caches without
 * revisiting concurrency.
 *
 * Selection: invoked by the url_fetcher_fallback dispatcher when
 * [url_fetcher] fallback = "tavily" AND secrets.tavily_api_key is set.
 */

#ifndef URL_FETCH_TAVILY_H
#define URL_FETCH_TAVILY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Fetch a URL via Tavily /extract and return clean markdown.
 *
 * Pre-validates the URL via url_is_blocked() to prevent SSRF — internal /
 * RFC1918 / metadata addresses are rejected with URL_FETCH_ERROR_BLOCKED_URL
 * BEFORE the POST. We do not delegate this check to Tavily.
 *
 * @param url URL to fetch.
 * @param out_content Receives allocated markdown content (caller frees).
 * @param out_size Receives content size (optional, can be NULL).
 * @return URL_FETCH_SUCCESS on success, URL_FETCH_ERROR_* on failure.
 */
int url_fetch_tavily(const char *url, char **out_content, size_t *out_size);

/**
 * @brief True when [url_fetcher] fallback = "tavily" AND tavily_api_key is set.
 *        Cheap accessor used by the dispatcher in url_fetcher_fallback.c.
 */
int url_fetch_tavily_is_configured(void);

#ifdef __cplusplus
}
#endif

#endif /* URL_FETCH_TAVILY_H */
