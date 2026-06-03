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
 * Unit tests for messaging_format.c / messaging_format_telegram.c —
 * per-format rendering (plain / Discord / Slack / Telegram HTML), the
 * tag-stack + fence splitters, code-span holdout, escaping, the
 * cap-measured-on-converted-output property, and edge cases.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dawn_error.h"
#include "messaging/messaging_format.h"
#include "unity.h"

/* ---- helpers ----------------------------------------------------------- */

static char **g_parts = NULL;
static size_t g_count = 0;

void setUp(void) {
}
/* Runs after every test (pass OR fail).  Unity longjmps out of a failing
 * TEST_ASSERT before the in-body cleanup() runs, so free the parts here to
 * keep the leak-checked test binary clean on assertion failure. */
void tearDown(void) {
   messaging_format_free_parts(g_parts, g_count);
   g_parts = NULL;
   g_count = 0;
}

static int render(const char *md, messaging_format_t fmt, size_t cap) {
   messaging_format_free_parts(g_parts, g_count);
   g_parts = NULL;
   g_count = 0;
   char err[256] = { 0 };
   return messaging_format_render_split(md, fmt, cap, &g_parts, &g_count, err, sizeof(err));
}

static void cleanup(void) {
   messaging_format_free_parts(g_parts, g_count);
   g_parts = NULL;
   g_count = 0;
}

static size_t count_substr(const char *hay, const char *needle) {
   size_t n = 0;
   size_t nl = strlen(needle);
   for (const char *p = hay; (p = strstr(p, needle)) != NULL; p += nl) {
      n++;
   }
   return n;
}

/* ============================================================================
 * PLAIN (SMS)
 * ============================================================================ */

static void test_plain_strips_emphasis_and_flattens(void) {
   int rc = render("**bold** *italic* ~~strike~~ `code` and [link](https://ex.com)", MSG_FMT_PLAIN,
                   670);
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   TEST_ASSERT_EQUAL_size_t(1, g_count);
   TEST_ASSERT_NULL(strstr(g_parts[0], "**"));
   TEST_ASSERT_NULL(strstr(g_parts[0], "~~"));
   TEST_ASSERT_NULL(strstr(g_parts[0], "`"));
   TEST_ASSERT_NOT_NULL(strstr(g_parts[0], "bold"));
   TEST_ASSERT_NOT_NULL(strstr(g_parts[0], "code"));
   TEST_ASSERT_NOT_NULL(strstr(g_parts[0], "link (https://ex.com)"));
   cleanup();
}

static void test_plain_table_flattened(void) {
   int rc = render("| A | B |\n|---|---|\n| 1 | 2 |", MSG_FMT_PLAIN, 670);
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   TEST_ASSERT_NOT_NULL(strstr(g_parts[0], "A | B"));
   TEST_ASSERT_NOT_NULL(strstr(g_parts[0], "1 | 2"));
   TEST_ASSERT_NULL(strstr(g_parts[0], "---"));
   cleanup();
}

static void test_plain_no_bullet_glyph(void) {
   /* SMS must use "- ", never the • glyph (forces UCS-2). */
   int rc = render("- one\n- two", MSG_FMT_PLAIN, 670);
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   TEST_ASSERT_NULL(strstr(g_parts[0], "\xe2\x80\xa2")); /* U+2022 bullet */
   TEST_ASSERT_NOT_NULL(strstr(g_parts[0], "- one"));
   cleanup();
}

/* ============================================================================
 * DISCORD
 * ============================================================================ */

static void test_discord_keeps_markdown_flattens_table_and_link(void) {
   int rc = render("**bold** [t](https://e.com)\n\n| A | B |\n|---|---|\n| 1 | 2 |",
                   MSG_FMT_DISCORD, 1980);
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   TEST_ASSERT_NOT_NULL(strstr(g_parts[0], "**bold**"));          /* emphasis preserved */
   TEST_ASSERT_NOT_NULL(strstr(g_parts[0], "t (https://e.com)")); /* masked link flattened */
   TEST_ASSERT_NOT_NULL(strstr(g_parts[0], "```"));               /* table fenced */
   cleanup();
}

