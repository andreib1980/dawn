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
 * Phone contact resolver — see phone_contacts.h.  Turns a call/SMS target
 * into a dial decision, surfacing candidates (never a silent guess) when the
 * name is ambiguous or only a fuzzy near-miss.
 */

#include "tools/phone_contacts.h"

#include <stdio.h>
#include <string.h>

#include "core/str_fuzzy.h"
#include "memory/contacts_db.h"
#include "tools/phone_db.h" /* phone_number_normalize */

/* A name must score at least this (max of token-overlap and edit-distance
 * similarity) against a contact to be offered as a "did you mean" suggestion.
 * The user still confirms, so this favours recall — but not so low that every
 * contact is offered.  A shared first name plus a near-miss surname clears it
 * comfortably (e.g. "shelley curzy" vs "shelley kersey"). */
#define PHONE_SUGGEST_THRESHOLD 50

/* Bound the fuzzy scan so a very large address book can't stall a call. */
#define PHONE_FUZZY_SCAN_MAX 500
#define PHONE_FUZZY_PAGE 25

/* Mirrors the accepted phone-number punctuation of resolve-by-number. */
static int number_is_valid(const char *input) {
   size_t len = strlen(input);
   if (len < 3 || len > 20) {
      return 0;
   }
   for (size_t i = 0; i < len; i++) {
      char c = input[i];
      if (c != '+' && c != '-' && c != ' ' && c != '(' && c != ')' && c != '.' &&
          !(c >= '0' && c <= '9')) {
         return 0;
      }
   }
   return 1;
}

/* A normalized number is dialable only if it carries at least a few digits —
 * guards against a contact whose stored "number" is junk normalizing to "" or
 * "+".  E.164 is far longer, so 3 is a permissive floor. */
static int normalized_number_ok(const char *number) {
   int digits = 0;
   for (const char *p = number; *p; p++) {
      if (*p >= '0' && *p <= '9') {
         digits++;
      }
   }
   return digits >= 3;
}

/* Best fuzzy score of @p name against the (already-lowercased) needle.  Runs
 * the cheap token scorer first and only pays for the edit-distance DP when the
 * token score hasn't already cleared the suggest threshold. */
static int score_name(const char *needle_lower, const char *name) {
   char lower[128];
   str_fuzzy_tolower(lower, name, sizeof(lower));
   int score = str_fuzzy_score(lower, needle_lower);
   if (score < PHONE_SUGGEST_THRESHOLD) {
      int ratio = str_fuzzy_ratio(lower, needle_lower);
      if (ratio > score) {
         score = ratio;
      }
   }
   return score;
}

/* True if this normalized number already sits in the candidate list — avoids
 * listing the same person twice when several name rows share a number. */
static int candidate_has_number(const phone_resolve_t *r, const char *number) {
   for (int i = 0; i < r->candidate_count; i++) {
      if (strcmp(r->candidates[i].number, number) == 0) {
         return 1;
      }
   }
   return 0;
}

static void candidate_from_contact(phone_candidate_t *dst, const contact_result_t *src) {
   snprintf(dst->name, sizeof(dst->name), "%s", src->entity_name);
   phone_number_normalize(src->value, dst->number, sizeof(dst->number));
   snprintf(dst->label, sizeof(dst->label), "%s", src->label);
}

/* Fuzzy fallback: scan the user's phone contacts, keep the best matches above
 * threshold (sorted best-first, deduped by number).  Fills out->candidates and
 * sets kind to SUGGEST (matches found) or NONE. */
static void fuzzy_suggest(int user_id, const char *input, phone_resolve_t *out) {
   char needle_lower[128];
   str_fuzzy_tolower(needle_lower, input, sizeof(needle_lower));

   int scores[PHONE_CONTACTS_MAX_CANDIDATES] = { 0 };

   contact_result_t page[PHONE_FUZZY_PAGE];
   int offset = 0;
   while (offset < PHONE_FUZZY_SCAN_MAX) {
      int got = 0;
      if (contacts_list(user_id, "phone", page, PHONE_FUZZY_PAGE, offset, &got) != 0 || got <= 0) {
         break;
      }

      for (int i = 0; i < got; i++) {
         /* Score the display name; if it misses, try the canonical name too
          * (the substring tier matches on canonical_name, so scoring only
          * entity_name here would leave a recall gap when they differ). */
         int score = score_name(needle_lower, page[i].entity_name);
         if (score < PHONE_SUGGEST_THRESHOLD &&
             strcmp(page[i].entity_name, page[i].canonical_name) != 0) {
            int cscore = score_name(needle_lower, page[i].canonical_name);
            if (cscore > score) {
               score = cscore;
            }
         }
         if (score < PHONE_SUGGEST_THRESHOLD) {
            continue;
         }

         char number[24];
         phone_number_normalize(page[i].value, number, sizeof(number));
         if (candidate_has_number(out, number)) {
            continue;
         }

         /* Insert into the best-first top-K (array is small; linear is fine). */
         int pos = out->candidate_count;
         if (pos >= PHONE_CONTACTS_MAX_CANDIDATES) {
            if (score <= scores[PHONE_CONTACTS_MAX_CANDIDATES - 1]) {
               continue;
            }
            pos = PHONE_CONTACTS_MAX_CANDIDATES - 1; /* displace the weakest */
         } else {
            out->candidate_count++;
         }
         while (pos > 0 && scores[pos - 1] < score) {
            scores[pos] = scores[pos - 1];
            out->candidates[pos] = out->candidates[pos - 1];
            pos--;
         }
         scores[pos] = score;
         candidate_from_contact(&out->candidates[pos], &page[i]);
      }

      if (got < PHONE_FUZZY_PAGE) {
         break; /* last page */
      }
      offset += got;
   }

   out->kind = out->candidate_count > 0 ? PHONE_RESOLVE_SUGGEST : PHONE_RESOLVE_NONE;
}

