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
 * Tavily per-user rate limiter — fixed-window counters for minute + hour.
 */

#include "tools/tavily_rate_limit.h"

#include <pthread.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

#include "logging.h"

/* TAVILY_RL_MAX_USERS is declared in the header so tests and operators can
 * inspect it. Definition lives there. */

typedef struct {
   int user_id; /* 0 = slot unused */
   time_t minute_window_start;
   int minute_count;
   time_t hour_window_start;
   int hour_count;
} bucket_t;

static bucket_t s_buckets[TAVILY_RL_MAX_USERS];
static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Sentinel id for user_id <= 0 — same bucket for all anonymous callers. */
#define ANON_USER_ID (-1)

static int normalize_user_id(int user_id) {
   return user_id > 0 ? user_id : ANON_USER_ID;
}

/* Caller holds s_mutex. Read-only: returns slot index for @p user_id, or
 * -1 if no slot exists yet. Never allocates — safe to call from diagnostic
 * paths without consuming a bucket entry. */
static int find_slot(int user_id) {
   for (int i = 0; i < TAVILY_RL_MAX_USERS; i++) {
      if (s_buckets[i].user_id == user_id) {
         return i;
      }
   }
   return -1;
}

/* Caller holds s_mutex. Returns slot index, or -1 if full. Allocates a new
 * slot for the user if none exists. Used by the charging path only. */
static int find_or_alloc_slot(int user_id) {
   int free_slot = -1;
   for (int i = 0; i < TAVILY_RL_MAX_USERS; i++) {
      if (s_buckets[i].user_id == user_id) {
         return i;
      }
      if (free_slot < 0 && s_buckets[i].user_id == 0) {
         free_slot = i;
      }
   }
   if (free_slot >= 0) {
      memset(&s_buckets[free_slot], 0, sizeof(bucket_t));
      s_buckets[free_slot].user_id = user_id;
      return free_slot;
   }
   return -1;
}

/* Caller holds s_mutex. Roll the per-window counters forward if expired. */
static void roll_windows(bucket_t *b, time_t now) {
   if (now - b->minute_window_start >= 60) {
      b->minute_window_start = now;
      b->minute_count = 0;
   }
   if (now - b->hour_window_start >= 3600) {
      b->hour_window_start = now;
      b->hour_count = 0;
   }
}

bool tavily_rate_limit_check(int user_id) {
   int uid = normalize_user_id(user_id);
   time_t now = time(NULL);

   pthread_mutex_lock(&s_mutex);
   int idx = find_or_alloc_slot(uid);
   if (idx < 0) {
      /* All slots full — fail open rather than block legitimate work.
       * Personal deployments shouldn't hit this; logged once for diagnostics. */
      pthread_mutex_unlock(&s_mutex);
      OLOG_WARNING("tavily_rate_limit: bucket table full (max=%d), allowing call",
                   TAVILY_RL_MAX_USERS);
      return true;
   }

   bucket_t *b = &s_buckets[idx];
   roll_windows(b, now);

   if (b->minute_count >= TAVILY_RL_PER_MINUTE) {
      pthread_mutex_unlock(&s_mutex);
      OLOG_WARNING("tavily_rate_limit: user=%d hit minute cap (%d/%d)", user_id, b->minute_count,
                   TAVILY_RL_PER_MINUTE);
      return false;
   }
   if (b->hour_count >= TAVILY_RL_PER_HOUR) {
      pthread_mutex_unlock(&s_mutex);
      OLOG_WARNING("tavily_rate_limit: user=%d hit hour cap (%d/%d)", user_id, b->hour_count,
                   TAVILY_RL_PER_HOUR);
      return false;
   }

   b->minute_count++;
   b->hour_count++;
   pthread_mutex_unlock(&s_mutex);
   return true;
}

void tavily_rate_limit_reset(void) {
   pthread_mutex_lock(&s_mutex);
   memset(s_buckets, 0, sizeof(s_buckets));
   pthread_mutex_unlock(&s_mutex);
}

void tavily_rate_limit_remaining(int user_id, int *minute_remaining, int *hour_remaining) {
   int uid = normalize_user_id(user_id);
   time_t now = time(NULL);

   pthread_mutex_lock(&s_mutex);
   /* Read-only lookup — diagnostic queries must not consume a slot from the
    * fixed bucket table or an unauthenticated admin endpoint could exhaust
    * it. Users with no bucket yet report the full budget. */
   int idx = find_slot(uid);
   if (idx < 0) {
      pthread_mutex_unlock(&s_mutex);
      if (minute_remaining) {
         *minute_remaining = TAVILY_RL_PER_MINUTE;
      }
      if (hour_remaining) {
         *hour_remaining = TAVILY_RL_PER_HOUR;
      }
      return;
   }

   bucket_t *b = &s_buckets[idx];
   roll_windows(b, now);
   int mrem = TAVILY_RL_PER_MINUTE - b->minute_count;
   int hrem = TAVILY_RL_PER_HOUR - b->hour_count;
   pthread_mutex_unlock(&s_mutex);

   if (mrem < 0) {
      mrem = 0;
   }
   if (hrem < 0) {
      hrem = 0;
   }
   if (minute_remaining) {
      *minute_remaining = mrem;
   }
   if (hour_remaining) {
      *hour_remaining = hrem;
   }
}
