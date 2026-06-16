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
 * Discord driver — INTERNAL shared surface.  Private to the
 * src/messaging/messaging_discord*.c translation units; lets the
 * channel-history READ path (messaging_discord_read.c) share the bot token,
 * snowflake validation, and REST constants with the gateway/send core
 * (messaging_discord.c) after the file was split for size.  NOT a public API —
 * external code uses include/messaging/messaging_discord.h.
 */
#ifndef MESSAGING_DISCORD_INTERNAL_H
#define MESSAGING_DISCORD_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "messaging/messaging_driver.h" /* messaging_read_window_t */

/* REST surface shared by the send core and the read path. */
#define DC_BOT_TOKEN_MAX 256
#define DC_REST_BASE_URL "https://discord.com/api/v10"
#define DC_USER_AGENT "DAWN-Discord/0.1 (libcurl, libwebsockets)"
#define DC_SNOWFLAKE_MAX_DIGITS 20 /* a 64-bit snowflake is <= 20 digits */

/* Bot token — defined in messaging_discord.c, read by the REST surfaces. */
extern char s_bot_token[DC_BOT_TOKEN_MAX];

/**
 * @brief Validate a Discord snowflake: decimal digits only, <= 20 of them.
 *
 * Shared defense-in-depth gate before any id is interpolated into a REST URL,
 * and a length cap so a downstream strtoull() can't silently saturate.  Inline
 * so both translation units get a copy without a cross-TU symbol.
 */
static inline bool dc_is_valid_snowflake(const char *s) {
   if (!s || !s[0]) {
      return false;
   }
   size_t i;
   for (i = 0; s[i] != '\0'; i++) {
      /* Fail fast on over-length input — don't walk an arbitrarily long
       * attacker-controlled string just to reject it. */
      if (i >= DC_SNOWFLAKE_MAX_DIGITS) {
         return false;
      }
      if (s[i] < '0' || s[i] > '9') {
         return false;
      }
   }
   return true;
}

/* Read path — defined in messaging_discord_read.c, wired into the driver
 * descriptor in messaging_discord.c. */
int dc_list_readable_channels(char **out_json);
int dc_read_history(const char *channel_id, const messaging_read_window_t *window, char **out_json);
void dc_invalidate_channel_cache(void); /* drop discovery cache (recover from miss) */
void dc_read_shutdown(void);            /* free the read CURL handle + discovery cache */

#endif /* MESSAGING_DISCORD_INTERNAL_H */
