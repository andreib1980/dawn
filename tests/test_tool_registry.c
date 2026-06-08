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
 * Unit tests for tool_registry.c — registration, lookup, capability checks,
 * locking, alias resolution, iteration, and variation counting.
 *
 * Each test gets a fresh registry via setUp/tearDown calling init/shutdown.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tools/tool_registry.h"
#include "unity.h"

/* ============================================================================
 * Mock callback and tool metadata
 * ============================================================================ */

static char *mock_callback(const char *action, char *value, int *should_respond) {
   (void)action;
   (void)value;
   *should_respond = 1;
   return strdup("ok");
}

static char *mock_callback_b(const char *action, char *value, int *should_respond) {
   (void)action;
   (void)value;
   *should_respond = 0;
   return NULL;
}

static const treg_param_t mock_params[] = {
   {
       .name = "action",
       .description = "Action to perform",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = true,
       .maps_to = TOOL_MAPS_TO_ACTION,
       .enum_values = { "play", "pause", "stop" },
       .enum_count = 3,
   },
};

static const tool_metadata_t mock_tool = {
   .name = "test_tool",
   .device_string = "test device",
   .description = "A test tool for unit tests",
   .callback = mock_callback,
   .device_type = TOOL_DEVICE_TYPE_TRIGGER,
   .capabilities = TOOL_CAP_NONE,
   .params = mock_params,
   .param_count = 1,
   .default_local = true,
   .default_remote = true,
};

static const tool_metadata_t mock_tool_network = {
   .name = "net_tool",
   .device_string = "net device",
   .description = "A tool with network capability",
   .callback = mock_callback_b,
   .device_type = TOOL_DEVICE_TYPE_TRIGGER,
   .capabilities = TOOL_CAP_NETWORK,
   .params = NULL,
   .param_count = 0,
   .default_local = true,
   .default_remote = false,
};

static const tool_metadata_t mock_tool_aliased = {
   .name = "alias_tool",
   .device_string = "alias device",
   .description = "A tool with aliases",
   .callback = mock_callback,
   .device_type = TOOL_DEVICE_TYPE_TRIGGER,
   .capabilities = TOOL_CAP_NONE,
   .aliases = { "alias_one", "alias_two" },
   .alias_count = 2,
   .params = NULL,
   .param_count = 0,
   .default_local = true,
   .default_remote = true,
};

/* Schedulable mock without TOOL_CAP_REQUIRES_VALUE — empty/missing tool_value
 * must pass.  Mirrors the shape of e.g. `weather` whose tool_value can be
 * legitimately empty when the user has a configured default location. */
static const tool_metadata_t mock_schedulable_no_value = {
   .name = "sched_tool",
   .device_string = "sched device",
   .description = "Schedulable tool with no value requirement",
   .callback = mock_callback,
   .device_type = TOOL_DEVICE_TYPE_TRIGGER,
   .capabilities = TOOL_CAP_SCHEDULABLE,
   .params = NULL,
   .param_count = 0,
   .default_local = true,
   .default_remote = true,
};

/* Schedulable mock that REQUIRES a non-empty tool_value.  Mirrors `search` /
 * `url_fetch` — the briefing fire path can't proceed without a query. */
static const tool_metadata_t mock_schedulable_with_value = {
   .name = "sched_val_tool",
   .device_string = "sched val device",
   .description = "Schedulable tool that requires a value",
   .callback = mock_callback,
   .device_type = TOOL_DEVICE_TYPE_TRIGGER,
   .capabilities = TOOL_CAP_SCHEDULABLE | TOOL_CAP_REQUIRES_VALUE,
   .params = NULL,
   .param_count = 0,
   .default_local = true,
   .default_remote = true,
};


/* Tool with an ARRAY param (declared last, per the ARRAY terminal-slot
 * contract) — exercises array schema emission. */
static const treg_param_t mock_array_params[] = {
   {
       .name = "action",
       .description = "Action to perform",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = true,
       .maps_to = TOOL_MAPS_TO_ACTION,
       .enum_values = { "enqueue", "search" },
       .enum_count = 2,
   },
   {
       .name = "items",
       .description = "List of items",
       .type = TOOL_PARAM_TYPE_ARRAY,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "items",
   },
};

