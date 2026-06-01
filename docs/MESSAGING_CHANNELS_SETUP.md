# Messaging Channels Setup (Telegram / Slack / Discord / SMS)

DAWN can talk to you through the chat apps you already use. Link a Telegram,
Slack, Discord, or SMS conversation to your DAWN account and you get the full
assistant — tools, memory, scheduler, the works — from your phone or desktop
chat client, without opening the WebUI.

Each linked channel is a **forever-conversation**: messages persist into one
conversation that shows up in the WebUI history alongside your voice and web
chats, and DAWN extracts memory from it like any other session. You can reset
the thread at any time with `/new`.

> **SMS here is the messaging-channels path** (link your number, then text
> Friday directly). It is **not** the same as the ECHO phone integration, which
> turns inbound calls/texts into HUD notifications. If you have ECHO set up,
> both can coexist — see [Notes](#notes-and-troubleshooting).

---

## How linking works (all providers)

1. Put the provider's bot/app token in `secrets.toml` and restart DAWN. The
   matching driver only loads when its token is present.
2. Generate a one-time **link code** for your user, either in the WebUI
   **Messaging Channels** settings panel or with the admin CLI:
   ```bash
   ./build/dawn-admin messaging generate-link-code --user <username> [--provider telegram|slack|discord|sms]
   ```
   Codes are 8-character Crockford base32 (no I/L/O/U) and expire in 10 minutes.
3. From the chat app, send `/link CODE` to the bot (Slack users: send
   `link CODE` without the slash — Slack reserves `/` for its own commands).
4. DAWN confirms the link. From then on, every message in that conversation
   reaches the assistant.

Once linked, just talk normally — no wake word needed (except SMS outside its
active window; see [SMS](#sms)). Send `/new` (Slack: ask the assistant to
"start a new conversation") to reset the forever-conversation and start fresh.

---

## Telegram

1. Open [@BotFather](https://t.me/BotFather), send `/newbot`, and follow the
   prompts. BotFather returns a token like `123456:ABC-DEF...`.
2. Add it to `secrets.toml`:
   ```toml
   [secrets]
   telegram_bot_token = "123456:ABC-DEF..."
   ```
3. Restart DAWN, generate a link code, and DM your bot `/link CODE`.

## Discord

v1 is **DM-only, text-only**.

1. Create an application + bot at
   [discord.com/developers/applications](https://discord.com/developers/applications).
2. Under **Bot** settings, enable the **MESSAGE CONTENT INTENT** toggle (a
   privileged intent — required to read DM text).
3. Copy the bot token into `secrets.toml`:
   ```toml
   [secrets]
   discord_bot_token = "..."
   ```
4. Invite the bot so you can DM it, restart DAWN, generate a code, and DM
   `/link CODE`.

## Slack

v1 is **DM-only, single-workspace**, over Socket Mode (no public URL needed).

1. Create an app at [api.slack.com/apps](https://api.slack.com/apps).
2. **Socket Mode** → enable. This generates an **App-Level Token** (`xapp-...`).
3. **OAuth & Permissions** → add bot scopes: `chat:write`, `im:history`,
   `im:read`, `app_mentions:read`. Install the app to your workspace and copy
   the **Bot User OAuth Token** (`xoxb-...`).
4. **Event Subscriptions** → enable, and subscribe to bot events: `message.im`,
   `app_mention`.
5. Add both tokens to `secrets.toml`:
   ```toml
   [secrets]
   slack_app_token = "xapp-..."
   slack_bot_token = "xoxb-..."
   ```
6. Restart DAWN, generate a code, and DM the bot `link CODE` (no leading slash).

## SMS

SMS routes through the **ECHO** cellular daemon (SIM7600G-H modem). Set up ECHO
first (see the [echo repo](https://github.com/The-OASIS-Project/echo)); no token
goes in `secrets.toml` — DAWN registers the `sms` provider automatically when
the messaging engine is running.

1. Generate a link code for your user (with `--provider sms` if you like).
2. From the phone you want to link, text `/link CODE` to your DAWN/ECHO number.
3. After linking, you have an **active-conversation window**: for
   `active_window_sec` seconds after each exchange (default 600 = 10 min),
   replies route straight to the assistant with no wake word. Outside the
   window, an SMS needs the wake word ("Hey Friday, ...") or it falls through to
   the normal HUD-notification path. Tune in `dawn.toml`:
   ```toml
   [messaging.sms]
   active_window_sec = 600   # 0 disables (every SMS then needs a wake word); range 0-86400
   ```

---

## Delivering scheduled events to a channel

Scheduled events — timers, alarms, reminders, tasks, and briefings — can be
delivered to a messaging channel instead of (or in addition to being) spoken
aloud. Ask the assistant from within a channel ("schedule a morning briefing")
and it sets the event's delivery target to the current channel automatically.
When an event has a delivery target, that channel is the destination and the
local TTS/banner are suppressed for it.

---

## Managing channels

**WebUI** — Settings → **Messaging Channels**: list your linked channels,
generate link codes, rename a channel's display name, unlink (soft-delete,
preserving history), and re-enable a previously unlinked channel.

**Operator CLI** — `dawn-admin messaging`:

| Command | Purpose |
|---|---|
| `generate-link-code --user <u> [--provider <p>]` | Issue a one-time link code |
| `list-channels --user <u>` | List a user's linked channels |
| `unlink --user <u> --name <display_name>` | Soft-delete a channel (history preserved) |
| `reenable --user <u> --name <display_name>` | Restore a previously unlinked channel |
| `link-attempts [--provider <p>]` | Review recent `/link` attempts (abuse audit) |

Unlinking sets `is_enabled = 0` but keeps the row and its conversation, so a
later re-link of the same address resumes the prior thread and custom name.

---

## Notes and troubleshooting

- **Driver not loading?** The driver only registers when its token is present in
  `secrets.toml` and DAWN has been restarted. Check the daemon log for
  `registered driver '<provider>'`.
- **`/link` says invalid/expired?** Codes last 10 minutes and are single-use.
  Generate a fresh one. Review attempts with `dawn-admin messaging link-attempts`.
- **Messages from an unlinked sender are ignored** by design — the bot never
  replies to strangers.
- **SMS vs ECHO phone:** the messaging-channels SMS path (this doc) routes your
  texts to the LLM as a conversation. The ECHO phone integration
  (`PHONE_SMS_DESIGN`) surfaces inbound calls/texts as MIRAGE HUD notifications
  with contact photos. An inbound SMS from a linked number inside its active
  window goes to the assistant; otherwise it falls through to the HUD path.
- **Rate limits & length caps** are enforced per channel (inbound flood
  protection, and per-provider outbound length caps with natural-break
  splitting — e.g., SMS is capped and long replies are summarized with an offer
  to continue in the WebUI).

For the design rationale and internals, see the architecture notes in the
project's design docs.
