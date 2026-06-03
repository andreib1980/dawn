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
 * Messaging engine — link-code flow + async outbound.
 *
 * One-time link-code issuance/lookup/atomic-claim (the /link proof-of-control
 * handshake), the link-attempt audit log, and the detached async-send helper
 * used to confirm /link and /new from the mosquitto callback thread without
 * self-deadlocking.  Split out of messaging_engine.c; see
 * messaging_engine_internal.h and docs/MESSAGING_ENGINE_SPLIT_PLAN.md.
 */
#define AUTH_DB_INTERNAL_ALLOWED
#define MESSAGING_ENGINE_INTERNAL_ALLOWED

#include <ctype.h>
#include <fcntl.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "dawn_error.h"
#include "logging.h"
#include "messaging/messaging_engine.h"
#include "messaging/messaging_engine_internal.h"
#include "messaging/messaging_format.h"

/* =============================================================================
 * Module-local constants and types
 * ============================================================================= */

/* Crockford base32 alphabet — excludes I/L/O/U to avoid typing
 * confusion. */
static const char CROCKFORD_ALPHABET[32] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

/* Async outbound send item.  Used when we need to call drv->send_text()
 * from a thread that cannot block waiting for the response —
 * specifically, the mosquitto network callback thread that delivers
 * inbound SMS events via echo/events.  That thread is also the one that
 * delivers the echo/response message carrying ECHO's send_sms ack; if we
 * block it inside send_text waiting for that ack, we self-deadlock and
 * time out after 60s.  The normal LLM-reply path doesn't hit this because
 * the engine worker thread is the one that calls send_text, leaving
 * mosquitto's callback thread free.  The /link confirmation, however,
 * fires inline from handle_link_command which runs on the mosquitto
 * thread — hence the need for an async dispatch.
 *
 * Spawned threads are detached (no join), small stack (64 KB), and
 * outlive the engine briefly on shutdown if a send is in flight.
 * Acceptable because shutdown doesn't free the driver descriptor. */
typedef struct {
   const messaging_driver_t *drv;
   int user_id;
   char provider_address[128];
   char address_json[MESSAGING_ADDRESS_JSON_BUF_SIZE];
   /* Heap-owned, already rendered into drv->out_format by engine_send_async.
    * Heap (not a fixed buffer) so HTML escaping/expansion can't truncate a
    * tag mid-stream and trip Telegram's HTTP-400 reject.  Freed by the
    * async-send thread. */
   char *text;
} async_send_item_t;

static int generate_random_code(char *out, size_t out_buf_size) {
   if (out_buf_size < MESSAGING_LINK_CODE_LEN + 1) {
      return MESSAGING_FAILURE;
   }
   /* Read MESSAGING_LINK_CODE_LEN bytes from /dev/urandom; each byte
    * indexes into the 32-char Crockford alphabet via mod-32. */
   int fd = open("/dev/urandom", O_RDONLY);
   if (fd < 0) {
      return MESSAGING_FAILURE;
   }
   unsigned char buf[MESSAGING_LINK_CODE_LEN];
   ssize_t n = read(fd, buf, sizeof(buf));
   close(fd);
   if (n != (ssize_t)sizeof(buf)) {
      return MESSAGING_FAILURE;
   }
   for (size_t i = 0; i < MESSAGING_LINK_CODE_LEN; i++) {
      out[i] = CROCKFORD_ALPHABET[buf[i] & 0x1F];
   }
   out[MESSAGING_LINK_CODE_LEN] = '\0';
   return MESSAGING_SUCCESS;
}

static void purge_expired_link_codes(void) {
   /* Gate behind a 60-second timestamp so clustered link-issuance
    * (e.g., operator stress-testing) doesn't run a DELETE per call. */
   static time_t s_last_purge = 0;
   time_t now = time(NULL);
   if (now - s_last_purge < 60) {
      return;
   }
   s_last_purge = now;

   AUTH_DB_LOCK_OR_RETURN_VOID();
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db,
                          "DELETE FROM messaging_link_codes WHERE ROWID IN "
                          "(SELECT ROWID FROM messaging_link_codes "
                          "WHERE expires_at < ? LIMIT 100)",
                          -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_int64(stmt, 1, (int64_t)now);
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);
   }
   AUTH_DB_UNLOCK();
}