static const tool_metadata_t mock_tool_array = {
   .name = "array_tool",
   .device_string = "array device",
   .description = "A tool with a string-array param",
   .callback = mock_callback,
   .device_type = TOOL_DEVICE_TYPE_TRIGGER,
   .capabilities = TOOL_CAP_NONE,
   .params = mock_array_params,
   .param_count = 2,
   .default_local = true,
   .default_remote = true,
};

/* ARRAY param NOT in the last slot — must be rejected at registration time
 * (terminal-slot contract). */
static const treg_param_t mock_array_params_bad[] = {
   {
       .name = "items",
       .description = "List of items (illegally not last)",
       .type = TOOL_PARAM_TYPE_ARRAY,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "items",
   },
   {
       .name = "action",
       .description = "Action",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = true,
       .maps_to = TOOL_MAPS_TO_ACTION,
       .enum_values = { "go" },
       .enum_count = 1,
   },
};

static const tool_metadata_t mock_tool_array_bad = {
   .name = "array_bad_tool",
   .device_string = "array bad device",
   .description = "ARRAY param not last — should fail registration",
   .callback = mock_callback,
   .device_type = TOOL_DEVICE_TYPE_TRIGGER,
   .capabilities = TOOL_CAP_NONE,
   .params = mock_array_params_bad,
   .param_count = 2,
   .default_local = true,
   .default_remote = true,
};

/* ============================================================================
 * setUp / tearDown — fresh registry for each test
 * ============================================================================ */

void setUp(void) {
   tool_registry_init();
}

void tearDown(void) {
   tool_registry_shutdown();
}

/* ============================================================================
 * Tests
 * ============================================================================ */

static void test_init_returns_success(void) {
   /* setUp already called init, verify available flag */
   TEST_ASSERT_TRUE_MESSAGE(tool_registry_is_available(), "registry available after init");
}

static void test_register_returns_success(void) {
   int ret = tool_registry_register(&mock_tool);
   TEST_ASSERT_EQUAL_INT_MESSAGE(0, ret, "register returns 0 on success");
}

static void test_lookup_by_name(void) {
   tool_registry_register(&mock_tool);

   const tool_metadata_t *found = tool_registry_lookup("test_tool");
   TEST_ASSERT_NOT_NULL_MESSAGE(found, "lookup returns non-NULL for registered tool");
   TEST_ASSERT_EQUAL_STRING_MESSAGE("test_tool", found->name, "name matches");
   TEST_ASSERT_EQUAL_STRING_MESSAGE("A test tool for unit tests", found->description,
                                    "description matches");
}

static void test_lookup_not_found(void) {
   const tool_metadata_t *found = tool_registry_lookup("nonexistent");
   TEST_ASSERT_NULL_MESSAGE(found, "lookup returns NULL for unregistered name");
}

static void test_find_by_name(void) {
   tool_registry_register(&mock_tool);

   const tool_metadata_t *found = tool_registry_find("test_tool");
   TEST_ASSERT_NOT_NULL_MESSAGE(found, "find returns non-NULL for registered name");
   TEST_ASSERT_EQUAL_STRING_MESSAGE("test_tool", found->name, "find by name matches");
}

static void test_find_by_alias(void) {
   tool_registry_register(&mock_tool_aliased);

   const tool_metadata_t *found = tool_registry_find("alias_one");
   TEST_ASSERT_NOT_NULL_MESSAGE(found, "find returns non-NULL when searching by alias");
   TEST_ASSERT_EQUAL_STRING_MESSAGE("alias_tool", found->name, "alias resolves to correct tool");
}

static void test_lookup_alias(void) {
   tool_registry_register(&mock_tool_aliased);

   const tool_metadata_t *found = tool_registry_lookup_alias("alias_two");
   TEST_ASSERT_NOT_NULL_MESSAGE(found, "lookup_alias returns non-NULL for valid alias");
   TEST_ASSERT_EQUAL_STRING_MESSAGE("alias_tool", found->name, "alias_two resolves to alias_tool");
}

