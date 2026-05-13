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
 * Web Search Module — Tavily /search adapter
 *
 * Opt-in alternative to SearXNG. POSTs to https://api.tavily.com/search,
 * returns results normalized into the shared search_result_t struct so the
 * dispatcher in web_search.c can use either provider transparently.
 *
 * Thread Safety: Stateless. Each call uses its own CURL handle and its own
 * json-c root object. Safe to invoke from concurrent LLM tool worker threads.
 * No module-level mutex; do not introduce per-process caches without revisiting
 * concurrency.
 *
 * Selection: enabled when [search] engine = "tavily" AND secrets.tavily_api_key
 * is non-empty. Failures fall through to SearXNG via the web_search.c
 * dispatcher; rate-limit hits also fall through.
 */

#ifndef WEB_SEARCH_TAVILY_H
#define WEB_SEARCH_TAVILY_H

#include "tools/web_search.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief True when [search] engine = "tavily" AND tavily_api_key is set.
 *        Cheap accessor used by the dispatcher in web_search.c.
 */
int web_search_tavily_is_configured(void);

/**
 * @brief Perform a Tavily /search query.
 *
 * @param query Search query (must be non-empty).
 * @param max_results Max results to return (clamped 1..25).
 * @param type DAWN search type — only NEWS maps to Tavily's "news" topic;
 *             all others map to "general". Categories beyond news/general
 *             are NOT supported by Tavily.
 * @param time_range "day" | "week" | "month" | "year" | NULL. Only takes
 *             effect with NEWS topic.
 *
 * @return Heap-allocated search_response_t. Caller frees via
 *         web_search_free_response(). On failure returns a response with
 *         response->error set; the dispatcher uses error presence to decide
 *         whether to cascade to SearXNG.
 */
search_response_t *web_search_tavily_query_typed(const char *query,
                                                 int max_results,
                                                 search_type_t type,
                                                 const char *time_range);

#ifdef __cplusplus
}
#endif

#endif /* WEB_SEARCH_TAVILY_H */
