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
 * ONNX Embedding Provider
 *
 * Local embedding generation using all-MiniLM-L6-v2 (int8 quantized).
 * Implements WordPiece tokenizer and ONNX Runtime inference with
 * mean pooling to produce 384-dimensional sentence embeddings.
 *
 * Model files: models/embeddings/all-MiniLM-L6-v2-int8.onnx
 *              models/embeddings/vocab.txt
 */

#include <ctype.h>
#include <math.h>
#include <onnxruntime_c_api.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dawn_error.h"
#include "logging.h"
#include "memory/memory_embeddings.h"

/* =============================================================================
 * Constants
 * ============================================================================= */

#define ONNX_MAX_SEQ_LEN 256   /* Max tokens (MiniLM limit) */
#define ONNX_HIDDEN_DIM 384    /* MiniLM output dimension */
#define VOCAB_HASH_SIZE 65536  /* Hash table size (~30K entries, 0.46 load factor) */
#define VOCAB_MAX_WORD_LEN 128 /* Max token length in vocab */
#define MODEL_PATH "models/embeddings/all-MiniLM-L6-v2-int8.onnx"
#define VOCAB_PATH "models/embeddings/vocab.txt"

/* Special token IDs (standard BERT/MiniLM) */
#define TOKEN_CLS 101
#define TOKEN_SEP 102
#define TOKEN_UNK 100

/* =============================================================================
 * WordPiece Hash Table
 * ============================================================================= */

typedef struct vocab_entry {
   char word[VOCAB_MAX_WORD_LEN];
   int token_id;
   struct vocab_entry *next; /* Chain for collisions */
} vocab_entry_t;

static vocab_entry_t *s_vocab_table[VOCAB_HASH_SIZE];
static int s_vocab_size = 0;

/* FNV-1a hash */
static uint32_t vocab_hash(const char *str) {
   uint32_t h = 2166136261u;
   for (; *str; str++) {
      h ^= (uint8_t)*str;
      h *= 16777619u;
   }
   return h & (VOCAB_HASH_SIZE - 1);
}

static int vocab_lookup(const char *word) {
   uint32_t idx = vocab_hash(word);
   vocab_entry_t *e = s_vocab_table[idx];
   while (e) {
      if (strcmp(e->word, word) == 0)
         return e->token_id;
      e = e->next;
   }
   return -1; /* Not found */
}

static int vocab_load(const char *path) {
   FILE *fp = fopen(path, "r");
   if (!fp) {
      OLOG_ERROR("memory_embed_onnx: cannot open vocab: %s", path);
      return FAILURE;
   }

   memset(s_vocab_table, 0, sizeof(s_vocab_table));

   char line[VOCAB_MAX_WORD_LEN];
   int id = 0;
   while (fgets(line, sizeof(line), fp)) {
      /* Strip newline */
      size_t len = strlen(line);
      while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
         line[--len] = '\0';

      if (len == 0) {
         id++;
         continue;
      }

      uint32_t idx = vocab_hash(line);
      vocab_entry_t *entry = malloc(sizeof(vocab_entry_t));
      if (!entry)
         break;
      strncpy(entry->word, line, VOCAB_MAX_WORD_LEN - 1);
      entry->word[VOCAB_MAX_WORD_LEN - 1] = '\0';
      entry->token_id = id;
      entry->next = s_vocab_table[idx];
      s_vocab_table[idx] = entry;
      id++;
   }

   fclose(fp);
   s_vocab_size = id;
   OLOG_INFO("memory_embed_onnx: loaded vocab with %d entries", s_vocab_size);
   return 0;
}

static void vocab_free(void) {
   for (int i = 0; i < VOCAB_HASH_SIZE; i++) {
      vocab_entry_t *e = s_vocab_table[i];
      while (e) {
         vocab_entry_t *next = e->next;
         free(e);
         e = next;
      }
      s_vocab_table[i] = NULL;
   }
   s_vocab_size = 0;
}

/* =============================================================================
 * WordPiece Tokenizer
 * ============================================================================= */

/**
 * @brief Tokenize text using WordPiece algorithm
 *
 * 1. Lowercase and split on whitespace/punctuation
 * 2. For each word, greedily match longest prefix in vocab
 * 3. Subsequent pieces get ## prefix
 * 4. Prepend [CLS], append [SEP]
 *
 * @param text Input text
 * @param input_ids Output token IDs
 * @param attention_mask Output attention mask (1 for real tokens)
 * @param token_type_ids Output token type IDs (all 0 for single sentence)
 * @param max_len Maximum sequence length
 * @return Number of tokens produced
 */
