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
 * Time Query Parser implementation.
 *
 * Pure C, no external dependencies beyond libc + math.  No mutex needed —
 * stateless, thread-safe by construction.  Recognizers are tried in
 * specificity order: explicit absolute dates first, relative durations
 * next, vague terms last.
 */

#define _GNU_SOURCE /* timegm */

#include "tools/time_query_parser.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h> /* snprintf */
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strncasecmp (portable POSIX; also works via _GNU_SOURCE) */

#define DAY_SEC ((int64_t)86400)
#define WEEK_SEC ((int64_t)7 * DAY_SEC)
#define MONTH_SEC ((int64_t)30 * DAY_SEC) /* nominal */
#define YEAR_SEC ((int64_t)365 * DAY_SEC)

#define SUCCESS 0
#define FAILURE 1

/* ============================================================================
 * Lower-case scan helpers
 * ============================================================================ */

/* Case-insensitive substring search.  needle assumed lowercase; haystack any case.
 * Returns pointer to start of match in haystack, or NULL. */
static const char *strcasestr_lower(const char *haystack, const char *needle) {
   if (!haystack || !needle || !*needle)
      return NULL;
   size_t nlen = strlen(needle);
   for (const char *p = haystack; *p; p++) {
      size_t i;
      for (i = 0; i < nlen; i++) {
         if (!p[i])
            return NULL;
         if ((char)tolower((unsigned char)p[i]) != needle[i])
            break;
      }
      if (i == nlen)
         return p;
   }
   return NULL;
}

/* Word-boundary check: previous char and char after match are non-alphanumeric.
 * Prevents "fall" matching inside "fallback".  start is the match position
 * within haystack. */
static bool is_word_boundary(const char *haystack, const char *start, size_t mlen) {
   if (start > haystack) {
      char prev = start[-1];
      if (isalnum((unsigned char)prev) || prev == '_')
         return false;
   }
   char next = start[mlen];
   if (isalnum((unsigned char)next) || next == '_')
      return false;
   return true;
}

/* Scan for a 4-digit year (1900-2099) immediately after `pos` (skipping spaces).
 * Returns the year as an int, or 0 if no year found. */
static int scan_year_after(const char *pos, size_t skip) {
   const char *p = pos + skip;
   while (*p == ' ' || *p == '\t' || *p == ',')
      p++;
   if (!isdigit((unsigned char)p[0]) || !isdigit((unsigned char)p[1]) ||
       !isdigit((unsigned char)p[2]) || !isdigit((unsigned char)p[3]))
      return 0;
   int year = (p[0] - '0') * 1000 + (p[1] - '0') * 100 + (p[2] - '0') * 10 + (p[3] - '0');
   if (year < 1900 || year > 2099)
      return 0;
   /* Reject 5+ digit numbers (e.g., "20210" — must end at word boundary) */
   if (isdigit((unsigned char)p[4]))
      return 0;
   return year;
}

/* Build a UTC timestamp at noon on (year, month0-indexed, day).  Noon (12:00
 * UTC) is deliberate — it keeps the same calendar day for any TZ from UTC-11
 * to UTC+11, so a "September 15" reference lands on September 15 everywhere
 * rather than flipping to an adjacent day for some TZs.  A midnight anchor
 * would be TZ-fragile; we pick noon and never look back. */
static int64_t make_ts(int year, int month0, int day) {
   struct tm tm = { 0 };
   tm.tm_year = year - 1900;
   tm.tm_mon = month0;
   tm.tm_mday = day;
   tm.tm_hour = 12;
   time_t t = timegm(&tm);
   if (t == (time_t)-1)
      return 0;
   return (int64_t)t;
}

/* ============================================================================
 * Recognizers
 * ============================================================================ */

static const struct {
   const char *name;
   int month0; /* 0-11 */
} MONTHS[] = {
   { "january", 0 },   { "february", 1 },  { "march", 2 },  { "april", 3 },     { "may", 4 },
   { "june", 5 },      { "july", 6 },      { "august", 7 }, { "september", 8 }, { "october", 9 },
   { "november", 10 }, { "december", 11 }, { "jan", 0 },    { "feb", 1 },       { "mar", 2 },
   { "apr", 3 },       { "jun", 5 },       { "jul", 6 },    { "aug", 7 },       { "sep", 8 },
   { "sept", 8 },      { "oct", 9 },       { "nov", 10 },   { "dec", 11 },
};
#define MONTHS_COUNT (int)(sizeof(MONTHS) / sizeof(MONTHS[0]))

