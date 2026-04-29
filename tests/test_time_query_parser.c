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

#include "core/time_query_parser.h"
#include "unity.h"

void setUp(void) {
}
void tearDown(void) {
}

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
   time_query_t tq;
   time_query_parse("Was the first half of September 2022 a good month for Nate?", pin_now(), &tq);
   TEST_ASSERT_TRUE_MESSAGE(tq.found, "September 2022 found");
   TEST_ASSERT_EQUAL_INT_MESSAGE(TQP_ABSOLUTE, tq.kind, "September 2022 ABSOLUTE");
   int64_t expected = make_ref(2022, 8, 15);
   TEST_ASSERT_TRUE_MESSAGE(llabs(tq.target_ts - expected) < 86400, "September 2022 mid-Sept 2022");

   time_query_parse("In May 2020 we moved", pin_now(), &tq);
   TEST_ASSERT_TRUE_MESSAGE(tq.found && tq.kind == TQP_ABSOLUTE, "May 2020 ABSOLUTE");

   time_query_parse("on January 5 2019", pin_now(), &tq);
   TEST_ASSERT_TRUE_MESSAGE(tq.found, "January 2019 found via abbrev path");
}

static void test_season_year(void) {
   time_query_t tq;
   time_query_parse("What state did Joanna visit in summer 2021?", pin_now(), &tq);
   TEST_ASSERT_TRUE_MESSAGE(tq.found, "summer 2021 found");
   TEST_ASSERT_EQUAL_INT_MESSAGE(TQP_ABSOLUTE, tq.kind, "summer 2021 ABSOLUTE");
   int64_t july_2021 = make_ref(2021, 6, 15); /* July midpoint */
   TEST_ASSERT_TRUE_MESSAGE(llabs(tq.target_ts - july_2021) < 30 * 86400,
                            "summer 2021 mid-summer 2021");

   time_query_parse("the winter 2020 storm", pin_now(), &tq);
   TEST_ASSERT_TRUE_MESSAGE(tq.found, "winter 2020 found");
   /* Winter 2020 = Dec 2020 - Feb 2021, midpoint = mid-January 2021 */
   int64_t jan_2021 = make_ref(2021, 0, 15);
   TEST_ASSERT_TRUE_MESSAGE(llabs(tq.target_ts - jan_2021) < 30 * 86400,
                            "winter 2020 mid-January 2021");

   time_query_parse("during fall 2022", pin_now(), &tq);
   TEST_ASSERT_TRUE_MESSAGE(tq.found, "fall 2022 found");
}

static void test_lone_year(void) {
   time_query_t tq;
   time_query_parse("things that happened in 2020", pin_now(), &tq);
   TEST_ASSERT_TRUE_MESSAGE(tq.found && tq.kind == TQP_ABSOLUTE, "in 2020 ABSOLUTE");

   time_query_parse("plans for 2025", pin_now(), &tq);
   TEST_ASSERT_TRUE_MESSAGE(tq.found, "for 2025 found (future)");

   /* Year inside a longer number should NOT match. */
   time_query_parse("call 12022 if you need help", pin_now(), &tq);
   TEST_ASSERT_FALSE_MESSAGE(tq.found, "12022 inside number no match");

   time_query_parse("conv id 20210", pin_now(), &tq);
   TEST_ASSERT_FALSE_MESSAGE(tq.found, "20210 (5-digit) no match");

   /* Out-of-range years should not match. */
   time_query_parse("dating from 1850", pin_now(), &tq);
   TEST_ASSERT_FALSE_MESSAGE(tq.found, "1850 (pre-1900) no match");
}

/* ============================================================================
 * Relative expressions (anchored to pinned now)
 * ============================================================================ */

static void test_yesterday(void) {
   time_query_t tq;
   int64_t now = pin_now();
   time_query_parse("what did we discuss yesterday", now, &tq);
   TEST_ASSERT_TRUE_MESSAGE(tq.found, "yesterday found");
   TEST_ASSERT_EQUAL_INT_MESSAGE(TQP_RELATIVE, tq.kind, "yesterday RELATIVE");
   TEST_ASSERT_TRUE_MESSAGE(llabs(tq.target_ts - (now - 86400)) < 60, "yesterday now - 1d");
}

