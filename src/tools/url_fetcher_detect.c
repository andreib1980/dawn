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
 * Pure detector helpers for the FlareSolverr Chromium-error-page gate.
 * No external deps beyond utils/string_utils.h's strcasestr_portable.
 */

#include "tools/url_fetcher_detect.h"

#include <stddef.h>

#include "utils/string_utils.h"

int count_chromium_error_signatures(const char *text) {
   if (!text) {
      return 0;
   }
   static const char *const SIGS[] = { "DNS_PROBE_FINISHED_BAD_CONFIG", "ERR_NAME_NOT_RESOLVED",
                                       "site can't be reached",
                                       "server IP address could not be found",
                                       "Check your DNS settings" };
   int hits = 0;
   for (size_t i = 0; i < sizeof(SIGS) / sizeof(SIGS[0]); i++) {
      if (strcasestr_portable(text, SIGS[i])) {
         hits++;
      }
   }
   return hits;
}

bool html_has_article_markers(const char *html) {
   if (!html) {
      return false;
   }
   /* `<h1>` is intentionally NOT in this list — Chromium's network error
    * template uses `<h1>This site can't be reached</h1>` for the headline,
    * which falsely indicated "real article" and let error pages through the
    * SEC-H2 detection gate. `<article>` and `<main>` are reliable structural
    * markers; `<h1>` appears too broadly across UI chrome to discriminate.
    * Observed 2026-05-13: Barron's FlareSolverr DNS failure leaked the
    * Chromium error page to the LLM because its h1 fooled this check. */
   return strcasestr_portable(html, "<article") || strcasestr_portable(html, "<main");
}