static void test_lookup_alias_not_found(void) {
   tool_registry_register(&mock_tool_aliased);

   const tool_metadata_t *found = tool_registry_lookup_alias("no_such_alias");
   TEST_ASSERT_NULL_MESSAGE(found, "lookup_alias returns NULL for unknown alias");
}

static void test_count(void) {
   TEST_ASSERT_EQUAL_INT_MESSAGE(0, tool_registry_count(), "count is 0 before registration");

   tool_registry_register(&mock_tool);
   TEST_ASSERT_EQUAL_INT_MESSAGE(1, tool_registry_count(), "count is 1 after one register");

   tool_registry_register(&mock_tool_network);
   TEST_ASSERT_EQUAL_INT_MESSAGE(2, tool_registry_count(), "count is 2 after two registers");
}

static void test_get_by_index(void) {
   tool_registry_register(&mock_tool);
   tool_registry_register(&mock_tool_network);

   const tool_metadata_t *first = tool_registry_get_by_index(0);
   TEST_ASSERT_NOT_NULL_MESSAGE(first, "get_by_index(0) returns non-NULL");
   TEST_ASSERT_EQUAL_STRING_MESSAGE("test_tool", first->name, "index 0 is first registered");

   const tool_metadata_t *second = tool_registry_get_by_index(1);
   TEST_ASSERT_NOT_NULL_MESSAGE(second, "get_by_index(1) returns non-NULL");
   TEST_ASSERT_EQUAL_STRING_MESSAGE("net_tool", second->name, "index 1 is second registered");

   const tool_metadata_t *oob = tool_registry_get_by_index(99);
   TEST_ASSERT_NULL_MESSAGE(oob, "get_by_index out of range returns NULL");
}

static void test_get_callback(void) {
   tool_registry_register(&mock_tool);

   tool_callback_fn cb = tool_registry_get_callback("test_tool");
   TEST_ASSERT_EQUAL_PTR_MESSAGE(mock_callback, cb, "get_callback returns registered callback");

   tool_callback_fn missing = tool_registry_get_callback("nonexistent");
   TEST_ASSERT_NULL_MESSAGE(missing, "get_callback returns NULL for unknown tool");
}

static void test_is_enabled_non_dangerous(void) {
   tool_registry_register(&mock_tool);

   bool enabled = tool_registry_is_enabled("test_tool");
   TEST_ASSERT_TRUE_MESSAGE(enabled, "non-DANGEROUS tool is always enabled");
}

static void test_is_enabled_not_found(void) {
   bool enabled = tool_registry_is_enabled("ghost_tool");
   TEST_ASSERT_FALSE_MESSAGE(enabled, "is_enabled returns false for unregistered tool");
}

static void test_has_capability_true(void) {
   tool_registry_register(&mock_tool_network);

   bool has_net = tool_registry_has_capability("net_tool", TOOL_CAP_NETWORK);
   TEST_ASSERT_TRUE_MESSAGE(has_net, "net_tool has TOOL_CAP_NETWORK");
}

static void test_has_capability_false(void) {
   tool_registry_register(&mock_tool);

   bool has_net = tool_registry_has_capability("test_tool", TOOL_CAP_NETWORK);
   TEST_ASSERT_FALSE_MESSAGE(has_net, "test_tool does not have TOOL_CAP_NETWORK");
}

static void test_lock_prevents_registration(void) {
   tool_registry_register(&mock_tool);
   tool_registry_lock();

   TEST_ASSERT_TRUE_MESSAGE(tool_registry_is_locked(), "registry is locked");

   int ret = tool_registry_register(&mock_tool_network);
   TEST_ASSERT_NOT_EQUAL_MESSAGE(0, ret, "register fails when registry is locked");

   /* Count should still be 1 (only the pre-lock registration) */
   TEST_ASSERT_EQUAL_INT_MESSAGE(1, tool_registry_count(),
                                 "count unchanged after rejected registration");
}

