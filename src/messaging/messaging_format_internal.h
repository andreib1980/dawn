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
 * Internal contract shared between messaging_format.c (tokenizer + the
 * plain/Discord/Slack renderers + generic splitter) and
 * messaging_format_telegram.c (the HTML renderer + tag-stack splitter).
 *
 * Deliberately separate from messaging_engine_internal.h: the formatter is a
 * pure text transform and must NOT see engine state.  The two TUs share only
 * the block model and the format-agnostic inline walker declared here.
 */
#ifndef MESSAGING_FORMAT_INTERNAL_H
#define MESSAGING_FORMAT_INTERNAL_H

#include <stddef.h>

#include "messaging/messaging_format.h"

/* Maximum block-nesting / inline-emphasis recursion depth.  Beyond this the
 * walker stops recursing and emits the remaining markers as literal text —
 * cheap insurance against a stack-overflow crash from degenerate (possibly
 * model- or attacker-influenced) input. */
#define MSGFMT_MAX_NESTING 16

/* Delivery-only part emitted when a message strips to nothing renderable (e.g.
 * it was only a table or a bare header).  Shared by both renderer TUs; never
 * written to conversation history. */
#define MSGFMT_EMPTY_PLACEHOLDER "(see full message in WebUI)"

/* Block kinds produced by the line-oriented tokenizer. */
typedef enum {
   MSGFMT_BLK_PARAGRAPH = 0,
   MSGFMT_BLK_HEADING,
   MSGFMT_BLK_CODE_FENCE,
   MSGFMT_BLK_BLOCKQUOTE,
   MSGFMT_BLK_LIST_ITEM,
   MSGFMT_BLK_TABLE,
   MSGFMT_BLK_HR
} msgfmt_block_type_t;

/* One parsed block.  `text` holds the inline source (markers intact, to be
 * walked) for PARAGRAPH/HEADING/BLOCKQUOTE/LIST_ITEM; the verbatim code
 * interior for CODE_FENCE; the raw row lines for TABLE; NULL for HR.  All
 * heap-owned; release with msgfmt_free_blocks(). */
typedef struct {
   msgfmt_block_type_t type;
   char *text;
   char *lang;        /* CODE_FENCE info string ("python"), or NULL. */
   int heading_level; /* HEADING: raw 1..6 (renderers cap as needed). */
   int list_ordered;  /* LIST_ITEM: 1 = ordered, 0 = bullet. */
   int list_number;   /* LIST_ITEM: parsed ordinal for ordered items. */
   int depth;         /* LIST_ITEM: indent depth (0-based, capped). */
} msgfmt_block_t;

/* Inline emphasis kinds passed to the sink's emph_open/emph_close. */
enum {
   MSGFMT_EMPH_ITALIC = 1,
   MSGFMT_EMPH_BOLD,
   MSGFMT_EMPH_BOLDITALIC,
   MSGFMT_EMPH_STRIKE
};

/*
 * Sink for the format-agnostic inline walker.  Each target dialect supplies a
 * sink; the walker resolves emphasis/code/link structure once and calls these
 * to emit the dialect-specific bytes.  `ctx` is the dialect's own state.
 * `text`/link `text`/`url` point into the source and are NOT NUL-terminated —
 * use the length.
 */
typedef struct msgfmt_inline_sink {
   void *ctx;
   void (*text)(void *ctx, const char *s, size_t n);
   void (*code_span)(void *ctx, const char *s, size_t n);
   void (*emph_open)(void *ctx, int kind);
   void (*emph_close)(void *ctx, int kind);
   void (*link)(void *ctx, const char *text, size_t tn, const char *url, size_t un);
} msgfmt_inline_sink_t;

/* Tokenize canonical markdown into a heap block array (SUCCESS/FAILURE). */
int msgfmt_tokenize(const char *markdown, msgfmt_block_t **blocks_out, size_t *count_out);
void msgfmt_free_blocks(msgfmt_block_t *blocks, size_t count);

/* Walk @p len bytes of inline source, invoking @p sink.  Depth-bounded. */
void msgfmt_walk_inline(const char *text, size_t len, const msgfmt_inline_sink_t *sink);

/* Telegram HTML render+split (implemented in messaging_format_telegram.c). */
int msgfmt_render_split_telegram(const msgfmt_block_t *blocks,
                                 size_t nblocks,
                                 size_t max_visible,
                                 char ***parts_out,
                                 size_t *count_out,
                                 char *err_msg,
                                 size_t err_size);

#endif /* MESSAGING_FORMAT_INTERNAL_H */
