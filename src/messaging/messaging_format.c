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
 * Per-channel outbound markdown formatter — tokenizer, the format-agnostic
 * inline walker, the plain/Discord/Slack renderers, and the generic
 * block-by-block splitter.  The Telegram HTML renderer + tag-stack splitter
 * live in messaging_format_telegram.c (it needs visible-length accounting and
 * balanced-tag close/reopen that the byte-oriented generic splitter doesn't).
 *
 * See include/messaging/messaging_format.h for the public contract.
 */

#include "messaging/messaging_format.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/strbuf.h"
#include "dawn_error.h"
#include "messaging/messaging_split.h"
#include "messaging_format_internal.h"

/* ======================================================================== */
/* Small lexical helpers                                                     */
/* ======================================================================== */

static size_t line_length(const char *p) {
   const char *nl = strchr(p, '\n');
   return nl ? (size_t)(nl - p) : strlen(p);
}

/* Count leading ASCII spaces (tabs counted as one). */
static size_t leading_indent(const char *p, size_t len) {
   size_t i = 0;
   while (i < len && (p[i] == ' ' || p[i] == '\t')) {
      i++;
   }
   return i;
}

static bool line_is_blank(const char *p, size_t len) {
   for (size_t i = 0; i < len; i++) {
      if (!isspace((unsigned char)p[i])) {
         return false;
      }
   }
   return true;
}

/* True when the trimmed line is a thematic break: >=3 of the same -, * or _
 * (spaces allowed between), nothing else, and NO '|' (which would make it a
 * table separator instead). */
static bool line_is_hr(const char *p, size_t len) {
   size_t i = leading_indent(p, len);
   if (i >= len) {
      return false;
   }
   char c = p[i];
   if (c != '-' && c != '*' && c != '_') {
      return false;
   }
   int count = 0;
   for (; i < len; i++) {
      if (p[i] == c) {
         count++;
      } else if (p[i] == ' ' || p[i] == '\t') {
         continue;
      } else {
         return false;
      }
   }
   return count >= 3;
}

/* True when the trimmed line is a GFM table delimiter row: only |, -, :, and
 * whitespace, with at least one '-'. */
static bool line_is_table_sep(const char *p, size_t len) {
   bool saw_dash = false;
   bool saw_pipe = false;
   for (size_t i = 0; i < len; i++) {
      char c = p[i];
      if (c == '-') {
         saw_dash = true;
      } else if (c == '|') {
         saw_pipe = true;
      } else if (c == ':' || c == ' ' || c == '\t') {
         continue;
      } else {
         return false;
      }
   }
   return saw_dash && saw_pipe;
}

/* ATX heading: 1..6 '#' then a space (or end of line).  Sets *level and the
 * text span. */
static bool parse_heading(const char *p,
                          size_t len,
                          int *level,
                          const char **text,
                          size_t *text_len) {
   size_t i = leading_indent(p, len);
   int hashes = 0;
   while (i < len && p[i] == '#') {
      hashes++;
      i++;
   }
   if (hashes < 1 || hashes > 6) {
      return false;
   }
   if (i < len && p[i] != ' ' && p[i] != '\t') {
      return false; /* "#text" with no space is not a heading. */
   }
   while (i < len && (p[i] == ' ' || p[i] == '\t')) {
      i++;
   }
   size_t end = len;
   /* Drop trailing spaces and a closing run of '#'. */
   while (end > i && (p[end - 1] == ' ' || p[end - 1] == '\t')) {
      end--;
   }
   while (end > i && p[end - 1] == '#') {
      end--;
   }
   while (end > i && (p[end - 1] == ' ' || p[end - 1] == '\t')) {
      end--;
   }
   *level = hashes;
   *text = p + i;
   *text_len = end - i;
   return true;
}

/* Unordered ("- " / "* " / "+ ") or ordered ("12. " / "12) ") list item. */
static bool parse_list_item(const char *p,
                            size_t len,
                            int *ordered,
                            int *number,
                            int *depth,
                            const char **text,
                            size_t *text_len) {
   size_t indent = leading_indent(p, len);
   size_t i = indent;
   if (i >= len) {
      return false;
   }
   if (p[i] == '-' || p[i] == '*' || p[i] == '+') {
      if (i + 1 >= len || (p[i + 1] != ' ' && p[i + 1] != '\t')) {
         return false;
      }
      *ordered = 0;
      *number = 0;
      i += 2;
   } else if (isdigit((unsigned char)p[i])) {
      int num = 0;
      size_t d = i;
      while (d < len && isdigit((unsigned char)p[d])) {
         num = num * 10 + (p[d] - '0');
         d++;
      }
      if (d >= len || (p[d] != '.' && p[d] != ')')) {
         return false;
      }
      d++;
      if (d >= len || (p[d] != ' ' && p[d] != '\t')) {
         return false;
      }
      *ordered = 1;
      *number = num;
      i = d + 1;
   } else {
      return false;
   }
   while (i < len && (p[i] == ' ' || p[i] == '\t')) {
      i++;
   }
   int dep = (int)(indent / 2);
   if (dep > MSGFMT_MAX_NESTING) {
      dep = MSGFMT_MAX_NESTING;
   }
   *depth = dep;
   *text = p + i;
   *text_len = len - i;
   return true;
}

/* True if a line begins a non-paragraph block (used to terminate a paragraph
 * run).  Tables are detected separately at the top-level scan. */
static bool line_starts_block(const char *p, size_t len) {
   size_t i = leading_indent(p, len);
   if (i >= len) {
      return true; /* blank ends the paragraph */
   }
   const char *t = p + i;
   size_t tl = len - i;
   if (tl >= 3 && (strncmp(t, "```", 3) == 0 || strncmp(t, "~~~", 3) == 0)) {
      return true;
   }
   int lvl;
   const char *htext;
   size_t hlen;
   if (parse_heading(p, len, &lvl, &htext, &hlen)) {
      return true;
   }
   if (line_is_hr(p, len)) {
      return true;
   }
   if (t[0] == '>') {
      return true;
   }
   int ord, num, dep;
   const char *ltext;
   size_t llen;
   if (parse_list_item(p, len, &ord, &num, &dep, &ltext, &llen)) {
      return true;
   }
   return false;
}

/* ======================================================================== */
/* Block array management                                                    */
/* ======================================================================== */

static char *dupn(const char *s, size_t n) {
   char *out = (char *)malloc(n + 1);
   if (!out) {
      return NULL;
   }
   memcpy(out, s, n);
   out[n] = '\0';
   return out;
}

static int blocks_push(msgfmt_block_t **arr, size_t *n, size_t *cap, msgfmt_block_t b) {
   if (*n == *cap) {
      size_t ncap = *cap ? *cap * 2 : 8;
      msgfmt_block_t *na = (msgfmt_block_t *)realloc(*arr, ncap * sizeof(**arr));
      if (!na) {
         return FAILURE;
      }
      *arr = na;
      *cap = ncap;
   }
   (*arr)[(*n)++] = b;
   return SUCCESS;
}

void msgfmt_free_blocks(msgfmt_block_t *blocks, size_t count) {
   if (!blocks) {
      return;
   }
   for (size_t i = 0; i < count; i++) {
      free(blocks[i].text);
      free(blocks[i].lang);
   }
   free(blocks);
}

/* ======================================================================== */
/* Tokenizer                                                                 */
/* ======================================================================== */

int msgfmt_tokenize(const char *markdown, msgfmt_block_t **blocks_out, size_t *count_out) {
   if (!markdown || !blocks_out || !count_out) {
      return FAILURE;
   }
   *blocks_out = NULL;
   *count_out = 0;

   msgfmt_block_t *arr = NULL;
   size_t n = 0;
   size_t cap = 0;
   const char *p = markdown;

   while (*p) {
      size_t len = line_length(p);
      const char *next = p[len] ? p + len + 1 : p + len;

      if (line_is_blank(p, len)) {
         p = next;
         continue;
      }

      size_t indent = leading_indent(p, len);
      const char *t = p + indent;
      size_t tl = len - indent;

      /* --- fenced code --- */
      if (tl >= 3 && (strncmp(t, "```", 3) == 0 || strncmp(t, "~~~", 3) == 0)) {
         char fence = t[0];
         size_t fence_len = 0;
         while (fence_len < tl && t[fence_len] == fence) {
            fence_len++;
         }
         /* info string = remainder of the opening line, trimmed */
         const char *info = t + fence_len;
         size_t info_len = tl - fence_len;
         while (info_len && (*info == ' ' || *info == '\t')) {
            info++;
            info_len--;
         }
         while (info_len && (info[info_len - 1] == ' ' || info[info_len - 1] == '\t')) {
            info_len--;
         }

         strbuf_t code;
         strbuf_init(&code, 0);
         const char *q = next;
         bool closed = false;
         while (*q) {
            size_t qlen = line_length(q);
            const char *qnext = q[qlen] ? q + qlen + 1 : q + qlen;
            size_t qind = leading_indent(q, qlen);
            const char *qt = q + qind;
            size_t qtl = qlen - qind;
            size_t run = 0;
            while (run < qtl && qt[run] == fence) {
               run++;
            }
            bool only_fence = (run >= fence_len);
            for (size_t k = run; only_fence && k < qtl; k++) {
               if (qt[k] != ' ' && qt[k] != '\t') {
                  only_fence = false;
               }
            }
            if (only_fence) {
               q = qnext;
               closed = true;
               break;
            }
            if (strbuf_len(&code) > 0) {
               strbuf_append(&code, "\n");
            }
            strbuf_append_n(&code, q, qlen);
            q = qnext;
         }
         (void)closed;

         msgfmt_block_t b = { 0 };
         b.type = MSGFMT_BLK_CODE_FENCE;
         b.text = strbuf_steal(&code);
         b.lang = info_len ? dupn(info, info_len) : NULL;
         if (!b.text || blocks_push(&arr, &n, &cap, b) != SUCCESS) {
            free(b.text);
            free(b.lang);
            msgfmt_free_blocks(arr, n);
            return FAILURE;
         }
         p = q;
         continue;
      }

      /* --- heading --- */
      int level;
      const char *htext;
      size_t hlen;
      if (parse_heading(p, len, &level, &htext, &hlen)) {
         msgfmt_block_t b = { 0 };
         b.type = MSGFMT_BLK_HEADING;
         b.heading_level = level;
         b.text = dupn(htext, hlen);
         if (!b.text || blocks_push(&arr, &n, &cap, b) != SUCCESS) {
            free(b.text);
            msgfmt_free_blocks(arr, n);
            return FAILURE;
         }
         p = next;
         continue;
      }

      /* --- thematic break --- */
      if (line_is_hr(p, len)) {
         msgfmt_block_t b = { 0 };
         b.type = MSGFMT_BLK_HR;
         if (blocks_push(&arr, &n, &cap, b) != SUCCESS) {
            msgfmt_free_blocks(arr, n);
            return FAILURE;
         }
         p = next;
         continue;
      }

      /* --- blockquote (consecutive '>' lines) --- */
      if (t[0] == '>') {
         strbuf_t qbuf;
         strbuf_init(&qbuf, 0);
         const char *q = p;
         while (*q) {
            size_t qlen = line_length(q);
            const char *qnext = q[qlen] ? q + qlen + 1 : q + qlen;
            size_t qind = leading_indent(q, qlen);
            if (qind >= qlen || q[qind] != '>') {
               break;
            }
            size_t s = qind + 1;
            if (s < qlen && q[s] == ' ') {
               s++;
            }
            if (strbuf_len(&qbuf) > 0) {
               strbuf_append(&qbuf, "\n");
            }
            strbuf_append_n(&qbuf, q + s, qlen - s);
            q = qnext;
         }
         msgfmt_block_t b = { 0 };
         b.type = MSGFMT_BLK_BLOCKQUOTE;
         b.text = strbuf_steal(&qbuf);
         if (!b.text || blocks_push(&arr, &n, &cap, b) != SUCCESS) {
            free(b.text);
            msgfmt_free_blocks(arr, n);
            return FAILURE;
         }
         p = q;
         continue;
      }

      /* --- table (header row + delimiter row) --- */
      if (memchr(t, '|', tl) != NULL && p[len]) {
         const char *peek = next;
         size_t plen = line_length(peek);
         if (line_is_table_sep(peek, plen)) {
            strbuf_t tab;
            strbuf_init(&tab, 0);
            /* header line */
            strbuf_append_n(&tab, p, len);
            /* skip the delimiter row, then consume body rows containing '|' */
            const char *q = peek[plen] ? peek + plen + 1 : peek + plen;
            while (*q) {
               size_t qlen = line_length(q);
               if (line_is_blank(q, qlen) || memchr(q, '|', qlen) == NULL) {
                  break;
               }
               strbuf_append(&tab, "\n");
               strbuf_append_n(&tab, q, qlen);
               q = q[qlen] ? q + qlen + 1 : q + qlen;
            }
            msgfmt_block_t b = { 0 };
            b.type = MSGFMT_BLK_TABLE;
            b.text = strbuf_steal(&tab);
            if (!b.text || blocks_push(&arr, &n, &cap, b) != SUCCESS) {
               free(b.text);
               msgfmt_free_blocks(arr, n);
               return FAILURE;
            }
            p = q;
            continue;
         }
      }

      /* --- list item (one line per block) --- */
      int ord, num, dep;
      const char *ltext;
      size_t llen;
      if (parse_list_item(p, len, &ord, &num, &dep, &ltext, &llen)) {
         msgfmt_block_t b = { 0 };
         b.type = MSGFMT_BLK_LIST_ITEM;
         b.list_ordered = ord;
         b.list_number = num;
         b.depth = dep;
         b.text = dupn(ltext, llen);
         if (!b.text || blocks_push(&arr, &n, &cap, b) != SUCCESS) {
            free(b.text);
            msgfmt_free_blocks(arr, n);
            return FAILURE;
         }
         p = next;
         continue;
      }

      /* --- paragraph (run of plain lines) --- */
      strbuf_t para;
      strbuf_init(&para, 0);
      const char *q = p;
      while (*q) {
         size_t qlen = line_length(q);
         if (line_is_blank(q, qlen)) {
            q = q[qlen] ? q + qlen + 1 : q + qlen;
            break;
         }
         if (q != p && line_starts_block(q, qlen)) {
            break;
         }
         if (strbuf_len(&para) > 0) {
            strbuf_append(&para, "\n");
         }
         strbuf_append_n(&para, q, qlen);
         q = q[qlen] ? q + qlen + 1 : q + qlen;
      }
      msgfmt_block_t b = { 0 };
      b.type = MSGFMT_BLK_PARAGRAPH;
      b.text = strbuf_steal(&para);
      if (!b.text || blocks_push(&arr, &n, &cap, b) != SUCCESS) {
         free(b.text);
         msgfmt_free_blocks(arr, n);
         return FAILURE;
      }
      p = q;
   }

   *blocks_out = arr;
   *count_out = n;
   return SUCCESS;
}

/* ======================================================================== */
/* Format-agnostic inline walker                                             */
/* ======================================================================== */

/* Find the closing backtick run of exactly `runlen` backticks, starting at
 * `from`.  Returns its index, or `len` if unmatched. */
static size_t find_code_close(const char *text, size_t len, size_t from, size_t runlen) {
   size_t i = from;
   while (i < len) {
      if (text[i] == '`') {
         size_t r = 0;
         while (i + r < len && text[i + r] == '`') {
            r++;
         }
         if (r == runlen) {
            return i;
         }
         i += r;
      } else {
         i++;
      }
   }
   return len;
}

/* Find a closing emphasis run of delimiter `d` (length >= need) at or after
 * `from`, skipping over code spans.  Returns its index, or `len`.
 *
 * Called per opening delimiter and itself scans forward, so the inline walk is
 * O(n^2) in the worst case.  Safe here because the tokenizer feeds it ONE block
 * at a time (n = a paragraph, not the whole reply) and the input is the LLM's
 * own length-capped output.  If this is ever reused on untrusted, uncapped
 * input, memoize code-span boundaries in a single forward pass instead. */
static size_t find_emph_close(const char *text, size_t len, size_t from, char d, size_t need) {
   size_t i = from;
   while (i < len) {
      char c = text[i];
      if (c == '`') {
         size_t r = 0;
         while (i + r < len && text[i + r] == '`') {
            r++;
         }
         size_t close = find_code_close(text, len, i + r, r);
         i = (close < len) ? close + r : len;
         continue;
      }
      if (c == d) {
         size_t r = 0;
         while (i + r < len && text[i + r] == d) {
            r++;
         }
         if (r >= need) {
            return i;
         }
         i += r;
         continue;
      }
      i++;
   }
   return len;
}

/* Parse a "[text](url)" link starting at `i` ('['). */
static bool parse_link(const char *text,
                       size_t len,
                       size_t i,
                       const char **txt,
                       size_t *tn,
                       const char **url,
                       size_t *un,
                       size_t *after) {
   size_t close_br = len;
   for (size_t j = i + 1; j < len; j++) {
      if (text[j] == '\n') {
         return false;
      }
      if (text[j] == ']') {
         close_br = j;
         break;
      }
   }
   if (close_br >= len || close_br + 1 >= len || text[close_br + 1] != '(') {
      return false;
   }
   size_t close_par = len;
   for (size_t j = close_br + 2; j < len; j++) {
      if (text[j] == '\n') {
         return false;
      }
      if (text[j] == ')') {
         close_par = j;
         break;
      }
   }
   if (close_par >= len) {
      return false;
   }
   *txt = text + i + 1;
   *tn = close_br - (i + 1);
   *url = text + close_br + 2;
   *un = close_par - (close_br + 2);
   *after = close_par + 1;
   return true;
}

static void walk_depth(const char *text, size_t len, const msgfmt_inline_sink_t *sink, int depth) {
   size_t i = 0;
   while (i < len) {
      char c = text[i];

      if (c == '`') {
         size_t r = 0;
         while (i + r < len && text[i + r] == '`') {
            r++;
         }
         size_t close = find_code_close(text, len, i + r, r);
         if (close < len) {
            sink->code_span(sink->ctx, text + i + r, close - (i + r));
            i = close + r;
            continue;
         }
         sink->text(sink->ctx, text + i, r);
         i += r;
         continue;
      }

      if (c == '[') {
         const char *txt;
         const char *url;
         size_t tn, un, after;
         if (parse_link(text, len, i, &txt, &tn, &url, &un, &after)) {
            sink->link(sink->ctx, txt, tn, url, un);
            i = after;
            continue;
         }
         sink->text(sink->ctx, text + i, 1);
         i++;
         continue;
      }

      if (c == '*' || c == '_' || c == '~') {
         size_t r = 0;
         while (i + r < len && text[i + r] == c) {
            r++;
         }
         int kind;
         size_t need;
         if (c == '~') {
            if (r < 2) {
               sink->text(sink->ctx, text + i, r);
               i += r;
               continue;
            }
            kind = MSGFMT_EMPH_STRIKE;
            need = 2;
         } else {
            need = (r >= 3) ? 3 : r;
            kind = (need == 3) ? MSGFMT_EMPH_BOLDITALIC
                               : (need == 2 ? MSGFMT_EMPH_BOLD : MSGFMT_EMPH_ITALIC);
         }
         size_t close = find_emph_close(text, len, i + r, c, need);
         if (close < len && depth < MSGFMT_MAX_NESTING) {
            sink->emph_open(sink->ctx, kind);
            walk_depth(text + i + need, close - (i + need), sink, depth + 1);
            sink->emph_close(sink->ctx, kind);
            i = close + need;
            continue;
         }
         sink->text(sink->ctx, text + i, r);
         i += r;
         continue;
      }

      /* literal run up to the next special char */
      size_t j = i;
      while (j < len && text[j] != '`' && text[j] != '[' && text[j] != '*' && text[j] != '_' &&
             text[j] != '~') {
         j++;
      }
      sink->text(sink->ctx, text + i, j - i);
      i = j;
   }
}

void msgfmt_walk_inline(const char *text, size_t len, const msgfmt_inline_sink_t *sink) {
   if (text && len) {
      walk_depth(text, len, sink, 0);
   }
}

/* ======================================================================== */
/* Plain / Discord / Slack inline sink                                       */
/* ======================================================================== */

typedef struct {
   strbuf_t *out;
   messaging_format_t fmt;
} gen_ctx_t;

/* Slack escapes &, <, > everywhere in the text field. */
static void append_slack_escaped(strbuf_t *out, const char *s, size_t n) {
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

/* Append `s` escaped for Slack, verbatim for other formats.  Used for fenced
 * code / table interiors (Slack's text field interprets &<> even inside code
 * fences, so they must be entity-escaped there too). */
static void append_maybe_escaped(messaging_format_t fmt, strbuf_t *out, const char *s, size_t n) {
   if (fmt == MSG_FMT_SLACK_MRKDWN) {
      append_slack_escaped(out, s, n);
   } else {
      strbuf_append_n(out, s, n);
   }
}

/* malloc'd Slack-escaped copy of `s` (NULL on OOM). */
static char *slack_escape_dup(const char *s) {
   strbuf_t sb;
   strbuf_init(&sb, 0);
   append_slack_escaped(&sb, s, strlen(s));
   return strbuf_steal(&sb);
}

static void gen_text(void *ctx, const char *s, size_t n) {
   gen_ctx_t *g = (gen_ctx_t *)ctx;
   if (g->fmt == MSG_FMT_SLACK_MRKDWN) {
      append_slack_escaped(g->out, s, n);
   } else {
      strbuf_append_n(g->out, s, n);
   }
}

static void gen_code_span(void *ctx, const char *s, size_t n) {
   gen_ctx_t *g = (gen_ctx_t *)ctx;
   if (g->fmt == MSG_FMT_PLAIN) {
      strbuf_append_n(g->out, s, n);
      return;
   }
   strbuf_append(g->out, "`");
   if (g->fmt == MSG_FMT_SLACK_MRKDWN) {
      append_slack_escaped(g->out, s, n);
   } else {
      strbuf_append_n(g->out, s, n);
   }
   strbuf_append(g->out, "`");
}

static void gen_emph_open(void *ctx, int kind) {
   gen_ctx_t *g = (gen_ctx_t *)ctx;
   if (g->fmt == MSG_FMT_PLAIN) {
      return;
   }
   if (g->fmt == MSG_FMT_DISCORD) {
      switch (kind) {
         case MSGFMT_EMPH_ITALIC:
            strbuf_append(g->out, "*");
            break;
         case MSGFMT_EMPH_BOLD:
            strbuf_append(g->out, "**");
            break;
         case MSGFMT_EMPH_BOLDITALIC:
            strbuf_append(g->out, "***");
            break;
         case MSGFMT_EMPH_STRIKE:
            strbuf_append(g->out, "~~");
            break;
         default:
            break;
      }
      return;
   }
   /* Slack */
   switch (kind) {
      case MSGFMT_EMPH_ITALIC:
         strbuf_append(g->out, "_");
         break;
      case MSGFMT_EMPH_BOLD:
         strbuf_append(g->out, "*");
         break;
      case MSGFMT_EMPH_BOLDITALIC:
         strbuf_append(g->out, "*_");
         break;
      case MSGFMT_EMPH_STRIKE:
         strbuf_append(g->out, "~");
         break;
      default:
         break;
   }
}

static void gen_emph_close(void *ctx, int kind) {
   gen_ctx_t *g = (gen_ctx_t *)ctx;
   if (g->fmt == MSG_FMT_PLAIN) {
      return;
   }
   if (g->fmt == MSG_FMT_DISCORD) {
      gen_emph_open(ctx, kind); /* symmetric markers */
      return;
   }
   /* Slack — bold-italic closes mirror-image */
   switch (kind) {
      case MSGFMT_EMPH_ITALIC:
         strbuf_append(g->out, "_");
         break;
      case MSGFMT_EMPH_BOLD:
         strbuf_append(g->out, "*");
         break;
      case MSGFMT_EMPH_BOLDITALIC:
         strbuf_append(g->out, "_*");
         break;
      case MSGFMT_EMPH_STRIKE:
         strbuf_append(g->out, "~");
         break;
      default:
         break;
   }
}

static void gen_link(void *ctx, const char *text, size_t tn, const char *url, size_t un) {
   gen_ctx_t *g = (gen_ctx_t *)ctx;
   if (g->fmt == MSG_FMT_SLACK_MRKDWN) {
      /* Slack link form <url|label>.  A raw <, > or | in the URL would break
       * out of the link grammar and let a crafted link inject Slack control
       * sequences (<!channel>, <@Uxxx> mentions).  Escape < > to entities; if
       * the URL contains a | (no Slack escape for it inside the URL slot),
       * drop to the bare-label form rather than emit a malformed link.  The
       * URL's & is left raw — Slack treats the link target literally and
       * entity-encoding it would corrupt query strings. */
      if (un == 0 || memchr(url, '|', un) != NULL) {
         append_slack_escaped(g->out, text, tn);
         return;
      }
      strbuf_append(g->out, "<");
      for (size_t i = 0; i < un; i++) {
         if (url[i] == '<') {
            strbuf_append(g->out, "&lt;");
         } else if (url[i] == '>') {
            strbuf_append(g->out, "&gt;");
         } else {
            strbuf_append_n(g->out, url + i, 1);
         }
      }
      strbuf_append(g->out, "|");
      append_slack_escaped(g->out, text, tn);
      strbuf_append(g->out, ">");
      return;
   }
   /* Plain + Discord: flatten to "text (url)". */
   strbuf_append_n(g->out, text, tn);
   if (un > 0) {
      strbuf_append(g->out, " (");
      strbuf_append_n(g->out, url, un);
      strbuf_append(g->out, ")");
   }
}

static void gen_inline(messaging_format_t fmt, const char *src, size_t len, strbuf_t *out) {
   gen_ctx_t ctx = { .out = out, .fmt = fmt };
   msgfmt_inline_sink_t sink = { .ctx = &ctx,
                                 .text = gen_text,
                                 .code_span = gen_code_span,
                                 .emph_open = gen_emph_open,
                                 .emph_close = gen_emph_close,
                                 .link = gen_link };
   msgfmt_walk_inline(src, len, &sink);
}

/* ======================================================================== */
/* Plain / Discord / Slack block renderer                                    */
/* ======================================================================== */

/* Flatten a GFM table to plain " | "-joined rows (drops the delimiter row). */
static void render_table_plain(const char *raw, strbuf_t *out) {
   const char *p = raw;
   bool first = true;
   while (*p) {
      size_t len = line_length(p);
      const char *next = p[len] ? p + len + 1 : p + len;
      if (!line_is_table_sep(p, len) && !line_is_blank(p, len)) {
         if (!first) {
            strbuf_append(out, "\n");
         }
         first = false;
         /* split on '|', trim cells, join with " | " */
         size_t i = 0;
         bool first_cell = true;
         while (i < len) {
            while (i < len && p[i] == '|') {
               i++;
            }
            size_t start = i;
            while (i < len && p[i] != '|') {
               i++;
            }
            size_t cstart = start;
            size_t cend = i;
            while (cstart < cend && (p[cstart] == ' ' || p[cstart] == '\t')) {
               cstart++;
            }
            while (cend > cstart && (p[cend - 1] == ' ' || p[cend - 1] == '\t')) {
               cend--;
            }
            if (cend > cstart) {
               if (!first_cell) {
                  strbuf_append(out, " | ");
               }
               first_cell = false;
               strbuf_append_n(out, p + cstart, cend - cstart);
            }
         }
      }
      p = next;
   }
}

static void render_block(messaging_format_t fmt, const msgfmt_block_t *b, strbuf_t *out) {
   switch (b->type) {
      case MSGFMT_BLK_HEADING: {
         if (fmt == MSG_FMT_DISCORD) {
            int lvl = b->heading_level > 3 ? 3 : b->heading_level;
            for (int k = 0; k < lvl; k++) {
               strbuf_append(out, "#");
            }
            strbuf_append(out, " ");
            gen_inline(fmt, b->text, strlen(b->text), out);
         } else if (fmt == MSG_FMT_SLACK_MRKDWN) {
            strbuf_append(out, "*");
            gen_inline(fmt, b->text, strlen(b->text), out);
            strbuf_append(out, "*");
         } else {
            gen_inline(fmt, b->text, strlen(b->text), out);
         }
         break;
      }
      case MSGFMT_BLK_PARAGRAPH:
         gen_inline(fmt, b->text, strlen(b->text), out);
         break;
      case MSGFMT_BLK_BLOCKQUOTE: {
         const char *p = b->text;
         bool first = true;
         while (true) {
            size_t len = line_length(p);
            if (!first) {
               strbuf_append(out, "\n");
            }
            first = false;
            if (fmt == MSG_FMT_DISCORD || fmt == MSG_FMT_SLACK_MRKDWN) {
               strbuf_append(out, "> ");
            }
            gen_inline(fmt, p, len, out);
            if (!p[len]) {
               break;
            }
            p += len + 1;
         }
         break;
      }
      case MSGFMT_BLK_CODE_FENCE:
         if (fmt == MSG_FMT_PLAIN) {
            strbuf_append(out, b->text);
         } else {
            strbuf_append(out, "```");
            if (b->lang) {
               strbuf_append(out, b->lang);
            }
            strbuf_append(out, "\n");
            append_maybe_escaped(fmt, out, b->text, strlen(b->text));
            size_t tl = strlen(b->text);
            if (tl == 0 || b->text[tl - 1] != '\n') {
               strbuf_append(out, "\n");
            }
            strbuf_append(out, "```");
         }
         break;
      case MSGFMT_BLK_LIST_ITEM: {
         for (int d = 0; d < b->depth; d++) {
            strbuf_append(out, "  ");
         }
         if (b->list_ordered) {
            strbuf_appendf(out, "%d. ", b->list_number);
         } else {
            strbuf_append(out, fmt == MSG_FMT_SLACK_MRKDWN ? "\xe2\x80\xa2 " : "- ");
         }
         gen_inline(fmt, b->text, strlen(b->text), out);
         break;
      }
      case MSGFMT_BLK_TABLE:
         if (fmt == MSG_FMT_PLAIN) {
            render_table_plain(b->text, out);
         } else {
            strbuf_append(out, "```\n");
            append_maybe_escaped(fmt, out, b->text, strlen(b->text));
            size_t tl = strlen(b->text);
            if (tl == 0 || b->text[tl - 1] != '\n') {
               strbuf_append(out, "\n");
            }
            strbuf_append(out, "```");
         }
         break;
      case MSGFMT_BLK_HR:
         strbuf_append(out, "---");
         break;
      default:
         break;
   }
}

/* ======================================================================== */
/* Parts array + generic block-by-block splitter                             */
/* ======================================================================== */

typedef struct {
   char **items;
   size_t count;
   size_t cap;
} parts_t;

static int parts_push_owned(parts_t *p, char *s) {
   if (p->count == p->cap) {
      size_t ncap = p->cap ? p->cap * 2 : 8;
      char **ni = (char **)realloc(p->items, ncap * sizeof(*ni));
      if (!ni) {
         return FAILURE;
      }
      p->items = ni;
      p->cap = ncap;
   }
   p->items[p->count++] = s;
   return SUCCESS;
}

static int parts_push_copy(parts_t *p, const char *s, size_t n) {
   char *copy = dupn(s, n);
   if (!copy || parts_push_owned(p, copy) != SUCCESS) {
      free(copy);
      return FAILURE;
   }
   return SUCCESS;
}

void messaging_format_free_parts(char **parts, size_t count) {
   if (!parts) {
      return;
   }
   for (size_t i = 0; i < count; i++) {
      free(parts[i]);
   }
   free(parts);
}

/* Hard-cut `s` into <= cap-byte chunks at UTF-8 boundaries (last resort for an
 * atomic unit with no natural break). */
static int split_hardcut(parts_t *parts, const char *s, size_t cap) {
   size_t len = strlen(s);
   size_t pos = 0;
   while (pos < len) {
      size_t take = (len - pos > cap) ? cap : (len - pos);
      /* don't cut a UTF-8 continuation byte */
      while (take > 1 && (s[pos + take] & 0xC0) == 0x80) {
         take--;
      }
      if (parts_push_copy(parts, s + pos, take) != SUCCESS) {
         return FAILURE;
      }
      pos += take;
   }
   return SUCCESS;
}

/* Split an oversized flat run at natural breaks, falling back to a hard cut. */
static int split_run(parts_t *parts, const char *s, size_t cap) {
   char **sub = NULL;
   size_t subn = 0;
   char err[256] = { 0 };
   if (messaging_split_at_breaks(s, cap, &sub, &subn, err, sizeof(err)) == SUCCESS) {
      for (size_t i = 0; i < subn; i++) {
         if (parts_push_owned(parts, sub[i]) != SUCCESS) {
            for (size_t j = i; j < subn; j++) {
               free(sub[j]);
            }
            free(sub);
            return FAILURE;
         }
      }
      free(sub);
      return SUCCESS;
   }
   return split_hardcut(parts, s, cap);
}

/* Split an oversized fenced block: chunk the interior, wrap each chunk. */
static int split_code_block(messaging_format_t fmt,
                            const msgfmt_block_t *b,
                            size_t cap,
                            parts_t *parts) {
   if (fmt == MSG_FMT_PLAIN) {
      return split_run(parts, b->text, cap);
   }
   /* Slack escapes &<> even inside fences; escape BEFORE splitting so the cap
    * is measured on (and cuts land in) the escaped output. */
   char *esc = NULL;
   const char *interior = b->text;
   if (fmt == MSG_FMT_SLACK_MRKDWN) {
      esc = slack_escape_dup(b->text);
      if (!esc) {
         return FAILURE;
      }
      interior = esc;
   }
   /* fence overhead: "```" + lang + "\n" ... "\n```" */
   size_t overhead = 3 + (b->lang ? strlen(b->lang) : 0) + 1 + 1 + 3;
   size_t inner_cap = (cap > overhead + 16) ? cap - overhead : 16;

   char **sub = NULL;
   size_t subn = 0;
   char err[256] = { 0 };
   int rc = messaging_split_at_breaks(interior, inner_cap, &sub, &subn, err, sizeof(err));
   if (rc != SUCCESS) {
      /* hard-cut the interior into inner_cap chunks */
      int hc = split_hardcut(parts, interior, inner_cap);
      free(esc);
      return hc;
   }
   int ret = SUCCESS;
   for (size_t i = 0; i < subn && ret == SUCCESS; i++) {
      strbuf_t wb;
      strbuf_init(&wb, inner_cap + overhead + 1);
      strbuf_append(&wb, "```");
      if (b->lang) {
         strbuf_append(&wb, b->lang);
      }
      strbuf_append(&wb, "\n");
      strbuf_append(&wb, sub[i]);
      size_t sl = strlen(sub[i]);
      if (sl == 0 || sub[i][sl - 1] != '\n') {
         strbuf_append(&wb, "\n");
      }
      strbuf_append(&wb, "```");
      char *wrapped = strbuf_steal(&wb);
      if (!wrapped || parts_push_owned(parts, wrapped) != SUCCESS) {
         free(wrapped);
         ret = FAILURE;
      }
   }
   for (size_t i = 0; i < subn; i++) {
      free(sub[i]);
   }
   free(sub);
   free(esc);
   return ret;
}

/* Separator inserted between two rendered blocks in the same part. */
static const char *block_sep(msgfmt_block_type_t prev, msgfmt_block_type_t cur) {
   if (prev == MSGFMT_BLK_LIST_ITEM && cur == MSGFMT_BLK_LIST_ITEM) {
      return "\n";
   }
   return "\n\n";
}

static int generic_render_split(messaging_format_t fmt,
                                const msgfmt_block_t *blocks,
                                size_t nblocks,
                                size_t cap,
                                char ***parts_out,
                                size_t *count_out,
                                char *err_msg,
                                size_t err_size) {
   parts_t parts = { 0 };
   strbuf_t cur;
   strbuf_init(&cur, 0);
   strbuf_t blk;
   strbuf_init(&blk, 0);

   bool have_prev = false;
   msgfmt_block_type_t prev_type = MSGFMT_BLK_PARAGRAPH;
   int rc = SUCCESS;

   for (size_t i = 0; i < nblocks && rc == SUCCESS; i++) {
      /* render this block into blk (reset first) */
      strbuf_free(&blk);
      strbuf_init(&blk, 0);
      render_block(fmt, &blocks[i], &blk);
      size_t blen = strbuf_len(&blk);
      if (blen == 0) {
         continue; /* nothing rendered (e.g. empty heading) */
      }
      const char *sep = have_prev && strbuf_len(&cur) > 0 ? block_sep(prev_type, blocks[i].type)
                                                          : "";
      size_t seplen = strlen(sep);

      if (strbuf_len(&cur) + seplen + blen <= cap) {
         strbuf_append(&cur, sep);
         strbuf_append_n(&cur, strbuf_str(&blk), blen);
      } else {
         if (strbuf_len(&cur) > 0) {
            char *done = strbuf_steal(&cur);
            if (!done || parts_push_owned(&parts, done) != SUCCESS) {
               free(done);
               rc = FAILURE;
               break;
            }
            strbuf_init(&cur, 0);
         }
         if (blen <= cap) {
            strbuf_append_n(&cur, strbuf_str(&blk), blen);
         } else if (blocks[i].type == MSGFMT_BLK_CODE_FENCE || blocks[i].type == MSGFMT_BLK_TABLE) {
            const msgfmt_block_t *cb = &blocks[i];
            if (blocks[i].type == MSGFMT_BLK_TABLE && fmt != MSG_FMT_PLAIN) {
               /* a fenced table is just a code block whose interior is the raw
                * rows with no language */
               msgfmt_block_t tmp = { .type = MSGFMT_BLK_CODE_FENCE, .text = cb->text };
               rc = split_code_block(fmt, &tmp, cap, &parts);
            } else if (blocks[i].type == MSGFMT_BLK_CODE_FENCE) {
               rc = split_code_block(fmt, cb, cap, &parts);
            } else {
               rc = split_run(&parts, strbuf_str(&blk), cap);
            }
         } else {
            rc = split_run(&parts, strbuf_str(&blk), cap);
         }
      }
      have_prev = true;
      prev_type = blocks[i].type;
   }

   if (rc == SUCCESS && strbuf_len(&cur) > 0) {
      char *done = strbuf_steal(&cur);
      if (!done || parts_push_owned(&parts, done) != SUCCESS) {
         free(done);
         rc = FAILURE;
      }
   }

   strbuf_free(&cur);
   strbuf_free(&blk);

   if (rc != SUCCESS) {
      messaging_format_free_parts(parts.items, parts.count);
      snprintf(err_msg, err_size, "Out of memory formatting the reply.");
      *parts_out = NULL;
      *count_out = 0;
      return FAILURE;
   }

   if (parts.count == 0) {
      if (parts_push_copy(&parts, MSGFMT_EMPTY_PLACEHOLDER, strlen(MSGFMT_EMPTY_PLACEHOLDER)) !=
          SUCCESS) {
         messaging_format_free_parts(parts.items, parts.count);
         snprintf(err_msg, err_size, "Out of memory formatting the reply.");
         *parts_out = NULL;
         *count_out = 0;
         return FAILURE;
      }
   }

   *parts_out = parts.items;
   *count_out = parts.count;
   return SUCCESS;
}

/* ======================================================================== */
/* Public entry point                                                        */
/* ======================================================================== */

int messaging_format_render_split(const char *markdown,
                                  messaging_format_t fmt,
                                  size_t max_part_chars,
                                  char ***parts_out,
                                  size_t *count_out,
                                  char *err_msg,
                                  size_t err_size) {
   if (!markdown || !parts_out || !count_out || max_part_chars == 0) {
      if (err_msg && err_size) {
         snprintf(err_msg, err_size, "Invalid formatter arguments.");
      }
      if (parts_out) {
         *parts_out = NULL;
      }
      if (count_out) {
         *count_out = 0;
      }
      return FAILURE;
   }

   msgfmt_block_t *blocks = NULL;
   size_t nblocks = 0;
   if (msgfmt_tokenize(markdown, &blocks, &nblocks) != SUCCESS) {
      snprintf(err_msg, err_size, "Out of memory parsing the reply.");
      *parts_out = NULL;
      *count_out = 0;
      return FAILURE;
   }

   int rc;
   if (fmt == MSG_FMT_TELEGRAM_HTML) {
      rc = msgfmt_render_split_telegram(blocks, nblocks, max_part_chars, parts_out, count_out,
                                        err_msg, err_size);
   } else {
      rc = generic_render_split(fmt, blocks, nblocks, max_part_chars, parts_out, count_out, err_msg,
                                err_size);
   }

   msgfmt_free_blocks(blocks, nblocks);
   return rc;
}
