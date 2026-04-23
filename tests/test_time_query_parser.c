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
 * Unit tests for time_query_parser.  Pure module — no DB or harness needed.
 */

#define _GNU_SOURCE

#include <math.h>
#include <stdio.h>
#include <stdlib.h> /* llabs */
#include <string.h>
#include <time.h>

#include "tools/time_query_parser.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg)         \
   do {                                \
      if (cond) {                      \
         printf("  [PASS] %s\n", msg); \
         tests_passed++;               \
      } else {                         \
         printf("  [FAIL] %s\n", msg); \
         tests_failed++;               \
      }                                \
   } while (0)

/* Pinned "now" = 2024-04-15 12:00 UTC for relative-expression tests.
 * Noon matches make_ts() in the parser — keeps the same calendar day for
 * any TZ from UTC-11 to UTC+11. */
static int64_t pin_now(void) {
   struct tm tm = { 0 };
   tm.tm_year = 2024 - 1900;
   tm.tm_mon = 3; /* April */
   tm.tm_mday = 15;
   tm.tm_hour = 12;
   return (int64_t)timegm(&tm);
}

static int64_t make_ref(int year, int month0, int day) {
   struct tm tm = { 0 };
   tm.tm_year = year - 1900;
   tm.tm_mon = month0;
   tm.tm_mday = day;
   tm.tm_hour = 12;
   return (int64_t)timegm(&tm);
}

/* ============================================================================
 * Absolute date recognition
 * ============================================================================ */

static void test_month_year(void) {
   printf("\n--- test_month_year ---\n");

   time_query_t tq;
   time_query_parse("Was the first half of September 2022 a good month for Nate?", pin_now(), &tq);
   TEST_ASSERT(tq.found, "September 2022 → found");
   TEST_ASSERT(tq.kind == TQP_ABSOLUTE, "September 2022 → ABSOLUTE");
   int64_t expected = make_ref(2022, 8, 15);
   TEST_ASSERT(llabs(tq.target_ts - expected) < 86400, "September 2022 → mid-Sept 2022");

   time_query_parse("In May 2020 we moved", pin_now(), &tq);
   TEST_ASSERT(tq.found && tq.kind == TQP_ABSOLUTE, "May 2020 → ABSOLUTE");

   time_query_parse("on January 5 2019", pin_now(), &tq);
   TEST_ASSERT(tq.found, "January 2019 → found via abbrev path");
}

static void test_season_year(void) {
   printf("\n--- test_season_year ---\n");

   time_query_t tq;
   time_query_parse("What state did Joanna visit in summer 2021?", pin_now(), &tq);
   TEST_ASSERT(tq.found, "summer 2021 → found");
   TEST_ASSERT(tq.kind == TQP_ABSOLUTE, "summer 2021 → ABSOLUTE");
   int64_t july_2021 = make_ref(2021, 6, 15); /* July midpoint */
   TEST_ASSERT(llabs(tq.target_ts - july_2021) < 30 * 86400, "summer 2021 → mid-summer 2021");

   time_query_parse("the winter 2020 storm", pin_now(), &tq);
   TEST_ASSERT(tq.found, "winter 2020 → found");
   /* Winter 2020 = Dec 2020 - Feb 2021, midpoint = mid-January 2021 */
   int64_t jan_2021 = make_ref(2021, 0, 15);
   TEST_ASSERT(llabs(tq.target_ts - jan_2021) < 30 * 86400, "winter 2020 → mid-January 2021");

   time_query_parse("during fall 2022", pin_now(), &tq);
   TEST_ASSERT(tq.found, "fall 2022 → found");
}

static void test_lone_year(void) {
   printf("\n--- test_lone_year ---\n");

   time_query_t tq;
   time_query_parse("things that happened in 2020", pin_now(), &tq);
   TEST_ASSERT(tq.found && tq.kind == TQP_ABSOLUTE, "in 2020 → ABSOLUTE");

   time_query_parse("plans for 2025", pin_now(), &tq);
   TEST_ASSERT(tq.found, "for 2025 → found (future)");

   /* Year inside a longer number should NOT match. */
   time_query_parse("call 12022 if you need help", pin_now(), &tq);
   TEST_ASSERT(!tq.found, "12022 inside number → no match");

   time_query_parse("conv id 20210", pin_now(), &tq);
   TEST_ASSERT(!tq.found, "20210 (5-digit) → no match");

   /* Out-of-range years should not match. */
   time_query_parse("dating from 1850", pin_now(), &tq);
   TEST_ASSERT(!tq.found, "1850 (pre-1900) → no match");
}

