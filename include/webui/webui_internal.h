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
 * WebUI Internal Header - Shared state and helpers for webui_*.c modules
 *
 * This header is NOT part of the public API. It exposes internal state
 * and helper functions shared between webui_server.c and split handler
 * modules (webui_http.c, webui_admin.c, webui_history.c, etc.).
 *
 * All modules including this header MUST be compiled into the same binary.
 * Do not expose this header to external code.
 */

#ifndef WEBUI_INTERNAL_H
#define WEBUI_INTERNAL_H

#include <json-c/json.h>
#include <libwebsockets.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "auth/auth_db.h"
#include "core/session_manager.h"
#include "webui/webui_server.h"

/* =============================================================================
 * Request Supersession Macro (used by worker threads)
 * ============================================================================= */

/**
 * @brief Check if a request has been superseded by a newer one
 *
 * A request is superseded if:
 * 1. The session was disconnected (user closed connection or clicked stop)
 * 2. A newer request was initiated (user sent new message before old completed)
 *
 * Workers should check this before and after long operations (LLM calls, etc.)
 * to avoid processing stale requests.
 *
 * @param session Pointer to session_t
 * @param expected_gen The request_generation captured when work was queued
 * @return true if request should be aborted, false if still valid
 */
#define REQUEST_SUPERSEDED(session, expected_gen) \
   (atomic_load(&(session)->disconnected) ||      \
    atomic_load(&(session)->request_generation) != (expected_gen))

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Internal Constants
 * ============================================================================= */

#define WS_SEND_BUFFER_SIZE 16384
#define HTTP_MAX_POST_BODY 4096
#define AUTH_COOKIE_NAME "dawn_session"
#define AUTH_COOKIE_MAX_AGE (24 * 60 * 60) /* 24 hours */
#define MAX_TOKEN_MAPPINGS 16
#define MODEL_CACHE_TTL 60 /* Cache refresh interval in seconds */

/* WebSocket text buffer limits */
#define WEBUI_TEXT_BUFFER_INITIAL_CAP 8192
#define WEBUI_TEXT_BUFFER_MAX_CAP \
   (8 * 1024 * 1024) /* 8MB for vision (4MB image + base64 overhead) */

/* =============================================================================
 * Per-WebSocket Connection Data
 * ============================================================================= */

typedef struct {
   struct lws *wsi;                             /* libwebsockets handle */
   session_t *session;                          /* Session manager reference (use
                                                 * conn_get_session/conn_set_session for
                                                 * cross-thread access) */
   char session_token[WEBUI_SESSION_TOKEN_LEN]; /* Reconnection token */
   uint8_t *audio_buffer;                       /* Opus audio accumulation */
   size_t audio_buffer_len;
   size_t audio_buffer_capacity;
   bool in_binary_fragment; /* True if receiving fragmented binary frame */
   uint8_t binary_msg_type; /* Message type from first fragment */
   _Atomic bool
       use_opus; /* True if client supports Opus codec (atomic: set by LWS, read by worker) */
   _Atomic bool tts_enabled; /* True if TTS output enabled (atomic: set by LWS, read by worker) */
   bool is_satellite;        /* True if this is a DAP2 satellite connection */

   /* Text message fragmentation support (for large JSON payloads) */
   char *text_buffer;      /* Accumulation buffer for fragmented text messages */
   size_t text_buffer_len; /* Current data length in text_buffer */
   size_t text_buffer_cap; /* Allocated capacity of text_buffer */

   /* Auth state (populated at WebSocket establishment from HTTP cookie) */
   bool authenticated;
   int auth_user_id;
   char auth_session_token[AUTH_TOKEN_LEN]; /* For DB re-validation */
   char username[AUTH_USERNAME_MAX];
   /* Note: is_admin NOT cached - re-validated from DB on each admin operation */

   /* Missed notification delivery: set once queued replay has been pushed to the
    * client so the delivery only happens on the first ready message per connection. */
   bool missed_notif_delivered;

   /* Client IP address (captured at connection establishment for reliable logging) */
   char client_ip[64];

   /* Client counting: true once this connection has incremented s_client_count.
    * Ensures balanced decrement even if session is detached before disconnect. */
   bool counted;

   /* Active conversation tracking (for memory extraction on switch) */
   int64_t active_conversation_id;
   bool active_conversation_private; /* If true, skip memory extraction */

   /* Music streaming state (per-session, owned by webui_music.c) */
   void *music_state; /* session_music_state_t*, NULL if not initialized */

   /* Per-session music volume (0.0-1.0), synced with client.
    * Stored on connection (not music_state) because volume must be available
    * before music_subscribe and is an audio property of the connection.
    * Atomic: written by LLM tool thread and LWS thread, read by music stream thread. */
   _Atomic float volume;

   /* Always-on voice mode (per-connection, allocated on enable, freed on disable/disconnect).
    * NULL when always-on is not active. Owns per-connection VAD context, Opus decoder,
    * resampler, and circular audio buffer. */
   struct always_on_ctx *always_on;

   /* TTS audio pacing state (pacing fields written only from LLM worker thread,
    * not accessed by scheduler or LWS thread — no synchronization needed).
    * Note: ws_connection_t memory is owned by lws and remains valid until
    * server shutdown (not freed per-connection), so pointer access during
    * sleep is safe against use-after-free. */
   uint64_t tts_pace_start_us;  /* CLOCK_MONOTONIC when first audio sent (0 = not started) */
   uint64_t tts_audio_sent_us;  /* Cumulative audio duration sent (microseconds) */
   uint32_t tts_pace_stream_id; /* Stream ID for reset detection */
} ws_connection_t;