static void test_discord_header_capped_at_3(void) {
   int rc = render("#### Deep Heading", MSG_FMT_DISCORD, 1980);
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(0, strncmp(g_parts[0], "### Deep Heading", 16));
   TEST_ASSERT_NULL(strstr(g_parts[0], "####"));
   cleanup();
}

/* ============================================================================
 * SLACK mrkdwn
 * ============================================================================ */

static void test_slack_dialect_conversion(void) {
   int rc = render("**bold** *italic* ~~strike~~ [t](https://e.com)", MSG_FMT_SLACK_MRKDWN, 3980);
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   TEST_ASSERT_NOT_NULL(strstr(g_parts[0], "*bold*"));   /* ** -> * */
   TEST_ASSERT_NOT_NULL(strstr(g_parts[0], "_italic_")); /* * -> _ */
   TEST_ASSERT_NOT_NULL(strstr(g_parts[0], "~strike~")); /* ~~ -> ~ */
   TEST_ASSERT_NULL(strstr(g_parts[0], "**"));
   TEST_ASSERT_NULL(strstr(g_parts[0], "~~"));
   TEST_ASSERT_NOT_NULL(strstr(g_parts[0], "<https://e.com|t>")); /* link form */
   cleanup();
}

static void test_slack_header_becomes_bold(void) {
   int rc = render("# Heading", MSG_FMT_SLACK_MRKDWN, 3980);
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   TEST_ASSERT_NOT_NULL(strstr(g_parts[0], "*Heading*"));
   TEST_ASSERT_NULL(strstr(g_parts[0], "#"));
   cleanup();
}

static void test_slack_escapes_and_preserves_link_delims(void) {
   int rc = render("a < b & c [x](https://e.com/?p=1&q=2)", MSG_FMT_SLACK_MRKDWN, 3980);
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   TEST_ASSERT_NOT_NULL(strstr(g_parts[0], "&lt;"));
   TEST_ASSERT_NOT_NULL(strstr(g_parts[0], "&amp;"));
   /* link delimiters < | > are NOT escaped */
   TEST_ASSERT_NOT_NULL(strstr(g_parts[0], "<https://e.com/?p=1&q=2|x>"));
   cleanup();
}

/* A | / < / > in a link URL must not break out of the <url|text> grammar and
 * inject Slack control sequences. */
static void test_slack_link_url_no_breakout(void) {
   int rc = render("[hi](x|<!channel>)", MSG_FMT_SLACK_MRKDWN, 3980);
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   /* URL contains a pipe -> drop to bare-label form, no <!channel> emitted */
   TEST_ASSERT_NULL(strstr(g_parts[0], "<!channel>"));
   cleanup();
   /* angle brackets in URL get entity-escaped, not passed through */
   rc = render("[hi](http://e.com/<x>)", MSG_FMT_SLACK_MRKDWN, 3980);
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   TEST_ASSERT_NULL(strstr(g_parts[0], "/<x>"));
   TEST_ASSERT_NOT_NULL(strstr(g_parts[0], "&lt;x&gt;"));
   cleanup();
}

static void test_slack_no_double_escape(void) {
   int rc = render("a & b", MSG_FMT_SLACK_MRKDWN, 3980);
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   TEST_ASSERT_NOT_NULL(strstr(g_parts[0], "a &amp; b"));
   TEST_ASSERT_NULL(strstr(g_parts[0], "&amp;amp;"));
   cleanup();
}

/* ============================================================================
 * TELEGRAM HTML
 * ============================================================================ */

static void test_telegram_basic_html(void) {
   int rc = render("**bold** *italic* `co<de>` [t](https://e.com/?a=1&b=2)", MSG_FMT_TELEGRAM_HTML,
                   4076);
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   TEST_ASSERT_NOT_NULL(strstr(g_parts[0], "<b>bold</b>"));
   TEST_ASSERT_NOT_NULL(strstr(g_parts[0], "<i>italic</i>"));
   TEST_ASSERT_NOT_NULL(strstr(g_parts[0], "<code>co&lt;de&gt;</code>")); /* interior escaped */
   TEST_ASSERT_NOT_NULL(strstr(g_parts[0], "<a href=\"https://e.com/?a=1&amp;b=2\">t</a>"));
   cleanup();
}

