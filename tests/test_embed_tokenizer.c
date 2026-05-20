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
 * Unit tests for the shared WordPiece tokenizer.  Covers single-segment
 * encoding, two-segment ([CLS] q [SEP] p [SEP]) cross-encoder encoding,
 * truncation, padding, and refcount semantics.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "memory/memory_embed_tokenizer.h"
#include "unity.h"

/* Resolve vocab.txt regardless of the test's cwd (ctest invokes from build
 * dir; manual runs may use repo root).  CI doesn't ship the production
 * models/embeddings/ tree (gitignored), so tests/fixtures/embeddings/vocab.txt
 * is the tracked test-fixture copy and is the first candidate. */
static const char *vocab_path(void) {
   const char *candidates[] = { "tests/fixtures/embeddings/vocab.txt",
                                "../tests/fixtures/embeddings/vocab.txt",
                                "../../tests/fixtures/embeddings/vocab.txt",
                                "models/embeddings/vocab.txt",
                                "../models/embeddings/vocab.txt",
                                "../../models/embeddings/vocab.txt",
                                NULL };
   for (int i = 0; candidates[i]; i++) {
      FILE *fp = fopen(candidates[i], "r");
      if (fp) {
         fclose(fp);
         return candidates[i];
      }
   }
   return NULL;
}

void setUp(void) {
}

void tearDown(void) {
   /* Drain any leftover refs so the next test starts clean. */
   while (memory_embed_tokenizer_available()) {
      memory_embed_tokenizer_release();
   }
}

/* ── refcount ──────────────────────────────────────────────────────────── */

static void test_acquire_release_refcount(void) {
   const char *path = vocab_path();
   TEST_ASSERT_NOT_NULL_MESSAGE(path, "vocab.txt not found");

   TEST_ASSERT_EQUAL_INT(0, memory_embed_tokenizer_acquire(path));
   TEST_ASSERT_TRUE(memory_embed_tokenizer_available());

   /* Second acquire = bumped refcount; no reload. */
   TEST_ASSERT_EQUAL_INT(0, memory_embed_tokenizer_acquire(path));
   TEST_ASSERT_TRUE(memory_embed_tokenizer_available());

   memory_embed_tokenizer_release();
   TEST_ASSERT_TRUE(memory_embed_tokenizer_available());

   memory_embed_tokenizer_release();
   TEST_ASSERT_FALSE(memory_embed_tokenizer_available());
}

static void test_acquire_with_null_or_empty_path_fails(void) {
   TEST_ASSERT_NOT_EQUAL(0, memory_embed_tokenizer_acquire(NULL));
   TEST_ASSERT_NOT_EQUAL(0, memory_embed_tokenizer_acquire(""));
   TEST_ASSERT_FALSE(memory_embed_tokenizer_available());
}

/* ── single-segment encoding ────────────────────────────────────────────── */

static void test_encode_single_emits_cls_and_sep(void) {
   const char *path = vocab_path();
   TEST_ASSERT_NOT_NULL_MESSAGE(path, "vocab.txt not found");
   TEST_ASSERT_EQUAL_INT(0, memory_embed_tokenizer_acquire(path));

   const int max_len = 32;
   int64_t ids[max_len];
   int64_t mask[max_len];
   int64_t types[max_len];

   int n = memory_embed_tokenizer_encode("hello world", ids, mask, types, max_len);
   TEST_ASSERT_GREATER_THAN_INT(2, n);
   TEST_ASSERT_EQUAL_INT64(MEMORY_TOKENIZER_TOKEN_CLS, ids[0]);
   TEST_ASSERT_EQUAL_INT64(MEMORY_TOKENIZER_TOKEN_SEP, ids[n - 1]);

   /* Single-segment: every token type ID must be 0. */
   for (int i = 0; i < n; i++) {
      TEST_ASSERT_EQUAL_INT64(0, types[i]);
   }
}

/* ── two-segment (cross-encoder) encoding ───────────────────────────────── */

static void test_encode_pair_emits_two_seps_and_segment_ids(void) {
   const char *path = vocab_path();
   TEST_ASSERT_NOT_NULL_MESSAGE(path, "vocab.txt not found");
   TEST_ASSERT_EQUAL_INT(0, memory_embed_tokenizer_acquire(path));

   const int max_len = 32;
   int64_t ids[max_len];
   int64_t mask[max_len];
   int64_t types[max_len];

   int n = memory_embed_tokenizer_encode_pair("hello", "world example", ids, mask, types, max_len);
   TEST_ASSERT_GREATER_THAN_INT(3, n);

   TEST_ASSERT_EQUAL_INT64(MEMORY_TOKENIZER_TOKEN_CLS, ids[0]);
   TEST_ASSERT_EQUAL_INT64(0, types[0]);
   TEST_ASSERT_EQUAL_INT64(1, mask[0]);

   TEST_ASSERT_EQUAL_INT64(MEMORY_TOKENIZER_TOKEN_SEP, ids[n - 1]);
   TEST_ASSERT_EQUAL_INT64(1, types[n - 1]);

   int sep_count = 0;
   for (int i = 0; i < n; i++) {
      if (ids[i] == MEMORY_TOKENIZER_TOKEN_SEP)
         sep_count++;
   }
   TEST_ASSERT_EQUAL_INT(2, sep_count);

   /* Type IDs must transition 0 → 1 monotonically. */
   bool seen_one = false;
   for (int i = 0; i < n; i++) {
      if (types[i] == 1)
         seen_one = true;
      else if (seen_one)
         TEST_FAIL_MESSAGE("token_type_ids reverted from 1 to 0");
   }
   TEST_ASSERT_TRUE_MESSAGE(seen_one, "no passage tokens emitted");
}

