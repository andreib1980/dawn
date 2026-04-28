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
 * WebUI Server Implementation - libwebsockets HTTP + WebSocket handling
 */

#define _GNU_SOURCE /* For strcasestr */

#include "webui/webui_server.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <ifaddrs.h>
#include <json-c/json.h>
#include <libwebsockets.h>
#include <limits.h>
#include <mosquitto.h>
#include <net/if.h>
#include <pthread.h>
#include <signal.h>
#include <sodium.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/random.h>
#include <unistd.h>

#include "webui/webui_internal.h"

#ifdef ENABLE_WEBUI_AUDIO
#include "webui/webui_audio.h"
#endif

#include "config/config_env.h"
#include "config/config_parser.h"
#include "config/dawn_config.h"
#include "core/command_router.h"
#include "core/missed_notifications_db.h"
#include "core/ocp_helpers.h"
#include "core/rate_limiter.h"
#include "core/scheduler.h"
#include "core/scheduler_db.h"
#include "core/session_manager.h"
#include "core/text_filter.h"
#include "core/worker_pool.h"
#include "dawn.h"
#include "llm/llm_command_parser.h"
#include "llm/llm_context.h"
#include "llm/llm_interface.h"
#include "llm/llm_local_provider.h"
#include "llm/llm_tools.h"
#include "logging.h"
#include "state_machine.h"
#ifdef DAWN_ENABLE_SMARTTHINGS_TOOL
#include "tools/smartthings_service.h"
#endif
#include "tts/tts_preprocessing.h"
#include "ui/metrics.h"
#include "utils/string_utils.h"
#include "version.h"
#include "webui/webui_always_on.h"
#include "webui/webui_music.h"

#ifdef ENABLE_AUTH
#include "auth/auth_crypto.h"
#include "auth/auth_db.h"
#include "core/buf_printf.h"
#include "memory/memory_context.h"
#include "webui/webui_doc_library.h"
#ifdef DAWN_ENABLE_CALENDAR_TOOL
#include "webui/webui_calendar.h"
#endif
#ifdef DAWN_ENABLE_EMAIL_TOOL
#include "webui/webui_email.h"
#endif
#include "webui/webui_contacts.h"
#if defined(DAWN_ENABLE_CALENDAR_TOOL) || defined(DAWN_ENABLE_EMAIL_TOOL)
#include "webui/webui_oauth.h"
#endif
/* HTTP rate limiting and CSRF constants moved to webui_http.c */
#endif /* ENABLE_AUTH */

/* =============================================================================
 * Module State
 *
 * These variables are non-static to allow access from split handler modules
 * via webui_internal.h. They should NOT be accessed from outside webui_*.c.
 * ============================================================================= */

struct lws_context *s_lws_context = NULL;
static pthread_t s_webui_thread;
volatile int s_running = 0;
volatile int s_client_count = 0;
int s_port = 0;
char s_www_path[256] = { 0 };
pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Config modification mutex - protects against concurrent config reads during writes */
pthread_rwlock_t s_config_rwlock = PTHREAD_RWLOCK_INITIALIZER;

/* HTTP rate limiting state (s_csrf_used, s_csrf_rate, s_login_rate)
 * moved to webui_http.c */

/* =============================================================================
 * Response Queue (worker -> WebUI thread)
 *
 * Workers cannot call lws_write() directly (not thread-safe).
 * They queue responses here, then call lws_cancel_service() to wake
 * the WebUI thread, which processes the queue in LWS_CALLBACK_EVENT_WAIT_CANCELLED.
 *
 * ws_response_t is defined in webui_internal.h
 * ============================================================================= */

ws_response_t s_response_queue[WEBUI_RESPONSE_QUEUE_SIZE];
int s_queue_head = 0;
int s_queue_tail = 0;
pthread_mutex_t s_queue_mutex = PTHREAD_MUTEX_INITIALIZER;

/* =============================================================================
 * Token-to-Session Mapping (for reconnection)
 *
 * token_mapping_t is defined in webui_internal.h
 * ============================================================================= */

token_mapping_t s_token_map[MAX_TOKEN_MAPPINGS];
pthread_mutex_t s_token_mutex = PTHREAD_MUTEX_INITIALIZER;

/* =============================================================================
 * Active Connection Registry (for proactive notifications like force logout)
 *
 * Tracks all active WebSocket connections so we can find and notify specific
 * clients (e.g., when their auth session is revoked).
 * ============================================================================= */

#define MAX_ACTIVE_CONNECTIONS 64
static ws_connection_t *s_active_connections[MAX_ACTIVE_CONNECTIONS];
static pthread_mutex_t s_conn_registry_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Forward decl for early call sites (reconnect handler) — defined later in file. */
static void deliver_missed_notifications(ws_connection_t *conn);

static void register_connection(ws_connection_t *conn) {
   pthread_mutex_lock(&s_conn_registry_mutex);
   for (int i = 0; i < MAX_ACTIVE_CONNECTIONS; i++) {
      if (s_active_connections[i] == NULL) {
         s_active_connections[i] = conn;
         pthread_mutex_unlock(&s_conn_registry_mutex);
         return;
      }
   }
   /* Registry full - shouldn't happen if MAX_ACTIVE_CONNECTIONS >= max_clients */
   OLOG_WARNING("WebUI: Connection registry full, cannot track connection");
   pthread_mutex_unlock(&s_conn_registry_mutex);
}

static void unregister_connection(ws_connection_t *conn) {
   pthread_mutex_lock(&s_conn_registry_mutex);
   for (int i = 0; i < MAX_ACTIVE_CONNECTIONS; i++) {
      if (s_active_connections[i] == conn) {
         s_active_connections[i] = NULL;
         pthread_mutex_unlock(&s_conn_registry_mutex);
         return;
      }
   }
   pthread_mutex_unlock(&s_conn_registry_mutex);
}

/* Constant-time comparison via libsodium (replaces hand-rolled secure_token_compare) */

/**
 * @brief Expire stale token mappings (must be called with s_token_mutex held)
 */
static void cleanup_expired_tokens(void) {
   time_t now = time(NULL);
   for (int i = 0; i < MAX_TOKEN_MAPPINGS; i++) {
      if (s_token_map[i].in_use && (now - s_token_map[i].created) > AUTH_COOKIE_MAX_AGE) {
         OLOG_INFO("WebUI: Expiring token slot %d (age %lds)", i,
                   (long)(now - s_token_map[i].created));
         explicit_bzero(s_token_map[i].token, sizeof(s_token_map[i].token));
         s_token_map[i].in_use = false;
      }
   }
}

void register_token(const char *token, uint32_t session_id) {
   pthread_mutex_lock(&s_token_mutex);

   /* Lazy cleanup: expire stale tokens on each registration */
   cleanup_expired_tokens();

   /* Find existing or empty slot */
   int empty_slot = -1;
   for (int i = 0; i < MAX_TOKEN_MAPPINGS; i++) {
      if (s_token_map[i].in_use &&
          sodium_memcmp(s_token_map[i].token, token, WEBUI_SESSION_TOKEN_LEN - 1) == 0) {
         /* Update existing */
         s_token_map[i].session_id = session_id;
         s_token_map[i].created = time(NULL);
         pthread_mutex_unlock(&s_token_mutex);
         return;
      }
      if (!s_token_map[i].in_use && empty_slot < 0) {
         empty_slot = i;
      }
   }

   if (empty_slot >= 0) {
      strncpy(s_token_map[empty_slot].token, token, WEBUI_SESSION_TOKEN_LEN - 1);
      s_token_map[empty_slot].token[WEBUI_SESSION_TOKEN_LEN - 1] = '\0';
      s_token_map[empty_slot].session_id = session_id;
      s_token_map[empty_slot].created = time(NULL);
      s_token_map[empty_slot].in_use = true;
   } else {
      /* Table full - prefer evicting entries for destroyed sessions */
      int evict = -1;
      for (int i = 0; i < MAX_TOKEN_MAPPINGS; i++) {
         if (s_token_map[i].in_use) {
            session_t *s = session_get_for_reconnect(s_token_map[i].session_id);
            if (!s) {
               evict = i; /* Session gone — safe to evict */
               break;
            }
            session_release(s);
         }
      }
      if (evict < 0) {
         /* All entries have live sessions — fall back to oldest */
         evict = 0;
         for (int i = 1; i < MAX_TOKEN_MAPPINGS; i++) {
            if (s_token_map[i].created < s_token_map[evict].created) {
               evict = i;
            }
         }
      }
      explicit_bzero(s_token_map[evict].token, sizeof(s_token_map[evict].token));
      strncpy(s_token_map[evict].token, token, WEBUI_SESSION_TOKEN_LEN - 1);
      s_token_map[evict].token[WEBUI_SESSION_TOKEN_LEN - 1] = '\0';
      s_token_map[evict].session_id = session_id;
      s_token_map[evict].created = time(NULL);
      s_token_map[evict].in_use = true;
   }

   pthread_mutex_unlock(&s_token_mutex);
}

void unregister_tokens_for_session(uint32_t session_id) {
   pthread_mutex_lock(&s_token_mutex);

   for (int i = 0; i < MAX_TOKEN_MAPPINGS; i++) {
      if (s_token_map[i].in_use && s_token_map[i].session_id == session_id) {
         OLOG_INFO("WebUI: Clearing token slot %d for destroyed session %u", i, session_id);
         explicit_bzero(s_token_map[i].token, sizeof(s_token_map[i].token));
         s_token_map[i].in_use = false;
      }
   }

   pthread_mutex_unlock(&s_token_mutex);
}

session_t *lookup_session_by_token(const char *token) {
   if (!token) {
      return NULL;
   }

   pthread_mutex_lock(&s_token_mutex);

   for (int i = 0; i < MAX_TOKEN_MAPPINGS; i++) {
      /* Use constant-time comparison to prevent timing attacks */
      if (s_token_map[i].in_use &&
          sodium_memcmp(s_token_map[i].token, token, WEBUI_SESSION_TOKEN_LEN - 1) == 0) {
         /* Check token age against AUTH_COOKIE_MAX_AGE */
         if ((time(NULL) - s_token_map[i].created) > AUTH_COOKIE_MAX_AGE) {
            OLOG_INFO("WebUI: Token %.4s... expired (session %u)", token,
                      s_token_map[i].session_id);
            explicit_bzero(s_token_map[i].token, sizeof(s_token_map[i].token));
            s_token_map[i].in_use = false;
            pthread_mutex_unlock(&s_token_mutex);
            return NULL;
         }

         uint32_t session_id = s_token_map[i].session_id;
         pthread_mutex_unlock(&s_token_mutex);

         /* Look up actual session - use reconnect variant to allow disconnected sessions */
         session_t *session = session_get_for_reconnect(session_id);
         if (session) {
            /* Session exists - reconnect handler will clear disconnected flag */
            OLOG_INFO("WebUI: Found existing session %u for token %.4s...", session_id, token);
            return session;
         }

         /* Session destroyed — clean up the stale token map entry */
         pthread_mutex_lock(&s_token_mutex);
         if (s_token_map[i].in_use && s_token_map[i].session_id == session_id) {
            explicit_bzero(s_token_map[i].token, sizeof(s_token_map[i].token));
            s_token_map[i].in_use = false;
         }
         pthread_mutex_unlock(&s_token_mutex);

         OLOG_INFO("WebUI: Token %.4s... mapped to session %u but session destroyed (cleaned up)",
                   token, session_id);
         return NULL;
      }
   }

   pthread_mutex_unlock(&s_token_mutex);
   return NULL;
}

/* Discovery cache and allowed path prefixes moved to webui_config.c */

/* =============================================================================
 * Per-WebSocket Connection Data
 *
 * ws_connection_t is defined in webui_internal.h
 * ============================================================================= */

/* =============================================================================
 * MIME Type Mapping
 * ============================================================================= */

static const struct {
   const char *extension;
   const char *mime_type;
} s_mime_types[] = {
   { ".html", "text/html" },
   { ".htm", "text/html" },
   { ".css", "text/css" },
   { ".js", "application/javascript" },
   { ".json", "application/json" },
   { ".wasm", "application/wasm" },
   { ".png", "image/png" },
   { ".jpg", "image/jpeg" },
   { ".jpeg", "image/jpeg" },
   { ".gif", "image/gif" },
   { ".svg", "image/svg+xml" },
   { ".ico", "image/x-icon" },
   { ".woff", "font/woff" },
   { ".woff2", "font/woff2" },
   { ".ttf", "font/ttf" },
   { ".txt", "text/plain" },
   { NULL, NULL },
};

const char *get_mime_type(const char *path) {
   const char *ext = strrchr(path, '.');
   if (!ext) {
      return "application/octet-stream";
   }

   for (int i = 0; s_mime_types[i].extension != NULL; i++) {
      if (strcasecmp(ext, s_mime_types[i].extension) == 0) {
         return s_mime_types[i].mime_type;
      }
   }

   return "application/octet-stream";
}

/**
 * @brief Check if a path contains directory traversal patterns
 *
 * Checks for literal ".." as well as URL-encoded variants (%2e, %252e)
 * to prevent path traversal attacks.
 *
 * @param path The URL path to check
 * @return true if traversal pattern detected, false if path is safe
 */
bool contains_path_traversal(const char *path) {
   if (!path) {
      return false;
   }

   /* Check for literal ".." as a path component (not inside filenames like "file..ext") */
   const char *p = path;
   while ((p = strstr(p, "..")) != NULL) {
      /* Check if preceded by '/' or at start of string */
      bool at_start = (p == path) || (*(p - 1) == '/');
      /* Check if followed by '/' or null or end of string */
      bool at_end = (p[2] == '\0') || (p[2] == '/');
      if (at_start && at_end) {
         return true;
      }
      p += 2;
   }

   /* Check for URL-encoded variants (case-insensitive) */
   /* %2e = ".", so %2e%2e = ".." */
   if (strcasestr(path, "%2e%2e") != NULL) {
      return true;
   }

   /* Single encoded dot followed by literal dot or vice versa */
   if (strcasestr(path, "%2e.") != NULL || strcasestr(path, ".%2e") != NULL) {
      return true;
   }

   /* Double-encoded: %252e = "%2e" after first decode */
   if (strcasestr(path, "%252e") != NULL) {
      return true;
   }

   return false;
}

/**
 * @brief Validate that a resolved path is within the allowed directory
 *
 * Uses realpath() to resolve symlinks and relative paths, then verifies
 * the canonical path is within the www directory.
 *
 * @param filepath The filesystem path to validate
 * @param www_path The allowed base directory (www path)
 * @return true if path is safe (within www_path), false otherwise
 */
bool is_path_within_www(const char *filepath, const char *www_path) {
   char resolved_path[PATH_MAX];
   char resolved_www[PATH_MAX];

   /* Resolve the www base path */
   if (realpath(www_path, resolved_www) == NULL) {
      OLOG_ERROR("WebUI: Cannot resolve www path: %s", www_path);
      return false;
   }

   /* Resolve the requested file path */
   if (realpath(filepath, resolved_path) == NULL) {
      /* File doesn't exist - check parent directory instead */
      /* This allows serving files that don't exist yet (404 handled elsewhere) */
      char *filepath_copy = strdup(filepath);
      if (!filepath_copy) {
         return false;
      }

      /* Find last slash to get parent directory */
      char *last_slash = strrchr(filepath_copy, '/');
      if (last_slash && last_slash != filepath_copy) {
         *last_slash = '\0';
         if (realpath(filepath_copy, resolved_path) == NULL) {
            free(filepath_copy);
            return false;
         }
      } else {
         free(filepath_copy);
         return false;
      }
      free(filepath_copy);
   }

   /* Ensure resolved path starts with resolved www path */
   size_t www_len = strlen(resolved_www);
   if (strncmp(resolved_path, resolved_www, www_len) != 0) {
      return false;
   }

   /* Ensure it's either exact match or followed by '/' */
   if (resolved_path[www_len] != '\0' && resolved_path[www_len] != '/') {
      return false;
   }

   return true;
}

/* =============================================================================
 * Vision Image Validation
 *
 * Security-hardened validation for uploaded images. Prevents memory
 * amplification attacks by checking sizes BEFORE allocation.
 * ============================================================================= */

/**
 * @brief Check if MIME type is in vision whitelist
 *
 * SVG explicitly excluded to prevent XSS attacks.
 *
 * @param mime_type MIME type string to check
 * @return true if allowed, false if rejected
 */
static bool is_vision_mime_allowed(const char *mime_type) {
   if (!mime_type)
      return false;

   /* Explicit whitelist - SVG excluded for XSS prevention */
   static const char *allowed[] = { "image/jpeg", "image/png", "image/gif", "image/webp" };
   static const int num_allowed = sizeof(allowed) / sizeof(allowed[0]);

   for (int i = 0; i < num_allowed; i++) {
      if (strcasecmp(mime_type, allowed[i]) == 0) {
         return true;
      }
   }
   return false;
}

/**
 * @brief Check if string contains only valid base64 characters
 *
 * @param str String to check
 * @param len Length of string
 * @return true if valid base64 charset, false otherwise
 */
static bool is_valid_base64_charset(const char *str, size_t len) {
   for (size_t i = 0; i < len; i++) {
      char c = str[i];
      /* Valid base64: A-Z, a-z, 0-9, +, /, = (padding) */
      if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '+' || c == '/' || c == '=')) {
         return false;
      }
   }
   return true;
}

/**
 * @brief Check image magic bytes against expected MIME type
 *
 * @param data First 16 bytes of decoded image
 * @param len Length of data (may be less than 16)
 * @param mime_type Expected MIME type
 * @return true if magic bytes match, false otherwise
 */
static bool check_image_magic(const unsigned char *data, size_t len, const char *mime_type) {
   if (!data || len < 3 || !mime_type)
      return false;

   /* JPEG: FF D8 FF */
   if (strcasecmp(mime_type, "image/jpeg") == 0) {
      return (len >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF);
   }

   /* PNG: 89 50 4E 47 0D 0A 1A 0A */
   if (strcasecmp(mime_type, "image/png") == 0) {
      static const unsigned char png_magic[] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
      return (len >= 8 && memcmp(data, png_magic, 8) == 0);
   }

   /* GIF: 47 49 46 38 ("GIF8") */
   if (strcasecmp(mime_type, "image/gif") == 0) {
      return (len >= 4 && memcmp(data, "GIF8", 4) == 0);
   }

   /* WebP: RIFF....WEBP */
   if (strcasecmp(mime_type, "image/webp") == 0) {
      return (len >= 12 && memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "WEBP", 4) == 0);
   }

   return false;
}

/**
 * @brief Validate base64-encoded image data (security-hardened)
 *
 * Validation order prevents memory amplification attacks:
 * 1. Check MIME type against whitelist (no allocation)
 * 2. Check base64 string length against WEBUI_MAX_BASE64_SIZE (no allocation)
 * 3. Validate base64 character set (no allocation)
 * 4. Decode first 24 bytes only to check magic bytes (stack buffer)
 *
 * @param base64_data Base64 encoded image string
 * @param base64_len Length of base64 string
 * @param mime_type MIME type string
 * @return 0 on success, error code on failure:
 *         1 = Invalid MIME type
 *         2 = Base64 data too large
 *         3 = Invalid base64 characters
 *         4 = Magic bytes don't match MIME type
 *         5 = Decode error
 */
static int validate_image_data(const char *base64_data, size_t base64_len, const char *mime_type) {
   /* Step 1: MIME type whitelist (no allocation) */
   if (!is_vision_mime_allowed(mime_type)) {
      OLOG_WARNING("WebUI Vision: Rejected MIME type: %s", mime_type ? mime_type : "(null)");
      return 1;
   }

   /* Step 2: Size check (no allocation) - prevents memory amplification */
   if (base64_len > WEBUI_MAX_BASE64_SIZE) {
      OLOG_WARNING("WebUI Vision: Base64 data too large: %zu > %zu", base64_len,
                   (size_t)WEBUI_MAX_BASE64_SIZE);
      return 2;
   }

   /* Step 3: Validate base64 character set (no allocation) */
   if (!is_valid_base64_charset(base64_data, base64_len)) {
      OLOG_WARNING("WebUI Vision: Invalid base64 characters");
      return 3;
   }

   /* Step 4: Decode minimal prefix to check magic bytes (stack buffer)
    * We need 24 base64 chars to decode 18 bytes (enough for all magic checks) */
   if (base64_len >= 24) {
      /* Create null-terminated subset for decoder */
      char prefix[25];
      memcpy(prefix, base64_data, 24);
      prefix[24] = '\0';

      size_t decoded_len = 0;
      unsigned char *decoded = ocp_base64_decode(prefix, &decoded_len);
      if (!decoded) {
         OLOG_WARNING("WebUI Vision: Failed to decode base64 prefix");
         return 5;
      }

      bool magic_ok = check_image_magic(decoded, decoded_len, mime_type);
      free(decoded);

      if (!magic_ok) {
         OLOG_WARNING("WebUI Vision: Magic bytes don't match MIME type %s", mime_type);
         return 4;
      }
   }

   return 0;
}

/* =============================================================================
 * HTTP Session Data
 *
 * struct http_session_data and related constants are in webui_internal.h
 * ============================================================================= */

/* =============================================================================
 * Session Token Generation
 * ============================================================================= */

/**
 * @brief Generate a cryptographically secure session token
 *
 * @param token_out Buffer to store the hex-encoded token (must be WEBUI_SESSION_TOKEN_LEN)
 * @return 0 on success, 1 on failure (token_out will be empty string)
 */
int generate_session_token(char token_out[WEBUI_SESSION_TOKEN_LEN]) {
   uint8_t random_bytes[16];
   if (getrandom(random_bytes, 16, 0) != 16) {
      /* Security: fail instead of using weak random - getrandom should never fail on modern Linux
       */
      OLOG_ERROR("getrandom() failed - cannot generate secure session token");
      token_out[0] = '\0';
      return 1;
   }
   for (int i = 0; i < 16; i++) {
      snprintf(&token_out[i * 2], 3, "%02x", random_bytes[i]);
   }
   token_out[32] = '\0';
   return 0;
}

/* =============================================================================
 * Response Queue Functions
 * ============================================================================= */

void queue_response(ws_response_t *resp) {
   pthread_mutex_lock(&s_queue_mutex);

   int next_tail = (s_queue_tail + 1) % WEBUI_RESPONSE_QUEUE_SIZE;
   if (next_tail == s_queue_head) {
      /* Queue full - free oldest entry's heap allocations, then drop it */
      OLOG_WARNING("WebUI: Response queue full, dropping oldest entry");
      free_response(&s_response_queue[s_queue_head]);
      s_queue_head = (s_queue_head + 1) % WEBUI_RESPONSE_QUEUE_SIZE;
   }

   s_response_queue[s_queue_tail] = *resp;
   s_queue_tail = next_tail;

   /* Warn when queue utilization exceeds 75% — indicates potential drain bottleneck */
   int count = (s_queue_tail >= s_queue_head)
                   ? (s_queue_tail - s_queue_head)
                   : (WEBUI_RESPONSE_QUEUE_SIZE - s_queue_head + s_queue_tail);
   if (count > (WEBUI_RESPONSE_QUEUE_SIZE * 3 / 4) && (count % 128 == 0)) {
      OLOG_WARNING("WebUI: Response queue at %d%% (%d/%d entries)",
                   (count * 100) / WEBUI_RESPONSE_QUEUE_SIZE, count, WEBUI_RESPONSE_QUEUE_SIZE);
   }

   pthread_mutex_unlock(&s_queue_mutex);

   /* Wake up lws_service() to process queue */
   if (s_lws_context) {
      lws_cancel_service(s_lws_context);
   }
}

void free_response(ws_response_t *resp) {
   switch (resp->type) {
      case WS_RESP_STATE:
         free(resp->state.state);
         free(resp->state.detail);
         free(resp->state.tools_json);
         break;
      case WS_RESP_TRANSCRIPT:
         free(resp->transcript.role);
         free(resp->transcript.text);
         break;
      case WS_RESP_ERROR:
         free(resp->error.code);
         free(resp->error.message);
         break;
      case WS_RESP_SESSION:
         free(resp->session_token.token);
         break;
      case WS_RESP_AUDIO:
         free(resp->audio.data);
         break;
      case WS_RESP_AUDIO_END:
         /* No data to free */
         break;
      case WS_RESP_CONTEXT:
         /* No data to free - all inline values */
         break;
      case WS_RESP_STREAM_START:
      case WS_RESP_STREAM_DELTA:
      case WS_RESP_STREAM_END:
         /* No data to free - text[] is inline fixed buffer */
         break;
      case WS_RESP_METRICS_UPDATE:
         /* No data to free - all inline values */
         break;
      case WS_RESP_COMPACTION_COMPLETE:
         free(resp->compaction.summary);
         break;
      case WS_RESP_THINKING_START:
      case WS_RESP_THINKING_DELTA:
      case WS_RESP_THINKING_END:
         /* No data to free - text[] is inline fixed buffer */
         break;
      case WS_RESP_CONVERSATION_RESET:
         /* No data to free */
         break;
      case WS_RESP_MUSIC_DATA:
         free(resp->audio.data);
         break;
      case WS_RESP_MUSIC_POSITION:
         /* No data to free - all inline values */
         break;
      case WS_RESP_MUSIC_STATE:
      case WS_RESP_MUSIC_ERROR:
         free(resp->music_json.json);
         break;
      case WS_RESP_SCHEDULER_NOTIFICATION:
         free(resp->scheduler_json.json);
         break;
      case WS_RESP_JSON:
         free(resp->generic_json.json);
         break;
   }
}

/* =============================================================================
 * WebSocket Send Helpers — LWS SERVICE THREAD ONLY
 *
 * These functions call lws_write() directly. libwebsockets is NOT thread-safe
 * for writes — lws_write() must only be called from within an LWS callback
 * (the service thread) or from process_one_response() which runs on that thread.
 *
 * To send from other threads (LLM workers, music streaming, tool execution),
 * use queue_response() to enqueue a ws_response_t. The LWS service thread
 * will dequeue and send it via process_one_response().
 * ============================================================================= */

/* LWS requires LWS_PRE bytes before the buffer for protocol framing.
 * WS_SEND_BUFFER_SIZE is defined in webui_internal.h */

int send_json_message(struct lws *wsi, const char *json) {
   size_t len = strlen(json);

   /* Log messages that may be too large for HTTP/2 frames (default 16KB) */
   if (len > 12000) {
      /* Try to extract message type for better logging.
       * Handle both "type":"value" and "type": "value" (with optional space). */
      const char *type_start = strstr(json, "\"type\":");
      char type_buf[32] = "unknown";
      if (type_start) {
         type_start += 7; /* skip past "type": */
         while (*type_start == ' ')
            type_start++; /* skip optional whitespace */
         if (*type_start == '"')
            type_start++; /* skip opening quote */
         const char *type_end = strchr(type_start, '"');
         if (type_end && (size_t)(type_end - type_start) < sizeof(type_buf) - 1) {
            strncpy(type_buf, type_start, (size_t)(type_end - type_start));
            type_buf[type_end - type_start] = '\0';
         }
      }
      OLOG_INFO("WebUI: Large message via send_json_message: type=%s, size=%zu bytes", type_buf,
                len);
   }

   /* For messages that fit in the stack buffer, use the fast path */
   if (len < WS_SEND_BUFFER_SIZE - LWS_PRE) {
      unsigned char buf[LWS_PRE + WS_SEND_BUFFER_SIZE];
      memcpy(&buf[LWS_PRE], json, len);

      int written = lws_write(wsi, &buf[LWS_PRE], len, LWS_WRITE_TEXT);
      if (written < (int)len) {
         OLOG_ERROR("WebUI: lws_write failed (wrote %d of %zu)", written, len);
         return LWS_CLOSE_CONNECTION;
      }
      return 0;
   }

   /* Large messages (e.g. transcript with attached documents): heap-allocate */
   unsigned char *buf = malloc(LWS_PRE + len);
   if (!buf) {
      OLOG_ERROR("WebUI: Failed to allocate send buffer (%zu bytes)", LWS_PRE + len);
      return LWS_CLOSE_CONNECTION;
   }

   memcpy(&buf[LWS_PRE], json, len);
   int written = lws_write(wsi, &buf[LWS_PRE], len, LWS_WRITE_TEXT);
   free(buf);

   if (written < (int)len) {
      OLOG_ERROR("WebUI: lws_write failed (wrote %d of %zu)", written, len);
      return LWS_CLOSE_CONNECTION;
   }

   return 0;
}

int send_binary_message(struct lws *wsi, uint8_t msg_type, const uint8_t *data, size_t len) {
   if (!wsi) {
      OLOG_ERROR("WebUI: send_binary_message called with NULL wsi");
      return LWS_CLOSE_CONNECTION;
   }

   /* Allocate buffer with LWS_PRE padding + 1 byte for type + data */
   size_t total_len = 1 + len;
   unsigned char *buf = malloc(LWS_PRE + total_len);
   if (!buf) {
      OLOG_ERROR("WebUI: Failed to allocate binary send buffer (%zu bytes)", LWS_PRE + total_len);
      return LWS_CLOSE_CONNECTION;
   }

   buf[LWS_PRE] = msg_type;
   if (data && len > 0) {
      memcpy(&buf[LWS_PRE + 1], data, len);
   }

   int written = lws_write(wsi, &buf[LWS_PRE], total_len, LWS_WRITE_BINARY);
   free(buf);

   if (written < 0) {
      OLOG_ERROR("WebUI: lws_write binary failed with error %d", written);
      return LWS_CLOSE_CONNECTION;
   }

   if (written < (int)total_len) {
      OLOG_ERROR("WebUI: lws_write binary partial write (%d of %zu)", written, total_len);
      return LWS_CLOSE_CONNECTION;
   }

   return 0;
}

void send_audio_impl(struct lws *wsi, const uint8_t *data, size_t len) {
   int ret = send_binary_message(wsi, WS_BIN_AUDIO_OUT, data, len);
   if (ret != 0) {
      OLOG_ERROR("WebUI: Failed to send audio chunk (%zu bytes)", len);
   }
}

void send_audio_end_impl(struct lws *wsi, bool is_opus) {
   uint8_t codec_flag = is_opus ? 1 : 0;
   send_binary_message(wsi, WS_BIN_AUDIO_SEGMENT_END, &codec_flag, 1);
}

/**
 * @brief Check if client capabilities include Opus audio codec support
 *
 * Parses the payload for capabilities.audio_codecs array and checks
 * if "opus" is in the list. Falls back to PCM if not specified.
 *
 * @param payload JSON object from init/reconnect message
 * @return true if client supports Opus, false otherwise
 */
