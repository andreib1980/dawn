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
 * Outbound-message splitter for chat-app drivers.
 *
 * When the LLM produces a reply that exceeds a provider's per-message
 * size limit (Discord 2000 chars, Telegram 4096, Slack 4000, SMS 670),
 * this splitter carves the text at natural break points so the engine
 * can deliver it as multiple messages rather than truncating or
 * dropping the response.  Preference order: paragraph (\n\n then \n)
 * → sentence (". " / "! " / "? " with closing-punct variants) →
 * reject as "noise" (no acceptable break within the trailing 30% of
 * the limit).  The engine surfaces a reject as a single short message
 * pointing the user at the WebUI for the full reply.
 *
 * Pure function: no DAWN dependencies (no logging, no DB, no config).
 * Lives at Layer 2 in src/messaging/ rather than Layer 1 src/core/
 * because the break-tier policy and acceptance-window heuristic are
 * messaging-domain choices.  Promote to src/core/text_break.c if a
 * non-messaging consumer adopts the algorithm.
 */
#ifndef MESSAGING_SPLIT_H
#define MESSAGING_SPLIT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Inter-part pacing applied by the engine between successive
 * drv->send_text() calls when delivering a split reply.  100ms keeps
 * us under provider per-channel rate ceilings (Telegram ~1/sec/chat,
 * Slack ~1/sec/channel, Discord burst-tolerant) without making the
 * worker thread feel sluggish for typical 1-3-part splits.  Tests
 * may override before #include to make multi-part tests run fast. */
#ifndef MESSAGING_SPLIT_INTER_PART_USEC
#define MESSAGING_SPLIT_INTER_PART_USEC 100000
#endif

/**
 * Split `text` into chunks each ≤ `max_chars` bytes, preferring (in
 * order) paragraph breaks (`\n\n` then `\n`), then sentence breaks
 * (`. ` / `! ` / `? ` plus closing-punct variants).  An input that
 * already fits returns as a single one-chunk array.
 *
 * Search is restricted to the trailing 30% of each prospective chunk
 * (the "acceptance window") so chunks stay as full as possible.  When
 * no acceptable break exists in the window the function returns
 * FAILURE with an LLM-actionable `err_msg` the caller can deliver to
 * the user as a single short message.
 *
 * @param text         Input string (UTF-8 safe — breaks only occur
 *                     at ASCII bytes).  Must be non-NULL.
 * @param max_chars    Per-chunk byte cap.  Must be > 0.
 * @param parts_out    On SUCCESS, set to a heap-allocated array of
 *                     `*count_out` heap-allocated NUL-terminated
 *                     chunks.  Caller frees each chunk then the array.
 *                     On FAILURE, set to NULL.
 * @param count_out    On SUCCESS, set to the chunk count (≥ 1).
 *                     On FAILURE, set to 0.
 * @param err_msg      On FAILURE, filled with a user-facing
 *                     explanation.  On SUCCESS, left untouched.
 * @param err_size     Size of err_msg buffer (256 typical).
 * @return SUCCESS or FAILURE per include/dawn_error.h.
 */
int messaging_split_at_breaks(const char *text,
                              size_t max_chars,
                              char ***parts_out,
                              size_t *count_out,
                              char *err_msg,
                              size_t err_size);

#ifdef __cplusplus
}
#endif

#endif /* MESSAGING_SPLIT_H */
