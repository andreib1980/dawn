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

/* Threshold tuning — tightened from initial PitchBook / Investors.com /
 * TipRanks fixtures observed live 2026-05-12. High precision over recall;
 * a false positive (legit article flagged as chrome) gets replaced with a
 * search snippet — still on-topic, bounded loss — but a false negative
 * leaves today's behavior unchanged.
 *
 *   - LINK_DENSITY_THRESHOLD: 0.4 link tails per KB. PitchBook truncated
 *     content had ~3.5/KB; a typical article has 0.05–0.15.
 *   - PARAGRAPH_DENSITY_THRESHOLD: 0.5 long-prose-runs per KB. Heavy-chrome
 *     pages have near-zero; articles have ~1–3.
 *   - MIN_SAMPLE_BYTES: 4 KB. Very small fetches don't have enough signal
 *     for confident classification.
 *   - PROSE_RUN_MIN_CHARS: 200. A "paragraph" is at least this many
 *     contiguous non-newline characters. */
#define LINK_DENSITY_THRESHOLD 0.4
#define PARAGRAPH_DENSITY_THRESHOLD 0.5
#define MIN_SAMPLE_BYTES 4096
#define PROSE_RUN_MIN_CHARS 200

bool markdown_is_chrome_dominated(const char *markdown, size_t len, chrome_metrics_t *out_metrics) {
   if (out_metrics) {
      out_metrics->total_bytes = 0;
      out_metrics->link_tail_count = 0;
      out_metrics->paragraph_count = 0;
      out_metrics->link_density = 0.0;
      out_metrics->paragraph_density = 0.0;
   }
   if (!markdown || len < MIN_SAMPLE_BYTES) {
      return false;
   }

   /* Count "](http" occurrences — markdown link tails. Single pass. */
   int link_tails = 0;
   for (size_t i = 0; i + 6 <= len; i++) {
      if (markdown[i] == ']' && markdown[i + 1] == '(' && markdown[i + 2] == 'h' &&
          markdown[i + 3] == 't' && markdown[i + 4] == 't' && markdown[i + 5] == 'p') {
         link_tails++;
      }
   }

   /* Count prose runs — contiguous non-newline character spans of at least
    * PROSE_RUN_MIN_CHARS. Single pass, O(N). */
   int paragraphs = 0;
   size_t run_start = 0;
   bool in_run = false;
   for (size_t i = 0; i <= len; i++) {
      char c = (i < len) ? markdown[i] : '\n'; /* sentinel close at EOF */
      if (c == '\n') {
         if (in_run && (i - run_start) >= PROSE_RUN_MIN_CHARS) {
            paragraphs++;
         }
         in_run = false;
      } else if (!in_run) {
         run_start = i;
         in_run = true;
      }
   }

   double kb = (double)len / 1024.0;
   double link_density = (double)link_tails / kb;
   double paragraph_density = (double)paragraphs / kb;

   if (out_metrics) {
      out_metrics->total_bytes = len;
      out_metrics->link_tail_count = link_tails;
      out_metrics->paragraph_count = paragraphs;
      out_metrics->link_density = link_density;
      out_metrics->paragraph_density = paragraph_density;
   }

   return (link_density > LINK_DENSITY_THRESHOLD) &&
          (paragraph_density < PARAGRAPH_DENSITY_THRESHOLD);
}
