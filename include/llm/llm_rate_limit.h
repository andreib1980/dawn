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
 * Process-wide sliding window rate limiter for cloud LLM API calls, plus a
 * shared interrupt-aware sleep helper (llm_sleep_with_interrupt_check) used by
 * both the rate-limiter slot wait and the tool-loop transient-error backoff.
 * (If a third, non-rate-limit caller appears, promote the sleep helper to a
 * dedicated llm_util.{c,h} — at two co-located callers it lives here.)
 */

#ifndef LLM_RATE_LIMIT_H
#define LLM_RATE_LIMIT_H

/** Maximum number of timestamp slots in the circular buffer */
#define LLM_RATE_LIMIT_MAX_SLOTS 128

/**
 * Initialize the rate limiter.
 * @param max_rpm Maximum requests per minute. Pass 0 to disable.
 */
void llm_rate_limit_init(int max_rpm);

/**
 * Block until a request slot is available.
 * Returns immediately if rate limiting is disabled.
 * @return 0 on success, 1 if interrupted (caller should abort the request)
 */
int llm_rate_limit_wait(void);

/**
 * Sleep for @p total_ms, in chunks, polling the global LLM interrupt flag
 * (llm_is_interrupt_requested) between chunks so a wake-word / cancel can cut a
 * backoff or rate-limit wait short.  Shared by the transient-error backoff in
 * the tool loop and the rate-limiter's slot wait.
 * @param total_ms Total time to sleep, in milliseconds (<= 0 returns immediately).
 * @return 1 if interrupted (caller should bail), 0 if the full duration elapsed.
 *         The 1/0 is a normal/interrupted sentinel, NOT SUCCESS/FAILURE.
 */
int llm_sleep_with_interrupt_check(int total_ms);

/**
 * Update the RPM limit at runtime (e.g., from WebUI settings).
 * Pass 0 to disable rate limiting.
 * @param max_rpm New maximum requests per minute (clamped to LLM_RATE_LIMIT_MAX_SLOTS)
 */
void llm_rate_limit_set_rpm(int max_rpm);

/** Clean up rate limiter resources (call on shutdown) */
void llm_rate_limit_cleanup(void);

#endif /* LLM_RATE_LIMIT_H */