static void test_duplicate_name_rejected(void) {
   int first = tool_registry_register(&mock_tool);
   TEST_ASSERT_EQUAL_INT_MESSAGE(0, first, "first registration succeeds");

   int second = tool_registry_register(&mock_tool);
   TEST_ASSERT_NOT_EQUAL_MESSAGE(0, second, "duplicate name registration fails");

   TEST_ASSERT_EQUAL_INT_MESSAGE(1, tool_registry_count(), "count is 1 after duplicate rejected");
}

static void test_count_tool_variations(void) {
   tool_registry_register(&mock_tool);

   int variations = tool_registry_count_tool_variations("test_tool");
   TEST_ASSERT_GREATER_THAN_MESSAGE(0, variations, "TRIGGER tool has at least 1 variation");
}

static void test_count_tool_variations_with_aliases(void) {
   tool_registry_register(&mock_tool_aliased);

   int variations = tool_registry_count_tool_variations("alias_tool");
   /* Patterns are multiplied by (1 + alias_count), so with 2 aliases the
    * count should be 3x the base pattern count. */
   TEST_ASSERT_GREATER_THAN_MESSAGE(0, variations, "aliased tool has variations");

   /* Also verify total count includes this tool */
   int total = tool_registry_count_variations();
   TEST_ASSERT_EQUAL_INT_MESSAGE(variations, total,
                                 "total variations equals single tool when only one registered");
}

static void foreach_increment_callback(const tool_metadata_t *metadata, void *user_data) {
   (void)metadata;
   int *count = (int *)user_data;
   (*count)++;
}

static void test_foreach_iterates_all(void) {
   tool_registry_register(&mock_tool);
   tool_registry_register(&mock_tool_network);

   int count = 0;
   tool_registry_foreach(foreach_increment_callback, &count);
   TEST_ASSERT_EQUAL_INT_MESSAGE(2, count, "foreach invokes callback once per registered tool");
}

static void test_cache_invalidation(void) {
   TEST_ASSERT_TRUE_MESSAGE(tool_registry_is_cache_valid(), "cache valid initially");

   tool_registry_invalidate_cache();
   TEST_ASSERT_FALSE_MESSAGE(tool_registry_is_cache_valid(), "cache invalid after invalidate");
}

/* ============================================================================
 * validate_schedulable — exhaustive branch coverage.  This helper is the
 * single source of truth for "is this tool safe to fire from the scheduler",
 * called from both create-time (LLM tool) and fire-time (briefing thread).
 * Drift between the two paths historically caused legitimate briefings to
 * fail validation at fire — pinning every branch keeps the contract stable.
 * ============================================================================ */

static void test_validate_unknown_tool(void) {
   char err[160] = { 0 };
   int rc = tool_registry_validate_schedulable("not_a_tool", "anything", err, sizeof(err));
   TEST_ASSERT_NOT_EQUAL_MESSAGE(0, rc, "unknown tool name returns FAILURE");
   TEST_ASSERT_NOT_NULL_MESSAGE(strstr(err, "unknown tool"),
                                "error message names the unknown-tool branch");
}

static void test_validate_null_tool_name(void) {
   char err[160] = { 0 };
   int rc = tool_registry_validate_schedulable(NULL, "anything", err, sizeof(err));
   TEST_ASSERT_NOT_EQUAL_MESSAGE(0, rc, "NULL tool_name returns FAILURE");
   TEST_ASSERT_NOT_NULL_MESSAGE(strstr(err, "tool_name is required"),
                                "error names the missing-tool-name branch");

   rc = tool_registry_validate_schedulable("", "anything", err, sizeof(err));
   TEST_ASSERT_NOT_EQUAL_MESSAGE(0, rc, "empty tool_name returns FAILURE");
}

static void test_validate_not_schedulable(void) {
   tool_registry_register(&mock_tool); /* TOOL_CAP_NONE — not schedulable */

   char err[160] = { 0 };
   int rc = tool_registry_validate_schedulable("test_tool", "anything", err, sizeof(err));
   TEST_ASSERT_NOT_EQUAL_MESSAGE(0, rc, "non-schedulable tool returns FAILURE");
   TEST_ASSERT_NOT_NULL_MESSAGE(strstr(err, "not schedulable"),
                                "error names the not-schedulable branch");
}

