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
 * Unit tests for the growable strbuf utility.  Verifies the doubling
 * realloc, max_cap safety, sticky OOM behavior, and steal contract — the
 * properties that the memory_callback code now relies on.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "core/strbuf.h"
#include "unity.h"

void setUp(void) {
}
void tearDown(void) {
}

/* === Init / free === */

void test_init_lazy_no_alloc(void) {
   strbuf_t sb;
   strbuf_init(&sb, 0);
   TEST_ASSERT_NULL(sb.buf);
   TEST_ASSERT_EQUAL_UINT(0, sb.len);
   TEST_ASSERT_EQUAL_UINT(0, sb.cap);
   TEST_ASSERT_FALSE(sb.oom);
   /* strbuf_str on never-written returns "" sentinel, not NULL */
   TEST_ASSERT_EQUAL_STRING("", strbuf_str(&sb));
   strbuf_free(&sb);
}

void test_init_with_initial_cap(void) {
   strbuf_t sb;
   strbuf_init(&sb, 256);
   TEST_ASSERT_NOT_NULL(sb.buf);
   TEST_ASSERT_TRUE(sb.cap >= 256);
   TEST_ASSERT_EQUAL_UINT(0, sb.len);
   TEST_ASSERT_FALSE(sb.oom);
   strbuf_free(&sb);
}

void test_free_is_idempotent(void) {
   strbuf_t sb;
   strbuf_init(&sb, 32);
   strbuf_free(&sb);
   strbuf_free(&sb); /* must not crash */
   TEST_ASSERT_NULL(sb.buf);
}

/* === Append === */

void test_appendf_basic(void) {
   strbuf_t sb;
   strbuf_init(&sb, 0);
   int n = strbuf_appendf(&sb, "hello %s, age %d", "world", 42);
   TEST_ASSERT_EQUAL_INT((int)strlen("hello world, age 42"), n);
   TEST_ASSERT_EQUAL_STRING("hello world, age 42", strbuf_str(&sb));
   strbuf_free(&sb);
}

void test_append_basic(void) {
   strbuf_t sb;
   strbuf_init(&sb, 0);
   strbuf_append(&sb, "abc");
   strbuf_append(&sb, "def");
   TEST_ASSERT_EQUAL_STRING("abcdef", strbuf_str(&sb));
   TEST_ASSERT_EQUAL_UINT(6, strbuf_len(&sb));
   strbuf_free(&sb);
}

void test_append_n_with_embedded_data(void) {
   strbuf_t sb;
   strbuf_init(&sb, 0);
   /* Append exactly 5 bytes regardless of NUL position */
   strbuf_append_n(&sb, "abc\0d", 5);
   TEST_ASSERT_EQUAL_UINT(5, strbuf_len(&sb));
   /* Buffer is still NUL-terminated past the data */
   TEST_ASSERT_EQUAL_INT('\0', sb.buf[5]);
   strbuf_free(&sb);
}

/* === Growth === */

void test_growth_doubles_to_fit(void) {
   strbuf_t sb;
   strbuf_init(&sb, 64);
   size_t initial_cap = sb.cap;
   /* Append more than initial_cap bytes; must grow without truncation */
   char big[1024];
   memset(big, 'x', sizeof(big) - 1);
   big[sizeof(big) - 1] = '\0';
   strbuf_append(&sb, big);
   TEST_ASSERT_EQUAL_UINT(sizeof(big) - 1, strbuf_len(&sb));
   TEST_ASSERT_TRUE(sb.cap > initial_cap);
   TEST_ASSERT_FALSE(strbuf_oom(&sb));
   /* Last byte should be 'x', no truncation */
   TEST_ASSERT_EQUAL_INT('x', sb.buf[sizeof(big) - 2]);
   strbuf_free(&sb);
}

void test_growth_handles_large_appendf(void) {
   strbuf_t sb;
   strbuf_init(&sb, 16); /* deliberately tiny initial */
   /* Single appendf produces a string ~3KB long */
   strbuf_appendf(&sb, "%0500d%0500d%0500d%0500d%0500d%0500d", 0, 0, 0, 0, 0, 0);
   TEST_ASSERT_EQUAL_UINT(3000, strbuf_len(&sb));
   TEST_ASSERT_FALSE(strbuf_oom(&sb));
   /* All zeros; no garbage */
   for (size_t i = 0; i < strbuf_len(&sb); i++)
      TEST_ASSERT_EQUAL_INT('0', sb.buf[i]);
   strbuf_free(&sb);
}

/* === max_cap / OOM === */

