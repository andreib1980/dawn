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
 * Unit tests for the FlareSolverr Chromium-error-page detection (SEC-H2,
 * ARCH-M4). Pins the combined-gate algorithm against false-positives on
 * legitimate articles about DNS errors and against single-signature
 * censorship attempts.
 *
 * Pure tests on the two pure-function detectors exposed via
 * url_fetcher_internal.h. The full integration (length-cap + signature
 * count + article-marker absence) is exercised by composing the helpers
 * here; the combined decision lives in flaresolverr_fallback_fetch() and
 * is covered indirectly.
 */

#include <stdbool.h>
#include <stddef.h>

#include "tools/url_fetcher_detect.h"
#include "unity.h"

void setUp(void) {
}
void tearDown(void) {
}

/* ── count_chromium_error_signatures ─────────────────────────────────────── */

static void test_count_null_returns_zero(void) {
   TEST_ASSERT_EQUAL_INT(0, count_chromium_error_signatures(NULL));
}

static void test_count_empty_returns_zero(void) {
   TEST_ASSERT_EQUAL_INT(0, count_chromium_error_signatures(""));
}

static void test_count_clean_article(void) {
   const char *article = "Lorem ipsum dolor sit amet, consectetur adipiscing elit. "
                         "Sed do eiusmod tempor incididunt ut labore et dolore magna aliqua.";
   TEST_ASSERT_EQUAL_INT(0, count_chromium_error_signatures(article));
}

static void test_count_one_signature(void) {
   const char *page = "An error occurred: DNS_PROBE_FINISHED_BAD_CONFIG";
   TEST_ASSERT_EQUAL_INT(1, count_chromium_error_signatures(page));
}

static void test_count_full_chromium_error_page(void) {
   /* Approximation of the actual Chromium error page seen live 2026-05-12. */
   const char *page = "This site can't be reached "
                      "example.com's server IP address could not be found "
                      "DNS_PROBE_FINISHED_BAD_CONFIG Check your DNS settings";
   /* Four distinct signatures present. */
   TEST_ASSERT_GREATER_OR_EQUAL_INT(4, count_chromium_error_signatures(page));
}

static void test_count_case_insensitive(void) {
   const char *page = "dns_probe_finished_bad_config and ERR_NAME_NOT_RESOLVED";
   TEST_ASSERT_EQUAL_INT(2, count_chromium_error_signatures(page));
}

/* ── html_has_article_markers ────────────────────────────────────────────── */

static void test_markers_null(void) {
   TEST_ASSERT_FALSE(html_has_article_markers(NULL));
}

static void test_markers_empty(void) {
   TEST_ASSERT_FALSE(html_has_article_markers(""));
}

static void test_markers_with_article_tag(void) {
   TEST_ASSERT_TRUE(html_has_article_markers("<html><body><article>x</article></body></html>"));
}

static void test_markers_with_main_tag(void) {
   TEST_ASSERT_TRUE(html_has_article_markers("<main id=content>...</main>"));
}

static void test_markers_h1_alone_does_not_count(void) {
   /* Chromium's network error template uses `<h1>This site can't be reached</h1>`,
    * so we deliberately do NOT treat `<h1>` as an article-body marker. */
   TEST_ASSERT_FALSE(html_has_article_markers("<div><h1>Title</h1></div>"));
}

static void test_markers_chromium_error_template_not_an_article(void) {
   /* Real shape observed 2026-05-13 on FlareSolverr DNS failure. */
   const char *err = "<html><body><h1>This site can't be reached</h1>"
                     "<p>DNS_PROBE_FINISHED_BAD_CONFIG</p></body></html>";
   TEST_ASSERT_FALSE(html_has_article_markers(err));
}

static void test_markers_case_insensitive(void) {
   TEST_ASSERT_TRUE(html_has_article_markers("<ARTICLE>x</ARTICLE>"));
   TEST_ASSERT_TRUE(html_has_article_markers("<MAIN>x</MAIN>"));
}

static void test_markers_only_chromium_error_page(void) {
   /* Chromium error page typically has <head><title>..</title></head><body>
    * but no <article>, <main>, or <h1>. */
   const char *err = "<html><head><title>error</title></head>"
                     "<body><div>DNS_PROBE_FINISHED_BAD_CONFIG</div></body></html>";
   TEST_ASSERT_FALSE(html_has_article_markers(err));
}

/* ── Combined gate (composes the two helpers) ────────────────────────────── */

/*
 * The production gate inside flaresolverr_fallback_fetch is:
 *   reject IFF (extracted_len < 2048) AND (no article markers in HTML)
 *               AND (signature count >= 2)
 *
 * Tests below verify the composition produces the expected REJECT / KEEP
 * decision for representative inputs.
 */

