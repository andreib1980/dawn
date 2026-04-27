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
 * Authentication Database Internal Header
 *
 * SECURITY: This header exposes internal database state and MUST NOT be
 * included by code outside the auth_db_*.c modules. Use auth/auth_db.h
 * for the public API.
 *
 * This header provides shared state and helper macros for the modularized
 * auth_db implementation (auth_db_core.c, auth_db_user.c, etc.).
 */

#ifndef AUTH_DB_INTERNAL_H
#define AUTH_DB_INTERNAL_H

/* Security guard - only auth_db modules should include this */
#ifndef AUTH_DB_INTERNAL_ALLOWED
#error "auth_db_internal.h is an internal header - include auth/auth_db.h instead"
#endif

#include <pthread.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <time.h>

#include "auth/auth_db.h"

/* =============================================================================
 * Constants
 * ============================================================================= */

/* Current schema version */
#define AUTH_DB_SCHEMA_VERSION 38

/* Retention periods */
#define LOGIN_ATTEMPT_RETENTION_SEC (7 * 24 * 60 * 60) /* 7 days */
#define AUTH_LOG_RETENTION_SEC (30 * 24 * 60 * 60)     /* 30 days */

/* Helper macro for stringifying values in SQL */
#define STRINGIFY_HELPER(x) #x
#define STRINGIFY(x) STRINGIFY_HELPER(x)

/* =============================================================================
 * Database State Structure (~408 bytes in BSS)
 *
 * Contains the SQLite database handle, mutex, and all 43 prepared statements.
 * Allocated statically in auth_db_core.c, not on heap.
 * ============================================================================= */