/**
 * @brief Atomically load conn->session (acquire semantics)
 *
 * Use at cross-thread boundaries where the maintenance thread may have
 * set conn->session to NULL via webui_detach_session(). Within the
 * single-threaded LWS callback context, direct conn->session access is fine
 * after an initial atomic load confirms non-NULL.
 */
static inline session_t *conn_get_session(ws_connection_t *conn) {
   return __atomic_load_n(&conn->session, __ATOMIC_ACQUIRE);
}

/**
 * @brief Atomically store conn->session (release semantics)
 *
 * Use when writing conn->session from a non-LWS thread (e.g., maintenance
 * thread in webui_detach_session). Pairs with conn_get_session().
 */
static inline void conn_set_session(ws_connection_t *conn, session_t *s) {
   __atomic_store_n(&conn->session, s, __ATOMIC_RELEASE);
}

/* =============================================================================
 * HTTP Session Data
 * ============================================================================= */

/* Forward declarations for upload sessions */
struct http_image_session;
struct document_upload_session;

struct http_session_data {
   char path[256]; /* Request path */
   char post_body[HTTP_MAX_POST_BODY];
   size_t post_body_len;
   bool is_post;
   struct http_image_session *image_session;         /* For image uploads (NULL if not image) */
   struct document_upload_session *document_session; /* For doc uploads (NULL if not doc) */
   /* Dynamic body buffer for large POST endpoints (e.g., /api/documents/summarize) */
   char *large_body;      /* Dynamically allocated body (NULL if using post_body) */
   size_t large_body_len; /* Current length */
   size_t large_body_cap; /* Allocated capacity */
};

/* =============================================================================
 * Response Queue (worker -> WebUI thread)
 * ============================================================================= */

typedef struct {
   session_t *session;
   ws_response_type_t type;
   union {
      struct {
         char *state;
         char *detail;     /* Optional detail message */
         char *tools_json; /* Optional JSON array of active tools */
      } state;
      struct {
         char *role;
         char *text;
         bool server_saved; /* Message already persisted to DB server-side */
      } transcript;
      struct {
         char *code;
         char *message;
      } error;
      struct {
         char *token;
      } session_token;
      struct {
         uint8_t *data;
         size_t len;
         bool is_opus; /* Codec used for this segment (for AUDIO_END marker) */
      } audio;
      struct {
         int current_tokens;
         int max_tokens;
         float threshold;
      } context;
      struct {
         uint32_t stream_id;
         char text[1024]; /* Buffer for delta/end text (increased for thinking) */
      } stream;
      struct {
         char state[16];   /* idle, listening, thinking, speaking, error */
         int ttft_ms;      /* Time to first token (ms) */
         float token_rate; /* Tokens per second */
         int context_pct;  /* Context utilization 0-100 */
      } metrics;
      struct {
         int tokens_before;
         int tokens_after;
         int messages_summarized;
         int level;
         char *summary;
      } compaction;
      struct {
         double position_sec;
         uint32_t duration_sec;
      } music_position;
      struct {
         char *json; /* Pre-serialized JSON string (heap-allocated) */
      } music_json;
      struct {
         char *json; /* Pre-serialized scheduler notification JSON */
      } scheduler_json;
      struct {
         char *json; /* Pre-serialized generic JSON (heap-allocated) */
      } generic_json;
   };
} ws_response_t;

