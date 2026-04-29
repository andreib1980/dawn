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
 * Unit tests for plan_executor.c — parsing, conditions, interpolation,
 * variable store, safety limits, and tool capability checks.
 *
 * Uses mock tool callbacks registered via tool_registry to test execution
 * without real tool infrastructure. Each test function is self-contained.
 */

#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tools/plan_executor.h"
#include "unity.h"

void setUp(void) {
}
void tearDown(void) {
}

/* ============================================================================
 * 1. Variable Name Validation
 * ============================================================================ */

static void test_variable_name_validation(void) {
   /* Valid names */
   TEST_ASSERT_TRUE(plan_validate_var_name("x"));
   TEST_ASSERT_TRUE(plan_validate_var_name("result"));
   TEST_ASSERT_TRUE(plan_validate_var_name("my_var"));
   TEST_ASSERT_TRUE(plan_validate_var_name("_private"));
   TEST_ASSERT_TRUE(plan_validate_var_name("var123"));
   TEST_ASSERT_TRUE(plan_validate_var_name("a1b2c3"));
   TEST_ASSERT_TRUE(plan_validate_var_name("abcdefghijklmnopqrstuvwxyz012345"));

   /* Invalid names */
   TEST_ASSERT_FALSE(plan_validate_var_name(NULL));
   TEST_ASSERT_FALSE(plan_validate_var_name(""));
   TEST_ASSERT_FALSE(plan_validate_var_name("1start"));
   TEST_ASSERT_FALSE(plan_validate_var_name("CamelCase"));
   TEST_ASSERT_FALSE(plan_validate_var_name("has space"));
   TEST_ASSERT_FALSE(plan_validate_var_name("has-dash"));
   TEST_ASSERT_FALSE(plan_validate_var_name("has.dot"));
   TEST_ASSERT_FALSE(plan_validate_var_name("has$dollar"));
   TEST_ASSERT_FALSE(plan_validate_var_name("has{brace"));
   TEST_ASSERT_FALSE(plan_validate_var_name("has}brace"));
   TEST_ASSERT_FALSE(plan_validate_var_name("abcdefghijklmnopqrstuvwxyz0123456"));
}

/* ============================================================================
 * 2. Variable Store Operations
 * ============================================================================ */

static void test_variable_store(void) {
   plan_context_t ctx = { 0 };

   /* Set and get */
   plan_vars_set(&ctx, "foo", "hello");
   TEST_ASSERT_EQUAL_STRING("hello", plan_vars_get(&ctx, "foo"));
   TEST_ASSERT_EQUAL_INT(1, ctx.var_count);

   /* Overwrite existing */
   plan_vars_set(&ctx, "foo", "world");
   TEST_ASSERT_EQUAL_STRING("world", plan_vars_get(&ctx, "foo"));
   TEST_ASSERT_EQUAL_INT(1, ctx.var_count);

   /* Multiple variables */
   plan_vars_set(&ctx, "bar", "value2");
   plan_vars_set(&ctx, "baz", "value3");
   TEST_ASSERT_EQUAL_INT(3, ctx.var_count);
   TEST_ASSERT_EQUAL_STRING("value2", plan_vars_get(&ctx, "bar"));
   TEST_ASSERT_EQUAL_STRING("value3", plan_vars_get(&ctx, "baz"));

   /* Get nonexistent */
   const char *missing = plan_vars_get(&ctx, "nonexistent");
   TEST_ASSERT_TRUE_MESSAGE(missing == NULL || missing[0] == '\0',
                            "nonexistent returns NULL or empty");

   /* Success flag */
   plan_vars_set_success(&ctx, "foo", true);
   plan_vars_set_success(&ctx, "bar", false);

   /* Cleanup frees all heap values */
   plan_context_cleanup(&ctx);
   TEST_ASSERT_EQUAL_INT(0, ctx.var_count);
}

static void test_variable_store_max_capacity(void) {
   plan_context_t ctx = { 0 };

   /* Fill to PLAN_MAX_VARS */
   for (int i = 0; i < PLAN_MAX_VARS; i++) {
      char name[PLAN_VAR_NAME_MAX + 1];
      snprintf(name, sizeof(name), "var_%02d", i);
      plan_vars_set(&ctx, name, "value");
   }
   TEST_ASSERT_EQUAL_INT(PLAN_MAX_VARS, ctx.var_count);

   /* One more should silently fail */
   plan_vars_set(&ctx, "overflow_var", "should_fail");
   const char *overflow = plan_vars_get(&ctx, "overflow_var");
   TEST_ASSERT_TRUE_MESSAGE(overflow == NULL || overflow[0] == '\0',
                            "variable beyond max capacity not stored");

   plan_context_cleanup(&ctx);
}

/* ============================================================================
 * 3. Condition Evaluation
 * ============================================================================ */