typedef struct {
   sqlite3 *db;
   pthread_mutex_t mutex;
   bool initialized;
   time_t last_cleanup;
   time_t last_vacuum; /* Rate limiting for vacuum operations */

   /* === User module statements (auth_db_user.c) === */
   sqlite3_stmt *stmt_create_user;
   sqlite3_stmt *stmt_get_user;
   sqlite3_stmt *stmt_count_users;
   sqlite3_stmt *stmt_inc_failed_attempts;
   sqlite3_stmt *stmt_reset_failed_attempts;
   sqlite3_stmt *stmt_update_last_login;
   sqlite3_stmt *stmt_set_lockout;

   /* === Session module statements (auth_db_session.c) === */
   sqlite3_stmt *stmt_create_session;
   sqlite3_stmt *stmt_get_session;
   sqlite3_stmt *stmt_update_session_activity;
   sqlite3_stmt *stmt_delete_session;
   sqlite3_stmt *stmt_delete_user_sessions;
   sqlite3_stmt *stmt_delete_expired_sessions;

   /* === Rate limit module statements (auth_db_rate_limit.c) === */
   sqlite3_stmt *stmt_count_recent_failures;
   sqlite3_stmt *stmt_log_attempt;
   sqlite3_stmt *stmt_delete_old_attempts;

   /* === Audit module statements (auth_db_audit.c) === */
   sqlite3_stmt *stmt_log_event;
   sqlite3_stmt *stmt_delete_old_logs;

   /* === Settings module statements (auth_db_settings.c) === */
   sqlite3_stmt *stmt_get_user_settings;
   sqlite3_stmt *stmt_set_user_settings;

   /* === Conversation module statements (auth_db_conv.c) === */
   sqlite3_stmt *stmt_conv_create;
   sqlite3_stmt *stmt_conv_get;
   sqlite3_stmt *stmt_conv_list;
   sqlite3_stmt *stmt_conv_list_all;
   sqlite3_stmt *stmt_conv_search;
   sqlite3_stmt *stmt_conv_search_content;
   sqlite3_stmt *stmt_conv_rename;
   sqlite3_stmt *stmt_conv_delete;
   sqlite3_stmt *stmt_conv_delete_admin;
   sqlite3_stmt *stmt_conv_count;
   sqlite3_stmt *stmt_msg_add;
   sqlite3_stmt *stmt_msg_get;
   sqlite3_stmt *stmt_msg_get_admin;
   sqlite3_stmt *stmt_conv_update_meta;
   sqlite3_stmt *stmt_conv_update_context;
   sqlite3_stmt *stmt_conv_create_origin;
   sqlite3_stmt *stmt_conv_reassign;

   /* === Metrics module statements (auth_db_metrics.c) === */
   sqlite3_stmt *stmt_metrics_save;
   sqlite3_stmt *stmt_metrics_update;
   sqlite3_stmt *stmt_metrics_delete_old;
   sqlite3_stmt *stmt_provider_metrics_save;
   sqlite3_stmt *stmt_provider_metrics_delete;

   /* === Image module statements (image_store.c) === */
   sqlite3_stmt *stmt_image_create;
   sqlite3_stmt *stmt_image_get;
   sqlite3_stmt *stmt_image_get_file; /* filename + user_id + source for path + access check */
   sqlite3_stmt *stmt_image_delete;
   sqlite3_stmt *stmt_image_update_access;
   sqlite3_stmt *stmt_image_update_retention;
   sqlite3_stmt *stmt_image_count_user;
   sqlite3_stmt *stmt_image_delete_old;        /* DEFAULT retention: created_at < cutoff */
   sqlite3_stmt *stmt_image_cache_total_size;  /* SUM(size) for RETAIN_CACHE images */
   sqlite3_stmt *stmt_image_delete_cache_lru;  /* oldest CACHE image by last_accessed */
   sqlite3_stmt *stmt_image_get_expired_ids;   /* IDs + filenames of expired images */
   sqlite3_stmt *stmt_image_get_cache_lru_ids; /* IDs + filenames of LRU cache overflow */
   sqlite3_stmt *stmt_image_stats;             /* COUNT + SUM(size) for all images */

   /* === Memory module statements (memory_db.c) === */
   sqlite3_stmt *stmt_memory_fact_create;
   sqlite3_stmt *stmt_memory_fact_get;
   sqlite3_stmt *stmt_memory_fact_list;
   sqlite3_stmt *stmt_memory_fact_search;
   sqlite3_stmt *stmt_memory_fact_update_access;
   sqlite3_stmt *stmt_memory_fact_update_confidence;
   sqlite3_stmt *stmt_memory_fact_supersede;
   sqlite3_stmt *stmt_memory_fact_delete;
   sqlite3_stmt *stmt_memory_fact_find_similar;
   sqlite3_stmt *stmt_memory_fact_find_by_hash;
   sqlite3_stmt *stmt_memory_fact_prune_superseded;
   sqlite3_stmt *stmt_memory_fact_prune_stale;

   sqlite3_stmt *stmt_memory_pref_upsert;
   sqlite3_stmt *stmt_memory_pref_get;
   sqlite3_stmt *stmt_memory_pref_list;
   sqlite3_stmt *stmt_memory_pref_search;
   sqlite3_stmt *stmt_memory_pref_delete;

   sqlite3_stmt *stmt_memory_summary_create;
   sqlite3_stmt *stmt_memory_summary_list;
   sqlite3_stmt *stmt_memory_summary_mark_consolidated;
   sqlite3_stmt *stmt_memory_summary_search;

   /* Date-filtered memory queries */
   sqlite3_stmt *stmt_memory_fact_search_since;
   sqlite3_stmt *stmt_memory_summary_search_since;
   sqlite3_stmt *stmt_memory_fact_list_since;
   sqlite3_stmt *stmt_memory_summary_list_since;

   /* Category-filtered fact queries (v34) */
   sqlite3_stmt *stmt_memory_fact_search_by_category;
   sqlite3_stmt *stmt_memory_fact_update_category;
   sqlite3_stmt *stmt_memory_fact_list_general;
   sqlite3_stmt *stmt_memory_fact_count_general;

   /* Extraction tracking */
   sqlite3_stmt *stmt_conv_get_last_extracted;
   sqlite3_stmt *stmt_conv_set_last_extracted;

   /* Privacy flag */
   sqlite3_stmt *stmt_conv_set_private;

   /* Auto-title (memory extraction) */
   sqlite3_stmt *stmt_conv_auto_title;
   sqlite3_stmt *stmt_conv_set_title_locked;

   /* === Embedding module statements (memory_db.c) === */
   sqlite3_stmt *stmt_memory_fact_update_embedding;
   sqlite3_stmt *stmt_memory_fact_get_embeddings;
   sqlite3_stmt *stmt_memory_fact_list_without_embedding;

   /* === Satellite mapping statements (auth_db_satellite.c) === */
   sqlite3_stmt *stmt_satellite_upsert;
   sqlite3_stmt *stmt_satellite_get;
   sqlite3_stmt *stmt_satellite_delete;
   sqlite3_stmt *stmt_satellite_update_user;
   sqlite3_stmt *stmt_satellite_update_location;
   sqlite3_stmt *stmt_satellite_set_enabled;
   sqlite3_stmt *stmt_satellite_update_last_seen;
   sqlite3_stmt *stmt_satellite_list;

   /* === Entity graph statements (memory_db.c) === */
   sqlite3_stmt *stmt_memory_entity_upsert;
   sqlite3_stmt *stmt_memory_entity_get_by_name;
   sqlite3_stmt *stmt_memory_entity_update_embedding;
   sqlite3_stmt *stmt_memory_entity_get_embeddings;
   sqlite3_stmt *stmt_memory_relation_create;
   sqlite3_stmt *stmt_memory_relation_close_open; /* v33 — supersede helper */
   sqlite3_stmt *stmt_memory_relation_list_by_subject;
   sqlite3_stmt *stmt_memory_relation_list_by_subject_at; /* v33 — as-of variant */
   sqlite3_stmt *stmt_memory_relation_list_by_object;
   sqlite3_stmt *stmt_memory_entity_search;
   sqlite3_stmt *stmt_memory_entity_delete;
   sqlite3_stmt *stmt_memory_entity_set_photo;
   sqlite3_stmt *stmt_memory_entity_get_photo;
   sqlite3_stmt *stmt_memory_relation_delete_by_entity;

   /* === Document search statements (document_db.c) === */
   sqlite3_stmt *stmt_doc_create;
   sqlite3_stmt *stmt_doc_get;
   sqlite3_stmt *stmt_doc_get_by_hash;
   sqlite3_stmt *stmt_doc_list;
   sqlite3_stmt *stmt_doc_delete;
   sqlite3_stmt *stmt_doc_count_user;
   sqlite3_stmt *stmt_doc_chunk_create;
   sqlite3_stmt *stmt_doc_chunk_search;
   sqlite3_stmt *stmt_doc_find_by_name;
   sqlite3_stmt *stmt_doc_chunk_read;
   sqlite3_stmt *stmt_doc_list_all;
   sqlite3_stmt *stmt_doc_update_global;

   /* === Calendar module statements (calendar_db.c) === */
   sqlite3_stmt *stmt_cal_acct_create;
   sqlite3_stmt *stmt_cal_acct_get;
   sqlite3_stmt *stmt_cal_acct_list;
   sqlite3_stmt *stmt_cal_acct_update;
   sqlite3_stmt *stmt_cal_acct_delete;
   sqlite3_stmt *stmt_cal_acct_update_sync;
   sqlite3_stmt *stmt_cal_acct_update_discovery;
   sqlite3_stmt *stmt_cal_cal_create;
   sqlite3_stmt *stmt_cal_cal_get;
   sqlite3_stmt *stmt_cal_cal_list;
   sqlite3_stmt *stmt_cal_cal_update_ctag;
   sqlite3_stmt *stmt_cal_cal_set_active;
   sqlite3_stmt *stmt_cal_cal_delete;
   sqlite3_stmt *stmt_cal_cal_active_for_user;
   sqlite3_stmt *stmt_cal_evt_upsert;
   sqlite3_stmt *stmt_cal_evt_get_by_uid;
   sqlite3_stmt *stmt_cal_evt_delete;
   sqlite3_stmt *stmt_cal_evt_delete_by_cal;
   sqlite3_stmt *stmt_cal_occ_insert;
   sqlite3_stmt *stmt_cal_occ_delete_for_event;
   sqlite3_stmt *stmt_cal_occ_in_range;
   sqlite3_stmt *stmt_cal_occ_allday_in_range;
   sqlite3_stmt *stmt_cal_occ_search;
   sqlite3_stmt *stmt_cal_occ_next;
   sqlite3_stmt *stmt_cal_acct_list_enabled;
   sqlite3_stmt *stmt_cal_acct_set_read_only;
   sqlite3_stmt *stmt_cal_acct_set_enabled;

   /* === Contacts module statements (contacts_db.c) === */
   sqlite3_stmt *stmt_contacts_find;
   sqlite3_stmt *stmt_contacts_add;
   sqlite3_stmt *stmt_contacts_delete;
   sqlite3_stmt *stmt_contacts_list;
   sqlite3_stmt *stmt_contacts_update;
   sqlite3_stmt *stmt_contacts_count;

   /* === Email module statements (email_db.c) === */
   sqlite3_stmt *stmt_email_acct_create;
   sqlite3_stmt *stmt_email_acct_get;
   sqlite3_stmt *stmt_email_acct_list;
   sqlite3_stmt *stmt_email_acct_update;
   sqlite3_stmt *stmt_email_acct_delete;
   sqlite3_stmt *stmt_email_acct_set_read_only;
   sqlite3_stmt *stmt_email_acct_set_enabled;

   /* === OAuth module statements (oauth_client.c) === */
   sqlite3_stmt *stmt_oauth_store;
   sqlite3_stmt *stmt_oauth_load;
   sqlite3_stmt *stmt_oauth_delete;
   sqlite3_stmt *stmt_oauth_exists;
   sqlite3_stmt *stmt_oauth_list_accounts;
} auth_db_state_t;