static int wordpiece_tokenize(const char *text,
                              int64_t *input_ids,
                              int64_t *attention_mask,
                              int64_t *token_type_ids,
                              int max_len) {
   if (!text || !input_ids || !attention_mask || !token_type_ids || max_len < 3)
      return 0;

   /* Start with [CLS] */
   int pos = 0;
   input_ids[pos] = TOKEN_CLS;
   attention_mask[pos] = 1;
   token_type_ids[pos] = 0;
   pos++;

   /* Lowercase copy for tokenization */
   size_t text_len = strlen(text);
   if (text_len > 4096)
      text_len = 4096;
   char lower[4097];
   for (size_t i = 0; i < text_len; i++) {
      lower[i] = (char)tolower((unsigned char)text[i]);
   }
   lower[text_len] = '\0';

   /* Split into words and tokenize each */
   const char *p = lower;
   while (*p && pos < max_len - 1) {
      /* Skip whitespace */
      while (*p && (isspace((unsigned char)*p) || *p == '\0'))
         p++;
      if (!*p)
         break;

      /* Find word boundary (split on whitespace and punctuation) */
      const char *word_start = p;
      while (*p && !isspace((unsigned char)*p)) {
         /* Treat punctuation as separate tokens */
         if (ispunct((unsigned char)*p) && p > word_start)
            break;
         if (ispunct((unsigned char)*p)) {
            p++;
            break;
         }
         p++;
      }

      int word_len = (int)(p - word_start);
      if (word_len <= 0)
         continue;

      /* WordPiece: greedy longest-match */
      int offset = 0;
      bool is_first_piece = true;

      while (offset < word_len && pos < max_len - 1) {
         int best_len = 0;
         int best_id = TOKEN_UNK;

         /* Try decreasing lengths */
         for (int try_len = word_len - offset; try_len > 0; try_len--) {
            char piece[VOCAB_MAX_WORD_LEN];
            int prefix_len = 0;

            if (!is_first_piece) {
               piece[0] = '#';
               piece[1] = '#';
               prefix_len = 2;
            }

            if (prefix_len + try_len >= VOCAB_MAX_WORD_LEN)
               continue;

            memcpy(piece + prefix_len, word_start + offset, (size_t)try_len);
            piece[prefix_len + try_len] = '\0';

            int id = vocab_lookup(piece);
            if (id >= 0) {
               best_len = try_len;
               best_id = id;
               break;
            }
         }

         if (best_len == 0) {
            /* No match — emit [UNK] for remaining word */
            input_ids[pos] = TOKEN_UNK;
            attention_mask[pos] = 1;
            token_type_ids[pos] = 0;
            pos++;
            break;
         }

         input_ids[pos] = best_id;
         attention_mask[pos] = 1;
         token_type_ids[pos] = 0;
         pos++;

         offset += best_len;
         is_first_piece = false;
      }
   }

   /* Append [SEP] */
   if (pos < max_len) {
      input_ids[pos] = TOKEN_SEP;
      attention_mask[pos] = 1;
      token_type_ids[pos] = 0;
      pos++;
   }

   /* Zero-pad remaining positions */
   for (int i = pos; i < max_len; i++) {
      input_ids[i] = 0;
      attention_mask[i] = 0;
      token_type_ids[i] = 0;
   }

   return pos;
}

/* =============================================================================
 * ONNX Runtime State
 * ============================================================================= */

static struct {
   const OrtApi *ort;
   OrtEnv *env;
   OrtSession *session;
   OrtMemoryInfo *memory_info;
   bool initialized;
} s_onnx;

/* =============================================================================
 * Provider Implementation
 * ============================================================================= */

