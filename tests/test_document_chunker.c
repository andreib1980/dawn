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
 * Unit tests for document_chunker.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tools/document_chunker.h"

static int passed = 0;
static int failed = 0;

#define ASSERT(cond, msg)                                  \
   do {                                                    \
      if (cond) {                                          \
         printf("  [PASS] %s\n", msg);                     \
         passed++;                                         \
      } else {                                             \
         printf("  [FAIL] %s (line %d)\n", msg, __LINE__); \
         failed++;                                         \
      }                                                    \
   } while (0)

/* =============================================================================
 * Test Cases
 * ============================================================================= */

static void test_empty_input(void) {
   printf("\n--- test_empty_input ---\n");
   chunk_result_t result;

   int rc = document_chunk_text("", NULL, &result);
   ASSERT(rc == 0, "empty string returns success");
   ASSERT(result.count == 0, "empty string produces 0 chunks");
   chunk_result_free(&result);

   rc = document_chunk_text(NULL, NULL, &result);
   ASSERT(rc != 0, "NULL input returns error");
}

static void test_single_sentence(void) {
   printf("\n--- test_single_sentence ---\n");
   chunk_result_t result;
   const char *text = "This is a simple sentence.";

   int rc = document_chunk_text(text, NULL, &result);
   ASSERT(rc == 0, "single sentence returns success");
   ASSERT(result.count == 1, "single sentence produces 1 chunk");
   ASSERT(strcmp(result.chunks[0], text) == 0, "chunk matches input");
   chunk_result_free(&result);
}

static void test_two_paragraphs_small(void) {
   printf("\n--- test_two_paragraphs_small ---\n");
   chunk_result_t result;
   /* Two short paragraphs that should merge into one chunk */
   const char *text = "First paragraph here.\n\nSecond paragraph here.";

   chunk_config_t cfg = { .target_tokens = 500, .max_tokens = 1000, .overlap_tokens = 0 };
   int rc = document_chunk_text(text, &cfg, &result);
   ASSERT(rc == 0, "returns success");
   ASSERT(result.count == 1, "two small paragraphs merge into 1 chunk");
   ASSERT(strstr(result.chunks[0], "First paragraph") != NULL, "contains first paragraph");
   ASSERT(strstr(result.chunks[0], "Second paragraph") != NULL, "contains second paragraph");
   chunk_result_free(&result);
}

static void test_paragraph_splitting(void) {
   printf("\n--- test_paragraph_splitting ---\n");
   chunk_result_t result;

   /* Create text with multiple paragraphs that exceed target size */
   char text[8192];
   int pos = 0;
   for (int i = 0; i < 10; i++) {
      if (i > 0) {
         pos += sprintf(text + pos, "\n\n");
      }
      /* Each paragraph ~200 chars = ~50 tokens */
      for (int j = 0; j < 5; j++) {
         pos += sprintf(text + pos, "This is sentence number %d in paragraph %d. ", j + 1, i + 1);
      }
   }

   /* target=120 tokens, so ~2 paragraphs per chunk (~53 tokens each) */
   chunk_config_t cfg = { .target_tokens = 120, .max_tokens = 500, .overlap_tokens = 0 };
   int rc = document_chunk_text(text, &cfg, &result);
   ASSERT(rc == 0, "returns success");
   ASSERT(result.count > 1, "multiple paragraphs produce multiple chunks");
   ASSERT(result.count < 10, "some merging occurred");

   /* Verify no chunk is empty */
   int all_nonempty = 1;
   for (int i = 0; i < result.count; i++) {
      if (strlen(result.chunks[i]) == 0)
         all_nonempty = 0;
   }
   ASSERT(all_nonempty, "all chunks are non-empty");

   chunk_result_free(&result);
}

static void test_sentence_splitting(void) {
   printf("\n--- test_sentence_splitting ---\n");
   chunk_result_t result;

   /* One giant paragraph that needs sentence-level splitting */
   char text[8192];
   int pos = 0;
   for (int i = 0; i < 40; i++) {
      pos += sprintf(text + pos, "This is test sentence number %d with some filler words. ", i + 1);
   }

   /* max=100 tokens (~400 chars), so the single paragraph must be split */
   chunk_config_t cfg = { .target_tokens = 50, .max_tokens = 100, .overlap_tokens = 0 };
   int rc = document_chunk_text(text, &cfg, &result);
   ASSERT(rc == 0, "returns success");
   ASSERT(result.count > 1, "large paragraph split into multiple chunks");

   /* Each chunk should be under max_tokens */
   int all_under_max = 1;
   for (int i = 0; i < result.count; i++) {
      int tokens = chunk_estimate_tokens(result.chunks[i], (int)strlen(result.chunks[i]));
      if (tokens > cfg.max_tokens + 10) /* small tolerance */
         all_under_max = 0;
   }
   ASSERT(all_under_max, "all chunks under max_tokens");

   chunk_result_free(&result);
}

