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
 * Entity-graph candidate retrieval — Phase 1A (fact-linked relations).
 * See memory_graph_retrieval.h for the pipeline contract.
 */

#include "memory/memory_graph_retrieval.h"

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "config/dawn_config.h"
#include "dawn_error.h"
#include "logging.h"
#include "memory/memory_db.h"
#include "memory/memory_db_entities.h"
#include "memory/memory_types.h"

/* Question-starter words.  When the first sentence-initial capitalized
 * token is one of these, we skip it — it's never a proper noun in
 * question context.  Other capitalized tokens later in the query are
 * always considered (a proper noun can follow a question-starter, e.g.
 * "What did John say?"). */
static const char *const QUESTION_STARTERS[] = {
   "What", "Why", "How",  "Who", "When", "Where", "Which", "Would", "Could", "Should", "Is",
   "Are",  "Was", "Were", "Did", "Does", "Do",    "Has",   "Have",  "Had",   "Will",   NULL,
};

/* Minimum proper-noun length to count as a candidate.  3 covers common
 * names ("Tim", "Sam") without admitting noise like "U.S.". */
#define MIN_CANDIDATE_LEN 3

/* Per-candidate scratch buffer.  MEMORY_ENTITY_NAME_MAX covers any
 * canonical_name in memory_entities; the candidate itself can't be
 * longer than that or it wouldn't resolve. */
#define CANDIDATE_BUF_LEN MEMORY_ENTITY_NAME_MAX

static bool token_is_question_starter(const char *tok, size_t len) {
   for (int i = 0; QUESTION_STARTERS[i] != NULL; i++) {
      if (strlen(QUESTION_STARTERS[i]) == len && strncmp(QUESTION_STARTERS[i], tok, len) == 0) {
         return true;
      }
   }
   return false;
}

/* Inside-token character predicate: letters, digits, apostrophe, hyphen.
 * Apostrophe + hyphen admit compound names like "O'Brien" and
 * "Mary-Anne".  We strip a trailing apostrophe-s ("John's") at
 * normalize time so possessives resolve. */
static bool is_token_char(unsigned char c) {
   return isalnum(c) || c == '\'' || c == '-';
}

/* Lowercase + possessive strip in one pass.  Writes to @p out (capacity
 * @p out_cap, must be > len + 1).  Returns the written length.
 *
 * "John's" → "john"
 * "Mary-Anne's" → "mary-anne"
 * "O'Brien" → "o'brien"
 */
static size_t normalize_candidate(const char *src, size_t len, char *out, size_t out_cap) {
   if (out_cap == 0)
      return 0;
   /* Strip trailing 's" — case-insensitive match for either apostrophe.
    * We check before the lower-case pass so we don't have to deal with
    * unicode curly quotes here (LoCoMo is ASCII). */
   if (len >= 2 && src[len - 1] == 's' && src[len - 2] == '\'') {
      len -= 2;
   }
   if (len >= out_cap)
      len = out_cap - 1;
   for (size_t i = 0; i < len; i++) {
      out[i] = (char)tolower((unsigned char)src[i]);
   }
   out[len] = '\0';
   return len;
}

static bool seed_set_contains(const int64_t *set, int set_count, int64_t id) {
   for (int i = 0; i < set_count; i++) {
      if (set[i] == id)
         return true;
   }
   return false;
}

