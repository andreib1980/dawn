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
 * Test: Per-Session LLM Command Context
 *
 * Tests that the command context mechanism (thread-local storage) works correctly
 * for passing session pointers to device callbacks.
 *
 * This is a simplified test that validates the TLS mechanism without needing
 * the full session manager or LLM infrastructure.
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "unity.h"

void setUp(void) {
}
void tearDown(void) {
}

/* =============================================================================
 * Minimal Session Structure for Testing
 * ============================================================================= */

typedef struct test_session {
   uint32_t session_id;
   int type; /* 0=local, 1=websocket */
} test_session_t;

/* Thread-local command context (mirrors session_manager.c implementation) */
static __thread test_session_t *tl_test_context = NULL;

void test_set_context(test_session_t *session) {
   tl_test_context = session;
}

test_session_t *test_get_context(void) {
   return tl_test_context;
}

/* =============================================================================
 * Test: Basic TLS Operations
 * ============================================================================= */
static void test_tls_basics(void) {
   /* Initially NULL */
   TEST_ASSERT_NULL_MESSAGE(test_get_context(), "Initial context is NULL");

   /* Create test sessions */
   test_session_t session1 = { .session_id = 1, .type = 0 };
   test_session_t session2 = { .session_id = 2, .type = 1 };

   /* Set and get */
   test_set_context(&session1);
   TEST_ASSERT_EQUAL_PTR_MESSAGE(&session1, test_get_context(), "Context set to session1");
   TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, test_get_context()->session_id, "Session ID is 1");

   /* Switch context */
   test_set_context(&session2);
   TEST_ASSERT_EQUAL_PTR_MESSAGE(&session2, test_get_context(), "Context switched to session2");
   TEST_ASSERT_EQUAL_UINT32_MESSAGE(2, test_get_context()->session_id, "Session ID is 2");

   /* Clear context */
   test_set_context(NULL);
   TEST_ASSERT_NULL_MESSAGE(test_get_context(), "Context cleared");
}

/* =============================================================================
 * Test: Thread Isolation
 * ============================================================================= */

typedef struct {
   test_session_t *session;
   test_session_t *observed_context;
   int context_was_isolated;
} thread_test_data_t;

static void *thread_test_func(void *arg) {
   thread_test_data_t *data = (thread_test_data_t *)arg;

   /* Check that context starts NULL in new thread */
   data->observed_context = test_get_context();
   data->context_was_isolated = (data->observed_context == NULL);

   /* Set context in this thread */
   test_set_context(data->session);

   /* Small delay to let main thread verify */
   usleep(50000);

   /* Clear before exit */
   test_set_context(NULL);

   return NULL;
}

static void test_thread_isolation(void) {
   test_session_t main_session = { .session_id = 100, .type = 0 };
   test_session_t thread_session = { .session_id = 200, .type = 1 };

   /* Set context in main thread */
   test_set_context(&main_session);
   TEST_ASSERT_EQUAL_PTR_MESSAGE(&main_session, test_get_context(), "Main thread context set");

   /* Spawn worker thread */
   thread_test_data_t thread_data = { .session = &thread_session,
                                      .observed_context = NULL,
                                      .context_was_isolated = 0 };

   pthread_t worker;
   int rc = pthread_create(&worker, NULL, thread_test_func, &thread_data);
   TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc, "Worker thread created");

   /* While worker is running, verify main thread context unchanged */
   usleep(25000);
   TEST_ASSERT_EQUAL_PTR_MESSAGE(&main_session, test_get_context(),
                                 "Main thread context unchanged during worker");

   /* Wait for worker */
   pthread_join(worker, NULL);

   /* Verify thread isolation */
   TEST_ASSERT_TRUE_MESSAGE(thread_data.context_was_isolated,
                            "Worker thread started with NULL context");
   TEST_ASSERT_EQUAL_PTR_MESSAGE(&main_session, test_get_context(),
                                 "Main thread context still intact after worker");

   /* Cleanup */
   test_set_context(NULL);
}

/* =============================================================================
 * Test: Simulated Callback Flow
 * ============================================================================= */

/* Simulated device callback that reads context */
static uint32_t simulated_callback_session_id = 0;

static char *simulated_llm_callback(void) {
   test_session_t *ctx = test_get_context();
   if (ctx) {
      simulated_callback_session_id = ctx->session_id;
      return "Used session config";
   } else {
      simulated_callback_session_id = 0;
      return "Used global config";
   }
}

static void test_callback_flow(void) {
   test_session_t local = { .session_id = 0, .type = 0 };
   test_session_t webui = { .session_id = 5, .type = 1 };

   /* Simulate local voice command flow */
   test_set_context(&local);
   char *result1 = simulated_llm_callback();
   test_set_context(NULL);

   TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, simulated_callback_session_id,
                                    "Callback saw local session (ID 0)");
   TEST_ASSERT_EQUAL_STRING_MESSAGE("Used session config", result1, "Callback used session config");

   /* Simulate WebUI command flow */
   test_set_context(&webui);
   char *result2 = simulated_llm_callback();
   test_set_context(NULL);

   TEST_ASSERT_EQUAL_UINT32_MESSAGE(5, simulated_callback_session_id,
                                    "Callback saw WebUI session (ID 5)");
   TEST_ASSERT_EQUAL_STRING_MESSAGE("Used session config", result2, "Callback used session config");

   /* Simulate no context (fallback to global) */
   char *result3 = simulated_llm_callback();
   TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, simulated_callback_session_id, "Callback got no session");
   TEST_ASSERT_EQUAL_STRING_MESSAGE("Used global config", result3, "Callback fell back to global");
}

/* =============================================================================
 * Test: Context Set/Clear Pairs
 * ============================================================================= */
static void test_context_pairs(void) {
   test_session_t sessions[3] = {
      { .session_id = 10, .type = 0 },
      { .session_id = 20, .type = 1 },
      { .session_id = 30, .type = 1 },
   };

   /* Simulate nested-like operations (not actually nested, but sequential) */
   for (int i = 0; i < 3; i++) {
      test_set_context(&sessions[i]);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(sessions[i].session_id, test_get_context()->session_id,
                                       "Context correct for iteration");
      test_set_context(NULL);
      TEST_ASSERT_NULL_MESSAGE(test_get_context(), "Context cleared after iteration");
   }
}

/* =============================================================================
 * Main
 * ============================================================================= */
int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_tls_basics);
   RUN_TEST(test_thread_isolation);
   RUN_TEST(test_callback_flow);
   RUN_TEST(test_context_pairs);
   return UNITY_END();
}