static int onnx_init(const char *endpoint, const char *model, const char *api_key) {
   (void)endpoint;
   (void)model;
   (void)api_key;

   memset(&s_onnx, 0, sizeof(s_onnx));

   /* Load vocabulary */
   if (vocab_load(VOCAB_PATH) != 0) {
      return FAILURE;
   }

   /* Get ONNX Runtime API */
   s_onnx.ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);
   if (!s_onnx.ort) {
      OLOG_ERROR("memory_embed_onnx: failed to get ONNX Runtime API");
      vocab_free();
      return FAILURE;
   }

   /* Create environment */
   OrtStatus *status = s_onnx.ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "memory_embed",
                                             &s_onnx.env);
   if (status != NULL) {
      OLOG_ERROR("memory_embed_onnx: create env failed: %s", s_onnx.ort->GetErrorMessage(status));
      s_onnx.ort->ReleaseStatus(status);
      vocab_free();
      return FAILURE;
   }

   /* Session options */
   OrtSessionOptions *opts;
   status = s_onnx.ort->CreateSessionOptions(&opts);
   if (status != NULL) {
      OLOG_ERROR("memory_embed_onnx: create session options failed: %s",
                 s_onnx.ort->GetErrorMessage(status));
      s_onnx.ort->ReleaseStatus(status);
      s_onnx.ort->ReleaseEnv(s_onnx.env);
      vocab_free();
      return FAILURE;
   }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"
   s_onnx.ort->SetIntraOpNumThreads(opts, 1);
   s_onnx.ort->SetSessionGraphOptimizationLevel(opts, ORT_ENABLE_ALL);
#pragma GCC diagnostic pop

   /* Load model */
   status = s_onnx.ort->CreateSession(s_onnx.env, MODEL_PATH, opts, &s_onnx.session);
   s_onnx.ort->ReleaseSessionOptions(opts);

   if (status != NULL) {
      OLOG_ERROR("memory_embed_onnx: load model failed: %s", s_onnx.ort->GetErrorMessage(status));
      s_onnx.ort->ReleaseStatus(status);
      s_onnx.ort->ReleaseEnv(s_onnx.env);
      vocab_free();
      return FAILURE;
   }

   /* Memory info */
   status = s_onnx.ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault,
                                            &s_onnx.memory_info);
   if (status != NULL) {
      OLOG_ERROR("memory_embed_onnx: create memory info failed: %s",
                 s_onnx.ort->GetErrorMessage(status));
      s_onnx.ort->ReleaseStatus(status);
      s_onnx.ort->ReleaseSession(s_onnx.session);
      s_onnx.ort->ReleaseEnv(s_onnx.env);
      vocab_free();
      return FAILURE;
   }

   s_onnx.initialized = true;
   OLOG_INFO("memory_embed_onnx: ONNX provider initialized (model: %s)", MODEL_PATH);
   return 0;
}

static void onnx_cleanup(void) {
   if (!s_onnx.initialized)
      return;

   if (s_onnx.memory_info)
      s_onnx.ort->ReleaseMemoryInfo(s_onnx.memory_info);
   if (s_onnx.session)
      s_onnx.ort->ReleaseSession(s_onnx.session);
   if (s_onnx.env)
      s_onnx.ort->ReleaseEnv(s_onnx.env);

   vocab_free();
   s_onnx.initialized = false;
   OLOG_INFO("memory_embed_onnx: cleanup complete");
}

/**
 * @brief Run ONNX inference and mean-pool to get sentence embedding
 *
 * Input: 3 tensors (input_ids, attention_mask, token_type_ids) [1, seq_len]
 * Output: [1, seq_len, 384] -> mean pool over seq_len -> [384]
 */
