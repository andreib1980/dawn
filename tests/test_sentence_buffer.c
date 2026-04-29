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
 * Unit tests for the sentence buffer used by the TTS streaming pipeline.
 */

#include <string.h>

#include "unity.h"
#include "utils/sentence_buffer.h"

#define MAX_CAPTURED 16
#define MAX_SENTENCE_LEN 256

static int sentence_count;
static char captured[MAX_CAPTURED][MAX_SENTENCE_LEN];

static void capture_callback(const char *sentence, void *userdata) {
   (void)userdata;
   if (sentence_count < MAX_CAPTURED) {
      strncpy(captured[sentence_count], sentence, MAX_SENTENCE_LEN - 1);
      captured[sentence_count][MAX_SENTENCE_LEN - 1] = '\0';
   }
   sentence_count++;
}

void setUp(void) {
   sentence_count = 0;
   memset(captured, 0, sizeof(captured));
}

void tearDown(void) {
}

void test_simple_sentence(void) {
   sentence_buffer_t *buf = sentence_buffer_create(capture_callback, NULL);
   TEST_ASSERT_NOT_NULL(buf);

   sentence_buffer_feed(buf, "Hello world. ");
   TEST_ASSERT_EQUAL_INT(1, sentence_count);
   TEST_ASSERT_EQUAL_STRING("Hello world.", captured[0]);

   sentence_buffer_free(buf);
}

void test_split_across_chunks(void) {
   sentence_buffer_t *buf = sentence_buffer_create(capture_callback, NULL);
   TEST_ASSERT_NOT_NULL(buf);

   sentence_buffer_feed(buf, "This is ");
   TEST_ASSERT_EQUAL_INT(0, sentence_count);
   sentence_buffer_feed(buf, "a test. ");
   TEST_ASSERT_EQUAL_INT(1, sentence_count);
   TEST_ASSERT_EQUAL_STRING("This is a test.", captured[0]);

   sentence_buffer_free(buf);
}

void test_multiple_terminators(void) {
   sentence_buffer_t *buf = sentence_buffer_create(capture_callback, NULL);
   TEST_ASSERT_NOT_NULL(buf);

   sentence_buffer_feed(buf, "Question? ");
   TEST_ASSERT_EQUAL_INT(1, sentence_count);
   TEST_ASSERT_EQUAL_STRING("Question?", captured[0]);

   sentence_buffer_feed(buf, "Exclamation! ");
   TEST_ASSERT_EQUAL_INT(2, sentence_count);
   TEST_ASSERT_EQUAL_STRING("Exclamation!", captured[1]);

   /* Colon is NOT a sentence terminator (only on :\n) */
   sentence_buffer_feed(buf, "Note: ");
   TEST_ASSERT_EQUAL_INT(2, sentence_count);

   sentence_buffer_free(buf);
}

void test_multiple_sentences_one_chunk(void) {
   sentence_buffer_t *buf = sentence_buffer_create(capture_callback, NULL);
   TEST_ASSERT_NOT_NULL(buf);

   sentence_buffer_feed(buf, "First sentence. Second sentence! Third one? ");
   TEST_ASSERT_EQUAL_INT(3, sentence_count);
   TEST_ASSERT_EQUAL_STRING("First sentence.", captured[0]);
   TEST_ASSERT_EQUAL_STRING("Second sentence!", captured[1]);
   TEST_ASSERT_EQUAL_STRING("Third one?", captured[2]);

   sentence_buffer_free(buf);
}

void test_flush_incomplete(void) {
   sentence_buffer_t *buf = sentence_buffer_create(capture_callback, NULL);
   TEST_ASSERT_NOT_NULL(buf);

   sentence_buffer_feed(buf, "Incomplete without terminator");
   TEST_ASSERT_EQUAL_INT(0, sentence_count);

   sentence_buffer_flush(buf);
   TEST_ASSERT_EQUAL_INT(1, sentence_count);
   TEST_ASSERT_EQUAL_STRING("Incomplete without terminator", captured[0]);

   sentence_buffer_free(buf);
}

void test_token_by_token(void) {
   sentence_buffer_t *buf = sentence_buffer_create(capture_callback, NULL);
   TEST_ASSERT_NOT_NULL(buf);

   sentence_buffer_feed(buf, "Hello");
   sentence_buffer_feed(buf, "!");
   sentence_buffer_feed(buf, " ");
   TEST_ASSERT_EQUAL_INT(1, sentence_count);
   TEST_ASSERT_EQUAL_STRING("Hello!", captured[0]);

   sentence_buffer_feed(buf, "2");
   sentence_buffer_feed(buf, " ");
   sentence_buffer_feed(buf, "+");
   sentence_buffer_feed(buf, " ");
   sentence_buffer_feed(buf, "2");
   sentence_buffer_feed(buf, " ");
   sentence_buffer_feed(buf, "equals");
   sentence_buffer_feed(buf, " ");
   sentence_buffer_feed(buf, "4");
   sentence_buffer_feed(buf, ".");
   sentence_buffer_flush(buf);
   TEST_ASSERT_EQUAL_INT(2, sentence_count);
   TEST_ASSERT_EQUAL_STRING("2 + 2 equals 4.", captured[1]);

   sentence_buffer_free(buf);
}

void test_paragraph_break(void) {
   sentence_buffer_t *buf = sentence_buffer_create(capture_callback, NULL);
   TEST_ASSERT_NOT_NULL(buf);

   sentence_buffer_feed(buf, "Hello!\n\n2 + 2 equals 4.");
   TEST_ASSERT_EQUAL_INT(1, sentence_count);
   TEST_ASSERT_EQUAL_STRING("Hello!", captured[0]);

   sentence_buffer_flush(buf);
   TEST_ASSERT_EQUAL_INT(2, sentence_count);
   TEST_ASSERT_EQUAL_STRING("2 + 2 equals 4.", captured[1]);

   sentence_buffer_free(buf);
}

void test_create_null_callback(void) {
   sentence_buffer_t *buf = sentence_buffer_create(NULL, NULL);
   TEST_ASSERT_NULL(buf);
}

void test_clear_discards(void) {
   sentence_buffer_t *buf = sentence_buffer_create(capture_callback, NULL);
   TEST_ASSERT_NOT_NULL(buf);

   sentence_buffer_feed(buf, "Buffered text");
   sentence_buffer_clear(buf);
   sentence_buffer_flush(buf);
   TEST_ASSERT_EQUAL_INT(0, sentence_count);

   sentence_buffer_free(buf);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_simple_sentence);
   RUN_TEST(test_split_across_chunks);
   RUN_TEST(test_multiple_terminators);
   RUN_TEST(test_multiple_sentences_one_chunk);
   RUN_TEST(test_flush_incomplete);
   RUN_TEST(test_token_by_token);
   RUN_TEST(test_paragraph_break);
   RUN_TEST(test_create_null_callback);
   RUN_TEST(test_clear_discards);
   return UNITY_END();
}
