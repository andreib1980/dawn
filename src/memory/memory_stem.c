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
 * Porter2 stemmer wrapper around libstemmer (Snowball).
 *
 * libstemmer is BSD-3-Clause; linked dynamically as a shared library.
 * See DEPENDENCIES.md.
 */

#include "memory/memory_stem.h"

#include <ctype.h>
#include <libstemmer.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "logging.h"
#include "memory/memory_types.h"

/* Compile-time tripwire: the stems output buffer must comfortably exceed
 * the maximum fact_text length, since Porter2 only shortens (never grows)
 * tokens and the framing replaces delimiters 1-to-1 with spaces.  If
 * MEMORY_FACT_TEXT_MAX is ever bumped above this constant, the stems
 * buffer used by callers needs to grow in lock-step. */
_Static_assert(MEMORY_FACT_STEMS_MAX >= MEMORY_FACT_TEXT_MAX,
               "MEMORY_FACT_STEMS_MAX must be >= MEMORY_FACT_TEXT_MAX so the BM25 "
               "index never silently truncates fact_text mid-token");

static struct sb_stemmer *s_stemmer = NULL;
static pthread_mutex_t s_stemmer_mutex = PTHREAD_MUTEX_INITIALIZER;
/* Atomic so the fast-path read in memory_stem_string can race against a
 * concurrent first-caller init without UB.  memory_stem_init re-checks
 * under the mutex so the flag's role is purely "have we tried init at
 * least once?" — the mutex still serializes the actual sb_stemmer_new
 * call. */
static _Atomic bool s_init_attempted = false;

/* Same delimiter set as memory_fact_search.c::tokenize_query — keeps
 * extraction-time stems and query-time stems aligned on token shape. */
static const char *const SB_DELIMS = " \t\n\r,.;:!?\"'()[]{}/-";

int memory_stem_init(void) {
   pthread_mutex_lock(&s_stemmer_mutex);
   if (s_stemmer != NULL) {
      pthread_mutex_unlock(&s_stemmer_mutex);
      return 0;
   }
   s_init_attempted = true;
   /* "english" with UTF_8 charset selects Porter2 (the "english" algorithm
    * in libstemmer 2.x is Porter2; "porter" would be Porter1). */
   s_stemmer = sb_stemmer_new("english", "UTF_8");
   if (s_stemmer == NULL) {
      OLOG_WARNING("memory_stem: sb_stemmer_new('english','UTF_8') failed — "
                   "stemming disabled, BM25 will index raw lowercased tokens");
      pthread_mutex_unlock(&s_stemmer_mutex);
      return 1;
   }
   pthread_mutex_unlock(&s_stemmer_mutex);
   return 0;
}

void memory_stem_shutdown(void) {
   pthread_mutex_lock(&s_stemmer_mutex);
   if (s_stemmer != NULL) {
      sb_stemmer_delete(s_stemmer);
      s_stemmer = NULL;
   }
   s_init_attempted = false;
   pthread_mutex_unlock(&s_stemmer_mutex);
}

/* Lowercase + stem a single token in-place into `dst` (size `dst_sz`).
 * Returns length written (excluding NUL), or 0 on failure / empty input.
 * Caller holds s_stemmer_mutex when s_stemmer != NULL. */
static size_t stem_one(const char *tok, size_t tok_len, char *dst, size_t dst_sz) {
   if (tok == NULL || tok_len == 0 || dst == NULL || dst_sz < 2)
      return 0;

   /* Lowercase first into a working buffer (libstemmer is case-sensitive
    * — Porter2 only handles lowercased input).  Sized to MEMORY_FACT_TEXT_MAX
    * so the longest legitimate fact_text fragment lands without truncation;
    * the previous 128-byte limit could cut multi-byte UTF-8 codepoints
    * (e.g., Cyrillic 2-byte sequences at byte 127) and feed malformed
    * input to libstemmer.  Excess past MEMORY_FACT_TEXT_MAX is still
    * truncated as a final safety net for any caller that feeds longer
    * input. */
   char lower[MEMORY_FACT_TEXT_MAX];
   if (tok_len >= sizeof(lower))
      tok_len = sizeof(lower) - 1;
   for (size_t i = 0; i < tok_len; i++) {
      lower[i] = (char)tolower((unsigned char)tok[i]);
   }
   lower[tok_len] = '\0';

   const char *stemmed = lower;
   int stemmed_len = (int)tok_len;

   if (s_stemmer != NULL) {
      const sb_symbol *out = sb_stemmer_stem(s_stemmer, (const sb_symbol *)lower, (int)tok_len);
      if (out != NULL) {
         stemmed = (const char *)out;
         stemmed_len = sb_stemmer_length(s_stemmer);
      }
   }

   if (stemmed_len <= 0)
      return 0;
   size_t copy = (size_t)stemmed_len;
   if (copy >= dst_sz)
      copy = dst_sz - 1;
   memcpy(dst, stemmed, copy);
   dst[copy] = '\0';
   return copy;
}

int memory_stem_string(const char *input, char *out, size_t out_sz) {
   if (out == NULL || out_sz == 0)
      return 0;
   out[0] = '\0';
   if (input == NULL || input[0] == '\0')
      return 0;

   /* Auto-init on first use so callers don't have to thread init through
    * their startup order.  Threadsafe: memory_stem_init guards itself. */
   if (!s_init_attempted) {
      (void)memory_stem_init();
   }

   /* Working copy so we can NUL-write delimiters in place for strtok_r.
    * Sized to MEMORY_FACT_TEXT_MAX so a maximal fact_text + a small slack
    * margin always lands without silent truncation; trim is still a final
    * safety net for any caller feeding longer input. */
   char buf[MEMORY_FACT_TEXT_MAX];
   size_t in_len = strlen(input);
   if (in_len >= sizeof(buf))
      in_len = sizeof(buf) - 1;
   memcpy(buf, input, in_len);
   buf[in_len] = '\0';

   pthread_mutex_lock(&s_stemmer_mutex);

   size_t out_off = 0;
   int count = 0;
   char *saveptr = NULL;
   char *tok = strtok_r(buf, SB_DELIMS, &saveptr);
   while (tok != NULL) {
      size_t tlen = strlen(tok);
      if (tlen >= 2) {
         char stem[64];
         size_t slen = stem_one(tok, tlen, stem, sizeof(stem));
         if (slen > 0) {
            size_t need = slen + (out_off > 0 ? 1 : 0);
            if (out_off + need >= out_sz)
               break;
            if (out_off > 0)
               out[out_off++] = ' ';
            memcpy(out + out_off, stem, slen);
            out_off += slen;
            count++;
         }
      }
      tok = strtok_r(NULL, SB_DELIMS, &saveptr);
   }
   out[out_off] = '\0';

   pthread_mutex_unlock(&s_stemmer_mutex);
   return count;
}

int memory_stem_count(const char *input) {
   if (input == NULL || input[0] == '\0')
      return 0;
   /* Identical tokenization to memory_stem_string but without writing
    * output.  Stays cheap so callers can size their FTS5 expressions. */
   char buf[1024];
   size_t in_len = strlen(input);
   if (in_len >= sizeof(buf))
      in_len = sizeof(buf) - 1;
   memcpy(buf, input, in_len);
   buf[in_len] = '\0';

   int count = 0;
   char *saveptr = NULL;
   char *tok = strtok_r(buf, SB_DELIMS, &saveptr);
   while (tok != NULL) {
      if (strlen(tok) >= 2)
         count++;
      tok = strtok_r(NULL, SB_DELIMS, &saveptr);
   }
   return count;
}