void phone_contacts_resolve(int user_id, const char *input, phone_resolve_t *out) {
   if (!out) {
      return;
   }
   memset(out, 0, sizeof(*out));
   out->kind = PHONE_RESOLVE_NONE;

   if (!input || input[0] == '\0') {
      return;
   }

   /* Tier 1: looks like a number.  Digit-led but malformed is a bad number,
    * not a name — say so rather than "no contact found". */
   if (input[0] == '+' || (input[0] >= '0' && input[0] <= '9')) {
      if (number_is_valid(input)) {
         phone_number_normalize(input, out->number, sizeof(out->number));
         out->kind = PHONE_RESOLVE_NUMBER;
      } else {
         out->kind = PHONE_RESOLVE_BAD_NUMBER;
      }
      return;
   }

   /* Tiers 2-3: substring name match via contacts (LIKE %input%).  A failed
    * lookup is treated as no-match (fall through to fuzzy), not an error. */
   contact_result_t found[PHONE_CONTACTS_MAX_CANDIDATES];
   int count = 0;
   if (contacts_find(user_id, input, "phone", found, PHONE_CONTACTS_MAX_CANDIDATES, &count) != 0) {
      count = 0;
   }

   if (count == 1) {
      phone_number_normalize(found[0].value, out->number, sizeof(out->number));
      snprintf(out->name, sizeof(out->name), "%s", found[0].entity_name);
      out->kind = normalized_number_ok(out->number) ? PHONE_RESOLVE_UNIQUE
                                                    : PHONE_RESOLVE_BAD_NUMBER;
      return;
   }
   if (count > 1) {
      for (int i = 0; i < count; i++) {
         char number[24];
         phone_number_normalize(found[i].value, number, sizeof(number));
         if (candidate_has_number(out, number)) {
            continue;
         }
         candidate_from_contact(&out->candidates[out->candidate_count++], &found[i]);
      }
      /* Dedup may collapse to a single distinct number (same person, listed
       * twice) — that's really a unique match, so treat it as one. */
      if (out->candidate_count == 1) {
         snprintf(out->number, sizeof(out->number), "%s", out->candidates[0].number);
         snprintf(out->name, sizeof(out->name), "%s", out->candidates[0].name);
         out->kind = normalized_number_ok(out->number) ? PHONE_RESOLVE_UNIQUE
                                                       : PHONE_RESOLVE_BAD_NUMBER;
         out->candidate_count = 0;
      } else {
         out->kind = PHONE_RESOLVE_AMBIGUOUS;
      }
      return;
   }

   /* Tier 4-5: no substring match — fuzzy near-miss suggestions. */
   fuzzy_suggest(user_id, input, out);
}

/* Append "Name (+1number)[, label]" for each candidate, "; "-separated. */
static void append_candidate_list(const phone_resolve_t *r, char *buf, size_t buf_size) {
   size_t len = strnlen(buf, buf_size);
   for (int i = 0; i < r->candidate_count && len < buf_size; i++) {
      const phone_candidate_t *c = &r->candidates[i];
      int n;
      if (c->label[0]) {
         n = snprintf(buf + len, buf_size - len, "%s%s (%s, %s)", i == 0 ? "" : "; ", c->name,
                      c->number, c->label);
      } else {
         n = snprintf(buf + len, buf_size - len, "%s%s (%s)", i == 0 ? "" : "; ", c->name,
                      c->number);
      }
      if (n < 0 || (size_t)n >= buf_size - len) {
         break; /* error or truncated — stop before over-counting len */
      }
      len += (size_t)n;
   }
}

void phone_contacts_format_disambiguation(const char *input,
                                          const phone_resolve_t *r,
                                          char *buf,
                                          size_t buf_size) {
   if (!buf || buf_size == 0) {
      return;
   }
   if (!r) {
      snprintf(buf, buf_size, "Error: could not resolve that contact.");
      return;
   }
   const char *who = (input && input[0]) ? input : "that contact";

   switch (r->kind) {
      case PHONE_RESOLVE_AMBIGUOUS:
         snprintf(buf, buf_size, "Multiple contacts match '%s' — which one? ", who);
         append_candidate_list(r, buf, buf_size);
         break;
      case PHONE_RESOLVE_SUGGEST:
         snprintf(buf, buf_size, "No exact contact for '%s'. Did you mean: ", who);
         append_candidate_list(r, buf, buf_size);
         {
            size_t len = strnlen(buf, buf_size);
            snprintf(buf + len, buf_size - len,
                     "? If so, ask me to call that name or number; otherwise give me a number to "
                     "dial.");
         }
         break;
      case PHONE_RESOLVE_BAD_NUMBER:
         if (r->name[0]) {
            snprintf(buf, buf_size,
                     "The number on file for %s isn't a valid phone number. Give me a number to "
                     "dial instead.",
                     r->name);
         } else {
            snprintf(buf, buf_size,
                     "'%s' isn't a valid phone number. Give me a valid number or a contact name.",
                     who);
         }
         break;
      case PHONE_RESOLVE_NONE:
         snprintf(buf, buf_size,
                  "No contact named '%s' found. Give me a phone number to dial, or add them to "
                  "contacts first.",
                  who);
         break;
      default:
         snprintf(buf, buf_size, "Could not resolve '%s' to a phone number.", who);
         break;
   }
}