/* Ensure last_stmt_end covers all statement fields (catches reorder bugs) */
_Static_assert(offsetof(auth_db_state_t, stmt_conv_set_title_locked) >
                   offsetof(auth_db_state_t, stmt_create_user),
               "last_stmt_end must be after first_stmt_offset");

/* =============================================================================
 * Shared State (defined in auth_db_core.c)
 * ============================================================================= */

extern auth_db_state_t s_db;

/* =============================================================================
 * Mutex Helper Macros
 *
 * Use these macros to enforce consistent locking patterns across all modules.
 * ============================================================================= */

/**
 * @brief Lock mutex and check initialization, returning specified value if not ready
 *
 * Use this for functions that return -1 or false on failure instead of AUTH_DB_FAILURE.
 */
#define AUTH_DB_LOCK_OR_RETURN(val)         \
   do {                                     \
      pthread_mutex_lock(&s_db.mutex);      \
      if (!s_db.initialized) {              \
         pthread_mutex_unlock(&s_db.mutex); \
         return (val);                      \
      }                                     \
   } while (0)

/**
 * @brief Lock mutex and check initialization, returning AUTH_DB_FAILURE if not ready
 *
 * Usage:
 *   AUTH_DB_LOCK_OR_FAIL();
 *   // ... do work ...
 *   AUTH_DB_UNLOCK();
 *   return result;
 */