static void test_condition_evaluation(void) {
   plan_context_t ctx = { 0 };
   plan_vars_set(&ctx, "full", "some result text");
   plan_vars_set(&ctx, "empty_str", "");
   plan_vars_set(&ctx, "none_str", "none");
   plan_vars_set(&ctx, "no_results", "no results");
   plan_vars_set(&ctx, "array_empty", "[]");
   plan_vars_set(&ctx, "rain_forecast", "Partly cloudy with rain in the afternoon");
   plan_vars_set(&ctx, "clear_forecast", "Sunny and clear all day");
   plan_vars_set(&ctx, "exact_match", "hello");

   plan_vars_set_success(&ctx, "full", true);
   plan_vars_set_success(&ctx, "empty_str", false);

   /* .empty / .notempty */
   TEST_ASSERT_FALSE(plan_eval_condition(&ctx, "full.empty"));
   TEST_ASSERT_TRUE(plan_eval_condition(&ctx, "full.notempty"));
   TEST_ASSERT_TRUE(plan_eval_condition(&ctx, "empty_str.empty"));
   TEST_ASSERT_FALSE(plan_eval_condition(&ctx, "empty_str.notempty"));
   TEST_ASSERT_TRUE(plan_eval_condition(&ctx, "none_str.empty"));
   TEST_ASSERT_TRUE(plan_eval_condition(&ctx, "no_results.empty"));
   TEST_ASSERT_TRUE(plan_eval_condition(&ctx, "array_empty.empty"));
   TEST_ASSERT_TRUE(plan_eval_condition(&ctx, "undefined_var.empty"));
   TEST_ASSERT_FALSE(plan_eval_condition(&ctx, "undefined_var.notempty"));

   /* .contains */
   TEST_ASSERT_TRUE(plan_eval_condition(&ctx, "rain_forecast.contains:rain"));
   TEST_ASSERT_TRUE(plan_eval_condition(&ctx, "rain_forecast.contains:RAIN"));
   TEST_ASSERT_FALSE(plan_eval_condition(&ctx, "clear_forecast.contains:rain"));
   TEST_ASSERT_TRUE(plan_eval_condition(&ctx, "rain_forecast.contains:afternoon"));

   /* .equals */
   TEST_ASSERT_TRUE(plan_eval_condition(&ctx, "exact_match.equals:hello"));
   TEST_ASSERT_TRUE(plan_eval_condition(&ctx, "exact_match.equals:HELLO"));
   TEST_ASSERT_FALSE(plan_eval_condition(&ctx, "exact_match.equals:world"));

   /* .success / .failed */
   TEST_ASSERT_TRUE(plan_eval_condition(&ctx, "full.success"));
   TEST_ASSERT_FALSE(plan_eval_condition(&ctx, "full.failed"));
   TEST_ASSERT_FALSE(plan_eval_condition(&ctx, "empty_str.success"));
   TEST_ASSERT_TRUE(plan_eval_condition(&ctx, "empty_str.failed"));

   /* Boolean literals */
   TEST_ASSERT_TRUE(plan_eval_condition(&ctx, "true"));
   TEST_ASSERT_FALSE(plan_eval_condition(&ctx, "false"));

   /* Malformed conditions */
   TEST_ASSERT_FALSE(plan_eval_condition(&ctx, ""));
   TEST_ASSERT_FALSE(plan_eval_condition(&ctx, "full.bogus_op"));
   TEST_ASSERT_FALSE(plan_eval_condition(&ctx, "noperiod"));

   plan_context_cleanup(&ctx);
}

/* ============================================================================
 * 4. Variable Interpolation
 * ============================================================================ */

static void test_interpolation(void) {
   plan_context_t ctx = { 0 };
   plan_vars_set(&ctx, "name", "Friday");
   plan_vars_set(&ctx, "room", "kitchen");
   plan_vars_set(&ctx, "count", "42");

   char out[256];
   int rc;

   /* Simple substitution */
   rc = plan_interpolate(&ctx, "Hello $name", out, sizeof(out));
   TEST_ASSERT_EQUAL_INT(0, rc);
   TEST_ASSERT_EQUAL_STRING("Hello Friday", out);

   /* Braced substitution */
   rc = plan_interpolate(&ctx, "${room}_light", out, sizeof(out));
   TEST_ASSERT_EQUAL_INT(0, rc);
   TEST_ASSERT_EQUAL_STRING("kitchen_light", out);

   /* Multiple variables */
   rc = plan_interpolate(&ctx, "$name is in the $room ($count)", out, sizeof(out));
   TEST_ASSERT_EQUAL_INT(0, rc);
   TEST_ASSERT_EQUAL_STRING("Friday is in the kitchen (42)", out);

   /* Undefined variable — expands to empty */
   rc = plan_interpolate(&ctx, "value is $undefined here", out, sizeof(out));
   TEST_ASSERT_EQUAL_INT(0, rc);
   TEST_ASSERT_EQUAL_STRING("value is  here", out);

   /* No variables — passthrough */
   rc = plan_interpolate(&ctx, "no variables here", out, sizeof(out));
   TEST_ASSERT_EQUAL_INT(0, rc);
   TEST_ASSERT_EQUAL_STRING("no variables here", out);

   /* Lone dollar sign (not a variable) */
   rc = plan_interpolate(&ctx, "costs $5 each", out, sizeof(out));
   TEST_ASSERT_EQUAL_INT(0, rc);

   /* Adjacent dollar signs */
   rc = plan_interpolate(&ctx, "$$name", out, sizeof(out));
   TEST_ASSERT_EQUAL_INT(0, rc);

   /* Empty braces */
   rc = plan_interpolate(&ctx, "x ${}y", out, sizeof(out));
   TEST_ASSERT_EQUAL_INT(0, rc);

   plan_context_cleanup(&ctx);
}