bool check_opus_capability(struct json_object *payload) {
   if (!payload) {
      return false;
   }

   struct json_object *capabilities;
   if (!json_object_object_get_ex(payload, "capabilities", &capabilities)) {
      return false;
   }

   struct json_object *audio_codecs;
   if (!json_object_object_get_ex(capabilities, "audio_codecs", &audio_codecs)) {
      return false;
   }

   if (!json_object_is_type(audio_codecs, json_type_array)) {
      return false;
   }

   int len = json_object_array_length(audio_codecs);
   /* Defensive bound: no client should send more than 16 codecs */
   if (len > 16) {
      OLOG_WARNING("WebUI: Too many audio codecs in capability list (%d), ignoring", len);
      return false;
   }

   for (int i = 0; i < len; i++) {
      struct json_object *codec = json_object_array_get_idx(audio_codecs, i);
      if (codec && json_object_is_type(codec, json_type_string)) {
         const char *codec_str = json_object_get_string(codec);
         if (codec_str && strcmp(codec_str, "opus") == 0) {
            return true;
         }
      }
   }

   return false;
}

void send_state_impl_full(struct lws *wsi,
                          const char *state,
                          const char *detail,
                          const char *tools_json) {
   /* Build state message with proper JSON escaping using json-c */
   struct json_object *obj = json_object_new_object();
   struct json_object *payload = json_object_new_object();

   json_object_object_add(payload, "state", json_object_new_string(state));

   if (detail && detail[0] != '\0') {
      json_object_object_add(payload, "detail", json_object_new_string(detail));
   }

   if (tools_json && tools_json[0] != '\0') {
      /* tools_json is already a JSON array string like "[{...},{...}]" */
      struct json_object *tools = json_tokener_parse(tools_json);
      if (tools) {
         json_object_object_add(payload, "tools", tools);
      }
   }

   json_object_object_add(obj, "type", json_object_new_string("state"));
   json_object_object_add(obj, "payload", payload);

   const char *json_str = json_object_to_json_string(obj);
   send_json_message(wsi, json_str);
   json_object_put(obj);
}

void send_state_impl(struct lws *wsi, const char *state, const char *detail) {
   send_state_impl_full(wsi, state, detail, NULL);
}

static void send_conversation_reset_impl(struct lws *wsi) {
   struct json_object *obj = json_object_new_object();

   json_object_object_add(obj, "type", json_object_new_string("conversation_reset"));

   const char *json_str = json_object_to_json_string(obj);
   send_json_message(wsi, json_str);
   json_object_put(obj);
}

static void send_music_position_impl(struct lws *wsi, double position_sec, uint32_t duration_sec) {
   struct json_object *obj = json_object_new_object();

   json_object_object_add(obj, "type", json_object_new_string("music_position"));

   struct json_object *payload = json_object_new_object();
   json_object_object_add(payload, "position_sec", json_object_new_double(position_sec));
   json_object_object_add(payload, "duration_sec", json_object_new_int(duration_sec));

   json_object_object_add(obj, "payload", payload);

   const char *json_str = json_object_to_json_string(obj);
   send_json_message(wsi, json_str);
   json_object_put(obj);
}

static void send_transcript_impl_ex(struct lws *wsi,
                                    const char *role,
                                    const char *text,
                                    bool replay,
                                    bool server_saved) {
   /* Escape JSON special characters in text */
   struct json_object *obj = json_object_new_object();
   struct json_object *payload = json_object_new_object();

   json_object_object_add(payload, "role", json_object_new_string(role));
   json_object_object_add(payload, "text", json_object_new_string(text));
   if (replay) {
      json_object_object_add(payload, "replay", json_object_new_boolean(true));
   }
   if (server_saved) {
      json_object_object_add(payload, "server_saved", json_object_new_boolean(true));
   }
   json_object_object_add(obj, "type", json_object_new_string("transcript"));
   json_object_object_add(obj, "payload", payload);

   const char *json_str = json_object_to_json_string(obj);
   send_json_message(wsi, json_str);
   json_object_put(obj);
}

static void send_transcript_impl(struct lws *wsi, const char *role, const char *text) {
   send_transcript_impl_ex(wsi, role, text, false, false);
}

void send_error_impl(struct lws *wsi, const char *code, const char *message) {
   struct json_object *obj = json_object_new_object();
   struct json_object *payload = json_object_new_object();

   json_object_object_add(payload, "code", json_object_new_string(code));
   json_object_object_add(payload, "message", json_object_new_string(message));
   json_object_object_add(payload, "recoverable", json_object_new_boolean(1));
   json_object_object_add(obj, "type", json_object_new_string("error"));
   json_object_object_add(obj, "payload", payload);

   const char *json_str = json_object_to_json_string(obj);
   send_json_message(wsi, json_str);
   json_object_put(obj);
}

static void send_force_logout_impl(struct lws *wsi, const char *reason) {
   char json[256];
   snprintf(json, sizeof(json), "{\"type\":\"force_logout\",\"payload\":{\"reason\":\"%s\"}}",
            reason ? reason : "Session revoked");
   send_json_message(wsi, json);
}

/* Forward declaration — defined later but called from scheduler dismiss handler */
void scheduler_broadcast_notification(const sched_event_t *event, const char *text);

static void send_session_token_impl(ws_connection_t *conn, const char *token) {
   char json[512];

   /* Include auth state in session response to avoid separate config fetch */
   if (conn->authenticated) {
      /* Fetch is_admin from DB (not cached to prevent stale state) */
      auth_session_t auth_session;
      bool is_admin = false;
      if (auth_db_get_session(conn->auth_session_token, &auth_session) == AUTH_DB_SUCCESS) {
         is_admin = auth_session.is_admin;
      }
      snprintf(json, sizeof(json),
               "{\"type\":\"session\",\"payload\":{\"token\":\"%s\","
               "\"authenticated\":true,\"username\":\"%s\",\"is_admin\":%s}}",
               token, conn->username, is_admin ? "true" : "false");
   } else {
      snprintf(json, sizeof(json),
               "{\"type\":\"session\",\"payload\":{\"token\":\"%s\","
               "\"authenticated\":false}}",
               token);
   }
   send_json_message(conn->wsi, json);
}

static void send_config_impl(struct lws *wsi) {
   char json[256];
   snprintf(json, sizeof(json), "{\"type\":\"config\",\"payload\":{\"audio_chunk_ms\":%d}}",
            g_config.webui.audio_chunk_ms);
   send_json_message(wsi, json);
}

/* Compile-time constant: feature flags sent to all clients on connect */
static const char s_server_features_json[] = "{\"type\":\"server_features\",\"payload\":{"
                                             "\"smartthings\":"
#ifdef DAWN_ENABLE_SMARTTHINGS_TOOL
                                             "true"
#else
                                             "false"
#endif
                                             ",\"home_assistant\":"
#ifdef DAWN_ENABLE_HOMEASSISTANT_TOOL
                                             "true"
#else
                                             "false"
#endif
                                             "}}";

static void send_server_features(struct lws *wsi) {
   send_json_message(wsi, s_server_features_json);
}

/**
 * Queue the 4 init messages through the response queue instead of calling
 * lws_write() directly. This ensures one write per writeable callback,
 * preventing WebSocket frame corruption ("Invalid frame header").
 *
 * Messages queued: session token, config, server_features, state(idle).
 */
static void queue_init_messages(ws_connection_t *conn, const char *token) {
   session_t *session = conn->session;

   /* 1. Session token — build the same JSON as send_session_token_impl() */
   {
      ws_response_t resp = { 0 };
      resp.session = session;
      resp.type = WS_RESP_SESSION;
      resp.session_token.token = strdup(token);
      queue_response(&resp);
   }

   /* 2. Config — pre-serialize as JSON string */
   {
      char json[256];
      snprintf(json, sizeof(json), "{\"type\":\"config\",\"payload\":{\"audio_chunk_ms\":%d}}",
               g_config.webui.audio_chunk_ms);
      ws_response_t resp = { 0 };
      resp.session = session;
      resp.type = WS_RESP_JSON;
      resp.generic_json.json = strdup(json);
      queue_response(&resp);
   }

   /* 3. Server features — compile-time constant string */
   {
      ws_response_t resp = { 0 };
      resp.session = session;
      resp.type = WS_RESP_JSON;
      resp.generic_json.json = strdup(s_server_features_json);
      queue_response(&resp);
   }

   /* 4. State: idle */
   {
      ws_response_t resp = { 0 };
      resp.session = session;
      resp.type = WS_RESP_STATE;
      resp.state.state = strdup("idle");
      resp.state.detail = NULL;
      resp.state.tools_json = NULL;
      queue_response(&resp);
   }
}

static void send_context_impl(struct lws *wsi,
                              int current_tokens,
                              int max_tokens,
                              float threshold) {
   char json[256];
   float usage_pct = (max_tokens > 0) ? (float)current_tokens / (float)max_tokens * 100.0f : 0;
   snprintf(json, sizeof(json),
            "{\"type\":\"context\",\"payload\":{\"current\":%d,\"max\":%d,\"usage\":%.1f,"
            "\"threshold\":%.0f}}",
            current_tokens, max_tokens, usage_pct, threshold * 100.0f);
   send_json_message(wsi, json);
}

static void send_metrics_impl(struct lws *wsi,
                              const char *state,
                              int ttft_ms,
                              float token_rate,
                              int context_pct) {
   char json[256];
   snprintf(json, sizeof(json),
            "{\"type\":\"metrics_update\",\"payload\":{\"state\":\"%s\",\"ttft_ms\":%d,"
            "\"token_rate\":%.1f,\"context_percent\":%d}}",
            state, ttft_ms, token_rate, context_pct);
   send_json_message(wsi, json);
}

static void send_compaction_impl(struct lws *wsi,
                                 int tokens_before,
                                 int tokens_after,
                                 int messages_summarized,
                                 const char *summary,
                                 int level) {
   struct json_object *obj = json_object_new_object();
   struct json_object *payload = json_object_new_object();

   json_object_object_add(payload, "tokens_before", json_object_new_int(tokens_before));
   json_object_object_add(payload, "tokens_after", json_object_new_int(tokens_after));
   json_object_object_add(payload, "messages_summarized", json_object_new_int(messages_summarized));
   json_object_object_add(payload, "level", json_object_new_int(level));
   if (summary) {
      json_object_object_add(payload, "summary", json_object_new_string(summary));
   }
   json_object_object_add(obj, "type", json_object_new_string("context_compacted"));
   json_object_object_add(obj, "payload", payload);

   const char *json = json_object_to_json_string(obj);
   send_json_message(wsi, json);
   json_object_put(obj);
}

/* =============================================================================
 * LLM Streaming Impl Functions (ChatGPT-style real-time text)
 *
 * Protocol:
 *   stream_start - Create new assistant entry, enter streaming state
 *   stream_delta - Append text to current entry
 *   stream_end   - Finalize entry, exit streaming state
 * ============================================================================= */

static void send_stream_start_impl(struct lws *wsi, uint32_t stream_id) {
   char json[128];
   snprintf(json, sizeof(json), "{\"type\":\"stream_start\",\"payload\":{\"stream_id\":%u}}",
            stream_id);
   send_json_message(wsi, json);
}

static void send_stream_delta_impl(struct lws *wsi, uint32_t stream_id, const char *text) {
   struct json_object *obj = json_object_new_object();
   struct json_object *payload = json_object_new_object();

   json_object_object_add(payload, "stream_id", json_object_new_int((int32_t)stream_id));
   json_object_object_add(payload, "delta", json_object_new_string(text));
   json_object_object_add(obj, "type", json_object_new_string("stream_delta"));
   json_object_object_add(obj, "payload", payload);

   const char *json_str = json_object_to_json_string(obj);
   send_json_message(wsi, json_str);
   json_object_put(obj);
}

static void send_stream_end_impl(struct lws *wsi, uint32_t stream_id, const char *reason) {
   struct json_object *obj = json_object_new_object();
   struct json_object *payload = json_object_new_object();

   json_object_object_add(payload, "stream_id", json_object_new_int((int32_t)stream_id));
   json_object_object_add(payload, "reason", json_object_new_string(reason ? reason : "complete"));
   json_object_object_add(obj, "type", json_object_new_string("stream_end"));
   json_object_object_add(obj, "payload", payload);

   const char *json_str = json_object_to_json_string(obj);
   send_json_message(wsi, json_str);
   json_object_put(obj);
}

/* =============================================================================
 * Extended Thinking WebSocket Helpers
 * ============================================================================= */

static void send_thinking_start_impl(struct lws *wsi, uint32_t stream_id, const char *provider) {
   struct json_object *obj = json_object_new_object();
   struct json_object *payload = json_object_new_object();

   json_object_object_add(payload, "stream_id", json_object_new_int((int32_t)stream_id));
   json_object_object_add(payload, "provider",
                          json_object_new_string(provider ? provider : "unknown"));
   json_object_object_add(obj, "type", json_object_new_string("thinking_start"));
   json_object_object_add(obj, "payload", payload);

   const char *json_str = json_object_to_json_string(obj);
   send_json_message(wsi, json_str);
   json_object_put(obj);
}

static void send_thinking_delta_impl(struct lws *wsi, uint32_t stream_id, const char *text) {
   struct json_object *obj = json_object_new_object();
   struct json_object *payload = json_object_new_object();

   json_object_object_add(payload, "stream_id", json_object_new_int((int32_t)stream_id));
   json_object_object_add(payload, "delta", json_object_new_string(text));
   json_object_object_add(obj, "type", json_object_new_string("thinking_delta"));
   json_object_object_add(obj, "payload", payload);

   const char *json_str = json_object_to_json_string(obj);
   send_json_message(wsi, json_str);
   json_object_put(obj);
}

static void send_thinking_end_impl(struct lws *wsi, uint32_t stream_id, int has_content) {
   struct json_object *obj = json_object_new_object();
   struct json_object *payload = json_object_new_object();

   json_object_object_add(payload, "stream_id", json_object_new_int((int32_t)stream_id));
   json_object_object_add(payload, "has_content", json_object_new_boolean(has_content));
   json_object_object_add(obj, "type", json_object_new_string("thinking_end"));
   json_object_object_add(obj, "payload", payload);

   const char *json_str = json_object_to_json_string(obj);
   send_json_message(wsi, json_str);
   json_object_put(obj);
}

static void send_reasoning_summary_impl(struct lws *wsi, uint32_t stream_id, int reasoning_tokens) {
   struct json_object *obj = json_object_new_object();
   struct json_object *payload = json_object_new_object();

   json_object_object_add(payload, "stream_id", json_object_new_int((int32_t)stream_id));
   json_object_object_add(payload, "reasoning_tokens", json_object_new_int(reasoning_tokens));
   json_object_object_add(obj, "type", json_object_new_string("reasoning_summary"));
   json_object_object_add(obj, "payload", payload);

   const char *json_str = json_object_to_json_string(obj);
   send_json_message(wsi, json_str);
   json_object_put(obj);
}

/* =============================================================================
 * Response Queue Processing (called from WebUI thread)
 *
 * IMPORTANT: libwebsockets only allows ONE lws_write() per writeable callback.
 * This function processes only ONE response per call. If more responses are
 * pending, it requests a writeable callback via lws_callback_on_writable().
 * ============================================================================= */

static void process_one_response(void) {
   pthread_mutex_lock(&s_queue_mutex);

   if (s_queue_head == s_queue_tail) {
      pthread_mutex_unlock(&s_queue_mutex);
      return;
   }

   /* Scan forward from head to find the first entry whose socket isn't choked.
    * This prevents a slow client (e.g. ESP32 Tier 2 over WiFi/SSL) from
    * blocking the entire response queue for all other sessions (WebUI, Tier 1).
    * Per-session FIFO ordering is preserved: if Session A is choked, ALL entries
    * for Session A are skipped (since choke is per-socket, not per-message). */
   int queue_len = (s_queue_tail >= s_queue_head)
                       ? (s_queue_tail - s_queue_head)
                       : (WEBUI_RESPONSE_QUEUE_SIZE - s_queue_head + s_queue_tail);

   /* Cap scan to first 64 entries to bound mutex hold time. If all 64 are
    * choked, we'll retry on the next writable callback. */
   int scan_limit = queue_len < 64 ? queue_len : 64;
   int found_offset = -1;
   for (int i = 0; i < scan_limit; i++) {
      int idx = (s_queue_head + i) % WEBUI_RESPONSE_QUEUE_SIZE;
      ws_response_t *candidate = &s_response_queue[idx];

      /* Disconnected sessions can be dequeued immediately (freed below) */
      if (!candidate->session || candidate->session->disconnected) {
         found_offset = i;
         break;
      }

      /* Check flow control — skip choked sockets, schedule writeable retry */
      ws_connection_t *cand_conn = (ws_connection_t *)candidate->session->client_data;
      if (cand_conn && cand_conn->wsi && lws_send_pipe_choked(cand_conn->wsi)) {
         lws_callback_on_writable(cand_conn->wsi);
         continue;
      }

      found_offset = i;
      break;
   }

   if (found_offset < 0) {
      /* All queued entries are for choked sockets — nothing to send now */
      pthread_mutex_unlock(&s_queue_mutex);
      return;
   }

   /* Dequeue the found entry. Shift earlier (choked) entries forward to fill
    * the gap, then advance head. Preserves FIFO ordering for all sessions. */
   int found_idx = (s_queue_head + found_offset) % WEBUI_RESPONSE_QUEUE_SIZE;
   ws_response_t resp = s_response_queue[found_idx];
   for (int i = found_offset; i > 0; i--) {
      int dst = (s_queue_head + i) % WEBUI_RESPONSE_QUEUE_SIZE;
      int src = (s_queue_head + i - 1) % WEBUI_RESPONSE_QUEUE_SIZE;
      s_response_queue[dst] = s_response_queue[src];
   }
   /* Zero the vacated head slot to prevent double-free: the shift copied
    * pointer members forward, so the old head still holds stale pointers. */
   memset(&s_response_queue[s_queue_head], 0, sizeof(ws_response_t));
   s_queue_head = (s_queue_head + 1) % WEBUI_RESPONSE_QUEUE_SIZE;
   bool more_pending = (s_queue_head != s_queue_tail);
   pthread_mutex_unlock(&s_queue_mutex);

   /* Find connection for this session */
   if (!resp.session || resp.session->disconnected) {
      free_response(&resp);
      /* If more responses, schedule another callback */
      if (more_pending && s_lws_context) {
         lws_cancel_service(s_lws_context);
      }
      return;
   }

   ws_connection_t *conn = (ws_connection_t *)resp.session->client_data;
   if (!conn || !conn->wsi) {
      free_response(&resp);
      if (more_pending && s_lws_context) {
         lws_cancel_service(s_lws_context);
      }
      return;
   }

   /* Send via lws_write (one write per callback!) */
   switch (resp.type) {
      case WS_RESP_STATE:
         send_state_impl_full(conn->wsi, resp.state.state, resp.state.detail,
                              resp.state.tools_json);
         /* If LLM pipeline sent "idle" and always-on is in PROCESSING, resume listening.
          * processing_complete resets internal state (buffer, VAD, cooldown) but set_state
          * does NOT send a WebSocket message. We must notify the client via the response
          * queue (not lws_write directly, since we already wrote once in this callback). */
         if (conn->always_on && always_on_get_state(conn->always_on) == ALWAYS_ON_PROCESSING &&
             strcmp(resp.state.state, "idle") == 0) {
            always_on_processing_complete(conn->always_on);
            ws_response_t ao_resp = { 0 };
            ao_resp.session = conn->session;
            ao_resp.type = WS_RESP_JSON;
            ao_resp.generic_json.json = strdup(
                "{\"type\":\"always_on_state\",\"payload\":{\"state\":\"listening\"}}");
            queue_response(&ao_resp);
         }
         free(resp.state.state);
         free(resp.state.detail);
         free(resp.state.tools_json);
         break;
      case WS_RESP_TRANSCRIPT:
         send_transcript_impl_ex(conn->wsi, resp.transcript.role, resp.transcript.text, false,
                                 resp.transcript.server_saved);
         free(resp.transcript.role);
         free(resp.transcript.text);
         break;
      case WS_RESP_ERROR:
         send_error_impl(conn->wsi, resp.error.code, resp.error.message);
         free(resp.error.code);
         free(resp.error.message);
         break;
      case WS_RESP_SESSION:
         send_session_token_impl(conn, resp.session_token.token);
         free(resp.session_token.token);
         break;
      case WS_RESP_AUDIO:
         send_audio_impl(conn->wsi, resp.audio.data, resp.audio.len);
         free(resp.audio.data);
         break;
      case WS_RESP_AUDIO_END:
         send_audio_end_impl(conn->wsi, resp.audio.is_opus);
         break;
      case WS_RESP_MUSIC_DATA:
         send_binary_message(conn->wsi, WS_BIN_MUSIC_DATA, resp.audio.data, resp.audio.len);
         free(resp.audio.data);
         break;
      case WS_RESP_CONTEXT:
         send_context_impl(conn->wsi, resp.context.current_tokens, resp.context.max_tokens,
                           resp.context.threshold);
         break;
      case WS_RESP_STREAM_START:
         send_stream_start_impl(conn->wsi, resp.stream.stream_id);
         break;
      case WS_RESP_STREAM_DELTA:
         send_stream_delta_impl(conn->wsi, resp.stream.stream_id, resp.stream.text);
         /* text[] is inline buffer - no free needed */
         break;
      case WS_RESP_STREAM_END:
         send_stream_end_impl(conn->wsi, resp.stream.stream_id, resp.stream.text);
         /* text[] is inline buffer - no free needed */
         break;
      case WS_RESP_METRICS_UPDATE:
         send_metrics_impl(conn->wsi, resp.metrics.state, resp.metrics.ttft_ms,
                           resp.metrics.token_rate, resp.metrics.context_pct);
         break;
      case WS_RESP_COMPACTION_COMPLETE:
         send_compaction_impl(conn->wsi, resp.compaction.tokens_before,
                              resp.compaction.tokens_after, resp.compaction.messages_summarized,
                              resp.compaction.summary, resp.compaction.level);
         free(resp.compaction.summary);
         break;
      case WS_RESP_THINKING_START:
         send_thinking_start_impl(conn->wsi, resp.stream.stream_id, resp.stream.text);
         /* text[] reused for provider name - no free needed */
         break;
      case WS_RESP_THINKING_DELTA:
         send_thinking_delta_impl(conn->wsi, resp.stream.stream_id, resp.stream.text);
         /* text[] is inline buffer - no free needed */
         break;
      case WS_RESP_THINKING_END:
         send_thinking_end_impl(conn->wsi, resp.stream.stream_id,
                                resp.stream.text[0] == '1'); /* has_content flag */
         /* text[] reused for flag - no free needed */
         break;
      case WS_RESP_REASONING_SUMMARY:
         send_reasoning_summary_impl(conn->wsi, resp.stream.stream_id,
                                     atoi(resp.stream.text)); /* reasoning_tokens as string */
         /* text[] reused for token count - no free needed */
         break;
      case WS_RESP_CONVERSATION_RESET:
         send_conversation_reset_impl(conn->wsi);
         break;
      case WS_RESP_MUSIC_POSITION:
         send_music_position_impl(conn->wsi, resp.music_position.position_sec,
                                  resp.music_position.duration_sec);
         break;
      case WS_RESP_MUSIC_STATE:
      case WS_RESP_MUSIC_ERROR:
         /* Pre-serialized JSON from webui_music.c — just send the string */
         if (resp.music_json.json) {
            send_json_message(conn->wsi, resp.music_json.json);
            free(resp.music_json.json);
            resp.music_json.json = NULL;
         }
         break;
      case WS_RESP_SCHEDULER_NOTIFICATION:
         /* Pre-serialized JSON from scheduler — just send the string */
         if (resp.scheduler_json.json) {
            send_json_message(conn->wsi, resp.scheduler_json.json);
            free(resp.scheduler_json.json);
            resp.scheduler_json.json = NULL;
         }
         break;
      case WS_RESP_JSON:
         /* Generic pre-serialized JSON (init messages, etc.) */
         if (resp.generic_json.json) {
            send_json_message(conn->wsi, resp.generic_json.json);
            free(resp.generic_json.json);
            resp.generic_json.json = NULL;
         }
         break;
   }

   /* If more responses pending, ensure they get processed.
    * lws_callback_on_writable() handles responses for THIS connection.
    * lws_cancel_service() wakes the main loop for responses to OTHER connections. */
   if (more_pending) {
      lws_callback_on_writable(conn->wsi);
      if (s_lws_context) {
         lws_cancel_service(s_lws_context);
      }
   }
}

/* Legacy name for backward compatibility */
static void process_response_queue(void) {
   process_one_response();
}

/* =============================================================================
 * Authentication Helpers
 * ============================================================================= */

#ifdef ENABLE_AUTH

/* HTTP auth helpers (extract_session_cookie, is_request_authenticated)
 * moved to webui_http.c */

/**
 * @brief Check if WebSocket connection is authenticated
 *
 * CRITICAL: Re-validates session against database to prevent TOCTOU attacks
 * where session may have been revoked (password change, admin action, etc.)
 * but cached conn->authenticated flag remains true.
 *
 * Sends UNAUTHORIZED error if not authenticated or session invalid.
 *
 * @param conn WebSocket connection
 * @return true if authenticated with valid session, false otherwise (error sent)
 */
bool conn_require_auth(ws_connection_t *conn) {
   if (!conn->authenticated) {
      send_error_impl(conn->wsi, "UNAUTHORIZED", "Authentication required");
      return false;
   }

   /* Re-validate session from DB (prevents stale session exploitation) */
   auth_session_t session;
   if (auth_db_get_session(conn->auth_session_token, &session) != AUTH_DB_SUCCESS) {
      conn->authenticated = false;
      send_error_impl(conn->wsi, "UNAUTHORIZED", "Session expired or revoked");
      return false;
   }

   return true;
}

/**
 * @brief Check if WebSocket connection has admin privileges
 *
 * CRITICAL: Re-validates is_admin against database to prevent stale cache
 * exploitation if user is demoted mid-session.
 *
 * Sends UNAUTHORIZED if not authenticated, FORBIDDEN if not admin.
 *
 * @param conn WebSocket connection
 * @return true if admin, false otherwise (error sent)
 */
bool conn_require_admin(ws_connection_t *conn) {
   if (!conn->authenticated) {
      send_error_impl(conn->wsi, "UNAUTHORIZED", "Authentication required");
      return false;
   }

   /* Re-validate session from DB (prevents stale is_admin cache) */
   auth_session_t session;
   if (auth_db_get_session(conn->auth_session_token, &session) != AUTH_DB_SUCCESS) {
      conn->authenticated = false;
      send_error_impl(conn->wsi, "UNAUTHORIZED", "Session expired");
      return false;
   }

   if (!session.is_admin) {
      auth_db_log_event("PERMISSION_DENIED", conn->username, conn->client_ip,
                        "Admin access required");
      send_error_impl(conn->wsi, "FORBIDDEN", "Admin access required");
      return false;
   }

   return true;
}

/**
 * @brief Build a personalized system prompt with user settings
 *
 * Supports two modes based on user's persona_mode setting:
 * - "append" (default): Add user settings as additional context at the end
 * - "replace": Prepend user's custom persona with override instruction
 *
 * @param user_id User ID (0 for unauthenticated - returns base prompt copy)
 * @return Allocated prompt string (caller must free)
 */
char *build_user_prompt(int user_id) {
   /* Take an owned copy of the base prompt up-front. get_remote_command_prompt()
    * returns a pointer into a shared static buffer that can be rebuilt in place
    * by invalidate_system_instructions() firing from MQTT callback threads
    * (HUD status / discovery). We may do DB I/O and memory_build_context()
    * below — holding the static pointer across that work would race against
    * a concurrent rebuild and read a torn buffer. */
   const char *source = get_remote_command_prompt();
   if (!source) {
      return NULL;
   }
   char *base_prompt = strdup(source);
   if (!base_prompt) {
      return NULL;
   }

   /* No user ID - return the owned base prompt (transfer ownership) */
   if (user_id <= 0) {
      return base_prompt;
   }

   /* Load user settings */
   auth_user_settings_t settings;
   if (auth_db_get_user_settings(user_id, &settings) != AUTH_DB_SUCCESS) {
      return base_prompt;
   }

   /* Check if any settings are customized */
   bool has_persona = settings.persona_description[0] != '\0';
   bool has_location = settings.location[0] != '\0';
   bool has_timezone = settings.timezone[0] != '\0';
   bool has_units = settings.units[0] != '\0';
   bool is_replace_mode = (strcmp(settings.persona_mode, "replace") == 0);

   if (!has_persona && !has_location && !has_timezone && !has_units) {
      return base_prompt;
   }

   size_t base_len = strlen(base_prompt);

   /* Build memory context if enabled */
   char memory_ctx[4096] = { 0 };
   size_t memory_len = 0;
   if (g_config.memory.enabled) {
      int ctx_len = memory_build_context(user_id, memory_ctx, sizeof(memory_ctx),
                                         g_config.memory.context_budget_tokens);
      if (ctx_len > 0) {
         memory_len = (size_t)ctx_len;
      }
   }

   /* Replace mode: Prepend custom persona with override instruction */
   if (is_replace_mode && has_persona) {
      /* Build replacement prefix (persona 512 + boilerplate ~130 = ~650 max) */
      char prefix[768];
      int prefix_ret = snprintf(prefix, sizeof(prefix),
                                "## Your Identity\n%s\n\n"
                                "IMPORTANT: Use the identity above. Ignore any conflicting persona "
                                "descriptions that follow.\n\n",
                                settings.persona_description);
      /* Clamp to actual buffer content (snprintf may return would-be length on truncation) */
      size_t prefix_len = (prefix_ret > 0 && (size_t)prefix_ret < sizeof(prefix))
                              ? (size_t)prefix_ret
                              : sizeof(prefix) - 1;

      /* Build suffix with other user context (loc 128 + tz 64 + units 16 = ~250 max) */
      char suffix[320];
      size_t suffix_len = 0;
      size_t suffix_rem = sizeof(suffix);

      if (has_location || has_timezone || has_units) {
         BUF_PRINTF(suffix, suffix_len, suffix_rem, "\n\n## User Info\n");
         if (has_location) {
            BUF_PRINTF(suffix, suffix_len, suffix_rem, "Location: %s\n", settings.location);
         }
         if (has_timezone) {
            BUF_PRINTF(suffix, suffix_len, suffix_rem, "Timezone: %s\n", settings.timezone);
         }
         if (has_units) {
            BUF_PRINTF(suffix, suffix_len, suffix_rem, "Preferred units: %s\n", settings.units);
         }
      } else {
         suffix[0] = '\0';
      }

      /* Allocate: prefix + base + suffix + memory */
      char *combined = malloc(prefix_len + base_len + suffix_len + memory_len + 1);
      if (!combined) {
         return base_prompt; /* fallback: transfer ownership of base copy */
      }

      memcpy(combined, prefix, prefix_len);
      memcpy(combined + prefix_len, base_prompt, base_len);
      memcpy(combined + prefix_len + base_len, suffix, suffix_len);
      if (memory_len > 0) {
         memcpy(combined + prefix_len + base_len + suffix_len, memory_ctx, memory_len);
      }
      combined[prefix_len + base_len + suffix_len + memory_len] = '\0';

      OLOG_INFO("Built REPLACE prompt for user_id=%d (%zu + %zu + %zu + %zu bytes)", user_id,
                prefix_len, base_len, suffix_len, memory_len);

      free(base_prompt);
      return combined;
   }

   /* Append mode: Add user context (persona 512 + loc 128 + tz 64 + units 16 + headers ~40) */
   char user_context[1024];
   size_t offset = 0;
   size_t remain = sizeof(user_context);

   BUF_PRINTF(user_context, offset, remain, "\n\n## User Context\n");

   if (has_persona) {
      BUF_PRINTF(user_context, offset, remain, "Additional persona traits: %s\n",
                 settings.persona_description);
   }
   if (has_location) {
      BUF_PRINTF(user_context, offset, remain, "Location: %s\n", settings.location);
   }
   if (has_timezone) {
      BUF_PRINTF(user_context, offset, remain, "Timezone: %s\n", settings.timezone);
   }
   if (has_units) {
      BUF_PRINTF(user_context, offset, remain, "Preferred units: %s\n", settings.units);
   }

   /* Allocate combined prompt with memory context */
   size_t context_len = strlen(user_context);
   char *combined = malloc(base_len + context_len + memory_len + 1);
   if (!combined) {
      return base_prompt; /* fallback: transfer ownership of base copy */
   }

   memcpy(combined, base_prompt, base_len);
   memcpy(combined + base_len, user_context, context_len);
   if (memory_len > 0) {
      memcpy(combined + base_len + context_len, memory_ctx, memory_len);
   }
   combined[base_len + context_len + memory_len] = '\0';

   OLOG_INFO("Built APPEND prompt for user_id=%d (%zu + %zu + %zu bytes)", user_id, base_len,
             context_len, memory_len);

   free(base_prompt);
   return combined;
}

