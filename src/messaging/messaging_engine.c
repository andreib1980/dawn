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
#include "config/dawn_config.h"
#include "core/memory_filter.h"
#include "core/rate_limiter.h"
#include "core/session_manager.h"
#include "core/text_input_dispatch.h"
#include "core/wake_word.h"
#include "dawn_error.h"
#include "logging.h"
#include "memory/memory_history_loader.h"

/* =============================================================================
 * Internal constants and types
 * ============================================================================= */

#define MESSAGING_MAX_DRIVERS 4
#define MESSAGING_INBOUND_QUEUE_DEPTH 32
#define MESSAGING_MAX_SESSIONS 64
#define MESSAGING_MAX_BODY_LEN 4096
#define MESSAGING_LINK_BODY_CAP 256
#define MESSAGING_RL_KEY_SIZE 96 /* "provider:address" composite */

/* Rate limiter slot counts — these MUST match the static array
 * declarations below (s_inbound_link_entries[N] etc.).  If you change
 * one, change both. */
#define MESSAGING_RL_LINK_SLOTS 64
#define MESSAGING_RL_GENERAL_SLOTS 128
#define MESSAGING_RL_OUTBOUND_SLOTS 64

/* Bounded stack buffer for the engine's async-send confirmation text
 * (/link confirm, /new confirm).  Current callers send ~80-90-char
 * messages; the cap is well above that with margin for future
 * confirmation strings.  Oversize content is truncated by snprintf;
 * callers must NOT assume arbitrary-length delivery through
 * engine_send_async. */
#define MESSAGING_ASYNC_SEND_TEXT_MAX 1024

/* Stack buffer for per-provider address_json blob construction
 * (build_address_json_for).  Sized for Telegram chat_id +
 * SMS phone_e164 + Phase 3+ Discord/Slack extras (guild_id, team_id).
 * Used at several call sites that all need to agree. */
#define MESSAGING_ADDRESS_JSON_BUF_SIZE 256

/* Per-turn channel-hint buffer (the SMS system-prompt augmentation).
 * Holds the base hint plus an optional truncation-feedback append
 * when the previous reply overflowed.  See provider_outbound_for. */
#define MESSAGING_CHANNEL_HINT_BUF_SIZE 1024

/* Crockford base32 alphabet — excludes I/L/O/U to avoid typing
 * confusion. */
static const char CROCKFORD_ALPHABET[32] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

typedef struct {
   char provider[16];
   char provider_address[128];
   char sender_display[64];
   char *body; /* heap-allocated */
   int64_t timestamp;
   int user_id; /* resolved at enqueue time; 0 if unresolved (shouldn't happen) */
} inbound_item_t;

typedef struct {
   char provider[16];
   char provider_address[128];
   session_t *session;
   time_t last_used;
   /* Self-reset deferral flag.  Set when the LLM (running on THIS
    * slot's session via the tool loop) calls
    * `messaging.reset_conversation` targeting its own channel.
    * Immediate reset would session_destroy the session out from under
    * the worker that's still mid-dispatch.  Instead, the reset is
    * deferred: process_inbound checks this flag after its dispatch
    * returns + persistence completes, then performs the eviction +
    * conv_id clear there.  Reset by the worker after processing. */
   bool pending_reset;
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

/* Rate limiters.  Slot counts live in MESSAGING_RL_*_SLOTS so the
 * array decls below and the slot_count fields in the rate_limiter
 * configs (messaging_engine_init) can't drift apart. */
static rate_limit_entry_t s_inbound_link_entries[MESSAGING_RL_LINK_SLOTS];
static rate_limit_entry_t s_inbound_general_entries[MESSAGING_RL_GENERAL_SLOTS];
static rate_limit_entry_t s_outbound_per_user_entries[MESSAGING_RL_OUTBOUND_SLOTS];

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
                           int64_t timestamp,
                           int user_id);
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
                                                  const char *provider_address,
                                                  int user_id,
                                                  int64_t conversation_id);
static void process_inbound(inbound_item_t *item);
static void link_attempt_log(const char *provider,
                             const char *sender_address,
                             const char *code_tried,
                             const char *result);
static void purge_expired_link_codes(void);
static int64_t resolve_channel_conversation_id(const char *provider,
                                               const char *provider_address,
                                               int user_id);
static void evict_session_slot(const char *provider, const char *provider_address);
static int clear_channel_conversation_id(const char *provider, const char *provider_address);
static bool sms_within_active_window(const char *sender_e164);
static void touch_channel_last_used(const char *provider, const char *provider_address);

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
   rate_limiter_config_t link_cfg = { .max_count = 5,
                                      .window_sec = 600,
                                      .slot_count = MESSAGING_RL_LINK_SLOTS };
   rate_limiter_config_t general_cfg = { .max_count = 60,
                                         .window_sec = 600,
                                         .slot_count = MESSAGING_RL_GENERAL_SLOTS };
   rate_limiter_config_t outbound_cfg = { .max_count = 10,
                                          .window_sec = 60,
                                          .slot_count = MESSAGING_RL_OUTBOUND_SLOTS };

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

   /* Destroy any retained sessions.  Using session_destroy (vs the
    * old session_release-only path) triggers memory extraction for
    * the closing conversation via session_destroy's existing hook —
    * so in-flight forever-conversations get their facts extracted on
    * graceful shutdown.  Cost: up to 3 sec per slot waiting for
    * ref_count to converge, × MESSAGING_MAX_SESSIONS=64 worst-case.
    * In practice only a handful of slots are active, and ref_count
    * is already 1 (engine's retain) since the worker thread joined
    * above — destroy returns essentially instantly per slot.  We
    * capture the session_ids under the lock, drop it, then call
    * session_destroy outside (avoids the per-module → global lock
    * inversion that the eviction path already documented). */
   pthread_mutex_lock(&s_session_slots_mutex);
   uint32_t shutdown_session_ids[MESSAGING_MAX_SESSIONS];
   size_t shutdown_session_count = 0;
   for (size_t i = 0; i < MESSAGING_MAX_SESSIONS; i++) {
      if (s_session_slots[i].session) {
         shutdown_session_ids[shutdown_session_count++] = s_session_slots[i].session->session_id;
         session_release(s_session_slots[i].session);
         memset(&s_session_slots[i], 0, sizeof(session_slot_t));
      }
   }
   pthread_mutex_unlock(&s_session_slots_mutex);

   for (size_t i = 0; i < shutdown_session_count; i++) {
      session_destroy(shutdown_session_ids[i]);
   }

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

/* Resolve the forever-binding conversation_id for a (provider,
 * provider_address) channel.  If the channel already has a non-NULL
 * conversation_id, return it.  Otherwise create a fresh conversations
 * row via conv_db_create_with_origin (origin="messaging:<provider>"),
 * stamp it onto the channel mapping, and return the new id.
 *
 * Returns 0 on any failure (lookup miss, conv-create failure, lock
 * failure).  Callers MUST handle the 0 case as "no DB persistence this
 * turn" — the conversation still works in memory, but messages won't
 * survive daemon restart and the recovery worker won't extract from
 * them.  See docs/MESSAGING_CHANNELS_DESIGN.md §13 Phase 2.5. */
