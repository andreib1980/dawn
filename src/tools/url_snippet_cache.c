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
 * URL → search-snippet cache implementation. Fixed-size LRU, no malloc on
 * the hot path: each slot has fixed-size url and snippet buffers, so the
 * cache footprint is bounded and predictable.
 */

#include "tools/url_snippet_cache.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* Bound URL and snippet lengths so each entry is a fixed-size struct,
 * which keeps the cache footprint predictable on Jetson. URL_MAX matches
 * the Tavily adapter's URL cap; SNIPPET_MAX fits SEARXNG_SNIPPET_LEN (600)
 * plus the "..." truncation marker that bounded_dup() may append, plus
 * the NUL terminator — so the round trip is lossless even when the
 * search adapter signaled truncation. */
#define URL_SLOT_MAX 2048
#define SNIPPET_SLOT_MAX 604

typedef struct {
   char url[URL_SLOT_MAX];
   char snippet[SNIPPET_SLOT_MAX];
   time_t inserted_at;    /* 0 = slot empty */
   time_t last_access_at; /* For LRU eviction */
} slot_t;

static slot_t s_slots[URL_SNIPPET_CACHE_MAX_ENTRIES];
static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Caller holds s_mutex. Returns true if the slot has expired. */
static bool slot_is_expired(const slot_t *s, time_t now) {
   if (s->inserted_at == 0) {
      return true;
   }
   return (now - s->inserted_at) > URL_SNIPPET_CACHE_TTL_SEC;
}

/* Caller holds s_mutex. Returns the slot index for @p url, or -1 if not
 * present. Does NOT check expiration — caller decides. */
static int find_slot(const char *url) {
   for (int i = 0; i < URL_SNIPPET_CACHE_MAX_ENTRIES; i++) {
      if (s_slots[i].inserted_at != 0 && strcmp(s_slots[i].url, url) == 0) {
         return i;
      }
   }
   return -1;
}

/* Caller holds s_mutex. Returns index of a free slot (inserted_at==0),
 * an expired slot, or the LRU slot — in that order of preference. */
static int find_eviction_slot(time_t now) {
   /* Prefer an empty slot. */
   for (int i = 0; i < URL_SNIPPET_CACHE_MAX_ENTRIES; i++) {
      if (s_slots[i].inserted_at == 0) {
         return i;
      }
   }
   /* Then an expired slot. */
   for (int i = 0; i < URL_SNIPPET_CACHE_MAX_ENTRIES; i++) {
      if (slot_is_expired(&s_slots[i], now)) {
         return i;
      }
   }
   /* Finally, LRU. */
   int lru = 0;
   for (int i = 1; i < URL_SNIPPET_CACHE_MAX_ENTRIES; i++) {
      if (s_slots[i].last_access_at < s_slots[lru].last_access_at) {
         lru = i;
      }
   }
   return lru;
}

void url_snippet_cache_put(const char *url, const char *snippet) {
   if (!url || !url[0] || !snippet || !snippet[0]) {
      return;
   }
   time_t now = time(NULL);

   pthread_mutex_lock(&s_mutex);

   int idx = find_slot(url);
   if (idx < 0) {
      idx = find_eviction_slot(now);
   }
   slot_t *s = &s_slots[idx];
   /* snprintf truncates on overflow; caller bounds upstream so this is
    * a safety net for unusually long URLs / snippets. */
   snprintf(s->url, sizeof(s->url), "%s", url);
   snprintf(s->snippet, sizeof(s->snippet), "%s", snippet);
   s->inserted_at = now;
   s->last_access_at = now;

   pthread_mutex_unlock(&s_mutex);
}

bool url_snippet_cache_get(const char *url, char *out_snippet, size_t out_snippet_sz) {
   if (!url || !url[0] || !out_snippet || out_snippet_sz == 0) {
      return false;
   }
   time_t now = time(NULL);

   pthread_mutex_lock(&s_mutex);

   int idx = find_slot(url);
   if (idx < 0 || slot_is_expired(&s_slots[idx], now)) {
      pthread_mutex_unlock(&s_mutex);
      return false;
   }
   snprintf(out_snippet, out_snippet_sz, "%s", s_slots[idx].snippet);
   s_slots[idx].last_access_at = now;
   pthread_mutex_unlock(&s_mutex);
   return true;
}

void url_snippet_cache_clear(void) {
   pthread_mutex_lock(&s_mutex);
   memset(s_slots, 0, sizeof(s_slots));
   pthread_mutex_unlock(&s_mutex);
}

int url_snippet_cache_count(void) {
   time_t now = time(NULL);
   int count = 0;
   pthread_mutex_lock(&s_mutex);
   for (int i = 0; i < URL_SNIPPET_CACHE_MAX_ENTRIES; i++) {
      if (!slot_is_expired(&s_slots[i], now)) {
         count++;
      }
   }
   pthread_mutex_unlock(&s_mutex);
   return count;
}