/* =============================================================================
 * Token-to-Session Mapping
 * ============================================================================= */

typedef struct {
   char token[WEBUI_SESSION_TOKEN_LEN];
   uint32_t session_id;
   time_t created;
   bool in_use;
} token_mapping_t;

/* =============================================================================
 * Discovery Cache (for model/interface scanning)
 * ============================================================================= */

typedef struct {
   struct json_object *models_response;     /* Cached list_models_response */
   struct json_object *interfaces_response; /* Cached list_interfaces_response */
   time_t models_cache_time;                /* When models were last scanned */
   time_t interfaces_cache_time;            /* When interfaces were last enumerated */
   pthread_mutex_t cache_mutex;             /* Protects cache access */
} discovery_cache_t;

/* =============================================================================
 * Extern Declarations for Module State (defined in webui_server.c)
 * ============================================================================= */

extern struct lws_context *s_lws_context;
extern volatile int s_running;
extern volatile int s_client_count;
extern int s_port;
extern char s_www_path[256];
extern pthread_mutex_t s_mutex;
extern pthread_rwlock_t s_config_rwlock;

/* Response queue */
extern ws_response_t s_response_queue[WEBUI_RESPONSE_QUEUE_SIZE];
extern int s_queue_head;
extern int s_queue_tail;
extern pthread_mutex_t s_queue_mutex;

/* Token mapping */
extern token_mapping_t s_token_map[MAX_TOKEN_MAPPINGS];
extern pthread_mutex_t s_token_mutex;

/* Discovery cache and allowed path prefixes are module-local in webui_config.c */

/* =============================================================================
 * Response Queue Functions
 * ============================================================================= */

/**
 * @brief Queue a response for delivery to WebSocket client
 *
 * Thread-safe. Wakes the LWS event loop via lws_cancel_service().
 *
 * @param resp Response to queue (copied, caller retains ownership)
 */
void queue_response(ws_response_t *resp);

/**
 * @brief Free dynamically allocated members of a response
 *
 * Called after response is sent. Frees strings/buffers based on type.
 *
 * @param resp Response to free
 */
void free_response(ws_response_t *resp);

/* =============================================================================
 * Token Mapping Functions
 * ============================================================================= */

/**
 * @brief Register a token->session_id mapping for reconnection
 *
 * Thread-safe. Evicts oldest if table is full.
 */
void register_token(const char *token, uint32_t session_id);

/**
 * @brief Remove all token mappings for a given session ID
 *
 * Thread-safe. Call when a session is destroyed to prevent stale lookups.
 */
void unregister_tokens_for_session(uint32_t session_id);

/**
 * @brief Look up session by reconnection token
 *
 * Thread-safe. Returns NULL if not found or session destroyed.
 * Cleans up stale entries when a mapped session no longer exists.
 */
session_t *lookup_session_by_token(const char *token);

/* =============================================================================
 * WebSocket Send Helpers
 *
 * send_json_response() — safe from any context, routes through the response
 * queue ensuring one lws_write() per WRITEABLE callback.
 *
 * send_json_message() — LWS SERVICE THREAD ONLY, calls lws_write() directly.
 * Only safe from within process_one_response() or similar WRITEABLE-driven
 * code paths. For handler code, prefer send_json_response().
 * ============================================================================= */

/**
 * @brief Send JSON text message to WebSocket client
 *
 * WARNING: LWS service thread only — do NOT call from worker/tool threads.
 * Use queue_response() with an appropriate WS_RESP_* type instead.
 *
 * @param wsi WebSocket instance
 * @param json JSON string to send
 * @return 0 on success, -1 on error
 */
int send_json_message(struct lws *wsi, const char *json);

