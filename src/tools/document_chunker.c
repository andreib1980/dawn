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
 * the project author(s). Contributions include any modifications,
 * enhancements, or additions to the project. These contributions become
 * part of the project and are adopted by the project author(s).
 *
 * Document Text Chunker
 *
 * Splits document text into overlapping chunks for embedding and search.
 * Paragraph-first splitting with sentence-level fallback for large paragraphs.
 */

#include "tools/document_chunker.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "dawn_error.h"

/* =============================================================================
 * Token Estimation
 * ============================================================================= */

int chunk_estimate_tokens(const char *text, int len) {
   if (!text || len <= 0)
      return 0;
   /* chars/4 is a reasonable heuristic for English with BPE tokenizers */
   return (len + 3) / 4;
}

/* =============================================================================
 * Internal: Dynamic Chunk Array
 * ============================================================================= */

static int result_init(chunk_result_t *r) {
   r->count = 0;
   r->capacity = 16;
   r->chunks = calloc((size_t)r->capacity, sizeof(char *));
   return r->chunks ? SUCCESS : FAILURE;
}

static int result_add(chunk_result_t *r, const char *text, int len) {
   if (len <= 0)
      return SUCCESS;

   if (r->count >= r->capacity) {
      int new_cap = r->capacity * 2;
      char **new_chunks = realloc(r->chunks, (size_t)new_cap * sizeof(char *));
      if (!new_chunks)
         return FAILURE;
      r->chunks = new_chunks;
      r->capacity = new_cap;
   }

   char *copy = malloc((size_t)len + 1);
   if (!copy)
      return FAILURE;
   memcpy(copy, text, (size_t)len);
   copy[len] = '\0';

   r->chunks[r->count++] = copy;
   return SUCCESS;
}

void chunk_result_free(chunk_result_t *result) {
   if (!result)
      return;
   for (int i = 0; i < result->count; i++)
      free(result->chunks[i]);
   free(result->chunks);
   result->chunks = NULL;
   result->count = 0;
   result->capacity = 0;
}

/* =============================================================================
 * Internal: Sentence Boundary Detection
 *
 * Splits on .!? followed by whitespace, skipping common abbreviations
 * (Mr., Mrs., Ms., Dr., vs., etc., e.g., i.e., St.) and decimal numbers.
 * ============================================================================= */

static const char *abbreviations[] = { "Mr.",  "Mrs.", "Ms.",  "Dr.",  "Prof.", "Sr.",  "Jr.",
                                       "vs.",  "etc.", "e.g.", "i.e.", "St.",   "Fig.", "No.",
                                       "Vol.", "Ch.",  "Sec.", "Inc.", "Ltd.",  "Corp." };
static const int num_abbreviations = sizeof(abbreviations) / sizeof(abbreviations[0]);

static bool is_abbreviation(const char *text, int period_pos) {
   for (int a = 0; a < num_abbreviations; a++) {
      int alen = (int)strlen(abbreviations[a]);
      int start = period_pos - alen + 1;
      if (start >= 0 && strncmp(text + start, abbreviations[a], (size_t)alen) == 0)
         return true;
   }
   return false;
}

static bool is_decimal(const char *text, int period_pos) {
   /* Check for digit.digit pattern */
   if (period_pos > 0 && isdigit((unsigned char)text[period_pos - 1]) &&
       text[period_pos + 1] != '\0' && isdigit((unsigned char)text[period_pos + 1]))
      return true;
   return false;
}

/**
 * Find next sentence boundary starting from pos.
 * Returns index of the first char after the sentence-ending punctuation + space,
 * or -1 if no boundary found before end.
 */
static int find_sentence_boundary(const char *text, int len, int pos) {
   for (int i = pos; i < len; i++) {
      char c = text[i];
      if (c == '.' || c == '!' || c == '?') {
         /* Must be followed by whitespace or end of text */
         if (i + 1 >= len)
            return i + 1;
         if (!isspace((unsigned char)text[i + 1]))
            continue;
         /* Skip abbreviations and decimals for periods */
         if (c == '.' && (is_abbreviation(text, i) || is_decimal(text, i)))
            continue;
         /* Skip past trailing whitespace */
         int j = i + 1;
         while (j < len && (text[j] == ' ' || text[j] == '\t'))
            j++;
         return j;
      }
   }
   return -1;
}

/* =============================================================================
 * Internal: Split a Single Block Into Sentence-Sized Chunks
 *
 * Used when a paragraph exceeds max_tokens.
 * ============================================================================= */

