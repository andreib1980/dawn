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
 * Messaging engine implementation.  See
 * include/messaging/messaging_engine.h for the public API and
 * docs/MESSAGING_CHANNELS_DESIGN.md for the design rationale.
 */
#define AUTH_DB_INTERNAL_ALLOWED

#include "messaging/messaging_engine.h"

#include <ctype.h>
#include <fcntl.h>
#include <json-c/json.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "core/rate_limiter.h"
#include "core/session_manager.h"
#include "core/text_input_dispatch.h"
#include "dawn_error.h"
#include "logging.h"

/* =============================================================================
 * Internal constants and types
 * ============================================================================= */

#define MESSAGING_MAX_DRIVERS 4
#define MESSAGING_INBOUND_QUEUE_DEPTH 32
#define MESSAGING_MAX_SESSIONS 64
#define MESSAGING_MAX_BODY_LEN 4096
#define MESSAGING_LINK_BODY_CAP 256
#define MESSAGING_RL_KEY_SIZE 96 /* "provider:address" composite */

/* Crockford base32 alphabet — excludes I/L/O/U to avoid typing
 * confusion. */
static const char CROCKFORD_ALPHABET[32] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

typedef struct {
   char provider[16];
   char provider_address[128];
   char sender_display[64];
   char *body; /* heap-allocated */
   int64_t timestamp;
} inbound_item_t;

typedef struct {
   char provider[16];
   char provider_address[128];
   session_t *session;
   time_t last_used;
} session_slot_t;

/* =============================================================================
 * Module state
 * ============================================================================= */

static atomic_bool s_initialized = ATOMIC_VAR_INIT(false);
static atomic_bool s_shutdown_requested = ATOMIC_VAR_INIT(false);

static const messaging_driver_t *s_drivers[MESSAGING_MAX_DRIVERS];
static size_t s_num_drivers = 0;
static pthread_mutex_t s_drivers_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Inbound queue (bounded ring buffer). */
static inbound_item_t *s_inbound_queue[MESSAGING_INBOUND_QUEUE_DEPTH];
static size_t s_inbound_head = 0;
static size_t s_inbound_tail = 0;
static size_t s_inbound_count = 0;
static pthread_mutex_t s_inbound_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_inbound_cond = PTHREAD_COND_INITIALIZER;

static pthread_t s_worker_thread;
static bool s_worker_started = false;

/* In-memory session map: (provider, provider_address) → session_t*.
 * Linear scan; v1 scale is small (one slot per active conversation per
 * user, typically < 10). */
static session_slot_t s_session_slots[MESSAGING_MAX_SESSIONS];
static pthread_mutex_t s_session_slots_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Rate limiters. */
static rate_limit_entry_t s_inbound_link_entries[64];
static rate_limit_entry_t s_inbound_general_entries[128];
static rate_limit_entry_t s_outbound_per_user_entries[64];

static rate_limiter_t s_inbound_link_limiter;
static rate_limiter_t s_inbound_general_limiter;
static rate_limiter_t s_outbound_per_user_limiter;

/* =============================================================================
 * Forward declarations
 * ============================================================================= */

static void *worker_thread(void *arg);
static int enqueue_inbound(const char *provider,
                           const char *provider_address,
                           const char *sender_display,
                           const char *body,
                           int64_t timestamp);
static int handle_link_command(const char *provider,
                               const char *sender_address,
                               const char *code_part);
static int engine_inbound_dispatch(const char *provider,
                                   const char *provider_address,
                                   const char *sender_display,
                                   const char *body,
                                   int64_t timestamp);
static const messaging_driver_t *find_driver(const char *name);
static int lookup_channel_user(const char *provider,
                               const char *provider_address,
                               char *display_name_out,
                               size_t display_name_buf_size);
static session_t *get_or_create_messaging_session(const char *provider,
                                                  const char *provider_address);
static void process_inbound(inbound_item_t *item);
static void link_attempt_log(const char *provider,
                             const char *sender_address,
                             const char *code_tried,
                             const char *result);