static void test_validate_schedulable_no_value_pass(void) {
   tool_registry_register(&mock_schedulable_no_value);

   char err[160] = { 0 };
   /* Empty value OK for tools that don't require one. */
   int rc = tool_registry_validate_schedulable("sched_tool", "", err, sizeof(err));
   TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc, "schedulable + no-requires-value + empty value passes");

   /* NULL value OK too. */
   rc = tool_registry_validate_schedulable("sched_tool", NULL, err, sizeof(err));
   TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc, "schedulable + no-requires-value + NULL value passes");

   /* Populated value also passes (no requirement either way). */
   rc = tool_registry_validate_schedulable("sched_tool", "Atlanta", err, sizeof(err));
   TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc, "schedulable + populated value passes");
}

static void test_validate_requires_value_empty_fails(void) {
   tool_registry_register(&mock_schedulable_with_value);

   char err[160] = { 0 };
   int rc = tool_registry_validate_schedulable("sched_val_tool", "", err, sizeof(err));
   TEST_ASSERT_NOT_EQUAL_MESSAGE(0, rc, "requires_value + empty value returns FAILURE");
   TEST_ASSERT_NOT_NULL_MESSAGE(strstr(err, "requires"),
                                "error message names the requires-value branch");

   rc = tool_registry_validate_schedulable("sched_val_tool", NULL, err, sizeof(err));
   TEST_ASSERT_NOT_EQUAL_MESSAGE(0, rc, "requires_value + NULL value returns FAILURE");
}

static void test_validate_requires_value_populated_passes(void) {
   tool_registry_register(&mock_schedulable_with_value);

   char err[160] = { 0 };
   int rc = tool_registry_validate_schedulable("sched_val_tool", "today's news", err, sizeof(err));
   TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc, "requires_value + populated value passes");
}

/* Note: the "disabled" branch of validate_schedulable
 * (`!tool_registry_is_enabled(name)` between the SCHEDULABLE check and the
 * REQUIRES_VALUE check) is structurally unreachable from this test
 * harness — registered tools default to enabled=true and the registry
 * exposes no public toggle.  Unregistered tools hit the unknown-tool
 * branch in find() first.  In production it fires when an operator sets
 * `[tool.foo] enabled = false` via dawn.toml; that path is config-driven
 * and out of scope here. */

static void test_validate_null_err_buf_safe(void) {
   tool_registry_register(&mock_schedulable_no_value);

   /* Caller passing NULL err_buf is allowed — function must not deref. */
   int rc = tool_registry_validate_schedulable("sched_tool", "ok", NULL, 0);
   TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc, "NULL err_buf with valid input returns SUCCESS");

   /* And on the failure path the NULL err_buf must still be safe. */
   rc = tool_registry_validate_schedulable("not_real", "ok", NULL, 0);
   TEST_ASSERT_NOT_EQUAL_MESSAGE(0, rc, "NULL err_buf with unknown tool returns FAILURE");
}

/* ============================================================================
 * ARRAY param: schema emission + custom-field encode/decode contract
 * ============================================================================ */

static void test_schema_array_emits_items(void) {
   tool_registry_register(&mock_tool_array);

   char buf[4096] = { 0 };
   int written = 0;
   int rc = tool_registry_generate_llm_schema(buf, sizeof(buf), false, false, &written);
   TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc, "schema generation succeeds");

   /* The array param advertises type:array with string items. */
   TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "\"items\":{\"type\":\"string\"}"),
                                "array param emits items:{type:string}");
   TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "\"type\":\"array\""), "array param emits type:array");
}

static void test_array_param_must_be_last(void) {
   /* ARRAY param declared last → registers fine. */
   TEST_ASSERT_EQUAL_INT_MESSAGE(0, tool_registry_register(&mock_tool_array),
                                 "ARRAY-last tool registers");
   /* ARRAY param NOT last → rejected. */
   TEST_ASSERT_NOT_EQUAL_MESSAGE(0, tool_registry_register(&mock_tool_array_bad),
                                 "ARRAY-not-last tool is rejected");
}

