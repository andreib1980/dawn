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
 * Per-channel outbound markdown formatter (Layer 2).
 *
 * The LLM emits ONE canonical form — standard CommonMark+GFM markdown — which
 * is stored verbatim in the channel's forever-conversation (so the WebUI can
 * render it with marked.js).  Not every chat platform renders that canonical
 * markdown, and each that does uses a different dialect:
 *
 *   SMS       — plain text only; strip all markup.
 *   Telegram  — native rich via parse_mode=HTML (a restricted tag subset).
 *   Discord   — standard markdown EXCEPT tables (flatten) and masked links
 *               in `content` (unreliable → flatten to "text (url)").
 *   Slack     — its own "mrkdwn" dialect (*bold*, _italic_, <url|text>, ...).
 *
 * This module converts the canonical markdown into a target dialect at the
 * SEND boundary, on a throwaway copy — the stored canonical form is never
 * touched.  Conversion is paired with splitting because the two cannot be
 * separated correctly: the per-part size cap is measured on the CONVERTED
 * output (Telegram's limit is visible chars after entity parsing, not raw
 * HTML bytes), and a balanced-format target (Telegram HTML) must only break
 * where its tag stack is closeable — knowledge that exists only mid-render.
 *
 * Pure function: no DAWN dependencies beyond the strbuf utility and the
 * sibling messaging_split helper (reused for the plain-text path).
 */
#ifndef MESSAGING_FORMAT_H
#define MESSAGING_FORMAT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Target wire format a driver consumes on its outbound send.
 *
 * Declared on `messaging_driver_t` (out_format).  The engine renders every
 * outbound message into the driver's declared format before calling
 * send_text(), so a driver can statically rely on receiving its own dialect
 * (e.g. the Telegram driver hardcodes parse_mode=HTML).
 */
typedef enum {
   MSG_FMT_PLAIN = 0,    /**< SMS — markdown stripped to readable plain text. */
   MSG_FMT_DISCORD,      /**< Discord — standard markdown, tables fenced,
                          *   masked links flattened, headers capped at 3. */
   MSG_FMT_SLACK_MRKDWN, /**< Slack — mrkdwn dialect. */
   MSG_FMT_TELEGRAM_HTML /**< Telegram — parse_mode=HTML tag subset. */
} messaging_format_t;

/**
 * @brief Render canonical markdown into @p fmt, then split into parts each
 *        already in @p fmt, independently valid, and within the per-part cap.
 *
 * The size cap is measured ON THE CONVERTED OUTPUT, never the input markdown —
 * several conversions change length (Discord table→fence and masked-link
 * flattening expand; Slack `&`→`&amp;` expands; the plain-text strip shrinks).
 * Measuring the input would let an expanding conversion overflow the platform
 * limit.  The unit of @p max_part_chars is the unit the target platform counts
 * toward its own limit:
 *   - Telegram: VISIBLE chars after entity parsing (open/close tags = 0,
 *     `&amp;` = 1, one per UTF-8 codepoint for body text).
 *   - Discord / Slack: rendered output length (markup characters included).
 *   - Plain (SMS): stripped UTF-8 byte length.
 *
 * Each emitted part is independently valid in @p fmt: balanced HTML for
 * Telegram (a multi-part split closes open tags in LIFO order at the end of a
 * part and reopens them in FIFO order at the start of the next), and balanced
 * code fences for Discord/Slack.
 *
 * @param markdown        Canonical CommonMark+GFM markdown.  READ-ONLY — the
 *                        function allocates fresh buffers and never writes
 *                        through this pointer, preserving the immutability of
 *                        the stored conversation.  Must be non-NULL.
 * @param fmt             Target dialect.
 * @param max_part_chars  Per-part cap, measured on converted output (see
 *                        above).  Must be > 0.
 * @param parts_out       On SUCCESS, set to a heap array of @p *count_out
 *                        heap NUL-terminated parts.  Free with
 *                        messaging_format_free_parts().  On FAILURE, NULL.
 * @param count_out       On SUCCESS, the part count (always >= 1, and every
 *                        part is non-empty).  On FAILURE, 0.
 * @param err_msg         On FAILURE, a user-facing explanation the caller can
 *                        deliver as a single short message.  Untouched on
 *                        SUCCESS.
 * @param err_size        Size of @p err_msg (256 typical).
 * @return SUCCESS or FAILURE per include/dawn_error.h.
 *
 * Guarantees: SUCCESS implies >= 1 non-empty part (a message that strips to
 * nothing — e.g. only a table — yields a single delivery-only placeholder so
 * downstream never sends an empty body, which Slack/Telegram/Discord reject).
 * An atomic unit larger than the cap that cannot be safely cut returns FAILURE
 * with @p err_msg.  Every error path frees already-produced parts before
 * returning.
 */
int messaging_format_render_split(const char *markdown,
                                  messaging_format_t fmt,
                                  size_t max_part_chars,
                                  char ***parts_out,
                                  size_t *count_out,
                                  char *err_msg,
                                  size_t err_size);

/**
 * @brief Free a parts array returned by messaging_format_render_split().
 *        Frees each part then the array.  NULL-safe.
 */
void messaging_format_free_parts(char **parts, size_t count);

#ifdef __cplusplus
}
#endif

#endif /* MESSAGING_FORMAT_H */
