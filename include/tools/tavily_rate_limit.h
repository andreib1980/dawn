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
 * Implements a fixed-window counter at three scales:
 *   - 10 calls/minute and 100 calls/hour per user (runaway-loop bound)
 *   - 500 calls/day GLOBAL across all users (monthly-quota backstop)
 *
 * The per-user limits cap any one caller; the global daily ceiling caps the
 * deployment as a whole so two simultaneously-runaway users can't double the
 * quota burn. 500/day = ~15k/month theoretical, but a single-day burst is
 * capped at half of Tavily's 1000/mo free tier — operator notices before
 * the month's budget is gone.
 *
 * Thread Safety: All functions thread-safe (internal pthread_mutex).
 */

#ifndef TAVILY_RATE_LIMIT_H
#define TAVILY_RATE_LIMIT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Compile-time default limits. Live values are read from
 * `g_config.url_fetcher.tavily.rate_limit_*` at check time so operators
 * can tune via dawn.toml / WebUI without recompile. A configured value of
 * 0 falls back to these defaults (matches the rest of the Tavily config
 * surface). Both /search and /extract count against the same buckets
 * since Tavily quota is unified. */
#define TAVILY_RL_PER_MINUTE_DEFAULT 10
#define TAVILY_RL_PER_HOUR_DEFAULT 100
#define TAVILY_RL_PER_DAY_DEFAULT 500

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

/**
 * @brief Resolve the active user_id for rate-limit isolation.
 *
 * Reads the per-call command-context session, falls back to the local session
 * for command-context-less invocations (legacy MQTT path), and finally to 0
 * (anonymous bucket) when no session exists. Lives here because both
 * url_fetcher_fallback.c and web_search.c need the same resolution to keep
 * one user's runaway loop from poisoning another user's quota.
 */
int tavily_rate_limit_resolve_user_id(void);

#ifdef __cplusplus
}
#endif

#endif /* TAVILY_RATE_LIMIT_H */
