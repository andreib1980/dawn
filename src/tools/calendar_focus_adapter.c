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
 * Calendar event focus adapter — Phase 1d of Dynamic Context Injection.
 *
 * source_id          = "calendar_event"
 * source_type        = FOCUS_SOURCE_EXTERNAL
 * requires_embedding = false   /  Adapter consulted on every compose;
 *                                 keyword + recency + structured time
 *                                 signal — no embedding required for v1.
 *
 * DESIGN: this adapter goes DIRECT to calendar_db_*, NOT through
 * calendar_service_*.  Rationale:
 *   1. calendar_service_* may trigger background sync on stale
 *      accounts → covert network call, forbidden by the
 *      no-live-network invariant in the focus-injection hot path.
 *   2. calendar_service_* helpers return count-via-int, conflating
 *      "no results" with "error" — incompatible with focus-source
 *      framework's strict SUCCESS/FAILURE contract.
 *   3. calendar_db_* signatures already match the framework's
 *      SUCCESS/FAILURE convention (verified at calendar_db.c:649,
 *      725) and are pure SQLite under the auth_db mutex.
 *
 * Pipeline (DB-direct chain, all calls pure cache):
 *   1. calendar_db_account_list(user_id, ...) — user-scoped at SQL.
 *   2. Cap to EXTERNAL_MAX_ACCOUNTS_PER_COMPOSE; OLOG_WARNING if more
 *      accounts exist.
 *   3. For each account: calendar_db_calendar_list(account_id, ...)
 *      → aggregate active (is_active==true) calendar_ids into a flat
 *      int64_t array.  account_id chain is user-scoped transitively.
 *   4. calendar_db_occurrences_in_range(cal_ids, cal_count,
 *      now - 86400, now + 7*86400, ...) — recent past + 1-week
 *      lookahead.  SQL filters cancelled occurrences.
 *   5. If query_text non-NULL/non-empty: also call
 *      calendar_db_occurrences_search(cal_ids, cal_count, query, ...)
 *      and merge unique by occurrence.id (DB row id).  Search-path
 *      occurrences receive an additional semantic_score = 0.7
 *      "presence-of-text-match" heuristic; range-only occurrences
 *      get FOCUS_SCORE_NA semantic.
 *   6. Render "[YYYY-MM-DD HH:MM] <summary>: <description-fragment>"
 *      through focus_candidate_init (FOCUS_TEXT_MAX_BYTES truncation).
 *   7. Compute asymmetric recency: past events 7-day half-life;
 *      future events 14-day half-life.  Importance = base + (within
 *      24h && future ? imminent boost : 0) + (today ? today boost
 *      : 0); clamped to 1.0.
 *
 * Provenance: {0,0,0} sentinel — calendar occurrences have no
 * conv-based provenance.
 *
 * item_id format: "calendar_occ:<occurrence.id>" — `occurrence.id` is
 * the local DB row id (server-generated, opaque).  NEVER constructed
 * from `event_uid` (iCal UID, server-controlled by the CalDAV
 * provider but ultimately user-influenceable through event creation
 * on the upstream calendar).
 *
 * Network-call audit (verified cache-only on 2026-05-08 by reading
 * src/tools/calendar_db.c — pure SQLite via AUTH_DB_LOCK_OR_FAIL → s_db
 * prepared statements; no curl_*, socket(), connect(), lws_*, SSL_*
 * calls in any function or its helpers):
 *   - calendar_db_account_list             (calendar_db.c:221)
 *   - calendar_db_calendar_list            (calendar_db.c:415)
 *   - calendar_db_occurrences_in_range     (calendar_db.c:649)
 *   - calendar_db_occurrences_search       (calendar_db.c:725)
 *
 * Filter-on-retrieval is FRAMEWORK-OWNED + trust-tier-gated.  This
 * adapter does NOT call `memory_filter_check()` — `focus_compose()`
 * decides based on `source_type`.  Calendar events are
 * FOCUS_SOURCE_EXTERNAL (the user owns their CalDAV account) and pass
 * through without filtering.  If a multi-user calendar-share threat
 * model ever applies, reclassify as FOCUS_SOURCE_USER_CONTENT.
 */

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "dawn_error.h"
#include "logging.h"
#include "memory/focus_candidate_helpers.h"
#include "memory/focus_source.h"
#include "tools/calendar_db.h"
#include "tools/external_focus_adapters_internal.h"