static int64_t resolve_channel_conversation_id(const char *provider,
                                               const char *provider_address,
                                               int user_id) {
   if (!provider || !provider_address || user_id <= 0) {
      return 0;
   }

   /* Phase 1: read existing conversation_id under lock. */
   AUTH_DB_LOCK_OR_RETURN(0);
   int64_t existing = 0;
   char display_name[128] = { 0 };
   sqlite3_stmt *stmt = NULL;
   const char *sel_sql = "SELECT COALESCE(conversation_id, 0), COALESCE(display_name,'') "
                         "FROM messaging_channels "
                         "WHERE provider = ? AND provider_address = ? AND is_enabled = 1 LIMIT 1";
   bool found = false;
   if (sqlite3_prepare_v2(s_db.db, sel_sql, -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_text(stmt, 1, provider, -1, SQLITE_STATIC);
      sqlite3_bind_text(stmt, 2, provider_address, -1, SQLITE_STATIC);
      if (sqlite3_step(stmt) == SQLITE_ROW) {
         existing = sqlite3_column_int64(stmt, 0);
         const unsigned char *dn = sqlite3_column_text(stmt, 1);
         if (dn) {
            snprintf(display_name, sizeof(display_name), "%s", (const char *)dn);
         }
         found = true;
      }
   }
   if (stmt) {
      sqlite3_finalize(stmt);
   }
   AUTH_DB_UNLOCK();

   if (!found) {
      OLOG_WARNING("messaging: resolve_conv: channel %s:%s not found (race with unlink?)", provider,
                   provider_address);
      return 0;
   }

   if (existing > 0) {
      return existing;
   }

   /* Phase 2: no existing conversation — create one.  Title uses the
    * channel's display_name when set, falling back to the address.
    * conv_db_create_with_origin takes the auth_db lock itself, so we
    * MUST release ours first. */
   char title[256];
   if (display_name[0] != '\0') {
      snprintf(title, sizeof(title), "%s (%s)", display_name, provider);
   } else {
      snprintf(title, sizeof(title), "%s: %s", provider, provider_address);
   }
   char origin[64];
   snprintf(origin, sizeof(origin), "messaging:%s", provider);

   int64_t new_conv_id = 0;
   int rc = conv_db_create_with_origin(user_id, title, origin, &new_conv_id);
   if (rc != AUTH_DB_SUCCESS || new_conv_id <= 0) {
      OLOG_ERROR("messaging: resolve_conv: conv_db_create_with_origin failed for %s:%s (rc=%d)",
                 provider, provider_address, rc);
      return 0;
   }

   /* Phase 3: stamp the new conv_id onto the channel row.  If this
    * UPDATE fails the conv row is orphaned (no link back to the
    * channel) — recoverable but ugly.  Logging surfaces the case for
    * post-hoc cleanup. */
   AUTH_DB_LOCK_OR_RETURN(new_conv_id); /* fall through on lock fail; conv exists, just unlinked */
   sqlite3_stmt *upd = NULL;
   const char *upd_sql = "UPDATE messaging_channels SET conversation_id = ? "
                         "WHERE provider = ? AND provider_address = ? AND is_enabled = 1 AND "
                         "conversation_id IS NULL";
   bool stamped = false;
   if (sqlite3_prepare_v2(s_db.db, upd_sql, -1, &upd, NULL) == SQLITE_OK) {
      sqlite3_bind_int64(upd, 1, new_conv_id);
      sqlite3_bind_text(upd, 2, provider, -1, SQLITE_STATIC);
      sqlite3_bind_text(upd, 3, provider_address, -1, SQLITE_STATIC);
      if (sqlite3_step(upd) == SQLITE_DONE) {
         stamped = (sqlite3_changes(s_db.db) > 0);
      }
   }
   if (upd) {
      sqlite3_finalize(upd);
   }
   AUTH_DB_UNLOCK();

   if (!stamped) {
      OLOG_WARNING("messaging: resolve_conv: created conv %lld but UPDATE matched 0 rows for %s:%s "
                   "(concurrent claim?) — conv is orphaned, will be invisible to /new",
                   (long long)new_conv_id, provider, provider_address);
   } else {
      OLOG_INFO("messaging: bound channel %s:%s to conv %lld (origin=%s)", provider,
                provider_address, (long long)new_conv_id, origin);
   }

   return new_conv_id;
}

/* Find the in-memory session slot for (provider, provider_address) and
 * evict it.  Eviction triggers memory extraction on the closing
 * conversation via session_destroy's existing extraction hook.  No-op
 * when no slot matches (channel never received an inbound this
 * daemon-uptime).  Drops s_session_slots_mutex before calling
 * session_destroy per the same lock-order rule the LRU path follows
 * (per-module → global is forbidden). */
static void evict_session_slot(const char *provider, const char *provider_address) {
   if (!provider || !provider_address) {
      return;
   }

   pthread_mutex_lock(&s_session_slots_mutex);
   session_t *evictee = NULL;
   uint32_t evictee_session_id = 0;
   for (size_t i = 0; i < MESSAGING_MAX_SESSIONS; i++) {
      if (s_session_slots[i].session && strcmp(s_session_slots[i].provider, provider) == 0 &&
          strcmp(s_session_slots[i].provider_address, provider_address) == 0) {
         evictee = s_session_slots[i].session;
         evictee_session_id = evictee->session_id;
         memset(&s_session_slots[i], 0, sizeof(session_slot_t));
         break;
      }
   }
   pthread_mutex_unlock(&s_session_slots_mutex);

   if (evictee) {
      OLOG_INFO("messaging: /new evicting session slot for %s:%s (session_id=%u)", provider,
                provider_address, evictee_session_id);
      /* Drop the engine's retain so session_destroy's ref-count wait
       * converges immediately.  session_destroy fires memory extraction
       * for the closing conversation. */
      session_release(evictee);
      session_destroy(evictee_session_id);
   }
}

/* Clear messaging_channels.conversation_id for a channel.  Returns
 * MESSAGING_SUCCESS on a 1-row update, MESSAGING_UNKNOWN_CHANNEL if no
 * row matched, MESSAGING_FAILURE on lock / prepare failure. */
static int clear_channel_conversation_id(const char *provider, const char *provider_address) {
   if (!provider || !provider_address) {
      return MESSAGING_FAILURE;
   }
   AUTH_DB_LOCK_OR_RETURN(MESSAGING_FAILURE);
   sqlite3_stmt *stmt = NULL;
   int rc = MESSAGING_FAILURE;
   const char *sql = "UPDATE messaging_channels SET conversation_id = NULL "
                     "WHERE provider = ? AND provider_address = ? AND is_enabled = 1";
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_text(stmt, 1, provider, -1, SQLITE_STATIC);
      sqlite3_bind_text(stmt, 2, provider_address, -1, SQLITE_STATIC);
      if (sqlite3_step(stmt) == SQLITE_DONE) {
         rc = (sqlite3_changes(s_db.db) > 0) ? MESSAGING_SUCCESS : MESSAGING_UNKNOWN_CHANNEL;
      }
   }
   if (stmt) {
      sqlite3_finalize(stmt);
   }
   AUTH_DB_UNLOCK();
   return rc;
}