static void purge_expired_link_codes(void);

/* =============================================================================
 * Init / shutdown
 * ============================================================================= */

int messaging_engine_init(void) {
   if (atomic_load(&s_initialized)) {
      return MESSAGING_SUCCESS;
   }

   memset(s_drivers, 0, sizeof(s_drivers));
   memset(s_inbound_queue, 0, sizeof(s_inbound_queue));
   memset(s_session_slots, 0, sizeof(s_session_slots));

   /* Rate limiters.  Budgets per docs/MESSAGING_CHANNELS_DESIGN.md §12. */
   rate_limiter_config_t link_cfg = { .max_count = 5, .window_sec = 600, .slot_count = 64 };
   rate_limiter_config_t general_cfg = { .max_count = 60, .window_sec = 600, .slot_count = 128 };
   rate_limiter_config_t outbound_cfg = { .max_count = 10, .window_sec = 60, .slot_count = 64 };

   memset(s_inbound_link_entries, 0, sizeof(s_inbound_link_entries));
   memset(s_inbound_general_entries, 0, sizeof(s_inbound_general_entries));
   memset(s_outbound_per_user_entries, 0, sizeof(s_outbound_per_user_entries));

   rate_limiter_init(&s_inbound_link_limiter, s_inbound_link_entries, &link_cfg);
   rate_limiter_init(&s_inbound_general_limiter, s_inbound_general_entries, &general_cfg);
   rate_limiter_init(&s_outbound_per_user_limiter, s_outbound_per_user_entries, &outbound_cfg);

   /* Worker thread for the inbound drain. */
   atomic_store(&s_shutdown_requested, false);
   if (pthread_create(&s_worker_thread, NULL, worker_thread, NULL) != 0) {
      OLOG_ERROR("messaging_engine: failed to spawn worker thread");
      return MESSAGING_FAILURE;
   }
   s_worker_started = true;

   atomic_store(&s_initialized, true);
   OLOG_INFO("messaging_engine: initialized (worker thread running, "
             "drivers will self-register)");
   return MESSAGING_SUCCESS;
}

void messaging_engine_shutdown(void) {
   if (!atomic_load(&s_initialized)) {
      return;
   }

   atomic_store(&s_shutdown_requested, true);

   /* Wake worker so it can exit. */
   pthread_mutex_lock(&s_inbound_mutex);
   pthread_cond_broadcast(&s_inbound_cond);
   pthread_mutex_unlock(&s_inbound_mutex);

   if (s_worker_started) {
      pthread_join(s_worker_thread, NULL);
      s_worker_started = false;
   }

   /* Release any retained sessions. */
   pthread_mutex_lock(&s_session_slots_mutex);
   for (size_t i = 0; i < MESSAGING_MAX_SESSIONS; i++) {
      if (s_session_slots[i].session) {
         session_release(s_session_slots[i].session);
         memset(&s_session_slots[i], 0, sizeof(session_slot_t));
      }
   }
   pthread_mutex_unlock(&s_session_slots_mutex);

   /* Drain remaining items in the queue. */
   pthread_mutex_lock(&s_inbound_mutex);
   while (s_inbound_count > 0) {
      inbound_item_t *item = s_inbound_queue[s_inbound_head];
      s_inbound_head = (s_inbound_head + 1) % MESSAGING_INBOUND_QUEUE_DEPTH;
      s_inbound_count--;
      if (item) {
         free(item->body);
         free(item);
      }
   }
   pthread_mutex_unlock(&s_inbound_mutex);

   atomic_store(&s_initialized, false);
   OLOG_INFO("messaging_engine: shutdown complete");
}

/* =============================================================================
 * Driver registry
 * ============================================================================= */