static void test_last_unit(void) {
   time_query_t tq;
   int64_t now = pin_now();
   time_query_parse("did anything happen last week", now, &tq);
   TEST_ASSERT_TRUE_MESSAGE(tq.found && tq.kind == TQP_RELATIVE, "last week RELATIVE");
   TEST_ASSERT_TRUE_MESSAGE(llabs(tq.target_ts - (now - 7 * 86400)) < 60, "last week now - 7d");

   time_query_parse("a month ago I started", now, &tq);
   TEST_ASSERT_TRUE_MESSAGE(tq.found, "a month ago found");
   TEST_ASSERT_TRUE_MESSAGE(llabs(tq.target_ts - (now - 30 * 86400)) < 60, "a month ago now - 30d");

   time_query_parse("last year was rough", now, &tq);
   TEST_ASSERT_TRUE_MESSAGE(tq.found, "last year found");
}

static void test_today(void) {
   time_query_t tq;
   int64_t now = pin_now();
   time_query_parse("what did I say today", now, &tq);
   TEST_ASSERT_TRUE_MESSAGE(tq.found, "today found");
   TEST_ASSERT_TRUE_MESSAGE(llabs(tq.target_ts - now) < 60, "today now");
}

static void test_vague(void) {
   time_query_t tq;
   int64_t now = pin_now();
   time_query_parse("we talked recently about lunch", now, &tq);
   TEST_ASSERT_TRUE_MESSAGE(tq.found, "recently found");
   TEST_ASSERT_EQUAL_INT_MESSAGE(TQP_VAGUE, tq.kind, "recently VAGUE");
}

static void test_no_match(void) {
   time_query_t tq;
   time_query_parse("what's the weather like", pin_now(), &tq);
   TEST_ASSERT_FALSE_MESSAGE(tq.found, "no temporal expression not found");

   time_query_parse("", pin_now(), &tq);
   TEST_ASSERT_FALSE_MESSAGE(tq.found, "empty query not found");

   /* Word boundary: "fall" inside "fallback" must not match. */
   time_query_parse("we need a fallback for 2099", pin_now(), &tq);
   /* The "2099" should still match. */
   TEST_ASSERT_TRUE_MESSAGE(tq.found && strcmp(tq.matched, "2099") == 0,
                            "fallback 2099 matches 2099, not 'fall'");
}

/* ============================================================================
 * Proximity scoring
 * ============================================================================ */

static void test_proximity(void) {
   time_query_t tq;
   time_query_parse("September 2022", pin_now(), &tq);

   /* Right on the target — score = 1.0 */
   float p = time_query_proximity(&tq, tq.target_ts);
   TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, 1.0f, p, "distance 0 score 1.0");

   /* One window away — score ~ exp(-0.5) ~ 0.61 */
   p = time_query_proximity(&tq, tq.target_ts + tq.window_seconds);
   TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.05f, 0.6065f, p, "distance 1w score 0.61");

   /* Far away — score ~ 0 */
   p = time_query_proximity(&tq, tq.target_ts + 5 * tq.window_seconds);
   TEST_ASSERT_TRUE_MESSAGE(p < 0.01f, "distance 5w score 0");

   /* No temporal query — score 0 */
   time_query_t empty = { 0 };
   p = time_query_proximity(&empty, tq.target_ts);
   TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.001f, 0.0f, p, "no temporal query score 0");

   /* No chunk timestamp — score 0 */
   p = time_query_proximity(&tq, 0);
   TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.001f, 0.0f, p, "chunk_ts = 0 score 0");
}

/* ============================================================================
 * N units ago (digits + words)
 * ============================================================================ */

static void test_n_units_ago(void) {
   time_query_t tq;
   int64_t now = pin_now();

   time_query_parse("how many days ago did I attend the service", now, &tq);
   TEST_ASSERT_TRUE_MESSAGE(tq.found && tq.kind == TQP_RELATIVE,
                            "'how many days ago' without N recency anchor");

   time_query_parse("5 days ago I went hiking", now, &tq);
   TEST_ASSERT_TRUE_MESSAGE(tq.found && tq.kind == TQP_RELATIVE, "5 days ago RELATIVE");
   TEST_ASSERT_TRUE_MESSAGE(llabs(tq.target_ts - (now - 5 * 86400)) < 60, "5 days ago now - 5d");

   time_query_parse("two weeks ago I bought a guitar", now, &tq);
   TEST_ASSERT_TRUE_MESSAGE(tq.found, "two weeks ago found");
   TEST_ASSERT_TRUE_MESSAGE(llabs(tq.target_ts - (now - 14 * 86400)) < 60,
                            "two weeks ago now - 14d");

   time_query_parse("in the past 30 days I started running", now, &tq);
   TEST_ASSERT_TRUE_MESSAGE(tq.found, "in the past 30 days found");

   time_query_parse("3 months ago I moved", now, &tq);
   TEST_ASSERT_TRUE_MESSAGE(tq.found, "3 months ago found");
   TEST_ASSERT_TRUE_MESSAGE(llabs(tq.target_ts - (now - 90 * 86400)) < 86400,
                            "3 months ago ~now - 90d");
}