static void test_telegram_header_bold_underline(void) {
   /* Telegram has no heading tag — headers render bold+underline so they're
    * distinct from inline bold. */
   int rc = render("# Heading\n\nbody", MSG_FMT_TELEGRAM_HTML, 4076);
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   TEST_ASSERT_NOT_NULL(strstr(g_parts[0], "<b><u>Heading</u></b>"));
   cleanup();
}

static void test_telegram_code_fence_language(void) {
   int rc = render("```python\nx = '<a> & <b>'\n```", MSG_FMT_TELEGRAM_HTML, 4076);
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   TEST_ASSERT_NOT_NULL(strstr(g_parts[0], "<pre><code class=\"language-python\">"));
   TEST_ASSERT_NOT_NULL(strstr(g_parts[0], "&lt;a&gt; &amp; &lt;b&gt;"));
   TEST_ASSERT_NOT_NULL(strstr(g_parts[0], "</code></pre>"));
   cleanup();
}

/* A double-quote in a code-fence info string must not break out of the
 * class="language-..." attribute (would get the whole message rejected). */
static void test_telegram_lang_attr_quote_escaped(void) {
   int rc = render("```x\" onload=\"y\ncode\n```", MSG_FMT_TELEGRAM_HTML, 4076);
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   TEST_ASSERT_NOT_NULL(strstr(g_parts[0], "&quot;"));
   /* the raw attribute-breaking sequence must NOT survive */
   TEST_ASSERT_NULL(strstr(g_parts[0], "language-x\" onload"));
   cleanup();
}

/* Each Telegram part must be independently tag-balanced (Telegram rejects the
 * whole message otherwise).  Force a long bold run across a tiny cap. */
static void test_telegram_split_balances_tags(void) {
   int rc = render("**a very long bold run that has to be split across several parts here**",
                   MSG_FMT_TELEGRAM_HTML, 16);
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   TEST_ASSERT_TRUE(g_count > 1);
   for (size_t i = 0; i < g_count; i++) {
      TEST_ASSERT_EQUAL_size_t_MESSAGE(count_substr(g_parts[i], "<b>"),
                                       count_substr(g_parts[i], "</b>"),
                                       "each part has balanced <b>/</b>");
   }
   cleanup();
}

/* A <pre> block larger than the cap is closed and reopened per part. */
static void test_telegram_split_pre_balanced(void) {
   int rc = render("```\nAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\n```", MSG_FMT_TELEGRAM_HTML, 20);
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   TEST_ASSERT_TRUE(g_count > 1);
   for (size_t i = 0; i < g_count; i++) {
      TEST_ASSERT_EQUAL_size_t(count_substr(g_parts[i], "<pre>"),
                               count_substr(g_parts[i], "</pre>"));
   }
   cleanup();
}

/* Visible length (not raw HTML bytes) drives the cap: a short message with
 * many entities stays a single part even though its byte length exceeds the
 * cap. */
static void test_telegram_cap_is_visible_not_bytes(void) {
   /* "a & b & c & d & e" = 17 visible chars; escaped bytes ~ 33. */
   int rc = render("a & b & c & d & e", MSG_FMT_TELEGRAM_HTML, 20);
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   TEST_ASSERT_EQUAL_size_t(1, g_count);
   TEST_ASSERT_TRUE(strlen(g_parts[0]) > 20); /* bytes exceed cap, visible does not */
   cleanup();
}

/* ============================================================================
 * Cap-after-conversion (the reviewed property)
 * ============================================================================ */

static void test_cap_expansion_forces_split(void) {
   /* Raw "a & b & c & d & e & f" is 21 bytes; Slack-escaped it is longer and
    * must split when the cap is below the CONVERTED length. */
   int rc = render("a & b & c & d & e & f", MSG_FMT_SLACK_MRKDWN, 12);
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   TEST_ASSERT_TRUE(g_count >= 2);
   for (size_t i = 0; i < g_count; i++) {
      TEST_ASSERT_TRUE(strlen(g_parts[i]) <= 12);
   }
   cleanup();
}