/**
 * @brief Send binary message with type byte prefix
 *
 * WARNING: LWS service thread only — do NOT call from worker/tool threads.
 *
 * @param wsi WebSocket instance
 * @param msg_type Message type (WS_BIN_* constant)
 * @param data Payload data (may be NULL if len=0)
 * @param len Payload length
 * @return 0 on success, -1 on error
 */
int send_binary_message(struct lws *wsi, uint8_t msg_type, const uint8_t *data, size_t len);

/**
 * @brief Send state update to WebSocket client
 */
void send_state_impl(struct lws *wsi, const char *state, const char *detail);

/**
 * @brief Send state update with optional tools JSON
 */
void send_state_impl_full(struct lws *wsi,
                          const char *state,
                          const char *detail,
                          const char *tools_json);

/**
 * @brief Send audio chunk to WebSocket client
 */
void send_audio_impl(struct lws *wsi, const uint8_t *data, size_t len);

/**
 * @brief Send audio end marker to WebSocket client
 */
void send_audio_end_impl(struct lws *wsi, bool is_opus);

/* =============================================================================
 * Path Security Helpers
 * ============================================================================= */

/**
 * @brief Get MIME type for file extension
 */
const char *get_mime_type(const char *path);

/**
 * @brief Check if path contains directory traversal patterns
 */
bool contains_path_traversal(const char *path);

/**
 * @brief Validate path is within www directory after symlink resolution
 */
bool is_path_within_www(const char *filepath, const char *www_path);

/* =============================================================================
 * HTTP Security Headers (defined in webui_http.c)
 * ============================================================================= */

/**
 * @brief Add security headers to an HTTP response being constructed
 *
 * Adds CSP, X-Frame-Options, X-Content-Type-Options, Referrer-Policy,
 * Permissions-Policy, and HSTS (when HTTPS is enabled).
 *
 * Must be called AFTER content-type/content-length headers but BEFORE
 * lws_finalize_http_header().
 *
 * @param wsi HTTP connection
 * @param p Pointer to current write position in header buffer
 * @param end Pointer to end of header buffer
 * @return 0 on success, -1 on failure (buffer overflow)
 */
int webui_add_security_headers(struct lws *wsi, unsigned char **p, unsigned char *end);

/**
 * @brief Get pre-formatted security headers string for lws_serve_http_file()
 *
 * Returns a pointer to a static CRLF-separated header string built at init.
 * Used as the other_headers parameter for lws_serve_http_file().
 *
 * @param out_len If not NULL, receives the string length
 * @return Pointer to static header string (valid for lifetime of process)
 */
const char *webui_get_static_security_headers(int *out_len);

/**
 * @brief Initialize pre-formatted security headers string
 *
 * Must be called once during webui_server_init(), after g_config is loaded.
 */
void webui_security_headers_init(void);

/* =============================================================================
 * HTTP Protocol Handler (defined in webui_http.c)
 * ============================================================================= */

#ifdef ENABLE_AUTH
/**
 * @brief Check if HTTP request has valid session cookie
 *
 * @param wsi HTTP connection
 * @param session_out If not NULL, filled with session info on success
 * @return true if authenticated, false otherwise
 */
bool is_request_authenticated(struct lws *wsi, auth_session_t *session_out);
#endif

/**
 * @brief HTTP protocol callback for libwebsockets
 *
 * Handles static file serving, authentication endpoints, and OAuth callbacks.
 */
int callback_http(struct lws *wsi,
                  enum lws_callback_reasons reason,
                  void *user,
                  void *in,
                  size_t len);

/* =============================================================================
 * Session Token Generation
 * ============================================================================= */

/**
 * @brief Generate cryptographically secure session token
 *
 * @param token_out Buffer (WEBUI_SESSION_TOKEN_LEN bytes)
 * @return 0 on success, 1 on failure
 */
int generate_session_token(char token_out[WEBUI_SESSION_TOKEN_LEN]);

/* =============================================================================
 * Capability Helpers
 * ============================================================================= */

/**
 * @brief Check if client supports Opus codec
 *
 * Parses capabilities.audio_codecs array from init/reconnect payload.
 */
bool check_opus_capability(struct json_object *payload);

/* =============================================================================
 * Authentication Helpers
 * ============================================================================= */