int messaging_engine_register_driver(const messaging_driver_t *driver) {
   if (!driver || !driver->name || !driver->init || !driver->send_text) {
      return MESSAGING_FAILURE;
   }
   if (!atomic_load(&s_initialized)) {
      OLOG_ERROR("messaging_engine: register_driver called before init");
      return MESSAGING_FAILURE;
   }

   pthread_mutex_lock(&s_drivers_mutex);

   /* Reject duplicates. */
   for (size_t i = 0; i < s_num_drivers; i++) {
      if (strcmp(s_drivers[i]->name, driver->name) == 0) {
         pthread_mutex_unlock(&s_drivers_mutex);
         OLOG_WARNING("messaging_engine: driver '%s' already registered", driver->name);
         return MESSAGING_FAILURE;
      }
   }

   if (s_num_drivers >= MESSAGING_MAX_DRIVERS) {
      pthread_mutex_unlock(&s_drivers_mutex);
      OLOG_ERROR("messaging_engine: registry full (max %d)", MESSAGING_MAX_DRIVERS);
      return MESSAGING_FAILURE;
   }

   s_drivers[s_num_drivers++] = driver;
   pthread_mutex_unlock(&s_drivers_mutex);

   /* Plumb the engine's inbound dispatch into the driver before its
    * init runs (so the listener thread can call back immediately). */
   if (driver->register_inbound_cb) {
      driver->register_inbound_cb(engine_inbound_dispatch);
   }

   OLOG_INFO("messaging_engine: registered driver '%s'", driver->name);
   return MESSAGING_SUCCESS;
}

static const messaging_driver_t *find_driver(const char *name) {
   if (!name) {
      return NULL;
   }
   const messaging_driver_t *result = NULL;
   pthread_mutex_lock(&s_drivers_mutex);
   for (size_t i = 0; i < s_num_drivers; i++) {
      if (strcmp(s_drivers[i]->name, name) == 0) {
         result = s_drivers[i];
         break;
      }
   }
   pthread_mutex_unlock(&s_drivers_mutex);
   return result;
}

/* =============================================================================
 * Channel lookup
 * ============================================================================= */

static int lookup_channel_user(const char *provider,
                               const char *provider_address,
                               char *display_name_out,
                               size_t display_name_buf_size) {
   if (!provider || !provider_address) {
      return 0;
   }

   AUTH_DB_LOCK_OR_RETURN(0);
   int user_id = 0;
   sqlite3_stmt *stmt = NULL;
   const char *sql = "SELECT user_id, COALESCE(display_name,'') FROM messaging_channels "
                     "WHERE provider = ? AND provider_address = ? AND is_enabled = 1 LIMIT 1";
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_text(stmt, 1, provider, -1, SQLITE_STATIC);
      sqlite3_bind_text(stmt, 2, provider_address, -1, SQLITE_STATIC);
      if (sqlite3_step(stmt) == SQLITE_ROW) {
         user_id = sqlite3_column_int(stmt, 0);
         if (display_name_out && display_name_buf_size > 0) {
            const unsigned char *dn = sqlite3_column_text(stmt, 1);
            snprintf(display_name_out, display_name_buf_size, "%s", dn ? (const char *)dn : "");
         }
      }
   }
   if (stmt) {
      sqlite3_finalize(stmt);
   }
   AUTH_DB_UNLOCK();
   return user_id;
}

/* Look up channel by (user_id, display_name) — case-insensitive name
 * match.  Returns the address_json (caller frees) and the provider via
 * out-param.  Returns NULL if not found. */
static char *lookup_channel_address(int user_id,
                                    const char *channel_name,
                                    char *provider_out,
                                    size_t provider_buf_size) {
   if (user_id <= 0 || !channel_name) {
      return NULL;
   }

   AUTH_DB_LOCK_OR_RETURN(NULL);
   char *address_json = NULL;
   sqlite3_stmt *stmt = NULL;
   const char *sql = "SELECT provider, address_json FROM messaging_channels "
                     "WHERE user_id = ? AND is_enabled = 1 AND "
                     "LOWER(COALESCE(display_name,'')) = LOWER(?) LIMIT 1";
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_int(stmt, 1, user_id);
      sqlite3_bind_text(stmt, 2, channel_name, -1, SQLITE_STATIC);
      if (sqlite3_step(stmt) == SQLITE_ROW) {
         const unsigned char *p = sqlite3_column_text(stmt, 0);
         const unsigned char *a = sqlite3_column_text(stmt, 1);
         if (p && provider_out && provider_buf_size > 0) {
            snprintf(provider_out, provider_buf_size, "%s", (const char *)p);
         }
         if (a) {
            address_json = strdup((const char *)a);
         }
      }
   }
   if (stmt) {
      sqlite3_finalize(stmt);
   }
   AUTH_DB_UNLOCK();
   return address_json;
}