static void test_interpolation_size_limit(void) {
   plan_context_t ctx = { 0 };

   /* Set a long variable value */
   char long_value[512];
   memset(long_value, 'A', sizeof(long_value) - 1);
   long_value[sizeof(long_value) - 1] = '\0';
   plan_vars_set(&ctx, "big", long_value);

   /* Interpolate into a small buffer — should truncate, not overflow */
   char small_out[32];
   int rc = plan_interpolate(&ctx, "prefix $big suffix", small_out, sizeof(small_out));
   TEST_ASSERT_EQUAL_INT(0, rc);
   TEST_ASSERT_TRUE_MESSAGE(strlen(small_out) < sizeof(small_out), "output fits in buffer");

   plan_context_cleanup(&ctx);
}

/* ============================================================================
 * 5. Plan Parsing
 * ============================================================================ */

static void test_parse_valid_plan(void) {
   const char *json = "[{\"type\": \"log\", \"message\": \"hello\"}]";
   struct json_object *plan = NULL;
   int rc = plan_parse(json, &plan);
   TEST_ASSERT_EQUAL_INT(0, rc);
   TEST_ASSERT_NOT_NULL(plan);
   TEST_ASSERT_EQUAL_INT(1, (int)json_object_array_length(plan));
   json_object_put(plan);
}

static void test_parse_multi_step_plan(void) {
   const char *json = "["
                      "  {\"type\": \"set\", \"var\": \"x\", \"value\": \"hello\"},"
                      "  {\"type\": \"log\", \"message\": \"$x world\"},"
                      "  {\"type\": \"if\", \"condition\": \"x.notempty\", \"then\": ["
                      "    {\"type\": \"log\", \"message\": \"x is set\"}"
                      "  ]}"
                      "]";
   struct json_object *plan = NULL;
   int rc = plan_parse(json, &plan);
   TEST_ASSERT_EQUAL_INT(0, rc);
   TEST_ASSERT_EQUAL_INT(3, (int)json_object_array_length(plan));
   json_object_put(plan);
}

static void test_parse_invalid_json(void) {
   struct json_object *plan = NULL;

   /* Not JSON at all */
   TEST_ASSERT_NOT_EQUAL(0, plan_parse("this is not json", &plan));
   TEST_ASSERT_NULL(plan);

   /* JSON but not an array */
   TEST_ASSERT_NOT_EQUAL(0, plan_parse("{\"type\": \"log\"}", &plan));

   /* Empty array */
   int rc = plan_parse("[]", &plan);
   if (rc == 0) {
      TEST_ASSERT_EQUAL_INT(0, (int)json_object_array_length(plan));
      json_object_put(plan);
   }
}

static void test_parse_null_input(void) {
   struct json_object *plan = NULL;
   TEST_ASSERT_NOT_EQUAL(0, plan_parse(NULL, &plan));
   TEST_ASSERT_NOT_EQUAL(0, plan_parse("", &plan));
}

static void test_parse_oversized_plan(void) {
   /* Build a JSON string larger than PLAN_MAX_PLAN_SIZE (16384) */
   size_t big_size = PLAN_MAX_PLAN_SIZE + 1024;
   char *big_json = malloc(big_size);
   TEST_ASSERT_NOT_NULL(big_json);
   if (!big_json)
      return;

   /* Fill with valid-ish JSON: [{"type":"log","message":"AAAA..."}] */
   strcpy(big_json, "[{\"type\":\"log\",\"message\":\"");
   size_t prefix_len = strlen(big_json);
   memset(big_json + prefix_len, 'A', big_size - prefix_len - 4);
   strcpy(big_json + big_size - 4, "\"}]");

   struct json_object *plan = NULL;
   TEST_ASSERT_NOT_EQUAL(0, plan_parse(big_json, &plan));
   free(big_json);
}

