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
 * Tavily per-user rate limiter (shared between /search and /extract adapters).
 *
 * Implements a fixed-window counter for two scales: 10 calls/minute and
 * 100 calls/hour per user. A runaway LLM tool loop is bounded at
 * ~2400 calls/day worst case, well inside Tavily's 1000/mo free tier.
 *
 * Thread Safety: All functions thread-safe (internal pthread_mutex).
 */

#ifndef TAVILY_RATE_LIMIT_H
#define TAVILY_RATE_LIMIT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Default limits — both /search and /extract count against the same bucket
 * since Tavily quota is unified. */
#define TAVILY_RL_PER_MINUTE 10
#define TAVILY_RL_PER_HOUR 100

/* Bucket table capacity. Sized for personal DAWN deployments (1-3 users).
 * Beyond this the limiter fails open (logged), letting calls through rather
 * than blocking legitimate work. Bump this if a deployment regularly serves
 * more than ~8 concurrent authenticated users. */
#define TAVILY_RL_MAX_USERS 8

/**
 * @brief Charge one call against the user's bucket and report if allowed.
 *
 * On exhaustion, returns false; the caller should fall through to SearXNG /
 * FlareSolverr / empty result. Counter is NOT charged when returning false
 * (so a denied request doesn't push the bucket further over the cap).
 *
 * @param user_id Authenticated user id (1 = admin in DAWN's default setup).
 *                user_id <= 0 is treated as a single "anonymous" bucket.
 * @return true if call is allowed, false if rate-limited.
 */
bool tavily_rate_limit_check(int user_id);

/**
 * @brief Reset all buckets. Intended for tests; safe to call in production.
 */
void tavily_rate_limit_reset(void);

/**
 * @brief Query remaining budget without charging.
 *
 * Convenience helper for diagnostics / logs. Either out pointer can be NULL.
 */
void tavily_rate_limit_remaining(int user_id, int *minute_remaining, int *hour_remaining);

#ifdef __cplusplus
}
#endif

#endif /* TAVILY_RATE_LIMIT_H */