/* Northern-hemisphere meteorological seasons.  Each is a 3-month window. */
static const struct {
   const char *name;
   int start_month0;
   int end_month0;
} SEASONS[] = {
   { "spring", 2, 4 },  /* Mar-May */
   { "summer", 5, 7 },  /* Jun-Aug */
   { "fall", 8, 10 },   /* Sep-Nov */
   { "autumn", 8, 10 }, /* Sep-Nov (synonym) */
   { "winter", 11, 1 }, /* Dec-Feb (wraps year — special-cased below) */
};
#define SEASONS_COUNT (int)(sizeof(SEASONS) / sizeof(SEASONS[0]))

/* Try "MONTH YYYY" → set out fields, return true.  Window = ±15 days. */
static bool try_month_year(const char *q, time_query_t *out) {
   for (int i = 0; i < MONTHS_COUNT; i++) {
      const char *match = strcasestr_lower(q, MONTHS[i].name);
      if (!match)
         continue;
      size_t mlen = strlen(MONTHS[i].name);
      if (!is_word_boundary(q, match, mlen))
         continue;
      int year = scan_year_after(match, mlen);
      if (year == 0)
         continue;
      out->found = true;
      out->kind = TQP_ABSOLUTE;
      out->target_ts = make_ts(year, MONTHS[i].month0, 15);
      out->window_seconds = 15 * DAY_SEC;
      snprintf(out->matched, sizeof(out->matched), "%s %d", MONTHS[i].name, year);
      return true;
   }
   return false;
}

/* Try "SEASON YYYY" → midpoint of the season, window = ±45 days. */
static bool try_season_year(const char *q, time_query_t *out) {
   for (int i = 0; i < SEASONS_COUNT; i++) {
      const char *match = strcasestr_lower(q, SEASONS[i].name);
      if (!match)
         continue;
      size_t mlen = strlen(SEASONS[i].name);
      if (!is_word_boundary(q, match, mlen))
         continue;
      int year = scan_year_after(match, mlen);
      if (year == 0)
         continue;
      int s = SEASONS[i].start_month0;
      int e = SEASONS[i].end_month0;
      int mid_month, mid_year = year;
      if (s <= e) {
         mid_month = (s + e) / 2;
      } else {
         /* Winter wraps: Dec(11)..Feb(1).  Midpoint is mid-January of year+1. */
         mid_month = 0;
         mid_year = year + 1;
      }
      out->found = true;
      out->kind = TQP_ABSOLUTE;
      out->target_ts = make_ts(mid_year, mid_month, 15);
      out->window_seconds = 45 * DAY_SEC;
      snprintf(out->matched, sizeof(out->matched), "%s %d", SEASONS[i].name, year);
      return true;
   }
   return false;
}

/* Try "in YYYY" / "during YYYY" / "back in YYYY" / lone " YYYY ".  Window = ±180 days. */
static bool try_year(const char *q, time_query_t *out) {
   /* Look for any 4-digit number in 1900-2099 that's at a word boundary. */
   for (const char *p = q; *p; p++) {
      if (!isdigit((unsigned char)*p))
         continue;
      /* must be at the start of a 4-digit run */
      if (p > q && (isdigit((unsigned char)p[-1]) || isalpha((unsigned char)p[-1])))
         continue;
      if (!isdigit((unsigned char)p[1]) || !isdigit((unsigned char)p[2]) ||
          !isdigit((unsigned char)p[3]))
         continue;
      if (isdigit((unsigned char)p[4]) || isalpha((unsigned char)p[4]))
         continue;
      int year = (p[0] - '0') * 1000 + (p[1] - '0') * 100 + (p[2] - '0') * 10 + (p[3] - '0');
      if (year < 1900 || year > 2099)
         continue;
      out->found = true;
      out->kind = TQP_ABSOLUTE;
      out->target_ts = make_ts(year, 6, 1); /* July 1 = midpoint */
      out->window_seconds = 180 * DAY_SEC;
      snprintf(out->matched, sizeof(out->matched), "%d", year);
      return true;
   }
   return false;
}

/* "yesterday" → now - 1 day, window = 1 day. */
static bool try_yesterday(const char *q, int64_t now_ts, time_query_t *out) {
   const char *m = strcasestr_lower(q, "yesterday");
   if (!m || !is_word_boundary(q, m, 9))
      return false;
   out->found = true;
   out->kind = TQP_RELATIVE;
   out->target_ts = now_ts - DAY_SEC;
   out->window_seconds = DAY_SEC;
   snprintf(out->matched, sizeof(out->matched), "yesterday");
   return true;
}