#endif /* ENABLE_AUTH */

/* HTTP auth handlers and protocol callback moved to webui_http.c */

/* =============================================================================
 * JSON Message Handling
 * ============================================================================= */

static void handle_text_message(ws_connection_t *conn,
                                const char *text,
                                size_t len,
                                const char **vision_images,
                                const size_t *vision_image_sizes,
                                const char **vision_mimes,
                                int vision_image_count);
static void handle_cancel_message(ws_connection_t *conn);
/* Config handlers: handle_get_config, handle_set_config, handle_set_secrets,
 * handle_get_audio_devices, handle_list_models, handle_list_interfaces
 * moved to webui_config.c (declarations in webui_internal.h) */
/* Tools handlers: handle_get_tools_config, handle_set_tools_config
 * moved to webui_tools.c (declarations in webui_internal.h) */
/* Audio handler: handle_binary_message
 * moved to webui_audio.c (declaration in webui_internal.h) */
static void handle_get_metrics(ws_connection_t *conn);


/* Queue a JSON response for delivery via the response queue.
 * Safe to call from any context — the JSON is serialized and copied. */
void send_json_response(ws_connection_t *conn, json_object *response) {
   const char *json_str = json_object_to_json_string(response);
   if (!json_str) {
      OLOG_ERROR("WebUI: Failed to serialize JSON response");
      return;
   }

   ws_response_t resp = { 0 };
   resp.session = conn->session;
   resp.type = WS_RESP_JSON;
   resp.generic_json.json = strdup(json_str);
   if (!resp.generic_json.json) {
      OLOG_ERROR("WebUI: Failed to allocate JSON response string");
      return;
   }
   queue_response(&resp);
}


/* Audio device helpers, model/interface discovery moved to webui_config.c */


/* Tool configuration handlers moved to webui_tools.c */


/* =============================================================================
 * Metrics Handler
 * ============================================================================= */

static void handle_get_metrics(ws_connection_t *conn) {
   dawn_metrics_t snapshot;
   metrics_get_snapshot(&snapshot);

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("get_metrics_response"));

   json_object *payload = json_object_new_object();

   /* Session statistics */
   json_object *session = json_object_new_object();
   json_object_object_add(session, "uptime_seconds", json_object_new_int64(metrics_get_uptime()));
   json_object_object_add(session, "queries_total", json_object_new_int(snapshot.queries_total));
   json_object_object_add(session, "queries_cloud", json_object_new_int(snapshot.queries_cloud));
   json_object_object_add(session, "queries_local", json_object_new_int(snapshot.queries_local));
   json_object_object_add(session, "errors", json_object_new_int(snapshot.errors_count));
   json_object_object_add(session, "fallbacks", json_object_new_int(snapshot.fallbacks_count));
   json_object_object_add(session, "bargeins", json_object_new_int(snapshot.bargein_count));
   json_object_object_add(payload, "session", session);

   /* Token usage */
   json_object *tokens = json_object_new_object();
   json_object_object_add(tokens, "cloud_input",
                          json_object_new_int64(snapshot.tokens_cloud_input));
   json_object_object_add(tokens, "cloud_output",
                          json_object_new_int64(snapshot.tokens_cloud_output));
   json_object_object_add(tokens, "local_input",
                          json_object_new_int64(snapshot.tokens_local_input));
   json_object_object_add(tokens, "local_output",
                          json_object_new_int64(snapshot.tokens_local_output));
   json_object_object_add(tokens, "cached", json_object_new_int64(snapshot.tokens_cached));
   json_object_object_add(payload, "tokens", tokens);

   /* Last query timing */
   json_object *last = json_object_new_object();
   json_object_object_add(last, "vad_ms", json_object_new_double(snapshot.last_vad_time_ms));
   json_object_object_add(last, "asr_ms", json_object_new_double(snapshot.last_asr_time_ms));
   json_object_object_add(last, "asr_rtf", json_object_new_double(snapshot.last_asr_rtf));
   json_object_object_add(last, "llm_ttft_ms", json_object_new_double(snapshot.last_llm_ttft_ms));
   json_object_object_add(last, "llm_total_ms", json_object_new_double(snapshot.last_llm_total_ms));
   json_object_object_add(last, "tts_ms", json_object_new_double(snapshot.last_tts_time_ms));
   json_object_object_add(last, "pipeline_ms",
                          json_object_new_double(snapshot.last_total_pipeline_ms));
   json_object_object_add(payload, "last", last);

   /* Average timing */
   json_object *avg = json_object_new_object();
   json_object_object_add(avg, "vad_ms", json_object_new_double(snapshot.avg_vad_ms));
   json_object_object_add(avg, "asr_ms", json_object_new_double(snapshot.avg_asr_ms));
   json_object_object_add(avg, "asr_rtf", json_object_new_double(snapshot.avg_asr_rtf));
   json_object_object_add(avg, "llm_ttft_ms", json_object_new_double(snapshot.avg_llm_ttft_ms));
   json_object_object_add(avg, "llm_total_ms", json_object_new_double(snapshot.avg_llm_total_ms));
   json_object_object_add(avg, "tts_ms", json_object_new_double(snapshot.avg_tts_ms));
   json_object_object_add(avg, "pipeline_ms",
                          json_object_new_double(snapshot.avg_total_pipeline_ms));
   json_object_object_add(payload, "averages", avg);

   /* Real-time state */
   json_object *state = json_object_new_object();
   json_object_object_add(state, "current",
                          json_object_new_string(dawn_state_name(snapshot.current_state)));
   json_object_object_add(state, "vad_probability",
                          json_object_new_double(snapshot.current_vad_probability));
   json_object_object_add(state, "audio_buffer_fill",
                          json_object_new_double(snapshot.audio_buffer_fill_pct));
   json_object_object_add(payload, "state", state);

   /* AEC status */
   json_object *aec = json_object_new_object();
   json_object_object_add(aec, "enabled", json_object_new_boolean(snapshot.aec_enabled));
   json_object_object_add(aec, "calibrated", json_object_new_boolean(snapshot.aec_calibrated));
   json_object_object_add(aec, "delay_ms", json_object_new_int(snapshot.aec_delay_ms));
   json_object_object_add(aec, "correlation", json_object_new_double(snapshot.aec_correlation));
   json_object_object_add(payload, "aec", aec);

   /* Summarizer stats */
   json_object *summarizer = json_object_new_object();
   json_object_object_add(summarizer, "backend",
                          json_object_new_string(snapshot.summarizer_backend));
   json_object_object_add(summarizer, "threshold",
                          json_object_new_int64(snapshot.summarizer_threshold));
   json_object_object_add(summarizer, "calls", json_object_new_int(snapshot.summarizer_call_count));
   json_object_object_add(payload, "summarizer", summarizer);

   json_object_object_add(response, "payload", payload);
   send_json_response(conn, response);
   json_object_put(response);
}


/* handle_set_tools_config moved to webui_tools.c */


/* Admin handlers moved to webui_admin.c */


/* Personal settings handlers moved to webui_settings.c */


/* Session handlers moved to webui_session.c */


/* History handlers moved to webui_history.c */

/* =============================================================================
 * Smart Home Message Dispatch (SmartThings / Home Assistant)
 *
 * Extracted into a helper to avoid fragile #ifdef interleaving in the main
 * else-if message handler chain.  Returns true if the message was handled.
 * ============================================================================= */
static bool handle_smart_home_message(ws_connection_t *conn,
                                      const char *type,
                                      struct json_object *payload) {
#ifdef DAWN_ENABLE_SMARTTHINGS_TOOL
   if (strcmp(type, "smartthings_status") == 0) {
      /* Get SmartThings connection status */
      struct json_object *response = json_object_new_object();
      json_object_object_add(response, "type",
                             json_object_new_string("smartthings_status_response"));
      struct json_object *resp_payload = json_object_new_object();

      json_object_object_add(resp_payload, "configured",
                             json_object_new_boolean(smartthings_is_configured()));
      json_object_object_add(resp_payload, "authenticated",
                             json_object_new_boolean(smartthings_is_authenticated()));

      if (smartthings_is_configured()) {
         st_status_t status;
         smartthings_get_status(&status);
         json_object_object_add(resp_payload, "has_tokens",
                                json_object_new_boolean(status.has_tokens));
         json_object_object_add(resp_payload, "tokens_valid",
                                json_object_new_boolean(status.tokens_valid));
         json_object_object_add(resp_payload, "token_expiry",
                                json_object_new_int64(status.token_expiry));
         json_object_object_add(resp_payload, "devices_count",
                                json_object_new_int(status.devices_count));
         json_object_object_add(resp_payload, "auth_mode",
                                json_object_new_string(
                                    smartthings_auth_mode_str(status.auth_mode)));
      }

      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return true;
   }
   if (strcmp(type, "smartthings_get_auth_url") == 0) {
      if (!conn_require_admin(conn))
         return true;
      struct json_object *response = json_object_new_object();
      json_object_object_add(response, "type",
                             json_object_new_string("smartthings_auth_url_response"));
      struct json_object *resp_payload = json_object_new_object();

      if (!smartthings_is_configured()) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string(
                                    "SmartThings client credentials not configured"));
      } else {
         char redirect_uri[256];
         struct json_object *redirect_obj = NULL;
         if (json_object_object_get_ex(payload, "redirect_uri", &redirect_obj) &&
             json_object_is_type(redirect_obj, json_type_string)) {
            strncpy(redirect_uri, json_object_get_string(redirect_obj), sizeof(redirect_uri) - 1);
            redirect_uri[sizeof(redirect_uri) - 1] = '\0';
         } else {
            const dawn_config_t *cfg = config_get();
            snprintf(redirect_uri, sizeof(redirect_uri), "%s://localhost:%d/smartthings/callback",
                     cfg->webui.https ? "https" : "http", cfg->webui.port);
         }

         char auth_url[1024];
         st_error_t err = smartthings_get_auth_url(redirect_uri, auth_url, sizeof(auth_url));
         if (err == ST_OK) {
            json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
            json_object_object_add(resp_payload, "auth_url", json_object_new_string(auth_url));
            json_object_object_add(resp_payload, "redirect_uri",
                                   json_object_new_string(redirect_uri));
         } else {
            json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
            json_object_object_add(resp_payload, "error",
                                   json_object_new_string(smartthings_error_str(err)));
         }
      }

      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return true;
   }
   if (strcmp(type, "smartthings_exchange_code") == 0) {
      if (!conn_require_admin(conn))
         return true;
      struct json_object *response = json_object_new_object();
      json_object_object_add(response, "type",
                             json_object_new_string("smartthings_exchange_response"));
      struct json_object *resp_payload = json_object_new_object();

      struct json_object *code_obj, *redirect_obj, *state_obj;
      if (payload && json_object_object_get_ex(payload, "code", &code_obj) &&
          json_object_object_get_ex(payload, "redirect_uri", &redirect_obj)) {
         const char *code = json_object_get_string(code_obj);
         const char *redirect_uri = json_object_get_string(redirect_obj);
         const char *state = NULL;
         if (json_object_object_get_ex(payload, "state", &state_obj)) {
            state = json_object_get_string(state_obj);
         }

         st_error_t err = smartthings_exchange_code(code, redirect_uri, state);
         if (err == ST_OK) {
            json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
            OLOG_INFO("WebUI: SmartThings OAuth authorization successful");
         } else {
            json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
            json_object_object_add(resp_payload, "error",
                                   json_object_new_string(smartthings_error_str(err)));
            OLOG_WARNING("WebUI: SmartThings OAuth failed: %s", smartthings_error_str(err));
         }
      } else {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string("Missing code or redirect_uri"));
      }

      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return true;
   }
   if (strcmp(type, "smartthings_disconnect") == 0) {
      if (!conn_require_admin(conn))
         return true;
      struct json_object *response = json_object_new_object();
      json_object_object_add(response, "type",
                             json_object_new_string("smartthings_disconnect_response"));
      struct json_object *resp_payload = json_object_new_object();

      smartthings_disconnect();
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      OLOG_INFO("WebUI: SmartThings disconnected");

      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return true;
   }
   if (strcmp(type, "smartthings_list_devices") == 0) {
      if (!conn_require_admin(conn))
         return true;
      struct json_object *response = json_object_new_object();
      json_object_object_add(response, "type",
                             json_object_new_string("smartthings_devices_response"));
      struct json_object *resp_payload = json_object_new_object();

      if (!smartthings_is_authenticated()) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error", json_object_new_string("Not authenticated"));
      } else {
         const st_device_list_t *devices;
         st_error_t err = smartthings_list_devices(&devices);
         if (err == ST_OK) {
            json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
            json_object_object_add(resp_payload, "count", json_object_new_int(devices->count));

            struct json_object *devices_arr = json_object_new_array();
            for (int i = 0; i < devices->count; i++) {
               const st_device_t *dev = &devices->devices[i];
               struct json_object *dev_obj = json_object_new_object();
               json_object_object_add(dev_obj, "id", json_object_new_string(dev->id));
               json_object_object_add(dev_obj, "name", json_object_new_string(dev->name));
               json_object_object_add(dev_obj, "label", json_object_new_string(dev->label));
               json_object_object_add(dev_obj, "room", json_object_new_string(dev->room));

               struct json_object *caps = json_object_new_array();
               for (int j = 0; j < 15; j++) {
                  st_capability_t cap = (st_capability_t)(1 << j);
                  if (dev->capabilities & cap) {
                     json_object_array_add(caps,
                                           json_object_new_string(smartthings_capability_str(cap)));
                  }
               }
               json_object_object_add(dev_obj, "capabilities", caps);

               json_object_array_add(devices_arr, dev_obj);
            }
            json_object_object_add(resp_payload, "devices", devices_arr);
         } else {
            json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
            json_object_object_add(resp_payload, "error",
                                   json_object_new_string(smartthings_error_str(err)));
         }
      }

      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return true;
   }
   if (strcmp(type, "smartthings_refresh_devices") == 0) {
      if (!conn_require_admin(conn))
         return true;
      struct json_object *response = json_object_new_object();
      json_object_object_add(response, "type",
                             json_object_new_string("smartthings_devices_response"));
      struct json_object *resp_payload = json_object_new_object();

      if (!smartthings_is_authenticated()) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error", json_object_new_string("Not authenticated"));
      } else {
         const st_device_list_t *devices;
         st_error_t err = smartthings_refresh_devices(&devices);
         if (err == ST_OK) {
            json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
            json_object_object_add(resp_payload, "count", json_object_new_int(devices->count));

            struct json_object *devices_arr = json_object_new_array();
            for (int i = 0; i < devices->count; i++) {
               const st_device_t *dev = &devices->devices[i];
               struct json_object *dev_obj = json_object_new_object();
               json_object_object_add(dev_obj, "id", json_object_new_string(dev->id));
               json_object_object_add(dev_obj, "name", json_object_new_string(dev->name));
               json_object_object_add(dev_obj, "label", json_object_new_string(dev->label));
               json_object_object_add(dev_obj, "room", json_object_new_string(dev->room));

               struct json_object *caps = json_object_new_array();
               for (int j = 0; j < 15; j++) {
                  st_capability_t cap = (st_capability_t)(1 << j);
                  if (dev->capabilities & cap) {
                     json_object_array_add(caps,
                                           json_object_new_string(smartthings_capability_str(cap)));
                  }
               }
               json_object_object_add(dev_obj, "capabilities", caps);

               json_object_array_add(devices_arr, dev_obj);
            }
            json_object_object_add(resp_payload, "devices", devices_arr);
         } else {
            json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
            json_object_object_add(resp_payload, "error",
                                   json_object_new_string(smartthings_error_str(err)));
         }
      }

      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return true;
   }
#endif /* DAWN_ENABLE_SMARTTHINGS_TOOL */

#ifdef DAWN_ENABLE_HOMEASSISTANT_TOOL
   if (strcmp(type, "ha_status") == 0) {
      handle_ha_status(conn);
      return true;
   }
   if (strcmp(type, "ha_test_connection") == 0) {
      handle_ha_test_connection(conn);
      return true;
   }
   if (strcmp(type, "ha_list_entities") == 0) {
      handle_ha_list_entities(conn);
      return true;
   }
   if (strcmp(type, "ha_refresh_entities") == 0) {
      handle_ha_refresh_entities(conn);
      return true;
   }
#endif /* DAWN_ENABLE_HOMEASSISTANT_TOOL */

   (void)payload; /* Suppress unused warning when both tools are disabled */
   return false;
}

/* =============================================================================
 * Always-On Voice Mode Handlers
 * ============================================================================= */

/**
 * Check if another connection for the same user already has always-on active.
 * Must NOT hold s_conn_registry_mutex when calling conn_require_auth.
 */
static bool user_has_always_on(int auth_user_id) {
   pthread_mutex_lock(&s_conn_registry_mutex);
   for (int i = 0; i < MAX_ACTIVE_CONNECTIONS; i++) {
      ws_connection_t *c = s_active_connections[i];
      if (c && c->auth_user_id == auth_user_id && c->always_on) {
         pthread_mutex_unlock(&s_conn_registry_mutex);
         return true;
      }
   }
   pthread_mutex_unlock(&s_conn_registry_mutex);
   return false;
}

static void handle_always_on_enable(ws_connection_t *conn, struct json_object *payload) {
   /* Reject if already enabled on this connection */
   if (conn->always_on) {
      send_always_on_state(conn->wsi, "listening"); /* Already active, re-confirm */
      return;
   }

   /* Reject if push-to-talk audio is in progress */
   if (conn->audio_buffer && conn->audio_buffer_len > 0) {
      send_error_impl(conn->wsi, "PTT_ACTIVE",
                      "Cannot enable always-on while push-to-talk recording is active");
      return;
   }

   /* Enforce per-user limit (max 1 always-on session) */
   if (user_has_always_on(conn->auth_user_id)) {
      send_error_impl(conn->wsi, "ALREADY_ACTIVE",
                      "Always-on is active in another tab for this user");
      return;
   }

   /* Validate sample_rate from payload */
   uint32_t sample_rate = 48000; /* Default if not specified */
   if (payload) {
      struct json_object *sr_obj;
      if (json_object_object_get_ex(payload, "sample_rate", &sr_obj)) {
         sample_rate = (uint32_t)json_object_get_int(sr_obj);
      }
   }

   if (!always_on_valid_sample_rate(sample_rate)) {
      OLOG_WARNING("WebUI: Invalid always-on sample rate %u", sample_rate);
      send_error_impl(conn->wsi, "INVALID_SAMPLE_RATE", "Unsupported sample rate");
      return;
   }

   /* Create always-on context */
   conn->always_on = always_on_create(sample_rate, conn->wsi);
   if (!conn->always_on) {
      send_error_impl(conn->wsi, "INIT_FAILED", "Failed to initialize always-on mode");
      return;
   }

   OLOG_INFO("WebUI: Always-on enabled for user %d (sample_rate=%u)", conn->auth_user_id,
             sample_rate);
   send_always_on_state(conn->wsi, "listening");
}

static void handle_always_on_disable(ws_connection_t *conn) {
   if (!conn->always_on) {
      send_always_on_state(conn->wsi, "disabled");
      return;
   }

   OLOG_INFO("WebUI: Always-on disabled for user %d", conn->auth_user_id);
   always_on_destroy(conn->always_on);
   conn->always_on = NULL;
   send_always_on_state(conn->wsi, "disabled");
}