/* Constants — file-static, all TODO(1j) for bench-driven tuning. */

/* Asymmetric recency half-lives.  Past events fade faster than future
 * events because the user usually wants to know about what's coming
 * up; a yesterday meeting is less relevant than a meeting in 3 days
 * about the same topic.  TODO(1j). */
#define CALENDAR_PAST_HALF_LIFE_SECONDS (7 * 86400)
#define CALENDAR_FUTURE_HALF_LIFE_SECONDS (14 * 86400)

/* Importance boosts on top of the base.  Imminent (next 24h, future)
 * is the strongest signal; same-day adds a smaller boost.  Both stack
 * for events in the next 24h that are also today.  Clamped to 1.0. */
#define CALENDAR_IMMINENT_BOOST 0.3f
#define CALENDAR_TODAY_BOOST 0.2f
#define CALENDAR_DEFAULT_IMPORTANCE 0.5f

/* Window the range-path query covers around `now`.
 * Past 1 day catches just-finished events the user may ask about;
 * forward 1 week is the practical horizon for "what's coming up". */
#define CALENDAR_PAST_WINDOW_SECONDS (1 * 86400)
#define CALENDAR_FUTURE_WINDOW_SECONDS (7 * 86400)

/* Heuristic semantic score assigned to occurrences that came from
 * the LIKE %query% search path (vs. range-only).  Adapter doesn't
 * embed; this is the "the keyword matched" presence signal handed
 * to the framework ranker. */
#define CALENDAR_SEARCH_HIT_SEMANTIC 0.7f

/* Per-account upper bound on calendars + per-pull upper bound on
 * occurrences.  Keep them low — the focus pool is itself capped at
 * top_k (≤64) so over-fetching just to throw away is wasted IO.
 * EXTERNAL_MAX_ACCOUNTS_PER_COMPOSE × MAX_CALENDARS_PER_ACCOUNT
 * = 3 × 16 = 48 calendar IDs max, well within sqlite IN-clause
 * limits. */
#define MAX_CALENDARS_PER_ACCOUNT 16
#define MAX_OCCURRENCES_PER_PULL 64

/* Compile-time-folded calendar_ids[] cap.  Macro form (rather than
 * `const int`) so the array decays to a fixed-size array under C11
 * §6.7.6.2 — `const int` is not an integer constant expression in C
 * and would emit a VLA, which the project standard discourages
 * ("prefer static allocation").  Stack footprint = 48 × 8 B = 384 B. */
#define MAX_TOTAL_CALENDARS (EXTERNAL_MAX_ACCOUNTS_PER_COMPOSE * MAX_CALENDARS_PER_ACCOUNT)

/* "calendar_occ:9223372036854775807\0" → 33 chars; fits buf. */

/* Aggregate work buffers (stack) sized to the static caps above so
 * we never heap-allocate during the DB walk; only the final
 * `focus_candidate_t` array is heap-allocated. */

static bool same_local_day(time_t a, time_t b) {
   struct tm ta, tb;
   if (localtime_r(&a, &ta) == NULL || localtime_r(&b, &tb) == NULL)
      return false;
   return ta.tm_year == tb.tm_year && ta.tm_yday == tb.tm_yday;
}

/* 0.5 ** (age / half) with source-specific half-life — same exponential
 * shape as focus_recency_decay_uniform but asymmetric: past events use
 * the shorter half-life, future events the longer.  Returns 0.0 for
 * unset event_ts so the ranker doesn't credit unknowns. */