/**
 * @brief Check if connection is a registered satellite session.
 *
 * Use this alongside conn_require_auth() at endpoints that satellites
 * should be allowed to access (e.g., music handlers). Check this FIRST
 * to avoid conn_require_auth()'s side-effect of sending an UNAUTHORIZED error.
 */
static inline bool conn_is_satellite_session(ws_connection_t *conn) {
   return conn && conn->is_satellite && conn->session != NULL;
}

/**
 * @brief Check if WebSocket connection is authenticated
 *
 * Re-validates session against database. Sends error if not authenticated.
 *
 * @param conn WebSocket connection
 * @return true if authenticated, false otherwise (error sent)
 */
bool conn_require_auth(ws_connection_t *conn);

/**
 * @brief Check if WebSocket connection has admin privileges
 *
 * Re-validates is_admin from database. Sends error if not admin.
 *
 * @param conn WebSocket connection
 * @return true if admin, false otherwise (error sent)
 */
bool conn_require_admin(ws_connection_t *conn);

/**
 * @brief Send JSON response to WebSocket client via the response queue
 *
 * Serializes the JSON object to a string and queues it as WS_RESP_JSON.
 * Safe to call from any context (RECEIVE callbacks, WRITEABLE callbacks,
 * or worker threads). The response is sent asynchronously via the
 * process_one_response() drain loop, ensuring one lws_write() per
 * WRITEABLE callback.
 *
 * The caller retains ownership of the json_object and must free it
 * after this call returns (the JSON string is copied internally).
 */
void send_json_response(ws_connection_t *conn, json_object *response);

/**
 * @brief Send error message implementation
 */
void send_error_impl(struct lws *wsi, const char *code, const char *message);

/**
 * @brief Force logout connections by auth session token prefix
 *
 * Finds all WebSocket connections with matching auth_session_token prefix
 * and sends them a force_logout message. Used when a session is revoked.
 *
 * @param auth_token_prefix First AUTH_TOKEN_PREFIX_LEN chars of auth token
 * @return Number of connections notified
 */
int webui_force_logout_by_auth_token(const char *auth_token_prefix);

/**
 * @brief Destroy session_manager sessions for connections with matching auth token.
 *
 * Used by the logout handler to release session slots immediately instead of
 * waiting for the 30-minute idle timeout. Detaches matching connections from
 * their sessions and destroys the sessions.
 *
 * @param auth_token_prefix First AUTH_TOKEN_PREFIX_LEN chars of auth token
 * @return Number of sessions destroyed
 */
int webui_destroy_sessions_by_auth_token(const char *auth_token_prefix);

/* =============================================================================
 * Prompt Construction Helpers
 * ============================================================================= */

/**
 * @brief Build user-specific system prompt with persona settings
 *
 * @param user_id User ID (0 for unauthenticated - returns base prompt copy)
 * @return Allocated prompt string (caller must free), or NULL on error
 */
char *build_user_prompt(int user_id);

/**
 * @brief Process command tags in LLM response
 *
 * Extracts <command> tags, publishes to MQTT, and collects results.
 *
 * @param llm_response The LLM response containing command tags
 * @param session The session for context
 * @return Allocated string with follow-up response, or NULL on error
 */
char *webui_process_commands(const char *llm_response, session_t *session);

/* =============================================================================
 * Admin Handler Functions (defined in webui_admin.c)
 * ============================================================================= */

/**
 * @brief List all users (admin only)
 */
void handle_list_users(ws_connection_t *conn);

/**
 * @brief Create a new user (admin only)
 */