/* ============================================================================
 * Bare month name (no year)
 * ============================================================================ */

static void test_bare_month(void) {
   time_query_t tq;
   int64_t now = pin_now(); /* April 15, 2024 */

   time_query_parse("what did I do in September", now, &tq);
   TEST_ASSERT_TRUE_MESSAGE(tq.found, "in September found (most-recent)");
   /* September 2023 (most recent past September from April 2024) */
   int64_t sept_2023 = make_ref(2023, 8, 15);
   TEST_ASSERT_TRUE_MESSAGE(llabs(tq.target_ts - sept_2023) < 86400, "in September mid-Sept 2023");

   time_query_parse("what about June plans", now, &tq);
   /* June 2023 (last completed June from April 2024) */
   int64_t june_2023 = make_ref(2023, 5, 15);
   TEST_ASSERT_TRUE_MESSAGE(tq.found && llabs(tq.target_ts - june_2023) < 86400,
                            "bare June June 2023 (most recent)");

   /* "may" with temporal preposition — month */
   time_query_parse("in may", now, &tq);
   TEST_ASSERT_TRUE_MESSAGE(tq.found, "'in may' found (month)");
   int64_t may_2023 = make_ref(2023, 4, 15);
   TEST_ASSERT_TRUE_MESSAGE(llabs(tq.target_ts - may_2023) < 86400,
                            "'in may' May 2023 (most recent)");

   time_query_parse("last may was warm", now, &tq);
   TEST_ASSERT_TRUE_MESSAGE(tq.found, "'last may' found");

   time_query_parse("during may", now, &tq);
   TEST_ASSERT_TRUE_MESSAGE(tq.found, "'during may' found");

   time_query_parse("early may", now, &tq);
   TEST_ASSERT_TRUE_MESSAGE(tq.found, "'early may' found");

   time_query_parse("by may we should finish", now, &tq);
   TEST_ASSERT_TRUE_MESSAGE(tq.found, "'by may' found");

   /* "may" without temporal context — verb, not matched */
   time_query_parse("I may visit Paris", now, &tq);
   TEST_ASSERT_FALSE_MESSAGE(tq.found, "bare 'may' not matched (verb)");

   time_query_parse("may I help you", now, &tq);
   TEST_ASSERT_FALSE_MESSAGE(tq.found, "'may' at start not matched");

   time_query_parse("he may decide", now, &tq);
   TEST_ASSERT_FALSE_MESSAGE(tq.found, "'he may' not matched");
}

/* ============================================================================
 * ISO-8601 dates
 * ============================================================================ */