/* ============================================================================
 * Relative expressions (anchored to pinned now)
 * ============================================================================ */

static void test_yesterday(void) {
   printf("\n--- test_yesterday ---\n");

   time_query_t tq;
   int64_t now = pin_now();
   time_query_parse("what did we discuss yesterday", now, &tq);
   TEST_ASSERT(tq.found, "yesterday → found");
   TEST_ASSERT(tq.kind == TQP_RELATIVE, "yesterday → RELATIVE");
   TEST_ASSERT(llabs(tq.target_ts - (now - 86400)) < 60, "yesterday → now - 1d");
}

static void test_last_unit(void) {
   printf("\n--- test_last_unit ---\n");

   time_query_t tq;
   int64_t now = pin_now();
   time_query_parse("did anything happen last week", now, &tq);
   TEST_ASSERT(tq.found && tq.kind == TQP_RELATIVE, "last week → RELATIVE");
   TEST_ASSERT(llabs(tq.target_ts - (now - 7 * 86400)) < 60, "last week → now - 7d");

   time_query_parse("a month ago I started", now, &tq);
   TEST_ASSERT(tq.found, "a month ago → found");
   TEST_ASSERT(llabs(tq.target_ts - (now - 30 * 86400)) < 60, "a month ago → now - 30d");

   time_query_parse("last year was rough", now, &tq);
   TEST_ASSERT(tq.found, "last year → found");
}

static void test_today(void) {
   printf("\n--- test_today ---\n");

   time_query_t tq;
   int64_t now = pin_now();
   time_query_parse("what did I say today", now, &tq);
   TEST_ASSERT(tq.found, "today → found");
   TEST_ASSERT(llabs(tq.target_ts - now) < 60, "today → now");
}

static void test_vague(void) {
   printf("\n--- test_vague ---\n");

   time_query_t tq;
   int64_t now = pin_now();
   time_query_parse("we talked recently about lunch", now, &tq);
   TEST_ASSERT(tq.found, "recently → found");
   TEST_ASSERT(tq.kind == TQP_VAGUE, "recently → VAGUE");
}

static void test_no_match(void) {
   printf("\n--- test_no_match ---\n");

   time_query_t tq;
   time_query_parse("what's the weather like", pin_now(), &tq);
   TEST_ASSERT(!tq.found, "no temporal expression → not found");

   time_query_parse("", pin_now(), &tq);
   TEST_ASSERT(!tq.found, "empty query → not found");

   /* Word boundary: "fall" inside "fallback" must not match. */
   time_query_parse("we need a fallback for 2099", pin_now(), &tq);
   /* The "2099" should still match. */
   TEST_ASSERT(tq.found && strcmp(tq.matched, "2099") == 0,
               "fallback 2099 → matches 2099, not 'fall'");
}

/* ============================================================================
 * Proximity scoring
 * ============================================================================ */

static void test_proximity(void) {
   printf("\n--- test_proximity ---\n");

   time_query_t tq;
   time_query_parse("September 2022", pin_now(), &tq);

   /* Right on the target — score = 1.0 */
   float p = time_query_proximity(&tq, tq.target_ts);
   TEST_ASSERT(fabsf(p - 1.0f) < 0.01f, "distance 0 → score ≈ 1.0");

   /* One window away — score ≈ exp(-0.5) ≈ 0.61 */
   p = time_query_proximity(&tq, tq.target_ts + tq.window_seconds);
   TEST_ASSERT(fabsf(p - 0.6065f) < 0.05f, "distance 1w → score ≈ 0.61");

   /* Far away — score ≈ 0 */
   p = time_query_proximity(&tq, tq.target_ts + 5 * tq.window_seconds);
   TEST_ASSERT(p < 0.01f, "distance 5w → score ≈ 0");

   /* No temporal query → score 0 */
   time_query_t empty = { 0 };
   p = time_query_proximity(&empty, tq.target_ts);
   TEST_ASSERT(p == 0.0f, "no temporal query → score 0");

   /* No chunk timestamp → score 0 */
   p = time_query_proximity(&tq, 0);
   TEST_ASSERT(p == 0.0f, "chunk_ts = 0 → score 0");
}

/* ============================================================================
 * N units ago (digits + words)
 * ============================================================================ */