static void test_cap_shrink_single_part(void) {
   /* Markdown-heavy body that exceeds the cap raw but strips well under it. */
   const char *md = "**bold** _and_ `code` ~~strike~~ here";
   TEST_ASSERT_TRUE(strlen(md) > 30);
   int rc = render(md, MSG_FMT_PLAIN, 30);
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   TEST_ASSERT_EQUAL_size_t(1, g_count);
   TEST_ASSERT_TRUE(strlen(g_parts[0]) <= 30);
   cleanup();
}

/* ============================================================================
 * Edge cases
 * ============================================================================ */

static void test_empty_yields_placeholder(void) {
   int rc = render("", MSG_FMT_DISCORD, 1980);
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   TEST_ASSERT_EQUAL_size_t(1, g_count);
   TEST_ASSERT_TRUE(strlen(g_parts[0]) > 0);
   cleanup();
}

static void test_table_only_not_empty(void) {
   /* A message that is only a table must still produce a non-empty part. */
   int rc = render("| A | B |\n|---|---|\n| 1 | 2 |", MSG_FMT_SLACK_MRKDWN, 3980);
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   TEST_ASSERT_EQUAL_size_t(1, g_count);
   TEST_ASSERT_TRUE(strlen(g_parts[0]) > 0);
   cleanup();
}

static void test_deep_nesting_no_crash(void) {
   /* Pathological emphasis nesting must not overflow the stack. */
   char buf[256];
   size_t i = 0;
   for (; i < 60; i++) {
      buf[i] = '*';
   }
   const char *word = "x";
   memcpy(buf + i, word, 1);
   i += 1;
   for (size_t k = 0; k < 60; k++) {
      buf[i++] = '*';
   }
   buf[i] = '\0';
   int rc = render(buf, MSG_FMT_TELEGRAM_HTML, 4076);
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   TEST_ASSERT_TRUE(g_count >= 1);
   cleanup();
}

static void test_code_span_holdout(void) {
   /* Emphasis markers inside a code span are NOT interpreted. */
   int rc = render("`**not bold**`", MSG_FMT_DISCORD, 1980);
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   TEST_ASSERT_NOT_NULL(strstr(g_parts[0], "**not bold**"));
   cleanup();
}

static void test_invalid_args(void) {
   char **p = NULL;
   size_t c = 0;
   char err[64] = { 0 };
   TEST_ASSERT_EQUAL_INT(FAILURE, messaging_format_render_split(NULL, MSG_FMT_PLAIN, 100, &p, &c,
                                                                err, sizeof(err)));
   TEST_ASSERT_EQUAL_INT(FAILURE, messaging_format_render_split("hi", MSG_FMT_PLAIN, 0, &p, &c, err,
                                                                sizeof(err)));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_plain_strips_emphasis_and_flattens);
   RUN_TEST(test_plain_table_flattened);
   RUN_TEST(test_plain_no_bullet_glyph);
   RUN_TEST(test_discord_keeps_markdown_flattens_table_and_link);
   RUN_TEST(test_discord_header_capped_at_3);
   RUN_TEST(test_slack_dialect_conversion);
   RUN_TEST(test_slack_header_becomes_bold);
   RUN_TEST(test_slack_escapes_and_preserves_link_delims);
   RUN_TEST(test_slack_link_url_no_breakout);
   RUN_TEST(test_slack_no_double_escape);
   RUN_TEST(test_telegram_basic_html);
   RUN_TEST(test_telegram_header_bold_underline);
   RUN_TEST(test_telegram_code_fence_language);
   RUN_TEST(test_telegram_lang_attr_quote_escaped);
   RUN_TEST(test_telegram_split_balances_tags);
   RUN_TEST(test_telegram_split_pre_balanced);
   RUN_TEST(test_telegram_cap_is_visible_not_bytes);
   RUN_TEST(test_cap_expansion_forces_split);
   RUN_TEST(test_cap_shrink_single_part);
   RUN_TEST(test_empty_yields_placeholder);
   RUN_TEST(test_table_only_not_empty);
   RUN_TEST(test_deep_nesting_no_crash);
   RUN_TEST(test_code_span_holdout);
   RUN_TEST(test_invalid_args);
   return UNITY_END();
}
