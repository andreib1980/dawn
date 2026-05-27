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
 * @return SUCCESS if the engine accepted the event, FAILURE if it
 *         couldn't enqueue (queue full, body too long, gate rejected,
 *         etc.).  Drivers MAY ignore the return — the engine's
 *         response semantics are downstream of enqueue.
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
    * @param user_id           The DAWN user the send is acting on
    *                          behalf of.  Drivers that scope per-user
    *                          state (SMS audit logs, rate-limit
    *                          buckets in phone_service) consume this;
    *                          drivers that don't (Telegram bot tokens
    *                          are bot-wide) ignore it.  Must be > 0.
    * @param provider_address  Typed primary key for this driver
    *                          (chat_id / phone_e164 / channel_id).
    *                          Drivers that need ONLY this can skip
    *                          parsing `address_json`.  Must be
    *                          non-NULL.
    * @param address_json      Full address blob with provider extras
    *                          (e.g. Discord's guild_id, Slack's
    *                          team_id, Telegram's reply_to overrides).
    *                          May be NULL or "{}" for providers with
    *                          no extras (SMS, Telegram MVP).  Driver
    *                          parses only when it needs an extra.
    * @param text              UTF-8 message body.  Driver may segment
    *                          if provider has a per-message length cap.
    *
    * @return SUCCESS / FAILURE.  Network errors map to FAILURE; the
    *         engine layer may retry per its rate-limit policy.
    */
   int (*send_text)(int user_id,
                    const char *provider_address,
                    const char *address_json,
                    const char *text);

   /**
    * Build the canonical address_json blob for this driver given a
    * provider_address.  Used by the engine when it has only a typed
    * primary key (e.g., the inbound dispatcher's process_inbound,
    * /link confirmations, /new replies) and needs to construct the
    * full JSON shape for downstream consumers.  Each driver owns the
    * shape of its own JSON — eliminates the engine's strcmp-on-provider
    * switch and lets Phase 3 (Discord) add a new driver without
    * touching the engine.
    *
    * Drivers SHOULD write `{}` (or the minimal valid shape) when
    * `provider_address` is the only data available.  Buffer too small
    * → caller's responsibility; drivers SHOULD null-terminate within
    * `buf_size`.
    *
    * @param provider_address  Typed primary key.
    * @param buf               Caller-provided buffer.
    * @param buf_size          Buffer size (recommended >= 256).
    */
   void (*build_address_json)(const char *provider_address, char *buf, size_t buf_size);

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

   /**
    * OPTIONAL — fire a "typing" indicator to the recipient so they see
    * "Bot is typing..." while the engine processes the inbound message.
    * Fire-and-forget: failures (network, throttling) are NOT propagated.
    *
    * Engine spawns a keepalive thread for the lifetime of LLM
    * processing and re-fires this every ~4 seconds (stays under
    * Telegram's ~5s + Discord's ~10s indicator timeouts).
    *
    * Drivers without a "typing" concept (SMS, future Slack via bot
    * tokens) leave this NULL.
    *
    * @param user_id           DAWN user the typing is on behalf of.
    *                          Drivers with bot-wide tokens may ignore.
    * @param provider_address  Typed primary key (chat_id /
    *                          channel_id).  Must be non-NULL/empty.
    * @param address_json      Full address blob — drivers that need
    *                          extras consult this.  May be NULL or
    *                          "{}" otherwise.
    */
   void (*send_typing)(int user_id, const char *provider_address, const char *address_json);
} messaging_driver_t;

#ifdef __cplusplus
}
#endif

#endif /* MESSAGING_DRIVER_H */
