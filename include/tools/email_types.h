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
 * the project author(s). Contributions include any modifications,
 * enhancements, or additions to the project. These contributions become
 * part of the project and are adopted by the project author(s).
 *
 * Shared email types — used by both IMAP (email_client) and Gmail API (gmail_client).
 */

#ifndef EMAIL_TYPES_H
#define EMAIL_TYPES_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* Maximum emails to fetch in a single request (bounds stack usage).
 * 50 entries * ~1.3KB = ~65KB stack — well within Jetson limits. */
#define EMAIL_MAX_FETCH_RESULTS 50

/* Inbound (read) body cap — default fallback for both backends when an account
 * sets no max_body_chars override, and the upper bound the WebUI accepts for the
 * per-account override.  Read bodies are heap-allocated, so this can be large;
 * only excessive messages truncate.  Shared here (rather than in the
 * service-layer header) so both IMAP and Gmail backends reference one owner.
 * The outbound (send) cap lives in email_service.h — it is bounded by the fixed
 * email_draft_t.body[] buffer and is a service-layer concern.
 * NOTE: the auth layer cannot include tools headers, so the email_accounts
 * schema default + v55 migration mirror this value as EMAIL_DEFAULT_BODY_CHARS
 * in include/auth/auth_db_internal.h — keep the two in sync. */
#define EMAIL_MAX_READ_BODY_LEN 50000

/* Lower bound the WebUI accepts for a per-account max_body_chars override. */
#define EMAIL_MIN_READ_BODY_LEN 500

typedef struct {
   uint32_t uid;
   char message_id[192]; /* Gmail hex ID or IMAP folder:uid composite */
   char from_name[64];
   char from_addr[256];
   char subject[256];
   char date_str[32];
   time_t date;
   char preview[512];
} email_summary_t;

typedef struct {
   uint32_t uid;
   char message_id[192]; /* Gmail hex ID or IMAP folder:uid composite */
   char from_name[64];
   char from_addr[256];
   char to[256];
   char subject[256];
   char date_str[32];
   char *body;   /* Heap-allocated, caller frees via email_message_free() */
   int body_len; /* MUST equal strlen(body); callers size read buffers from it */
   int attachment_count;
   bool truncated;
} email_message_t;

typedef struct {
   char from[128];
   char subject[128];
   char text[128];
   char since[16]; /* YYYY-MM-DD, validated via strptime/strftime */
   char before[16];
   bool unread_only;     /* Only match UNSEEN messages */
   char folder[256];     /* Folder/label or normalized Gmail query fragment */
   char page_token[256]; /* Gmail pagination token (empty = first page) */
} email_search_params_t;

/**
 * @brief Free heap-allocated fields in email_message_t.
 * Nulls the body pointer after freeing.
 */
void email_message_free(email_message_t *msg);

#endif /* EMAIL_TYPES_H */