/* "last week/month/year", "this week/month/year", "a X ago" → relative offset. */
static bool try_last_unit(const char *q, int64_t now_ts, time_query_t *out) {
   static const struct {
      const char *phrase;
      int64_t offset;
      int64_t window;
   } UNITS[] = {
      { "last week", WEEK_SEC, WEEK_SEC },  { "last month", MONTH_SEC, MONTH_SEC },
      { "last year", YEAR_SEC, YEAR_SEC },  { "this week", 0, WEEK_SEC },
      { "this month", 0, MONTH_SEC },       { "this year", 0, YEAR_SEC },
      { "a week ago", WEEK_SEC, WEEK_SEC }, { "a month ago", MONTH_SEC, MONTH_SEC },
      { "a year ago", YEAR_SEC, YEAR_SEC },
   };
   int n = (int)(sizeof(UNITS) / sizeof(UNITS[0]));
   for (int i = 0; i < n; i++) {
      const char *m = strcasestr_lower(q, UNITS[i].phrase);
      if (!m)
         continue;
      size_t mlen = strlen(UNITS[i].phrase);
      if (!is_word_boundary(q, m, mlen))
         continue;
      out->found = true;
      out->kind = TQP_RELATIVE;
      out->target_ts = now_ts - UNITS[i].offset;
      out->window_seconds = UNITS[i].window;
      snprintf(out->matched, sizeof(out->matched), "%s", UNITS[i].phrase);
      return true;
   }
   return false;
}

/* English number words 1-31, sufficient for "X days ago" style queries. */
static int parse_number_word(const char *p, size_t *out_len) {
   static const struct {
      const char *word;
      int value;
   } WORDS[] = {
      { "thirty-one", 31 },   { "thirty one", 31 },   { "thirty", 30 },
      { "twenty-nine", 29 },  { "twenty nine", 29 },  { "twenty-eight", 28 },
      { "twenty eight", 28 }, { "twenty-seven", 27 }, { "twenty seven", 27 },
      { "twenty-six", 26 },   { "twenty six", 26 },   { "twenty-five", 25 },
      { "twenty five", 25 },  { "twenty-four", 24 },  { "twenty four", 24 },
      { "twenty-three", 23 }, { "twenty three", 23 }, { "twenty-two", 22 },
      { "twenty two", 22 },   { "twenty-one", 21 },   { "twenty one", 21 },
      { "twenty", 20 },       { "nineteen", 19 },     { "eighteen", 18 },
      { "seventeen", 17 },    { "sixteen", 16 },      { "fifteen", 15 },
      { "fourteen", 14 },     { "thirteen", 13 },     { "twelve", 12 },
      { "eleven", 11 },       { "ten", 10 },          { "nine", 9 },
      { "eight", 8 },         { "seven", 7 },         { "six", 6 },
      { "five", 5 },          { "four", 4 },          { "three", 3 },
      { "two", 2 },           { "one", 1 },
   };
   int n = (int)(sizeof(WORDS) / sizeof(WORDS[0]));
   for (int i = 0; i < n; i++) {
      size_t wlen = strlen(WORDS[i].word);
      if (strncasecmp(p, WORDS[i].word, wlen) == 0) {
         char next = p[wlen];
         if (next == ' ' || next == '\t' || next == '\0' || next == ',') {
            *out_len = wlen;
            return WORDS[i].value;
         }
      }
   }
   return -1;
}

/* "N days/weeks/months/years ago" or "in the past N <unit>" or "how many days
 * passed since X happened N units ago".  N may be a digit run (1-365) or an
 * English number word (1-31, e.g., "two weeks ago").  Anchors target_ts to
 * now - N * unit, with window scaled to the unit. */