static void test_schema_scalar_has_no_items(void) {
   /* A tool with only scalar params must NOT emit an items key (back-compat). */
   tool_registry_register(&mock_tool);

   char buf[4096] = { 0 };
   int written = 0;
   int rc = tool_registry_generate_llm_schema(buf, sizeof(buf), false, false, &written);
   TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc, "schema generation succeeds");
   TEST_ASSERT_NULL_MESSAGE(strstr(buf, "\"items\""), "scalar-only tool emits no items key");
}

static void test_extract_custom_tail_reads_to_end(void) {
   /* A serialized array containing a literal "::" must survive the tail
    * extractor intact (terminal slot), whereas the plain extractor truncates. */
   const char *encoded = "::items::[\"Song :: Reprise\",\"Plain\"]";
   char out[256] = { 0 };

   TEST_ASSERT_TRUE(tool_param_extract_custom_tail(encoded, "items", out, sizeof(out)));
   TEST_ASSERT_EQUAL_STRING("[\"Song :: Reprise\",\"Plain\"]", out);

   /* Plain extractor stops at the internal "::" — demonstrates why tail is needed. */
   char trunc[256] = { 0 };
   TEST_ASSERT_TRUE(tool_param_extract_custom(encoded, "items", trunc, sizeof(trunc)));
   TEST_ASSERT_EQUAL_STRING("[\"Song ", trunc);
}

static void test_extract_base_and_custom_coexist(void) {
   /* base value + a scalar custom before the terminal array. */
   const char *encoded = "queenquery::limit::5::items::[\"a\",\"b\"]";
   char base[64] = { 0 };
   char limit[16] = { 0 };
   char items[64] = { 0 };

   tool_param_extract_base(encoded, base, sizeof(base));
   TEST_ASSERT_EQUAL_STRING("queenquery", base);

   TEST_ASSERT_TRUE(tool_param_extract_custom(encoded, "limit", limit, sizeof(limit)));
   TEST_ASSERT_EQUAL_STRING("5", limit);

   TEST_ASSERT_TRUE(tool_param_extract_custom_tail(encoded, "items", items, sizeof(items)));
   TEST_ASSERT_EQUAL_STRING("[\"a\",\"b\"]", items);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
   UNITY_BEGIN();

   RUN_TEST(test_init_returns_success);
   RUN_TEST(test_register_returns_success);
   RUN_TEST(test_lookup_by_name);
   RUN_TEST(test_lookup_not_found);
   RUN_TEST(test_find_by_name);
   RUN_TEST(test_find_by_alias);
   RUN_TEST(test_lookup_alias);
   RUN_TEST(test_lookup_alias_not_found);
   RUN_TEST(test_count);
   RUN_TEST(test_get_by_index);
   RUN_TEST(test_get_callback);
   RUN_TEST(test_is_enabled_non_dangerous);
   RUN_TEST(test_is_enabled_not_found);
   RUN_TEST(test_has_capability_true);
   RUN_TEST(test_has_capability_false);
   RUN_TEST(test_lock_prevents_registration);
   RUN_TEST(test_duplicate_name_rejected);
   RUN_TEST(test_count_tool_variations);
   RUN_TEST(test_count_tool_variations_with_aliases);
   RUN_TEST(test_foreach_iterates_all);
   RUN_TEST(test_cache_invalidation);

   /* validate_schedulable branch pins */
   RUN_TEST(test_validate_unknown_tool);
   RUN_TEST(test_validate_null_tool_name);
   RUN_TEST(test_validate_not_schedulable);
   RUN_TEST(test_validate_schedulable_no_value_pass);
   RUN_TEST(test_validate_requires_value_empty_fails);
   RUN_TEST(test_validate_requires_value_populated_passes);
   RUN_TEST(test_validate_null_err_buf_safe);

   /* ARRAY param: schema emission + encode/decode contract */
   RUN_TEST(test_schema_array_emits_items);
   RUN_TEST(test_array_param_must_be_last);
   RUN_TEST(test_schema_scalar_has_no_items);
   RUN_TEST(test_extract_custom_tail_reads_to_end);
   RUN_TEST(test_extract_base_and_custom_coexist);

   return UNITY_END();
}