/* Active-conversation window for SMS.  Returns true when an inbound
 * SMS from this sender should bypass the wake-word gate because the
 * channel had an LLM-bound exchange within g_config.messaging
 * .sms_active_window_sec.  Returns false when:
 *   - the window is disabled (config value 0 or negative),
 *   - the sender isn't linked (no channel row),
 *   - no prior LLM exchange has happened yet (last_used_at NULL or 0),
 *   - the most recent exchange is older than the window.
 *
 * Reads BOTH user_id and last_used_at in one query to avoid a
 * double-lookup on the inbound hot path. */
static bool sms_within_active_window(const char *sender_e164) {
   if (!sender_e164) {
      return false;
   }
   int window_sec = g_config.messaging.sms_active_window_sec;
   if (window_sec <= 0) {
      return false;
   }

   AUTH_DB_LOCK_OR_RETURN(false);
   sqlite3_stmt *stmt = NULL;
   bool within = false;
   const char *sql = "SELECT COALESCE(last_used_at, 0) FROM messaging_channels "
                     "WHERE provider = 'sms' AND provider_address = ? AND is_enabled = 1 LIMIT 1";
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_text(stmt, 1, sender_e164, -1, SQLITE_STATIC);
      if (sqlite3_step(stmt) == SQLITE_ROW) {
         int64_t last_used = sqlite3_column_int64(stmt, 0);
         if (last_used > 0) {
            int64_t now = (int64_t)time(NULL);
            within = ((now - last_used) <= (int64_t)window_sec);
         }
      }
   }
   if (stmt) {
      sqlite3_finalize(stmt);
   }
   AUTH_DB_UNLOCK();
   return within;
}

/* Touch a channel's last_used_at to NOW.  Called after a successful
 * LLM-bound exchange so the active-conversation window slides forward
 * with each turn.  Best-effort — failure is logged at DEBUG and the
 * turn still completes. */
static void touch_channel_last_used(const char *provider, const char *provider_address) {
   if (!provider || !provider_address) {
      return;
   }
   AUTH_DB_LOCK_OR_RETURN_VOID();
   sqlite3_stmt *stmt = NULL;
   const char *sql = "UPDATE messaging_channels SET last_used_at = ? "
                     "WHERE provider = ? AND provider_address = ? AND is_enabled = 1";
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_int64(stmt, 1, (int64_t)time(NULL));
      sqlite3_bind_text(stmt, 2, provider, -1, SQLITE_STATIC);
      sqlite3_bind_text(stmt, 3, provider_address, -1, SQLITE_STATIC);
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);
   }
   AUTH_DB_UNLOCK();
}

/* Try to detect that the caller is the LLM running on the same
 * session the reset would destroy.  When this returns true, the
 * caller MUST treat the reset as deferred (set pending_reset on the
 * slot, return MESSAGING_SUCCESS, let process_inbound handle it after
 * dispatch).  Synchronous reset on a self-targeting call leads to
 * UAF: session_destroy times out waiting for the worker's retain to
 * drop, frees the session_t anyway, worker resumes on freed memory.
 *
 * Detection: session_get_command_context() returns the session
 * currently executing tool callbacks on this thread.  We compare its
 * session_id against the slot's session for (provider, address).  A
 * match means the LLM tool is running on THIS slot's session — exact
 * self-reset case.  Non-match (or no command context, or no slot
 * yet) → immediate reset is safe. */
static bool mark_pending_reset_if_self(const char *provider, const char *provider_address) {
   session_t *caller = session_get_command_context();
   if (!caller) {
      return false;
   }
   uint32_t caller_id = caller->session_id;

   pthread_mutex_lock(&s_session_slots_mutex);
   bool self_reset = false;
   for (size_t i = 0; i < MESSAGING_MAX_SESSIONS; i++) {
      if (s_session_slots[i].session && strcmp(s_session_slots[i].provider, provider) == 0 &&
          strcmp(s_session_slots[i].provider_address, provider_address) == 0) {
         if (s_session_slots[i].session->session_id == caller_id) {
            s_session_slots[i].pending_reset = true;
            self_reset = true;
            OLOG_INFO("messaging: /new on %s:%s deferred — caller IS the target session "
                      "(session_id=%u), processing after dispatch completes",
                      provider, provider_address, caller_id);
         }
         break;
      }
   }
   pthread_mutex_unlock(&s_session_slots_mutex);
   return self_reset;
}

int messaging_engine_reset_channel(const char *provider, const char *provider_address) {
   if (!provider || !provider_address) {
      return MESSAGING_FAILURE;
   }
   if (!atomic_load(&s_initialized)) {
      return MESSAGING_FAILURE;
   }

   /* Validate the channel exists first.  reset is a no-op + error if
    * the channel isn't linked (matches messaging_engine_send's contract). */
   int user_id = lookup_channel_user(provider, provider_address, NULL, 0);
   if (user_id <= 0) {
      return MESSAGING_UNKNOWN_CHANNEL;
   }

   /* CRITICAL: detect self-reset and defer.  When the LLM emits
    * messaging.reset_conversation targeting its own channel, the
    * calling thread is the messaging worker mid-dispatch holding a
    * retain on the very session_t we'd destroy.  Synchronous
    * session_destroy times out after 3s and frees the session
    * anyway → UAF on resume.  Defer via pending_reset on the slot;
    * process_inbound checks at end-of-dispatch and performs the
    * actual eviction + clear there. */
   if (mark_pending_reset_if_self(provider, provider_address)) {
      return MESSAGING_SUCCESS;
   }

   /* Order matters: evict FIRST (triggers extraction on the closing
    * conv via session_destroy → memory_trigger_extraction with the
    * session's in-memory history), then clear the DB binding.  If we
    * cleared first, the extraction trigger inside session_destroy
    * would still fire but on a session whose conv_id was already
    * un-stamped — extraction works either way (it copies history) but
    * the audit log makes more sense in eviction-then-clear order. */
   evict_session_slot(provider, provider_address);

   int rc = clear_channel_conversation_id(provider, provider_address);
   if (rc == MESSAGING_SUCCESS) {
      OLOG_INFO("messaging: /new reset channel %s:%s — next inbound will start a fresh conv",
                provider, provider_address);
   } else if (rc == MESSAGING_UNKNOWN_CHANNEL) {
      /* Channel exists (lookup succeeded above) but conversation_id was
       * already NULL — that's not an error, just nothing to clear. */
      OLOG_DEBUG("messaging: /new on %s:%s — conversation_id already NULL, no-op", provider,
                 provider_address);
      rc = MESSAGING_SUCCESS;
   }
   return rc;
}

