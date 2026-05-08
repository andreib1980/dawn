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
 * Stubs for test_memory_db_admin: provides s_db, g_config, and the
 * extraction-template-size helper so the test can link memory_db_admin.c
 * standalone without pulling in memory_extraction.c (which drags
 * llm_interface, json-c, embeddings, etc. into the test binary).
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "config/dawn_config.h"

auth_db_state_t s_db = {
   .db = NULL,
   .mutex = PTHREAD_MUTEX_INITIALIZER,
   .initialized = false,
};

dawn_config_t g_config;

/* memory_extraction stub — return a representative template size so the
 * cost estimator's real-prompt-size invariant is exercised without
 * pulling memory_extraction.c into the test link. */
size_t memory_extraction_get_template_size_chars(void) {
   return 4744; /* matches actual EXTRACTION_PROMPT_TEMPLATE size as of v42 */
}

/* memory_recovery stub — the test never spawns the worker thread, so
 * memory_recovery_run_pass is reachable only via that path; provide a
 * no-op. */
void memory_recovery_run_pass(int *triggered_out, int *skipped_out) {
   if (triggered_out)
      *triggered_out = 0;
   if (skipped_out)
      *skipped_out = 0;
}

/* memory_embeddings_invalidate_all is called after a successful reset
 * transaction — the test doesn't exercise the embedding cache. */
void memory_embeddings_invalidate_all(void) {
}