static void test_parse_invalid_step_type(void) {
   const char *json = "[{\"type\": \"execute_shell\", \"cmd\": \"rm -rf /\"}]";
   struct json_object *plan = NULL;
   int rc = plan_parse(json, &plan);
   if (rc == 0) {
      TEST_ASSERT_NOT_NULL(plan);
      json_object_put(plan);
   }
}

static void test_parse_invalid_variable_names(void) {
   /* 'store' with invalid name in call step */
   const char *json1 = "[{\"type\": \"call\", \"tool\": \"weather\", "
                       "\"args\": {\"action\": \"forecast\"}, \"store\": \"BAD_NAME\"}]";
   struct json_object *plan = NULL;
   int rc = plan_parse(json1, &plan);
   TEST_ASSERT_NOT_EQUAL(0, rc);
   if (plan)
      json_object_put(plan);

   /* 'var' with injection attempt in set step */
   const char *json2 = "[{\"type\": \"set\", \"var\": \"$inject\", \"value\": \"bad\"}]";
   plan = NULL;
   rc = plan_parse(json2, &plan);
   TEST_ASSERT_NOT_EQUAL(0, rc);
   if (plan)
      json_object_put(plan);

   /* 'as' with dot in loop step */
   const char *json3 = "[{\"type\": \"loop\", \"over\": [\"a\"], \"as\": \"item.name\", "
                       "\"steps\": [{\"type\": \"log\", \"message\": \"hi\"}]}]";
   plan = NULL;
   rc = plan_parse(json3, &plan);
   TEST_ASSERT_NOT_EQUAL(0, rc);
   if (plan)
      json_object_put(plan);
}

/* ============================================================================
 * 6. Plan Execution — Pure Logic (set, log, if)
 * ============================================================================ */

static void test_execute_set_and_log(void) {
   const char *json = "["
                      "  {\"type\": \"set\", \"var\": \"greeting\", \"value\": \"Hello world\"},"
                      "  {\"type\": \"log\", \"message\": \"$greeting\"}"
                      "]";

   struct json_object *plan = NULL;
   int rc = plan_parse(json, &plan);
   TEST_ASSERT_EQUAL_INT(0, rc);

   plan_context_t ctx = { 0 };
   ctx.timeout_s = 5;
   clock_gettime(CLOCK_MONOTONIC, &ctx.start_time);

   rc = plan_execute_steps(&ctx, plan);
   TEST_ASSERT_EQUAL_INT(0, rc);
   TEST_ASSERT_NOT_NULL(strstr(ctx.output, "Hello world"));
   TEST_ASSERT_EQUAL_INT(2, ctx.total_steps_executed);

   plan_context_cleanup(&ctx);
   json_object_put(plan);
}

static void test_execute_if_then(void) {
   const char *json = "["
                      "  {\"type\": \"set\", \"var\": \"data\", \"value\": \"something\"},"
                      "  {\"type\": \"if\", \"condition\": \"data.notempty\", \"then\": ["
                      "    {\"type\": \"log\", \"message\": \"then branch\"}"
                      "  ], \"else\": ["
                      "    {\"type\": \"log\", \"message\": \"else branch\"}"
                      "  ]}"
                      "]";

   struct json_object *plan = NULL;
   int rc = plan_parse(json, &plan);
   TEST_ASSERT_EQUAL_INT(0, rc);

   plan_context_t ctx = { 0 };
   ctx.timeout_s = 5;
   clock_gettime(CLOCK_MONOTONIC, &ctx.start_time);

   rc = plan_execute_steps(&ctx, plan);
   TEST_ASSERT_EQUAL_INT(0, rc);
   TEST_ASSERT_NOT_NULL(strstr(ctx.output, "then branch"));
   TEST_ASSERT_NULL(strstr(ctx.output, "else branch"));

   plan_context_cleanup(&ctx);
   json_object_put(plan);
}

static void test_execute_if_else(void) {
   const char *json = "["
                      "  {\"type\": \"if\", \"condition\": \"undefined_var.notempty\", \"then\": ["
                      "    {\"type\": \"log\", \"message\": \"then branch\"}"
                      "  ], \"else\": ["
                      "    {\"type\": \"log\", \"message\": \"else branch\"}"
                      "  ]}"
                      "]";

   struct json_object *plan = NULL;
   int rc = plan_parse(json, &plan);
   TEST_ASSERT_EQUAL_INT(0, rc);

   plan_context_t ctx = { 0 };
   ctx.timeout_s = 5;
   clock_gettime(CLOCK_MONOTONIC, &ctx.start_time);

   rc = plan_execute_steps(&ctx, plan);
   TEST_ASSERT_EQUAL_INT(0, rc);
   TEST_ASSERT_NOT_NULL(strstr(ctx.output, "else branch"));
   TEST_ASSERT_NULL(strstr(ctx.output, "then branch"));

   plan_context_cleanup(&ctx);
   json_object_put(plan);
}

