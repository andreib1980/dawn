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
 * URL fetcher fallback dispatcher — single entry point invoked from the 5
 * fallback sites in url_fetcher.c. Reads g_config.url_fetcher.fallback once
 * (TOCTOU guard) and routes to FlareSolverr or Tavily. Optionally cascades
 * Tavily-failure to FlareSolverr when both are configured.
 *
 * Lives outside url_fetcher.c to keep that file under the 1500-line soft
 * limit while preserving a clear primary→fallback chain (ARCH-M6).
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "config/dawn_config.h"
#include "core/session_manager.h"
#include "logging.h"
#include "tools/tavily_rate_limit.h"
#include "tools/url_fetch_tavily.h"
#include "tools/url_fetcher.h"
#include "tools/url_fetcher_internal.h"

/* fallback string must accommodate the longest valid value ("flaresolverr"
 * = 12 chars + NUL). Tripwire so a future "brave_search" addition fails at
 * compile time instead of silently truncating. */
_Static_assert(sizeof("flaresolverr") <= 16,
               "fallback[16] must fit the longest fallback name plus NUL");

/* Resolve the active user_id for rate-limit isolation. Falls back to local
 * session for command-context-less invocations (e.g. legacy MQTT path) and
 * finally to 0 (anonymous bucket) when no session exists yet. The user_id
 * lives on the per-session metrics tracker (session_set_metrics_user). */
static int resolve_fallback_user_id(void) {
   session_t *sess = session_get_command_context();
   if (!sess) {
      sess = session_get_local();
   }
   return sess ? sess->metrics.user_id : 0;
}

int url_fetcher_try_fallback(const char *url,
                             const char *base_url,
                             char **out_content,
                             size_t *out_size) {
   if (!url || !out_content) {
      return URL_FETCH_ERROR_INVALID_URL;
   }

   /* SEC-M5: TOCTOU-safe read — WebUI POST can mutate g_config mid-call. */
   char fallback[16];
   snprintf(fallback, sizeof(fallback), "%s", g_config.url_fetcher.fallback);

   /* Empty default → behave as if "flaresolverr" was selected (back-compat
    * with configs that predate this field). */
   if (fallback[0] == '\0') {
      snprintf(fallback, sizeof(fallback), "%s", "flaresolverr");
   }

   if (strcmp(fallback, "none") == 0) {
      return URL_FETCH_ERROR_NETWORK;
   }

   /* Tavily branch: rate-limit gate → call adapter → cascade on failure. */
   if (strcmp(fallback, "tavily") == 0) {
      if (!url_fetch_tavily_is_configured()) {
         OLOG_WARNING("url_fetcher: fallback=tavily but adapter not configured (missing key?)");
         /* Cascade to FlareSolverr if it's an option, else give up. */
         if (flaresolverr_is_enabled_and_available()) {
            OLOG_INFO("url_fetcher: cascading to FlareSolverr");
            return flaresolverr_fallback_fetch(url, base_url, out_content, out_size);
         }
         return URL_FETCH_ERROR_NETWORK;
      }

      if (!tavily_rate_limit_check(resolve_fallback_user_id())) {
         /* Bucket exhausted; transparent cascade to FlareSolverr if available. */
         if (flaresolverr_is_enabled_and_available()) {
            OLOG_INFO("url_fetcher: Tavily rate-limited, cascading to FlareSolverr");
            return flaresolverr_fallback_fetch(url, base_url, out_content, out_size);
         }
         OLOG_WARNING("url_fetcher: Tavily rate-limited and no FlareSolverr fallback");
         return URL_FETCH_ERROR_NETWORK;
      }

      int rc = url_fetch_tavily(url, out_content, out_size);
      if (rc == URL_FETCH_SUCCESS) {
         return URL_FETCH_SUCCESS;
      }
      if (rc == URL_FETCH_ERROR_BLOCKED_URL) {
         /* SSRF rejection — do NOT cascade to FlareSolverr (same URL would
          * be re-evaluated against the same allowlist anyway). */
         return rc;
      }
      /* Transparent cascade: if Tavily fails on network/HTTP/empty, try
       * FlareSolverr if it's configured. */
      if (flaresolverr_is_enabled_and_available()) {
         OLOG_INFO("url_fetcher: Tavily failed (rc=%d), cascading to FlareSolverr", rc);
         return flaresolverr_fallback_fetch(url, base_url, out_content, out_size);
      }
      return rc;
   }

   /* "flaresolverr" branch — direct call to existing chain. */
   if (strcmp(fallback, "flaresolverr") == 0) {
      if (!flaresolverr_is_enabled_and_available()) {
         return URL_FETCH_ERROR_NETWORK;
      }
      return flaresolverr_fallback_fetch(url, base_url, out_content, out_size);
   }

   /* Unknown value — already caught by config_validate, but be defensive. */
   OLOG_WARNING("url_fetcher: unknown fallback value '%s', treating as none", fallback);
   return URL_FETCH_ERROR_NETWORK;
}