static int onnx_embed(const char *text, float *out, int max_dims, int *out_dims) {
   if (!s_onnx.initialized || !text || !out || !out_dims)
      return FAILURE;

   /* Tokenize */
   int64_t input_ids[ONNX_MAX_SEQ_LEN];
   int64_t attention_mask[ONNX_MAX_SEQ_LEN];
   int64_t token_type_ids[ONNX_MAX_SEQ_LEN];

   int seq_len = wordpiece_tokenize(text, input_ids, attention_mask, token_type_ids,
                                    ONNX_MAX_SEQ_LEN);
   if (seq_len <= 2) {
      /* Only [CLS] and [SEP] — no real content */
      *out_dims = 0;
      return FAILURE;
   }

   /* Create input tensors */
   int64_t shape[2] = { 1, seq_len };

   OrtValue *input_tensors[3] = { NULL, NULL, NULL };
   OrtStatus *status;

   status = s_onnx.ort->CreateTensorWithDataAsOrtValue(s_onnx.memory_info, input_ids,
                                                       seq_len * sizeof(int64_t), shape, 2,
                                                       ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64,
                                                       &input_tensors[0]);
   if (status != NULL) {
      s_onnx.ort->ReleaseStatus(status);
      return FAILURE;
   }

   status = s_onnx.ort->CreateTensorWithDataAsOrtValue(s_onnx.memory_info, attention_mask,
                                                       seq_len * sizeof(int64_t), shape, 2,
                                                       ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64,
                                                       &input_tensors[1]);
   if (status != NULL) {
      s_onnx.ort->ReleaseStatus(status);
      s_onnx.ort->ReleaseValue(input_tensors[0]);
      return FAILURE;
   }

   status = s_onnx.ort->CreateTensorWithDataAsOrtValue(s_onnx.memory_info, token_type_ids,
                                                       seq_len * sizeof(int64_t), shape, 2,
                                                       ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64,
                                                       &input_tensors[2]);
   if (status != NULL) {
      s_onnx.ort->ReleaseStatus(status);
      s_onnx.ort->ReleaseValue(input_tensors[0]);
      s_onnx.ort->ReleaseValue(input_tensors[1]);
      return FAILURE;
   }

   /* Run inference */
   const char *input_names[] = { "input_ids", "attention_mask", "token_type_ids" };
   const char *output_names[] = { "last_hidden_state" };
   OrtValue *output_tensor = NULL;

   status = s_onnx.ort->Run(s_onnx.session, NULL, input_names,
                            (const OrtValue *const *)input_tensors, 3, output_names, 1,
                            &output_tensor);

   /* Release inputs */
   s_onnx.ort->ReleaseValue(input_tensors[0]);
   s_onnx.ort->ReleaseValue(input_tensors[1]);
   s_onnx.ort->ReleaseValue(input_tensors[2]);

   if (status != NULL) {
      OLOG_ERROR("memory_embed_onnx: inference failed: %s", s_onnx.ort->GetErrorMessage(status));
      s_onnx.ort->ReleaseStatus(status);
      return FAILURE;
   }

   /* Get output data — shape [1, seq_len, hidden_dim] */
   float *output_data;
   status = s_onnx.ort->GetTensorMutableData(output_tensor, (void **)&output_data);
   if (status != NULL) {
      s_onnx.ort->ReleaseStatus(status);
      s_onnx.ort->ReleaseValue(output_tensor);
      return FAILURE;
   }

   /* Get output shape to determine hidden_dim */
   OrtTensorTypeAndShapeInfo *shape_info;
   OrtStatus *shape_status = s_onnx.ort->GetTensorTypeAndShape(output_tensor, &shape_info);
   if (shape_status) {
      s_onnx.ort->ReleaseStatus(shape_status);
      s_onnx.ort->ReleaseValue(output_tensor);
      return FAILURE;
   }
   size_t num_dims;
   OrtStatus *dim_status = s_onnx.ort->GetDimensionsCount(shape_info, &num_dims);
   if (dim_status)
      s_onnx.ort->ReleaseStatus(dim_status);
   int64_t out_shape[4];
   dim_status = s_onnx.ort->GetDimensions(shape_info, out_shape, num_dims);
   if (dim_status)
      s_onnx.ort->ReleaseStatus(dim_status);
   s_onnx.ort->ReleaseTensorTypeAndShapeInfo(shape_info);

   int hidden_dim = (num_dims >= 3) ? (int)out_shape[2] : ONNX_HIDDEN_DIM;
   if (hidden_dim > max_dims)
      hidden_dim = max_dims;

   /* Mean pooling with attention mask */
   memset(out, 0, (size_t)hidden_dim * sizeof(float));
   float token_sum = 0.0f;

   for (int t = 0; t < seq_len; t++) {
      if (attention_mask[t] == 0)
         continue;
      for (int d = 0; d < hidden_dim; d++) {
         out[d] += output_data[t * hidden_dim + d];
      }
      token_sum += 1.0f;
   }

   if (token_sum > 0.0f) {
      for (int d = 0; d < hidden_dim; d++) {
         out[d] /= token_sum;
      }
   }

   /* L2 normalize */
   float norm = 0.0f;
   for (int d = 0; d < hidden_dim; d++) {
      norm += out[d] * out[d];
   }
   norm = sqrtf(norm);
   if (norm > 0.0f) {
      for (int d = 0; d < hidden_dim; d++) {
         out[d] /= norm;
      }
   }

   *out_dims = hidden_dim;
   s_onnx.ort->ReleaseValue(output_tensor);

   return 0;
}

/* =============================================================================
 * Provider Registration
 * ============================================================================= */

const embedding_provider_t embedding_provider_onnx = {
   .name = "onnx",
   .init = onnx_init,
   .cleanup = onnx_cleanup,
   .embed = onnx_embed,
};