static void test_iso_date(void) {
   time_query_t tq;

   /* YYYY-MM-DD — exact day, +/-1 day window */
   time_query_parse("2020-03-15", pin_now(), &tq);
   TEST_ASSERT_TRUE_MESSAGE(tq.found && tq.kind == TQP_ABSOLUTE, "2020-03-15 found ABSOLUTE");
   int64_t mar15 = make_ref(2020, 2, 15);
   TEST_ASSERT_TRUE_MESSAGE(llabs(tq.target_ts - mar15) < 86400, "2020-03-15 target near March 15");
   TEST_ASSERT_EQUAL_INT_MESSAGE(86400, (int)tq.window_seconds, "2020-03-15 window = 1 day");
   TEST_ASSERT_EQUAL_STRING_MESSAGE("2020-03-15", tq.matched, "2020-03-15 matched string");

   /* YYYY-MM — month-level, +/-15 day window */
   time_query_parse("2022-11", pin_now(), &tq);
   TEST_ASSERT_TRUE_MESSAGE(tq.found && tq.kind == TQP_ABSOLUTE, "2022-11 found ABSOLUTE");
   int64_t nov15 = make_ref(2022, 10, 15);
   TEST_ASSERT_TRUE_MESSAGE(llabs(tq.target_ts - nov15) < 86400, "2022-11 target near Nov 15");
   TEST_ASSERT_EQUAL_INT_MESSAGE(15 * 86400, (int)tq.window_seconds, "2022-11 window = 15 days");

   /* Embedded in sentence */
   time_query_parse("documents from 2022-11-05", pin_now(), &tq);
   TEST_ASSERT_TRUE_MESSAGE(tq.found, "embedded ISO date found");
   TEST_ASSERT_EQUAL_STRING_MESSAGE("2022-11-05", tq.matched, "embedded matched string");

   /* Invalid month — falls through to try_year (matches "2020" with 180d window) */
   time_query_parse("2020-13-01", pin_now(), &tq);
   TEST_ASSERT_TRUE_MESSAGE(!tq.found || tq.window_seconds != 86400,
                            "month 13 not matched as ISO date");

   /* Invalid day — falls through to try_year */
   time_query_parse("2020-03-32", pin_now(), &tq);
   TEST_ASSERT_TRUE_MESSAGE(!tq.found || tq.window_seconds != 86400,
                            "day 32 not matched as ISO date");

   /* Year out of range */
   time_query_parse("1850-06-15", pin_now(), &tq);
   TEST_ASSERT_FALSE_MESSAGE(tq.found, "year 1850 not found");

   /* No left word boundary */
   time_query_parse("abc2020-03-15", pin_now(), &tq);
   TEST_ASSERT_FALSE_MESSAGE(tq.found, "no left boundary not found");

   /* No right word boundary — try_year may still match "2020" */
   time_query_parse("2020-03-15abc", pin_now(), &tq);
   TEST_ASSERT_TRUE_MESSAGE(!tq.found || tq.window_seconds != 86400,
                            "no right boundary not matched as ISO date");

   /* Precedence: ISO date beats bare year */
   time_query_parse("what happened 2020-03-15", pin_now(), &tq);
   TEST_ASSERT_TRUE_MESSAGE(tq.found && tq.window_seconds == 86400,
                            "ISO date beats bare year (window = 1 day, not 180 days)");

   /* Datetime with T accepted as right boundary */
   time_query_parse("2020-03-15T14:30:00Z", pin_now(), &tq);
   TEST_ASSERT_TRUE_MESSAGE(tq.found, "datetime with T found (date portion)");
   TEST_ASSERT_EQUAL_STRING_MESSAGE("2020-03-15", tq.matched, "datetime with T matched date only");

   /* Slash separator is NOT ISO-8601 */
   time_query_parse("2020/03/15", pin_now(), &tq);
   TEST_ASSERT_TRUE_MESSAGE(!tq.found || tq.window_seconds != 86400,
                            "slash separator not ISO date");

   /* Single-digit month not valid strict ISO */
   time_query_parse("2020-3-15", pin_now(), &tq);
   TEST_ASSERT_TRUE_MESSAGE(!tq.found || tq.window_seconds != 86400,
                            "single-digit month not strict ISO date");
}

/* ============================================================================
 * LoCoMo cat-3 sample queries (from real dataset)
 * ============================================================================ */

static void test_locomo_cat3_samples(void) {
   time_query_t tq;

   /* From LoCoMo: "What state did Joanna visit in summer 2021?" */
   time_query_parse("What state did Joanna visit in summer 2021?", pin_now(), &tq);
   TEST_ASSERT_TRUE_MESSAGE(tq.found && tq.kind == TQP_ABSOLUTE, "Joanna summer 2021 ABSOLUTE");

   /* "Was the first half of September 2022 a good month career-wise for Nate and Joanna?" */
   time_query_parse(
       "Was the first half of September 2022 a good month career-wise for Nate and Joanna?",
       pin_now(), &tq);
   TEST_ASSERT_TRUE_MESSAGE(tq.found && tq.kind == TQP_ABSOLUTE, "Sept 2022 career ABSOLUTE");

   /* "Which US states might Tim be in during September 2023?" */
   time_query_parse("Which US states might Tim be in during September 2023?", pin_now(), &tq);
   TEST_ASSERT_TRUE_MESSAGE(tq.found && tq.kind == TQP_ABSOLUTE, "Tim Sept 2023 ABSOLUTE");
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_month_year);
   RUN_TEST(test_season_year);
   RUN_TEST(test_lone_year);
   RUN_TEST(test_yesterday);
   RUN_TEST(test_last_unit);
   RUN_TEST(test_today);
   RUN_TEST(test_vague);
   RUN_TEST(test_no_match);
   RUN_TEST(test_n_units_ago);
   RUN_TEST(test_bare_month);
   RUN_TEST(test_iso_date);
   RUN_TEST(test_proximity);
   RUN_TEST(test_locomo_cat3_samples);
   return UNITY_END();
}
