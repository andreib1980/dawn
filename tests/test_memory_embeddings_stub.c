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
 * Stub symbols for test_memory_embeddings. Provides globals and DB/provider
 * symbols so the test can link memory_embeddings.c without the full daemon.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <stddef.h>
#include <stdint.h>

#include "auth/auth_db_internal.h"
#include "config/dawn_config.h"
#include "memory/memory_embeddings.h"

/* Global config stubs */
dawn_config_t g_config;
secrets_config_t g_secrets;

/* Stub auth_db state.  Category backfill paths in memory_embeddings.c reference
 * s_db; we never call the backfill from these tests, but the symbol must link. */
auth_db_state_t s_db = {
   .db = NULL,
   .mutex = PTHREAD_MUTEX_INITIALIZER,
   .initialized = false,
};

/* Provider stubs — test never calls init/embed */
const embedding_provider_t embedding_provider_onnx = { .name = "onnx" };
const embedding_provider_t embedding_provider_ollama = { .name = "ollama" };
const embedding_provider_t embedding_provider_openai = { .name = "openai" };

/* DB stubs — match signatures from memory_db.h exactly */
int memory_db_fact_update_embedding(int user_id,
                                    int64_t fact_id,
                                    const float *embedding,
                                    int dims,
                                    float norm) {
   (void)user_id;
   (void)fact_id;
   (void)embedding;
   (void)dims;
   (void)norm;
   return 0;
}

int memory_db_fact_get_embeddings(int user_id,
                                  int expected_dims,
                                  int64_t *out_ids,
                                  float *out_embeddings,
                                  float *out_norms,
                                  int64_t *out_created_ats,
                                  int max_count,
                                  int *count_out) {
   (void)user_id;
   (void)expected_dims;
   (void)out_ids;
   (void)out_embeddings;
   (void)out_norms;
   (void)out_created_ats;
   (void)max_count;
   if (count_out)
      *count_out = 0;
   return 0;
}

int memory_db_fact_list_without_embedding(int user_id,
                                          int expected_dims,
                                          int64_t *out_ids,
                                          char out_texts[][512],
                                          int max_count,
                                          int *count_out) {
   (void)user_id;
   (void)expected_dims;
   (void)out_ids;
   (void)out_texts;
   (void)max_count;
   if (count_out)
      *count_out = 0;
   return 0;
}

#include "memory/memory_types.h"

int memory_db_entity_get_embeddings(int user_id,
                                    int expected_dims,
                                    int64_t *out_ids,
                                    char out_names[][MEMORY_ENTITY_NAME_MAX],
                                    char out_types[][MEMORY_ENTITY_TYPE_MAX],
                                    float *out_embeddings,
                                    float *out_norms,
                                    int max_count,
                                    int *count_out) {
   (void)user_id;
   (void)expected_dims;
   (void)out_ids;
   (void)out_names;
   (void)out_types;
   (void)out_embeddings;
   (void)out_norms;
   (void)max_count;
   if (count_out)
      *count_out = 0;
   return 0;
}

int memory_db_entity_update_embedding(int64_t entity_id,
                                      int user_id,
                                      const float *embedding,
                                      int dims,
                                      float norm) {
   (void)entity_id;
   (void)user_id;
   (void)embedding;
   (void)dims;
   (void)norm;
   return 0;
}