static bool try_n_units_ago(const char *q, int64_t now_ts, time_query_t *out) {
   static const struct {
      const char *unit;
      int64_t sec;
   } UNITS[] = {
      { "days", DAY_SEC },     { "day", DAY_SEC },     { "weeks", WEEK_SEC }, { "week", WEEK_SEC },
      { "months", MONTH_SEC }, { "month", MONTH_SEC }, { "years", YEAR_SEC }, { "year", YEAR_SEC },
   };
   int nu = (int)(sizeof(UNITS) / sizeof(UNITS[0]));

   /* Cheap context gate: only fire when query expresses past-tense intent. */
   bool has_ago_phrase = (strcasestr_lower(q, "ago") != NULL ||
                          strcasestr_lower(q, "in the past ") != NULL ||
                          strcasestr_lower(q, "in the last ") != NULL ||
                          strcasestr_lower(q, "how many ") != NULL ||
                          strcasestr_lower(q, "passed since") != NULL ||
                          strcasestr_lower(q, "have passed") != NULL ||
                          strcasestr_lower(q, "had passed") != NULL);
   if (!has_ago_phrase)
      return false;

   for (int u = 0; u < nu; u++) {
      const char *m = strcasestr_lower(q, UNITS[u].unit);
      if (!m)
         continue;
      size_t ulen = strlen(UNITS[u].unit);
      if (!is_word_boundary(q, m, ulen))
         continue;

      /* Walk back over whitespace to find the preceding number token. */
      const char *p = m;
      while (p > q && (p[-1] == ' ' || p[-1] == '\t'))
         p--;
      if (p == q)
         continue;

      int n_value = -1;

      /* Try digit run */
      if (isdigit((unsigned char)p[-1])) {
         const char *start = p;
         while (start > q && isdigit((unsigned char)start[-1]))
            start--;
         int v = 0;
         for (const char *cp = start; cp < p; cp++)
            v = v * 10 + (*cp - '0');
         if (v > 0 && v <= 365)
            n_value = v;
      } else {
         /* Try English number word. Walk back to find word boundary. */
         const char *wstart = p;
         while (wstart > q && wstart[-1] != ' ' && wstart[-1] != '\t')
            wstart--;
         /* Try one word back too for "twenty one" style */
         const char *try2 = wstart;
         if (try2 > q + 1) {
            const char *prev = try2 - 1;
            while (prev > q && prev[-1] != ' ' && prev[-1] != '\t')
               prev--;
            try2 = prev;
         }
         size_t consumed = 0;
         int v = parse_number_word(try2, &consumed);
         if (v <= 0)
            v = parse_number_word(wstart, &consumed);
         if (v > 0)
            n_value = v;
      }

      if (n_value <= 0)
         continue;

      out->found = true;
      out->kind = TQP_RELATIVE;
      out->target_ts = now_ts - (int64_t)n_value * UNITS[u].sec;
      /* Window scales with N — wider for older references to forgive imprecision. */
      out->window_seconds = UNITS[u].sec * (n_value > 4 ? 2 : 1);
      snprintf(out->matched, sizeof(out->matched), "%d %s ago", n_value, UNITS[u].unit);
      return true;
   }
   return false;
}

/* Bare month name ("in September", "during May") with no year — anchor to
 * the most recent past occurrence relative to now_ts.  Window = ±15 days. */
static bool try_bare_month(const char *q, int64_t now_ts, time_query_t *out) {
   for (int i = 0; i < MONTHS_COUNT; i++) {
      const char *match = strcasestr_lower(q, MONTHS[i].name);
      if (!match)
         continue;
      size_t mlen = strlen(MONTHS[i].name);
      if (!is_word_boundary(q, match, mlen))
         continue;
      /* Skip "may" as bare match — too noisy as a verb (pure "may" without year
       * is the only month/auxiliary collision; treat MAY-only as no-match). */
      if (strcmp(MONTHS[i].name, "may") == 0)
         continue;
      /* Skip if year follows — already handled by try_month_year. */
      if (scan_year_after(match, mlen) != 0)
         continue;
      time_t now_t = (time_t)now_ts;
      struct tm tm_now;
      gmtime_r(&now_t, &tm_now);
      int target_year = tm_now.tm_year + 1900;
      if (MONTHS[i].month0 > tm_now.tm_mon)
         target_year--; /* Month later in calendar than current → prior year */
      out->found = true;
      out->kind = TQP_RELATIVE;
      out->target_ts = make_ts(target_year, MONTHS[i].month0, 15);
      out->window_seconds = 15 * DAY_SEC;
      snprintf(out->matched, sizeof(out->matched), "%s (most recent)", MONTHS[i].name);
      return true;
   }
   return false;
}

/* "how many days/weeks/months ago" with NO explicit number → recency anchor.
 * The user's intent is "find the most recent occurrence" since they're asking
 * the system to compute the time-delta to something the user previously
 * mentioned.  Anchor at now - 14d, window 60d — gives strong preference to
 * recent chunks while still admitting events up to ~6 months back. */
