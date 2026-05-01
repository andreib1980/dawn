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
 * Unit tests for the embedding recomputation worker (Feature 1).
 *
 * Tests focus on the config-guard early-exit paths and the API contract
 * (in_progress, status, stop-when-not-started) that can be exercised without
 * a live database or embedding engine.
 *
 * DB-dependent paths (model_id mismatch detection, per-user gate, batch loop)
 * require an integration test with a real auth.db — tracked separately.
 */

#include <string.h>

#include "config/dawn_config.h"
#include "dawn_error.h"
#include "memory/memory_embed_recompute.h"
#include "unity.h"

/* g_config is defined in the stub; tests write directly into it. */
extern dawn_config_t g_config;

void setUp(void) {
   /* Reset to a known baseline: engine unavailable → safe no-op for all tests */
   memset(&g_config.memory.model_id, 0, sizeof(g_config.memory.model_id));
   g_config.memory.recompute_on_model_change = true;
   g_config.memory.recompute_batch_size = 50;
   g_config.memory.recompute_batch_sleep_ms = 0;
}

void tearDown(void) {
   /* Ensure no lingering worker thread between tests */
   memory_embed_recompute_stop();
}

/* =============================================================================
 * Test: disabled via config
 *
 * When recompute_on_model_change=false, start() must return SUCCESS immediately
 * without launching a thread.
 * ============================================================================= */

static void test_disabled_via_config(void) {
   g_config.memory.recompute_on_model_change = false;
   strncpy(g_config.memory.model_id, "bge-small-en-v1.5-int8",
           sizeof(g_config.memory.model_id) - 1);

   int rc = memory_embed_recompute_start();

   TEST_ASSERT_EQUAL_INT_MESSAGE(SUCCESS, rc, "disabled config returns SUCCESS");
   TEST_ASSERT_FALSE_MESSAGE(memory_embed_recompute_in_progress(),
                             "no worker launched when disabled");
}

/* =============================================================================
 * Test: embedding engine unavailable
 *
 * When the embedding engine reports unavailable, start() returns SUCCESS
 * without launching a worker.  The stub returns false for available().
 * ============================================================================= */

static void test_engine_unavailable(void) {
   g_config.memory.recompute_on_model_change = true;
   strncpy(g_config.memory.model_id, "bge-small-en-v1.5-int8",
           sizeof(g_config.memory.model_id) - 1);

   /* Stub embedding_engine_available() returns false — no thread starts */
   int rc = memory_embed_recompute_start();

   TEST_ASSERT_EQUAL_INT_MESSAGE(SUCCESS, rc, "unavailable engine returns SUCCESS");
   TEST_ASSERT_FALSE_MESSAGE(memory_embed_recompute_in_progress(),
                             "no worker launched when engine unavailable");
}

/* =============================================================================
 * Test: empty model_id
 *
 * An empty model_id string is not a valid identity; start() must bail out.
 * ============================================================================= */

static void test_empty_model_id(void) {
   g_config.memory.recompute_on_model_change = true;
   g_config.memory.model_id[0] = '\0';

   int rc = memory_embed_recompute_start();

   TEST_ASSERT_EQUAL_INT_MESSAGE(SUCCESS, rc, "empty model_id returns SUCCESS");
   TEST_ASSERT_FALSE_MESSAGE(memory_embed_recompute_in_progress(),
                             "no worker launched for empty model_id");
}

/* =============================================================================
 * Test: in_progress returns false before any start
 * ============================================================================= */

static void test_not_in_progress_initially(void) {
   TEST_ASSERT_FALSE_MESSAGE(memory_embed_recompute_in_progress(),
                             "in_progress is false before start");
}

/* =============================================================================
 * Test: status returns FAILURE when worker not running
 * ============================================================================= */

static void test_status_failure_when_idle(void) {
   int64_t done = -1, total = -1;
   int rc = memory_embed_recompute_status(&done, &total);

   TEST_ASSERT_EQUAL_INT_MESSAGE(FAILURE, rc, "status returns FAILURE when idle");
}

/* =============================================================================
 * Test: stop is safe when no worker was ever started
 * ============================================================================= */

static void test_stop_safe_when_idle(void) {
   /* Must not crash or hang */
   memory_embed_recompute_stop();
   TEST_ASSERT_FALSE(memory_embed_recompute_in_progress());
}

/* =============================================================================
 * Test: status output params accept NULL without crash
 * ============================================================================= */

static void test_status_null_params(void) {
   int rc = memory_embed_recompute_status(NULL, NULL);
   TEST_ASSERT_EQUAL_INT_MESSAGE(FAILURE, rc, "status with NULL params returns FAILURE when idle");
}

/* =============================================================================
 * main
 * ============================================================================= */

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_not_in_progress_initially);
   RUN_TEST(test_status_failure_when_idle);
   RUN_TEST(test_stop_safe_when_idle);
   RUN_TEST(test_status_null_params);
   RUN_TEST(test_disabled_via_config);
   RUN_TEST(test_engine_unavailable);
   RUN_TEST(test_empty_model_id);
   return UNITY_END();
}