/* =============================================================================
 * Outbound send
 * ============================================================================= */

int messaging_engine_send(int user_id, const char *channel_name, const char *text) {
   if (user_id <= 0 || !channel_name || !text || text[0] == '\0') {
      return MESSAGING_FAILURE;
   }
   if (!atomic_load(&s_initialized)) {
      return MESSAGING_FAILURE;
   }

   char provider[16] = { 0 };
   char *address_json = lookup_channel_address(user_id, channel_name, provider, sizeof(provider));
   if (!address_json) {
      return MESSAGING_UNKNOWN_CHANNEL;
   }

   /* Per-user-channel rate limit. */
   char rl_key[64];
   snprintf(rl_key, sizeof(rl_key), "u%d:%s", user_id, channel_name);
   if (rate_limiter_check(&s_outbound_per_user_limiter, rl_key)) {
      free(address_json);
      return MESSAGING_RATE_LIMITED;
   }

   const messaging_driver_t *drv = find_driver(provider);
   if (!drv) {
      free(address_json);
      return MESSAGING_DRIVER_NOT_REGISTERED;
   }

   int rc = drv->send_text(address_json, text);
   free(address_json);

   if (rc == 0) {
      /* Bump last_used_at. */
      AUTH_DB_LOCK_OR_RETURN(MESSAGING_SUCCESS); /* if lock fails just skip */
      sqlite3_stmt *stmt = NULL;
      if (sqlite3_prepare_v2(s_db.db,
                             "UPDATE messaging_channels SET last_used_at = ? "
                             "WHERE user_id = ? AND LOWER(COALESCE(display_name,'')) = LOWER(?)",
                             -1, &stmt, NULL) == SQLITE_OK) {
         sqlite3_bind_int64(stmt, 1, (int64_t)time(NULL));
         sqlite3_bind_int(stmt, 2, user_id);
         sqlite3_bind_text(stmt, 3, channel_name, -1, SQLITE_STATIC);
         sqlite3_step(stmt);
         sqlite3_finalize(stmt);
      }
      AUTH_DB_UNLOCK();
      return MESSAGING_SUCCESS;
   }

   return MESSAGING_FAILURE;
}

/* =============================================================================
 * List channels JSON
 * ============================================================================= */

char *messaging_engine_list_channels_json(int user_id) {
   if (user_id <= 0) {
      return NULL;
   }

   struct json_object *arr = json_object_new_array();
   if (!arr) {
      return NULL;
   }

   AUTH_DB_LOCK_OR_RETURN(strdup("[]"));
   sqlite3_stmt *stmt = NULL;
   const char *sql = "SELECT COALESCE(display_name,''), provider, is_enabled, last_used_at "
                     "FROM messaging_channels WHERE user_id = ? "
                     "ORDER BY last_used_at DESC NULLS LAST, id ASC";
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_int(stmt, 1, user_id);
      while (sqlite3_step(stmt) == SQLITE_ROW) {
         struct json_object *obj = json_object_new_object();
         const unsigned char *name = sqlite3_column_text(stmt, 0);
         const unsigned char *prov = sqlite3_column_text(stmt, 1);
         int enabled = sqlite3_column_int(stmt, 2);
         json_object_object_add(obj, "name",
                                json_object_new_string(name ? (const char *)name : ""));
         json_object_object_add(obj, "provider",
                                json_object_new_string(prov ? (const char *)prov : ""));
         json_object_object_add(obj, "enabled", json_object_new_boolean(enabled != 0));
         json_object_array_add(arr, obj);
      }
   }
   if (stmt) {
      sqlite3_finalize(stmt);
   }
   AUTH_DB_UNLOCK();

   const char *json_str = json_object_to_json_string_ext(arr, JSON_C_TO_STRING_PLAIN);
   char *result = json_str ? strdup(json_str) : strdup("[]");
   json_object_put(arr);
   return result;
}