int messaging_engine_generate_link_code(int user_id,
                                        const char *provider_hint,
                                        char *code_out,
                                        size_t code_buf_size) {
   if (user_id <= 0 || !code_out || code_buf_size < MESSAGING_LINK_CODE_BUF_SIZE) {
      return MESSAGING_FAILURE;
   }
   if (!atomic_load(&s_initialized)) {
      return MESSAGING_FAILURE;
   }

   purge_expired_link_codes();

   /* Try up to 5 times to find an unused code.  At 32^8 keyspace this
    * is essentially always one-shot, but defensively retry. */
   for (int attempt = 0; attempt < 5; attempt++) {
      if (generate_random_code(code_out, code_buf_size) != MESSAGING_SUCCESS) {
         return MESSAGING_FAILURE;
      }

      AUTH_DB_LOCK_OR_FAIL();
      sqlite3_stmt *stmt = NULL;
      int rc = SQLITE_ERROR;
      if (sqlite3_prepare_v2(s_db.db,
                             "INSERT INTO messaging_link_codes "
                             "(code, user_id, provider_hint, created_at, expires_at) "
                             "VALUES (?, ?, ?, ?, ?)",
                             -1, &stmt, NULL) == SQLITE_OK) {
         time_t now = time(NULL);
         const char *hint = (provider_hint && provider_hint[0]) ? provider_hint : NULL;
         sqlite3_bind_text(stmt, 1, code_out, -1, SQLITE_STATIC);
         sqlite3_bind_int(stmt, 2, user_id);
         if (hint) {
            sqlite3_bind_text(stmt, 3, hint, -1, SQLITE_STATIC);
         } else {
            sqlite3_bind_null(stmt, 3);
         }
         sqlite3_bind_int64(stmt, 4, (int64_t)now);
         sqlite3_bind_int64(stmt, 5, (int64_t)(now + MESSAGING_LINK_TTL_SECONDS));
         rc = sqlite3_step(stmt);
         sqlite3_finalize(stmt);
      }
      AUTH_DB_UNLOCK();

      if (rc == SQLITE_DONE) {
         OLOG_INFO("messaging_engine: issued link code for user %d (hint='%s')", user_id,
                   provider_hint ? provider_hint : "");
         return MESSAGING_SUCCESS;
      }
      /* SQLITE_CONSTRAINT means the (unlikely) collision case — retry. */
   }
   return MESSAGING_FAILURE;
}

messaging_link_state_t messaging_engine_link_status(const char *code) {
   if (!code || code[0] == '\0') {
      return MESSAGING_LINK_STATE_NOT_FOUND;
   }
   messaging_link_state_t state = MESSAGING_LINK_STATE_NOT_FOUND;
   AUTH_DB_LOCK_OR_RETURN(MESSAGING_LINK_STATE_NOT_FOUND);
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db,
                          "SELECT claimed_at, expires_at FROM messaging_link_codes "
                          "WHERE code = ?",
                          -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_text(stmt, 1, code, -1, SQLITE_STATIC);
      if (sqlite3_step(stmt) == SQLITE_ROW) {
         int64_t claimed = sqlite3_column_int64(stmt, 0);
         int64_t expires = sqlite3_column_int64(stmt, 1);
         if (claimed > 0) {
            state = MESSAGING_LINK_STATE_CLAIMED;
         } else if (expires < (int64_t)time(NULL)) {
            state = MESSAGING_LINK_STATE_EXPIRED;
         } else {
            state = MESSAGING_LINK_STATE_PENDING;
         }
      }
   }
   if (stmt) {
      sqlite3_finalize(stmt);
   }
   AUTH_DB_UNLOCK();
   return state;
}

static void *async_send_thread(void *arg) {
   async_send_item_t *item = (async_send_item_t *)arg;
   if (item && item->drv && item->drv->send_text && item->text) {
      item->drv->send_text(item->user_id, item->provider_address, item->address_json, item->text);
   }
   if (item) {
      free(item->text);
   }
   free(item);
   return NULL;
}

/* Render an engine-authored system string (link / new confirmation) into the
 * driver's native format.  These are short and never split, so we render with
 * a generous single-message cap and take the first part.  Returns a heap
 * string (caller owns), or a raw strdup fallback if formatting fails. */
static char *format_system_text(const messaging_driver_t *drv, const char *text) {
   char **parts = NULL;
   size_t nparts = 0;
   char err[256] = { 0 };
   char *out = NULL;
   if (messaging_format_render_split(text, drv->out_format, 1u << 20, &parts, &nparts, err,
                                     sizeof(err)) == SUCCESS &&
       nparts >= 1) {
      out = strdup(parts[0]);
   }
   messaging_format_free_parts(parts, nparts);
   if (!out) {
      out = strdup(text);
   }
   return out;
}