void test_max_cap_blocks_growth(void) {
   strbuf_t sb;
   strbuf_init_with_max(&sb, 0, 100); /* hard cap 100 bytes */
   char big[256];
   memset(big, 'y', sizeof(big) - 1);
   big[sizeof(big) - 1] = '\0';
   int n = strbuf_append(&sb, big);
   TEST_ASSERT_EQUAL_INT(STRBUF_E_OOM, n); /* refused */
   TEST_ASSERT_TRUE(strbuf_oom(&sb));
   strbuf_free(&sb);
}

void test_oom_is_sticky(void) {
   strbuf_t sb;
   strbuf_init_with_max(&sb, 0, 50);
   strbuf_append(&sb, "this is more than fifty bytes worth of data, "
                      "definitely overflowing the small max cap.");
   TEST_ASSERT_TRUE(strbuf_oom(&sb));
   /* Subsequent appends are no-ops, do not crash, return STRBUF_E_OOM */
   TEST_ASSERT_EQUAL_INT(STRBUF_E_OOM, strbuf_append(&sb, "more"));
   TEST_ASSERT_EQUAL_INT(STRBUF_E_OOM, strbuf_appendf(&sb, "%d", 42));
   TEST_ASSERT_TRUE(strbuf_oom(&sb));
   strbuf_free(&sb);
}

void test_appendf_after_oom_returns_oom_sentinel(void) {
   strbuf_t sb;
   strbuf_init_with_max(&sb, 0, 16);
   strbuf_appendf(&sb, "this exceeds sixteen bytes");
   if (strbuf_oom(&sb)) {
      TEST_ASSERT_EQUAL_INT(STRBUF_E_OOM, strbuf_appendf(&sb, "anything"));
   }
   strbuf_free(&sb);
}

void test_appendf_legitimate_empty_returns_zero(void) {
   /* Per the new contract, 0 is reserved for legitimate empty appends.
    * STRBUF_E_OOM signals OOM exclusively. */
   strbuf_t sb;
   strbuf_init(&sb, 0);
   int n = strbuf_appendf(&sb, "%s", "");
   TEST_ASSERT_EQUAL_INT(0, n); /* not STRBUF_E_OOM */
   TEST_ASSERT_FALSE(strbuf_oom(&sb));
   /* Buffer is NUL-terminated and empty */
   TEST_ASSERT_EQUAL_UINT(0, strbuf_len(&sb));
   strbuf_free(&sb);
}

void test_append_legitimate_empty_returns_zero(void) {
   strbuf_t sb;
   strbuf_init(&sb, 0);
   TEST_ASSERT_EQUAL_INT(0, strbuf_append(&sb, ""));
   TEST_ASSERT_EQUAL_INT(0, strbuf_append_n(&sb, "anything", 0));
   TEST_ASSERT_FALSE(strbuf_oom(&sb));
   strbuf_free(&sb);
}

/* === Steal === */

void test_steal_transfers_ownership(void) {
   strbuf_t sb;
   strbuf_init(&sb, 0);
   strbuf_appendf(&sb, "transfer me");
   char *out = strbuf_steal(&sb);
   TEST_ASSERT_NOT_NULL(out);
   TEST_ASSERT_EQUAL_STRING("transfer me", out);
   /* sb is reset */
   TEST_ASSERT_NULL(sb.buf);
   TEST_ASSERT_EQUAL_UINT(0, sb.len);
   TEST_ASSERT_EQUAL_UINT(0, sb.cap);
   free(out);
}

void test_steal_after_oom_returns_null(void) {
   strbuf_t sb;
   strbuf_init_with_max(&sb, 0, 16);
   strbuf_appendf(&sb, "this is way more than sixteen bytes worth of data");
   TEST_ASSERT_TRUE(strbuf_oom(&sb));
   char *out = strbuf_steal(&sb);
   TEST_ASSERT_NULL(out);
   strbuf_free(&sb); /* still safe */
}

void test_steal_on_empty_returns_empty_string(void) {
   /* New contract: strbuf_steal returns strdup("") on the empty-buffer
    * case — caller treats it like any other success.  strbuf_steal_or_null
    * is the legacy variant that returns NULL on empty (kept for explicit
    * empty-vs-success-distinguishing callers). */
   strbuf_t sb;
   strbuf_init(&sb, 0);
   char *out = strbuf_steal(&sb);
   TEST_ASSERT_NOT_NULL(out);
   TEST_ASSERT_EQUAL_STRING("", out);
   free(out);

   strbuf_t sb2;
   strbuf_init(&sb2, 0);
   char *out2 = strbuf_steal_or_null(&sb2);
   TEST_ASSERT_NULL(out2);
   strbuf_free(&sb2);
}