static void test_encode_pair_pads_remainder_with_zero(void) {
   const char *path = vocab_path();
   TEST_ASSERT_NOT_NULL_MESSAGE(path, "vocab.txt not found");
   TEST_ASSERT_EQUAL_INT(0, memory_embed_tokenizer_acquire(path));

   const int max_len = 64;
   int64_t ids[max_len];
   int64_t mask[max_len];
   int64_t types[max_len];

   int n = memory_embed_tokenizer_encode_pair("a", "b", ids, mask, types, max_len);
   TEST_ASSERT_GREATER_THAN_INT(0, n);
   TEST_ASSERT_LESS_THAN_INT(max_len, n);

   for (int i = n; i < max_len; i++) {
      TEST_ASSERT_EQUAL_INT64(0, ids[i]);
      TEST_ASSERT_EQUAL_INT64(0, mask[i]);
      TEST_ASSERT_EQUAL_INT64(0, types[i]);
   }
}

static void test_encode_pair_truncates_long_passage(void) {
   const char *path = vocab_path();
   TEST_ASSERT_NOT_NULL_MESSAGE(path, "vocab.txt not found");
   TEST_ASSERT_EQUAL_INT(0, memory_embed_tokenizer_acquire(path));

   const int max_len = 16;
   int64_t ids[max_len];
   int64_t mask[max_len];
   int64_t types[max_len];

   const char *long_passage =
       "the quick brown fox jumps over the lazy dog and runs through the forest "
       "looking for a place to rest after a long journey under the bright sun";

   int n = memory_embed_tokenizer_encode_pair("query", long_passage, ids, mask, types, max_len);
   TEST_ASSERT_EQUAL_INT(max_len, n);
   /* Final emitted token must still be [SEP] even after truncation. */
   TEST_ASSERT_EQUAL_INT64(MEMORY_TOKENIZER_TOKEN_SEP, ids[max_len - 1]);
}

static void test_encode_pair_null_passage_is_safe(void) {
   const char *path = vocab_path();
   TEST_ASSERT_NOT_NULL_MESSAGE(path, "vocab.txt not found");
   TEST_ASSERT_EQUAL_INT(0, memory_embed_tokenizer_acquire(path));

   const int max_len = 16;
   int64_t ids[max_len];
   int64_t mask[max_len];
   int64_t types[max_len];

   int n = memory_embed_tokenizer_encode_pair("hello", NULL, ids, mask, types, max_len);
   TEST_ASSERT_GREATER_THAN_INT(2, n);
   TEST_ASSERT_EQUAL_INT64(MEMORY_TOKENIZER_TOKEN_CLS, ids[0]);
   TEST_ASSERT_EQUAL_INT64(MEMORY_TOKENIZER_TOKEN_SEP, ids[n - 1]);
}

static void test_encode_pair_max_len_too_small_returns_zero(void) {
   const char *path = vocab_path();
   TEST_ASSERT_NOT_NULL_MESSAGE(path, "vocab.txt not found");
   TEST_ASSERT_EQUAL_INT(0, memory_embed_tokenizer_acquire(path));

   int64_t ids[8];
   int64_t mask[8];
   int64_t types[8];
   /* max_len < 4 cannot fit [CLS] + 1 query + [SEP] + [SEP]. */
   TEST_ASSERT_EQUAL_INT(0, memory_embed_tokenizer_encode_pair("q", "p", ids, mask, types, 3));
}

int main(void) {
   UNITY_BEGIN();

   RUN_TEST(test_acquire_release_refcount);
   RUN_TEST(test_acquire_with_null_or_empty_path_fails);

   RUN_TEST(test_encode_single_emits_cls_and_sep);

   RUN_TEST(test_encode_pair_emits_two_seps_and_segment_ids);
   RUN_TEST(test_encode_pair_pads_remainder_with_zero);
   RUN_TEST(test_encode_pair_truncates_long_passage);
   RUN_TEST(test_encode_pair_null_passage_is_safe);
   RUN_TEST(test_encode_pair_max_len_too_small_returns_zero);

   return UNITY_END();
}