#define AUTH_DB_LOCK_OR_FAIL() AUTH_DB_LOCK_OR_RETURN(AUTH_DB_FAILURE)

/**
 * @brief Lock mutex and check initialization for void functions
 *
 * Use this in functions that return void.
 */
#define AUTH_DB_LOCK_OR_RETURN_VOID()       \
   do {                                     \
      pthread_mutex_lock(&s_db.mutex);      \
      if (!s_db.initialized) {              \
         pthread_mutex_unlock(&s_db.mutex); \
         return;                            \
      }                                     \
   } while (0)

/**
 * @brief Unlock the database mutex
 */
#define AUTH_DB_UNLOCK() pthread_mutex_unlock(&s_db.mutex)

/* =============================================================================
 * Prepared Statement Invariant
 *
 * INVARIANT: All s_db.stmt_* pointers are valid after auth_db_init() returns
 * AUTH_DB_SUCCESS and before auth_db_shutdown() is called.
 *
 * Module code MUST NOT check for NULL - if init failed, the system should not
 * be running. This avoids redundant NULL checks throughout the codebase.
 *
 * Statement Usage Pattern:
 *   sqlite3_reset(s_db.stmt_xxx);
 *   sqlite3_bind_*(s_db.stmt_xxx, ...);
 *   int rc = sqlite3_step(s_db.stmt_xxx);
 *   // ... process results ...
 *   sqlite3_reset(s_db.stmt_xxx);  // ALWAYS reset before returning
 * ============================================================================= */

/* =============================================================================
 * Internal Helper Functions (defined in auth_db_core.c)
 * ============================================================================= */

/**
 * @brief Verify database file has secure permissions (0600)
 *
 * @param path Path to database file
 * @return AUTH_DB_SUCCESS if OK, AUTH_DB_FAILURE on error
 */
int auth_db_internal_verify_permissions(const char *path);

/**
 * @brief Create parent directory with secure permissions (0700)
 *
 * @param path Path to file (directory will be extracted)
 * @return AUTH_DB_SUCCESS if OK, AUTH_DB_FAILURE on error
 */
int auth_db_internal_create_parent_dir(const char *path);

#endif /* AUTH_DB_INTERNAL_H */