static void test_n_units_ago(void) {
   printf("\n--- test_n_units_ago ---\n");

   time_query_t tq;
   int64_t now = pin_now();

   time_query_parse("how many days ago did I attend the service", now, &tq);
   /* "how many days/weeks/months ago" with no explicit number — handled by
    * try_how_many_ago as a recency hint (target = now - 14d, window = 60d).
    * Without this, structural failures dominate the "how many" subset. */
   TEST_ASSERT(tq.found && tq.kind == TQP_RELATIVE,
               "'how many days ago' without N → recency anchor");

   time_query_parse("5 days ago I went hiking", now, &tq);
   TEST_ASSERT(tq.found && tq.kind == TQP_RELATIVE, "5 days ago → RELATIVE");
   TEST_ASSERT(llabs(tq.target_ts - (now - 5 * 86400)) < 60, "5 days ago → now - 5d");

   time_query_parse("two weeks ago I bought a guitar", now, &tq);
   TEST_ASSERT(tq.found, "two weeks ago → found");
   TEST_ASSERT(llabs(tq.target_ts - (now - 14 * 86400)) < 60, "two weeks ago → now - 14d");

   time_query_parse("in the past 30 days I started running", now, &tq);
   TEST_ASSERT(tq.found, "in the past 30 days → found");

   time_query_parse("3 months ago I moved", now, &tq);
   TEST_ASSERT(tq.found, "3 months ago → found");
   TEST_ASSERT(llabs(tq.target_ts - (now - 90 * 86400)) < 86400, "3 months ago → ~now - 90d");
}

/* ============================================================================
 * Bare month name (no year)
 * ============================================================================ */

static void test_bare_month(void) {
   printf("\n--- test_bare_month ---\n");

   time_query_t tq;
   int64_t now = pin_now(); /* April 15, 2024 */

   time_query_parse("what did I do in September", now, &tq);
   TEST_ASSERT(tq.found, "in September → found (most-recent)");
   /* September 2023 (most recent past September from April 2024) */
   int64_t sept_2023 = make_ref(2023, 8, 15);
   TEST_ASSERT(llabs(tq.target_ts - sept_2023) < 86400, "in September → mid-Sept 2023");

   time_query_parse("what about June plans", now, &tq);
   /* June 2023 (last completed June from April 2024) */
   int64_t june_2023 = make_ref(2023, 5, 15);
   TEST_ASSERT(tq.found && llabs(tq.target_ts - june_2023) < 86400,
               "bare June → June 2023 (most recent)");

   /* "may" as bare match is intentionally skipped (verb collision). */
   time_query_parse("I may visit Paris", now, &tq);
   TEST_ASSERT(!tq.found, "bare 'may' → not matched (verb)");
}

/* ============================================================================
 * LoCoMo cat-3 sample queries (from real dataset)
 * ============================================================================ */

static void test_locomo_cat3_samples(void) {
   printf("\n--- test_locomo_cat3_samples ---\n");

   time_query_t tq;

   /* From LoCoMo: "What state did Joanna visit in summer 2021?" */
   time_query_parse("What state did Joanna visit in summer 2021?", pin_now(), &tq);
   TEST_ASSERT(tq.found && tq.kind == TQP_ABSOLUTE, "Joanna summer 2021 → ABSOLUTE");

   /* "Was the first half of September 2022 a good month career-wise for Nate and Joanna?" */
   time_query_parse(
       "Was the first half of September 2022 a good month career-wise for Nate and Joanna?",
       pin_now(), &tq);
   TEST_ASSERT(tq.found && tq.kind == TQP_ABSOLUTE, "Sept 2022 career → ABSOLUTE");

   /* "Which US states might Tim be in during September 2023?" */
   time_query_parse("Which US states might Tim be in during September 2023?", pin_now(), &tq);
   TEST_ASSERT(tq.found && tq.kind == TQP_ABSOLUTE, "Tim Sept 2023 → ABSOLUTE");
}

int main(void) {
   test_month_year();
   test_season_year();
   test_lone_year();
   test_yesterday();
   test_last_unit();
   test_today();
   test_vague();
   test_no_match();
   test_n_units_ago();
   test_bare_month();
   test_proximity();
   test_locomo_cat3_samples();

   printf("\n=========================================\n");
   printf("Tests passed: %d, failed: %d\n", tests_passed, tests_failed);
   printf("=========================================\n");
   return tests_failed > 0 ? 1 : 0;
}