static void handle_json_message(ws_connection_t *conn, const char *data, size_t len) {
   /* Null-terminate for JSON parsing */
   char *json_str = strndup(data, len);
   if (!json_str) {
      OLOG_ERROR("WebUI: Failed to allocate JSON string");
      return;
   }

   struct json_object *root = json_tokener_parse(json_str);
   if (!root) {
      OLOG_WARNING("WebUI: Invalid JSON received: %.*s", (int)len, data);
      free(json_str);
      return;
   }

   /* Get message type */
   struct json_object *type_obj;
   if (!json_object_object_get_ex(root, "type", &type_obj)) {
      OLOG_WARNING("WebUI: JSON missing 'type' field");
      json_object_put(root);
      free(json_str);
      return;
   }

   const char *type = json_object_get_string(type_obj);
   struct json_object *payload;
   json_object_object_get_ex(root, "payload", &payload);

   if (strcmp(type, "text") == 0) {
      /* Text input from user (with optional vision images - supports multiple) */
      if (payload) {
         struct json_object *text_obj;
         if (json_object_object_get_ex(payload, "text", &text_obj)) {
            const char *text = json_object_get_string(text_obj);
            if (text && strlen(text) > 0) {
               /* Extract optional images for vision (array format) */
               const char *vision_images[WEBUI_MAX_VISION_IMAGES_CAP] = { 0 };
               size_t vision_image_sizes[WEBUI_MAX_VISION_IMAGES_CAP] = { 0 };
               const char *vision_mimes[WEBUI_MAX_VISION_IMAGES_CAP] = { 0 };
               int vision_image_count = 0;
               const int max_vision_images = g_config.vision.max_images;

               struct json_object *images_obj;
               if (json_object_object_get_ex(payload, "images", &images_obj) &&
                   json_object_is_type(images_obj, json_type_array)) {
                  int array_len = json_object_array_length(images_obj);
                  if (array_len > max_vision_images) {
                     OLOG_WARNING("WebUI: Too many images (%d), limiting to %d", array_len,
                                  max_vision_images);
                     array_len = max_vision_images;
                  }

                  for (int i = 0; i < array_len; i++) {
                     struct json_object *image_obj = json_object_array_get_idx(images_obj, i);
                     struct json_object *data_obj, *mime_obj;
                     if (json_object_object_get_ex(image_obj, "data", &data_obj) &&
                         json_object_object_get_ex(image_obj, "mime_type", &mime_obj)) {
                        const char *img_data = json_object_get_string(data_obj);
                        const char *img_mime = json_object_get_string(mime_obj);
                        if (img_data) {
                           size_t img_size = strlen(img_data);

                           /* Validate each image BEFORE passing to handler */
                           int val_result = validate_image_data(img_data, img_size, img_mime);
                           if (val_result != 0) {
                              const char *err_msg = "Image validation failed";
                              switch (val_result) {
                                 case 1:
                                    err_msg = "Unsupported image type";
                                    break;
                                 case 2:
                                    err_msg = "Image too large (max 4MB)";
                                    break;
                                 case 3:
                                    err_msg = "Invalid image data encoding";
                                    break;
                                 case 4:
                                    err_msg = "Image format doesn't match declared type";
                                    break;
                              }
                              send_error_impl(conn->wsi, "INVALID_IMAGE", err_msg);
                              json_object_put(root);
                              free(json_str);
                              return;
                           }

                           vision_images[vision_image_count] = img_data;
                           vision_image_sizes[vision_image_count] = img_size;
                           vision_mimes[vision_image_count] = img_mime;
                           vision_image_count++;
                        }
                     }
                  }

                  if (vision_image_count > 0) {
                     OLOG_INFO("WebUI: %d vision image(s) attached", vision_image_count);
                  }
               }

               handle_text_message(conn, text, strlen(text), vision_images, vision_image_sizes,
                                   vision_mimes, vision_image_count);
            }
         }
      }
   } else if (strcmp(type, "cancel") == 0) {
      /* Cancel current operation */
      handle_cancel_message(conn);
   } else if (strcmp(type, "get_config") == 0) {
      /* Request current configuration */
      handle_get_config(conn);
   } else if (strcmp(type, "get_system_prompt") == 0) {
      /* Request current system prompt for debugging */
      struct json_object *response = json_object_new_object();
      json_object_object_add(response, "type", json_object_new_string("system_prompt_response"));
      struct json_object *resp_payload = json_object_new_object();

      if (conn->session) {
         char *prompt = session_get_system_prompt(conn->session);
         if (prompt) {
            json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
            json_object_object_add(resp_payload, "prompt", json_object_new_string(prompt));
            json_object_object_add(resp_payload, "length",
                                   json_object_new_int((int)strlen(prompt)));
            free(prompt);
         } else {
            json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
            json_object_object_add(resp_payload, "error",
                                   json_object_new_string("No system prompt found"));
         }
      } else {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error", json_object_new_string("No active session"));
      }

      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
   } else if (strcmp(type, "set_config") == 0) {
      /* Update configuration settings */
      if (payload) {
         handle_set_config(conn, payload);
      }
   } else if (strcmp(type, "set_secrets") == 0) {
      /* Update secrets (API keys, credentials) */
      if (payload) {
         handle_set_secrets(conn, payload);
      }
   } else if (strcmp(type, "get_audio_devices") == 0) {
      /* Request available audio devices for given backend */
      handle_get_audio_devices(conn, payload);
   } else if (strcmp(type, "list_models") == 0) {
      /* Request available ASR and TTS models */
      handle_list_models(conn);
   } else if (strcmp(type, "list_interfaces") == 0) {
      /* Request available network interfaces */
      handle_list_interfaces(conn);
   } else if (strcmp(type, "list_llm_models") == 0) {
      /* Request available local LLM models (Ollama/llama.cpp) */
      handle_list_llm_models(conn);
   } else if (strcmp(type, "restart") == 0) {
      /* Admin-only operation */
      if (!conn_require_admin(conn)) {
         return;
      }

      /* Request application restart */
      OLOG_INFO("WebUI: Restart requested by client '%s'", conn->username);

      /* Send confirmation response before initiating restart */
      struct json_object *response = json_object_new_object();
      json_object_object_add(response, "type", json_object_new_string("restart_response"));
      struct json_object *resp_payload = json_object_new_object();
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "message",
                             json_object_new_string("DAWN is restarting..."));
      json_object_object_add(response, "payload", resp_payload);

      send_json_message(conn->wsi, json_object_to_json_string(response));
      json_object_put(response);

      /* Request restart - this will trigger clean shutdown and re-exec */
      dawn_request_restart();
   } else if (strcmp(type, "set_llm_runtime") == 0) {
      /* Admin-only: affects all clients */
      if (!conn_require_admin(conn)) {
         return;
      }
      /* Switch LLM type or provider at runtime (immediate effect, no restart) */
      struct json_object *response = json_object_new_object();
      json_object_object_add(response, "type", json_object_new_string("set_llm_runtime_response"));
      struct json_object *resp_payload = json_object_new_object();

      int success = 1;
      const char *error_msg = NULL;

      if (payload) {
         struct json_object *type_obj, *provider_obj;

         /* Handle LLM type change (local/cloud) */
         if (json_object_object_get_ex(payload, "type", &type_obj)) {
            const char *new_type = json_object_get_string(type_obj);
            if (new_type) {
               if (strcmp(new_type, "local") == 0) {
                  llm_set_type(LLM_LOCAL);
                  OLOG_INFO("WebUI: Switched to local LLM");
               } else if (strcmp(new_type, "cloud") == 0) {
                  /* When switching to cloud, ensure we have a valid provider selected.
                   * Prefer OpenAI if available, otherwise Claude, otherwise Gemini. */
                  if (llm_has_openai_key()) {
                     llm_set_cloud_provider(CLOUD_PROVIDER_OPENAI);
                  } else if (llm_has_claude_key()) {
                     llm_set_cloud_provider(CLOUD_PROVIDER_CLAUDE);
                  } else if (llm_has_gemini_key()) {
                     llm_set_cloud_provider(CLOUD_PROVIDER_GEMINI);
                  }
                  int rc = llm_set_type(LLM_CLOUD);
                  if (rc != 0) {
                     success = 0;
                     error_msg = "No cloud API key configured in secrets.toml";
                  } else {
                     OLOG_INFO("WebUI: Switched to cloud LLM");
                  }
               }
            }
         }

         /* Handle cloud provider change (openai/claude/gemini) */
         if (success && json_object_object_get_ex(payload, "provider", &provider_obj)) {
            const char *new_provider = json_object_get_string(provider_obj);
            if (new_provider) {
               int rc = 0;
               if (strcmp(new_provider, "openai") == 0) {
                  rc = llm_set_cloud_provider(CLOUD_PROVIDER_OPENAI);
               } else if (strcmp(new_provider, "claude") == 0) {
                  rc = llm_set_cloud_provider(CLOUD_PROVIDER_CLAUDE);
               } else if (strcmp(new_provider, "gemini") == 0) {
                  rc = llm_set_cloud_provider(CLOUD_PROVIDER_GEMINI);
               }
               if (rc != 0) {
                  success = 0;
                  error_msg = "API key not configured for this provider";
               } else {
                  OLOG_INFO("WebUI: Switched cloud provider to %s", new_provider);
               }
            }
         }
      }

      json_object_object_add(resp_payload, "success", json_object_new_boolean(success));
      if (error_msg) {
         json_object_object_add(resp_payload, "error", json_object_new_string(error_msg));
      }

      /* Return updated runtime state */
      llm_type_t current_type = llm_get_type();
      json_object_object_add(resp_payload, "type",
                             json_object_new_string(current_type == LLM_LOCAL ? "local" : "cloud"));
      json_object_object_add(resp_payload, "provider",
                             json_object_new_string(llm_get_cloud_provider_name()));
      json_object_object_add(resp_payload, "model", json_object_new_string(llm_get_model_name()));

      /* Include API key availability so client can populate provider dropdown */
      json_object_object_add(resp_payload, "openai_available",
                             json_object_new_boolean(llm_has_openai_key()));
      json_object_object_add(resp_payload, "claude_available",
                             json_object_new_boolean(llm_has_claude_key()));
      json_object_object_add(resp_payload, "gemini_available",
                             json_object_new_boolean(llm_has_gemini_key()));

      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
   } else if (strcmp(type, "set_session_llm") == 0) {
      /* Per-session LLM configuration (does NOT affect other clients) */
      struct json_object *response = json_object_new_object();
      json_object_object_add(response, "type", json_object_new_string("set_session_llm_response"));
      struct json_object *resp_payload = json_object_new_object();

      int success = 1;
      const char *error_msg = NULL;

      if (!conn->session) {
         success = 0;
         error_msg = "No active session";
      } else if (payload) {
         struct json_object *type_obj, *provider_obj;

         /* Get current session config as starting point */
         session_llm_config_t config;
         session_get_llm_config(conn->session, &config);
         bool has_changes = false;

         /* Track old tool_mode for prompt rebuild */
         char old_tool_mode[LLM_TOOL_MODE_MAX];
         strncpy(old_tool_mode, config.tool_mode, sizeof(old_tool_mode) - 1);
         old_tool_mode[sizeof(old_tool_mode) - 1] = '\0';

         /* Track old model + type for context cache invalidation */
         char old_model[LLM_MODEL_NAME_MAX];
         strncpy(old_model, config.model, sizeof(old_model) - 1);
         old_model[sizeof(old_model) - 1] = '\0';
         int old_type = config.type;

         /* Parse type (local/cloud) */
         if (json_object_object_get_ex(payload, "type", &type_obj)) {
            const char *new_type = json_object_get_string(type_obj);
            if (new_type) {
               has_changes = true;
               if (strcmp(new_type, "local") == 0) {
                  config.type = LLM_LOCAL;
               } else if (strcmp(new_type, "cloud") == 0) {
                  config.type = LLM_CLOUD;
                  /* If no provider is set, pick the first one with an API key */
                  if (config.cloud_provider == CLOUD_PROVIDER_NONE) {
                     config.cloud_provider = llm_detect_available_provider();
                     if (config.cloud_provider != CLOUD_PROVIDER_NONE) {
                        OLOG_INFO("WebUI: Auto-selected %s (API key available)",
                                  cloud_provider_to_string(config.cloud_provider));
                     } else {
                        OLOG_WARNING("WebUI: No cloud API keys found — configure in "
                                     "Settings or secrets.toml");
                     }
                  }
               } else if (strcmp(new_type, "reset") == 0) {
                  /* Reset to defaults from dawn.toml */
                  session_clear_llm_config(conn->session);
                  OLOG_INFO("WebUI: Session %u LLM config reset to defaults",
                            conn->session->session_id);
                  has_changes = false; /* Already handled */
               }
            }
         }

         /* Parse provider (openai/claude/gemini) */
         bool provider_explicitly_set = false;
         if (json_object_object_get_ex(payload, "provider", &provider_obj)) {
            const char *new_provider = json_object_get_string(provider_obj);
            if (new_provider && new_provider[0] != '\0') {
               has_changes = true;
               provider_explicitly_set = true;
               if (strcmp(new_provider, "openai") == 0) {
                  config.cloud_provider = CLOUD_PROVIDER_OPENAI;
               } else if (strcmp(new_provider, "claude") == 0) {
                  config.cloud_provider = CLOUD_PROVIDER_CLAUDE;
               } else if (strcmp(new_provider, "gemini") == 0) {
                  config.cloud_provider = CLOUD_PROVIDER_GEMINI;
               }
            }
         }

         /* Parse model */
         struct json_object *model_obj;
         if (json_object_object_get_ex(payload, "model", &model_obj)) {
            const char *new_model = json_object_get_string(model_obj);
            if (new_model && new_model[0] != '\0') {
               /* Validate model name to prevent injection attacks */
               if (llm_local_is_valid_model_name(new_model)) {
                  has_changes = true;
                  strncpy(config.model, new_model, sizeof(config.model) - 1);
                  config.model[sizeof(config.model) - 1] = '\0';
                  OLOG_INFO("WebUI: Session model set to '%s'", config.model);

                  /* Infer provider from model name if not explicitly set
                   * (handles old conversations and frontend bugs).
                   * Only infer if the inferred provider has an API key available. */
                  if (!provider_explicitly_set) {
                     if ((strncmp(new_model, "gpt-", 4) == 0 || strncmp(new_model, "o1-", 3) == 0 ||
                          strncmp(new_model, "o3-", 3) == 0) &&
                         llm_has_openai_key()) {
                        config.cloud_provider = CLOUD_PROVIDER_OPENAI;
                        OLOG_INFO("WebUI: Inferred OpenAI provider from model '%s'", new_model);
                     } else if (strncmp(new_model, "claude-", 7) == 0 && llm_has_claude_key()) {
                        config.cloud_provider = CLOUD_PROVIDER_CLAUDE;
                        OLOG_INFO("WebUI: Inferred Claude provider from model '%s'", new_model);
                     } else if (strncmp(new_model, "gemini-", 7) == 0 && llm_has_gemini_key()) {
                        config.cloud_provider = CLOUD_PROVIDER_GEMINI;
                        OLOG_INFO("WebUI: Inferred Gemini provider from model '%s'", new_model);
                     }
                  }
               } else {
                  OLOG_WARNING("WebUI: Rejected invalid model name from client");
               }
            }
         }

         /* Parse tool_mode (native/command_tags/disabled) */
         struct json_object *tool_mode_obj;
         if (json_object_object_get_ex(payload, "tool_mode", &tool_mode_obj)) {
            const char *new_tool_mode = json_object_get_string(tool_mode_obj);
            if (new_tool_mode) {
               /* Validate tool mode value */
               if (strcmp(new_tool_mode, "native") == 0 ||
                   strcmp(new_tool_mode, "command_tags") == 0 ||
                   strcmp(new_tool_mode, "disabled") == 0) {
                  has_changes = true;
                  strncpy(config.tool_mode, new_tool_mode, sizeof(config.tool_mode) - 1);
                  config.tool_mode[sizeof(config.tool_mode) - 1] = '\0';
                  OLOG_INFO("WebUI: Session tool_mode set to '%s'", config.tool_mode);
               } else {
                  OLOG_WARNING("WebUI: Rejected invalid tool_mode '%s' from client", new_tool_mode);
               }
            }
         }

         /* Parse thinking_mode (disabled/auto/enabled) */
         struct json_object *thinking_mode_obj;
         if (json_object_object_get_ex(payload, "thinking_mode", &thinking_mode_obj)) {
            const char *new_thinking_mode = json_object_get_string(thinking_mode_obj);
            if (new_thinking_mode) {
               /* Validate thinking mode value */
               if (strcmp(new_thinking_mode, "disabled") == 0 ||
                   strcmp(new_thinking_mode, "auto") == 0 ||
                   strcmp(new_thinking_mode, "enabled") == 0) {
                  has_changes = true;
                  strncpy(config.thinking_mode, new_thinking_mode,
                          sizeof(config.thinking_mode) - 1);
                  config.thinking_mode[sizeof(config.thinking_mode) - 1] = '\0';
                  OLOG_INFO("WebUI: Session thinking_mode set to '%s'", config.thinking_mode);
               } else {
                  OLOG_WARNING("WebUI: Rejected invalid thinking_mode '%s' from client",
                               new_thinking_mode);
               }
            }
         }

         /* Parse reasoning_effort. The allowlist mirrors the gpt-5.4 Responses API
          * (none/low/medium/high/xhigh); Claude maps low/medium/high to budget
          * tokens via llm_get_effective_budget_tokens. */
         struct json_object *reasoning_effort_obj;
         if (json_object_object_get_ex(payload, "reasoning_effort", &reasoning_effort_obj)) {
            const char *new_effort = json_object_get_string(reasoning_effort_obj);
            if (new_effort) {
               if (strcmp(new_effort, "none") == 0 || strcmp(new_effort, "low") == 0 ||
                   strcmp(new_effort, "medium") == 0 || strcmp(new_effort, "high") == 0 ||
                   strcmp(new_effort, "xhigh") == 0) {
                  has_changes = true;
                  strncpy(config.reasoning_effort, new_effort, sizeof(config.reasoning_effort) - 1);
                  config.reasoning_effort[sizeof(config.reasoning_effort) - 1] = '\0';
                  OLOG_INFO("WebUI: Session reasoning_effort set to '%s'", config.reasoning_effort);
               } else {
                  OLOG_WARNING("WebUI: Rejected invalid reasoning_effort '%s' from client",
                               new_effort);
               }
            }
         }

         /* Apply config if changes were made */
         if (has_changes) {
            int rc = session_set_llm_config(conn->session, &config);
            if (rc != 0) {
               success = 0;
               error_msg = "API key not configured for requested provider";
            } else {
               OLOG_INFO("WebUI: Session %u LLM config updated (type=%d, provider=%d)",
                         conn->session->session_id, config.type, config.cloud_provider);

               /* If local model or LLM type changed, context size may differ */
               if (config.type == LLM_LOCAL &&
                   (strcmp(old_model, config.model) != 0 || old_type != config.type)) {
                  llm_context_refresh_local();
               }

               /* If tool_mode changed, rebuild and update the session's system prompt */
               if (strcmp(old_tool_mode, config.tool_mode) != 0) {
                  invalidate_system_instructions(); /* Clear cached prompt first */
                  char *new_prompt = build_remote_prompt_for_mode(config.tool_mode);
                  if (new_prompt) {
                     session_update_system_prompt(conn->session, new_prompt);
                     OLOG_INFO("WebUI: Updated session prompt for tool_mode change to '%s'",
                               config.tool_mode);
                     free(new_prompt);
                  }
               }

               /* Persist LLM settings to the active conversation DB so that
                * session recreation (after timeout) restores the latest config.
                *
                * Skip when the client tagged this as a restore-push: those carry
                * the loaded conversation's own settings, but rapid clicks can race
                * such that `active_conversation_id` has already advanced to the
                * NEXT conversation by the time we process this message — the
                * cascade would then silently overwrite that conv with the
                * previous conv's values. */
               bool from_restore = false;
               struct json_object *restore_obj;
               if (json_object_object_get_ex(payload, "from_restore", &restore_obj)) {
                  from_restore = json_object_get_boolean(restore_obj);
               }
               if (!from_restore && conn->active_conversation_id > 0) {
                  const char *type_str = config.type == LLM_LOCAL ? "local" : "cloud";
                  conv_db_update_llm_settings(conn->active_conversation_id, conn->auth_user_id,
                                              type_str,
                                              cloud_provider_to_string(config.cloud_provider),
                                              config.model, config.tool_mode, config.thinking_mode,
                                              config.reasoning_effort);
               }
            }
         }
      }

      json_object_object_add(resp_payload, "success", json_object_new_boolean(success));
      if (error_msg) {
         json_object_object_add(resp_payload, "error", json_object_new_string(error_msg));
      }

      /* Return current session config for confirmation */
      if (conn->session) {
         session_llm_config_t current;
         session_get_llm_config(conn->session, &current);

         const char *type_str = current.type == LLM_LOCAL ? "local" : "cloud";
         json_object_object_add(resp_payload, "type", json_object_new_string(type_str));
         json_object_object_add(resp_payload, "provider",
                                json_object_new_string(
                                    cloud_provider_to_string(current.cloud_provider)));

         /* Get model name - prefer session model, fall back to config */
         const char *model_name = NULL;
         if (current.model[0] != '\0') {
            /* Session has explicit model set */
            model_name = current.model;
         } else {
            /* Fall back to config default based on type/provider */
            const dawn_config_t *cfg = config_get();
            if (current.type == LLM_LOCAL) {
               model_name = cfg->llm.local.model[0] ? cfg->llm.local.model : "";
            } else if (current.cloud_provider == CLOUD_PROVIDER_OPENAI) {
               model_name = llm_get_default_openai_model();
            } else if (current.cloud_provider == CLOUD_PROVIDER_CLAUDE) {
               model_name = llm_get_default_claude_model();
            } else if (current.cloud_provider == CLOUD_PROVIDER_GEMINI) {
               model_name = llm_get_default_gemini_model();
            }
         }
         json_object_object_add(resp_payload, "model",
                                json_object_new_string(model_name ? model_name : ""));
      }

      /* Include API key availability */
      json_object_object_add(resp_payload, "openai_available",
                             json_object_new_boolean(llm_has_openai_key()));
      json_object_object_add(resp_payload, "claude_available",
                             json_object_new_boolean(llm_has_claude_key()));
      json_object_object_add(resp_payload, "gemini_available",
                             json_object_new_boolean(llm_has_gemini_key()));

      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);

      /* Send context info via queue (after response) so gauge reflects new model's
       * limit. Uses queued send to avoid multiple lws_write calls per callback.
       * Actual session token count preserves saved context on conversation load. */
      if (success && conn->session) {
         session_llm_config_t cfg;
         session_get_llm_config(conn->session, &cfg);
         int ctx_max = llm_context_get_size(cfg.type, cfg.cloud_provider, cfg.model);
         if (ctx_max > 0) {
            int ctx_current = 0;
            llm_context_usage_t ctx_usage;
            if (llm_context_get_usage(conn->session->session_id, cfg.type, cfg.cloud_provider,
                                      cfg.model, &ctx_usage) == 0) {
               ctx_current = ctx_usage.current_tokens;
            }
            webui_send_context(conn->session, ctx_current, ctx_max,
                               g_config.llm.compact_hard_threshold);
         }
      }
   } else if (strcmp(type, "reconnect") == 0) {
      /* Session reconnection with stored token */
      if (payload) {
         struct json_object *token_obj;
         if (json_object_object_get_ex(payload, "token", &token_obj)) {
            const char *token = json_object_get_string(token_obj);
            if (token && strlen(token) > 0) {
               session_t *existing = lookup_session_by_token(token);
               if (existing) {
                  /* Found existing session - switch to it */
                  if (conn->session && conn->session != existing) {
                     /* Destroy the abandoned session (just created on connect) */
                     uint32_t abandoned_id = conn->session->session_id;
                     conn->session->client_data = NULL;
                     /* Release ref_count (was 1 from creation) before destroy */
                     session_release(conn->session);
                     session_destroy(abandoned_id);
                     unregister_tokens_for_session(abandoned_id);
                     OLOG_INFO("WebUI: Destroyed abandoned session %u", abandoned_id);
                  }
                  conn->session = existing;
                  existing->client_data = conn;
                  existing->disconnected = false;
                  strncpy(conn->session_token, token, WEBUI_SESSION_TOKEN_LEN - 1);
                  conn->session_token[WEBUI_SESSION_TOKEN_LEN - 1] = '\0';

                  /* Restore connection capabilities from init payload */
                  conn->use_opus = check_opus_capability(payload);
                  struct json_object *tts_obj;
                  if (json_object_object_get_ex(payload, "tts_enabled", &tts_obj)) {
                     conn->tts_enabled = json_object_get_boolean(tts_obj);
                  }

                  OLOG_INFO("WebUI: Reconnected to session %u with token %.4s... "
                            "(opus: %s, tts: %s)",
                            existing->session_id, token, conn->use_opus ? "yes" : "no",
                            conn->tts_enabled ? "yes" : "no");

                  /* Queue init messages (one lws_write per callback) */
                  queue_init_messages(conn, token);

                  /* Re-deliver missed notifications to the reconnected session.
                   * On page refresh, LWS_CALLBACK_RECEIVE's early hook queued
                   * them to the abandoned session (now destroyed), so the
                   * client never saw them. Fire delivery against the real
                   * session and mark the connection as delivered. */
                  conn->missed_notif_delivered = false;
                  if (conn->authenticated && conn->auth_user_id > 0) {
                     deliver_missed_notifications(conn);
                     conn->missed_notif_delivered = true;
                  }
               } else {
                  /* Token not found or session expired - create new session */
                  OLOG_INFO("WebUI: Token %.4s... not found, creating new session", token);
                  if (!conn->session) {
                     conn->session = session_create(SESSION_TYPE_WEBUI, -1);
                     if (conn->session) {
                        /* Set user_id for metrics and memory extraction */
                        session_set_metrics_user(conn->session, conn->auth_user_id);
                        /* Build personalized prompt with user settings + memory context */
                        char *prompt = build_user_prompt(conn->auth_user_id);
                        session_init_system_prompt(conn->session,
                                                   prompt ? prompt : get_remote_command_prompt());
                        free(prompt);
                        conn->session->client_data = conn;
                        if (generate_session_token(conn->session_token) != 0) {
                           OLOG_ERROR("WebUI: Failed to generate session token");
                           session_destroy(conn->session->session_id);
                           conn->session = NULL;
                           return;
                        }
                        register_token(conn->session_token, conn->session->session_id);
                        queue_init_messages(conn, conn->session_token);
                     }
                  }

                  /* Sync capabilities from reconnect payload regardless of whether
                   * session was just created or already existed (e.g. auto-created
                   * from cookie auth before this message was processed). */
                  if (conn->session && payload) {
                     conn->use_opus = check_opus_capability(payload);
                     struct json_object *tts_obj;
                     if (json_object_object_get_ex(payload, "tts_enabled", &tts_obj)) {
                        conn->tts_enabled = json_object_get_boolean(tts_obj);
                     }
                     OLOG_INFO("WebUI: Session %u capabilities synced (opus: %s, tts: %s)",
                               conn->session->session_id, conn->use_opus ? "yes" : "no",
                               conn->tts_enabled ? "yes" : "no");
                  }
               }
            }
         }
      }
   } else if (strcmp(type, "init") == 0) {
      /* Init message arrived on an already-authenticated connection (cookie auth
       * auto-created the session before this message was processed). Sync capabilities. */
      if (payload) {
         conn->use_opus = check_opus_capability(payload);
         struct json_object *tts_obj;
         if (json_object_object_get_ex(payload, "tts_enabled", &tts_obj)) {
            conn->tts_enabled = json_object_get_boolean(tts_obj);
         }
         OLOG_INFO("WebUI: Session %u init capabilities synced (opus: %s, tts: %s)",
                   conn->session ? conn->session->session_id : 0, conn->use_opus ? "yes" : "no",
                   conn->tts_enabled ? "yes" : "no");
      }
   } else if (strcmp(type, "capabilities_update") == 0) {
      /* Client capability update (e.g., Opus codec became available after connect) */
      if (payload && conn->session) {
         conn->use_opus = check_opus_capability(payload);
         OLOG_INFO("WebUI: Session %u capabilities updated (opus: %s)", conn->session->session_id,
                   conn->use_opus ? "yes" : "no");
      } else if (payload) {
         /* Session not yet created - just store capability, session will read it later */
         conn->use_opus = check_opus_capability(payload);
         OLOG_INFO("WebUI: Connection capabilities updated before session (opus: %s)",
                   conn->use_opus ? "yes" : "no");
      }
   } else if (handle_smart_home_message(conn, type, payload)) {
      /* Handled by smart home dispatch (SmartThings / Home Assistant) */
   } else if (strcmp(type, "get_tools_config") == 0) {
      handle_get_tools_config(conn);
   } else if (strcmp(type, "set_tools_config") == 0) {
      if (payload) {
         handle_set_tools_config(conn, payload);
      }
   } else if (strcmp(type, "get_metrics") == 0) {
      handle_get_metrics(conn);
   }
   /* User management (admin only) */
   else if (strcmp(type, "list_users") == 0) {
      handle_list_users(conn);
   } else if (strcmp(type, "create_user") == 0) {
      if (payload) {
         handle_create_user(conn, payload);
      }
   } else if (strcmp(type, "delete_user") == 0) {
      if (payload) {
         handle_delete_user(conn, payload);
      }
   } else if (strcmp(type, "change_password") == 0) {
      if (payload) {
         handle_change_password(conn, payload);
      }
   } else if (strcmp(type, "unlock_user") == 0) {
      if (payload) {
         handle_unlock_user(conn, payload);
      }
   }
   /* Satellite management (admin only) */
   else if (strcmp(type, "list_satellites") == 0) {
      handle_list_satellites(conn);
   } else if (strcmp(type, "update_satellite") == 0) {
      if (payload) {
         handle_update_satellite(conn, payload);
      }
   } else if (strcmp(type, "delete_satellite") == 0) {
      if (payload) {
         handle_delete_satellite(conn, payload);
      }
   }
   /* Personal settings (authenticated users) */
   else if (strcmp(type, "get_my_settings") == 0) {
      handle_get_my_settings(conn);
   } else if (strcmp(type, "set_my_settings") == 0) {
      if (payload) {
         handle_set_my_settings(conn, payload);
      }
   }
   /* Session management (authenticated users) */
   else if (strcmp(type, "list_my_sessions") == 0) {
      handle_list_my_sessions(conn);
   } else if (strcmp(type, "revoke_session") == 0) {
      if (payload) {
         handle_revoke_session(conn, payload);
      }
   }
   /* Conversation history (authenticated users) */
   else if (strcmp(type, "list_conversations") == 0) {
      handle_list_conversations(conn, payload);
   } else if (strcmp(type, "new_conversation") == 0) {
      handle_new_conversation(conn, payload);
   } else if (strcmp(type, "load_conversation") == 0) {
      if (payload) {
         handle_load_conversation(conn, payload);
      }
   } else if (strcmp(type, "delete_conversation") == 0) {
      if (payload) {
         handle_delete_conversation(conn, payload);
      }
   } else if (strcmp(type, "rename_conversation") == 0) {
      if (payload) {
         handle_rename_conversation(conn, payload);
      }
   } else if (strcmp(type, "set_private") == 0) {
      if (payload) {
         handle_set_private(conn, payload);
      }
   } else if (strcmp(type, "reassign_conversation") == 0) {
      if (payload) {
         handle_reassign_conversation(conn, payload);
      }
   } else if (strcmp(type, "export_conversation") == 0) {
      if (payload) {
         handle_export_conversation(conn, payload);
      }
   } else if (strcmp(type, "search_conversations") == 0) {
      if (payload) {
         handle_search_conversations(conn, payload);
      }
   } else if (strcmp(type, "save_message") == 0) {
      if (payload) {
         handle_save_message(conn, payload);
      }
   } else if (strcmp(type, "update_context") == 0) {
      if (payload) {
         handle_update_context(conn, payload);
      }
   } else if (strcmp(type, "lock_conversation_llm") == 0) {
      if (payload) {
         handle_lock_conversation_llm(conn, payload);
      }
   } else if (strcmp(type, "continue_conversation") == 0) {
      if (payload) {
         handle_continue_conversation(conn, payload);
      }
   } else if (strcmp(type, "clear_session") == 0) {
      handle_clear_session(conn);
   }
   /* Memory management (authenticated users) */
   else if (strcmp(type, "get_memory_stats") == 0) {
      handle_get_memory_stats(conn);
   } else if (strcmp(type, "list_memory_facts") == 0) {
      handle_list_memory_facts(conn, payload);
   } else if (strcmp(type, "list_memory_preferences") == 0) {
      handle_list_memory_preferences(conn, payload);
   } else if (strcmp(type, "list_memory_summaries") == 0) {
      handle_list_memory_summaries(conn, payload);
   } else if (strcmp(type, "search_memory") == 0) {
      if (payload) {
         handle_search_memory(conn, payload);
      }
   } else if (strcmp(type, "delete_memory_fact") == 0) {
      if (payload) {
         handle_delete_memory_fact(conn, payload);
      }
   } else if (strcmp(type, "delete_memory_preference") == 0) {
      if (payload) {
         handle_delete_memory_preference(conn, payload);
      }
   } else if (strcmp(type, "delete_memory_summary") == 0) {
      if (payload) {
         handle_delete_memory_summary(conn, payload);
      }
   } else if (strcmp(type, "list_memory_entities") == 0) {
      handle_list_memory_entities(conn, payload);
   } else if (strcmp(type, "delete_memory_entity") == 0) {
      if (payload) {
         handle_delete_memory_entity(conn, payload);
      }
   } else if (strcmp(type, "merge_memory_entities") == 0) {
      if (payload) {
         handle_merge_memory_entities(conn, payload);
      }
   } else if (strcmp(type, "delete_all_memories") == 0) {
      if (payload) {
         handle_delete_all_memories(conn, payload);
      }
   } else if (strcmp(type, "export_memories") == 0) {
      handle_export_memories(conn, payload);
   } else if (strcmp(type, "import_memories") == 0) {
      if (payload) {
         handle_import_memories(conn, payload);
      }
   }
   /* Contacts management */
   else if (strcmp(type, "contacts_list") == 0) {
      handle_contacts_list(conn, payload);
   } else if (strcmp(type, "contacts_search") == 0) {
      if (payload) {
         handle_contacts_search(conn, payload);
      }
   } else if (strcmp(type, "contacts_add") == 0) {
      if (payload) {
         handle_contacts_add(conn, payload);
      }
   } else if (strcmp(type, "contacts_update") == 0) {
      if (payload) {
         handle_contacts_update(conn, payload);
      }
   } else if (strcmp(type, "contacts_delete") == 0) {
      if (payload) {
         handle_contacts_delete(conn, payload);
      }
   } else if (strcmp(type, "contacts_search_entities") == 0) {
      if (payload) {
         handle_contacts_search_entities(conn, payload);
      }
   } else if (strcmp(type, "entity_set_photo") == 0) {
      if (payload) {
         handle_entity_set_photo(conn, payload);
      }
   } else if (strcmp(type, "entity_ensure") == 0) {
      if (payload) {
         handle_entity_ensure(conn, payload);
      }
   }
   /* Document library (RAG) */
   else if (strcmp(type, "doc_library_list") == 0) {
      handle_doc_library_list(conn, payload);
   } else if (strcmp(type, "doc_library_delete") == 0) {
      if (payload) {
         handle_doc_library_delete(conn, payload);
      }
   } else if (strcmp(type, "doc_library_index") == 0) {
      if (payload) {
         handle_doc_library_index(conn, payload);
      }
   } else if (strcmp(type, "doc_library_toggle_global") == 0) {
      if (payload) {
         handle_doc_library_toggle_global(conn, payload);
      }
   }
   /* OAuth flow (shared by calendar and email) */
#if defined(DAWN_ENABLE_CALENDAR_TOOL) || defined(DAWN_ENABLE_EMAIL_TOOL)
   else if (strcmp(type, "oauth_get_auth_url") == 0) {
      if (payload)
         handle_oauth_get_auth_url(conn, payload);
   } else if (strcmp(type, "oauth_exchange_code") == 0) {
      if (payload)
         handle_oauth_exchange_code(conn, payload);
   } else if (strcmp(type, "oauth_disconnect") == 0) {
      if (payload)
         handle_oauth_disconnect(conn, payload);
   } else if (strcmp(type, "oauth_check_scopes") == 0) {
      if (payload)
         handle_oauth_check_scopes(conn, payload);
   }
#endif

   /* Calendar account management (per-user) */
#ifdef DAWN_ENABLE_CALENDAR_TOOL
   else if (strcmp(type, "calendar_list_accounts") == 0) {
      handle_calendar_list_accounts(conn);
   } else if (strcmp(type, "calendar_add_account") == 0) {
      if (payload) {
         handle_calendar_add_account(conn, payload);
      }
   } else if (strcmp(type, "calendar_edit_account") == 0) {
      if (payload) {
         handle_calendar_edit_account(conn, payload);
      }
   } else if (strcmp(type, "calendar_remove_account") == 0) {
      if (payload) {
         handle_calendar_remove_account(conn, payload);
      }
   } else if (strcmp(type, "calendar_test_account") == 0) {
      if (payload) {
         handle_calendar_test_account(conn, payload);
      }
   } else if (strcmp(type, "calendar_sync_account") == 0) {
      if (payload) {
         handle_calendar_sync_account(conn, payload);
      }
   } else if (strcmp(type, "calendar_list_calendars") == 0) {
      if (payload) {
         handle_calendar_list_calendars(conn, payload);
      }
   } else if (strcmp(type, "calendar_toggle_calendar") == 0) {
      if (payload) {
         handle_calendar_toggle_calendar(conn, payload);
      }
   } else if (strcmp(type, "calendar_toggle_read_only") == 0) {
      if (payload) {
         handle_calendar_toggle_read_only(conn, payload);
      }
   } else if (strcmp(type, "calendar_set_enabled") == 0) {
      if (payload) {
         handle_calendar_set_enabled(conn, payload);
      }
   }
#endif /* DAWN_ENABLE_CALENDAR_TOOL */
#ifdef DAWN_ENABLE_EMAIL_TOOL
   else if (strcmp(type, "email_list_accounts") == 0) {
      handle_email_list_accounts(conn);
   } else if (strcmp(type, "email_add_account") == 0) {
      if (payload) {
         handle_email_add_account(conn, payload);
      }
   } else if (strcmp(type, "email_update_account") == 0) {
      if (payload) {
         handle_email_update_account(conn, payload);
      }
   } else if (strcmp(type, "email_remove_account") == 0) {
      if (payload) {
         handle_email_remove_account(conn, payload);
      }
   } else if (strcmp(type, "email_test_connection") == 0) {
      if (payload) {
         handle_email_test_connection(conn, payload);
      }
   } else if (strcmp(type, "email_set_read_only") == 0) {
      if (payload) {
         handle_email_set_read_only(conn, payload);
      }
   } else if (strcmp(type, "email_set_enabled") == 0) {
      if (payload) {
         handle_email_set_enabled(conn, payload);
      }
   }
