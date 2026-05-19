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
 * Generic rate limiter with multi-IP tracking and LRU eviction.
 */

#ifndef RATE_LIMITER_H
#define RATE_LIMITER_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

/* Maximum IP address length (IPv6 = 45 chars + null, padded for alignment) */
#define RATE_LIMIT_IP_SIZE 48

/* Default slot count for rate limiters */
#define RATE_LIMIT_DEFAULT_SLOTS 32

/**
 * @brief Rate limit entry for tracking a single IP
 */
typedef struct {
   char ip[RATE_LIMIT_IP_SIZE];
   int count;
   time_t window_start;
   time_t last_access; /* For LRU eviction */
} rate_limit_entry_t;

/**
 * @brief Rate limiter configuration
 */
typedef struct {
   int max_count;     /**< Maximum requests allowed in window */
   time_t window_sec; /**< Window duration in seconds */
   int slot_count;    /**< Number of IP slots to track */
} rate_limiter_config_t;

/**
 * @brief Rate limiter instance
 *
 * Use RATE_LIMITER_STATIC_INIT for static initialization or
 * rate_limiter_init() for dynamic initialization.
 */
typedef struct {
   rate_limit_entry_t *entries; /**< Array of entries (caller-owned) */
   rate_limiter_config_t config;
   pthread_mutex_t mutex;
} rate_limiter_t;

/**
 * @brief Static initializer for rate limiter
 *
 * @note IMPORTANT: This macro is ONLY valid for static/file-scope variables.
 *       PTHREAD_MUTEX_INITIALIZER requires static storage duration.
 *       For heap or stack allocation, use rate_limiter_init() instead.
 *
 * Example usage:
 * @code
 * static rate_limit_entry_t csrf_entries[32];
 * static rate_limiter_t csrf_limiter = RATE_LIMITER_STATIC_INIT(
 *     csrf_entries, 32, 30, 60);  // 30 requests per 60 seconds
 * @endcode
 */
#define RATE_LIMITER_STATIC_INIT(entries_ptr, slots, max, window)                      \
   {                                                                                   \
      .entries = (entries_ptr),                                                        \
      .config = { .max_count = (max), .window_sec = (window), .slot_count = (slots) }, \
      .mutex = PTHREAD_MUTEX_INITIALIZER                                               \
   }

/**
 * @brief Initialize a rate limiter
 *
 * @param limiter Rate limiter to initialize
 * @param entries Caller-owned array of entries (must be zeroed)
 * @param config Rate limiter configuration
 */
void rate_limiter_init(rate_limiter_t *limiter,
                       rate_limit_entry_t *entries,
                       const rate_limiter_config_t *config);

/**
 * @brief Check rate limit for an IP and increment counter if allowed
 *
 * Uses multi-IP tracking with LRU eviction when slots are exhausted.
 *
 * @note Fail-closed on IP-too-long: if @p ip would not fit within
 *       RATE_LIMIT_IP_SIZE-1 bytes (i.e. silent truncation during the
 *       internal strncpy), this function returns true (rate-limited).
 *       Truncation would defeat the per-IP strcmp identity check — two
 *       distinct IPs sharing a prefix would collide on the stored slot
 *       and let an attacker evict a legitimate user's burned-budget
 *       state.  Callers MUST ensure their captured client_ip is bounded;
 *       in practice this is satisfied by @ref rate_limiter_normalize_ip
 *       (IPv6 → /64) when paired with the standard 64-byte LWS peer-info
 *       capture used in webui_http.c.
 *
 * @note Scaling: each check does an O(N) linear scan over slot_count
 *       entries.  N=64 is fine for the existing callers (cold paths,
 *       cache-line-friendly).  Past N≈256 the strcmp-per-slot cost starts
 *       to matter — at that point bucketed lookup (hash + linear probing)
 *       is the right migration, not larger linear-scan tables.
 *
 * @param limiter Rate limiter instance
 * @param ip Client IP address (will be copied internally)
 * @return true if rate limited (reject request), false if allowed
 */
bool rate_limiter_check(rate_limiter_t *limiter, const char *ip);

/**
 * @brief Reset rate limit for an IP (e.g., on successful login)
 *
 * @param limiter Rate limiter instance
 * @param ip Client IP address to reset
 */
void rate_limiter_reset(rate_limiter_t *limiter, const char *ip);

/**
 * @brief Clear all rate limit entries
 *
 * @param limiter Rate limiter instance
 */
void rate_limiter_clear_all(rate_limiter_t *limiter);

/**
 * @brief Normalize IP address for rate limiting
 *
 * For IPv6 addresses, normalizes to /64 prefix to prevent bypass via
 * address rotation within the same network. IPv4 addresses pass through.
 *
 * @param ip Input IP address
 * @param out Buffer for normalized address (must be at least RATE_LIMIT_IP_SIZE)
 * @param out_len Size of output buffer
 */
void rate_limiter_normalize_ip(const char *ip, char *out, size_t out_len);

#endif /* RATE_LIMITER_H */
