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
 * Telegram parse_mode=HTML renderer + tag-stack-aware splitter.
 *
 * Telegram is the only target whose per-message cap is measured in VISIBLE
 * characters (after entity parsing, NOT raw HTML bytes), and whose multi-part
 * splits must keep each part independently well-formed: Telegram returns HTTP
 * 400 and rejects the ENTIRE message on an unclosed or overlapping tag.  So a
 * message is rendered into a flat piece stream (open-tag / close-tag / escaped
 * text-with-visible-length), packed block-by-block (block boundaries leave the
 * tag stack empty, giving naturally balanced breaks), and any single block
 * larger than the cap is split with the tag stack closed in LIFO order at the
 * end of one part and reopened in FIFO order at the start of the next.
 *
 * Allowed tag subset (per core.telegram.org/bots/api#html-style):
 *   <b> <i> <u> <s> <tg-spoiler> <a href> <code> <pre>
 *   <pre><code class="language-x"> <blockquote>
 * Only <code>/<pre> interiors are non-nestable; everything the walker emits
 * inside them is plain escaped text, so that invariant holds by construction.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/strbuf.h"
#include "dawn_error.h"
#include "messaging_format_internal.h"

/* Maximum byte length scanned for an HTML entity reference (&...;) when
 * counting visible characters.  The longest standard named entity is ~10
 * chars; 12 covers it with the &/; delimiters plus headroom. */
#define TG_MAX_ENTITY_SCAN 12

/* ======================================================================== */
/* Escaping + counting helpers                                               */
/* ======================================================================== */

/* Count UTF-8 codepoints in [s, s+n) — one per VISIBLE Telegram character. */
static size_t utf8_count(const char *s, size_t n) {
   size_t c = 0;
   for (size_t i = 0; i < n;) {
      unsigned char ch = (unsigned char)s[i];
      if (ch < 0x80) {
         i += 1;
      } else if ((ch >> 5) == 0x6) {
         i += 2;
      } else if ((ch >> 4) == 0xE) {
         i += 3;
      } else if ((ch >> 3) == 0x1E) {
         i += 4;
      } else {
         i += 1;
      }
      c++;
   }
   return c;
}

/* Append [s,s+n) HTML-escaped (< > &) to a strbuf. */
static void html_escape(strbuf_t *out, const char *s, size_t n) {
   for (size_t i = 0; i < n; i++) {
      switch (s[i]) {
         case '&':
            strbuf_append(out, "&amp;");
            break;
         case '<':
            strbuf_append(out, "&lt;");
            break;
         case '>':
            strbuf_append(out, "&gt;");
            break;
         default:
            strbuf_append_n(out, s + i, 1);
            break;
      }
   }
}

static char *html_escape_dup(const char *s, size_t n) {
   strbuf_t sb;
   strbuf_init(&sb, n + 8);
   html_escape(&sb, s, n);
   return strbuf_steal(&sb);
}

/* Append [s,s+n) escaped for a double-quoted HTML attribute value: < > & AND
 * the quote " (else a " in the value breaks out of the attribute and Telegram
 * rejects the whole message). */
static void html_escape_attr(strbuf_t *out, const char *s, size_t n) {
   for (size_t i = 0; i < n; i++) {
      switch (s[i]) {
         case '&':
            strbuf_append(out, "&amp;");
            break;
         case '<':
            strbuf_append(out, "&lt;");
            break;
         case '>':
            strbuf_append(out, "&gt;");
            break;
         case '"':
            strbuf_append(out, "&quot;");
            break;
         default:
            strbuf_append_n(out, s + i, 1);
            break;
      }
   }
}

/* Escape a URL for an href attribute value (adds " on top of < > &). */
static char *href_escape_dup(const char *s, size_t n) {
   strbuf_t sb;
   strbuf_init(&sb, n + 8);
   for (size_t i = 0; i < n; i++) {
      switch (s[i]) {
         case '&':
            strbuf_append(&sb, "&amp;");
            break;
         case '<':
            strbuf_append(&sb, "&lt;");
            break;
         case '>':
            strbuf_append(&sb, "&gt;");
            break;
         case '"':
            strbuf_append(&sb, "&quot;");
            break;
         default:
            strbuf_append_n(&sb, s + i, 1);
            break;
      }
   }
   return strbuf_steal(&sb);
}

/* Bytes consumed from escaped text @p s to advance @p want VISIBLE units,
 * never splitting an entity (&...;) or a UTF-8 codepoint.  Sets
 * *vis_consumed. */
static size_t bytes_for_visible(const char *s,
                                size_t len,
                                size_t start,
                                size_t want,
                                size_t *vis_consumed) {
   size_t i = start;
   size_t v = 0;
   while (i < len && v < want) {
      unsigned char ch = (unsigned char)s[i];
      if (ch == '&') {
         size_t j = i + 1;
         while (j < len && s[j] != ';' && (j - i) < TG_MAX_ENTITY_SCAN) {
            j++;
         }
         i = (j < len && s[j] == ';') ? j + 1 : i + 1;
      } else if (ch < 0x80) {
         i += 1;
      } else {
         size_t cl = ((ch >> 5) == 0x6) ? 2 : ((ch >> 4) == 0xE) ? 3 : ((ch >> 3) == 0x1E) ? 4 : 1;
         if (i + cl > len) {
            cl = len - i;
         }
         i += cl;
      }
      v++;
   }
   *vis_consumed = v;
   return i - start;
}

/* ======================================================================== */
/* Piece stream                                                              */
/* ======================================================================== */

typedef enum {
   TG_OPEN,
   TG_CLOSE,
   TG_TEXT
} tg_kind_t;

typedef struct {
   tg_kind_t kind;
   char *a;        /* OPEN: open tag; CLOSE: close tag; TEXT: escaped bytes */
   char *b;        /* OPEN: matching close tag; else NULL */
   size_t visible; /* TEXT: visible length; else 0 */
} tg_piece_t;

typedef struct {
   tg_piece_t *items;
   size_t n;
   size_t cap;
   int oom;
} tg_builder_t;

static void tgb_init(tg_builder_t *b) {
   b->items = NULL;
   b->n = 0;
   b->cap = 0;
   b->oom = 0;
}

static tg_piece_t *tgb_slot(tg_builder_t *b) {
   if (b->n == b->cap) {
      size_t ncap = b->cap ? b->cap * 2 : 16;
      tg_piece_t *ni = (tg_piece_t *)realloc(b->items, ncap * sizeof(*ni));
      if (!ni) {
         b->oom = 1;
         return NULL;
      }
      b->items = ni;
      b->cap = ncap;
   }
   return &b->items[b->n++];
}

static void tgb_open(tg_builder_t *b, const char *open, const char *close) {
   tg_piece_t *p = tgb_slot(b);
   if (!p) {
      return;
   }
   p->kind = TG_OPEN;
   p->a = strdup(open);
   p->b = strdup(close);
   p->visible = 0;
   if (!p->a || !p->b) {
      b->oom = 1;
   }
}

/* Like tgb_open but takes ownership of an already-allocated open string. */
static void tgb_open_owned(tg_builder_t *b, char *open, const char *close) {
   tg_piece_t *p = tgb_slot(b);
   if (!p) {
      free(open);
      return;
   }
   p->kind = TG_OPEN;
   p->a = open;
   p->b = strdup(close);
   p->visible = 0;
   if (!p->a || !p->b) {
      b->oom = 1;
   }
}

static void tgb_close(tg_builder_t *b, const char *close) {
   tg_piece_t *p = tgb_slot(b);
   if (!p) {
      return;
   }
   p->kind = TG_CLOSE;
   p->a = strdup(close);
   p->b = NULL;
   p->visible = 0;
   if (!p->a) {
      b->oom = 1;
   }
}

/* Push escaped text from raw [s,n); visible counted on the raw codepoints. */
static void tgb_text(tg_builder_t *b, const char *s, size_t n) {
   if (n == 0) {
      return;
   }
   tg_piece_t *p = tgb_slot(b);
   if (!p) {
      return;
   }
   p->kind = TG_TEXT;
   p->a = html_escape_dup(s, n);
   p->b = NULL;
   p->visible = utf8_count(s, n);
   if (!p->a) {
      b->oom = 1;
   }
}

static void tgb_free(tg_builder_t *b) {
   for (size_t i = 0; i < b->n; i++) {
      free(b->items[i].a);
      free(b->items[i].b);
   }
   free(b->items);
   tgb_init(b);
}

/* ======================================================================== */
/* Inline sink → pieces                                                      */
/* ======================================================================== */

static void tg_emit_emph(tg_builder_t *b, int kind, int open) {
   switch (kind) {
      case MSGFMT_EMPH_ITALIC:
         open ? tgb_open(b, "<i>", "</i>") : tgb_close(b, "</i>");
         break;
      case MSGFMT_EMPH_BOLD:
         open ? tgb_open(b, "<b>", "</b>") : tgb_close(b, "</b>");
         break;
      case MSGFMT_EMPH_BOLDITALIC:
         if (open) {
            tgb_open(b, "<b>", "</b>");
            tgb_open(b, "<i>", "</i>");
         } else {
            tgb_close(b, "</i>");
            tgb_close(b, "</b>");
         }
         break;
      case MSGFMT_EMPH_STRIKE:
         open ? tgb_open(b, "<s>", "</s>") : tgb_close(b, "</s>");
         break;
      default:
         break;
   }
}

static void sink_text(void *ctx, const char *s, size_t n) {
   tgb_text((tg_builder_t *)ctx, s, n);
}
static void sink_code_span(void *ctx, const char *s, size_t n) {
   tg_builder_t *b = (tg_builder_t *)ctx;
   tgb_open(b, "<code>", "</code>");
   tgb_text(b, s, n);
   tgb_close(b, "</code>");
}
static void sink_emph_open(void *ctx, int kind) {
   tg_emit_emph((tg_builder_t *)ctx, kind, 1);
}
static void sink_emph_close(void *ctx, int kind) {
   tg_emit_emph((tg_builder_t *)ctx, kind, 0);
}
static void sink_link(void *ctx, const char *text, size_t tn, const char *url, size_t un) {
   tg_builder_t *b = (tg_builder_t *)ctx;
   char *href = href_escape_dup(url, un);
   if (!href) {
      b->oom = 1;
      return;
   }
   strbuf_t open;
   strbuf_init(&open, un + 16);
   strbuf_append(&open, "<a href=\"");
   strbuf_append(&open, href);
   strbuf_append(&open, "\">");
   free(href);
   char *open_str = strbuf_steal(&open);
   if (!open_str) {
      b->oom = 1;
      return;
   }
   tgb_open_owned(b, open_str, "</a>");
   tgb_text(b, text, tn);
   tgb_close(b, "</a>");
}

static void tg_walk(tg_builder_t *b, const char *src, size_t len) {
   msgfmt_inline_sink_t sink = { .ctx = b,
                                 .text = sink_text,
                                 .code_span = sink_code_span,
                                 .emph_open = sink_emph_open,
                                 .emph_close = sink_emph_close,
                                 .link = sink_link };
   msgfmt_walk_inline(src, len, &sink);
}

/* ======================================================================== */
/* Block → pieces                                                            */
/* ======================================================================== */

static void render_block_tg(const msgfmt_block_t *blk, tg_builder_t *b) {
   switch (blk->type) {
      case MSGFMT_BLK_HEADING:
         /* Telegram HTML has no heading tag.  Render headers bold + underlined
          * so they read as headers, distinct from inline **bold**.  The
          * combined <b><u>..</u></b> is one stack entry — properly nested, and
          * the splitter re-emits both tags together on a close/reopen. */
         tgb_open(b, "<b><u>", "</u></b>");
         tg_walk(b, blk->text, strlen(blk->text));
         tgb_close(b, "</u></b>");
         break;
      case MSGFMT_BLK_PARAGRAPH:
         tg_walk(b, blk->text, strlen(blk->text));
         break;
      case MSGFMT_BLK_BLOCKQUOTE: {
         tgb_open(b, "<blockquote>", "</blockquote>");
         const char *p = blk->text;
         bool first = true;
         while (true) {
            const char *nl = strchr(p, '\n');
            size_t len = nl ? (size_t)(nl - p) : strlen(p);
            if (!first) {
               tgb_text(b, "\n", 1);
            }
            first = false;
            tg_walk(b, p, len);
            if (!nl) {
               break;
            }
            p = nl + 1;
         }
         tgb_close(b, "</blockquote>");
         break;
      }
      case MSGFMT_BLK_CODE_FENCE:
         if (blk->lang) {
            strbuf_t open;
            strbuf_init(&open, 0);
            strbuf_append(&open, "<pre><code class=\"language-");
            html_escape_attr(&open, blk->lang, strlen(blk->lang));
            strbuf_append(&open, "\">");
            char *open_str = strbuf_steal(&open);
            if (open_str) {
               tgb_open_owned(b, open_str, "</code></pre>");
            } else {
               b->oom = 1;
            }
         } else {
            tgb_open(b, "<pre>", "</pre>");
         }
         tgb_text(b, blk->text, strlen(blk->text));
         if (blk->lang) {
            tgb_close(b, "</code></pre>");
         } else {
            tgb_close(b, "</pre>");
         }
         break;
      case MSGFMT_BLK_LIST_ITEM: {
         strbuf_t pre;
         strbuf_init(&pre, 0);
         for (int d = 0; d < blk->depth; d++) {
            strbuf_append(&pre, "  ");
         }
         if (blk->list_ordered) {
            strbuf_appendf(&pre, "%d. ", blk->list_number);
         } else {
            strbuf_append(&pre, "\xe2\x80\xa2 ");
         }
         const char *ps = strbuf_str(&pre);
         tgb_text(b, ps, strlen(ps));
         strbuf_free(&pre);
         tg_walk(b, blk->text, strlen(blk->text));
         break;
      }
      case MSGFMT_BLK_TABLE:
         tgb_open(b, "<pre>", "</pre>");
         tgb_text(b, blk->text, strlen(blk->text));
         tgb_close(b, "</pre>");
         break;
      case MSGFMT_BLK_HR:
         tgb_text(b, "---", 3);
         break;
      default:
         break;
   }
}

static size_t pieces_visible(const tg_builder_t *b) {
   size_t v = 0;
   for (size_t i = 0; i < b->n; i++) {
      v += b->items[i].visible;
   }
   return v;
}

static char *pieces_concat(const tg_builder_t *b) {
   strbuf_t sb;
   strbuf_init(&sb, 0);
   for (size_t i = 0; i < b->n; i++) {
      strbuf_append(&sb, b->items[i].a);
   }
   return strbuf_steal(&sb);
}

/* ======================================================================== */
/* Parts array                                                               */
/* ======================================================================== */

typedef struct {
   char **items;
   size_t n;
   size_t cap;
} tg_parts_t;

static int parts_push(tg_parts_t *p, char *owned) {
   if (!owned) {
      return FAILURE;
   }
   if (p->n == p->cap) {
      size_t ncap = p->cap ? p->cap * 2 : 8;
      char **ni = (char **)realloc(p->items, ncap * sizeof(*ni));
      if (!ni) {
         free(owned);
         return FAILURE;
      }
      p->items = ni;
      p->cap = ncap;
   }
   p->items[p->n++] = owned;
   return SUCCESS;
}

static void parts_free(tg_parts_t *p) {
   for (size_t i = 0; i < p->n; i++) {
      free(p->items[i]);
   }
   free(p->items);
   p->items = NULL;
   p->n = p->cap = 0;
}

/* ======================================================================== */
/* Tag-stack split of a single oversized block                               */
/* ======================================================================== */

static void close_stack_lifo(strbuf_t *cur,
                             const tg_piece_t *pieces,
                             const size_t *stack,
                             size_t sn) {
   for (size_t k = sn; k-- > 0;) {
      strbuf_append(cur, pieces[stack[k]].b);
   }
}
static void reopen_stack_fifo(strbuf_t *cur,
                              const tg_piece_t *pieces,
                              const size_t *stack,
                              size_t sn) {
   for (size_t k = 0; k < sn; k++) {
      strbuf_append(cur, pieces[stack[k]].a);
   }
}

static int tg_split_block(const tg_piece_t *pieces, size_t np, size_t cap, tg_parts_t *parts) {
   strbuf_t cur;
   strbuf_init(&cur, 0);
   size_t cur_vis = 0;
   size_t *stack = NULL;
   size_t sn = 0, scap = 0;
   int rc = SUCCESS;

   for (size_t i = 0; i < np && rc == SUCCESS; i++) {
      const tg_piece_t *pc = &pieces[i];
      if (pc->kind == TG_OPEN) {
         strbuf_append(&cur, pc->a);
         if (sn == scap) {
            size_t ncap = scap ? scap * 2 : 16;
            size_t *ns = (size_t *)realloc(stack, ncap * sizeof(*ns));
            if (!ns) {
               rc = FAILURE;
               break;
            }
            stack = ns;
            scap = ncap;
         }
         stack[sn++] = i;
      } else if (pc->kind == TG_CLOSE) {
         strbuf_append(&cur, pc->a);
         if (sn > 0) {
            sn--;
         }
      } else {
         /* Stream this TEXT piece, breaking when the running visible count
          * hits the cap.  An entity (&amp;) or UTF-8 codepoint is never split:
          * bytes_for_visible advances whole visible units only. */
         const char *tb = pc->a;
         size_t tlen = strlen(tb);
         size_t boff = 0;
         size_t remvis = pc->visible;
         while (remvis > 0 && rc == SUCCESS) {
            size_t avail = cap > cur_vis ? cap - cur_vis : 0;
            if (avail == 0) {
               close_stack_lifo(&cur, pieces, stack, sn);
               if (parts_push(parts, strbuf_steal(&cur)) != SUCCESS) {
                  rc = FAILURE;
                  break;
               }
               strbuf_init(&cur, 0);
               reopen_stack_fifo(&cur, pieces, stack, sn);
               cur_vis = 0;
               continue;
            }
            size_t want = avail < remvis ? avail : remvis;
            size_t vcons = 0;
            size_t bcons = bytes_for_visible(tb, tlen, boff, want, &vcons);
            strbuf_append_n(&cur, tb + boff, bcons);
            boff += bcons;
            cur_vis += vcons;
            remvis -= vcons;
            if (remvis > 0) {
               close_stack_lifo(&cur, pieces, stack, sn);
               if (parts_push(parts, strbuf_steal(&cur)) != SUCCESS) {
                  rc = FAILURE;
                  break;
               }
               strbuf_init(&cur, 0);
               reopen_stack_fifo(&cur, pieces, stack, sn);
               cur_vis = 0;
            }
         }
      }
   }

   if (rc == SUCCESS && cur_vis > 0) {
      if (parts_push(parts, strbuf_steal(&cur)) != SUCCESS) {
         rc = FAILURE;
      }
   }
   strbuf_free(&cur);
   free(stack);
   return rc;
}

/* ======================================================================== */
/* Block-by-block packing                                                    */
/* ======================================================================== */

static const char *tg_block_sep(msgfmt_block_type_t prev, msgfmt_block_type_t cur) {
   if (prev == MSGFMT_BLK_LIST_ITEM && cur == MSGFMT_BLK_LIST_ITEM) {
      return "\n";
   }
   return "\n\n";
}

int msgfmt_render_split_telegram(const msgfmt_block_t *blocks,
                                 size_t nblocks,
                                 size_t max_visible,
                                 char ***parts_out,
                                 size_t *count_out,
                                 char *err_msg,
                                 size_t err_size) {
   tg_parts_t parts = { 0 };
   strbuf_t cur;
   strbuf_init(&cur, 0);
   size_t cur_vis = 0;
   bool have_prev = false;
   msgfmt_block_type_t prev_type = MSGFMT_BLK_PARAGRAPH;
   int rc = SUCCESS;

   for (size_t i = 0; i < nblocks && rc == SUCCESS; i++) {
      tg_builder_t b;
      tgb_init(&b);
      render_block_tg(&blocks[i], &b);
      if (b.oom) {
         tgb_free(&b);
         rc = FAILURE;
         break;
      }
      size_t bvis = pieces_visible(&b);
      if (bvis == 0 && b.n == 0) {
         tgb_free(&b);
         continue;
      }

      const char *sep = (have_prev && cur_vis > 0) ? tg_block_sep(prev_type, blocks[i].type) : "";
      size_t sepvis = strlen(sep); /* "\n" / "\n\n" are ASCII => bytes == visible */

      if (cur_vis + sepvis + bvis <= max_visible) {
         char *bytes = pieces_concat(&b);
         if (!bytes) {
            tgb_free(&b);
            rc = FAILURE;
            break;
         }
         strbuf_append(&cur, sep);
         strbuf_append(&cur, bytes);
         free(bytes);
         cur_vis += sepvis + bvis;
      } else {
         if (cur_vis > 0) {
            if (parts_push(&parts, strbuf_steal(&cur)) != SUCCESS) {
               tgb_free(&b);
               rc = FAILURE;
               break;
            }
            strbuf_init(&cur, 0);
            cur_vis = 0;
         }
         if (bvis <= max_visible) {
            char *bytes = pieces_concat(&b);
            if (!bytes) {
               tgb_free(&b);
               rc = FAILURE;
               break;
            }
            strbuf_append(&cur, bytes);
            free(bytes);
            cur_vis = bvis;
         } else {
            rc = tg_split_block(b.items, b.n, max_visible, &parts);
         }
      }
      have_prev = true;
      prev_type = blocks[i].type;
      tgb_free(&b);
   }

   if (rc == SUCCESS && cur_vis > 0) {
      if (parts_push(&parts, strbuf_steal(&cur)) != SUCCESS) {
         rc = FAILURE;
      }
   }
   strbuf_free(&cur);

   if (rc != SUCCESS) {
      parts_free(&parts);
      if (err_msg && err_size) {
         snprintf(err_msg, err_size, "Out of memory formatting the reply.");
      }
      *parts_out = NULL;
      *count_out = 0;
      return FAILURE;
   }

   if (parts.n == 0) {
      if (parts_push(&parts, strdup(MSGFMT_EMPTY_PLACEHOLDER)) != SUCCESS) {
         parts_free(&parts);
         if (err_msg && err_size) {
            snprintf(err_msg, err_size, "Out of memory formatting the reply.");
         }
         *parts_out = NULL;
         *count_out = 0;
         return FAILURE;
      }
   }

   *parts_out = parts.items;
   *count_out = parts.n;
   return SUCCESS;
}
