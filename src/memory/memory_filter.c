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
 * Memory injection filter — blocks prompt-injection payloads from being
 * stored as facts or preferences in the persistent memory system.
 *
 * Checks text against a blocklist of known injection patterns after
 * normalizing away Unicode obfuscation (zero-width chars, homoglyphs,
 * bidi overrides, tag characters).
 */

#include "memory/memory_filter.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "logging.h"

/* =============================================================================
 * Blocked Patterns
 *
 * Multi-word patterns checked after normalization. Single common English
 * words are deliberately avoided to prevent false positives on legitimate
 * facts (lesson learned from the April 2026 incident with "token", "skip",
 * "always", etc.).
 * ============================================================================= */

static const char *BLOCKED_PATTERNS[] = {
   /* Imperative patterns addressing the AI */
   "you should", "you must", "you need to", "you shall", "you have to", "you will", "you are to",
   "make sure", "ensure that", "be sure to", "don't forget",
   /* "always/never/whenever" + imperative verb */
   "always respond", "always answer", "always say", "always reply", "always act", "always include",
   "always add", "always use", "always be ", "never refuse", "never deny", "never reject",
   "never decline", "never say", "never mention", "never reveal", "never tell", "whenever you",
   "whenever asked", "whenever i ",
   /* Negation/override */
   "ignore your", "ignore previous", "ignore above", "ignore all ", "ignore the ", "forget your",
   "forget previous", "forget above", "forget all ", "forget everything", "disregard", "pretend",
   "act as if", "override", "bypass", "disable safe", "disable filter", "disable guard",
   "disable content", "disable check", "skip check", "skip verif", "skip valid", "skip safe",
   /* System manipulation */
   "system prompt", "your instructions", "your guidelines", "your rules", "your constraints",
   "from now on", "going forward", "henceforth",
   /* Credential patterns */
   "password", "api key", "apikey", "api token", "access token", "auth token", "session token",
   "secret key", "credential", "private key", "bearer",
   /* Role/persona manipulation */
   "you are", "your role", "your purpose", "your job", "your task", "act like", "behave as",
   "respond as",
   /* LLM role/instruction markers (Phase 1) */
   "[inst]", "<<sys>>", "<|im_start|>", "<|im_end|>", "{{#system",
   /* XML/HTML injection (Phase 1) */
   "<system>", "<system_prompt>", "<script", "<claude_",
   /* Markdown exfiltration (Phase 1) */
   "](http", "](https",
   /* Base64 payload indicator (Phase 1) */
   "base64,", NULL
};

/* Cyrillic/Greek homoglyphs that map to ASCII equivalents */
static const struct {
   const char *lookalike;
   char replacement;
} HOMOGLYPHS[] = { { "\xd0\xb0", 'a' }, /* Cyrillic а */
                   { "\xd0\xb5", 'e' }, /* Cyrillic е */
                   { "\xd0\xbe", 'o' }, /* Cyrillic о */
                   { "\xd1\x80", 'p' }, /* Cyrillic р */
                   { "\xd1\x81", 'c' }, /* Cyrillic с */
                   { "\xd1\x85", 'x' }, /* Cyrillic х */
                   { "\xd1\x83", 'y' }, /* Cyrillic у */
                   { "\xd1\x96", 'i' }, /* Cyrillic і */
                   { "\xd1\x95", 's' }, /* Cyrillic ѕ */
                   { "\xce\xb1", 'a' }, /* Greek α */
                   { "\xce\xb5", 'e' }, /* Greek ε */
                   { "\xce\xb9", 'i' }, /* Greek ι (iota) */
                   { "\xce\xba", 'k' }, /* Greek κ (kappa) */
                   { "\xce\xbd", 'n' }, /* Greek ν (nu) */
                   { "\xce\xbf", 'o' }, /* Greek ο */
                   { "\xcf\x81", 'p' }, /* Greek ρ (rho) */
                   { "\xcf\x84", 't' }, /* Greek τ (tau) */
                   { NULL, 0 } };

/* Latin-1 Supplement accent → base letter (U+00C0-U+00FF, encoded as \xc3\x80-\xc3\xbf).
 * Maps accented characters to their ASCII base so "systèm" → "system". */
