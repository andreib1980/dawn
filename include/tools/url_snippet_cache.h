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
 * URL → search-snippet cache. Populated by search adapters (SearXNG and
 * Tavily) every time a result is returned to the LLM; consulted by the
 * url_fetch path when a fetch comes back chrome-dominated, so we can
 * substitute the pre-extracted snippet instead of feeding the LLM a wall
 * of nav links.
 *
 * Fixed-size LRU with mutex. Thread-safe — concurrent worker threads
 * publish and consume freely.
 */

#ifndef URL_SNIPPET_CACHE_H
#define URL_SNIPPET_CACHE_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bounded for personal-use deployments. 64 entries covers a few
 * conversations' worth of unique URLs without significant memory cost
 * (~64 × ~2.7 KB max = ~170 KB worst case). */
#define URL_SNIPPET_CACHE_MAX_ENTRIES 64

/* TTL — snippets expire after this many seconds. Sized for typical
 * conversation horizon; older snippets aren't relevant to the current
 * tool loop and would just inflate false-substitution risk. */
#define URL_SNIPPET_CACHE_TTL_SEC 1800 /* 30 minutes */

/**
 * @brief Insert (or update) a URL → snippet mapping.
 *
 * If the URL is already cached, its snippet and timestamp are refreshed.
 * If the cache is full, the least-recently-used entry is evicted.
 * Empty or NULL snippets are silently ignored — there's nothing useful
 * to remember.
 */
void url_snippet_cache_put(const char *url, const char *snippet);

/**
 * @brief Look up a snippet for @p url. Copies into @p out_snippet on hit;
 *        leaves it untouched on miss.
 *
 * Entries older than URL_SNIPPET_CACHE_TTL_SEC count as misses and are
 * lazily evicted on access.
 *
 * @param url            URL to look up (exact match).
 * @param out_snippet    Buffer to receive the snippet on hit.
 * @param out_snippet_sz Size of @p out_snippet (including NUL).
 * @return true on hit (out_snippet populated), false on miss.
 */
bool url_snippet_cache_get(const char *url, char *out_snippet, size_t out_snippet_sz);

/**
 * @brief Clear all entries. Used by tests; safe in production.
 */
void url_snippet_cache_clear(void);

/**
 * @brief Number of live (non-expired, non-empty) entries. Diagnostics only.
 */
int url_snippet_cache_count(void);

#ifdef __cplusplus
}
#endif

#endif /* URL_SNIPPET_CACHE_H */