static double calendar_recency_decay(time_t event_ts, time_t now) {
   if (event_ts <= 0)
      return 0.0;
   const double half = (event_ts < now) ? (double)CALENDAR_PAST_HALF_LIFE_SECONDS
                                        : (double)CALENDAR_FUTURE_HALF_LIFE_SECONDS;
   const double age = (event_ts < now) ? (double)(now - event_ts) : (double)(event_ts - now);
   const double decay = pow(0.5, age / half);
   if (decay <= 0.0)
      return 0.0;
   if (decay >= 1.0)
      return 1.0;
   return decay;
}

static int format_event_text(const calendar_occurrence_t *occ, char *out, size_t outlen) {
   /* Render localtime (matches DAWN's user-facing convention; CalDAV
    * dtstart is stored as epoch seconds in the DB, timezone-agnostic). */
   struct tm tm;
   if (localtime_r(&occ->dtstart, &tm) == NULL)
      return FAILURE;
   const char *summary = (occ->summary[0] != '\0') ? occ->summary : "(untitled event)";
   const int n = snprintf(out, outlen, "[%04d-%02d-%02d %02d:%02d] %s", tm.tm_year + 1900,
                          tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, summary);
   if (n < 0 || (size_t)n >= outlen)
      return FAILURE;
   return SUCCESS;
}