static const char LATIN1_BASE[] = {
   /* U+00C0-U+00CF: À Á Â Ã Ä Å Æ Ç È É Ê Ë Ì Í Î Ï */
   'a',
   'a',
   'a',
   'a',
   'a',
   'a',
   0,
   'c',
   'e',
   'e',
   'e',
   'e',
   'i',
   'i',
   'i',
   'i',
   /* U+00D0-U+00DF: Ð Ñ Ò Ó Ô Õ Ö × Ø Ù Ú Û Ü Ý Þ ß */
   'd',
   'n',
   'o',
   'o',
   'o',
   'o',
   'o',
   0,
   'o',
   'u',
   'u',
   'u',
   'u',
   'y',
   0,
   's',
   /* U+00E0-U+00EF: à á â ã ä å æ ç è é ê ë ì í î ï */
   'a',
   'a',
   'a',
   'a',
   'a',
   'a',
   0,
   'c',
   'e',
   'e',
   'e',
   'e',
   'i',
   'i',
   'i',
   'i',
   /* U+00F0-U+00FF: ð ñ ò ó ô õ ö ÷ ø ù ú û ü ý þ ÿ */
   'd',
   'n',
   'o',
   'o',
   'o',
   'o',
   'o',
   0,
   'o',
   'u',
   'u',
   'u',
   'u',
   'y',
   0,
   'y',
};

/* Zero-width and invisible characters to strip */
static const char *INVISIBLE_CHARS[] = {
   /* Original set */
   "\xe2\x80\x8b", /* Zero-width space U+200B */
   "\xe2\x80\x8c", /* Zero-width non-joiner U+200C */
   "\xe2\x80\x8d", /* Zero-width joiner U+200D */
   "\xef\xbb\xbf", /* BOM U+FEFF */
   "\xc2\xad",     /* Soft hyphen U+00AD */
   /* Phase 2 additions */
   "\xe2\x81\xa0", /* Word joiner U+2060 */
   "\xe2\x81\xa2", /* Invisible times U+2062 */
   "\xe2\x81\xa3", /* Invisible separator U+2063 */
   "\xe2\x81\xa4", /* Invisible plus U+2064 */
   "\xe2\x80\xaa", /* LRE U+202A */
   "\xe2\x80\xab", /* RLE U+202B */
   "\xe2\x80\xac", /* PDF U+202C */
   "\xe2\x80\xad", /* LRO U+202D */
   "\xe2\x80\xae", /* RLO U+202E */
   NULL
};

/* Unicode line/paragraph separators — replaced with space, not stripped */
static const char *UNICODE_WHITESPACE[] = { "\xe2\x80\xa8", /* Line separator U+2028 */
                                            "\xe2\x80\xa9", /* Paragraph separator U+2029 */
                                            NULL };

/* =============================================================================
 * Normalization
 * ============================================================================= */