void test_steal_or_null_after_oom_returns_null(void) {
   strbuf_t sb;
   strbuf_init_with_max(&sb, 0, 16);
   strbuf_appendf(&sb, "this is way more than sixteen bytes worth of data");
   TEST_ASSERT_TRUE(strbuf_oom(&sb));
   TEST_ASSERT_NULL(strbuf_steal(&sb));         /* OOM → NULL on either API */
   TEST_ASSERT_NULL(strbuf_steal_or_null(&sb)); /* OOM → NULL */
   strbuf_free(&sb);
}

/* === Realistic usage: build a multi-section response (mirrors memory_callback) === */

void test_realistic_memory_response_builds_without_truncation(void) {
   strbuf_t sb;
   strbuf_init(&sb, 8192); /* same initial as memory_action_search */

   /* Header */
   strbuf_appendf(&sb, "FACTS (10):\n");
   /* 10 facts each ~100 chars + a 6KB source excerpt block (mimics
    * source_budget=6144 with 6 facts source-augmented at ~1KB each).
    * Total: ~7KB, well within an 8KB initial.  But then add ~5KB of
    * preferences + summaries + entity graph to push past 8KB. */
   for (int i = 0; i < 10; i++) {
      strbuf_appendf(&sb,
                     "- [ID:%d] fact text here at length about 80 characters "
                     "covering typical content (confidence: 95%%, 2 days ago)\n",
                     i);
   }
   /* Simulate source excerpts totaling ~6KB */
   for (int i = 0; i < 6; i++) {
      strbuf_appendf(&sb, "[source: conv=1 msgs=%d-%d]\n", i, i + 10);
      char filler[1024];
      memset(filler, 'm', sizeof(filler) - 1);
      filler[sizeof(filler) - 1] = '\0';
      strbuf_appendf(&sb, "user: %.500s\nassistant: %.500s\n", filler, filler);
   }
   strbuf_appendf(&sb, "\nPREFERENCES:\n");
   for (int i = 0; i < 8; i++)
      strbuf_appendf(&sb, "- pref_%d: value_%d (reinforced %d times)\n", i, i, i);
   strbuf_appendf(&sb, "\nCONVERSATION SUMMARIES (5):\n");
   for (int i = 0; i < 5; i++)
      strbuf_appendf(&sb,
                     "- [N days ago] summary text approximately 200 chars long "
                     "covering some conversation topics %d\n  Topics: t1, t2, t3\n",
                     i);
   strbuf_appendf(&sb, "\nENTITIES:\n");
   for (int i = 0; i < 5; i++) {
      strbuf_appendf(&sb, "- entity_%d (person):\n", i);
      for (int r = 0; r < 4; r++)
         strbuf_appendf(&sb, "  rel_%d: object_%d\n", r, r);
   }

   /* Final size should be ~10-13KB — would have silently truncated past 8KB
    * with the old fixed buffer.  strbuf grows; OOM stays false. */
   TEST_ASSERT_FALSE(strbuf_oom(&sb));
   TEST_ASSERT_TRUE(strbuf_len(&sb) > 8192);
   /* Final character of the last entity line should be '\n' — no mid-line truncation. */
   TEST_ASSERT_EQUAL_INT('\n', sb.buf[strbuf_len(&sb) - 1]);

   char *out = strbuf_steal(&sb);
   TEST_ASSERT_NOT_NULL(out);
   /* Spot-check: the LAST section header is present (would be missing if truncated) */
   TEST_ASSERT_NOT_NULL(strstr(out, "ENTITIES:"));
   /* Spot-check: the LAST entity entry is present */
   TEST_ASSERT_NOT_NULL(strstr(out, "entity_4 (person):"));
   free(out);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_init_lazy_no_alloc);
   RUN_TEST(test_init_with_initial_cap);
   RUN_TEST(test_free_is_idempotent);
   RUN_TEST(test_appendf_basic);
   RUN_TEST(test_append_basic);
   RUN_TEST(test_append_n_with_embedded_data);
   RUN_TEST(test_growth_doubles_to_fit);
   RUN_TEST(test_growth_handles_large_appendf);
   RUN_TEST(test_max_cap_blocks_growth);
   RUN_TEST(test_oom_is_sticky);
   RUN_TEST(test_appendf_after_oom_returns_oom_sentinel);
   RUN_TEST(test_appendf_legitimate_empty_returns_zero);
   RUN_TEST(test_append_legitimate_empty_returns_zero);
   RUN_TEST(test_steal_transfers_ownership);
   RUN_TEST(test_steal_after_oom_returns_null);
   RUN_TEST(test_steal_on_empty_returns_empty_string);
   RUN_TEST(test_steal_or_null_after_oom_returns_null);
   RUN_TEST(test_realistic_memory_response_builds_without_truncation);
   return UNITY_END();
}