static void test_execute_loop(void) {
   const char *json = "["
                      "  {\"type\": \"loop\", \"over\": [\"kitchen\", \"bedroom\", \"office\"], "
                      "   \"as\": \"room\", \"steps\": ["
                      "    {\"type\": \"log\", \"message\": \"Processing $room\"}"
                      "  ]}"
                      "]";

   struct json_object *plan = NULL;
   int rc = plan_parse(json, &plan);
   TEST_ASSERT_EQUAL_INT(0, rc);

   plan_context_t ctx = { 0 };
   ctx.timeout_s = 5;
   clock_gettime(CLOCK_MONOTONIC, &ctx.start_time);

   rc = plan_execute_steps(&ctx, plan);
   TEST_ASSERT_EQUAL_INT(0, rc);
   TEST_ASSERT_NOT_NULL(strstr(ctx.output, "Processing kitchen"));
   TEST_ASSERT_NOT_NULL(strstr(ctx.output, "Processing bedroom"));
   TEST_ASSERT_NOT_NULL(strstr(ctx.output, "Processing office"));
   TEST_ASSERT_EQUAL_INT(4, ctx.total_steps_executed);

   plan_context_cleanup(&ctx);
   json_object_put(plan);
}

static void test_execute_nested_if_in_loop(void) {
   const char *json = "["
                      "  {\"type\": \"set\", \"var\": \"target\", \"value\": \"bedroom\"},"
                      "  {\"type\": \"loop\", \"over\": [\"kitchen\", \"bedroom\", \"office\"], "
                      "   \"as\": \"room\", \"steps\": ["
                      "    {\"type\": \"if\", \"condition\": \"room.equals:bedroom\", \"then\": ["
                      "      {\"type\": \"log\", \"message\": \"Found target: $room\"}"
                      "    ]}"
                      "  ]}"
                      "]";

   struct json_object *plan = NULL;
   int rc = plan_parse(json, &plan);
   TEST_ASSERT_EQUAL_INT(0, rc);

   plan_context_t ctx = { 0 };
   ctx.timeout_s = 5;
   clock_gettime(CLOCK_MONOTONIC, &ctx.start_time);

   rc = plan_execute_steps(&ctx, plan);
   TEST_ASSERT_EQUAL_INT(0, rc);
   TEST_ASSERT_NOT_NULL(strstr(ctx.output, "Found target: bedroom"));

   plan_context_cleanup(&ctx);
   json_object_put(plan);
}

/* ============================================================================
 * 7. Safety Limits
 * ============================================================================ */

static void test_max_steps_exceeded(void) {
   /* Build a plan with PLAN_MAX_STEPS + 5 log steps using json-c */
   struct json_object *plan = json_object_new_array();
   for (int i = 0; i < PLAN_MAX_STEPS + 5; i++) {
      struct json_object *step = json_object_new_object();
      json_object_object_add(step, "type", json_object_new_string("log"));
      json_object_object_add(step, "message", json_object_new_string("step"));
      json_object_array_add(plan, step);
   }

   plan_context_t ctx = { 0 };
   ctx.timeout_s = 30;
   clock_gettime(CLOCK_MONOTONIC, &ctx.start_time);

   int rc = plan_execute_steps(&ctx, plan);
   TEST_ASSERT_NOT_EQUAL(0, rc);
   TEST_ASSERT_TRUE_MESSAGE(ctx.total_steps_executed <= PLAN_MAX_STEPS,
                            "execution stopped at or before max steps");

   plan_context_cleanup(&ctx);
   json_object_put(plan);
}

static void test_max_depth_exceeded(void) {
   /* Build deeply nested if statements exceeding PLAN_MAX_DEPTH (5) */
   const char *json = "[{\"type\": \"if\", \"condition\": \"true\", \"then\": ["
                      "  {\"type\": \"if\", \"condition\": \"true\", \"then\": ["
                      "    {\"type\": \"if\", \"condition\": \"true\", \"then\": ["
                      "      {\"type\": \"if\", \"condition\": \"true\", \"then\": ["
                      "        {\"type\": \"if\", \"condition\": \"true\", \"then\": ["
                      "          {\"type\": \"if\", \"condition\": \"true\", \"then\": ["
                      "            {\"type\": \"log\", \"message\": \"too deep\"}"
                      "          ]}"
                      "        ]}"
                      "      ]}"
                      "    ]}"
                      "  ]}"
                      "]}]";

   struct json_object *plan = NULL;
   int rc = plan_parse(json, &plan);
   TEST_ASSERT_EQUAL_INT(0, rc);

   plan_context_t ctx = { 0 };
   ctx.timeout_s = 5;
   clock_gettime(CLOCK_MONOTONIC, &ctx.start_time);

   rc = plan_execute_steps(&ctx, plan);
   TEST_ASSERT_NOT_EQUAL(0, rc);

   plan_context_cleanup(&ctx);
   json_object_put(plan);
}