static int calendar_adapter_query(int user_id,
                                  bool include_private,
                                  const char *query_text,
                                  const float *query_embedding,
                                  size_t embed_dim,
                                  time_t now,
                                  int max_candidates,
                                  focus_candidate_t **out_candidates,
                                  int *out_count) {
   (void)include_private; /* 1f: calendar private flag not yet
                             modeled; per-account read_only is
                             separate (write protection only). */
   (void)query_embedding; /* requires_embedding=false; embedding
                             consumed only by document/memory adapters. */
   (void)embed_dim;
   *out_candidates = NULL;
   *out_count = 0;
   if (max_candidates <= 0)
      return SUCCESS;

   /* Stack-frame note: this function carries ~175 KB on the stack
    * (range_buf[64] + search_buf[64] of calendar_occurrence_t at
    * ~1.4 KB each + accounts[16] + smaller buffers).  Safe under
    * the default 8 MB pthread stack but worth knowing if this ever
    * runs on a constrained-stack thread (ESP32 satellite scratch).
    * Heap allocations + cleanup are funneled through a single
    * `cleanup:` epilogue at the end of the function so adding a
    * future workspace buffer means one calloc + one free, not
    * chasing every error branch. */

   calendar_occurrence_t *merged = NULL;
   bool *from_search = NULL;
   focus_candidate_t *out = NULL;
   int rc = SUCCESS;
   int produced = 0;

   /* Step 1: enumerate accounts (user-scoped at SQL). */
   calendar_account_t accounts[CALENDAR_MAX_ACCOUNTS];
   int account_count = 0;
   if (calendar_db_account_list(user_id, accounts, CALENDAR_MAX_ACCOUNTS, &account_count) !=
       SUCCESS) {
      OLOG_ERROR("calendar_adapter: calendar_db_account_list failed (user_id=%d)", user_id);
      rc = FAILURE;
      goto cleanup;
   }
   if (account_count <= 0)
      goto cleanup; /* SUCCESS, zero candidates */

   const int effective_account_count = (account_count > EXTERNAL_MAX_ACCOUNTS_PER_COMPOSE)
                                           ? EXTERNAL_MAX_ACCOUNTS_PER_COMPOSE
                                           : account_count;
   if (account_count > EXTERNAL_MAX_ACCOUNTS_PER_COMPOSE) {
      OLOG_WARNING("calendar_adapter: user_id=%d has %d accounts; capping at %d for focus compose "
                   "(remaining accounts dropped in db_account_list order)",
                   user_id, account_count, EXTERNAL_MAX_ACCOUNTS_PER_COMPOSE);
   }

   /* Step 2: aggregate active calendar IDs across the considered
    * accounts.  account_id is user-scoped transitively (we got it
    * from a user-scoped account_list above).  Active filter applied
    * client-side because calendar_db_calendar_list returns all
    * calendars on the account.  Stack array sized via MAX_TOTAL_CALENDARS
    * macro (NOT a `const int` that would emit a VLA). */
   int64_t calendar_ids[MAX_TOTAL_CALENDARS];
   int total_calendars = 0;
   for (int a = 0; a < effective_account_count; a++) {
      calendar_calendar_t cals[MAX_CALENDARS_PER_ACCOUNT];
      int cal_count = 0;
      if (calendar_db_calendar_list(accounts[a].id, cals, MAX_CALENDARS_PER_ACCOUNT, &cal_count) !=
          SUCCESS) {
         OLOG_WARNING("calendar_adapter: calendar_db_calendar_list failed (account_id=%lld) — "
                      "continuing with remaining accounts",
                      (long long)accounts[a].id);
         continue;
      }
      for (int c = 0; c < cal_count && total_calendars < MAX_TOTAL_CALENDARS; c++) {
         if (!cals[c].is_active)
            continue;
         calendar_ids[total_calendars++] = cals[c].id;
      }
   }
   if (total_calendars <= 0)
      goto cleanup; /* SUCCESS, zero candidates */

   /* Step 3: range-window pull. */
   calendar_occurrence_t range_buf[MAX_OCCURRENCES_PER_PULL];
   int range_count = 0;
   const time_t range_start = now - CALENDAR_PAST_WINDOW_SECONDS;
   const time_t range_end = now + CALENDAR_FUTURE_WINDOW_SECONDS;
   if (calendar_db_occurrences_in_range(calendar_ids, total_calendars, range_start, range_end,
                                        range_buf, MAX_OCCURRENCES_PER_PULL,
                                        &range_count) != SUCCESS) {
      OLOG_ERROR("calendar_adapter: calendar_db_occurrences_in_range failed");
      rc = FAILURE;
      goto cleanup;
   }

   /* Step 4: optional query_text search; merged with range pull,
    * deduped by occ.id.  query_text is user-controlled; do NOT log
    * it on failure — log a length+sentinel only, matching the
    * project log-redaction convention used for memory_facts. */
   calendar_occurrence_t search_buf[MAX_OCCURRENCES_PER_PULL];
   int search_count = 0;
   const bool have_query = (query_text != NULL && query_text[0] != '\0');
   if (have_query) {
      if (calendar_db_occurrences_search(calendar_ids, total_calendars, query_text, search_buf,
                                         MAX_OCCURRENCES_PER_PULL, &search_count) != SUCCESS) {
         OLOG_WARNING("calendar_adapter: calendar_db_occurrences_search failed (query_len=%zu) — "
                      "falling back to range-only",
                      strlen(query_text));
         search_count = 0;
      }
   }

   /* Merge/dedupe.  Order: search hits first (so they keep their
    * search-hit semantic score), then range hits not already seen.
    * Intermediate "merged" view tracks the source for each entry. */
   const int max_merged = range_count + search_count;
   if (max_merged <= 0)
      goto cleanup; /* SUCCESS, zero candidates */

   /* Stack-allocate the merge state — bounded by 2 *
    * MAX_OCCURRENCES_PER_PULL = 128 entries × ~1 KB occurrence struct
    * = ~128 KB worst case.  Use heap to keep stack predictable. */
   merged = calloc((size_t)max_merged, sizeof(*merged));
   from_search = calloc((size_t)max_merged, sizeof(*from_search));
   if (merged == NULL || from_search == NULL) {
      OLOG_ERROR("calendar_adapter: OOM allocating merge workspace (max=%d)", max_merged);
      rc = FAILURE;
      goto cleanup;
   }
   int merged_n = 0;
   for (int i = 0; i < search_count && merged_n < max_merged; i++) {
      merged[merged_n] = search_buf[i];
      from_search[merged_n] = true;
      merged_n++;
   }
   for (int i = 0; i < range_count && merged_n < max_merged; i++) {
      bool dup = false;
      for (int j = 0; j < merged_n; j++) {
         if (merged[j].id == range_buf[i].id) {
            dup = true;
            break;
         }
      }
      if (dup)
         continue;
      merged[merged_n] = range_buf[i];
      from_search[merged_n] = false;
      merged_n++;
   }
   if (merged_n <= 0)
      goto cleanup; /* SUCCESS, zero candidates */

   /* Trim to max_candidates.  Order chosen here is "search hits
    * first then range" rather than score-sorted because the
    * framework's ranker re-sorts everything anyway; what matters
    * is that we don't drop search hits when range pool is large. */
   const int kept = (merged_n > max_candidates) ? max_candidates : merged_n;

   out = calloc((size_t)kept, sizeof(*out));
   if (out == NULL) {
      OLOG_ERROR("calendar_adapter: OOM allocating candidate array (n=%d)", kept);
      rc = FAILURE;
      goto cleanup;
   }

   bool truncated_warned = false;
   for (int i = 0; i < kept; i++) {
      const calendar_occurrence_t *occ = &merged[i];

      char rendered[FOCUS_TEXT_MAX_BYTES + 64];
      if (format_event_text(occ, rendered, sizeof(rendered)) != SUCCESS) {
         /* Render failure is non-fatal: skip this occurrence and let
          * the rest of `kept` proceed.  `produced` will lag `kept` but
          * the SUCCESS path returns `produced` as the count, so the
          * tail of `out[]` past `produced` is unused — caller sees
          * only the populated slots.  Asymmetric with item_id /
          * focus_candidate_init failures, which are fatal because
          * they indicate a programming error not a data quality issue. */
         OLOG_WARNING("calendar_adapter: format_event_text failed (occ_id=%lld) — skipping",
                      (long long)occ->id);
         continue;
      }

      char item_id[FOCUS_ITEM_ID_BUFLEN];
      if (focus_candidate_format_item_id(item_id, sizeof(item_id), "calendar_occ", occ->id) !=
          SUCCESS) {
         OLOG_ERROR("calendar_adapter: item_id formatting failed (occ_id=%lld)",
                    (long long)occ->id);
         focus_adapter_failure_cleanup(out, produced, out_candidates, out_count);
         out = NULL; /* ownership transferred to failure-cleanup */
         rc = FAILURE;
         goto cleanup;
      }

      const float semantic = from_search[i] ? CALENDAR_SEARCH_HIT_SEMANTIC : FOCUS_SCORE_NA;
      const float recency = (float)calendar_recency_decay(occ->dtstart, now);

      float importance = CALENDAR_DEFAULT_IMPORTANCE;
      const time_t delta = occ->dtstart - now;
      if (delta > 0 && delta < 86400)
         importance += CALENDAR_IMMINENT_BOOST;
      if (same_local_day(occ->dtstart, now))
         importance += CALENDAR_TODAY_BOOST;
      if (importance > 1.0f)
         importance = 1.0f;

      if (focus_candidate_init(&out[produced], "calendar_event", FOCUS_SOURCE_EXTERNAL, rendered,
                               item_id, occ->dtstart, semantic, recency, importance,
                               &truncated_warned) != SUCCESS) {
         OLOG_ERROR("calendar_adapter: focus_candidate_init failed (occ_id=%lld)",
                    (long long)occ->id);
         focus_adapter_failure_cleanup(out, produced, out_candidates, out_count);
         out = NULL; /* ownership transferred to failure-cleanup */
         rc = FAILURE;
         goto cleanup;
      }
      /* Provenance intentionally zeroed — calendar occurrences have
       * no conv-based provenance. */
      produced++;
   }

cleanup:
   free(merged);
   free(from_search);
   if (rc == SUCCESS && out != NULL) {
      *out_candidates = out;
      *out_count = produced;
   }
   /* On FAILURE, focus_adapter_failure_cleanup already zeroed the
    * out-params and freed `out`; on SUCCESS-with-no-candidates
    * (early-exit at any "zero candidates" gate), out_candidates /
    * out_count remain NULL/0 from the function's top-of-body
    * initialization. */
   return rc;
}

static const focus_source_adapter_t k_calendar_focus_adapter = {
   .source_id = "calendar_event",
   .source_type = FOCUS_SOURCE_EXTERNAL,
   .requires_embedding = false,
   .query = calendar_adapter_query,
};

int calendar_focus_adapter_register(void) {
   return focus_register_source(&k_calendar_focus_adapter);
}