void handle_create_user(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Delete a user (admin only)
 */
void handle_delete_user(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Change user password (admin for any user, or user for self)
 */
void handle_change_password(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Unlock a locked user account (admin only)
 */
void handle_unlock_user(ws_connection_t *conn, struct json_object *payload);

/* =============================================================================
 * Conversation Context Restore (defined in webui_server.c)
 * ============================================================================= */

/**
 * @brief Restore conversation context into a session from DB
 *
 * Clears session history, rebuilds with system prompt + compaction summary +
 * stored messages, and restores LLM config (type, provider, model, tools,
 * thinking).
 *
 * Used by both webui_conn_create_session (session expiry recovery) and
 * handle_load_conversation (sidebar click). Caller owns the conversation_t
 * and must call conv_free() after this returns.
 *
 * @param conn Connection with authenticated user
 * @param conv Conversation metadata (already fetched via conv_db_get)
 * @param conv_id Conversation ID
 * @param preloaded_msgs Optional pre-fetched message array (json_object array
 *        with "role" and "content" fields per element). If NULL, messages are
 *        fetched from the DB. Caller retains ownership; not freed by this function.
 * @return Number of messages restored, or -1 on error
 */
int webui_restore_conversation_context(ws_connection_t *conn,
                                       const conversation_t *conv,
                                       int64_t conv_id,
                                       json_object *preloaded_msgs);

/* =============================================================================
 * History Handler Functions (defined in webui_history.c)
 * ============================================================================= */

/**
 * @brief List conversations for the current user
 */
void handle_list_conversations(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Create a new conversation
 */
void handle_new_conversation(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Clear session history for a fresh start
 */
void handle_clear_session(ws_connection_t *conn);

/**
 * @brief Continue a conversation (after context compaction)
 */
void handle_continue_conversation(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Load a conversation and its messages
 */
void handle_load_conversation(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Delete a conversation
 */
void handle_delete_conversation(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Rename a conversation
 */
void handle_rename_conversation(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Set private mode for a conversation
 *
 * Private conversations are excluded from memory extraction.
 */
void handle_set_private(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Export a conversation as a self-contained JSON document
 */
void handle_export_conversation(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Search conversations by title or content
 */
void handle_search_conversations(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Save a message to a conversation
 */
void handle_save_message(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Update context usage for a conversation
 */
void handle_update_context(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Lock LLM settings for a conversation
 *
 * Called when first message is sent. Stores the current LLM settings.
 */
void handle_lock_conversation_llm(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Reassign a conversation to a different user (admin only)
 *
 * Used to reassign voice conversations to different users after they
 * have been saved from local/DAP sessions.
 */
void handle_reassign_conversation(ws_connection_t *conn, struct json_object *payload);

/* =============================================================================
 * Memory Handler Functions (defined in webui_memory.c)
 * ============================================================================= */

/**
 * @brief Get memory statistics for the current user
 */
void handle_get_memory_stats(ws_connection_t *conn);

/**
 * @brief List memory facts for the current user (paginated)
 */
void handle_list_memory_facts(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Delete a memory fact
 */
void handle_delete_memory_fact(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief List memory preferences for the current user (paginated)
 */
void handle_list_memory_preferences(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Delete a memory preference by category
 */
void handle_delete_memory_preference(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief List memory summaries for the current user (paginated)
 */
void handle_list_memory_summaries(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Delete a memory summary
 */
void handle_delete_memory_summary(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Search memory facts and summaries by keyword
 */
void handle_search_memory(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief List memory entities with relations (paginated)
 */
void handle_list_memory_entities(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Delete a memory entity and its relations
 */
void handle_delete_memory_entity(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Merge two memory entities (source absorbed into target)
 */
void handle_merge_memory_entities(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Delete all memories for the current user
 */
void handle_delete_all_memories(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Export all memories for the current user
 *
 * Supports "json" (DAWN lossless) and "text" (human-readable) formats.
 */
void handle_export_memories(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Import memories from JSON or plain text
 *
 * Supports preview mode (commit=false) and deduplication.
 */
void handle_import_memories(ws_connection_t *conn, struct json_object *payload);

/* =============================================================================
 * Document Library Handler Functions (defined in webui_doc_library.c)
 * ============================================================================= */
void handle_doc_library_list(ws_connection_t *conn, struct json_object *payload);
void handle_doc_library_delete(ws_connection_t *conn, struct json_object *payload);
void handle_doc_library_index(ws_connection_t *conn, struct json_object *payload);

/* =============================================================================
 * Calendar Handler Functions (defined in webui_calendar.c)
 * ============================================================================= */

#ifdef DAWN_ENABLE_CALENDAR_TOOL
void handle_calendar_list_accounts(ws_connection_t *conn);
void handle_calendar_add_account(ws_connection_t *conn, struct json_object *payload);
void handle_calendar_edit_account(ws_connection_t *conn, struct json_object *payload);
void handle_calendar_remove_account(ws_connection_t *conn, struct json_object *payload);
void handle_calendar_test_account(ws_connection_t *conn, struct json_object *payload);
void handle_calendar_sync_account(ws_connection_t *conn, struct json_object *payload);
void handle_calendar_list_calendars(ws_connection_t *conn, struct json_object *payload);
void handle_calendar_toggle_calendar(ws_connection_t *conn, struct json_object *payload);
void handle_calendar_toggle_read_only(ws_connection_t *conn, struct json_object *payload);
void handle_calendar_set_enabled(ws_connection_t *conn, struct json_object *payload);
#endif /* DAWN_ENABLE_CALENDAR_TOOL */

/* =============================================================================
 * Config Handler Functions (defined in webui_config.c)
 * ============================================================================= */

/**
 * @brief Get current configuration
 */
void handle_get_config(ws_connection_t *conn);

/**
 * @brief Set configuration values
 */
void handle_set_config(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Set secrets (API keys, passwords)
 */
void handle_set_secrets(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Get available audio devices
 */
void handle_get_audio_devices(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief List available ASR and TTS models
 */
void handle_list_models(ws_connection_t *conn);

/**
 * @brief List available network interfaces
 */
void handle_list_interfaces(ws_connection_t *conn);

/**
 * @brief List available local LLM models (Ollama/llama.cpp)
 */
void handle_list_llm_models(ws_connection_t *conn);

/* =============================================================================
 * Session Handler Functions (defined in webui_session.c)
 * ============================================================================= */

/**
 * @brief List current user's active sessions
 */
void handle_list_my_sessions(ws_connection_t *conn);

/**
 * @brief Revoke a session by token prefix
 */
void handle_revoke_session(ws_connection_t *conn, struct json_object *payload);

/* =============================================================================
 * Settings Handler Functions (defined in webui_settings.c)
 * ============================================================================= */

/**
 * @brief Get current user's personal settings
 */
void handle_get_my_settings(ws_connection_t *conn);

/**
 * @brief Update current user's personal settings
 */
void handle_set_my_settings(ws_connection_t *conn, struct json_object *payload);

/* =============================================================================
 * Tools Handler Functions (defined in webui_tools.c)
 * ============================================================================= */

/**
 * @brief Get tool configuration (enabled states)
 */
void handle_get_tools_config(ws_connection_t *conn);

/**
 * @brief Update tool enabled states
 */
void handle_set_tools_config(ws_connection_t *conn, struct json_object *payload);

/* =============================================================================
 * Audio Handler Functions (defined in webui_audio.c)
 * ============================================================================= */

#ifdef ENABLE_WEBUI_AUDIO
/**
 * @brief Handle binary WebSocket message (audio data)
 */
void handle_binary_message(ws_connection_t *conn, const uint8_t *data, size_t len);
#endif

/* =============================================================================
 * Music Handler Functions (defined in webui_music.c)
 * ============================================================================= */

/**
 * @brief Handle music_subscribe message
 */
void handle_music_subscribe(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Handle music_unsubscribe message
 */
void handle_music_unsubscribe(ws_connection_t *conn);

/**
 * @brief Handle music_control message
 */
void handle_music_control(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Handle music_search message
 */
void handle_music_search(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Handle music_library message
 */
void handle_music_library(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Handle music_queue message
 */
void handle_music_queue(ws_connection_t *conn, struct json_object *payload);

/* =============================================================================
 * Home Assistant Handler Functions (defined in webui_homeassistant.c)
 * ============================================================================= */

#ifdef DAWN_ENABLE_HOMEASSISTANT_TOOL
/**
 * @brief Get Home Assistant connection status
 */
void handle_ha_status(ws_connection_t *conn);

/**
 * @brief Test Home Assistant connection
 */
void handle_ha_test_connection(ws_connection_t *conn);

/**
 * @brief List Home Assistant entities
 */
void handle_ha_list_entities(ws_connection_t *conn);

/**
 * @brief Force refresh Home Assistant entity cache
 */
void handle_ha_refresh_entities(ws_connection_t *conn);
#endif /* DAWN_ENABLE_HOMEASSISTANT_TOOL */

/* =============================================================================
 * Satellite Connection Helpers (defined in webui_server.c)
 * ============================================================================= */

/**
 * @brief Check if a satellite with the given UUID is currently connected
 *
 * Thread-safe. Scans the connection registry for an active satellite connection
 * whose session identity UUID matches.
 *
 * @param uuid Satellite UUID string (36 chars)
 * @return true if online, false if not found or offline
 */
bool webui_is_satellite_online(const char *uuid);

/**
 * @brief Force-disconnect a satellite by UUID
 *
 * Thread-safe. Finds the satellite connection and closes it with a policy
 * violation reason. Used when an admin disables a satellite.
 *
 * @param uuid Satellite UUID string (36 chars)
 */
void webui_force_disconnect_satellite(const char *uuid);

/* =============================================================================
 * Satellite Admin Handler Functions (defined in webui_admin_satellite.c)
 * ============================================================================= */

/**
 * @brief List all satellite mappings with online status (admin only)
 */
void handle_list_satellites(ws_connection_t *conn);

/**
 * @brief Update satellite mapping (user, ha_area, enabled) (admin only)
 */
void handle_update_satellite(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Delete a satellite mapping (admin only)
 */
void handle_delete_satellite(ws_connection_t *conn, struct json_object *payload);

/* =============================================================================
 * Satellite Handler Functions (defined in webui_satellite.c)
 * ============================================================================= */

/**
 * @brief Strip <command>...</command> and <end_of_turn> tags from text in-place
 *
 * Shared utility used by satellite worker and audio sentence callback.
 */
void strip_command_tags(char *text);

/**
 * @brief Handle satellite_register message
 */
void handle_satellite_register(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Handle satellite_query message
 */
void handle_satellite_query(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Handle satellite_ping message
 */
void handle_satellite_ping(ws_connection_t *conn);

/**
 * @brief Handle volume_state message from satellite
 */
void handle_satellite_volume_state(ws_connection_t *conn, struct json_object *payload);

/* =============================================================================
 * Connection Iterator (defined in webui_server.c, used by webui_music.c)
 * ============================================================================= */

/**
 * @brief Iterate all authenticated connections for a given user
 *
 * Calls callback for each active, authenticated connection with matching
 * auth_user_id and non-NULL music_state. Holds s_conn_registry_mutex
 * during iteration.
 *
 * @param user_id User ID to filter by (must be > 0)
 * @param callback Function to call for each matching connection
 * @param ctx Opaque context passed to callback
 */
void webui_for_each_conn_by_user(int user_id,
                                 void (*callback)(ws_connection_t *conn, void *ctx),
                                 void *ctx);

/**
 * @brief Collect authenticated connections for a given user into a caller-provided array
 *
 * Holds s_conn_registry_mutex only during collection, not during subsequent use.
 * Caller must use results promptly (pointers may become invalid on disconnect).
 *
 * @param user_id User ID to filter by (must be > 0)
 * @param out Array to fill with matching connection pointers
 * @param max_out Capacity of out array
 * @return Number of connections found (may exceed max_out; only max_out are stored)
 */
int webui_collect_conns_by_user(int user_id, ws_connection_t **out, int max_out);

/* =============================================================================
 * Audio Send Functions (defined in webui_server.c, used by webui_audio.c)
 * ============================================================================= */

/**
 * @brief Queue audio data for WebSocket client
 *
 * @param session WebSocket session
 * @param data Audio data (Opus or PCM depending on client capability)
 * @param len Data length in bytes
 */
void webui_send_audio(session_t *session, const uint8_t *data, size_t len);

/**
 * @brief Queue end-of-audio marker for WebSocket client
 *
 * @param session WebSocket session
 */
void webui_send_audio_end(session_t *session, bool is_opus);

/**
 * @brief TTS sentence callback for LLM streaming
 *
 * Called for each complete sentence during LLM response streaming.
 * Generates TTS audio and sends immediately, enabling real-time playback.
 * Respects conn->tts_enabled flag (no audio if disabled).
 *
 * @param sentence Complete sentence text
 * @param userdata Session pointer (cast to session_t*)
 */
void webui_sentence_audio_callback(const char *sentence, void *userdata);

#ifdef __cplusplus
}
#endif

#endif /* WEBUI_INTERNAL_H */