#endif /* DAWN_ENABLE_EMAIL_TOOL */
   /* TTS control (per-connection) */
   else if (strcmp(type, "set_tts_enabled") == 0) {
      if (!conn_require_auth(conn)) {
         return;
      }
      if (payload) {
         struct json_object *enabled_obj;
         if (json_object_object_get_ex(payload, "enabled", &enabled_obj)) {
            conn->tts_enabled = json_object_get_boolean(enabled_obj);
            OLOG_INFO("WebUI: TTS %s for session %u", conn->tts_enabled ? "enabled" : "disabled",
                      conn->session ? conn->session->session_id : 0);
         }
      }
   }
   /* Music streaming — accessible to authenticated users AND registered satellites.
    * Check satellite first: conn_require_auth() has a side-effect of sending
    * an UNAUTHORIZED error, so we must short-circuit before it fires. */
   else if (strcmp(type, "music_subscribe") == 0) {
      if (!conn_is_satellite_session(conn) && !conn_require_auth(conn)) {
         return;
      }
      handle_music_subscribe(conn, payload);
   } else if (strcmp(type, "music_unsubscribe") == 0) {
      if (!conn_is_satellite_session(conn) && !conn_require_auth(conn)) {
         return;
      }
      handle_music_unsubscribe(conn);
   } else if (strcmp(type, "music_control") == 0) {
      if (!conn_is_satellite_session(conn) && !conn_require_auth(conn)) {
         return;
      }
      if (payload) {
         handle_music_control(conn, payload);
      }
   } else if (strcmp(type, "music_search") == 0) {
      if (!conn_is_satellite_session(conn) && !conn_require_auth(conn)) {
         return;
      }
      handle_music_search(conn, payload);
   } else if (strcmp(type, "music_library") == 0) {
      if (!conn_is_satellite_session(conn) && !conn_require_auth(conn)) {
         return;
      }
      handle_music_library(conn, payload);
   } else if (strcmp(type, "music_queue") == 0) {
      if (!conn_is_satellite_session(conn) && !conn_require_auth(conn)) {
         return;
      }
      if (payload) {
         handle_music_queue(conn, payload);
      }
   }
   /* Scheduler dismiss/snooze from WebUI or satellite client */
   else if (strcmp(type, "scheduler_action") == 0) {
      if (!conn_is_satellite_session(conn) && !conn_require_auth(conn))
         return;
      if (!payload)
         return;

      json_object *action_obj, *event_id_obj, *snooze_obj;
      json_object_object_get_ex(payload, "action", &action_obj);
      json_object_object_get_ex(payload, "event_id", &event_id_obj);
      const char *action = action_obj ? json_object_get_string(action_obj) : NULL;
      int64_t event_id = event_id_obj ? json_object_get_int64(event_id_obj) : 0;

      if (!action || !action[0]) {
         send_error_impl(conn->wsi, "INVALID_PARAM", "Missing action");
      } else if (strcmp(action, "dismiss_missed") == 0) {
         /* Delete a queued missed notification. Uses missed_notif_id rather than
          * event_id, so this branch runs before the event_id validation.
          * The DB delete enforces user ownership via AND user_id = ?. */
         json_object *mid_obj = NULL;
         json_object_object_get_ex(payload, "missed_notif_id", &mid_obj);
         int64_t mid = mid_obj ? json_object_get_int64(mid_obj) : 0;
         if (mid <= 0) {
            send_error_impl(conn->wsi, "INVALID_PARAM", "Missing missed_notif_id");
         } else if (conn->auth_user_id <= 0) {
            send_error_impl(conn->wsi, "FORBIDDEN", "Not authenticated");
         } else {
            missed_notif_delete_by_user(mid, conn->auth_user_id);
            /* If the underlying event is still ringing (e.g. alarm looping on
             * the local speaker while user was offline), dismiss it too.
             * event_id comes from the client but ownership is verified before
             * dismissing so it's not an injection vector. */
            if (event_id > 0) {
               sched_event_t ev;
               if (scheduler_db_get(event_id, &ev) == 0 && ev.user_id == conn->auth_user_id &&
                   ev.status == SCHED_STATUS_RINGING) {
                  scheduler_dismiss(event_id);
               }
            }
         }
      } else if (event_id <= 0) {
         send_error_impl(conn->wsi, "INVALID_PARAM", "Missing or invalid event_id");
      } else {
         sched_event_t ev;
         int get_rc = scheduler_db_get(event_id, &ev);
         if (get_rc != 0) {
            send_error_impl(conn->wsi, "NOT_FOUND", "Event not found");
         } else if (!conn->is_satellite && ev.user_id != conn->auth_user_id) {
            /* Satellites can dismiss any announced event; WebUI users can only
             * dismiss their own events. */
            send_error_impl(conn->wsi, "FORBIDDEN", "Not your event");
         } else if (strcmp(action, "dismiss") == 0) {
            int rc = scheduler_dismiss(event_id);
            if (rc != 0) {
               /* Already dismissed (e.g. auto-dismiss for timers) — rebroadcast
                * so other clients (satellites) can sync their UI. */
               scheduler_broadcast_notification(&ev, "Dismissed");
            }
         } else if (strcmp(action, "snooze") == 0) {
            json_object_object_get_ex(payload, "snooze_minutes", &snooze_obj);
            int snooze_min = snooze_obj ? json_object_get_int(snooze_obj) : 0;
            int rc = scheduler_snooze(event_id, snooze_min);
            if (rc != 0)
               send_error_impl(conn->wsi, "NOT_FOUND", "No ringing event to snooze");
         } else {
            send_error_impl(conn->wsi, "INVALID_PARAM", "Unknown scheduler action");
         }
      }
   }
   /* Always-on voice mode */
   else if (strcmp(type, "always_on_enable") == 0) {
      if (!conn_require_auth(conn)) {
         return;
      }
      handle_always_on_enable(conn, payload);
   } else if (strcmp(type, "always_on_disable") == 0) {
      if (!conn_require_auth(conn)) {
         return;
      }
      handle_always_on_disable(conn);
   } else if (strcmp(type, "always_on_state_request") == 0) {
      if (!conn_require_auth(conn)) {
         return;
      }
      /* Client re-sync (e.g., tab returned from background) */
      const char *state_name = conn->always_on
                                   ? always_on_state_name(always_on_get_state(conn->always_on))
                                   : "disabled";
      send_always_on_state(conn->wsi, state_name);
   }
   /* Satellite (DAP2 Tier 1) messages — only accept from existing satellites.
    * Initial registration is handled in the init block above (line ~2924).
    * Re-registration is rejected to prevent identity spoofing. */
   else if (strcmp(type, "satellite_register") == 0) {
      if (conn->is_satellite && conn->session) {
         send_error_impl(conn->wsi, "ALREADY_REGISTERED",
                         "Satellite already registered on this connection");
      } else if (conn->is_satellite && payload) {
         handle_satellite_register(conn, payload);
      }
   } else if (strcmp(type, "satellite_query") == 0) {
      if (conn->is_satellite && payload) {
         handle_satellite_query(conn, payload);
      }
   } else if (strcmp(type, "satellite_ping") == 0) {
      if (conn->is_satellite) {
         handle_satellite_ping(conn);
      }
   } else if (strcmp(type, "volume_state") == 0) {
      if (conn->is_satellite && payload) {
         handle_satellite_volume_state(conn, payload);
      }
   } else if (strncmp(type, "smartthings_", sizeof("smartthings_") - 1) == 0 ||
              strncmp(type, "ha_", sizeof("ha_") - 1) == 0) {
      OLOG_DEBUG("WebUI: Ignoring %s message (feature not compiled in)", type);
   } else {
      OLOG_WARNING("WebUI: Unknown message type: %s", type);
   }

   json_object_put(root);
   free(json_str);
}

static void handle_cancel_message(ws_connection_t *conn) {
   if (conn->session) {
      OLOG_INFO("WebUI: Cancel requested for session %u", conn->session->session_id);
      conn->session->disconnected = true; /* Signal worker to abort */
      /* Note: llm_request_interrupt() is NOT called here because it uses a global flag
       * that would affect all users. Instead, session_manager.c sets the TLS cancel flag
       * to &session->disconnected before each LLM call, so the CURL progress callback
       * will check THIS session's disconnected flag only. */
      send_state_impl(conn->wsi, "idle", NULL);
   }
}

/* =============================================================================
 * WebSocket Protocol Callbacks
 * ============================================================================= */

/* Forward declaration — defined after handle_text_message */
static bool webui_conn_create_session(ws_connection_t *conn);

static int callback_websocket(struct lws *wsi,
                              enum lws_callback_reasons reason,
                              void *user,
                              void *in,
                              size_t len) {
   ws_connection_t *conn = (ws_connection_t *)user;

   switch (reason) {
      case LWS_CALLBACK_ESTABLISHED: {
         /*
          * New WebSocket connection - defer session creation until init message.
          * This allows reconnecting clients (with valid token) to reuse their
          * existing session without counting against max_clients during the
          * brief overlap when refreshing a browser page.
          */
         memset(conn, 0, sizeof(*conn));
         conn->wsi = wsi;
         conn->session = NULL; /* Will be created/assigned on init message */
         conn->volume = 0.8f;  /* Default music volume (0.0-1.0) */

         /* Capture client IP at connection time for reliable logging later */
         lws_get_peer_simple(wsi, conn->client_ip, sizeof(conn->client_ip));
         if (conn->client_ip[0] == '\0') {
            strncpy(conn->client_ip, "(unknown)", sizeof(conn->client_ip) - 1);
         }

         /* Populate auth state from HTTP cookie (if present) */
         auth_session_t auth_session;
         if (is_request_authenticated(wsi, &auth_session)) {
            conn->authenticated = true;
            conn->auth_user_id = auth_session.user_id;
            strncpy(conn->auth_session_token, auth_session.token,
                    sizeof(conn->auth_session_token) - 1);
            conn->auth_session_token[sizeof(conn->auth_session_token) - 1] = '\0';
            strncpy(conn->username, auth_session.username, sizeof(conn->username) - 1);
            conn->username[sizeof(conn->username) - 1] = '\0';
            OLOG_INFO("WebUI: WebSocket authenticated as user '%s' (id=%d)", conn->username,
                      conn->auth_user_id);
         } else {
            OLOG_INFO("WebUI: WebSocket connection established (unauthenticated)");
         }

         /* Register in connection registry for proactive notifications */
         register_connection(conn);

         OLOG_INFO("WebUI: WebSocket connection established, awaiting init message");
         break;
      }

      case LWS_CALLBACK_CLOSED: {
         /* Unregister from connection registry */
         unregister_connection(conn);
         /* WebSocket disconnected.
          * Atomic load: maintenance thread may have NULLed conn->session. */
         session_t *closing_session = conn_get_session(conn);
         OLOG_INFO("WebUI: WebSocket client disconnecting (session %u)",
                   closing_session ? closing_session->session_id : 0);

         if (closing_session) {
            /* Mark session as disconnected (aborts any pending LLM calls) */
            closing_session->disconnected = true;
            closing_session->client_data = NULL;

            /* Release our reference to the session.
             * Session manager will clean it up when ref_count reaches 0. */
            OLOG_INFO("WebUI: Releasing session reference...");
            session_release(closing_session);
            OLOG_INFO("WebUI: Session reference released");
            conn->session = NULL;
         }

         /* Clear wsi reference */
         conn->wsi = NULL;

         /* Free any pending audio buffer */
         if (conn->audio_buffer) {
            free(conn->audio_buffer);
            conn->audio_buffer = NULL;
         }

         /* Free any pending text fragment buffer */
         if (conn->text_buffer) {
            free(conn->text_buffer);
            conn->text_buffer = NULL;
            conn->text_buffer_len = 0;
            conn->text_buffer_cap = 0;
         }

         /* Clean up always-on state */
         if (conn->always_on) {
            always_on_destroy(conn->always_on);
            conn->always_on = NULL;
         }

         /* Clean up music streaming state */
         webui_music_session_cleanup(conn);

         /* Decrement client count if this connection was counted.
          * Uses conn->counted instead of checking session != NULL because
          * session may have been detached (set to NULL) by webui_detach_session
          * during session expiry while the WS connection stayed alive. */
         if (conn->counted) {
            pthread_mutex_lock(&s_mutex);
            if (s_client_count > 0) {
               s_client_count--;
            }
            pthread_mutex_unlock(&s_mutex);
            conn->counted = false;
         }

         OLOG_INFO("WebUI: WebSocket client disconnected (total: %d)", s_client_count);
         break;
      }

      case LWS_CALLBACK_RECEIVE: {
         /* Message received from client.
          * Atomic load: maintenance thread may have NULLed conn->session
          * via webui_detach_session() on session expiry. */
         if (!conn_get_session(conn)) {
            /* Session is NULL — either this is the first message (init/reconnect)
             * or the conversation session expired while the WS connection stayed alive.
             * If already authenticated, auto-create a new session so the user
             * doesn't have to log in again. */
            if (conn->authenticated) {
               OLOG_INFO("WebUI: Session expired for authenticated connection, auto-creating");
               if (!webui_conn_create_session(conn)) {
                  break; /* Error already sent to client */
               }
               /* Fall through to normal message handling below */
            } else {
               /*
                * Not yet authenticated - this must be the init/reconnect message.
                * Parse it to determine if we're reconnecting with a token or
                * need a new session. Check client limits here.
                */
               char *json_str = strndup((const char *)in, len);
               if (!json_str) {
                  OLOG_ERROR("WebUI: Failed to allocate init message buffer");
                  return LWS_CLOSE_CONNECTION;
               }

               struct json_object *root = json_tokener_parse(json_str);
               free(json_str);

               if (!root) {
                  OLOG_WARNING("WebUI: Invalid JSON in init message");
                  return LWS_CLOSE_CONNECTION;
               }

               struct json_object *type_obj;
               const char *type = NULL;
               if (json_object_object_get_ex(root, "type", &type_obj)) {
                  type = json_object_get_string(type_obj);
               }

               struct json_object *payload = NULL;
               json_object_object_get_ex(root, "payload", &payload);

               OLOG_INFO("WebUI: Init message received, type=%s", type ? type : "(null)");

               bool is_reconnect = false;
               session_t *existing_session = NULL;

               /* Check for reconnection token */
               if (type && strcmp(type, "reconnect") == 0 && payload) {
                  struct json_object *token_obj;
                  if (json_object_object_get_ex(payload, "token", &token_obj)) {
                     const char *token = json_object_get_string(token_obj);
                     if (token && strlen(token) > 0) {
                        existing_session = lookup_session_by_token(token);
                        if (existing_session) {
                           is_reconnect = true;
                           conn->session = existing_session;
                           existing_session->client_data = conn;
                           existing_session->disconnected = false;
                           strncpy(conn->session_token, token, WEBUI_SESSION_TOKEN_LEN - 1);
                           conn->session_token[WEBUI_SESSION_TOKEN_LEN - 1] = '\0';

                           /* Check for Opus codec support */
                           conn->use_opus = check_opus_capability(payload);

                           /* Check for TTS preference (default off) */
                           conn->tts_enabled = false;
                           struct json_object *tts_obj;
                           if (json_object_object_get_ex(payload, "tts_enabled", &tts_obj)) {
                              conn->tts_enabled = json_object_get_boolean(tts_obj);
                           }

                           /* Reconnections still count against client limit */
                           pthread_mutex_lock(&s_mutex);
                           s_client_count++;
                           pthread_mutex_unlock(&s_mutex);
                           conn->counted = true;

                           OLOG_INFO(
                               "WebUI: Reconnected to session %u with token %.4s... (total: %d, "
                               "opus: %s, tts: %s)",
                               existing_session->session_id, token, s_client_count,
                               conn->use_opus ? "yes" : "no", conn->tts_enabled ? "yes" : "no");

                           /* Queue init messages (one lws_write per callback) */
                           queue_init_messages(conn, token);
                        }
                     }
                  }
               }

               /* Check for satellite registration (DAP2 Tier 1) */
               if (type && strcmp(type, "satellite_register") == 0 && payload) {
                  pthread_mutex_lock(&s_mutex);
                  if (s_client_count >= g_config.webui.max_clients) {
                     pthread_mutex_unlock(&s_mutex);
                     OLOG_WARNING("WebUI: Satellite rejected - max clients reached (%d)",
                                  g_config.webui.max_clients);
                     send_error_impl(wsi, "MAX_CLIENTS",
                                     "Maximum clients reached. Please try again later.");
                     json_object_put(root);
                     return LWS_CLOSE_CONNECTION;
                  }
                  s_client_count++;
                  pthread_mutex_unlock(&s_mutex);
                  conn->counted = true;

                  /* Delegate to satellite handler for full registration
                   * (session_create_dap2 handles both new sessions and reconnection) */
                  conn->is_satellite = true;
                  handle_satellite_register(conn, payload);

                  /* If registration failed, session won't be set */
                  if (conn->session) {
                     /* Mark authenticated so broadcasts (scheduler notifications, etc.)
                      * include this satellite. Browser clients get this from cookie auth
                      * at LWS_CALLBACK_ESTABLISHED; satellites authenticate via
                      * registration key in handle_satellite_register instead. */
                     conn->authenticated = true;
                  }
                  if (!conn->session) {
                     OLOG_ERROR("WebUI: Failed to create satellite session");
                     pthread_mutex_lock(&s_mutex);
                     s_client_count--;
                     pthread_mutex_unlock(&s_mutex);
                     conn->counted = false;
                     json_object_put(root);
                     return LWS_CLOSE_CONNECTION;
                  }
                  json_object_put(root);
                  break;
               }

               /* If not reconnecting, create new session (with client limit check) */
               if (!is_reconnect) {
                  pthread_mutex_lock(&s_mutex);
                  if (s_client_count >= g_config.webui.max_clients) {
                     pthread_mutex_unlock(&s_mutex);
                     OLOG_WARNING("WebUI: Connection rejected - max clients reached (%d)",
                                  g_config.webui.max_clients);
                     send_error_impl(wsi, "MAX_CLIENTS",
                                     "Maximum WebUI clients reached. Please try again later.");
                     json_object_put(root);
                     return LWS_CLOSE_CONNECTION;
                  }
                  s_client_count++;
                  pthread_mutex_unlock(&s_mutex);
                  conn->counted = true;

                  conn->session = session_create(SESSION_TYPE_WEBUI, -1);
                  if (!conn->session) {
                     OLOG_ERROR("WebUI: Failed to create session");
                     send_error_impl(wsi, "SESSION_LIMIT", "Maximum sessions reached");
                     pthread_mutex_lock(&s_mutex);
                     s_client_count--;
                     pthread_mutex_unlock(&s_mutex);
                     conn->counted = false;
                     json_object_put(root);
                     return LWS_CLOSE_CONNECTION;
                  }

                  /* Set user_id for metrics and memory extraction */
                  session_set_metrics_user(conn->session, conn->auth_user_id);
                  /* Build personalized prompt with user settings + memory context */
                  char *prompt = build_user_prompt(conn->auth_user_id);
                  session_init_system_prompt(conn->session,
                                             prompt ? prompt : get_remote_command_prompt());
                  free(prompt);
                  conn->session->client_data = conn;

                  /* Check for Opus codec support */
                  conn->use_opus = check_opus_capability(payload);

                  /* Check for TTS preference (default off) */
                  conn->tts_enabled = false;
                  struct json_object *tts_obj;
                  if (json_object_object_get_ex(payload, "tts_enabled", &tts_obj)) {
                     conn->tts_enabled = json_object_get_boolean(tts_obj);
                  }

                  if (generate_session_token(conn->session_token) != 0) {
                     OLOG_ERROR("WebUI: Failed to generate session token");
                     session_destroy(conn->session->session_id);
                     conn->session = NULL;
                     pthread_mutex_lock(&s_mutex);
                     s_client_count--;
                     pthread_mutex_unlock(&s_mutex);
                     conn->counted = false;
                     json_object_put(root);
                     return LWS_CLOSE_CONNECTION;
                  }
                  register_token(conn->session_token, conn->session->session_id);

                  OLOG_INFO("WebUI: New session %u created (token %.4s..., total: %d, opus: %s, "
                            "tts: %s)",
                            conn->session->session_id, conn->session_token, s_client_count,
                            conn->use_opus ? "yes" : "no", conn->tts_enabled ? "yes" : "no");

                  queue_init_messages(conn, conn->session_token);
               }

               json_object_put(root);
               break; /* Don't process this message further - it was just the init */
            }         /* end else (not authenticated — init message) */
         }

         session_touch(conn->session);

         /* Deliver any queued missed notifications once the session is ready and
          * the user is authenticated. One-shot per connection. */
         if (!conn->missed_notif_delivered && conn->authenticated && conn->auth_user_id > 0) {
            deliver_missed_notifications(conn);
            conn->missed_notif_delivered = true;
         }

         int is_final = lws_is_final_fragment(wsi);
         int is_binary = lws_frame_is_binary(wsi);

         if (is_binary) {
#ifdef ENABLE_WEBUI_AUDIO
            /* Handle WebSocket frame fragmentation for binary messages */
            if (conn->in_binary_fragment) {
               /* Continuation of a fragmented message - append ALL bytes as payload */
               const uint8_t *data = (const uint8_t *)in;
               size_t data_len = len;

               if (conn->binary_msg_type == WS_BIN_AUDIO_IN && data_len > 0) {
                  /* Append continuation data to audio buffer — expand if needed
                   * (mirrors the realloc logic in handle_binary_message) */
                  if (conn->audio_buffer &&
                      conn->audio_buffer_len + data_len > conn->audio_buffer_capacity) {
                     size_t new_capacity = conn->audio_buffer_capacity * 2;
                     while (new_capacity < conn->audio_buffer_len + data_len)
                        new_capacity *= 2;
                     if (new_capacity > WEBUI_AUDIO_MAX_CAPACITY) {
                        OLOG_WARNING("WebUI: Fragment would exceed max audio capacity (%d bytes)",
                                     WEBUI_AUDIO_MAX_CAPACITY);
                        send_error_impl(conn->wsi, "BUFFER_FULL", "Recording too long");
                        conn->in_binary_fragment = false;
                     } else {
                        uint8_t *new_buffer = realloc(conn->audio_buffer, new_capacity);
                        if (!new_buffer) {
                           OLOG_ERROR("WebUI: Failed to expand audio buffer in fragment");
                           send_error_impl(conn->wsi, "BUFFER_ERROR",
                                           "Audio buffer allocation failed");
                           conn->in_binary_fragment = false;
                        } else {
                           conn->audio_buffer = new_buffer;
                           conn->audio_buffer_capacity = new_capacity;
                        }
                     }
                  }
                  if (conn->audio_buffer && conn->in_binary_fragment &&
                      conn->audio_buffer_len + data_len <= conn->audio_buffer_capacity) {
                     memcpy(conn->audio_buffer + conn->audio_buffer_len, data, data_len);
                     conn->audio_buffer_len += data_len;
                     OLOG_INFO("WebUI: Fragment continuation, added %zu bytes (total: %zu)",
                               data_len, conn->audio_buffer_len);
                  }
               }

               /* Check if this is the final fragment */
               if (is_final) {
                  conn->in_binary_fragment = false;

                  /* Always-on: process the fully reassembled audio frame.
                   * conn->audio_buffer has accumulated all fragment payloads.
                   * Prepend the type byte so handle_binary_message can parse it. */
                  if (conn->always_on && conn->binary_msg_type == WS_BIN_AUDIO_IN &&
                      always_on_get_state(conn->always_on) != ALWAYS_ON_DISABLED) {
                     if (conn->audio_buffer && conn->audio_buffer_len > 0) {
                        always_on_process_audio(conn->always_on, conn->audio_buffer,
                                                conn->audio_buffer_len, conn->use_opus, conn);
                     }
                     /* Clear the accumulation buffer */
                     conn->audio_buffer_len = 0;
                  }
               }
            } else {
               /* New message - parse type byte and handle */
               handle_binary_message(conn, (const uint8_t *)in, len);

               /* If not final, track that we're in a fragmented message */
               if (!is_final && len > 0) {
                  conn->in_binary_fragment = true;
                  conn->binary_msg_type = ((const uint8_t *)in)[0];
               }

               /* Always-on: for non-fragmented complete messages, route audio
                * from the accumulation buffer (handle_binary_message appended it). */
               if (is_final && conn->always_on && len > 0 &&
                   ((const uint8_t *)in)[0] == WS_BIN_AUDIO_IN &&
                   always_on_get_state(conn->always_on) != ALWAYS_ON_DISABLED) {
                  if (conn->audio_buffer && conn->audio_buffer_len > 0) {
                     always_on_process_audio(conn->always_on, conn->audio_buffer,
                                             conn->audio_buffer_len, conn->use_opus, conn);
                     conn->audio_buffer_len = 0;
                  }
               }
            }
#else
            OLOG_WARNING("WebUI: Audio not enabled, ignoring binary message (%zu bytes)", len);
#endif
         } else {
            /* Text message (JSON control) - handle fragmentation */
            if (is_final && conn->text_buffer_len == 0) {
               /* Unfragmented message (common case) - process directly */
               handle_json_message(conn, (const char *)in, len);
            } else {
               /* Check for integer overflow before calculating needed size */
               if (len > SIZE_MAX - conn->text_buffer_len - 1) {
                  OLOG_ERROR("WebUI: Text buffer size overflow detected");
                  conn->text_buffer_len = 0;
                  break;
               }

               /* Fragmented message - accumulate in buffer */
               size_t needed = conn->text_buffer_len + len + 1; /* +1 for null terminator */
               if (needed > conn->text_buffer_cap) {
                  /* Grow buffer (exponential growth, capped for safety) */
                  size_t new_cap = conn->text_buffer_cap ? conn->text_buffer_cap * 2
                                                         : WEBUI_TEXT_BUFFER_INITIAL_CAP;
                  while (new_cap < needed) {
                     new_cap *= 2;
                  }
                  if (new_cap > WEBUI_TEXT_BUFFER_MAX_CAP) {
                     OLOG_ERROR("WebUI: Text message too large (>%d bytes), dropping",
                                WEBUI_TEXT_BUFFER_MAX_CAP);
                     conn->text_buffer_len = 0; /* Reset for next message */
                     break;
                  }
                  char *new_buf = realloc(conn->text_buffer, new_cap);
                  if (!new_buf) {
                     OLOG_ERROR("WebUI: Failed to allocate text buffer (%zu bytes)", new_cap);
                     conn->text_buffer_len = 0;
                     break;
                  }
                  conn->text_buffer = new_buf;
                  conn->text_buffer_cap = new_cap;
               }

               /* Append data */
               memcpy(conn->text_buffer + conn->text_buffer_len, in, len);
               conn->text_buffer_len += len;
               conn->text_buffer[conn->text_buffer_len] = '\0';

               if (is_final) {
                  /* Complete message received - process it */
                  handle_json_message(conn, conn->text_buffer, conn->text_buffer_len);
                  conn->text_buffer_len = 0; /* Reset for next message (keep buffer) */
               }
            }
         }
         break;
      }

      case LWS_CALLBACK_SERVER_WRITEABLE:
         /* Ready to send more data - process next queued response */
         process_one_response();
         break;

      case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
         /* lws_cancel_service() was called - process response queue */
         process_response_queue();
         break;

      default:
         break;
   }

   return 0;
}

/* =============================================================================
 * Protocol Definitions
 * ============================================================================= */

static struct lws_protocols s_protocols[] = {
   /* HTTP protocol (must be first) */
   {
       .name = "http",
       .callback = callback_http,
       .per_session_data_size = sizeof(struct http_session_data),
       .rx_buffer_size = 0,
   },
   /* WebSocket protocol */
   {
       .name = WEBUI_SUBPROTOCOL,
       .callback = callback_websocket,
       .per_session_data_size = sizeof(ws_connection_t),
       .rx_buffer_size = 8192, /* Match DAP packet size */
   },
   /* Terminator */
   { NULL, NULL, 0, 0 },
};

/* =============================================================================
 * Server Thread
 * ============================================================================= */

static void *webui_thread_func(void *arg) {
   (void)arg;

   OLOG_INFO("WebUI: Server thread started");

   while (s_running) {
      /* Process events with 5ms timeout (fast for music streaming ~50fps).
       * lws_cancel_service() interrupts the wait when responses are queued. */
      lws_service(s_lws_context, 5);

      /* Process one pending response per iteration.
       * The writeable callback chain handles additional responses. */
      process_response_queue();

      /* Always-on timeout checks (~1Hz, not every 5ms iteration) */
      {
         static int64_t last_timeout_check_ms = 0;
         struct timespec ts;
         clock_gettime(CLOCK_MONOTONIC, &ts);
         int64_t now_ms = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
         if (now_ms - last_timeout_check_ms >= 1000) {
            last_timeout_check_ms = now_ms;
            pthread_mutex_lock(&s_conn_registry_mutex);
            for (int i = 0; i < MAX_ACTIVE_CONNECTIONS; i++) {
               ws_connection_t *c = s_active_connections[i];
               if (c && c->always_on) {
                  if (always_on_check_timeouts(c->always_on, c)) {
                     /* Auto-disabled — clean up */
                     send_always_on_state(c->wsi, "disabled");
                     always_on_destroy(c->always_on);
                     c->always_on = NULL;
                  }
               }
            }
            pthread_mutex_unlock(&s_conn_registry_mutex);
         }
      }
   }

   OLOG_INFO("WebUI: Server thread exiting");
   return NULL;
}

/* =============================================================================
 * Tool Execution Callback (for debug display)
 * ============================================================================= */

/**
 * @brief Callback for native tool execution notifications
 *
 * Sends tool call/result information to the WebUI for debug display.
 * Called by llm_tools module when tools are executed.
 */
/**
 * @brief Send LLM state update to WebSocket client
 *
 * Pushes the current LLM configuration to the client so it can update
 * the UI controls dynamically (e.g., after switch_llm tool call).
 */
static void webui_send_llm_state_update(session_t *session) {
   if (!session || session->type != SESSION_TYPE_WEBUI) {
      return;
   }

   /* Get session's current LLM config */
   session_llm_config_t config;
   session_get_llm_config(session, &config);

   /* Build JSON response */
   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("llm_state_update"));

   json_object *payload = json_object_new_object();
   json_object_object_add(payload, "success", json_object_new_boolean(1));

   const char *type_str = config.type == LLM_LOCAL ? "local" : "cloud";

   json_object_object_add(payload, "type", json_object_new_string(type_str));
   json_object_object_add(payload, "provider",
                          json_object_new_string(cloud_provider_to_string(config.cloud_provider)));
   json_object_object_add(payload, "model", json_object_new_string(config.model));
   json_object_object_add(payload, "openai_available",
                          json_object_new_boolean(llm_has_openai_key()));
   json_object_object_add(payload, "claude_available",
                          json_object_new_boolean(llm_has_claude_key()));
   json_object_object_add(payload, "gemini_available",
                          json_object_new_boolean(llm_has_gemini_key()));

   json_object_object_add(response, "payload", payload);

   /* Queue response for WebSocket send */
   ws_response_t resp = { .session = session,
                          .type = WS_RESP_TRANSCRIPT,
                          .transcript = { .role = strdup("__llm_state__"),
                                          .text = strdup(json_object_to_json_string(response)) } };

   json_object_put(response);

   if (resp.transcript.role && resp.transcript.text) {
      queue_response(&resp);
   } else {
      free(resp.transcript.role);
      free(resp.transcript.text);
   }
}