static void test_loop_iteration_cap(void) {
   /* Build a loop with more than PLAN_MAX_LOOP_ITERATIONS (10) items */
   struct json_object *plan = json_object_new_array();
   struct json_object *loop_step = json_object_new_object();
   json_object_object_add(loop_step, "type", json_object_new_string("loop"));
   json_object_object_add(loop_step, "as", json_object_new_string("item"));

   struct json_object *over = json_object_new_array();
   for (int i = 0; i < 20; i++) {
      char val[8];
      snprintf(val, sizeof(val), "item%d", i);
      json_object_array_add(over, json_object_new_string(val));
   }
   json_object_object_add(loop_step, "over", over);

   struct json_object *steps = json_object_new_array();
   struct json_object *log_step = json_object_new_object();
   json_object_object_add(log_step, "type", json_object_new_string("log"));
   json_object_object_add(log_step, "message", json_object_new_string("$item"));
   json_object_array_add(steps, log_step);
   json_object_object_add(loop_step, "steps", steps);

   json_object_array_add(plan, loop_step);

   plan_context_t ctx = { 0 };
   ctx.timeout_s = 5;
   clock_gettime(CLOCK_MONOTONIC, &ctx.start_time);

   int rc = plan_execute_steps(&ctx, plan);
   TEST_ASSERT_TRUE_MESSAGE(ctx.total_steps_executed <= PLAN_MAX_LOOP_ITERATIONS + 1,
                            "loop iterations capped");
   (void)rc;

   plan_context_cleanup(&ctx);
   json_object_put(plan);
}

static void test_output_buffer_overflow(void) {
   /* Generate many log messages that would overflow the output buffer */
   struct json_object *plan = json_object_new_array();
   char long_msg[256];
   memset(long_msg, 'X', sizeof(long_msg) - 1);
   long_msg[sizeof(long_msg) - 1] = '\0';

   for (int i = 0; i < 50; i++) {
      struct json_object *step = json_object_new_object();
      json_object_object_add(step, "type", json_object_new_string("log"));
      json_object_object_add(step, "message", json_object_new_string(long_msg));
      json_object_array_add(plan, step);
   }

   plan_context_t ctx = { 0 };
   ctx.timeout_s = 5;
   clock_gettime(CLOCK_MONOTONIC, &ctx.start_time);

   int rc = plan_execute_steps(&ctx, plan);
   TEST_ASSERT_EQUAL_INT(0, rc);
   TEST_ASSERT_TRUE_MESSAGE(ctx.output_len <= (int)sizeof(ctx.output),
                            "output stays within buffer");
   TEST_ASSERT_TRUE_MESSAGE(strstr(ctx.output, "[truncated]") != NULL || ctx.output_len > 0,
                            "output truncated or present");

   plan_context_cleanup(&ctx);
   json_object_put(plan);
}

/* ============================================================================
 * 8. Tool Capability Checks
 * ============================================================================ */

static void test_tool_capability_whitelist(void) {
   tool_metadata_t allowed_tool = { 0 };
   allowed_tool.name = "weather";
   allowed_tool.capabilities = TOOL_CAP_NETWORK | TOOL_CAP_SCHEDULABLE;

   tool_metadata_t dangerous_tool = { 0 };
   dangerous_tool.name = "shutdown";
   dangerous_tool.capabilities = TOOL_CAP_DANGEROUS | TOOL_CAP_SCHEDULABLE;

   tool_metadata_t no_schedule_tool = { 0 };
   no_schedule_tool.name = "custom_tool";
   no_schedule_tool.capabilities = TOOL_CAP_NETWORK;

   tool_metadata_t self_ref = { 0 };
   self_ref.name = "execute_plan";
   self_ref.capabilities = TOOL_CAP_SCHEDULABLE;

   TEST_ASSERT_TRUE(plan_tool_is_allowed(&allowed_tool));
   TEST_ASSERT_FALSE(plan_tool_is_allowed(&dangerous_tool));
   TEST_ASSERT_FALSE(plan_tool_is_allowed(&no_schedule_tool));
   TEST_ASSERT_FALSE(plan_tool_is_allowed(&self_ref));
}

/* ============================================================================
 * 9. JSON String vs Object Plan Parameter
 * ============================================================================ */

static void test_parse_json_string_form(void) {
   const char *direct = "[{\"type\": \"log\", \"message\": \"hello\"}]";
   struct json_object *plan = NULL;
   int rc = plan_parse(direct, &plan);
   TEST_ASSERT_EQUAL_INT(0, rc);
   if (plan)
      json_object_put(plan);
}

