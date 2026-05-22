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
 * Messaging driver contract (Layer 2).
 *
 * Each chat-app provider (Telegram, Discord, Slack) and the existing SMS
 * path implements this contract.  The engine
 * (src/messaging/messaging_engine.c) registers drivers at init, owns the
 * per-user channel resolution, and routes inbound + outbound messages
 * through this interface.  See docs/MESSAGING_CHANNELS_DESIGN.md §4.
 */
#ifndef MESSAGING_DRIVER_H
#define MESSAGING_DRIVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Callback invoked by a driver when an inbound message arrives.
 *
 * The driver runs this on its listener thread (do NOT block — the
 * engine's bound queue absorbs back-pressure on the worker drain).
 *
 * @param provider          Driver name ("telegram" / "discord" / ...).
 * @param provider_address  Typed provider primary key (chat_id /
 *                          channel_id / phone) as a string.
 * @param sender_display    Human-readable sender name (may be NULL).
 * @param body              Message body (UTF-8 text only — v1 is text-only).
 * @param timestamp         Unix epoch seconds when the provider claims
 *                          the message was sent (0 if unknown).
 *
 * @return 0 on success, non-zero if the engine couldn't enqueue.
 */
typedef int (*messaging_inbound_fn)(const char *provider,
                                    const char *provider_address,
                                    const char *sender_display,
                                    const char *body,
                                    int64_t timestamp);

/**
 * @brief Per-driver function table.
 *
 * Drivers own persistent connections (long-poll loops, Gateway
 * WebSockets, Socket Mode WebSockets) — unlike `embedding_provider_t`
 * (stateless request/response).  The connection-state hooks reflect
 * that.
 */
typedef struct messaging_driver_s {
   /** Driver name — used as the `provider` column value
    *  ("telegram" / "discord" / "slack" / "sms"). */
   const char *name;

   /**
    * Initialize the driver.  Spawns the listener thread, opens the
    * persistent connection, etc.  Returns SUCCESS / FAILURE.  The
    * `credentials_json` blob is provider-specific (e.g.,
    * `{"bot_token": "..."}` for Telegram).
    *
    * Called once at engine init time.
    */
   int (*init)(const char *credentials_json);

   /**
    * Tear the driver down.  Closes the connection, joins the listener
    * thread.  Called at engine shutdown.
    */
   void (*shutdown)(void);

   /**
    * Send a plain-text message to a provider address.
    *
    * @param address_json  Full address blob (driver-defined shape).
    *                      Typically the row's `address_json` column.
    * @param text          UTF-8 message body.  Driver may segment if
    *                      provider has a per-message length cap.
    *
    * @return SUCCESS / FAILURE.  Network errors map to FAILURE; the
    *         engine layer may retry per its rate-limit policy.
    */
   int (*send_text)(const char *address_json, const char *text);

   /**
    * Register the inbound-event sink.  Called once at engine init,
    * before init().  The driver stores the pointer and invokes it for
    * every inbound message.
    *
    * @return SUCCESS / FAILURE.
    */
   int (*register_inbound_cb)(messaging_inbound_fn cb);

   /**
    * Validate that `address_json` has the shape this driver expects
    * BEFORE INSERTing it into messaging_channels.  Catches malformed
    * JSON, missing keys, out-of-range integers, bad E.164, etc.
    *
    * @return SUCCESS if address is well-formed, FAILURE otherwise.
    */
   int (*validate_address)(const char *address_json);

   /**
    * @return 1 if the driver's persistent connection is currently
    *         healthy, 0 otherwise.  Engine uses this for health-check
    *         endpoints; the driver itself handles automatic reconnect.
    */
   int (*is_connected)(void);

   /**
    * Engine may request an explicit reconnect (e.g., after a config
    * change).  The driver tears down and re-establishes the connection.
    *
    * @return SUCCESS / FAILURE.  FAILURE means the driver couldn't
    *         reconnect; engine should mark the driver disabled.
    */
   int (*reconnect)(void);
} messaging_driver_t;

#ifdef __cplusplus
}
#endif

#endif /* MESSAGING_DRIVER_H */