int messaging_engine_reset_by_name(int user_id, const char *channel_name) {
   if (user_id <= 0 || !channel_name) {
      return MESSAGING_FAILURE;
   }
   if (!atomic_load(&s_initialized)) {
      return MESSAGING_FAILURE;
   }

   /* Resolve channel_name → (provider, provider_address) under the
    * authenticated user.  Mirrors the ownership boundary that
    * messaging_engine_send enforces. */
   AUTH_DB_LOCK_OR_RETURN(MESSAGING_FAILURE);
   char provider[16] = { 0 };
   char provider_address[128] = { 0 };
   sqlite3_stmt *stmt = NULL;
   bool found = false;
   const char *sql = "SELECT provider, provider_address FROM messaging_channels "
                     "WHERE user_id = ? AND is_enabled = 1 AND "
                     "LOWER(COALESCE(display_name,'')) = LOWER(?) LIMIT 1";
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_int(stmt, 1, user_id);
      sqlite3_bind_text(stmt, 2, channel_name, -1, SQLITE_STATIC);
      if (sqlite3_step(stmt) == SQLITE_ROW) {
         const unsigned char *p = sqlite3_column_text(stmt, 0);
         const unsigned char *a = sqlite3_column_text(stmt, 1);
         if (p) {
            snprintf(provider, sizeof(provider), "%s", (const char *)p);
         }
         if (a) {
            snprintf(provider_address, sizeof(provider_address), "%s", (const char *)a);
         }
         found = true;
      }
   }
   if (stmt) {
      sqlite3_finalize(stmt);
   }
   AUTH_DB_UNLOCK();

   if (!found || provider[0] == '\0' || provider_address[0] == '\0') {
      return MESSAGING_UNKNOWN_CHANNEL;
   }

   return messaging_engine_reset_channel(provider, provider_address);
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

   /* Pass NULL for provider_address — this code path looks the channel
    * up by display_name (LLM tool's `send` action), so we only have
    * address_json.  Drivers fall back to parsing it.  Future
    * optimization: extend lookup_channel_address to also return
    * provider_address so this hot path can skip the JSON parse too. */
   int rc = drv->send_text(user_id, NULL, address_json, text);
   free(address_json);

   if (rc == SUCCESS) {
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

   /* Acquire the DB lock BEFORE allocating the JSON array — otherwise
    * a lock-acquisition failure would leak `arr`.  Pattern matches
    * lookup_channel_address above. */
   AUTH_DB_LOCK_OR_RETURN(strdup("[]"));

   struct json_object *arr = json_object_new_array();
   if (!arr) {
      AUTH_DB_UNLOCK();
      return NULL;
   }

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

/* =============================================================================
 * /link command handling (inbound, runs on driver listener thread)
 * ============================================================================= */

/* =============================================================================
 * Async outbound send
 *
 * Used when we need to call drv->send_text() from a thread that cannot
 * block waiting for the response — specifically, the mosquitto network
 * callback thread that delivers inbound SMS events via echo/events.
 * That thread is also the one that delivers the echo/response message
 * carrying ECHO's send_sms ack; if we block it inside send_text waiting
 * for that ack, we self-deadlock and time out after 60s.  The normal
 * LLM-reply path doesn't hit this because the engine worker thread is
 * the one that calls send_text, leaving mosquitto's callback thread
 * free.  The /link confirmation, however, fires inline from
 * handle_link_command which runs on the mosquitto thread — hence the
 * need for an async dispatch.
 *
 * Spawned threads are detached (no join), small stack (64 KB), and
 * outlive the engine briefly on shutdown if a send is in flight.
 * Acceptable because shutdown doesn't free the driver descriptor.
 * ============================================================================= */

typedef struct {
   const messaging_driver_t *drv;
   int user_id;
   char provider_address[128];
   char address_json[MESSAGING_ADDRESS_JSON_BUF_SIZE];
   char text[MESSAGING_ASYNC_SEND_TEXT_MAX];
} async_send_item_t;

static void *async_send_thread(void *arg) {
   async_send_item_t *item = (async_send_item_t *)arg;
   if (item && item->drv && item->drv->send_text) {
      item->drv->send_text(item->user_id, item->provider_address, item->address_json, item->text);
   }
   free(item);
   return NULL;
}

static void engine_send_async(const messaging_driver_t *drv,
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
   snprintf(item->text, sizeof(item->text), "%s", text);

   pthread_attr_t attr;
   pthread_attr_init(&attr);
   pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
   pthread_attr_setstacksize(&attr, 64 * 1024);

   pthread_t tid;
   int rc = pthread_create(&tid, &attr, async_send_thread, item);
   pthread_attr_destroy(&attr);
   if (rc != 0) {
      OLOG_WARNING("messaging: async-send pthread_create failed (rc=%d); dropping send", rc);
      free(item);
   }
}

/* Per-provider address_json builder.  Delegates to the driver's
 * `build_address_json` hook when the driver is registered + provides
 * one — that's the single source of truth for each provider's JSON
 * shape and lets Phase 3 (Discord) and Phase 4 (Slack) add new drivers
 * without touching the engine.  Falls back to an inline shape table
 * when no driver is registered yet (e.g., during /link claim, before
 * the driver's init has wired its callbacks — though in practice the
 * driver always registers before claims).
 *
 * Telegram → {"chat_id":"<numeric_string>"}
 * SMS      → {"phone_e164":"<+1...>"}
 * Discord  → reserved for Phase 3 ({"channel_id":"...","user_id":"..."})
 * Slack    → reserved for Phase 4 ({"team_id":"T...","channel_id":"D..."})
 * Unknown  → "{}" (compatible with both UNIQUE constraint and
 *            future driver-supplied extras).
 */
static void build_address_json_for(const char *provider,
                                   const char *sender_address,
                                   char *buf,
                                   size_t buf_size) {
   if (!provider || !sender_address || !buf || buf_size == 0) {
      if (buf && buf_size > 0) {
         buf[0] = '\0';
      }
      return;
   }
   /* Driver-side dispatch (preferred). */
   const messaging_driver_t *drv = find_driver(provider);
   if (drv && drv->build_address_json) {
      drv->build_address_json(sender_address, buf, buf_size);
      return;
   }
   /* Fallback inline table.  Kept as a safety net for the narrow
    * window where /link claims a row before the driver finishes
    * registering its callbacks. */
   if (strcmp(provider, "telegram") == 0) {
      snprintf(buf, buf_size, "{\"chat_id\":\"%s\"}", sender_address);
   } else if (strcmp(provider, "sms") == 0) {
      snprintf(buf, buf_size, "{\"phone_e164\":\"%s\"}", sender_address);
   } else {
      snprintf(buf, buf_size, "{}");
   }
}

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

   /* Body length cap — protects the worker queue and the LLM from
    * over-long inbound payloads.  /link is gated above with a tighter
    * 256-byte cap; everything else gets MESSAGING_MAX_BODY_LEN. */
   if (body_len > MESSAGING_MAX_BODY_LEN) {
      OLOG_WARNING("messaging: oversized inbound body from %s:%s (%zu bytes) — dropping", provider,
                   provider_address, body_len);
      return MESSAGING_FAILURE;
   }

   /* Prompt-injection filter (memory_filter_check) — matches the
    * ingestion-gate rule that already protects fetched web content
    * and memory storage paths.  Hits get dropped + warned; legitimate
    * prose isn't blocked. */
   if (memory_filter_check(body)) {
      OLOG_WARNING("messaging: inbound body from %s:%s blocked by injection filter", provider,
                   provider_address);
      return MESSAGING_FAILURE;
   }

   /* Channel lookup.  No match → silently drop (don't reply to
    * strangers; bot would be a free spam amplifier). */
   int user_id = lookup_channel_user(provider, provider_address, NULL, 0);
   if (user_id <= 0) {
      OLOG_DEBUG("messaging: inbound from unlinked %s:%s — dropping", provider, provider_address);
      return MESSAGING_UNKNOWN_CHANNEL;
   }

   /* /new short-circuit — resets the channel's forever-conversation
    * binding.  Only honored from sender-in-channels (gated above);
    * unlinked senders can't trigger a reset on someone else's channel.
    * Recognized as case-insensitive "/new" followed by EOF or
    * whitespace; trailing content is tolerated and ignored (so "/new
    * conversation please" works as expected).  See
    * docs/MESSAGING_CHANNELS_DESIGN.md §13 Phase 2.5. */
   if (body_len >= 4 && (body[0] == '/') && (body[1] == 'n' || body[1] == 'N') &&
       (body[2] == 'e' || body[2] == 'E') && (body[3] == 'w' || body[3] == 'W') &&
       (body_len == 4 || body[4] == ' ' || body[4] == '\t' || body[4] == '\n' || body[4] == '\r')) {
      int rc = messaging_engine_reset_channel(provider, provider_address);
      /* Send confirmation through engine_send_async so callers running
       * on the mosquitto callback thread (SMS) don't deadlock waiting
       * for the echo/response that mosquitto can't deliver while the
       * callback is blocked.  Same pattern as handle_link_command.
       * user_id was resolved above by lookup_channel_user. */
      const messaging_driver_t *drv = find_driver(provider);
      if (drv && drv->send_text) {
         char address_json[MESSAGING_ADDRESS_JSON_BUF_SIZE];
         build_address_json_for(provider, provider_address, address_json, sizeof(address_json));
         const char *msg =
             (rc == MESSAGING_SUCCESS)
                 ? "Started a new conversation. Previous context is preserved in the WebUI."
                 : "Couldn't reset the conversation (internal error). Try again or check the "
                   "WebUI.";
         engine_send_async(drv, user_id, provider_address, address_json, msg);
      }
      OLOG_INFO("messaging: /new from %s:%s (rc=%d)", provider, provider_address, rc);
      return rc;
   }

   /* Enqueue for the worker drain.  user_id is already resolved by
    * the channel lookup above — pass it through so the worker
    * doesn't have to re-query. */
   return enqueue_inbound(provider, provider_address, sender_display, body, timestamp, user_id);
}

