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

#include "memory/memory_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Canonical memory-subsystem tokenizer delimiter set.  Whitespace plus the
 * punctuation classes the extraction prompt + LLM tool query path treat as
 * word boundaries.  Shared across memory_stem_string, memory_stem_count,
 * memory_fact_search.c::tokenize_query, and memory_callback.c::tokenize_query
 * so extraction-time stems and query-time tokens collide on the same token
 * shape.  NOT used by memory_db.c::build_fts5_match_expr — that path splits
 * already-stemmed space-separated input on " " only. */
extern const char *const MEMORY_TOKEN_DELIMS;

/**
 * @brief Initialize the global Porter2 stemmer.
 *
 * Allocates a process-wide `sb_stemmer` for English.  Safe to call
 * multiple times; subsequent calls are no-ops.  Stemming is thread-safe
 * via internal mutex (libstemmer instances are NOT individually
 * thread-safe per its docs).
 *
 * @return SUCCESS (0) on success, FAILURE (1) if libstemmer init fails.
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

/**
 * @brief Tokenize @p input in place on the canonical memory-subsystem
 *        delimiter set (`MEMORY_TOKEN_DELIMS`), lowercasing each token.
 *
 * Writes pointers to each surviving token into @p tokens_out.  Tokens are
 * pointers INTO @p input — they share its lifetime and remain valid only
 * while @p input is in scope.  Caller is responsible for providing a
 * writable working copy of any string they don't want mutated.
 *
 * Shared across memory_stem_string / memory_stem_count and the per-call-site
 * query tokenizers (memory_fact_search.c, memory_callback.c) so the BM25
 * ranking pipeline sees one source of truth for token shape + filter rules.
 *
 * @param input       Input UTF-8 string (will be modified — NUL bytes written
 *                    at every delimiter, all characters lowercased).
 * @param tokens_out  Output array of pointers into @p input.  Each entry
 *                    points to a NUL-terminated token of length >= @p min_len.
 * @param max_tokens  Capacity of @p tokens_out (excess tokens discarded).
 * @param min_len     Minimum token length to keep (typically 2).
 * @return Number of tokens written (0..max_tokens).
 */
int memory_stem_tokenize_in_place(char *input,
                                  const char **tokens_out,
                                  int max_tokens,
                                  int min_len);

/**
 * @brief Tokenize @p keywords into the caller-supplied padded-row buffer.
 *
 * Convenience wrapper around `memory_stem_tokenize_in_place` for sites that
 * use a `char tokens[N][64]` storage shape (memory_callback.c +
 * memory_fact_search.c).  Internally allocates a stack working buffer sized
 * to MEMORY_FACT_STEMS_MAX, lowercases + tokenizes via the canonical
 * delimiter set, and snprintfs each surviving token into the padded row.
 *
 * @param keywords    Input UTF-8 string (caller's storage; not modified).
 * @param tokens      Output: `tokens[i]` is a NUL-terminated lowercase stem.
 *                    Each row holds up to 63 bytes plus NUL.
 * @param max_tokens  Capacity of @p tokens (rows); excess tokens discarded.
 * @param min_len     Minimum stem length to keep (typically 2).
 * @return Number of rows written (0..max_tokens).
 */
int memory_stem_tokenize_padded(const char *keywords,
                                char tokens[][64],
                                int max_tokens,
                                int min_len);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_STEM_H */
