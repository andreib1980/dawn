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
 * Telegram driver — public registration entry point.
 */
#ifndef MESSAGING_TELEGRAM_H
#define MESSAGING_TELEGRAM_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register and initialize the Telegram driver with the messaging
 *        engine.  No-op when bot_token is NULL or empty.
 *
 * Must be called AFTER messaging_engine_init().  Spawns a listener
 * thread on a 64 KB stack that runs the getUpdates long-poll loop.
 *
 * @param bot_token  Bot token from @BotFather (stored in secrets.toml
 *                   as telegram_bot_token).
 * @return SUCCESS / FAILURE.
 */
int messaging_telegram_register(const char *bot_token);

/**
 * @brief Tear the driver down.  Joins the listener thread.
 */
void messaging_telegram_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* MESSAGING_TELEGRAM_H */