int memory_graph_extract_seed_entities(int user_id,
                                       const char *query,
                                       int64_t *out_entity_ids,
                                       int max,
                                       int *out_count) {
   if (out_count != NULL)
      *out_count = 0;
   if (out_entity_ids == NULL || max <= 0)
      return FAILURE;
   if (query == NULL || query[0] == '\0')
      return SUCCESS;

   int seed_count = 0;
   bool first_token = true;
   const char *p = query;
   const char *end = query + strlen(query);

   while (p < end && seed_count < max) {
      /* Skip non-token chars */
      while (p < end && !is_token_char((unsigned char)*p))
         p++;
      if (p >= end)
         break;
      const char *tok_start = p;
      while (p < end && is_token_char((unsigned char)*p))
         p++;
      size_t tok_len = (size_t)(p - tok_start);
      bool was_first = first_token;
      first_token = false;

      /* Filter: capitalized AND length >= MIN_CANDIDATE_LEN.  The
       * apostrophe-s suffix counts toward length here — "Tim's" passes
       * the length gate at 5 even though we'll strip to 3 when
       * normalizing.  That's deliberate (we want to match "Tim"). */
      if (tok_len < MIN_CANDIDATE_LEN)
         continue;
      if (!isupper((unsigned char)tok_start[0]))
         continue;
      if (was_first && token_is_question_starter(tok_start, tok_len)) {
         /* Sentence-initial question-starter — skip as candidate. */
         continue;
      }

      char canon[CANDIDATE_BUF_LEN];
      size_t canon_len = normalize_candidate(tok_start, tok_len, canon, sizeof(canon));
      if (canon_len < MIN_CANDIDATE_LEN)
         continue;

      /* Resolve via exact canonical-name match.  The entity-merge work
       * (May 2026) canonicalizes names to lowercase at insert time, so
       * the lookup is case-equal after normalize.  No fuzzy path here —
       * v1 keeps the false-positive surface tight; expand later if
       * cat-3 coverage proves insufficient. */
      memory_entity_t e = { 0 };
      int rc = memory_db_entity_get_by_name(user_id, canon, &e);
      if (rc != MEMORY_DB_SUCCESS)
         continue;
      if (e.id <= 0 || e.user_id != user_id)
         continue;

      /* Resolve canonical_id chain — if the matched row is a soft alias
       * (canonical_id != 0 in the entity-merge schema), seed against
       * the class canonical so a 1-hop expansion sees the full
       * equivalence class's relations.  Phase 2's memory_db_relation_
       * fact_ids_for_entity walks by entity_id directly; without this
       * step a query that uses an alias canonical name (e.g., "Kris"
       * when "Kristopher Kersey" is the canonical) would miss relations
       * stored against the canonical. */
      /* Skip — Phase 1 entity-merge keeps relations on whichever entity
       * the resolver chose at extraction time, and the alias_link
       * mechanism doesn't migrate relations.  Querying by the
       * resolved row's own id IS what we want.  Comment kept as a
       * design note for future maintainers. */

      if (seed_set_contains(out_entity_ids, seed_count, e.id))
         continue;
      out_entity_ids[seed_count++] = e.id;
   }

   if (out_count != NULL)
      *out_count = seed_count;
   return SUCCESS;
}

int memory_graph_expand_fact_linked(int user_id,
                                    const int64_t *seed_entity_ids,
                                    int seed_count,
                                    memory_fact_t *out_facts,
                                    float *out_scores,
                                    int max,
                                    int *out_count) {
   if (out_count != NULL)
      *out_count = 0;
   if (out_facts == NULL || out_scores == NULL || max <= 0)
      return FAILURE;
   if (seed_count <= 0 || seed_entity_ids == NULL)
      return SUCCESS;

   const float bonus = g_config.memory.graph_retrieval.entity_grounding_bonus;

   /* Per-seed fan-out budget — divide the total cap across seeds,
    * leaving the first seed with the bigger share when not evenly
    * divisible.  Caller passes the global cap (typically
    * graph_retrieval.max_facts_per_query, default 30) so we don't
    * explode for high-degree entities like John/Tim/Caroline. */
   int per_seed = max / seed_count;
   if (per_seed < 1)
      per_seed = 1;
   int leftover = max - (per_seed * seed_count);

   /* Dedup set: we walk seeds in order and skip fact_ids already
    * surfaced by an earlier seed.  out_count is the high-water mark. */
   int produced = 0;
   int64_t fact_id_buf[64]; /* Per-seed batch. */

   for (int s = 0; s < seed_count && produced < max; s++) {
      int seed_cap = per_seed + (s == 0 ? leftover : 0);
      if (seed_cap > (int)(sizeof(fact_id_buf) / sizeof(fact_id_buf[0])))
         seed_cap = (int)(sizeof(fact_id_buf) / sizeof(fact_id_buf[0]));
      if (produced + seed_cap > max)
         seed_cap = max - produced;
      if (seed_cap <= 0)
         break;

      int got = 0;
      int rc = memory_db_relation_fact_ids_for_entity(user_id, seed_entity_ids[s], fact_id_buf,
                                                      seed_cap, &got);
      if (rc != MEMORY_DB_SUCCESS)
         continue;

      for (int i = 0; i < got && produced < max; i++) {
         int64_t fid = fact_id_buf[i];
         if (fid <= 0)
            continue;
         /* Dedup against already-produced facts. */
         bool seen = false;
         for (int j = 0; j < produced; j++) {
            if (out_facts[j].id == fid) {
               seen = true;
               break;
            }
         }
         if (seen)
            continue;

         /* memory_db_fact_get is now user-scoped at the SQL layer
          * (CWE-639 defense-in-depth) — a relations row pointing at
          * another user's fact returns MEMORY_DB_NOT_FOUND here, which
          * would still indicate a schema invariant violation but no
          * longer requires a post-fetch user_id mismatch check. */
         memory_fact_t fact = { 0 };
         if (memory_db_fact_get(fid, user_id, &fact) != MEMORY_DB_SUCCESS)
            continue;

         out_facts[produced] = fact;
         out_scores[produced] = bonus;
         produced++;
      }
   }

   if (out_count != NULL)
      *out_count = produced;
   return SUCCESS;
}

/* Set-membership helper for the expanded seed table. */