/**
 * @brief Build JSON array of active tools for a session
 * @param session Session to query (tools_mutex must NOT be held by caller)
 * @return Allocated JSON string like "[{\"name\":\"weather\",\"status\":\"running\"}]"
 *         or NULL if no active tools. Caller must free().
 */
static char *build_active_tools_json(session_t *session) {
   if (!session) {
      return NULL;
   }

   pthread_mutex_lock(&session->tools_mutex);
   if (session->active_tool_count == 0) {
      pthread_mutex_unlock(&session->tools_mutex);
      return NULL;
   }

   struct json_object *arr = json_object_new_array();
   for (int i = 0; i < session->active_tool_count; i++) {
      struct json_object *tool = json_object_new_object();
      json_object_object_add(tool, "name", json_object_new_string(session->active_tools[i]));
      json_object_object_add(tool, "status", json_object_new_string("running"));
      json_object_array_add(arr, tool);
   }
   pthread_mutex_unlock(&session->tools_mutex);

   const char *json_str = json_object_to_json_string(arr);
   char *result = strdup(json_str);
   json_object_put(arr);
   return result;
}

/**
 * @brief Add a tool to session's active tools list
 * @return true if added, false if already present or list full
 */
static bool session_add_active_tool(session_t *session, const char *tool_name) {
   if (!session || !tool_name) {
      return false;
   }

   pthread_mutex_lock(&session->tools_mutex);

   /* Check if already in list */
   for (int i = 0; i < session->active_tool_count; i++) {
      if (strcmp(session->active_tools[i], tool_name) == 0) {
         pthread_mutex_unlock(&session->tools_mutex);
         return false; /* Already tracking */
      }
   }

   /* Add if space available */
   if (session->active_tool_count < 8) {
      strncpy(session->active_tools[session->active_tool_count], tool_name, 31);
      session->active_tools[session->active_tool_count][31] = '\0';
      session->active_tool_count++;
      pthread_mutex_unlock(&session->tools_mutex);
      return true;
   }

   pthread_mutex_unlock(&session->tools_mutex);
   return false;
}

/**
 * @brief Remove a tool from session's active tools list
 * @return true if removed, false if not found
 */
static bool session_remove_active_tool(session_t *session, const char *tool_name) {
   if (!session || !tool_name) {
      return false;
   }

   pthread_mutex_lock(&session->tools_mutex);

   for (int i = 0; i < session->active_tool_count; i++) {
      if (strcmp(session->active_tools[i], tool_name) == 0) {
         /* Shift remaining tools down */
         for (int j = i; j < session->active_tool_count - 1; j++) {
            strcpy(session->active_tools[j], session->active_tools[j + 1]);
         }
         session->active_tool_count--;
         pthread_mutex_unlock(&session->tools_mutex);
         return true;
      }
   }

   pthread_mutex_unlock(&session->tools_mutex);
   return false;
}

/**
 * @brief Send state update with current active tools
 */
static void send_state_with_tools(session_t *session, const char *state) {
   if (!session || session->type != SESSION_TYPE_WEBUI) {
      return;
   }

   char *tools_json = build_active_tools_json(session);

   ws_response_t resp = { .session = session,
                          .type = WS_RESP_STATE,
                          .state = {
                              .state = strdup(state),
                              .detail = NULL,
                              .tools_json = tools_json, /* Takes ownership */
                          } };

   if (!resp.state.state) {
      OLOG_ERROR("WebUI: Failed to allocate state response");
      free(tools_json);
      return;
   }

   queue_response(&resp);
}

/**
 * @brief Build a descriptive display name for a tool based on its arguments
 *
 * For tools that may run in parallel with different parameters (e.g., search),
 * this extracts a qualifier to distinguish them in the UI.
 *
 * @param tool_name Base tool name (e.g., "search", "weather")
 * @param tool_args JSON arguments string
 * @param out Output buffer for display name
 * @param out_size Size of output buffer
 */
static void build_tool_display_name(const char *tool_name,
                                    const char *tool_args,
                                    char *out,
                                    size_t out_size) {
   if (!tool_name || !out || out_size == 0) {
      return;
   }

   /* Default to just the tool name */
   strncpy(out, tool_name, out_size - 1);
   out[out_size - 1] = '\0';

   if (!tool_args || tool_args[0] == '\0') {
      return;
   }

   /* Parse arguments to extract qualifier */
   struct json_object *args = json_tokener_parse(tool_args);
   if (!args) {
      return;
   }

   const char *qualifier = NULL;
   struct json_object *val;
   char domain_buf[32]; /* Buffer for URL domain extraction (function scope for safety) */

   /* Tool-specific qualifier extraction */
   if (strcmp(tool_name, "search") == 0) {
      /* For search: use category (news, social, science, etc.) */
      if (json_object_object_get_ex(args, "category", &val)) {
         qualifier = json_object_get_string(val);
      }
   } else if (strcmp(tool_name, "weather") == 0) {
      /* For weather: use location (truncated) */
      if (json_object_object_get_ex(args, "location", &val)) {
         qualifier = json_object_get_string(val);
      }
   } else if (strcmp(tool_name, "url") == 0) {
      /* For url: use domain extracted from URL (uses shared extract_url_host) */
      if (json_object_object_get_ex(args, "url", &val)) {
         const char *url = json_object_get_string(val);
         if (url) {
            extract_url_host(url, domain_buf, sizeof(domain_buf));
            qualifier = domain_buf;
         }
      }
   }

   /* Build display name with qualifier if found */
   if (qualifier && qualifier[0] != '\0') {
      /* Truncate qualifier if too long */
      char short_qual[16];
      strncpy(short_qual, qualifier, sizeof(short_qual) - 1);
      short_qual[sizeof(short_qual) - 1] = '\0';
      snprintf(out, out_size, "%s:%s", tool_name, short_qual);
   }

   json_object_put(args);
}

static void webui_tool_execution_callback(void *session_ptr,
                                          const char *tool_name,
                                          const char *tool_args,
                                          const char *result,
                                          bool success) {
   session_t *session = (session_t *)session_ptr;
   if (!session || session->type != SESSION_TYPE_WEBUI) {
      return;
   }

   /* Build descriptive display name (e.g., "search:news" instead of just "search") */
   char display_name[48];
   build_tool_display_name(tool_name, tool_args, display_name, sizeof(display_name));

   /* result==NULL indicates tool execution is starting, not ending */
   if (result == NULL) {
      /* Tool is starting - add to active list and send state with tools array */
      session_add_active_tool(session, display_name);
      send_state_with_tools(session, "thinking");
      return;
   }

   /* Tool execution completed - remove from active list */
   session_remove_active_tool(session, display_name);

   /* If the tool result contains a <dawn-visual> tag, send the full result
    * as a visible transcript entry (not debug-only). The WebUI visual renderer
    * extracts and renders the tag as a sandboxed iframe. The result may exceed
    * the debug_msg buffer size (8KB), so we send the raw result directly. */
   if (success && result && strstr(result, "<dawn-visual") != NULL) {
      webui_send_transcript(session, "visual", result);

      /* Stash visual content on the session so it gets appended to the
       * assistant message when saved to the conversation DB. This enables
       * visual replay when the conversation is reloaded.
       * Protected by tools_mutex — consumed by handle_save_message. */
      pthread_mutex_lock(&session->tools_mutex);
      free(session->pending_visual);
      session->pending_visual = strdup(result);
      pthread_mutex_unlock(&session->tools_mutex);
   }

   /* Format as debug entry for transcript - use "tool" role (not "assistant")
    * to avoid confusion with actual LLM responses and ensure proper JS routing */
   char debug_msg[LLM_TOOLS_RESULT_LEN + 256];
   snprintf(debug_msg, sizeof(debug_msg), "[Tool Call: %s(%s) -> %s%s]", tool_name,
            tool_args ? tool_args : "", success ? "" : "FAILED: ", result ? result : "(null)");
   webui_send_transcript(session, "tool", debug_msg);

   /* Send updated state (may still have other active tools) */
   pthread_mutex_lock(&session->tools_mutex);
   int remaining = session->active_tool_count;
   pthread_mutex_unlock(&session->tools_mutex);

   if (remaining > 0) {
      /* More tools running - update state with remaining tools */
      send_state_with_tools(session, "thinking");
   }
   /* Note: Don't send idle here - let the LLM flow handle that */

   /* If this was a switch_llm call, send LLM state update to client */
   if (success && strcmp(tool_name, "switch_llm") == 0) {
      webui_send_llm_state_update(session);
   }
}

/* =============================================================================
 * Public API
 * ============================================================================= */

int webui_server_init(int port, const char *www_path) {
   struct lws_context_creation_info info;

   pthread_mutex_lock(&s_mutex);
   if (s_running) {
      pthread_mutex_unlock(&s_mutex);
      OLOG_WARNING("WebUI: Server already running");
      return WEBUI_ERROR_ALREADY_RUNNING;
   }
   pthread_mutex_unlock(&s_mutex);

   /* Determine port */
   if (port <= 0) {
      port = g_config.webui.port;
      if (port <= 0) {
         port = WEBUI_DEFAULT_PORT;
      }
   }

   /* Determine www path */
   if (www_path && www_path[0] != '\0') {
      strncpy(s_www_path, www_path, sizeof(s_www_path) - 1);
   } else if (g_config.webui.www_path[0] != '\0') {
      strncpy(s_www_path, g_config.webui.www_path, sizeof(s_www_path) - 1);
   } else {
      strncpy(s_www_path, WEBUI_DEFAULT_WWW_PATH, sizeof(s_www_path) - 1);
   }
   s_www_path[sizeof(s_www_path) - 1] = '\0';

   /* Configure libwebsockets context */
   memset(&info, 0, sizeof(info));
   info.port = port;
   info.protocols = s_protocols;
   info.gid = -1;
   info.uid = -1;
   /* Increase service buffer for large WebSocket messages (conversation history).
    * Default is ~4KB which causes OVERSIZED_PAYLOAD errors on HTTP/2 connections. */
   info.pt_serv_buf_size = 128 * 1024; /* 128KB - enough for large conversation loads */
#if LWS_LIBRARY_VERSION_NUMBER >= 4004000
   info.ws_ping_pong_interval = 0; /* Disabled: satellites use app-level pings instead */
#endif
   /* Note: Not using LWS_SERVER_OPTION_HTTP_HEADERS_SECURITY_BEST_PRACTICES_ENFORCE
    * because it sets CSP: default-src 'none' which blocks WebAssembly for Opus codec.
    * Security headers are added manually via webui_add_security_headers() and
    * webui_get_static_security_headers() for lws_serve_http_file(). */
   info.options = 0;

   /* Initialize security headers string (uses g_config.webui.https) */
   webui_security_headers_init();
   /* Note: session_manager_set_user_prompt_builder() is called earlier in
    * dawn.c, before mosquitto_loop_start, to close the init-order race
    * where MQTT-triggered refreshes could observe a NULL builder. */

   /* Configure HTTPS if enabled */
   bool use_https = g_config.webui.https;
   if (use_https) {
      if (g_config.webui.ssl_cert_path[0] == '\0' || g_config.webui.ssl_key_path[0] == '\0') {
         OLOG_ERROR("WebUI: HTTPS enabled but ssl_cert_path or ssl_key_path not set");
         OLOG_ERROR("  Hint: Set ssl_cert_path and ssl_key_path in dawn.toml [webui], or run "
                    "./generate_ssl_cert.sh");
         return WEBUI_ERROR_SOCKET;
      }

      /* Verify certificate files exist */
      if (access(g_config.webui.ssl_cert_path, R_OK) != 0) {
         OLOG_ERROR("WebUI: Cannot read SSL certificate: %s", g_config.webui.ssl_cert_path);
         OLOG_ERROR("  Hint: Check file exists and permissions. Regenerate with "
                    "./generate_ssl_cert.sh --renew if expired");
         return WEBUI_ERROR_SOCKET;
      }
      if (access(g_config.webui.ssl_key_path, R_OK) != 0) {
         OLOG_ERROR("WebUI: Cannot read SSL private key: %s", g_config.webui.ssl_key_path);
         OLOG_ERROR("  Hint: Check file exists and permissions. Key should be readable by the DAWN "
                    "process user");
         return WEBUI_ERROR_SOCKET;
      }

      info.options |= LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
      info.ssl_cert_filepath = g_config.webui.ssl_cert_path;
      info.ssl_private_key_filepath = g_config.webui.ssl_key_path;

      /* Force HTTP/1.1 only via ALPN to avoid HTTP/2 frame size limits (16KB).
       * WebSocket over HTTP/2 has stricter frame size requirements that cause
       * OVERSIZED_PAYLOAD errors with large conversation messages. */
      info.alpn = "http/1.1";

      OLOG_INFO("WebUI: HTTPS enabled with cert: %s (HTTP/1.1 only)", g_config.webui.ssl_cert_path);
   }

   OLOG_INFO("WebUI: Initializing %s server on port %d, serving from: %s",
             use_https ? "HTTPS" : "HTTP", port, s_www_path);

   /* Suppress noisy LWS lifecycle logs (connection tags, accept gate, netlink).
    * Only show errors and warnings — everything else is handled by DAWN's own logging. */
   lws_set_log_level(LLL_ERR | LLL_WARN, NULL);

   /* Create context */
   s_lws_context = lws_create_context(&info);
   if (!s_lws_context) {
      OLOG_ERROR("WebUI: Failed to create libwebsockets context");
      return WEBUI_ERROR_SOCKET;
   }

   s_port = port;
   s_running = 1;
   s_client_count = 0;

   /* Initialize audio subsystem (optional - continues if not available) */
#ifdef ENABLE_WEBUI_AUDIO
   if (webui_audio_init() != WEBUI_AUDIO_SUCCESS) {
      OLOG_WARNING("WebUI: Audio subsystem not available, voice input disabled");
   }
#endif

   /* Initialize music streaming subsystem (optional) */
   if (webui_music_init() != 0) {
      OLOG_WARNING("WebUI: Music streaming subsystem not available");
   }

   /* Register tool execution callback for debug display */
   llm_tools_set_execution_callback(webui_tool_execution_callback);

   /* Start server thread */
   if (pthread_create(&s_webui_thread, NULL, webui_thread_func, NULL) != 0) {
      OLOG_ERROR("WebUI: Failed to create server thread");
      webui_music_cleanup();
#ifdef ENABLE_WEBUI_AUDIO
      webui_audio_cleanup();
#endif
      lws_context_destroy(s_lws_context);
      s_lws_context = NULL;
      s_running = 0;
      return WEBUI_ERROR_THREAD;
   }

   OLOG_INFO("WebUI: Server started successfully on port %d", port);

   /* Warn if satellite registration is open (no pre-shared key configured) */
   const secrets_config_t *secrets = config_get_secrets();
   if (!secrets || !secrets->satellite_registration_key[0]) {
      OLOG_WARNING("WebUI: Satellite registration is OPEN (no registration key set)");
      OLOG_WARNING("WebUI: Any device on the network can register as a satellite.");
      OLOG_WARNING("WebUI: Generate a key with: ./generate_ssl_cert.sh --gen-key");
   } else {
      OLOG_INFO("WebUI: Satellite registration key is active");
   }

   return WEBUI_SUCCESS;
}

void webui_server_shutdown(void) {
   pthread_mutex_lock(&s_mutex);
   if (!s_running) {
      pthread_mutex_unlock(&s_mutex);
      return;
   }

   OLOG_INFO("WebUI: Shutting down server...");
   s_running = 0;
   pthread_mutex_unlock(&s_mutex);

   /* Wait for satellite worker threads to finish (max 5 seconds) */
   {
      extern atomic_int g_active_satellite_workers;
      int wait_ms = 0;
      while (atomic_load(&g_active_satellite_workers) > 0 && wait_ms < 5000) {
         if (wait_ms == 0)
            OLOG_INFO("WebUI: Waiting for %d satellite workers to finish...",
                      atomic_load(&g_active_satellite_workers));
         usleep(50000); /* 50ms */
         wait_ms += 50;
      }
      int remaining = atomic_load(&g_active_satellite_workers);
      if (remaining > 0)
         OLOG_WARNING("WebUI: %d satellite workers still active after 5s timeout", remaining);
   }

   /* Wake up lws_service() to process shutdown */
   if (s_lws_context) {
      lws_cancel_service(s_lws_context);
   }

   /* Wait for thread to exit with timeout */
   OLOG_INFO("WebUI: Waiting for server thread to exit (max 2 seconds)...");
   struct timespec ts;
   clock_gettime(CLOCK_REALTIME, &ts);
   ts.tv_sec += 2;

   int join_result = pthread_timedjoin_np(s_webui_thread, NULL, &ts);
   if (join_result == ETIMEDOUT) {
      OLOG_WARNING("WebUI: Server thread did not exit in time, cancelling...");
      pthread_cancel(s_webui_thread);
      pthread_join(s_webui_thread, NULL);
      OLOG_INFO("WebUI: Server thread cancelled and joined");
   } else if (join_result != 0) {
      OLOG_ERROR("WebUI: pthread_timedjoin_np failed: %d", join_result);
   } else {
      OLOG_INFO("WebUI: Server thread exited cleanly");
   }

   /* Destroy context */
   if (s_lws_context) {
      lws_context_destroy(s_lws_context);
      s_lws_context = NULL;
   }

   /* Cleanup music streaming subsystem */
   webui_music_cleanup();

   /* Cleanup audio subsystem */
#ifdef ENABLE_WEBUI_AUDIO
   webui_audio_cleanup();
#endif

   s_port = 0;
   s_client_count = 0;

   OLOG_INFO("WebUI: Server shutdown complete");
}

bool webui_server_is_running(void) {
   bool running;
   pthread_mutex_lock(&s_mutex);
   running = s_running != 0;
   pthread_mutex_unlock(&s_mutex);
   return running;
}

int webui_server_client_count(void) {
   int count;
   pthread_mutex_lock(&s_mutex);
   count = s_client_count;
   pthread_mutex_unlock(&s_mutex);
   return count;
}

int webui_server_get_port(void) {
   return s_port;
}

int webui_get_queue_fill_pct(void) {
   pthread_mutex_lock(&s_queue_mutex);
   int count;
   if (s_queue_tail >= s_queue_head) {
      count = s_queue_tail - s_queue_head;
   } else {
      count = WEBUI_RESPONSE_QUEUE_SIZE - s_queue_head + s_queue_tail;
   }
   pthread_mutex_unlock(&s_queue_mutex);
   return (count * 100) / WEBUI_RESPONSE_QUEUE_SIZE;
}

/* webui_clear_login_rate_limit moved to webui_http.c */

/* =============================================================================
 * Worker-Callable Response Functions (Thread-Safe)
 * ============================================================================= */

/**
 * @brief Send a transcript message to the WebUI client
 *
 * Valid roles and their WebUI behavior:
 *   - "user"      : User input, saved to conversation history, displayed normally
 *   - "assistant" : LLM response, saved to conversation history, displayed normally
 *   - "tool"      : Tool execution debug info, NOT saved to history, debug-only display
 *   - "system"    : System/error messages, displayed with error styling
 *   - "streaming" : Partial assistant response (handled separately by streaming code)
 *
 * The "tool" role is specifically for internal debug output (tool calls, results)
 * that should be visible in debug mode but not pollute conversation history.
 * This prevents LLM context pollution when debug output is replayed.
 *
 * @param session WebSocket session to send to
 * @param role    Message role (see above)
 * @param text    Message content
 */
void webui_send_transcript_ex(session_t *session,
                              const char *role,
                              const char *text,
                              bool server_saved) {
   if (!session || session->type != SESSION_TYPE_WEBUI) {
      return;
   }

   ws_response_t resp = { .session = session,
                          .type = WS_RESP_TRANSCRIPT,
                          .transcript = {
                              .role = strdup(role),
                              .text = strdup(text),
                              .server_saved = server_saved,
                          } };

   if (!resp.transcript.role || !resp.transcript.text) {
      free(resp.transcript.role);
      free(resp.transcript.text);
      OLOG_ERROR("WebUI: Failed to allocate transcript response");
      return;
   }

   queue_response(&resp);
}

void webui_send_transcript(session_t *session, const char *role, const char *text) {
   webui_send_transcript_ex(session, role, text, false);
}

void webui_send_state_with_detail(session_t *session, const char *state, const char *detail) {
   if (!session || (session->type != SESSION_TYPE_WEBUI && session->type != SESSION_TYPE_DAP2)) {
      return;
   }

   ws_response_t resp = { .session = session,
                          .type = WS_RESP_STATE,
                          .state = {
                              .state = strdup(state),
                              .detail = detail ? strdup(detail) : NULL,
                          } };

   if (!resp.state.state) {
      OLOG_ERROR("WebUI: Failed to allocate state response");
      return;
   }

   queue_response(&resp);
}

void webui_send_state(session_t *session, const char *state) {
   webui_send_state_with_detail(session, state, NULL);

   /* Also send metrics update with state change */
   int context_pct = -1; /* -1 = no data, JS will keep previous value */
   llm_context_usage_t usage;
   session_llm_config_t llm_cfg;
   session_get_llm_config(session, &llm_cfg);
   if (llm_context_get_usage(session->session_id, llm_cfg.type, llm_cfg.cloud_provider, NULL,
                             &usage) == 0 &&
       usage.max_tokens > 0) {
      context_pct = (int)((float)usage.current_tokens / (float)usage.max_tokens * 100.0f);
   }
   webui_send_metrics_update(session, state, 0, 0.0f, context_pct);
}

void webui_send_context(session_t *session, int current_tokens, int max_tokens, float threshold) {
   /* If session is NULL, broadcast to all WebSocket sessions */
   if (!session) {
      /* Get local session as default */
      session = session_get_local();
   }

   if (!session || session->type != SESSION_TYPE_WEBUI) {
      return;
   }

   ws_response_t resp = { .session = session,
                          .type = WS_RESP_CONTEXT,
                          .context = {
                              .current_tokens = current_tokens,
                              .max_tokens = max_tokens,
                              .threshold = threshold,
                          } };

   queue_response(&resp);
}

void webui_send_error(session_t *session, const char *code, const char *message) {
   if (!session || (session->type != SESSION_TYPE_WEBUI && session->type != SESSION_TYPE_DAP2)) {
      return;
   }

   ws_response_t resp = { .session = session,
                          .type = WS_RESP_ERROR,
                          .error = {
                              .code = strdup(code),
                              .message = strdup(message),
                          } };

   if (!resp.error.code || !resp.error.message) {
      free(resp.error.code);
      free(resp.error.message);
      OLOG_ERROR("WebUI: Failed to allocate error response");
      return;
   }

   queue_response(&resp);
}

void webui_send_compaction_complete(session_t *session,
                                    int tokens_before,
                                    int tokens_after,
                                    int messages_summarized,
                                    const char *summary,
                                    int level) {
   if (!session || session->type != SESSION_TYPE_WEBUI) {
      return;
   }

   ws_response_t resp = { .session = session,
                          .type = WS_RESP_COMPACTION_COMPLETE,
                          .compaction = {
                              .tokens_before = tokens_before,
                              .tokens_after = tokens_after,
                              .messages_summarized = messages_summarized,
                              .level = level,
                              .summary = summary ? strdup(summary) : NULL,
                          } };

   queue_response(&resp);
}

/**
 * @brief Queue audio data for sending to WebSocket client
 *
 * Copies the audio data to the response queue. The WebUI thread will
 * send it as binary WebSocket frames. Large audio is chunked to avoid
 * overwhelming libwebsockets with huge single writes.
 *
 * @param session WebSocket session
 * @param data PCM audio data
 * @param len Length of audio data
 */
#define AUDIO_CHUNK_SIZE \
   8192 /* 8KB chunks for WebSocket frames - keep small for lws compatibility */

void webui_send_audio(session_t *session, const uint8_t *data, size_t len) {
   if (!session || !data || len == 0) {
      return;
   }
   if (session->type != SESSION_TYPE_WEBUI && session->type != SESSION_TYPE_DAP2) {
      return;
   }

   /* Send audio in chunks to avoid overwhelming lws_write with huge buffers */
   size_t offset = 0;
   int chunk_num = 0;

   while (offset < len) {
      size_t chunk_len = len - offset;
      if (chunk_len > AUDIO_CHUNK_SIZE) {
         chunk_len = AUDIO_CHUNK_SIZE;
      }

      uint8_t *chunk_copy = malloc(chunk_len);
      if (!chunk_copy) {
         OLOG_ERROR("WebUI: Failed to allocate audio chunk buffer");
         return;
      }
      memcpy(chunk_copy, data + offset, chunk_len);

      ws_response_t resp = { .session = session,
                             .type = WS_RESP_AUDIO,
                             .audio = {
                                 .data = chunk_copy,
                                 .len = chunk_len,
                             } };

      queue_response(&resp);
      offset += chunk_len;
      chunk_num++;
   }
}

/**
 * @brief Queue end-of-audio marker for WebSocket client
 *
 * @param session WebSocket session
 */
void webui_send_audio_end(session_t *session, bool is_opus) {
   if (!session || (session->type != SESSION_TYPE_WEBUI && session->type != SESSION_TYPE_DAP2)) {
      return;
   }

   ws_response_t resp = { .session = session, .type = WS_RESP_AUDIO_END };
   resp.audio.is_opus = is_opus;

   queue_response(&resp);
}

/* =============================================================================
 * LLM Streaming Functions (ChatGPT-style real-time text)
 *
 * These functions provide real-time token streaming to WebUI clients.
 * Protocol:
 *   1. stream_start - Create new assistant entry, enter streaming state
 *   2. stream_delta - Append text to current entry (multiple calls)
 *   3. stream_end   - Finalize entry, exit streaming state
 *
 * Stream IDs prevent stale deltas from cancelled streams from being displayed.
 * ============================================================================= */

void webui_send_stream_start(session_t *session) {
   if (!session || (session->type != SESSION_TYPE_WEBUI && session->type != SESSION_TYPE_DAP2)) {
      return;
   }

   /* Increment stream ID and mark streaming active */
   uint32_t sid = atomic_fetch_add(&session->current_stream_id, 1) + 1;
   atomic_store(&session->llm_streaming_active, true);

   /* Reset command tag filter state for new stream */
   session->cmd_tag_filter.nesting_depth = 0;
   session->cmd_tag_filter.len = 0;

   /* Reset sentence spacing tracker */
   session->stream_last_char = '\0';

   /* Cache whether to bypass filtering (native tools enabled) */
   session->cmd_tag_filter_bypass = llm_tools_enabled(NULL);

   /* Transition to "speaking" state when streaming begins */
   webui_send_state(session, "speaking");

   ws_response_t resp = { .session = session,
                          .type = WS_RESP_STREAM_START,
                          .stream = {
                              .stream_id = sid,
                              .text = "",
                          } };

   queue_response(&resp);
   OLOG_INFO("WebUI: Stream start id=%u for session %u", sid, session->session_id);
}

/* Command tag filter uses shared constants from core/text_filter.h */

/**
 * @brief Check if character is a sentence terminator
 */
static inline bool is_sentence_terminator(char c) {
   return c == '.' || c == '!' || c == '?' || c == ':';
}

/**
 * @brief Check and fix sentence spacing for streaming text
 *
 * LLM streaming sometimes omits spaces after sentence terminators.
 * This function detects when the previous chunk ended with a terminator
 * and the new chunk starts with a letter, prepending a space if needed.
 *
 * @param session Session with stream_last_char tracking
 * @param text Input text
 * @param out_buf Output buffer (can overlap with text if no space needed)
 * @param out_size Size of output buffer
 * @return Pointer to text to send (either out_buf with space, or original text)
 */