static int split_by_sentences(const char *text, int len, int max_chars, chunk_result_t *r) {
   int start = 0;

   while (start < len) {
      /* If remainder fits in max, take it all */
      if (chunk_estimate_tokens(text + start, len - start) <= max_chars / 4) {
         /* Skip leading whitespace */
         while (start < len && isspace((unsigned char)text[start]))
            start++;
         if (start < len) {
            if (result_add(r, text + start, len - start) != SUCCESS)
               return FAILURE;
         }
         break;
      }

      /* Find the last sentence boundary before max_chars */
      int best = -1;
      int search_pos = start;
      while (search_pos < start + max_chars && search_pos < len) {
         int boundary = find_sentence_boundary(text, len, search_pos);
         if (boundary < 0 || boundary > start + max_chars)
            break;
         best = boundary;
         search_pos = boundary;
      }

      if (best > start) {
         /* Trim trailing whitespace from chunk */
         int end = best;
         while (end > start && isspace((unsigned char)text[end - 1]))
            end--;
         if (end > start) {
            if (result_add(r, text + start, end - start) != SUCCESS)
               return FAILURE;
         }
         start = best;
      } else {
         /* No sentence boundary found — force split at max_chars */
         int end = start + max_chars;
         if (end > len)
            end = len;
         /* Try to break at a word boundary */
         int word_break = end;
         while (word_break > start + max_chars / 2 && !isspace((unsigned char)text[word_break - 1]))
            word_break--;
         if (word_break > start + max_chars / 2)
            end = word_break;
         /* Trim trailing whitespace */
         while (end > start && isspace((unsigned char)text[end - 1]))
            end--;
         if (end > start) {
            if (result_add(r, text + start, end - start) != SUCCESS)
               return FAILURE;
         }
         start = (word_break > start + max_chars / 2) ? word_break : start + max_chars;
      }
   }

   return SUCCESS;
}

/* =============================================================================
 * Internal: Find Paragraph Boundaries
 * ============================================================================= */

typedef struct {
   const char *start;
   int len;
} text_span_t;

static int split_paragraphs(const char *text,
                            int text_len,
                            text_span_t **out_spans,
                            int *out_count) {
   int capacity = 32;
   text_span_t *spans = calloc((size_t)capacity, sizeof(text_span_t));
   if (!spans)
      return FAILURE;

   int count = 0;
   int pos = 0;

   while (pos < text_len) {
      /* Skip leading whitespace between paragraphs */
      while (pos < text_len && isspace((unsigned char)text[pos]))
         pos++;
      if (pos >= text_len)
         break;

      int para_start = pos;

      /* Find end of paragraph: double newline or end of text */
      while (pos < text_len) {
         if (text[pos] == '\n' && pos + 1 < text_len && text[pos + 1] == '\n')
            break;
         pos++;
      }

      int para_end = pos;
      /* Trim trailing whitespace */
      while (para_end > para_start && isspace((unsigned char)text[para_end - 1]))
         para_end--;

      if (para_end > para_start) {
         if (count >= capacity) {
            capacity *= 2;
            text_span_t *new_spans = realloc(spans, (size_t)capacity * sizeof(text_span_t));
            if (!new_spans) {
               free(spans);
               return FAILURE;
            }
            spans = new_spans;
         }
         spans[count].start = text + para_start;
         spans[count].len = para_end - para_start;
         count++;
      }

      /* Skip the double newline */
      if (pos < text_len && text[pos] == '\n')
         pos++;
   }

   *out_spans = spans;
   *out_count = count;
   return SUCCESS;
}

/* =============================================================================
 * Public API
 * ============================================================================= */

