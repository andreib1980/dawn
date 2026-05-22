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
 * Messaging engine — Layer 2.
 *
 * Driver registry, per-user channel resolution, link-code issuance, rate
 * limiters (inbound + outbound), worker-drain dispatch.  Public API for
 * the LLM tool and the WebUI link-code endpoint.  See
 * docs/MESSAGING_CHANNELS_DESIGN.md.
 */
#ifndef MESSAGING_ENGINE_H
#define MESSAGING_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "messaging/messaging_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Return codes (use SUCCESS/FAILURE from dawn_error.h elsewhere; these
 * are the specific cases the engine reports). */
#define MESSAGING_SUCCESS 0
#define MESSAGING_FAILURE 1
#define MESSAGING_UNKNOWN_CHANNEL 2
#define MESSAGING_UNKNOWN_USER 3
#define MESSAGING_RATE_LIMITED 4
#define MESSAGING_PROVIDER_RATE_LIMITED 5
#define MESSAGING_DRIVER_NOT_REGISTERED 6
#define MESSAGING_INVALID_ADDRESS 7

/* Link-code shape — Crockford base32, no I/L/O/U. */
#define MESSAGING_LINK_CODE_LEN 8
#define MESSAGING_LINK_CODE_BUF_SIZE 16 /* room for "DAWN" prefix later if added */
#define MESSAGING_LINK_TTL_SECONDS (10 * 60)

/** Link-code lookup result. */
typedef enum {
   MESSAGING_LINK_STATE_NOT_FOUND = 0,
   MESSAGING_LINK_STATE_PENDING,
   MESSAGING_LINK_STATE_CLAIMED,
   MESSAGING_LINK_STATE_EXPIRED,
} messaging_link_state_t;

/**
 * @brief Initialize the messaging engine.
 *
 * Spawns the worker drain thread, initializes rate limiters, primes the
 * link-code RNG.  Drivers register themselves separately via
 * messaging_engine_register_driver() at their own init time (after the
 * engine but before they go live).
 *
 * @return MESSAGING_SUCCESS on success, MESSAGING_FAILURE if init fails.
 */
int messaging_engine_init(void);

/**
 * @brief Shut the engine down.  Stops the worker drain.  Drivers are
 *        torn down separately by their own shutdown calls.
 */
void messaging_engine_shutdown(void);

/**
 * @brief Register a driver with the engine.
 *
 * Called by each driver at its init time, AFTER messaging_engine_init().
 * The engine takes a copy of the function pointers but does not own the
 * memory backing the driver struct (drivers are static const).
 *
 * @return MESSAGING_SUCCESS / MESSAGING_FAILURE (registry full or
 *         duplicate name).
 */
int messaging_engine_register_driver(const messaging_driver_t *driver);

/**
 * @brief Send a text message on a named channel for a specific user.
 *
 * Looks up the channel in messaging_channels by (user_id, display_name),
 * verifies ownership, applies per-channel + provider-global rate limit,
 * dispatches to the driver.
 *
 * @param user_id       The DAWN user the LLM tool is acting on behalf of.
 * @param channel_name  Display name as registered (e.g., "telegram_main").
 *                      Case-insensitive match.
 * @param text          UTF-8 message body (v1 text-only, no formatting).
 *
 * @return MESSAGING_SUCCESS, MESSAGING_UNKNOWN_CHANNEL,
 *         MESSAGING_RATE_LIMITED, MESSAGING_PROVIDER_RATE_LIMITED,
 *         MESSAGING_DRIVER_NOT_REGISTERED, MESSAGING_FAILURE.
 */
int messaging_engine_send(int user_id, const char *channel_name, const char *text);

/**
 * @brief List a user's channels as a JSON array.
 *
 * Returns an allocated string like
 * `[{"name":"telegram_main","provider":"telegram","enabled":true}, ...]`.
 * Caller frees.
 *
 * @return Allocated JSON string (caller frees), or NULL on failure.
 */
char *messaging_engine_list_channels_json(int user_id);

/**
 * @brief Generate a new one-time link code for `user_id`.
 *
 * Inserts into messaging_link_codes with a 10-minute TTL.  Prunes
 * expired codes opportunistically.
 *
 * @param user_id       Owner of the code (must exist in users).
 * @param provider_hint Optional ("telegram"/"discord"/"slack"/"sms") for
 *                      WebUI display purposes; NULL/empty for no hint.
 * @param code_out      Caller-allocated buffer.
 * @param code_buf_size Must be >= MESSAGING_LINK_CODE_BUF_SIZE.
 *
 * @return MESSAGING_SUCCESS / MESSAGING_FAILURE.
 */
int messaging_engine_generate_link_code(int user_id,
                                        const char *provider_hint,
                                        char *code_out,
                                        size_t code_buf_size);

/**
 * @brief Look up the current state of a link code.
 *
 * @param code The code to check (case-insensitive).
 * @return MESSAGING_LINK_STATE_*.
 */
messaging_link_state_t messaging_engine_link_status(const char *code);

#ifdef __cplusplus
}
#endif

#endif /* MESSAGING_ENGINE_H */