static const char *fix_sentence_spacing(session_t *session,
                                        const char *text,
                                        char *out_buf,
                                        size_t out_size) {
   if (!session || !text || !text[0] || !out_buf || out_size < 2) {
      return text;
   }

   /* Check if we need to add a space:
    * - Previous chunk ended with sentence terminator
    * - This chunk starts with a letter */
   char first = text[0];
   bool needs_space = is_sentence_terminator(session->stream_last_char) &&
                      ((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z'));

   if (needs_space) {
      /* Prepend space to the text */
      size_t text_len = strlen(text);
      if (text_len + 2 <= out_size) {
         out_buf[0] = ' ';
         memcpy(out_buf + 1, text, text_len + 1); /* Include null terminator */
         return out_buf;
      }
   }

   return text;
}

/**
 * @brief Update the last character tracker after sending text
 */
static inline void update_stream_last_char(session_t *session, const char *text) {
   if (session && text) {
      size_t len = strlen(text);
      if (len > 0) {
         session->stream_last_char = text[len - 1];
      }
   }
}

/**
 * @brief Output callback for WebUI streaming (adapter for text_filter API)
 *
 * This callback adapts the shared text_filter's signature to WebUI needs.
 * Session is passed via ctx parameter.
 */
static void webui_filter_output(const char *text, size_t len, void *ctx) {
   session_t *session = (session_t *)ctx;
   if (!session || !text || len == 0) {
      return;
   }

   /* Start stream on first content (lazy initialization) */
   if (!session->llm_streaming_active) {
      webui_send_stream_start(session);
   }

   /* Null-terminate the input for processing */
   char temp_buf[4096];
   size_t safe_len = len < sizeof(temp_buf) - 1 ? len : sizeof(temp_buf) - 1;
   memcpy(temp_buf, text, safe_len);
   temp_buf[safe_len] = '\0';

   /* Fix sentence spacing (LLM sometimes omits space after period) */
   char spaced_buf[4098]; /* Extra room for prepended space */
   const char *fixed_text = fix_sentence_spacing(session, temp_buf, spaced_buf, sizeof(spaced_buf));

   ws_response_t resp = { .session = session,
                          .type = WS_RESP_STREAM_DELTA,
                          .stream = {
                              .stream_id = session->current_stream_id,
                          } };

   snprintf(resp.stream.text, sizeof(resp.stream.text), "%s", fixed_text);

   session->stream_had_content = true;
   update_stream_last_char(session, resp.stream.text);
   queue_response(&resp);
}

/**
 * @brief Filter command tags and return filtered text
 *
 * Public function for callers that need the filtered text (e.g., TTS).
 * Uses the same state machine as WebUI streaming.
 *
 * @param session Session with filter state
 * @param text Input text to filter
 * @param out_buf Output buffer for filtered text
 * @param out_size Size of output buffer
 * @return Length of filtered text written to out_buf
 */
int webui_filter_command_tags(session_t *session,
                              const char *text,
                              char *out_buf,
                              size_t out_size) {
   if (!session || !text || !out_buf || out_size == 0) {
      return 0;
   }

   /* Native tools mode: no command tags to filter */
   if (session->cmd_tag_filter_bypass) {
      size_t len = strlen(text);
      size_t copy = len < out_size - 1 ? len : out_size - 1;
      memcpy(out_buf, text, copy);
      out_buf[copy] = '\0';
      return (int)copy;
   }

   /* Use shared text filter implementation */
   return text_filter_command_tags_to_buffer(&session->cmd_tag_filter, text, out_buf, out_size);
}

/**
 * @brief Send streaming text to WebUI with command tag filtering
 *
 * Filters <command>...</command> tags when in legacy mode (native tools disabled).
 * Automatically starts the stream on first content. Thread-safe per session.
 */
void webui_send_stream_delta(session_t *session, const char *text) {
   if (!session || (session->type != SESSION_TYPE_WEBUI && session->type != SESSION_TYPE_DAP2)) {
      return;
   }

   if (!text || text[0] == '\0') {
      return;
   }

   /* If native tools are enabled, pass through without filtering */
   if (session->cmd_tag_filter_bypass) {
      if (!session->llm_streaming_active) {
         webui_send_stream_start(session);
      }

      /* Fix sentence spacing (LLM sometimes omits space after period) */
      char spaced_buf[4098];
      const char *fixed_text = fix_sentence_spacing(session, text, spaced_buf, sizeof(spaced_buf));

      ws_response_t resp = { .session = session,
                             .type = WS_RESP_STREAM_DELTA,
                             .stream = {
                                 .stream_id = session->current_stream_id,
                             } };
      snprintf(resp.stream.text, sizeof(resp.stream.text), "%s", fixed_text);
      session->stream_had_content = true;
      update_stream_last_char(session, resp.stream.text);
      queue_response(&resp);
      return;
   }

   /* Legacy command tag mode: filter using shared state machine */
   text_filter_command_tags(&session->cmd_tag_filter, text, webui_filter_output, session);
}

void webui_send_stream_end(session_t *session, const char *reason) {
   if (!session || (session->type != SESSION_TYPE_WEBUI && session->type != SESSION_TYPE_DAP2)) {
      return;
   }

   /* Mark streaming inactive */
   atomic_store(&session->llm_streaming_active, false);

   ws_response_t resp = { .session = session,
                          .type = WS_RESP_STREAM_END,
                          .stream = {
                              .stream_id = atomic_load(&session->current_stream_id),
                          } };

   /* Copy reason into fixed buffer (no malloc/free churn) */
   const char *r = reason ? reason : "complete";
   strncpy(resp.stream.text, r, sizeof(resp.stream.text) - 1);
   resp.stream.text[sizeof(resp.stream.text) - 1] = '\0';

   queue_response(&resp);
   OLOG_INFO("WebUI: Stream end id=%u reason=%s for session %u", session->current_stream_id, r,
             session->session_id);
}

/* =============================================================================
 * Extended Thinking Public API
 * ============================================================================= */

void webui_send_thinking_start(session_t *session, const char *provider) {
   if (!session || session->type != SESSION_TYPE_WEBUI) {
      return;
   }

   ws_response_t resp = { .session = session,
                          .type = WS_RESP_THINKING_START,
                          .stream = {
                              .stream_id = session->current_stream_id,
                          } };

   /* Store provider name in text buffer */
   const char *p = provider ? provider : "unknown";
   strncpy(resp.stream.text, p, sizeof(resp.stream.text) - 1);
   resp.stream.text[sizeof(resp.stream.text) - 1] = '\0';

   queue_response(&resp);
   OLOG_INFO("WebUI: Thinking start id=%u provider=%s for session %u", session->current_stream_id,
             p, session->session_id);
}

void webui_send_thinking_delta(session_t *session, const char *text) {
   if (!session || session->type != SESSION_TYPE_WEBUI) {
      return;
   }

   if (!text || text[0] == '\0') {
      return;
   }

   ws_response_t resp = { .session = session,
                          .type = WS_RESP_THINKING_DELTA,
                          .stream = {
                              .stream_id = session->current_stream_id,
                          } };

   strncpy(resp.stream.text, text, sizeof(resp.stream.text) - 1);
   resp.stream.text[sizeof(resp.stream.text) - 1] = '\0';

   queue_response(&resp);
}

void webui_send_thinking_end(session_t *session, bool has_content) {
   if (!session || session->type != SESSION_TYPE_WEBUI) {
      return;
   }

   ws_response_t resp = { .session = session,
                          .type = WS_RESP_THINKING_END,
                          .stream = {
                              .stream_id = session->current_stream_id,
                          } };

   /* Use text[0] to store has_content flag */
   resp.stream.text[0] = has_content ? '1' : '0';
   resp.stream.text[1] = '\0';

   queue_response(&resp);
   OLOG_INFO("WebUI: Thinking end id=%u has_content=%s for session %u", session->current_stream_id,
             has_content ? "true" : "false", session->session_id);
}

void webui_send_reasoning_summary(session_t *session, int reasoning_tokens) {
   if (!session || session->type != SESSION_TYPE_WEBUI) {
      return;
   }

   if (reasoning_tokens <= 0) {
      return;
   }

   ws_response_t resp = { .session = session,
                          .type = WS_RESP_REASONING_SUMMARY,
                          .stream = {
                              .stream_id = session->current_stream_id,
                          } };

   /* Store reasoning_tokens as string in text buffer */
   snprintf(resp.stream.text, sizeof(resp.stream.text), "%d", reasoning_tokens);

   queue_response(&resp);
   OLOG_INFO("WebUI: Reasoning summary id=%u tokens=%d for session %u", session->current_stream_id,
             reasoning_tokens, session->session_id);
}

void webui_send_session_json(session_t *session, const char *json_str) {
   if (!session || session->type != SESSION_TYPE_WEBUI || !json_str) {
      return;
   }

   ws_response_t resp = { 0 };
   resp.session = session;
   resp.type = WS_RESP_JSON;
   resp.generic_json.json = strdup(json_str);
   if (!resp.generic_json.json) {
      OLOG_ERROR("WebUI: Failed to allocate JSON response string");
      return;
   }
   queue_response(&resp);
}

void webui_send_conversation_reset(session_t *session) {
   if (!session || session->type != SESSION_TYPE_WEBUI) {
      return;
   }

   ws_response_t resp = { .session = session, .type = WS_RESP_CONVERSATION_RESET };

   queue_response(&resp);
   OLOG_INFO("WebUI: Conversation reset notification for session %u", session->session_id);
}

/* =============================================================================
 * Session Detach (called before session destruction)
 * ============================================================================= */

void webui_detach_session(session_t *session) {
   if (!session)
      return;

   /* Clean up stale token registry entries for this session */
   unregister_tokens_for_session(session->session_id);

   int detached = 0;
   pthread_mutex_lock(&s_conn_registry_mutex);
   for (int i = 0; i < MAX_ACTIVE_CONNECTIONS; i++) {
      ws_connection_t *conn = s_active_connections[i];
      if (!conn || conn->session != session)
         continue;

      /* Detach session pointer — the connection stays alive.
       * On the next message, webui_conn_create_session() will
       * transparently create a fresh session so the user can
       * keep working without logging in again.
       * Atomic store: this runs on the maintenance thread while
       * the LWS thread may read conn->session concurrently. */
      conn_set_session(conn, NULL);
      detached++;
   }
   pthread_mutex_unlock(&s_conn_registry_mutex);

   if (detached > 0) {
      OLOG_INFO("WebUI: Detached %d connection(s) from expiring session %u", detached,
                session->session_id);

      /* Release the references that the connections held.
       * This unblocks session_destroy's ref_count wait. */
      for (int i = 0; i < detached; i++) {
         session_release(session);
      }
   }
}

/* =============================================================================
 * Plan Progress (session-targeted)
 * ============================================================================= */

int64_t webui_get_active_conversation_id(session_t *session) {
   if (!session || session->type != SESSION_TYPE_WEBUI || !session->client_data)
      return 0;
   ws_connection_t *conn = (ws_connection_t *)session->client_data;
   return conn->active_conversation_id;
}

void webui_broadcast_plan_progress(session_t *session, const char *json_str) {
   if (!session || session->type != SESSION_TYPE_WEBUI || !json_str)
      return;

   ws_response_t resp = { .session = session,
                          .type = WS_RESP_JSON,
                          .generic_json = { .json = strdup(json_str) } };
   if (!resp.generic_json.json)
      return;

   queue_response(&resp);
}

/* =============================================================================
 * Real-Time Metrics for UI Visualization
 *
 * Provides metrics for multi-ring visualization. Sent on:
 * - State changes (immediate)
 * - Token chunk events (during streaming)
 * - Periodic heartbeat (1Hz when idle)
 * ============================================================================= */

void webui_send_metrics_update(session_t *session,
                               const char *state,
                               int ttft_ms,
                               float token_rate,
                               int context_percent) {
   if (!session || session->type != SESSION_TYPE_WEBUI) {
      return;
   }

   ws_response_t resp = { .session = session, .type = WS_RESP_METRICS_UPDATE };

   /* Copy state into fixed buffer */
   const char *s = state ? state : "idle";
   strncpy(resp.metrics.state, s, sizeof(resp.metrics.state) - 1);
   resp.metrics.state[sizeof(resp.metrics.state) - 1] = '\0';

   resp.metrics.ttft_ms = ttft_ms;
   resp.metrics.token_rate = token_rate;
   resp.metrics.context_pct = context_percent;

   queue_response(&resp);
}

/* =============================================================================
 * Text Processing (Async Worker Thread)
 *
 * For Phase 2, we use a simple detached thread for text processing.
 * Phase 4 may integrate with the worker pool for audio + text.
 * ============================================================================= */

/* Command processing constants */
#define MAX_TOOL_RESULTS 8
#define TOOL_RESULT_MSG_SIZE 1024
#define WEBUI_WORKER_ID 100 /* Virtual worker ID for command router */

typedef struct {
   session_t *session;
   char *text;
   unsigned int request_gen; /* Captured request_generation to detect superseded requests */
   /* Vision support fields - supports multiple images per message */
   char *vision_images[WEBUI_MAX_VISION_IMAGES_CAP];       /* Base64 encoded images */
   size_t vision_image_sizes[WEBUI_MAX_VISION_IMAGES_CAP]; /* Size of each image */
   char vision_mimes[WEBUI_MAX_VISION_IMAGES_CAP][WEBUI_VISION_MIME_MAX]; /* MIME types */
   int vision_image_count;                                                /* Number of images */
} text_work_t;

/**
 * @brief Process commands in LLM response and make follow-up calls
 *
 * Searches for <command> tags in the response, executes them via MQTT,
 * and makes follow-up LLM calls with the results.
 *
 * @param llm_response The LLM response to process
 * @param session Session for follow-up LLM calls
 * @return Final response text (caller must free), or NULL if no commands
 */
char *webui_process_commands(const char *llm_response, session_t *session) {
   if (!llm_response || !session) {
      return NULL;
   }

   /* Bail early if satellite/client already disconnected — avoid wasting LLM API calls */
   if (session->disconnected) {
      return NULL;
   }

   struct mosquitto *mosq = worker_pool_get_mosq();
   if (!mosq) {
      OLOG_WARNING("WebUI: No MQTT connection, cannot process commands");
      return NULL;
   }

   /* Collect all tool results */
   char *tool_results[MAX_TOOL_RESULTS];
   int num_results = 0;

   for (int i = 0; i < MAX_TOOL_RESULTS; i++) {
      tool_results[i] = NULL;
   }

   /* Search for <command> tags and process each one */
   const char *search_ptr = llm_response;
   const char *cmd_start;

   while ((cmd_start = strstr(search_ptr, "<command>")) != NULL && num_results < MAX_TOOL_RESULTS) {
      const char *cmd_end = strstr(cmd_start, "</command>");
      if (!cmd_end) {
         OLOG_WARNING("WebUI: Unclosed <command> tag");
         break;
      }

      /* Extract command JSON */
      const char *json_start = cmd_start + strlen("<command>");
      size_t json_len = cmd_end - json_start;

      char *cmd_json = malloc(json_len + 1);
      if (!cmd_json) {
         OLOG_ERROR("WebUI: Failed to allocate command JSON");
         break;
      }
      memcpy(cmd_json, json_start, json_len);
      cmd_json[json_len] = '\0';

      /* Send command to WebUI debug panel (wraps in tags for JS extraction)
       * Only send if we had streamed content - otherwise the "didn't stream" fallback
       * in session_manager.c already sends the full response with command tags */
      if (session->stream_had_content) {
         char *debug_cmd = malloc(json_len + 32);
         if (debug_cmd) {
            snprintf(debug_cmd, json_len + 32, "<command>%s</command>", cmd_json);
            webui_send_transcript(session, "assistant", debug_cmd);
            free(debug_cmd);
         }
      }

      OLOG_INFO("WebUI: Processing command: %s", cmd_json);

      /* Parse JSON to extract device/action */
      struct json_object *parsed_json = json_tokener_parse(cmd_json);
      if (!parsed_json) {
         OLOG_WARNING("WebUI: Invalid command JSON: %s", cmd_json);
         free(cmd_json);
         search_ptr = cmd_end + strlen("</command>");
         continue;
      }

      /* Get device and action - both required for valid command */
      struct json_object *device_obj = NULL;
      struct json_object *action_obj = NULL;

      if (!json_object_object_get_ex(parsed_json, "device", &device_obj) || !device_obj) {
         OLOG_WARNING("WebUI: Skipping malformed command - missing 'device' field: %s", cmd_json);
         json_object_put(parsed_json);
         free(cmd_json);
         search_ptr = cmd_end + strlen("</command>");
         continue;
      }

      const char *device_name = json_object_get_string(device_obj);
      if (!device_name || device_name[0] == '\0') {
         OLOG_WARNING("WebUI: Skipping malformed command - empty 'device' field: %s", cmd_json);
         json_object_put(parsed_json);
         free(cmd_json);
         search_ptr = cmd_end + strlen("</command>");
         continue;
      }

      /* Action is optional for some commands (e.g., triggers), default to "unknown" */
      const char *action_name = "unknown";
      if (json_object_object_get_ex(parsed_json, "action", &action_obj) && action_obj) {
         const char *action_str = json_object_get_string(action_obj);
         if (action_str && action_str[0] != '\0') {
            action_name = action_str;
         }
      }

      /* Register pending request */
      pending_request_t *req = command_router_register(WEBUI_WORKER_ID);
      if (!req) {
         OLOG_ERROR("WebUI: Failed to register pending request");
         json_object_put(parsed_json);
         free(cmd_json);
         search_ptr = cmd_end + strlen("</command>");
         continue;
      }

      const char *request_id = command_router_get_id(req);
      OLOG_INFO("WebUI: Registered request %s", request_id);

      /* Add request_id, session_id, and timestamp to command JSON (OCP v1.1) */
      json_object_object_add(parsed_json, "request_id", json_object_new_string(request_id));
      json_object_object_add(parsed_json, "session_id",
                             json_object_new_int((int32_t)session->session_id));
      json_object_object_add(parsed_json, "timestamp",
                             json_object_new_int64(ocp_get_timestamp_ms()));
      const char *cmd_with_id = json_object_to_json_string(parsed_json);

      /* Send tool call status to UI (works for both WebUI and satellites) */
      char tool_detail[128];
      snprintf(tool_detail, sizeof(tool_detail), "Calling %s...", device_name);
      webui_send_state_with_detail(session, "tool_call", tool_detail);

      /* Publish command via MQTT */
      int rc = mosquitto_publish(mosq, NULL, APPLICATION_NAME, strlen(cmd_with_id), cmd_with_id, 0,
                                 false);
      if (rc != MOSQ_ERR_SUCCESS) {
         OLOG_ERROR("WebUI: MQTT publish failed: %d", rc);
         command_router_cancel(req);
         json_object_put(parsed_json);
         free(cmd_json);
         search_ptr = cmd_end + strlen("</command>");
         continue;
      }
      OLOG_INFO("WebUI: Published command to %s", APPLICATION_NAME);

      /* Wait for result */
      char *callback_result = command_router_wait(req, COMMAND_RESULT_TIMEOUT_MS);

      /* Format result for LLM */
      tool_results[num_results] = malloc(TOOL_RESULT_MSG_SIZE);
      if (tool_results[num_results]) {
         if (callback_result && strlen(callback_result) > 0) {
            OLOG_INFO("WebUI: Received callback result: %.50s%s", callback_result,
                      strlen(callback_result) > 50 ? "..." : "");
            snprintf(tool_results[num_results], TOOL_RESULT_MSG_SIZE,
                     "[Tool Result: %s.%s returned: %s]", device_name, action_name,
                     callback_result);
         } else {
            OLOG_WARNING("WebUI: No callback result (timeout or empty)");
            snprintf(tool_results[num_results], TOOL_RESULT_MSG_SIZE,
                     "[Tool Result: %s.%s completed successfully]", device_name, action_name);
         }

         /* Send tool result to WebUI for debug display */
         webui_send_transcript(session, "assistant", tool_results[num_results]);

         num_results++;
      }

      if (callback_result) {
         free(callback_result);
      }

      json_object_put(parsed_json);
      free(cmd_json);
      search_ptr = cmd_end + strlen("</command>");
   }

   /* If no results collected, return NULL (no commands processed) */
   if (num_results == 0) {
      return NULL;
   }

   /* Build combined tool results message for LLM */
   size_t total_len = 1; /* For null terminator */
   for (int i = 0; i < num_results; i++) {
      if (tool_results[i]) {
         total_len += strlen(tool_results[i]) + 1; /* +1 for newline */
      }
   }

   char *combined_results = malloc(total_len);
   if (!combined_results) {
      OLOG_ERROR("WebUI: Failed to allocate combined results");
      for (int i = 0; i < num_results; i++) {
         free(tool_results[i]);
      }
      return NULL;
   }

   char *ptr = combined_results;
   for (int i = 0; i < num_results; i++) {
      if (tool_results[i]) {
         size_t len = strlen(tool_results[i]);
         memcpy(ptr, tool_results[i], len);
         ptr += len;
         if (i < num_results - 1) {
            *ptr++ = '\n';
         }
         free(tool_results[i]);
      }
   }
   *ptr = '\0';

   OLOG_INFO("WebUI: Sending tool results to LLM: %s", combined_results);

   /* Make follow-up LLM call with tool results */
   char *final_response = session_llm_call(session, combined_results);

   free(combined_results);

   if (!final_response) {
      OLOG_ERROR("WebUI: Follow-up LLM call failed");
      return NULL;
   }

   OLOG_INFO("WebUI: LLM final response: %.50s%s", final_response,
             strlen(final_response) > 50 ? "..." : "");

   return final_response;
}

/* strip_command_tags() is shared across webui modules — see webui_satellite.c */

/* REQUEST_SUPERSEDED macro now defined in webui_internal.h */

static void text_worker_cleanup(text_work_t *work, session_t *session, char *text) {
   if (session) {
      session_release(session);
   }
   if (text) {
      free(text);
   }
   if (work) {
      /* Free all vision images if present */
      for (int i = 0; i < work->vision_image_count; i++) {
         if (work->vision_images[i]) {
            free(work->vision_images[i]);
            work->vision_images[i] = NULL;
         }
      }
      work->vision_image_count = 0;
      free(work);
   }
}

static void *text_worker_thread(void *arg) {
   text_work_t *work = (text_work_t *)arg;
   session_t *session = work->session;
   char *text = work->text;
   unsigned int expected_gen = work->request_gen;

   /* Check if session is still valid or if this request was superseded */
   if (!session || REQUEST_SUPERSEDED(session, expected_gen)) {
      OLOG_INFO("WebUI: Session disconnected or request superseded, aborting text processing");
      text_worker_cleanup(work, session, text);
      return NULL;
   }

   if (work->vision_image_count > 0) {
      size_t total_bytes = 0;
      for (int i = 0; i < work->vision_image_count; i++) {
         total_bytes += work->vision_image_sizes[i];
      }
      OLOG_INFO("WebUI: Processing text+vision for session %u: %s (%d images, %zu total bytes)",
                session->session_id, text, work->vision_image_count, total_bytes);
   } else {
      OLOG_INFO("WebUI: Processing text input for session %u: %s", session->session_id, text);
   }

   /* Clear stale pending_visual from a previous turn that may not have
    * been consumed (e.g., LLM response interrupted or error). Prevents
    * visuals from attaching to the wrong assistant message. */
   pthread_mutex_lock(&session->tools_mutex);
   free(session->pending_visual);
   session->pending_visual = NULL;
   pthread_mutex_unlock(&session->tools_mutex);

   /* Send "thinking" state */
   if (work->vision_image_count > 0) {
      if (work->vision_image_count == 1) {
         webui_send_state_with_detail(session, "thinking", "Analyzing image...");
      } else {
         webui_send_state_with_detail(session, "thinking", "Analyzing images...");
      }
   } else {
      webui_send_state_with_detail(session, "thinking", "Processing request...");
   }

   /* Add user message to session history immediately (before LLM call can be cancelled).
    * If images present, store in multi-part format so they persist across turns. */
   if (work->vision_image_count > 0) {
      session_add_message_with_images(session, "user", text,
                                      (const char *const *)work->vision_images,
                                      work->vision_image_count);
   } else {
      session_add_message(session, "user", text);
   }

   /* Persist user message to conversation DB immediately (server-side).
    * This ensures the message is in the DB before any client-side load_conversation
    * request arrives — fixes a race where session expiry triggers auto-create,
    * the client reloads conversation from DB, but the new user message hasn't been
    * persisted yet (normally saved via client round-trip save_message).
    * When server_saved=true, the client skips its own save_message to avoid dupes. */
   ws_connection_t *conn = (ws_connection_t *)session->client_data;
   bool saved_to_db = false;
   if (conn && conn->active_conversation_id > 0) {
      if (conv_db_add_message(conn->active_conversation_id, conn->auth_user_id, "user", text) ==
          AUTH_DB_SUCCESS) {
         saved_to_db = true;
      }
   }

   /* Echo user input as transcript (with server_saved flag to prevent duplicate DB save) */
   webui_send_transcript_ex(session, "user", text, saved_to_db);

   /* Check if TTS is enabled for this connection */
   bool tts_enabled = conn && conn->tts_enabled;

   /* Call LLM with session's conversation history (message already added above)
    * Pass vision data if present - LLM will include in request
    * TTS callback is NULL when disabled, causing simple text streaming */
   char *response = session_llm_call_with_tts_vision_no_add(
       session, text, (const char **)work->vision_images, work->vision_image_sizes,
       (const char(*)[WEBUI_VISION_MIME_MAX])work->vision_mimes, work->vision_image_count,
       tts_enabled ? webui_sentence_audio_callback : NULL, tts_enabled ? session : NULL);

   /* Free vision data after LLM call (it's been sent over HTTP, no longer needed) */
   for (int i = 0; i < work->vision_image_count; i++) {
      if (work->vision_images[i]) {
         free(work->vision_images[i]);
         work->vision_images[i] = NULL;
      }
   }
   work->vision_image_count = 0;

   /* Check if request was superseded during LLM call */
   if (REQUEST_SUPERSEDED(session, expected_gen)) {
      /* Request superseded - don't try to send response */
      OLOG_INFO("WebUI: Session %u request superseded during LLM call", session->session_id);
      session_release(session);
      free(response);
      free(text);
      free(work);
      return NULL;
   }

   if (!response) {
      /* LLM call failed */
      webui_send_error(session, "LLM_ERROR", "Failed to get response from AI");
      webui_send_state(session, "idle");
      session_release(session);
      free(text);
      free(work);
      return NULL;
   }

   /* Check for command tags and process them */
   char *final_response = response;
   if (strstr(response, "<command>")) {
      OLOG_INFO("WebUI: Response contains commands, processing...");

      /* Note: Don't send intermediate response here - streaming already delivered it */

      /* Process commands and get follow-up response */
      char *processed = webui_process_commands(response, session);
      if (processed) {
         /* Check if request was superseded after command processing */
         if (REQUEST_SUPERSEDED(session, expected_gen)) {
            OLOG_INFO("WebUI: Session %u request superseded during command processing",
                      session->session_id);
            session_release(session);
            free(response);
            free(processed);
            free(text);
            free(work);
            return NULL;
         }

         /* Recursively process if the follow-up also contains commands.
          * Limit iterations to prevent infinite loops from confused LLMs. */
         int follow_up_iterations = 0;
         const int MAX_FOLLOW_UP_ITERATIONS = 5;

         while (strstr(processed, "<command>") && !REQUEST_SUPERSEDED(session, expected_gen)) {
            follow_up_iterations++;
            if (follow_up_iterations > MAX_FOLLOW_UP_ITERATIONS) {
               OLOG_WARNING("WebUI: Command loop limit reached (%d iterations), breaking",
                            MAX_FOLLOW_UP_ITERATIONS);
               break;
            }

            OLOG_INFO(
                "WebUI: Follow-up response contains more commands, processing... (iter %d/%d)",
                follow_up_iterations, MAX_FOLLOW_UP_ITERATIONS);
            /* Note: Don't send transcript - streaming already delivered it */

            char *next_processed = webui_process_commands(processed, session);
            free(processed);
            if (!next_processed) {
               processed = NULL;
               break;
            }
            processed = next_processed;
         }

         if (processed) {
            free(response);
            final_response = processed;

            /* Generate TTS for the command result (follow-up response) */
            if (tts_enabled && strlen(processed) > 0) {
               OLOG_INFO("WebUI: Generating TTS for command result: %.60s%s", processed,
                         strlen(processed) > 60 ? "..." : "");
               webui_sentence_audio_callback(processed, session);
            }
         } else {
            /* Command processing failed, use original response */
            final_response = response;
         }
      }
   }

   /* Strip any remaining command tags from final response */
   strip_command_tags(final_response);

   /* Send audio end marker if TTS was enabled */
   if (tts_enabled) {
      bool use_opus = conn && conn->use_opus;
      webui_send_audio_end(session, use_opus);
   }

   /* Note: Don't send transcript here - streaming already delivered the content.
    * The LLM call uses webui_send_stream_start/delta/end for real-time delivery.
    * Assistant response is already added to history inside the LLM call. */

   /* Free the final response (either original response or processed copy) */
   free(final_response);

   /* Send context usage update to WebUI */
   {
      int current_tokens, max_tokens;
      float threshold;
      llm_context_get_last_usage(&current_tokens, &max_tokens, &threshold);
      if (max_tokens > 0) {
         webui_send_context(session, current_tokens, max_tokens, threshold);
      }
   }

   /* Return to idle state */
   webui_send_state(session, "idle");

   /* Mark interaction complete for conversation idle timeout tracking */
   session_update_interaction_complete(session);

   /* Release session reference (acquired in webui_process_text_input) */
   session_release(session);

   free(text);
   free(work);
   return NULL;
}

/**
 * @brief Process text input with optional vision images (internal)
 *
 * @param session Session context
 * @param text User text input
 * @param vision_images Array of base64 encoded image data (NULL for text-only)
 * @param vision_image_sizes Array of image sizes
 * @param vision_mimes Array of MIME type strings
 * @param vision_image_count Number of images (0 for text-only)
 * @return 0 on success, non-zero on failure
 */
int webui_process_text_input_with_vision(session_t *session,
                                         const char *text,
                                         const char **vision_images,
                                         const size_t *vision_image_sizes,
                                         const char **vision_mimes,
                                         int vision_image_count) {
   if (!session || !text || strlen(text) == 0) {
      return 1;
   }

   /* Validate image count */
   const int max_vision_images = g_config.vision.max_images;
   if (vision_image_count > max_vision_images) {
      OLOG_WARNING("WebUI: Too many images (%d), limiting to %d", vision_image_count,
                   max_vision_images);
      vision_image_count = max_vision_images;
   }

   /* Increment request generation FIRST to invalidate any pending requests,
    * then reset disconnected flag. This prevents race conditions where an
    * old worker sees disconnected=false after a new message is sent. */
   unsigned int new_gen = atomic_fetch_add(&session->request_generation, 1) + 1;
   session->disconnected = false;

   /* Create work item */
   text_work_t *work = calloc(1, sizeof(text_work_t));
   if (!work) {
      OLOG_ERROR("WebUI: Failed to allocate text work item");
      return 1;
   }

   work->session = session;
   work->text = strdup(text);
   work->request_gen = new_gen; /* Capture generation for this request */
   if (!work->text) {
      OLOG_ERROR("WebUI: Failed to allocate text copy");
      free(work);
      return 1;
   }

   /* Copy vision images if present */
   if (vision_images && vision_image_count > 0) {
      for (int i = 0; i < vision_image_count; i++) {
         if (!vision_images[i] || vision_image_sizes[i] == 0) {
            continue;
         }

         work->vision_images[work->vision_image_count] = strdup(vision_images[i]);
         if (!work->vision_images[work->vision_image_count]) {
            OLOG_ERROR("WebUI: Failed to allocate vision image %d copy", i);
            /* Free previously allocated images */
            for (int j = 0; j < work->vision_image_count; j++) {
               free(work->vision_images[j]);
            }
            free(work->text);
            free(work);
            return 1;
         }
         work->vision_image_sizes[work->vision_image_count] = vision_image_sizes[i];
         if (vision_mimes && vision_mimes[i]) {
            strncpy(work->vision_mimes[work->vision_image_count], vision_mimes[i],
                    WEBUI_VISION_MIME_MAX - 1);
            work->vision_mimes[work->vision_image_count][WEBUI_VISION_MIME_MAX - 1] = '\0';
         }
         work->vision_image_count++;
      }
   }

   /* Retain session for worker thread (worker will release when done) */
   session_retain(session);

   /* Spawn detached worker thread */
   pthread_t thread;
   pthread_attr_t attr;
   pthread_attr_init(&attr);
   pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

   int ret = pthread_create(&thread, &attr, text_worker_thread, work);
   pthread_attr_destroy(&attr);

   if (ret != 0) {
      OLOG_ERROR("WebUI: Failed to create text worker thread");
      session_release(session);
      free(work->text);
      for (int i = 0; i < work->vision_image_count; i++) {
         free(work->vision_images[i]);
      }
      free(work);
      return 1;
   }

   return 0;
}

int webui_process_text_input(session_t *session, const char *text) {
   return webui_process_text_input_with_vision(session, text, NULL, NULL, NULL, 0);
}

/* =============================================================================
 * Force Logout (for session revocation)
 *
 * Finds all WebSocket connections with matching auth_session_token prefix
 * and sends them a force_logout message.
 * ============================================================================= */

int webui_force_logout_by_auth_token(const char *auth_token_prefix) {
   if (!auth_token_prefix || strlen(auth_token_prefix) < AUTH_TOKEN_PREFIX_LEN) {
      return 0;
   }

   int count = 0;
   pthread_mutex_lock(&s_conn_registry_mutex);

   for (int i = 0; i < MAX_ACTIVE_CONNECTIONS; i++) {
      ws_connection_t *conn = s_active_connections[i];
      if (conn && conn->wsi && conn->authenticated) {
         /* Check if auth token prefix matches */
         if (strncmp(conn->auth_session_token, auth_token_prefix, AUTH_TOKEN_PREFIX_LEN) == 0) {
            OLOG_INFO("WebUI: Forcing logout for connection with auth token %.4s...",
                      auth_token_prefix);
            send_force_logout_impl(conn->wsi, "Session revoked");
            /* Mark as unauthenticated to prevent further requests */
            conn->authenticated = false;
            count++;
         }
      }
   }

   pthread_mutex_unlock(&s_conn_registry_mutex);

   if (count > 0) {
      OLOG_INFO("WebUI: Sent force_logout to %d connection(s)", count);
   }

   return count;
}

int webui_destroy_sessions_by_auth_token(const char *auth_token_prefix) {
   if (!auth_token_prefix || strlen(auth_token_prefix) < AUTH_TOKEN_PREFIX_LEN)
      return 0;

   /* Collect matching session IDs under the conn mutex, then destroy after
    * releasing (session_destroy blocks on ref_count and itself calls
    * webui_detach_session → s_conn_registry_mutex; recursive would deadlock). */
   uint32_t ids[MAX_ACTIVE_CONNECTIONS];
   int n = 0;

   pthread_mutex_lock(&s_conn_registry_mutex);
   for (int i = 0; i < MAX_ACTIVE_CONNECTIONS; i++) {
      ws_connection_t *conn = s_active_connections[i];
      if (!conn || !conn->session)
         continue;
      if (strncmp(conn->auth_session_token, auth_token_prefix, AUTH_TOKEN_PREFIX_LEN) != 0)
         continue;
      /* Skip satellites — their session lifecycle is managed by the satellite
       * protocol, not the auth cookie. */
      if (conn->is_satellite)
         continue;
      uint32_t sid = conn->session->session_id;
      bool dup = false;
      for (int j = 0; j < n; j++) {
         if (ids[j] == sid) {
            dup = true;
            break;
         }
      }
      if (!dup && n < MAX_ACTIVE_CONNECTIONS)
         ids[n++] = sid;
   }
   pthread_mutex_unlock(&s_conn_registry_mutex);

   for (int i = 0; i < n; i++) {
      session_destroy(ids[i]);
   }

   if (n > 0)
      OLOG_INFO("WebUI: Destroyed %d session slot(s) for auth token %.4s...", n, auth_token_prefix);

   return n;
}

/* =============================================================================
 * Scheduler Notification Broadcast
 * ============================================================================= */

/**
 * @brief Deliver queued missed notifications to a newly-authenticated connection.
 *
 * Called after cookie-based authentication completes at WebSocket open. Reads
 * up to 32 queued notifications for the user and emits them as regular
 * scheduler_notification messages with a "missed" flag. Entries remain in the
 * DB until the user explicitly dismisses them (dismiss_missed action), so
 * reloading the page or opening another tab shows the same set.
 */
static void deliver_missed_notifications(ws_connection_t *conn) {
   if (!conn || !conn->session || !conn->authenticated || conn->auth_user_id <= 0)
      return;

   missed_notif_t missed[MISSED_NOTIF_DELIVERY_BATCH];
   int count = 0;
   if (missed_notif_get_for_user(conn->auth_user_id, MISSED_NOTIF_DELIVERY_BATCH, missed, &count) !=
           AUTH_DB_SUCCESS ||
       count <= 0)
      return;

   for (int i = 0; i < count; i++) {
      json_object *root = json_object_new_object();
      json_object_object_add(root, "type", json_object_new_string("scheduler_notification"));

      json_object *payload = json_object_new_object();
      json_object_object_add(payload, "event_id", json_object_new_int64(missed[i].event_id));
      json_object_object_add(payload, "event_type", json_object_new_string(missed[i].event_type));
      json_object_object_add(payload, "status", json_object_new_string(missed[i].status));
      json_object_object_add(payload, "name", json_object_new_string(missed[i].name));
      json_object_object_add(payload, "message", json_object_new_string(missed[i].message));
      json_object_object_add(payload, "fire_at", json_object_new_int64((int64_t)missed[i].fire_at));
      json_object_object_add(payload, "missed", json_object_new_boolean(true));
      json_object_object_add(payload, "missed_notif_id", json_object_new_int64(missed[i].id));
      if (missed[i].conversation_id > 0) {
         json_object_object_add(payload, "conversation_id",
                                json_object_new_int64(missed[i].conversation_id));
      }

      json_object_object_add(root, "payload", payload);
      const char *json_str = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN);
      char *json_copy = json_str ? strdup(json_str) : NULL;
      json_object_put(root);

      if (!json_copy)
         continue;

      ws_response_t resp = { .session = conn->session,
                             .type = WS_RESP_SCHEDULER_NOTIFICATION,
                             .scheduler_json = { .json = json_copy } };
      queue_response(&resp);
   }

   OLOG_INFO("WebUI: Delivered %d missed notification(s) to user %d", count, conn->auth_user_id);
}

/**
 * @brief Broadcast scheduler notification to all authenticated WebUI connections
 *
 * Strong symbol that overrides the weak stub in scheduler.c.
 * Queues a response per connected browser client. Satellites are notified
 * separately via satellite_send_response in the scheduler fire logic.
 */
void scheduler_broadcast_notification(const sched_event_t *event, const char *text) {
   if (!event || !text)
      return;

   /* Build JSON notification */
   json_object *root = json_object_new_object();
   json_object_object_add(root, "type", json_object_new_string("scheduler_notification"));

   json_object *payload = json_object_new_object();
   json_object_object_add(payload, "event_id", json_object_new_int64(event->id));
   json_object_object_add(payload, "event_type",
                          json_object_new_string(sched_event_type_to_str(event->event_type)));
   json_object_object_add(payload, "status",
                          json_object_new_string(sched_status_to_str(event->status)));
   json_object_object_add(payload, "name", json_object_new_string(event->name));
   json_object_object_add(payload, "message", json_object_new_string(text));
   json_object_object_add(payload, "fire_at", json_object_new_int64((int64_t)event->fire_at));

   /* Tell WebUI clients whether TTS audio is being routed to them (skip chime if so) */
   bool tts_routed = (event->source_client_type == SCHED_SOURCE_WEBUI);
   json_object_object_add(payload, "tts_routed", json_object_new_boolean(tts_routed));

   json_object_object_add(root, "payload", payload);
   const char *json_str = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN);

   /* Queue a response for matching clients.
    * If event has a user_id > 0, only notify that user's connections.
    * If user_id == 0 (system event), broadcast to all authenticated connections. */
   int sent = 0;
   int target_user = event->user_id;
   pthread_mutex_lock(&s_conn_registry_mutex);
   for (int i = 0; i < MAX_ACTIVE_CONNECTIONS; i++) {
      ws_connection_t *conn = s_active_connections[i];
      if (!conn || !conn->session)
         continue;

      /* Skip unauthenticated non-satellite connections.
       * Note: unassigned satellites (user_id=0) receive system-wide events (target_user=0)
       * intentionally, so all devices get broadcast alerts like alarms. */
      if (!conn->authenticated && !conn->is_satellite)
         continue;

      /* Filter by user if event is user-specific */
      if (target_user > 0 && conn->auth_user_id != target_user)
         continue;

      char *json_copy = strdup(json_str);
      if (!json_copy)
         continue;

      ws_response_t resp = { .session = conn->session,
                             .type = WS_RESP_SCHEDULER_NOTIFICATION,
                             .scheduler_json = { .json = json_copy } };
      queue_response(&resp);
      sent++;
   }

   /* Queue as missed notification when no clients were available. Decision is
    * made and the DB insert runs while the conn mutex is still held so a
    * concurrent reconnect cannot slip between "nobody online" and the insert.
    * Only user-specific ringing events are queued; system events (user_id == 0)
    * and transient rebroadcasts (dismiss/timeout) are dropped. */
   bool queued_missed = false;
   if (sent == 0 && target_user > 0 && event->status == SCHED_STATUS_RINGING) {
      missed_notif_insert(target_user, event->id, sched_event_type_to_str(event->event_type),
                          sched_status_to_str(event->status), event->name, text, event->fire_at, 0);
      queued_missed = true;
   }
   pthread_mutex_unlock(&s_conn_registry_mutex);

   if (sent > 0) {
      OLOG_INFO("Scheduler: Broadcast notification to %d client(s): %s", sent, text);
   } else if (queued_missed) {
      OLOG_INFO("Scheduler: No clients for user %d, queued missed: %s", target_user, text);
   } else {
      OLOG_INFO("Scheduler: no recipients for '%s' (target_user=%d, status=%s) — dropped", text,
                target_user, sched_status_to_str(event->status));
   }

   json_object_put(root);
}