/* =============================================================================
 * Link-code generation, lookup, claim
 * ============================================================================= */

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
   AUTH_DB_LOCK_OR_RETURN_VOID();
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db,
                          "DELETE FROM messaging_link_codes WHERE expires_at < ? "
                          "AND ROWID IN (SELECT ROWID FROM messaging_link_codes "
                          "WHERE expires_at < ? LIMIT 100)",
                          -1, &stmt, NULL) == SQLITE_OK) {
      time_t now = time(NULL);
      sqlite3_bind_int64(stmt, 1, (int64_t)now);
      sqlite3_bind_int64(stmt, 2, (int64_t)now);
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

/* =============================================================================
 * /link command handling (inbound, runs on driver listener thread)
 * ============================================================================= */

static void link_attempt_log(const char *provider,
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

static int handle_link_command(const char *provider,
                               const char *sender_address,
                               const char *code_part) {
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
      link_attempt_log(provider, sender_address, code, "invalid");
      OLOG_WARNING("messaging: unknown /link code from %s:%s", provider, sender_address);
      return MESSAGING_FAILURE;
   }

   time_t now = time(NULL);
   if (claimed_at > 0 || expires_at < (int64_t)now) {
      AUTH_DB_UNLOCK();
      link_attempt_log(provider, sender_address, code, claimed_at > 0 ? "invalid" : "expired");
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
      AUTH_DB_UNLOCK();
      link_attempt_log(provider, sender_address, code, "invalid");
      return MESSAGING_FAILURE;
   }

   /* Insert messaging_channels row.  Validate address via the driver's
    * validate_address hook first. */
   const messaging_driver_t *drv = find_driver(provider);
   if (drv && drv->validate_address) {
      /* For Telegram/Discord/Slack the address blob is just the typed
       * primary key — the engine constructs minimal JSON here.  Driver
       * may extend (e.g., add user_id field) later via update path. */
      char address_json[256];
      snprintf(address_json, sizeof(address_json), "{\"chat_id\":\"%s\"}", sender_address);
      if (drv->validate_address(address_json) != SUCCESS) {
         AUTH_DB_UNLOCK();
         link_attempt_log(provider, sender_address, code, "invalid");
         return MESSAGING_FAILURE;
      }

      /* INSERT the row. */
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
   }

   AUTH_DB_UNLOCK();
   link_attempt_log(provider, sender_address, code, "success");
   OLOG_INFO("messaging: linked %s:%s to user %d via code %s", provider, sender_address, user_id,
             code);

   /* Send confirmation back to the user via the driver. */
   if (drv && drv->send_text) {
      char address_json[256];
      snprintf(address_json, sizeof(address_json), "{\"chat_id\":\"%s\"}", sender_address);
      drv->send_text(address_json,
                     "Your channel has been linked. Messages here will now reach the assistant.");
   }
   return MESSAGING_SUCCESS;
}

/* =============================================================================
 * Inbound dispatch (called from driver listener threads)
 * ============================================================================= */

static int engine_inbound_dispatch(const char *provider,
                                   const char *provider_address,
                                   const char *sender_display,
                                   const char *body,
                                   int64_t timestamp) {
   if (!provider || !provider_address || !body) {
      return MESSAGING_FAILURE;
   }
   if (!atomic_load(&s_initialized)) {
      return MESSAGING_FAILURE;
   }

   size_t body_len = strlen(body);

   /* Pre-DB general rate limit (per-sender).  Prevents DB-lookup DoS
    * from a stranger-flood. */
   char rl_key[MESSAGING_RL_KEY_SIZE];
   snprintf(rl_key, sizeof(rl_key), "%s:%s", provider, provider_address);
   if (rate_limiter_check(&s_inbound_general_limiter, rl_key)) {
      OLOG_DEBUG("messaging: inbound rate limit hit for %s", rl_key);
      return MESSAGING_RATE_LIMITED;
   }

   /* /link short-circuit (own stricter rate limit, body cap). */
   if (body_len >= 6 && strncmp(body, "/link ", 6) == 0) {
      if (body_len > MESSAGING_LINK_BODY_CAP) {
         OLOG_WARNING("messaging: oversized /link body from %s:%s (%zu bytes)", provider,
                      provider_address, body_len);
         link_attempt_log(provider, provider_address, NULL, "invalid");
         return MESSAGING_FAILURE;
      }
      if (rate_limiter_check(&s_inbound_link_limiter, rl_key)) {
         OLOG_WARNING("messaging: /link rate limit hit for %s:%s", provider, provider_address);
         link_attempt_log(provider, provider_address, NULL, "rate_limited");
         return MESSAGING_RATE_LIMITED;
      }
      return handle_link_command(provider, provider_address, body + 6);
   }

   /* Channel lookup.  No match → silently drop (don't reply to
    * strangers; bot would be a free spam amplifier). */
   int user_id = lookup_channel_user(provider, provider_address, NULL, 0);
   if (user_id <= 0) {
      OLOG_DEBUG("messaging: inbound from unlinked %s:%s — dropping", provider, provider_address);
      return MESSAGING_UNKNOWN_CHANNEL;
   }

   /* Enqueue for the worker drain. */
   return enqueue_inbound(provider, provider_address, sender_display, body, timestamp);
}

static int enqueue_inbound(const char *provider,
                           const char *provider_address,
                           const char *sender_display,
                           const char *body,
                           int64_t timestamp) {
   pthread_mutex_lock(&s_inbound_mutex);
   if (s_inbound_count >= MESSAGING_INBOUND_QUEUE_DEPTH) {
      pthread_mutex_unlock(&s_inbound_mutex);
      OLOG_WARNING("messaging: inbound queue full, dropping message from %s:%s", provider,
                   provider_address);
      return MESSAGING_FAILURE;
   }

   inbound_item_t *item = calloc(1, sizeof(*item));
   if (!item) {
      pthread_mutex_unlock(&s_inbound_mutex);
      return MESSAGING_FAILURE;
   }
   snprintf(item->provider, sizeof(item->provider), "%s", provider);
   snprintf(item->provider_address, sizeof(item->provider_address), "%s", provider_address);
   snprintf(item->sender_display, sizeof(item->sender_display), "%s",
            sender_display ? sender_display : "");
   item->body = strdup(body);
   item->timestamp = timestamp;
   if (!item->body) {
      free(item);
      pthread_mutex_unlock(&s_inbound_mutex);
      return MESSAGING_FAILURE;
   }

   s_inbound_queue[s_inbound_tail] = item;
   s_inbound_tail = (s_inbound_tail + 1) % MESSAGING_INBOUND_QUEUE_DEPTH;
   s_inbound_count++;
   pthread_cond_signal(&s_inbound_cond);
   pthread_mutex_unlock(&s_inbound_mutex);
   return MESSAGING_SUCCESS;
}

/* =============================================================================
 * Session map and worker thread
 * ============================================================================= */

static session_t *get_or_create_messaging_session(const char *provider,
                                                  const char *provider_address) {
   pthread_mutex_lock(&s_session_slots_mutex);

   /* Find existing slot. */
   for (size_t i = 0; i < MESSAGING_MAX_SESSIONS; i++) {
      if (s_session_slots[i].session && strcmp(s_session_slots[i].provider, provider) == 0 &&
          strcmp(s_session_slots[i].provider_address, provider_address) == 0) {
         s_session_slots[i].last_used = time(NULL);
         session_t *s = s_session_slots[i].session;
         session_retain(s);
         pthread_mutex_unlock(&s_session_slots_mutex);
         return s;
      }
   }

   /* Find a free slot (or evict the LRU). */
   size_t target = MESSAGING_MAX_SESSIONS;
   time_t lru_time = 0;
   size_t lru_idx = 0;
   for (size_t i = 0; i < MESSAGING_MAX_SESSIONS; i++) {
      if (!s_session_slots[i].session) {
         target = i;
         break;
      }
      if (lru_time == 0 || s_session_slots[i].last_used < lru_time) {
         lru_time = s_session_slots[i].last_used;
         lru_idx = i;
      }
   }
   if (target == MESSAGING_MAX_SESSIONS) {
      /* Evict LRU. */
      OLOG_INFO("messaging: evicting LRU session slot %zu (%s:%s)", lru_idx,
                s_session_slots[lru_idx].provider, s_session_slots[lru_idx].provider_address);
      session_release(s_session_slots[lru_idx].session);
      memset(&s_session_slots[lru_idx], 0, sizeof(session_slot_t));
      target = lru_idx;
   }

   /* Create new session.  Reuse SESSION_TYPE_WEBUI for the type tag —
    * client_data stays NULL; code that needs WebUI-specific state
    * (webui_send_*, ws_connection_t) NULL-checks before deref. */
   session_t *s = session_create(SESSION_TYPE_WEBUI, -1);
   if (!s) {
      pthread_mutex_unlock(&s_session_slots_mutex);
      return NULL;
   }

   snprintf(s_session_slots[target].provider, sizeof(s_session_slots[target].provider), "%s",
            provider);
   snprintf(s_session_slots[target].provider_address,
            sizeof(s_session_slots[target].provider_address), "%s", provider_address);
   s_session_slots[target].session = s;
   s_session_slots[target].last_used = time(NULL);

   /* Retain once for the map and once for the caller. */
   session_retain(s);
   pthread_mutex_unlock(&s_session_slots_mutex);
   return s;
}

static void process_inbound(inbound_item_t *item) {
   session_t *session = get_or_create_messaging_session(item->provider, item->provider_address);
   if (!session) {
      OLOG_ERROR("messaging: failed to acquire session for %s:%s", item->provider,
                 item->provider_address);
      return;
   }

   text_input_dispatch_opts_t opts = {
      .conversation_id = 0, /* v1: no persistent conv_db row for messaging-backed */
      .auth_user_id = 0,
      .sentence_cb = NULL,
      .sentence_userdata = NULL,
      .on_user_msg_added = NULL,
      .user_msg_added_ctx = NULL,
   };

   char *response = core_text_input_dispatch(session, item->body, NULL, NULL, NULL, 0, &opts);
   session_release(session);

   if (!response || response[0] == '\0') {
      free(response);
      return;
   }

   /* Send the response back via the originating driver. */
   const messaging_driver_t *drv = find_driver(item->provider);
   if (drv && drv->send_text) {
      char address_json[256];
      snprintf(address_json, sizeof(address_json), "{\"chat_id\":\"%s\"}", item->provider_address);
      drv->send_text(address_json, response);
   }
   free(response);
}

static void *worker_thread(void *arg) {
   (void)arg;
   OLOG_INFO("messaging: worker thread started");

   while (!atomic_load(&s_shutdown_requested)) {
      pthread_mutex_lock(&s_inbound_mutex);
      while (s_inbound_count == 0 && !atomic_load(&s_shutdown_requested)) {
         pthread_cond_wait(&s_inbound_cond, &s_inbound_mutex);
      }
      if (atomic_load(&s_shutdown_requested)) {
         pthread_mutex_unlock(&s_inbound_mutex);
         break;
      }
      inbound_item_t *item = s_inbound_queue[s_inbound_head];
      s_inbound_queue[s_inbound_head] = NULL;
      s_inbound_head = (s_inbound_head + 1) % MESSAGING_INBOUND_QUEUE_DEPTH;
      s_inbound_count--;
      pthread_mutex_unlock(&s_inbound_mutex);

      if (item) {
         process_inbound(item);
         free(item->body);
         free(item);
      }
   }

   OLOG_INFO("messaging: worker thread exiting");
   return NULL;
}