int document_chunk_text(const char *text, const chunk_config_t *config, chunk_result_t *out) {
   if (!text || !out)
      return FAILURE;

   chunk_config_t cfg = CHUNK_CONFIG_DEFAULT;
   if (config)
      cfg = *config;

   int text_len = (int)strlen(text);
   if (text_len == 0) {
      out->chunks = NULL;
      out->count = 0;
      out->capacity = 0;
      return SUCCESS;
   }

   /* Convert token limits to approximate char limits */
   int target_chars = cfg.target_tokens * 4;
   int max_chars = cfg.max_tokens * 4;
   int overlap_chars = cfg.overlap_tokens * 4;

   /* Step 1: Split into paragraphs */
   text_span_t *paragraphs = NULL;
   int para_count = 0;
   if (split_paragraphs(text, text_len, &paragraphs, &para_count) != SUCCESS)
      return FAILURE;

   if (para_count == 0) {
      free(paragraphs);
      out->chunks = NULL;
      out->count = 0;
      out->capacity = 0;
      return SUCCESS;
   }

   /* Step 2: Split oversized paragraphs by sentences, collect all segments */
   chunk_result_t segments;
   if (result_init(&segments) != SUCCESS) {
      free(paragraphs);
      return FAILURE;
   }

   for (int i = 0; i < para_count; i++) {
      int tokens = chunk_estimate_tokens(paragraphs[i].start, paragraphs[i].len);
      if (tokens > cfg.max_tokens) {
         /* Split this paragraph by sentences */
         if (split_by_sentences(paragraphs[i].start, paragraphs[i].len, max_chars, &segments) !=
             SUCCESS) {
            chunk_result_free(&segments);
            free(paragraphs);
            return FAILURE;
         }
      } else {
         /* Paragraph fits as a single segment */
         if (result_add(&segments, paragraphs[i].start, paragraphs[i].len) != SUCCESS) {
            chunk_result_free(&segments);
            free(paragraphs);
            return FAILURE;
         }
      }
   }
   free(paragraphs);

   /* Step 3: Merge small consecutive segments until target size */
   if (result_init(out) != SUCCESS) {
      chunk_result_free(&segments);
      return FAILURE;
   }

   /* Accumulate segments into merged chunks */
   char *merge_buf = malloc((size_t)max_chars + 256);
   if (!merge_buf) {
      chunk_result_free(&segments);
      chunk_result_free(out);
      return FAILURE;
   }

   int merge_len = 0;

   for (int i = 0; i < segments.count; i++) {
      int seg_len = (int)strlen(segments.chunks[i]);
      int combined_len = merge_len + (merge_len > 0 ? 2 : 0) + seg_len; /* +2 for "\n\n" sep */
      int combined_tokens = chunk_estimate_tokens(merge_buf, combined_len);

      if (merge_len > 0 && combined_tokens > cfg.target_tokens) {
         /* Flush current merge buffer as a chunk */
         if (result_add(out, merge_buf, merge_len) != SUCCESS) {
            free(merge_buf);
            chunk_result_free(&segments);
            chunk_result_free(out);
            return FAILURE;
         }
         merge_len = 0;
      }

      /* Append segment to merge buffer */
      if (merge_len > 0) {
         merge_buf[merge_len++] = '\n';
         merge_buf[merge_len++] = '\n';
      }
      memcpy(merge_buf + merge_len, segments.chunks[i], (size_t)seg_len);
      merge_len += seg_len;
      merge_buf[merge_len] = '\0';
   }

   /* Flush remaining */
   if (merge_len > 0) {
      if (result_add(out, merge_buf, merge_len) != SUCCESS) {
         free(merge_buf);
         chunk_result_free(&segments);
         chunk_result_free(out);
         return FAILURE;
      }
   }

   free(merge_buf);
   chunk_result_free(&segments);

   /* Step 4: Add overlap between chunks */
   if (overlap_chars > 0 && out->count > 1) {
      for (int i = 1; i < out->count; i++) {
         /* Prepend tail of previous chunk to this chunk */
         int prev_len = (int)strlen(out->chunks[i - 1]);
         int cur_len = (int)strlen(out->chunks[i]);

         if (prev_len <= overlap_chars)
            continue; /* Previous chunk too small for meaningful overlap */

         /* Find overlap start: go back overlap_chars from end of previous chunk,
          * then advance to next word boundary */
         int overlap_start = prev_len - overlap_chars;
         while (overlap_start < prev_len &&
                !isspace((unsigned char)out->chunks[i - 1][overlap_start]))
            overlap_start++;
         while (overlap_start < prev_len &&
                isspace((unsigned char)out->chunks[i - 1][overlap_start]))
            overlap_start++;

         if (overlap_start >= prev_len)
            continue;

         int actual_overlap = prev_len - overlap_start;
         int new_len = actual_overlap + 1 + cur_len; /* +1 for space separator */
         char *new_chunk = malloc((size_t)new_len + 1);
         if (!new_chunk)
            continue; /* Non-fatal: skip overlap for this chunk */

         memcpy(new_chunk, out->chunks[i - 1] + overlap_start, (size_t)actual_overlap);
         new_chunk[actual_overlap] = ' ';
         memcpy(new_chunk + actual_overlap + 1, out->chunks[i], (size_t)cur_len);
         new_chunk[new_len] = '\0';

         free(out->chunks[i]);
         out->chunks[i] = new_chunk;
      }
   }

   return 0;
}