static int enqueue_inbound(const char *provider,
                           const char *provider_address,
                           const char *sender_display,
                           const char *body,
                           int64_t timestamp,
                           int user_id) {
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
   item->user_id = user_id;
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

/* NOTE: engine_shutdown uses session_destroy for each slot, so
 * in-flight forever-conversations get their facts extracted on
 * graceful shutdown via session_destroy's existing memory-extraction
 * hook.  See messaging_engine_shutdown above.
 *
 * Lock ordering: this function NEVER holds s_session_slots_mutex
 * across session_create or session_destroy.  Both global-lock-
 * acquiring helpers are called outside the per-module mutex.  See
 * the inline release/re-acquire dance in the eviction branch and the
 * create-new-session branch. */
static session_t *get_or_create_messaging_session(const char *provider,
                                                  const char *provider_address,
                                                  int user_id,
                                                  int64_t conversation_id) {
   pthread_mutex_lock(&s_session_slots_mutex);

   /* Find existing slot. */
   for (size_t i = 0; i < MESSAGING_MAX_SESSIONS; i++) {
      if (s_session_slots[i].session && strcmp(s_session_slots[i].provider, provider) == 0 &&
          strcmp(s_session_slots[i].provider_address, provider_address) == 0) {
         s_session_slots[i].last_used = time(NULL);
         session_t *s = s_session_slots[i].session;
         /* Re-stamp user_id in case it changed (e.g., admin re-linked
          * the same address to a different DAWN user). */
         if (user_id > 0) {
            s->metrics.user_id = user_id;
         }
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

   /* If we need to evict, capture the evictee's identity, NULL the
    * slot, drop the per-module mutex, then release+destroy outside
    * the lock.  session_destroy:
    *   - reclaims the global sessions[] slot (closes the slow-burn
    *     leak that plain session_release would create),
    *   - triggers memory extraction for the conversation (gated on
    *     user_id > 0, which we stamped when the session was created
    *     or last used), so Telegram/SMS conversations participate in
    *     the same extraction pipeline as WebUI/DAP2 sessions,
    *   - waits for ref_count to reach zero (3 s timeout); in normal
    *     operation no one else holds a retain on a messaging-backed
    *     session, so this returns essentially instantly.
    * Dropping the mutex before session_destroy fixes the lock-order
    * inversion on this path (per-module → global is forbidden; we
    * now release → call → re-acquire). */
   session_t *evictee = NULL;
   uint32_t evictee_session_id = 0;
   if (target == MESSAGING_MAX_SESSIONS) {
      OLOG_INFO("messaging: evicting LRU session slot %zu (%s:%s)", lru_idx,
                s_session_slots[lru_idx].provider, s_session_slots[lru_idx].provider_address);
      evictee = s_session_slots[lru_idx].session;
      evictee_session_id = evictee->session_id;
      memset(&s_session_slots[lru_idx], 0, sizeof(session_slot_t));
      target = lru_idx;
   }
   if (evictee) {
      pthread_mutex_unlock(&s_session_slots_mutex);
      /* Drop the engine's retain so session_destroy's ref-count wait
       * can converge to zero immediately. */
      session_release(evictee);
      session_destroy(evictee_session_id);
      pthread_mutex_lock(&s_session_slots_mutex);

      /* Defensive re-scan: today there's a single worker thread so
       * `target` is guaranteed still empty after we re-acquire, but if
       * a future refactor adds concurrent dispatchers, another thread
       * could have claimed this slot during the unlock window.  Pick
       * any free slot if `target` got taken. */
      if (s_session_slots[target].session) {
         target = MESSAGING_MAX_SESSIONS;
         for (size_t i = 0; i < MESSAGING_MAX_SESSIONS; i++) {
            if (!s_session_slots[i].session) {
               target = i;
               break;
            }
         }
         if (target == MESSAGING_MAX_SESSIONS) {
            /* Every slot taken again — would require multiple
             * concurrent inbound creates filling the freshly evicted
             * slot AND every other slot.  Drop this inbound. */
            OLOG_WARNING("messaging: lost slot race after eviction; dropping inbound for %s:%s",
                         provider, provider_address);
            pthread_mutex_unlock(&s_session_slots_mutex);
            return NULL;
         }
      }
   }

   /* Lock-order discipline: session_create acquires the GLOBAL
    * session_manager_rwlock.  Per ARCHITECTURE.md "Mutex Lock
    * Ordering", global locks must be acquired BEFORE per-module
    * locks.  Holding s_session_slots_mutex across session_create
    * would invert that order — no deadlock today (nothing else
    * acquires slots mutex while holding session_manager_rwlock), but
    * a real footgun if/when a future refactor adds concurrent
    * messaging dispatchers.
    *
    * Drop the slots mutex BEFORE session_create.  Set up the new
    * session (stamp user_id, restore history) while no engine locks
    * are held — the session_t isn't yet visible to any other thread
    * since we haven't installed it in the slot.  Re-acquire the
    * slots mutex to install; defensive re-scan handles the case
    * where another thread claimed `target` during the unlock window
    * (currently impossible with a single worker, but defensive for
    * future concurrent dispatchers). */
   pthread_mutex_unlock(&s_session_slots_mutex);

   /* Create new session with SESSION_TYPE_MESSAGING.  The type itself
    * exempts the session from session_cleanup_expired (see
    * session_manager.c) — the messaging engine owns this session's
    * lifetime via its own LRU eviction (which calls session_destroy
    * when needed).  client_data stays NULL; code that branches on
    * type=WEBUI for WebSocket delivery (webui_send_*, ws_connection_t)
    * correctly excludes messaging sessions. */
   session_t *s = session_create(SESSION_TYPE_MESSAGING, -1);
   if (!s) {
      return NULL;
   }

   /* Stamp user_id so per-turn focus injection runs against the right
    * user AND session-end memory extraction fires (gated on user_id > 0).
    * Without this, every messaging-backed session would default to
    * user 0 and silently bypass both.  Safe to set without locking:
    * the session_t isn't yet visible to any other thread. */
   if (user_id > 0) {
      s->metrics.user_id = user_id;
   }

   /* Post-restart history restore.  Without this, daemon restart
    * silently breaks the forever-conversation contract: DB still has
    * the channel binding + conv_id, but the new in-memory session
    * starts empty, so the LLM has zero prior context while
    * conv_db_add_message_ex keeps appending to the same conv row.
    * Loads the conv's full history (image markers stripped — vision
    * blobs blow context for no benefit on a text-only channel).
    * Empty result (fresh conv just created by
    * resolve_channel_conversation_id) is a no-op since the array
    * starts empty.  conv_db_get_messages also enforces ownership
    * against user_id — defense in depth on the FK already in place.
    * Still safe to call without engine locks: session_t is private. */
   if (conversation_id > 0 && user_id > 0) {
      size_t restored_chars = 0;
      struct json_object *loaded = memory_history_load_from_db(conversation_id, user_id,
                                                               &restored_chars);
      if (loaded) {
         size_t restored_count = (size_t)json_object_array_length(loaded);
         if (restored_count > 0) {
            pthread_mutex_lock(&s->history_mutex);
            if (s->conversation_history) {
               json_object_put(s->conversation_history);
            }
            s->conversation_history = loaded;
            pthread_mutex_unlock(&s->history_mutex);
            OLOG_INFO("messaging: restored %zu messages (%zu chars) into session %u from conv %lld",
                      restored_count, restored_chars, s->session_id, (long long)conversation_id);
         } else {
            /* Fresh conv — nothing to restore.  Free the empty array
             * we just allocated. */
            json_object_put(loaded);
         }
      }
   }

   /* Re-acquire slots mutex and install.  Defensive re-scan against
    * concurrent dispatchers (single-worker today means `target` will
    * still be free, but future refactors can't trip on this). */
   pthread_mutex_lock(&s_session_slots_mutex);
   if (s_session_slots[target].session) {
      target = MESSAGING_MAX_SESSIONS;
      for (size_t i = 0; i < MESSAGING_MAX_SESSIONS; i++) {
         if (!s_session_slots[i].session) {
            target = i;
            break;
         }
      }
      if (target == MESSAGING_MAX_SESSIONS) {
         /* All slots filled during the unlock window.  Tear down the
          * just-created session and drop the inbound.  Same shape as
          * the post-eviction race-loss path above. */
         OLOG_WARNING("messaging: lost slot race during create for %s:%s — dropping inbound",
                      provider, provider_address);
         pthread_mutex_unlock(&s_session_slots_mutex);
         session_destroy(s->session_id);
         return NULL;
      }
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

/* Per-provider outbound shaping.
 *
 * `max_outbound_chars` is the engine-side cap before the driver hits
 * the provider's own limit.  Set conservatively below the documented
 * provider ceilings so concat-segment encoding (UCS-2 on emoji, GSM-7
 * vs Unicode, etc.) doesn't push us over.
 *
 * `channel_hint` is appended to the per-turn system prompt so the LLM
 * shapes its response to the channel.  NULL means "no special
 * channel constraints" (Telegram has 4096 chars and renders markdown,
 * close enough to WebUI/voice that we don't constrain). */
typedef struct {
   size_t max_outbound_chars; /* 0 = no engine-side cap */
   const char *channel_hint;
} provider_outbound_t;

static provider_outbound_t provider_outbound_for(const char *provider) {
   if (provider && strcmp(provider, "sms") == 0) {
      /* ECHO's hard ceiling (echo/include/pdu.h:42-43):
       *   PDU_MAX_SEGMENTS       = 10
       *   PDU_UCS2_CHARS_PER_SEG = 67   (140 octets − 6 UDH ÷ 2 bytes)
       *   v1 is UCS-2-only even for ASCII bodies — see the pdu.h
       *   block comment.  So 10 × 67 = 670 chars hard cap, regardless
       *   of content.
       *
       * Using 670 BYTES (strlen) is safe because UTF-8 byte length is
       * always ≥ UCS-2 code-unit count: 1-byte ASCII → 1 UCS-2;
       * 2- or 3-byte UTF-8 → 1 UCS-2; 4-byte UTF-8 (emoji supplementary
       * plane) → 2 UCS-2.  strlen ≤ 670 guarantees the encoded UCS-2
       * fits in 10 segments. */
      return (provider_outbound_t){
         .max_outbound_chars = 670,
         .channel_hint =
             "[Delivery channel: SMS.  Your reply is being sent as a text message, NOT to a "
             "voice or web client.  HARD CONSTRAINTS for this reply, regardless of what the "
             "user asked for: "
             "(1) under 400 characters total — count them; "
             "(2) plain text only — NO markdown bold/italic, NO headers, NO bullet lists, NO "
             "emoji; "
             "(3) if the user asks for 'everything', 'all', a deep explanation, a list of "
             "items, or anything that naturally wants a long answer, give a 1-2 sentence "
             "summary and offer the WebUI for the full version (e.g. \"Quick version: X.  Want "
             "the full breakdown?  I can pull it up in the WebUI.\"). "
             "Anything you write over 670 characters will be hard-truncated mid-sentence "
             "before delivery, leaving the user with a cut-off message.]",
      };
   }
   /* Telegram and future Discord / Slack — no engine-side cap for v1. */
   return (provider_outbound_t){ .max_outbound_chars = 0, .channel_hint = NULL };
}

/* Truncate `text` in place to fit within `max_chars`, appending the
 * ASCII marker " ... [truncated]" at the cut.  ASCII-only so we don't
 * force UCS-2 encoding on the SMS side (which would halve segment
 * capacity).  Returns true iff a truncation happened. */
static bool truncate_outbound_in_place(char *text, size_t max_chars) {
   if (!text || max_chars == 0) {
      return false;
   }
   size_t len = strlen(text);
   if (len <= max_chars) {
      return false;
   }
   static const char marker[] = " ... [truncated]";
   const size_t marker_len = sizeof(marker) - 1;
   if (max_chars <= marker_len) {
      text[max_chars] = '\0';
      return true;
   }
   size_t cut_at = max_chars - marker_len;
   memcpy(text + cut_at, marker, marker_len);
   text[cut_at + marker_len] = '\0';
   return true;
}

static void process_inbound(inbound_item_t *item) {
   /* Forever-binding: every messaging-backed exchange persists into one
    * conversations row per (provider, provider_address).  First inbound
    * for a channel creates the conv; subsequent inbound reuses it.
    * /new clears the binding and the next inbound starts a fresh conv.
    * LCM handles in-place context compaction; the recovery worker
    * extracts memory incrementally via last_extracted_msg_id.
    * resolve_channel_conversation_id returns 0 on any failure — when
    * that happens we still dispatch the turn, but messages won't
    * persist to DB for this turn (degraded mode, logged at the
    * resolver).
    *
    * Resolved BEFORE get_or_create_messaging_session so the
    * conversation_id can flow into session creation — when the
    * session is freshly created (e.g., daemon just restarted), the
    * conv's full history is restored into session->conversation_history
    * so the LLM picks up where the user left off. */
   int64_t conv_id = resolve_channel_conversation_id(item->provider, item->provider_address,
                                                     item->user_id);

   session_t *session = get_or_create_messaging_session(item->provider, item->provider_address,
                                                        item->user_id, conv_id);
   if (!session) {
      OLOG_ERROR("messaging: failed to acquire session for %s:%s", item->provider,
                 item->provider_address);
      return;
   }

   /* Build the per-turn channel hint, including a truncation-feedback
    * note when the prior assistant reply was cut.  The truncation
    * marker lives in the assistant history (we replaced it
    * post-truncation in the previous turn), so detecting prior
    * truncation is just a substring check — no separate per-session
    * "was-truncated" state needed. */
   provider_outbound_t cfg = provider_outbound_for(item->provider);
   char channel_hint_buf[MESSAGING_CHANNEL_HINT_BUF_SIZE] = { 0 };
   const char *channel_hint = cfg.channel_hint;
   if (channel_hint) {
      char *last_assistant = session_get_last_message_content(session, "assistant");
      /* Anchor the truncation-detect on the exact marker the
       * truncator emits at the END of an oversized message (see
       * truncate_outbound_in_place).  Suffix-match (not strstr) so
       * user content containing the literal "[truncated]" mid-text
       * doesn't trip the guardrail. */
      static const char trunc_marker[] = " ... [truncated]";
      bool prev_truncated = false;
      if (last_assistant) {
         size_t la_len = strlen(last_assistant);
         size_t mk_len = sizeof(trunc_marker) - 1;
         if (la_len >= mk_len && strcmp(last_assistant + la_len - mk_len, trunc_marker) == 0) {
            prev_truncated = true;
         }
      }
      free(last_assistant);
      if (prev_truncated) {
         snprintf(channel_hint_buf, sizeof(channel_hint_buf),
                  "%s  IMPORTANT: your previous reply OVERFLOWED and was truncated "
                  "mid-sentence — the user got only the text before the [truncated] marker "
                  "visible in your last assistant message above.  This is the failure mode "
                  "the constraints above are meant to prevent.  Your reply this turn MUST be "
                  "1-2 sentences max, even if it means deferring detail to the WebUI.",
                  channel_hint);
         channel_hint = channel_hint_buf;
      }
   }

   text_input_dispatch_opts_t opts = {
      .conversation_id = conv_id,
      .auth_user_id = item->user_id,
      .sentence_cb = NULL,
      .sentence_userdata = NULL,
      .on_user_msg_added = NULL,
      .user_msg_added_ctx = NULL,
      .channel_hint = channel_hint,
   };

   char *response = core_text_input_dispatch(session, item->body, NULL, NULL, NULL, 0, &opts);

   if (!response || response[0] == '\0') {
      session_release(session);
      free(response);
      return;
   }

   /* Provider-side truncation safety net.  If the LLM ignored the
    * channel hint and produced an oversized reply, cap it here.  When
    * we truncate, we also REPLACE the assistant message in session
    * history with the truncated version — so the LLM "sees" what the
    * user actually received on subsequent turns and can correctly
    * reference what was (and wasn't) delivered. */
   if (cfg.max_outbound_chars > 0) {
      size_t original_len = strlen(response);
      if (truncate_outbound_in_place(response, cfg.max_outbound_chars)) {
         OLOG_WARNING("messaging: truncated %s outbound %zu→%zu chars for %s:%s", item->provider,
                      original_len, strlen(response), item->provider, item->provider_address);
         if (!session_replace_last_message_content(session, "assistant", response)) {
            /* Either the assistant message uses multi-part content
             * (vision) or history was mutated under us — log only. */
            OLOG_DEBUG("messaging: couldn't replace assistant history with truncated text");
         }
      }
   }

   /* Persist the assistant message to conv_db so the WebUI conversation
    * list shows the full thread, the recovery worker can extract
    * memory incrementally, and LCM context_expand can drill back to
    * original text after compaction.  We persist the POST-truncation
    * text — that's what the user actually received, and what the
    * in-memory history now holds.  conv_db_add_message_ex enforces
    * ownership; passing user_id is mandatory.  Failure is logged but
    * non-fatal: the turn already happened in memory and the response
    * still goes to the driver. */
   if (conv_id > 0) {
      int64_t assistant_msg_id = 0;
      int rc = conv_db_add_message_ex(conv_id, item->user_id, "assistant", response,
                                      &assistant_msg_id);
      if (rc == AUTH_DB_SUCCESS && assistant_msg_id > 0) {
         session_stamp_last_message_id(session, "assistant", assistant_msg_id);
      } else if (rc != AUTH_DB_SUCCESS) {
         OLOG_WARNING("messaging: failed to persist assistant message to conv %lld (rc=%d)",
                      (long long)conv_id, rc);
      }
   }

   /* Touch last_used_at so the SMS active-conversation window slides
    * forward.  Also drives outbound rate-limit accounting and the
    * "most-recent channel" sort in any future WebUI surfacing.  Cheap
    * UPDATE; failure is silent (not enough signal to log). */
   touch_channel_last_used(item->provider, item->provider_address);

   session_release(session);

   /* Send the (possibly truncated) response back via the originating
    * driver.  Pass provider_address natively so the driver can skip
    * the JSON parse on the hot path.  Builder still constructs the
    * address_json blob for providers that need extras.  user_id flows
    * from the inbound item so drivers that scope per-user state (SMS
    * audit log + rate buckets in phone_service) accumulate against
    * the correct DAWN user, not a hardcoded admin fallback. */
   const messaging_driver_t *drv = find_driver(item->provider);
   if (drv && drv->send_text) {
      char address_json[MESSAGING_ADDRESS_JSON_BUF_SIZE];
      build_address_json_for(item->provider, item->provider_address, address_json,
                             sizeof(address_json));
      drv->send_text(item->user_id, item->provider_address, address_json, response);
   }
   free(response);

   /* Deferred self-reset processing.  When the LLM's tool call
    * targeted this very session for `/new`, mark_pending_reset_if_self
    * set the flag instead of triggering an immediate reset (which
    * would have UAF'd this worker thread).  Now that the response is
    * sent and our retain on `session` is dropped, it's safe to
    * actually reset.  Clear the flag under the slots mutex
    * (concurrent worker access is impossible — single worker thread —
    * but defensive against future multi-worker refactors), then
    * perform the standard reset_channel path. */
   bool do_deferred_reset = false;
   pthread_mutex_lock(&s_session_slots_mutex);
   for (size_t i = 0; i < MESSAGING_MAX_SESSIONS; i++) {
      if (s_session_slots[i].session && strcmp(s_session_slots[i].provider, item->provider) == 0 &&
          strcmp(s_session_slots[i].provider_address, item->provider_address) == 0 &&
          s_session_slots[i].pending_reset) {
         s_session_slots[i].pending_reset = false;
         do_deferred_reset = true;
         break;
      }
   }
   pthread_mutex_unlock(&s_session_slots_mutex);

   if (do_deferred_reset) {
      OLOG_INFO("messaging: processing deferred self-reset for %s:%s", item->provider,
                item->provider_address);
      evict_session_slot(item->provider, item->provider_address);
      clear_channel_conversation_id(item->provider, item->provider_address);
   }
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

/* =============================================================================
 * SMS inbound entry point (called by phone_service.c on echo/events)
 * ============================================================================= */

int messaging_engine_handle_sms_inbound(const char *sender_e164,
                                        const char *sender_display,
                                        const char *body,
                                        int64_t timestamp) {
   if (!sender_e164 || sender_e164[0] == '\0' || !body) {
      return MESSAGING_FAILURE;
   }
   if (!atomic_load(&s_initialized)) {
      return MESSAGING_FAILURE;
   }

   size_t body_len = strlen(body);

   /* Pre-DB general inbound rate limit, same shape as the Telegram
    * dispatcher.  Prevents a flood of texts to the modem from
    * hammering the DB lookup path. */
   char rl_key[MESSAGING_RL_KEY_SIZE];
   snprintf(rl_key, sizeof(rl_key), "sms:%s", sender_e164);
   if (rate_limiter_check(&s_inbound_general_limiter, rl_key)) {
      OLOG_DEBUG("messaging: SMS inbound rate limit hit for %s", sender_e164);
      return MESSAGING_RATE_LIMITED;
   }

   /* /link short-circuit.  This runs BEFORE the wake-word gate
    * because the linking flow is the only way a sender's E.164 ever
    * gets into messaging_channels in the first place — gating /link
    * on "must be linked already" would lock everyone out. */
   if (body_len >= 6 && strncmp(body, "/link ", 6) == 0) {
      if (body_len > MESSAGING_LINK_BODY_CAP) {
         OLOG_WARNING("messaging: oversized SMS /link body from %s (%zu bytes)", sender_e164,
                      body_len);
         link_attempt_log("sms", sender_e164, NULL, "invalid");
         return MESSAGING_FAILURE;
      }
      if (rate_limiter_check(&s_inbound_link_limiter, rl_key)) {
         OLOG_WARNING("messaging: SMS /link rate limit hit for %s", sender_e164);
         link_attempt_log("sms", sender_e164, NULL, "rate_limited");
         return MESSAGING_RATE_LIMITED;
      }
      return handle_link_command("sms", sender_e164, body + 6);
   }

   /* Body length cap (same shape as Telegram path). */
   if (body_len > MESSAGING_MAX_BODY_LEN) {
      OLOG_WARNING("messaging: oversized SMS inbound body from %s (%zu bytes) — falling through",
                   sender_e164, body_len);
      return MESSAGING_UNKNOWN_CHANNEL;
   }

   /* Injection filter — same gate the Telegram path uses, and the same
    * gate that protects every other LLM-ingestion path in DAWN. */
   if (memory_filter_check(body)) {
      OLOG_WARNING("messaging: SMS inbound from %s blocked by injection filter — falling through",
                   sender_e164);
      return MESSAGING_UNKNOWN_CHANNEL;
   }

   /* /new short-circuit — runs BEFORE the wake-word gate so users
    * don't have to say "Hey Friday /new", but AFTER the injection
    * filter and channel-lookup so unlinked senders can't trigger
    * resets.  Same recognition shape as the Telegram path: case-
    * insensitive "/new" + EOF or whitespace; trailing content
    * ignored.  See docs/MESSAGING_CHANNELS_DESIGN.md §13 Phase 2.5. */
   if (body_len >= 4 && (body[0] == '/') && (body[1] == 'n' || body[1] == 'N') &&
       (body[2] == 'e' || body[2] == 'E') && (body[3] == 'w' || body[3] == 'W') &&
       (body_len == 4 || body[4] == ' ' || body[4] == '\t' || body[4] == '\n' || body[4] == '\r')) {
      /* Sender-in-channels guard.  Unlinked phones can't reset
       * someone else's binding.  Capture user_id for the
       * confirmation-send so the driver scopes its audit + rate
       * buckets against the right user. */
      int reset_user_id = lookup_channel_user("sms", sender_e164, NULL, 0);
      if (reset_user_id <= 0) {
         OLOG_DEBUG("messaging: SMS /new from unlinked %s — dropping", sender_e164);
         return MESSAGING_UNKNOWN_CHANNEL;
      }
      int rc = messaging_engine_reset_channel("sms", sender_e164);
      const messaging_driver_t *drv = find_driver("sms");
      if (drv && drv->send_text) {
         char address_json[MESSAGING_ADDRESS_JSON_BUF_SIZE];
         build_address_json_for("sms", sender_e164, address_json, sizeof(address_json));
         const char *msg =
             (rc == MESSAGING_SUCCESS)
                 ? "Started a new conversation. Previous context is preserved in the WebUI."
                 : "Couldn't reset the conversation (internal error). Try again or check the "
                   "WebUI.";
         engine_send_async(drv, reset_user_id, sender_e164, address_json, msg);
      }
      OLOG_INFO("messaging: /new from sms:%s (rc=%d)", sender_e164, rc);
      return MESSAGING_SUCCESS;
   }

   /* Active-conversation window — skip the wake-word gate when this
    * sender's channel had an LLM-bound exchange recently.  Mirrors the
    * iMessage thread metaphor: once you're in a back-and-forth with
    * Friday, you don't need to re-announce her name on every reply.
    * The window slides forward on each successful exchange (see
    * touch_channel_last_used in process_inbound).  Telegram/Discord/
    * Slack don't need this (LLM-exclusive — every linked-sender
    * message routes to LLM unconditionally). */
   bool active_window = sms_within_active_window(sender_e164);
   const char *cmd = NULL;
   if (active_window) {
      /* No wake-word required — route the full body as the user
       * command.  Sender-in-channels was implicitly verified by the
       * window check (window only returns true for linked channels). */
      cmd = body;
   } else {
      /* Wake-word prefix gate.  Use the start-anchored matcher
       * (wake_word_check_prefix); the existing wake_word_check() uses
       * strstr substring search which is exploitable on
       * arbitrary-sender text. */
      wake_word_result_t wr = wake_word_check_prefix(body);
      if (!wr.detected) {
         return MESSAGING_UNKNOWN_CHANNEL; /* caller falls through */
      }
      /* Pass just the command remainder to the LLM, not the wake
       * word.  If the user only said the wake word with no command
       * ("hey friday"), there's nothing for the LLM to act on. */
      cmd = (wr.has_command && wr.command) ? wr.command : "";
   }

   /* Sender-in-channels check.  Even with a perfect wake-word prefix
    * (or an open active-window match), an unlinked phone number
    * can't reach the LLM. */
   int user_id = lookup_channel_user("sms", sender_e164, NULL, 0);
   if (user_id <= 0) {
      OLOG_DEBUG("messaging: SMS from unlinked %s passed wake-word but no channel — dropping",
                 sender_e164);
      return MESSAGING_UNKNOWN_CHANNEL;
   }

   if (cmd[0] == '\0') {
      return MESSAGING_SUCCESS; /* handled — no-op */
   }

   return enqueue_inbound("sms", sender_e164, sender_display ? sender_display : "", cmd, timestamp,
                          user_id);
}
