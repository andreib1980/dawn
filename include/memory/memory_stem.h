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
 * Porter2 stemmer wrapper (libstemmer / Snowball).
 *
 * Used by the BM25 keyword path to normalize plurals + verb tenses
 * before FTS5 indexing.  Replaces Mem0's spaCy lemmatization with a
 * C-native option; loses noun/verb disambiguation but covers the
 * common forms (memories → memori, attending → attend, organized →
 * organ).  See docs/MEM0_ARCHITECTURAL_PARITY.md.
 */

/**
 * @file memory_stem.h
 * @brief Porter2 stemming for fact text + queries.
 */

#ifndef MEMORY_STEM_H
#define MEMORY_STEM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Canonical size for buffers holding stemmed fact_text.  Porter2 only
 * shortens tokens (suffix manipulation), so the stem output is always
 * less-than-or-equal to the input length.  fact_text is bounded by
 * MEMORY_FACT_TEXT_MAX (512) — this leaves a comfortable margin for the
 * delimiter-replacing-with-space framing and any edge cases (e.g., the
 * tokenizer dropping multi-byte UTF-8 punctuation in favor of single
 * spaces). */
#define MEMORY_FACT_STEMS_MAX 768

/**
 * @brief Initialize the global Porter2 stemmer.
 *
 * Allocates a process-wide `sb_stemmer` for English.  Safe to call
 * multiple times; subsequent calls are no-ops.  Stemming is thread-safe
 * via internal mutex (libstemmer instances are NOT individually
 * thread-safe per its docs).
 *
 * @return 0 on success, non-zero if libstemmer init fails.
 */
int memory_stem_init(void);

/**
 * @brief Tear down the global stemmer.  Safe to call without a prior init.
 */
void memory_stem_shutdown(void);

/**
 * @brief Stem an input string into space-separated stemmed tokens.
 *
 * Tokenizes on the same delimiter class used by the existing tokenize
 * helper in memory_fact_search.c (whitespace + common punctuation),
 * lowercases each token, drops single-char tokens, and stems via
 * Porter2.  Output stems are joined with single spaces.
 *
 * Stems are stable across calls — passing "memories" or "memorize"
 * yields the same root, so query-time and index-time tokens collide.
 *
 * If stemming is uninitialized or fails, the function lowercases +
 * tokenizes only (graceful degradation; BM25 still works on raw forms).
 *
 * @param input   Input UTF-8 string (NUL-terminated).  May be NULL.
 * @param out     Output buffer; will receive a NUL-terminated string.
 * @param out_sz  Output buffer size (bytes).
 * @return Number of stems written, or 0 on empty/NULL input.
 */
int memory_stem_string(const char *input, char *out, size_t out_sz);

/**
 * @brief Count tokens that memory_stem_string would emit.
 *
 * Counts tokens of 2+ characters after delimiter-splitting; equivalent
 * to the stem count for English input since Porter2 never produces
 * empty output for a non-empty lowercase token.  Used by callers that
 * need the term-count to pick BM25 sigmoid parameters before
 * constructing the FTS5 MATCH expression.  Does NOT invoke libstemmer.
 *
 * @param input Input string.  May be NULL.
 * @return Number of tokens (>= 0) that would survive memory_stem_string.
 */
int memory_stem_count(const char *input);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_STEM_H */