void engine_send_async(const messaging_driver_t *drv,
                       int user_id,
                       const char *provider_address,
                       const char *address_json,
                       const char *text) {
   if (!drv || !drv->send_text || !text) {
      return;
   }
   if ((!provider_address || provider_address[0] == '\0') &&
       (!address_json || address_json[0] == '\0')) {
      return;
   }
   async_send_item_t *item = calloc(1, sizeof(*item));
   if (!item) {
      OLOG_WARNING("messaging: async-send alloc failed; dropping send to %s",
                   provider_address ? provider_address : "(no addr)");
      return;
   }
   item->drv = drv;
   item->user_id = user_id;
   if (provider_address) {
      snprintf(item->provider_address, sizeof(item->provider_address), "%s", provider_address);
   }
   if (address_json) {
      snprintf(item->address_json, sizeof(item->address_json), "%s", address_json);
   }
   /* Render into the driver's native format here (not at the call sites) so
    * every async confirmation — /link, Telegram /new, SMS /new — is escaped
    * and converted exactly once.  Critical for Telegram: a stray & or < in a
    * confirmation string would otherwise reach the parse_mode=HTML send and
    * get the whole message rejected. */
   item->text = format_system_text(drv, text);
   if (!item->text) {
      OLOG_WARNING("messaging: async-send format failed; dropping send to %s",
                   provider_address ? provider_address : "(no addr)");
      free(item);
      return;
   }

   pthread_attr_t attr;
   pthread_attr_init(&attr);
   pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
   pthread_attr_setstacksize(&attr, 64 * 1024);

   pthread_t tid;
   int rc = pthread_create(&tid, &attr, async_send_thread, item);
   pthread_attr_destroy(&attr);
   if (rc != 0) {
      OLOG_WARNING("messaging: async-send pthread_create failed (rc=%d); dropping send", rc);
      free(item->text);
      free(item);
   }
}

void link_attempt_log(const char *provider,
                      const char *sender_address,
                      const char *code_tried,
                      const char *result) {
   AUTH_DB_LOCK_OR_RETURN_VOID();
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db,
                          "INSERT INTO messaging_link_attempts "
                          "(provider, sender_address, code_tried, result, created_at) "
                          "VALUES (?, ?, ?, ?, ?)",
                          -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_text(stmt, 1, provider, -1, SQLITE_STATIC);
      sqlite3_bind_text(stmt, 2, sender_address, -1, SQLITE_STATIC);
      if (code_tried) {
         sqlite3_bind_text(stmt, 3, code_tried, -1, SQLITE_STATIC);
      } else {
         sqlite3_bind_null(stmt, 3);
      }
      sqlite3_bind_text(stmt, 4, result, -1, SQLITE_STATIC);
      sqlite3_bind_int64(stmt, 5, (int64_t)time(NULL));
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);
   }
   AUTH_DB_UNLOCK();
}