/* ============================================================================
 * 10. Edge Cases
 * ============================================================================ */

static void test_empty_loop(void) {
   const char *json = "[{\"type\": \"loop\", \"over\": [], \"as\": \"item\", \"steps\": ["
                      "  {\"type\": \"log\", \"message\": \"should not appear\"}"
                      "]}]";

   struct json_object *plan = NULL;
   int rc = plan_parse(json, &plan);
   TEST_ASSERT_EQUAL_INT(0, rc);

   plan_context_t ctx = { 0 };
   ctx.timeout_s = 5;
   clock_gettime(CLOCK_MONOTONIC, &ctx.start_time);

   rc = plan_execute_steps(&ctx, plan);
   TEST_ASSERT_EQUAL_INT(0, rc);
   TEST_ASSERT_TRUE_MESSAGE(ctx.output[0] == '\0' ||
                                strstr(ctx.output, "should not appear") == NULL,
                            "empty loop body never executed");

   plan_context_cleanup(&ctx);
   json_object_put(plan);
}

static void test_if_without_else(void) {
   const char *json = "["
                      "  {\"type\": \"if\", \"condition\": \"false\", \"then\": ["
                      "    {\"type\": \"log\", \"message\": \"should not appear\"}"
                      "  ]}"
                      "]";

   struct json_object *plan = NULL;
   int rc = plan_parse(json, &plan);
   TEST_ASSERT_EQUAL_INT(0, rc);

   plan_context_t ctx = { 0 };
   ctx.timeout_s = 5;
   clock_gettime(CLOCK_MONOTONIC, &ctx.start_time);

   rc = plan_execute_steps(&ctx, plan);
   TEST_ASSERT_EQUAL_INT(0, rc);
   TEST_ASSERT_TRUE_MESSAGE(ctx.output[0] == '\0' ||
                                strstr(ctx.output, "should not appear") == NULL,
                            "then branch not executed");

   plan_context_cleanup(&ctx);
   json_object_put(plan);
}

static void test_set_overwrite_in_loop(void) {
   const char *json = "["
                      "  {\"type\": \"loop\", \"over\": [\"first\", \"second\", \"third\"], "
                      "   \"as\": \"item\", \"steps\": ["
                      "    {\"type\": \"set\", \"var\": \"last_seen\", \"value\": \"$item\"}"
                      "  ]},"
                      "  {\"type\": \"log\", \"message\": \"last=$last_seen\"}"
                      "]";

   struct json_object *plan = NULL;
   int rc = plan_parse(json, &plan);
   TEST_ASSERT_EQUAL_INT(0, rc);

   plan_context_t ctx = { 0 };
   ctx.timeout_s = 5;
   clock_gettime(CLOCK_MONOTONIC, &ctx.start_time);

   rc = plan_execute_steps(&ctx, plan);
   TEST_ASSERT_EQUAL_INT(0, rc);
   TEST_ASSERT_NOT_NULL(strstr(ctx.output, "last=third"));

   plan_context_cleanup(&ctx);
   json_object_put(plan);
}

/* ============================================================================
 * 11. Call Step Argument Construction
 * ============================================================================ */

static void test_arg_interpolation_safe(void) {
   plan_context_t ctx = { 0 };
   ctx.timeout_s = 5;
   clock_gettime(CLOCK_MONOTONIC, &ctx.start_time);

   /* Set a variable containing JSON-hostile characters */
   plan_vars_set(&ctx, "user_input", "value with \"quotes\" and \\backslash");

   /* Build args template */
   struct json_object *args = json_object_new_object();
   json_object_object_add(args, "query", json_object_new_string("$user_input"));
   json_object_object_add(args, "action", json_object_new_string("search"));

   char out_json[1024];
   int rc = plan_build_args_json(&ctx, args, out_json, sizeof(out_json));
   TEST_ASSERT_EQUAL_INT(0, rc);

   /* The output should be valid JSON */
   struct json_object *parsed = json_tokener_parse(out_json);
   TEST_ASSERT_NOT_NULL(parsed);

   /* The interpolated value should preserve the original string content */
   if (parsed) {
      struct json_object *query_obj;
      bool has_query = json_object_object_get_ex(parsed, "query", &query_obj);
      TEST_ASSERT_TRUE_MESSAGE(has_query && json_object_is_type(query_obj, json_type_string),
                               "query field exists");
      if (has_query) {
         const char *query_str = json_object_get_string(query_obj);
         TEST_ASSERT_NOT_NULL(strstr(query_str, "quotes"));
      }
      json_object_put(parsed);
   }

   json_object_put(args);
   plan_context_cleanup(&ctx);
}