void scheduler_broadcast_briefing_notification(const sched_event_t *event,
                                               const char *text,
                                               int64_t conversation_id) {
   if (!event || !text)
      return;

   json_object *root = json_object_new_object();
   json_object_object_add(root, "type", json_object_new_string("scheduler_notification"));

   json_object *payload = json_object_new_object();
   json_object_object_add(payload, "event_id", json_object_new_int64(event->id));
   json_object_object_add(payload, "event_type",
                          json_object_new_string(sched_event_type_to_str(event->event_type)));
   json_object_object_add(payload, "status",
                          json_object_new_string(sched_status_to_str(event->status)));
   json_object_object_add(payload, "name", json_object_new_string(event->name));

   /* Truncate preview to ~80 chars */
   char preview[84];
   size_t text_len = strlen(text);
   if (text_len > 80) {
      memcpy(preview, text, 80);
      memcpy(preview + 80, "...", 4);
   } else {
      memcpy(preview, text, text_len + 1);
   }
   json_object_object_add(payload, "message", json_object_new_string(preview));
   json_object_object_add(payload, "fire_at", json_object_new_int64((int64_t)event->fire_at));

   if (conversation_id > 0) {
      json_object_object_add(payload, "conversation_id", json_object_new_int64(conversation_id));
   }

   json_object_object_add(root, "payload", payload);
   const char *json_str = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN);

   int sent = 0;
   int target_user = event->user_id;
   pthread_mutex_lock(&s_conn_registry_mutex);
   for (int i = 0; i < MAX_ACTIVE_CONNECTIONS; i++) {
      ws_connection_t *conn = s_active_connections[i];
      if (!conn || !conn->session)
         continue;
      if (!conn->authenticated && !conn->is_satellite)
         continue;
      if (target_user > 0 && conn->auth_user_id != target_user)
         continue;

      char *json_copy = strdup(json_str);
      if (!json_copy)
         continue;

      ws_response_t resp = { .session = conn->session,
                             .type = WS_RESP_SCHEDULER_NOTIFICATION,
                             .scheduler_json = { .json = json_copy } };
      queue_response(&resp);
      sent++;
   }

   bool queued_missed = false;
   if (sent == 0 && target_user > 0 && event->status == SCHED_STATUS_RINGING) {
      missed_notif_insert(target_user, event->id, sched_event_type_to_str(event->event_type),
                          sched_status_to_str(event->status), event->name, preview, event->fire_at,
                          conversation_id);
      queued_missed = true;
   }
   pthread_mutex_unlock(&s_conn_registry_mutex);

   if (sent > 0) {
      OLOG_INFO("Scheduler: Broadcast briefing notification to %d client(s)", sent);
   } else if (queued_missed) {
      OLOG_INFO("Scheduler: No clients for user %d, queued missed briefing", target_user);
   } else {
      OLOG_INFO("Scheduler: no recipients for briefing (target_user=%d, status=%s) — dropped",
                target_user, sched_status_to_str(event->status));
   }

   json_object_put(root);
}

/* Forward declarations for scheduler TTS routing */
extern void satellite_send_response(session_t *session, const char *text);
extern void scheduler_send_tts_to_session(session_t *session, const char *text);

int scheduler_route_tts_to_user(int user_id,
                                const char *text,
                                const char *skip_uuid,
                                int skip_user_id) {
   if (!text || !text[0])
      return 0;

   int delivered = 0;
   session_t *best_webui = NULL;
   time_t best_activity = 0;

   pthread_mutex_lock(&s_conn_registry_mutex);
   for (int i = 0; i < MAX_ACTIVE_CONNECTIONS; i++) {
      ws_connection_t *conn = s_active_connections[i];
      if (!conn || !conn->session)
         continue;
      if (!conn->authenticated && !conn->is_satellite)
         continue;
      if (user_id > 0 && conn->auth_user_id != user_id)
         continue;
      /* Skip originating user's sessions (for announce_all dedup) */
      if (skip_user_id > 0 && conn->auth_user_id == skip_user_id)
         continue;

      session_t *s = conn->session;
      if (s->disconnected)
         continue;

      if (conn->is_satellite) {
         /* Satellite: send TTS text for local synthesis (Tier 1 only) */
         if (s->type == SESSION_TYPE_DAP2 && s->tier == DAP2_TIER_1) {
            if (skip_uuid && skip_uuid[0] && strcmp(s->identity.uuid, skip_uuid) == 0)
               continue;
            satellite_send_response(s, text);
            delivered++;
            OLOG_INFO("scheduler: TTS routed to satellite %s for user %d", s->identity.uuid,
                      user_id);
         }
      } else {
         /* WebUI: pick the most recently active session to avoid multi-tab echo */
         if (!best_webui || s->last_activity > best_activity) {
            best_webui = s;
            best_activity = s->last_activity;
         }
      }
   }

   /* Retain best WebUI session before releasing mutex (prevents use-after-free
    * if the session disconnects between mutex release and TTS synthesis) */
   if (best_webui)
      session_retain(best_webui);

   pthread_mutex_unlock(&s_conn_registry_mutex);

   /* Send TTS to the best WebUI session (outside the mutex — TTS synthesis blocks) */
   if (best_webui) {
      scheduler_send_tts_to_session(best_webui, text);
      delivered++;
      OLOG_INFO("scheduler: TTS routed to WebUI session %u for user %d", best_webui->session_id,
                user_id);
      session_release(best_webui);
   }

   return delivered;
}

/* =============================================================================
 * Conversation Title Broadcast (called from memory extraction thread)
 * ============================================================================= */

void webui_broadcast_conversation_renamed(int user_id, int64_t conv_id, const char *title) {
   if (user_id <= 0 || !title)
      return;

   json_object *root = json_object_new_object();
   json_object_object_add(root, "type", json_object_new_string("conversation_renamed"));

   json_object *payload = json_object_new_object();
   json_object_object_add(payload, "conversation_id", json_object_new_int64(conv_id));
   json_object_object_add(payload, "title", json_object_new_string(title));
   json_object_object_add(root, "payload", payload);

   const char *json_str = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN);

   int sent = 0;
   pthread_mutex_lock(&s_conn_registry_mutex);
   for (int i = 0; i < MAX_ACTIVE_CONNECTIONS; i++) {
      ws_connection_t *conn = s_active_connections[i];
      if (!conn || !conn->session || !conn->authenticated)
         continue;
      if (conn->auth_user_id != user_id)
         continue;

      char *json_copy = strdup(json_str);
      if (!json_copy)
         continue;

      ws_response_t resp = { .session = conn->session,
                             .type = WS_RESP_JSON,
                             .generic_json = { .json = json_copy } };
      queue_response(&resp);
      sent++;
   }
   pthread_mutex_unlock(&s_conn_registry_mutex);

   json_object_put(root);

   if (sent > 0) {
      OLOG_INFO("WebUI: Broadcast conversation rename to %d client(s): %s", sent, title);
   }
}

/* =============================================================================
 * Memory Extraction Notice Broadcast
 * ============================================================================= */

void webui_broadcast_memory_notice(int user_id, const char *level, const char *message) {
   if (user_id <= 0 || !level || !message)
      return;

   json_object *root = json_object_new_object();
   json_object_object_add(root, "type", json_object_new_string("memory_extraction_notice"));

   json_object *payload = json_object_new_object();
   json_object_object_add(payload, "level", json_object_new_string(level));
   json_object_object_add(payload, "message", json_object_new_string(message));
   json_object_object_add(root, "payload", payload);

   const char *json_str = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN);

   int sent = 0;
   pthread_mutex_lock(&s_conn_registry_mutex);
   for (int i = 0; i < MAX_ACTIVE_CONNECTIONS; i++) {
      ws_connection_t *conn = s_active_connections[i];
      if (!conn || !conn->session || !conn->authenticated)
         continue;
      if (conn->auth_user_id != user_id)
         continue;

      char *json_copy = strdup(json_str);
      if (!json_copy)
         continue;

      ws_response_t resp = { .session = conn->session,
                             .type = WS_RESP_JSON,
                             .generic_json = { .json = json_copy } };
      queue_response(&resp);
      sent++;
   }
   pthread_mutex_unlock(&s_conn_registry_mutex);

   json_object_put(root);

   if (sent > 0) {
      OLOG_INFO("WebUI: Broadcast memory notice (%s) to %d client(s)", level, sent);
   }
}

/* =============================================================================
 * Connection Iterator (for per-user broadcasting)
 * ============================================================================= */

void webui_for_each_conn_by_user(int user_id,
                                 void (*callback)(ws_connection_t *conn, void *ctx),
                                 void *ctx) {
   if (user_id <= 0 || !callback) {
      return;
   }

   pthread_mutex_lock(&s_conn_registry_mutex);
   for (int i = 0; i < MAX_ACTIVE_CONNECTIONS; i++) {
      ws_connection_t *conn = s_active_connections[i];
      if (conn && conn->authenticated && conn->auth_user_id == user_id && conn->music_state) {
         callback(conn, ctx);
      }
   }
   pthread_mutex_unlock(&s_conn_registry_mutex);
}

int webui_collect_conns_by_user(int user_id, ws_connection_t **out, int max_out) {
   if (user_id <= 0 || !out || max_out <= 0) {
      return 0;
   }

   int count = 0;
   pthread_mutex_lock(&s_conn_registry_mutex);
   for (int i = 0; i < MAX_ACTIVE_CONNECTIONS; i++) {
      ws_connection_t *conn = s_active_connections[i];
      if (conn && conn->authenticated && conn->auth_user_id == user_id && conn->music_state) {
         if (count < max_out) {
            out[count] = conn;
         }
         count++;
      }
   }
   pthread_mutex_unlock(&s_conn_registry_mutex);
   return count;
}

/* =============================================================================
 * Satellite Connection Helpers
 * ============================================================================= */

bool webui_is_satellite_online(const char *uuid) {
   if (!uuid)
      return false;

   bool online = false;
   pthread_mutex_lock(&s_conn_registry_mutex);
   for (int i = 0; i < MAX_ACTIVE_CONNECTIONS; i++) {
      ws_connection_t *conn = s_active_connections[i];
      if (conn && conn->is_satellite && conn->session &&
          strcmp(conn->session->identity.uuid, uuid) == 0) {
         online = true;
         break;
      }
   }
   pthread_mutex_unlock(&s_conn_registry_mutex);
   return online;
}

/* NOTE: Must be called from the LWS service thread (admin handlers run within
 * LWS callbacks, so current callers are safe). Do NOT call from worker threads. */
void webui_force_disconnect_satellite(const char *uuid) {
   if (!uuid)
      return;

   pthread_mutex_lock(&s_conn_registry_mutex);
   for (int i = 0; i < MAX_ACTIVE_CONNECTIONS; i++) {
      ws_connection_t *conn = s_active_connections[i];
      if (conn && conn->is_satellite && conn->session && conn->wsi &&
          strcmp(conn->session->identity.uuid, uuid) == 0) {
         OLOG_INFO("WebUI: Force-disconnecting satellite %s (disabled by admin)", uuid);
         lws_close_reason(conn->wsi, LWS_CLOSE_STATUS_POLICY_VIOLATION, (unsigned char *)"disabled",
                          8);
         lws_set_timeout(conn->wsi, PENDING_TIMEOUT_CLOSE_SEND, 3);
         break;
      }
   }
   pthread_mutex_unlock(&s_conn_registry_mutex);
}

/* =============================================================================
 * JSON Message Handler Implementation
 * ============================================================================= */

/**
 * @brief Message callback for session context restoration.
 * Builds JSON objects with role + content for iterating into session_add_message.
 */
static int webui_session_restore_msg_cb(const conversation_message_t *msg, void *context) {
   json_object *arr = (json_object *)context;
   json_object *obj = json_object_new_object();
   json_object_object_add(obj, "role", json_object_new_string(msg->role));
   json_object_object_add(obj, "content", json_object_new_string(msg->content ? msg->content : ""));
   json_object_array_add(arr, obj);
   return 0;
}

/**
 * @brief Restore conversation context into a session from DB
 *
 * Shared implementation used by both session expiry recovery and sidebar load.
 */
int webui_restore_conversation_context(ws_connection_t *conn,
                                       const conversation_t *conv,
                                       int64_t conv_id,
                                       json_object *preloaded_msgs) {
   json_object *all_msgs = NULL;
   bool owns_msgs = false;

   if (preloaded_msgs) {
      all_msgs = preloaded_msgs;
   } else {
      all_msgs = json_object_new_array();
      owns_msgs = true;
      int rc = conv_db_get_messages(conv_id, conn->auth_user_id, webui_session_restore_msg_cb,
                                    all_msgs);
      if (rc != AUTH_DB_SUCCESS) {
         json_object_put(all_msgs);
         return LWS_CLOSE_CONNECTION;
      }
   }

   int count = json_object_array_length(all_msgs);

   /* Check if stored messages include a system prompt */
   bool has_system = false;
   if (count > 0) {
      json_object *first = json_object_array_get_idx(all_msgs, 0);
      json_object *role_obj;
      if (json_object_object_get_ex(first, "role", &role_obj)) {
         const char *role = json_object_get_string(role_obj);
         if (role && strcmp(role, "system") == 0)
            has_system = true;
      }
   }

   session_clear_history(conn->session);

   if (!has_system) {
      char *prompt = build_user_prompt(conn->auth_user_id);
      session_add_message(conn->session, "system", prompt ? prompt : get_remote_command_prompt());
      free(prompt);
   }

   if (conv->compaction_summary && strlen(conv->compaction_summary) > 0) {
      char summary[4096];
      snprintf(summary, sizeof(summary), "Previous conversation context (summarized): %s",
               conv->compaction_summary);
      session_add_message(conn->session, "system", summary);
   }

   for (int i = 0; i < count; i++) {
      json_object *msg = json_object_array_get_idx(all_msgs, i);
      json_object *role_obj, *content_obj;
      if (json_object_object_get_ex(msg, "role", &role_obj) &&
          json_object_object_get_ex(msg, "content", &content_obj)) {
         session_add_message(conn->session, json_object_get_string(role_obj),
                             json_object_get_string(content_obj));
      }
   }

   /* Restore LLM config from conversation DB (reflects last-used settings) */
   if (conv->llm_type[0] != '\0' || conv->tools_mode[0] != '\0') {
      session_llm_config_t cfg;
      session_get_llm_config(conn->session, &cfg);

      if (conv->llm_type[0] != '\0') {
         if (strcmp(conv->llm_type, "local") == 0)
            cfg.type = LLM_LOCAL;
         else if (strcmp(conv->llm_type, "cloud") == 0)
            cfg.type = LLM_CLOUD;
      }
      if (conv->cloud_provider[0] != '\0') {
         if (strcmp(conv->cloud_provider, "openai") == 0)
            cfg.cloud_provider = CLOUD_PROVIDER_OPENAI;
         else if (strcmp(conv->cloud_provider, "claude") == 0)
            cfg.cloud_provider = CLOUD_PROVIDER_CLAUDE;
         else if (strcmp(conv->cloud_provider, "gemini") == 0)
            cfg.cloud_provider = CLOUD_PROVIDER_GEMINI;
      }
      if (conv->model[0] != '\0') {
         strncpy(cfg.model, conv->model, sizeof(cfg.model) - 1);
         cfg.model[sizeof(cfg.model) - 1] = '\0';

         /* Infer provider from model name if not explicitly stored */
         if (conv->cloud_provider[0] == '\0') {
            if (strncmp(conv->model, "gpt-", 4) == 0 || strncmp(conv->model, "o1-", 3) == 0 ||
                strncmp(conv->model, "o3-", 3) == 0) {
               cfg.cloud_provider = CLOUD_PROVIDER_OPENAI;
            } else if (strncmp(conv->model, "claude-", 7) == 0) {
               cfg.cloud_provider = CLOUD_PROVIDER_CLAUDE;
            } else if (strncmp(conv->model, "gemini-", 7) == 0) {
               cfg.cloud_provider = CLOUD_PROVIDER_GEMINI;
            }
         }
      }
      if (conv->tools_mode[0] != '\0') {
         strncpy(cfg.tool_mode, conv->tools_mode, sizeof(cfg.tool_mode) - 1);
         cfg.tool_mode[sizeof(cfg.tool_mode) - 1] = '\0';
      }
      /* Fix #6: Restore thinking_mode from conversation DB */
      if (conv->thinking_mode[0] != '\0') {
         strncpy(cfg.thinking_mode, conv->thinking_mode, sizeof(cfg.thinking_mode) - 1);
         cfg.thinking_mode[sizeof(cfg.thinking_mode) - 1] = '\0';
      }
      session_set_llm_config(conn->session, &cfg);
   }

   if (owns_msgs)
      json_object_put(all_msgs);
   return count;
}

/**
 * @brief Create a new conversation session for an authenticated connection.
 *
 * Called when conn->session is NULL (e.g., after session expiry) but the user
 * is still authenticated. Creates a fresh session so the user can keep working
 * without logging in again. If the connection had an active conversation, its
 * messages and LLM config are silently restored from the DB.
 *
 * @return true if session was created, false on failure (error sent to client)
 */
static bool webui_conn_create_session(ws_connection_t *conn) {
   conn->session = session_create(SESSION_TYPE_WEBUI, -1);
   if (!conn->session) {
      send_error_impl(conn->wsi, "SESSION_LIMIT", "Maximum sessions reached");
      return false;
   }

   session_set_metrics_user(conn->session, conn->auth_user_id);
   conn->session->client_data = conn;

   /* Re-enable missed-notification replay on the new session. Any notifications
    * that arrived during the conversation-session-expired window would have
    * been queued as missed (no attached session), so re-delivery is needed. */
   conn->missed_notif_delivered = false;

   /* Generate and register a new session token */
   if (generate_session_token(conn->session_token) != 0) {
      OLOG_ERROR("WebUI: Failed to generate session token");
      session_destroy(conn->session->session_id);
      conn->session = NULL;
      return false;
   }
   register_token(conn->session_token, conn->session->session_id);

   OLOG_INFO("WebUI: Auto-created session %u for connection (user %d, token %.4s...)",
             conn->session->session_id, conn->auth_user_id, conn->session_token);

   /* Send new session token and init messages so the client updates seamlessly */
   queue_init_messages(conn, conn->session_token);

   /* Restore the active conversation's context into the new session.
    * Uses the shared helper (same logic as sidebar load, but without
    * sending a UI response — the browser still has the messages displayed). */
   bool restored = false;
   if (conn->active_conversation_id > 0) {
      conversation_t conv;
      int rc = conv_db_get(conn->active_conversation_id, conn->auth_user_id, &conv);
      if (rc == AUTH_DB_SUCCESS) {
         if (!conv.is_archived) {
            int count = webui_restore_conversation_context(conn, &conv,
                                                           conn->active_conversation_id, NULL);
            if (count >= 0) {
               restored = true;
               OLOG_INFO("WebUI: Restored conversation %lld (%d messages) into new session %u",
                         (long long)conn->active_conversation_id, count, conn->session->session_id);
            }
         }
         conv_free(&conv);
      }
   }

   /* If no conversation was restored, initialize with the user's system prompt */
   if (!restored) {
      char *prompt = build_user_prompt(conn->auth_user_id);
      session_init_system_prompt(conn->session, prompt ? prompt : get_remote_command_prompt());
      free(prompt);
   }

   return true;
}

static void handle_text_message(ws_connection_t *conn,
                                const char *text,
                                size_t len,
                                const char **vision_images,
                                const size_t *vision_image_sizes,
                                const char **vision_mimes,
                                int vision_image_count) {
   (void)len; /* Length already validated by caller */

   /* SECURITY: Require authentication for text processing */
   if (!conn_require_auth(conn)) {
      return; /* conn_require_auth already sent error */
   }

   /* Auto-create session if expired (user is still authenticated).
    * Atomic load: maintenance thread may have NULLed conn->session. */
   if (!conn_get_session(conn)) {
      OLOG_INFO("WebUI: Session expired for authenticated connection, creating new session");
      if (!webui_conn_create_session(conn)) {
         return;
      }
   }

   if (vision_image_count > 0) {
      size_t total_bytes = 0;
      for (int i = 0; i < vision_image_count; i++) {
         total_bytes += vision_image_sizes[i];
      }
      OLOG_INFO("WebUI: Text+Vision input from session %u: %s (%d images, %zu total bytes)",
                conn->session->session_id, text, vision_image_count, total_bytes);
   } else {
      OLOG_INFO("WebUI: Text input from session %u: %s", conn->session->session_id, text);
   }

   int ret = webui_process_text_input_with_vision(conn->session, text, vision_images,
                                                  vision_image_sizes, vision_mimes,
                                                  vision_image_count);
   if (ret != 0) {
      send_error_impl(conn->wsi, "PROCESSING_ERROR", "Failed to process text input");
   }
}

/* Audio handlers: handle_binary_message, audio_worker_thread, webui_sentence_audio_callback
 * moved to webui_audio.c (declaration in webui_internal.h) */