static bool try_how_many_ago(const char *q, int64_t now_ts, time_query_t *out) {
   const char *m = strcasestr_lower(q, "how many ");
   if (!m)
      return false;
   /* Confirm it's followed by a temporal unit + ago/passed/since. */
   const char *after = m + 9; /* past "how many " */
   bool has_unit = (strcasestr_lower(after, "days") != NULL ||
                    strcasestr_lower(after, "weeks") != NULL ||
                    strcasestr_lower(after, "months") != NULL ||
                    strcasestr_lower(after, "years") != NULL);
   bool has_ago = (strcasestr_lower(after, "ago") != NULL ||
                   strcasestr_lower(after, "passed") != NULL ||
                   strcasestr_lower(after, "since") != NULL);
   if (!has_unit || !has_ago)
      return false;
   out->found = true;
   out->kind = TQP_RELATIVE;
   out->target_ts = now_ts - 14 * DAY_SEC; /* recent past */
   out->window_seconds = 60 * DAY_SEC;     /* but admit ~6mo back */
   snprintf(out->matched, sizeof(out->matched), "how many ... ago (recency)");
   return true;
}

/* "today" → now, window = 1 day. */
static bool try_today(const char *q, int64_t now_ts, time_query_t *out) {
   const char *m = strcasestr_lower(q, "today");
   if (!m || !is_word_boundary(q, m, 5))
      return false;
   out->found = true;
   out->kind = TQP_RELATIVE;
   out->target_ts = now_ts;
   out->window_seconds = DAY_SEC;
   snprintf(out->matched, sizeof(out->matched), "today");
   return true;
}

/* "recently" / "the other day" → vague, ~30d window centered on now-15d. */
static bool try_vague(const char *q, int64_t now_ts, time_query_t *out) {
   static const char *VAGUE[] = { "recently", "the other day", "lately", NULL };
   for (int i = 0; VAGUE[i]; i++) {
      const char *m = strcasestr_lower(q, VAGUE[i]);
      if (!m)
         continue;
      size_t mlen = strlen(VAGUE[i]);
      if (!is_word_boundary(q, m, mlen))
         continue;
      out->found = true;
      out->kind = TQP_VAGUE;
      out->target_ts = now_ts - 15 * DAY_SEC;
      out->window_seconds = 30 * DAY_SEC;
      snprintf(out->matched, sizeof(out->matched), "%s", VAGUE[i]);
      return true;
   }
   return false;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

int time_query_parse(const char *query, int64_t now_ts, time_query_t *out) {
   if (!query || !out)
      return FAILURE;

   memset(out, 0, sizeof(*out));

   /* Try recognizers in specificity order — first match wins.
    * Most specific (year-bound absolute) → least specific (vague). */
   if (try_month_year(query, out))
      return SUCCESS;
   if (try_season_year(query, out))
      return SUCCESS;
   if (try_n_units_ago(query, now_ts, out)) /* "5 days ago", "two weeks ago" */
      return SUCCESS;
   if (try_how_many_ago(query, now_ts, out)) /* "how many days ago" — recency hint */
      return SUCCESS;
   if (try_year(query, out))
      return SUCCESS;
   if (try_yesterday(query, now_ts, out))
      return SUCCESS;
   if (try_last_unit(query, now_ts, out))
      return SUCCESS;
   if (try_today(query, now_ts, out))
      return SUCCESS;
   if (try_bare_month(query, now_ts, out)) /* "in September" without year */
      return SUCCESS;
   if (try_vague(query, now_ts, out))
      return SUCCESS;

   /* No match — out->found is false from memset. */
   return SUCCESS;
}

float time_query_proximity(const time_query_t *tq, int64_t chunk_ts) {
   if (!tq || !tq->found || chunk_ts <= 0 || tq->window_seconds <= 0)
      return 0.0f;

   int64_t distance = chunk_ts - tq->target_ts;
   if (distance < 0)
      distance = -distance;

   /* Gaussian decay: exp(-(d/w)^2 / 2)
    * d = 0     → 1.00
    * d = w     → 0.61
    * d = 2w    → 0.14
    * d = 3w    → 0.011
    * Past 3 windows we're effectively zero — short-circuit to avoid expf cost. */
   double ratio = (double)distance / (double)tq->window_seconds;
   if (ratio > 3.0)
      return 0.0f;
   return (float)exp(-(ratio * ratio) * 0.5);
}
