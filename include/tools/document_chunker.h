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
 * Document Text Chunker - splits text into overlapping chunks for embedding
 *
 * Algorithm:
 *   1. Split on paragraph boundaries (double newline)
 *   2. If paragraph > max tokens: split on sentence boundaries
 *   3. Merge small consecutive chunks until target size reached
 *   4. Add configurable overlap between chunks
 */

#ifndef DOCUMENT_CHUNKER_H
#define DOCUMENT_CHUNKER_H

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Configuration
 * ============================================================================= */

typedef struct {
   int target_tokens;  /* Target chunk size (default 500) */
   int max_tokens;     /* Hard max before forced split (default 1000) */
   int overlap_tokens; /* Overlap between consecutive chunks (default 50) */
} chunk_config_t;

/* Default configuration */
#define CHUNK_CONFIG_DEFAULT \
   { .target_tokens = 500, .max_tokens = 1000, .overlap_tokens = 50 }

/* =============================================================================
 * Result
 * ============================================================================= */

typedef struct {
   char **chunks; /* Array of null-terminated chunk strings */
   int count;     /* Number of chunks */
   int capacity;  /* Allocated slots (internal) */
} chunk_result_t;

/* =============================================================================
 * API
 * ============================================================================= */

/**
 * @brief Split text into overlapping chunks suitable for embedding
 *
 * @param text      Input text (null-terminated)
 * @param config    Chunking parameters (NULL for defaults)
 * @param out       Output chunk list (caller must free with chunk_result_free)
 * @return SUCCESS or FAILURE
 */
int document_chunk_text(const char *text, const chunk_config_t *config, chunk_result_t *out);

/**
 * @brief Free all memory in a chunk result
 */
void chunk_result_free(chunk_result_t *result);

/**
 * @brief Estimate token count for a text string
 *
 * Uses chars/4 heuristic (reasonable for English text with BPE tokenizers).
 */
int chunk_estimate_tokens(const char *text, int len);

#ifdef __cplusplus
}
#endif

#endif /* DOCUMENT_CHUNKER_H */