int handle_link_command(const char *provider, const char *sender_address, const char *code_part) {
   /* code_part points to the substring after "/link " — extract a
    * single token (whitespace-trimmed, uppercased for the lookup). */
   while (*code_part == ' ') {
      code_part++;
   }
   char code[MESSAGING_LINK_CODE_BUF_SIZE] = { 0 };
   size_t k = 0;
   for (size_t i = 0; code_part[i] != '\0' && code_part[i] != ' ' && code_part[i] != '\n' &&
                      k < MESSAGING_LINK_CODE_BUF_SIZE - 1;
        i++) {
      code[k++] = (char)toupper((unsigned char)code_part[i]);
   }
   code[k] = '\0';

   if (k != MESSAGING_LINK_CODE_LEN) {
      link_attempt_log(provider, sender_address, code, "invalid");
      OLOG_WARNING("messaging: bad /link code shape from %s:%s (len=%zu)", provider, sender_address,
                   k);
      return MESSAGING_FAILURE;
   }

   /* Atomic claim: UPDATE...WHERE code=? AND claimed_at IS NULL AND
    * expires_at>?.  changes() tells us whether we won. */
   AUTH_DB_LOCK_OR_FAIL();
   int user_id = 0;
   int claimed_rows = 0;

   /* Read row first to capture user_id and validity. */
   sqlite3_stmt *stmt = NULL;
   bool found = false;
   int64_t expires_at = 0;
   int64_t claimed_at = 0;
   if (sqlite3_prepare_v2(
           s_db.db,
           "SELECT user_id, expires_at, COALESCE(claimed_at,0) FROM messaging_link_codes "
           "WHERE code = ?",
           -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_text(stmt, 1, code, -1, SQLITE_STATIC);
      if (sqlite3_step(stmt) == SQLITE_ROW) {
         found = true;
         user_id = sqlite3_column_int(stmt, 0);
         expires_at = sqlite3_column_int64(stmt, 1);
         claimed_at = sqlite3_column_int64(stmt, 2);
      }
      sqlite3_finalize(stmt);
      stmt = NULL;
   }

   if (!found) {
      AUTH_DB_UNLOCK();
      link_attempt_log(provider, sender_address, code, "unknown");
      OLOG_WARNING("messaging: unknown /link code from %s:%s", provider, sender_address);
      return MESSAGING_FAILURE;
   }

   time_t now = time(NULL);
   if (claimed_at > 0 || expires_at < (int64_t)now) {
      AUTH_DB_UNLOCK();
      link_attempt_log(provider, sender_address, code, claimed_at > 0 ? "claimed" : "expired");
      OLOG_WARNING("messaging: %s /link code from %s:%s",
                   claimed_at > 0 ? "already-claimed" : "expired", provider, sender_address);
      return MESSAGING_FAILURE;
   }

   /* Atomic claim. */
   if (sqlite3_prepare_v2(s_db.db,
                          "UPDATE messaging_link_codes SET claimed_at = ? "
                          "WHERE code = ? AND claimed_at IS NULL AND expires_at > ?",
                          -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_int64(stmt, 1, (int64_t)now);
      sqlite3_bind_text(stmt, 2, code, -1, SQLITE_STATIC);
      sqlite3_bind_int64(stmt, 3, (int64_t)now);
      if (sqlite3_step(stmt) == SQLITE_DONE) {
         claimed_rows = sqlite3_changes(s_db.db);
      }
      sqlite3_finalize(stmt);
   }

   if (claimed_rows == 0) {
      /* Lost the atomic claim race — someone else claimed it between our
       * read and UPDATE.  Same operator-facing meaning as already-claimed. */
      AUTH_DB_UNLOCK();
      link_attempt_log(provider, sender_address, code, "claimed");
      return MESSAGING_FAILURE;
   }

   /* Build the per-provider address_json blob.  This is the single
    * source of truth for what shape each provider's row carries. */
   char address_json[MESSAGING_ADDRESS_JSON_BUF_SIZE];
   build_address_json_for(provider, sender_address, address_json, sizeof(address_json));

   /* Insert messaging_channels row.  Validate address via the driver's
    * validate_address hook (best-effort — drivers without a validator
    * accept whatever the builder produced). */
   const messaging_driver_t *drv = find_driver(provider);
   if (drv && drv->validate_address) {
      if (drv->validate_address(address_json) != SUCCESS) {
         AUTH_DB_UNLOCK();
         link_attempt_log(provider, sender_address, code, "invalid");
         return MESSAGING_FAILURE;
      }
   }

   /* INSERT the row.  Default display_name is "<provider>_<addr_short>"
    * — operator can rename via WebUI Settings (Phase 6). */
   char default_name[64];
   snprintf(default_name, sizeof(default_name), "%s_%.16s", provider, sender_address);
   if (sqlite3_prepare_v2(s_db.db,
                          "INSERT INTO messaging_channels "
                          "(user_id, provider, provider_address, address_json, "
                          "display_name, created_at) "
                          "VALUES (?, ?, ?, ?, ?, ?) "
                          "ON CONFLICT(user_id, provider, provider_address) DO UPDATE "
                          "SET is_enabled = 1",
                          -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_int(stmt, 1, user_id);
      sqlite3_bind_text(stmt, 2, provider, -1, SQLITE_STATIC);
      sqlite3_bind_text(stmt, 3, sender_address, -1, SQLITE_STATIC);
      sqlite3_bind_text(stmt, 4, address_json, -1, SQLITE_STATIC);
      sqlite3_bind_text(stmt, 5, default_name, -1, SQLITE_STATIC);
      sqlite3_bind_int64(stmt, 6, (int64_t)now);
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);
   }

   AUTH_DB_UNLOCK();
   link_attempt_log(provider, sender_address, code, "success");
   OLOG_INFO("messaging: linked %s:%s to user %d via code %s", provider, sender_address, user_id,
             code);

   /* Send confirmation back to the user via the driver.  ASYNC — this
    * code path runs on the mosquitto network callback thread for SMS
    * (echo/events delivery), and a synchronous send_text would
    * self-deadlock waiting for echo/response while the mosquitto
    * thread is busy executing us.  Reuses the same address_json shape
    * the row carries; sender_address is the typed primary key so the
    * driver can skip the JSON parse. */
   if (drv) {
      engine_send_async(
          drv, user_id, sender_address, address_json,
          "Your channel has been linked. Messages here will now reach the assistant.");
   }
   return MESSAGING_SUCCESS;
}
