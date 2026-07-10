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
 * Phone contact resolver — turns a user-supplied "target" (a phone number or
 * a spoken/typed contact name) into a dialable number, and, when the name is
 * ambiguous or only a near-miss, into a list of candidates to confirm rather
 * than silently guessing.  This is the safety net for the DANGEROUS phone
 * tool: it never picks for the user when confidence is low — it surfaces the
 * options so the LLM/user can confirm.  Shared by the call and SMS paths.
 */

#ifndef PHONE_CONTACTS_H
#define PHONE_CONTACTS_H

#include <stddef.h>

/* Max candidates surfaced in an ambiguous / did-you-mean list. */
#define PHONE_CONTACTS_MAX_CANDIDATES 5

typedef enum {
   PHONE_RESOLVE_NUMBER = 0, /* input was a dialable phone number */
   PHONE_RESOLVE_UNIQUE,     /* exactly one contact matched the name */
   PHONE_RESOLVE_AMBIGUOUS,  /* multiple contacts matched the name */
   PHONE_RESOLVE_SUGGEST,    /* no direct match; fuzzy near-misses found */
   PHONE_RESOLVE_BAD_NUMBER, /* input looked numeric but is not a valid number,
                                or the matched contact has no valid number on file */
   PHONE_RESOLVE_NONE        /* nothing matched */
} phone_resolve_kind_t;

typedef struct {
   char name[64];
   char number[24];
   char label[32]; /* "mobile", "work", ... or empty */
} phone_candidate_t;

typedef struct {
   phone_resolve_kind_t kind;
   char number[24]; /* set for NUMBER and UNIQUE */
   char name[64];   /* set for UNIQUE (empty for NUMBER) */
   phone_candidate_t candidates[PHONE_CONTACTS_MAX_CANDIDATES]; /* AMBIGUOUS / SUGGEST */
   int candidate_count;
} phone_resolve_t;

/**
 * @brief Resolve a phone "target" (number or contact name) to a dial decision.
 *
 * Tiered, confidence-ordered:
 *   1. Looks like a number  -> validate + normalize -> PHONE_RESOLVE_NUMBER.
 *   2. Exactly one contact name substring-match -> PHONE_RESOLVE_UNIQUE.
 *   3. Several contact name matches -> PHONE_RESOLVE_AMBIGUOUS (candidates).
 *   4. No substring match, but fuzzy (token + edit-distance) near-misses
 *      above threshold -> PHONE_RESOLVE_SUGGEST (candidates, best first).
 *   5. Nothing plausible -> PHONE_RESOLVE_NONE.
 *
 * Never returns a number for tiers 3-5: the caller must confirm/disambiguate.
 *
 * @param user_id User whose contacts are searched.
 * @param input   Target string (number or name); NULL/empty -> PHONE_RESOLVE_NONE.
 * @param out     Result (always fully initialized on return; must be non-NULL).
 */
void phone_contacts_resolve(int user_id, const char *input, phone_resolve_t *out);

/**
 * @brief Format a user-facing disambiguation / not-found message.
 *
 * For AMBIGUOUS / SUGGEST / NONE results, builds the text the phone tool
 * returns to the LLM so it can ask the user to pick (identical wording whether
 * produced at preview time or at dial time).  A NUMBER/UNIQUE result yields a
 * generic fallback string (callers should not reach here for those).
 *
 * @param input    Original target string (for echoing in the message).
 * @param r        Resolve result to describe.
 * @param buf      Output buffer.
 * @param buf_size Size of @p buf.
 */
void phone_contacts_format_disambiguation(const char *input,
                                          const phone_resolve_t *r,
                                          char *buf,
                                          size_t buf_size);

#endif /* PHONE_CONTACTS_H */