static void test_arg_delimiter_injection(void) {
   plan_context_t ctx = { 0 };
   ctx.timeout_s = 5;
   clock_gettime(CLOCK_MONOTONIC, &ctx.start_time);

   /* Variable containing :: delimiter pattern used by llm_tools */
   plan_vars_set(&ctx, "evil", "value::action::delete_all");

   struct json_object *args = json_object_new_object();
   json_object_object_add(args, "query", json_object_new_string("$evil"));

   char out_json[1024];
   int rc = plan_build_args_json(&ctx, args, out_json, sizeof(out_json));
   TEST_ASSERT_EQUAL_INT(0, rc);

   /* Parse and verify */
   struct json_object *parsed = json_tokener_parse(out_json);
   TEST_ASSERT_NOT_NULL(parsed);
   if (parsed)
      json_object_put(parsed);

   json_object_put(args);
   plan_context_cleanup(&ctx);
}

static void test_interpolation_dot_access(void) {
   plan_context_t ctx = { 0 };
   ctx.timeout_s = 5;
   clock_gettime(CLOCK_MONOTONIC, &ctx.start_time);

   /* Simulate a loop variable set to a serialized JSON object */
   plan_vars_set(&ctx, "step", "{\"color\":\"red\",\"room\":\"bedroom\"}");

   char out[256];

   /* $step.color should extract "red" */
   plan_interpolate(&ctx, "$step.color", out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("red", out);

   /* $step.room should extract "bedroom" */
   plan_interpolate(&ctx, "$step.room", out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("bedroom", out);

   /* $step without dot should output the raw JSON */
   plan_interpolate(&ctx, "$step", out, sizeof(out));
   TEST_ASSERT_NOT_NULL(strstr(out, "color"));

   /* $step.missing should output nothing (field doesn't exist) */
   plan_interpolate(&ctx, "$step.missing", out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("", out);

   /* Non-JSON variable with dot access falls through to raw value */
   plan_vars_set(&ctx, "plain", "hello");
   plan_interpolate(&ctx, "$plain.field", out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("hello", out);

   /* Dot access in a larger template */
   plan_interpolate(&ctx, "Set $step.room to $step.color", out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("Set bedroom to red", out);

   plan_context_cleanup(&ctx);
}

static void test_loop_with_object_items(void) {
   plan_context_t ctx = { 0 };
   ctx.timeout_s = 5;
   clock_gettime(CLOCK_MONOTONIC, &ctx.start_time);

   const char *plan_json =
       "[{\"type\":\"loop\",\"over\":[{\"color\":\"red\"},{\"color\":\"blue\"}],"
       "\"as\":\"item\",\"steps\":[{\"type\":\"log\",\"message\":\"$item.color\"}]}]";

   struct json_object *plan = NULL;
   int rc = plan_parse(plan_json, &plan);
   TEST_ASSERT_EQUAL_INT(PLAN_OK, rc);

   rc = plan_execute_steps(&ctx, plan);
   TEST_ASSERT_EQUAL_INT(PLAN_OK, rc);

   TEST_ASSERT_NOT_NULL(strstr(ctx.output, "red"));
   TEST_ASSERT_NOT_NULL(strstr(ctx.output, "blue"));

   json_object_put(plan);
   plan_context_cleanup(&ctx);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
   UNITY_BEGIN();

   RUN_TEST(test_variable_name_validation);
   RUN_TEST(test_variable_store);
   RUN_TEST(test_variable_store_max_capacity);
   RUN_TEST(test_condition_evaluation);
   RUN_TEST(test_interpolation);
   RUN_TEST(test_interpolation_size_limit);
   RUN_TEST(test_parse_valid_plan);
   RUN_TEST(test_parse_multi_step_plan);
   RUN_TEST(test_parse_invalid_json);
   RUN_TEST(test_parse_null_input);
   RUN_TEST(test_parse_oversized_plan);
   RUN_TEST(test_parse_invalid_step_type);
   RUN_TEST(test_parse_invalid_variable_names);
   RUN_TEST(test_execute_set_and_log);
   RUN_TEST(test_execute_if_then);
   RUN_TEST(test_execute_if_else);
   RUN_TEST(test_execute_loop);
   RUN_TEST(test_execute_nested_if_in_loop);
   RUN_TEST(test_max_steps_exceeded);
   RUN_TEST(test_max_depth_exceeded);
   RUN_TEST(test_loop_iteration_cap);
   RUN_TEST(test_output_buffer_overflow);
   RUN_TEST(test_tool_capability_whitelist);
   RUN_TEST(test_parse_json_string_form);
   RUN_TEST(test_empty_loop);
   RUN_TEST(test_if_without_else);
   RUN_TEST(test_set_overwrite_in_loop);
   RUN_TEST(test_arg_interpolation_safe);
   RUN_TEST(test_arg_delimiter_injection);
   RUN_TEST(test_interpolation_dot_access);
   RUN_TEST(test_loop_with_object_items);

   return UNITY_END();
}