static void test_abbreviations(void) {
   printf("\n--- test_abbreviations ---\n");
   chunk_result_t result;

   /* Text with abbreviations that shouldn't be treated as sentence boundaries */
   const char *text = "Dr. Smith met with Mrs. Jones at 3.14 Main St. "
                      "They discussed the results. The meeting was productive.";

   chunk_config_t cfg = { .target_tokens = 500, .max_tokens = 1000, .overlap_tokens = 0 };
   int rc = document_chunk_text(text, &cfg, &result);
   ASSERT(rc == 0, "returns success");
   ASSERT(result.count == 1, "abbreviations don't cause extra splits");
   ASSERT(strstr(result.chunks[0], "Dr. Smith") != NULL, "Dr. preserved");
   ASSERT(strstr(result.chunks[0], "Mrs. Jones") != NULL, "Mrs. preserved");
   chunk_result_free(&result);
}

static void test_overlap(void) {
   printf("\n--- test_overlap ---\n");
   chunk_result_t result;

   /* Create two paragraphs that will be separate chunks */
   char text[4096];
   int pos = 0;
   /* Paragraph 1: ~150 tokens */
   for (int i = 0; i < 12; i++)
      pos += sprintf(text + pos, "Alpha sentence number %d with content. ", i + 1);
   pos += sprintf(text + pos, "\n\n");
   /* Paragraph 2: ~150 tokens */
   for (int i = 0; i < 12; i++)
      pos += sprintf(text + pos, "Beta sentence number %d with content. ", i + 1);

   /* Target 100 tokens so each paragraph becomes its own chunk, overlap 25 */
   chunk_config_t cfg = { .target_tokens = 100, .max_tokens = 500, .overlap_tokens = 25 };
   int rc = document_chunk_text(text, &cfg, &result);
   ASSERT(rc == 0, "returns success");
   ASSERT(result.count == 2, "two separate chunks");

   if (result.count >= 2) {
      /* Second chunk should contain some text from end of first chunk */
      ASSERT(strstr(result.chunks[1], "Alpha") != NULL,
             "overlap: chunk 2 contains text from chunk 1");
      ASSERT(strstr(result.chunks[1], "Beta") != NULL, "overlap: chunk 2 contains its own text");
   }

   chunk_result_free(&result);
}

static void test_token_estimation(void) {
   printf("\n--- test_token_estimation ---\n");

   ASSERT(chunk_estimate_tokens("hello", 5) == 2, "5 chars ≈ 2 tokens");
   ASSERT(chunk_estimate_tokens("", 0) == 0, "0 chars = 0 tokens");
   ASSERT(chunk_estimate_tokens(NULL, 0) == 0, "NULL = 0 tokens");
   ASSERT(chunk_estimate_tokens("abcdefghijklmnop", 16) == 4, "16 chars ≈ 4 tokens");
}

static void test_whitespace_only(void) {
   printf("\n--- test_whitespace_only ---\n");
   chunk_result_t result;

   int rc = document_chunk_text("   \n\n   \n\n   ", NULL, &result);
   ASSERT(rc == 0, "whitespace-only returns success");
   ASSERT(result.count == 0, "whitespace-only produces 0 chunks");
   chunk_result_free(&result);
}

static void test_single_long_word(void) {
   printf("\n--- test_single_long_word ---\n");
   chunk_result_t result;

   /* A single "word" longer than max_tokens — must force-split */
   char text[2048];
   memset(text, 'x', 2000);
   text[2000] = '\0';

   chunk_config_t cfg = { .target_tokens = 50, .max_tokens = 100, .overlap_tokens = 0 };
   int rc = document_chunk_text(text, &cfg, &result);
   ASSERT(rc == 0, "long word returns success");
   ASSERT(result.count >= 1, "long word produces at least 1 chunk");

   /* Verify all text is captured */
   int total_len = 0;
   for (int i = 0; i < result.count; i++)
      total_len += (int)strlen(result.chunks[i]);
   ASSERT(total_len >= 1900, "most text preserved after force-split");

   chunk_result_free(&result);
}

/* =============================================================================
 * Main
 * ============================================================================= */

int main(void) {
   printf("=== Document Chunker Unit Tests ===\n");

   test_empty_input();
   test_single_sentence();
   test_two_paragraphs_small();
   test_paragraph_splitting();
   test_sentence_splitting();
   test_abbreviations();
   test_overlap();
   test_token_estimation();
   test_whitespace_only();
   test_single_long_word();

   printf("\n========================================\n");
   printf("Results: %d passed, %d failed\n", passed, failed);
   printf("========================================\n");

   return failed > 0 ? 1 : 0;
}