static bool gate_rejects(size_t extracted_len, const char *html, const char *extracted) {
   return (extracted_len < 2048) && !html_has_article_markers(html) &&
          (count_chromium_error_signatures(extracted) >= 2);
}

static void test_gate_rejects_canonical_error_page(void) {
   /* Real error pages: short, no article markers, multiple signatures. */
   const char *html = "<html><body>error</body></html>";
   const char *md = "# This site can't be reached\nDNS_PROBE_FINISHED_BAD_CONFIG\n"
                    "ERR_NAME_NOT_RESOLVED";
   TEST_ASSERT_TRUE(gate_rejects(200, html, md));
}

static void test_gate_keeps_long_legitimate_article(void) {
   /* Tech article about DNS errors: would have signatures but the article
    * itself is over 2 KB AND has article markup. */
   const char *html = "<html><article><h1>What is DNS_PROBE_FINISHED_BAD_CONFIG?</h1>"
                      "<p>...long content...</p></article></html>";
   const char *md = "Long article about DNS_PROBE_FINISHED_BAD_CONFIG and "
                    "ERR_NAME_NOT_RESOLVED ...";
   /* Length 5000 exceeds the 2048 gate, so even with markers absent we keep. */
   TEST_ASSERT_FALSE(gate_rejects(5000, html, md));
}

static void test_gate_keeps_short_article_with_markers(void) {
   /* Short page but has <article> markup → keep (it's a stub article, not
    * Chromium error). Two signatures present (e.g., glossary entry). h1
    * alone is no longer enough — we require `<article>` or `<main>`. */
   const char *html = "<article><h1>DNS errors</h1>"
                      "<p>DNS_PROBE_FINISHED_BAD_CONFIG, ERR_NAME_NOT_RESOLVED</p></article>";
   const char *md = "# DNS errors\nDNS_PROBE_FINISHED_BAD_CONFIG, ERR_NAME_NOT_RESOLVED";
   TEST_ASSERT_FALSE(gate_rejects(120, html, md));
}

static void test_gate_rejects_chromium_h1_error_page(void) {
   /* Regression for the 2026-05-13 Barron's bug. The Chromium error
    * template puts the headline in `<h1>`. Before the fix, this slipped
    * past the gate because h1 counted as article markup. After the fix,
    * only `<article>`/`<main>` count, so the gate correctly rejects. */
   const char *html = "<html><body><h1>This site can't be reached</h1>"
                      "<p>DNS_PROBE_FINISHED_BAD_CONFIG</p>"
                      "<p>Check your DNS settings</p></body></html>";
   const char *md = "# This site can't be reached\nDNS_PROBE_FINISHED_BAD_CONFIG\n"
                    "Check your DNS settings";
   TEST_ASSERT_TRUE(gate_rejects(150, html, md));
}

static void test_gate_keeps_single_signature_short_no_markers(void) {
   /* SEC-H2 anti-censorship arm: an adversarial page injecting ONE signature
    * to suppress its content should NOT trigger the gate. */
   const char *html = "<html><body>spam page</body></html>";
   const char *md = "Page mentioning DNS_PROBE_FINISHED_BAD_CONFIG only once.";
   TEST_ASSERT_FALSE(gate_rejects(70, html, md));
}

static void test_gate_keeps_normal_short_page(void) {
   /* A short page with no signatures should never reject. */
   const char *html = "<html><body>hi</body></html>";
   const char *md = "hi";
   TEST_ASSERT_FALSE(gate_rejects(2, html, md));
}

/* ── Entrypoint ──────────────────────────────────────────────────────────── */

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_count_null_returns_zero);
   RUN_TEST(test_count_empty_returns_zero);
   RUN_TEST(test_count_clean_article);
   RUN_TEST(test_count_one_signature);
   RUN_TEST(test_count_full_chromium_error_page);
   RUN_TEST(test_count_case_insensitive);

   RUN_TEST(test_markers_null);
   RUN_TEST(test_markers_empty);
   RUN_TEST(test_markers_with_article_tag);
   RUN_TEST(test_markers_with_main_tag);
   RUN_TEST(test_markers_h1_alone_does_not_count);
   RUN_TEST(test_markers_chromium_error_template_not_an_article);
   RUN_TEST(test_markers_case_insensitive);
   RUN_TEST(test_markers_only_chromium_error_page);

   RUN_TEST(test_gate_rejects_canonical_error_page);
   RUN_TEST(test_gate_rejects_chromium_h1_error_page);
   RUN_TEST(test_gate_keeps_long_legitimate_article);
   RUN_TEST(test_gate_keeps_short_article_with_markers);
   RUN_TEST(test_gate_keeps_single_signature_short_no_markers);
   RUN_TEST(test_gate_keeps_normal_short_page);
   return UNITY_END();
}