char *memory_filter_normalize(const char *text) {
   if (!text) {
      return NULL;
   }

   size_t len = strlen(text);
   char *result = malloc(len + 1);
   if (!result) {
      return NULL;
   }

   size_t out = 0;
   size_t in = 0;
   bool last_space = false;

   while (in < len) {
      bool handled = false;

      /* Strip invisible characters */
      for (int i = 0; INVISIBLE_CHARS[i] != NULL; i++) {
         size_t zw_len = strlen(INVISIBLE_CHARS[i]);
         if (in + zw_len <= len && memcmp(text + in, INVISIBLE_CHARS[i], zw_len) == 0) {
            in += zw_len;
            handled = true;
            break;
         }
      }
      if (handled)
         continue;

      /* Replace Unicode line/paragraph separators with space */
      for (int i = 0; UNICODE_WHITESPACE[i] != NULL; i++) {
         size_t ws_len = strlen(UNICODE_WHITESPACE[i]);
         if (in + ws_len <= len && memcmp(text + in, UNICODE_WHITESPACE[i], ws_len) == 0) {
            if (!last_space && out > 0) {
               result[out++] = ' ';
               last_space = true;
            }
            in += ws_len;
            handled = true;
            break;
         }
      }
      if (handled)
         continue;

      /* Map homoglyphs to ASCII */
      for (int i = 0; HOMOGLYPHS[i].lookalike != NULL; i++) {
         size_t hl_len = strlen(HOMOGLYPHS[i].lookalike);
         if (in + hl_len <= len && memcmp(text + in, HOMOGLYPHS[i].lookalike, hl_len) == 0) {
            result[out++] = HOMOGLYPHS[i].replacement;
            in += hl_len;
            last_space = false;
            handled = true;
            break;
         }
      }
      if (handled)
         continue;

      unsigned char c = (unsigned char)text[in];

      /* Handle remaining non-ASCII (multi-byte UTF-8).
       * Validate continuation bytes to prevent malformed leaders from
       * swallowing subsequent ASCII (e.g., "\xe0you" eating "yo"). */
      if (c >= 0x80) {
         if ((c & 0xF8) == 0xF0 && in + 3 < len && ((unsigned char)text[in + 1] & 0xC0) == 0x80) {
            unsigned char b1 = (unsigned char)text[in + 1];
            unsigned char b2 = (unsigned char)text[in + 2];
            /* Tag characters U+E0001-U+E007F: lead byte must be 0xF3 */
            if (c == 0xF3 && b1 == 0xA0 && (b2 == 0x80 || b2 == 0x81)) {
               in += 4;
               continue;
            }
            in += 4;
         } else if ((c & 0xF0) == 0xE0 && in + 2 < len &&
                    ((unsigned char)text[in + 1] & 0xC0) == 0x80) {
            unsigned char b1 = (unsigned char)text[in + 1];
            unsigned char b2 = (unsigned char)text[in + 2];
            /* Fullwidth ASCII U+FF01-U+FF5E → ASCII 0x21-0x7E.
             * Encoded as \xef\xbc\x81 through \xef\xbd\x9e. */
            if (c == 0xEF && b1 >= 0xBC && b1 <= 0xBD) {
               int cp = ((c & 0x0F) << 12) | ((b1 & 0x3F) << 6) | (b2 & 0x3F);
               if (cp >= 0xFF01 && cp <= 0xFF5E) {
                  result[out++] = (char)tolower(cp - 0xFEE0);
                  last_space = false;
                  in += 3;
                  continue;
               }
            }
            in += 3;
         } else if ((c & 0xE0) == 0xC0 && in + 1 < len &&
                    ((unsigned char)text[in + 1] & 0xC0) == 0x80) {
            /* Latin-1 Supplement U+00C0-U+00FF → base ASCII letter.
             * Encoded as \xc3\x80 through \xc3\xbf. */
            if (c == 0xC3) {
               int idx = (int)((unsigned char)text[in + 1] & 0x3F); /* 0x80→0, 0xBF→63 */
               char base = LATIN1_BASE[idx];
               if (base != 0) {
                  result[out++] = base;
                  last_space = false;
                  in += 2;
                  continue;
               }
            }
            in += 2;
         } else {
            in++;
         }
         continue;
      }

      /* Collapse whitespace */
      if (isspace(c)) {
         if (!last_space && out > 0) {
            result[out++] = ' ';
            last_space = true;
         }
         in++;
         continue;
      }

      result[out++] = (char)tolower(c);
      last_space = false;
      in++;
   }

   /* Trim trailing space */
   if (out > 0 && result[out - 1] == ' ') {
      out--;
   }

   result[out] = '\0';
   return result;
}

/* =============================================================================
 * ReAct Co-occurrence Check (Phase 1)
 *
 * Blocks text only when >= 2 of "thought:", "action:", "observation:" appear
 * together. Individual occurrences are common English and must not trigger.
 * ============================================================================= */

static bool check_react_cooccurrence(const char *normalized) {
   int count = 0;
   if (strstr(normalized, "thought:"))
      count++;
   if (strstr(normalized, "action:"))
      count++;
   if (strstr(normalized, "observation:"))
      count++;

   if (count >= 2) {
      OLOG_WARNING("memory_filter: blocked ReAct injection (%d/3 markers)", count);
      return true;
   }
   return false;
}

/* =============================================================================
 * Main Filter Entry Point
 * ============================================================================= */

bool memory_filter_check(const char *text) {
   if (!text) {
      return false;
   }

   char *normalized = memory_filter_normalize(text);
   if (!normalized) {
      OLOG_WARNING("memory_filter: normalization failed, blocking for safety");
      return true;
   }

   /* Substring pattern scan */
   bool blocked = false;
   for (int i = 0; BLOCKED_PATTERNS[i] != NULL; i++) {
      if (strstr(normalized, BLOCKED_PATTERNS[i]) != NULL) {
         OLOG_WARNING("memory_filter: blocked pattern: '%s'", BLOCKED_PATTERNS[i]);
         blocked = true;
         break;
      }
   }

   /* ReAct co-occurrence check */
   if (!blocked) {
      blocked = check_react_cooccurrence(normalized);
   }

   free(normalized);
   return blocked;
}
