#include "tools/shell_policy.h"

#include <jansson.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                                          \
    do {                                                                                          \
        if (!(condition)) {                                                                       \
            fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #condition);     \
            return -1;                                                                            \
        }                                                                                         \
    } while (0)

static const char valid_config_json[] =
    "{"
    "\"version\":2,"
    "\"control\":{\"token\":\"unit-test-token\"},"
    "\"defaults\":{"
    "\"shell_enabled\":true,"
    "\"command_length\":65536,"
    "\"timeout_ms\":300000,"
    "\"output_bytes\":65536,"
    "\"once_read_stdout_err_chunk_size\":1024,"
    "\"capture_stderr\":true,"
    "\"merge_stderr_to_stdout\":false,"
    "\"execution\":{"
    "\"mode\":\"shell\","
    "\"shell_path\":\"/bin/sh\","
    "\"shell_arg\":\"-c\","
    "\"working_directory\":\"/tmp/mcp-shell\","
    "\"inherit_env\":false,"
    "\"request_cwd_allowed\":true,"
    "\"request_env_allowed\":true,"
    "\"kill_process_group_on_timeout\":true,"
    "\"run_as_user\":\"\","
    "\"run_as_group\":\"\","
    "\"env\":{\"PATH\":\"/usr/bin:/bin\",\"HOME\":\"/tmp/mcp-shell\",\"LANG\":\"C\"}"
    "},"
    "\"limits\":{"
    "\"cpu_seconds\":3600,"
    "\"memory_bytes\":2147483648,"
    "\"file_size_bytes\":2147483648,"
    "\"open_files\":64,"
    "\"processes\":16"
    "},"
    "\"isolation\":{\"require_non_root\":false}"
    "},"
    "\"bounds\":{"
    "\"command_length\":{\"min\":1,\"max\":65536},"
    "\"timeout_ms\":{\"min\":1,\"max\":3600000},"
    "\"output_bytes\":{\"min\":0,\"max\":1048576},"
    "\"once_read_stdout_err_chunk_size\":{\"min\":64,\"max\":65536},"
    "\"execution\":{"
    "\"mode\":{\"allowed\":[\"shell\",\"exec\"]},"
    "\"shell_path\":{\"min_bytes\":1,\"max_bytes\":4096},"
    "\"shell_arg\":{\"min_bytes\":0,\"max_bytes\":65536},"
    "\"working_directory\":{\"min_bytes\":1,\"max_bytes\":4096},"
    "\"allowed_env\":{\"max_items\":1024,\"item_max_bytes\":255},"
    "\"run_as_user\":{\"min_bytes\":0},"
    "\"run_as_group\":{\"min_bytes\":0}"
    "},"
    "\"limits\":{"
    "\"cpu_seconds\":{\"min\":0,\"max\":3600},"
    "\"memory_bytes\":{\"min\":0,\"max\":34359738367},"
    "\"file_size_bytes\":{\"min\":0,\"max\":17179869184},"
    "\"open_files\":{\"min\":0,\"max\":65536},"
    "\"processes\":{\"min\":0,\"max\":2048}"
    "}"
    "}"
    "}";

static json_t *valid_root(void)
{
    json_error_t error;
    json_t *root = json_loads(valid_config_json, JSON_REJECT_DUPLICATES, &error);

    if (!root)
        fprintf(stderr, "test config parse failed: %s\n", error.text);
    return root;
}

static int parse_root(json_t *root, struct mcp_shell_policy_snapshot **out)
{
    char error[256];

    error[0] = '\0';
    if (mcp_shell_policy_snapshot_parse_json(out, root, "(unit-test)", true, error, sizeof(error)) !=
        0) {
        fprintf(stderr, "policy parse failed: %s\n", error);
        return -1;
    }
    return 0;
}

static int test_field_directory(void)
{
    const struct mcp_shell_policy_field_descriptor *fields;
    size_t count;
    size_t i;
    size_t j;

    fields = mcp_shell_policy_field_directory(&count);
    CHECK(fields != NULL);
    CHECK(count == MCP_SHELL_POLICY_FIELD_COUNT);
    for (i = 0; i < count; i++) {
        CHECK(fields[i].id == (enum mcp_shell_policy_field_id)i);
        CHECK(fields[i].path != NULL && fields[i].path[0] != '\0');
        for (j = i + 1; j < count; j++)
            CHECK(strcmp(fields[i].path, fields[j].path) != 0);
    }
    CHECK(fields[MCP_SHELL_POLICY_FIELD_MEMORY_BYTES].hard_max == UINT64_C(34359738367));
    CHECK(fields[MCP_SHELL_POLICY_FIELD_MEMORY_BYTES].hard_default == UINT64_C(34359738367));
    return 0;
}

static int test_valid_config_and_token_copy(void)
{
    const struct mcp_shell_policy_field_descriptor *fields;
    json_t *root = valid_root();
    struct mcp_shell_policy_snapshot *snapshot = NULL;
    json_t *control;
    json_t *serialized;
    size_t count;
    size_t index;

    CHECK(root != NULL);
    CHECK(parse_root(root, &snapshot) == 0);
    CHECK(snapshot->version == 2);
    CHECK(snapshot->config_loaded);
    CHECK(snapshot->control_enabled);
    CHECK(snapshot->token_length == strlen("unit-test-token"));
    CHECK(strcmp(snapshot->token, "unit-test-token") == 0);
    CHECK(snapshot->defaults.limits.memory_bytes.value == UINT64_C(2147483648));
    CHECK(snapshot->defaults.limits.memory_bytes.max == UINT64_C(34359738367));
    CHECK(snapshot->defaults.limits.memory_bytes.source == MCP_SHELL_POLICY_SOURCE_JSON);
    CHECK(snapshot->defaults.execution.run_as_user.max_bytes == 255u);
    CHECK(snapshot->environment_max_items == 1024u);
    CHECK(snapshot->environment_item_max_bytes == 255u);

    control = json_object_get(root, "control");
    CHECK(json_object_set_new(control, "token", json_string("changed")) == 0);
    CHECK(strcmp(snapshot->token, "unit-test-token") == 0);

    fields = mcp_shell_policy_field_directory(&count);
    for (index = 0; index < count; index++) {
        serialized = fields[index].serializer(snapshot, &fields[index]);
        CHECK(serialized != NULL);
        json_decref(serialized);
    }

    json_decref(root);
    mcp_shell_policy_snapshot_destroy(snapshot);
    return 0;
}

static int test_hard_profile(void)
{
    struct mcp_shell_policy_snapshot *snapshot = NULL;

    CHECK(mcp_shell_policy_snapshot_create_hard(&snapshot, "(unit-hard)", false) == 0);
    CHECK(snapshot != NULL);
    CHECK(!snapshot->config_loaded);
    CHECK(!snapshot->control_enabled);
    CHECK(snapshot->token == NULL && snapshot->token_length == 0u);
    CHECK(snapshot->defaults.shell_enabled);
    CHECK(snapshot->defaults.command_length.value == snapshot->defaults.command_length.max);
    CHECK(snapshot->defaults.timeout_ms.value == snapshot->defaults.timeout_ms.max);
    CHECK(snapshot->defaults.output_bytes.value == snapshot->defaults.output_bytes.max);
    CHECK(snapshot->defaults.limits.memory_bytes.value ==
          snapshot->defaults.limits.memory_bytes.max);
    CHECK(snapshot->defaults.limits.file_size_bytes.value ==
          snapshot->defaults.limits.file_size_bytes.max);
    CHECK(snapshot->environment_source == MCP_SHELL_POLICY_SOURCE_HARD);
    CHECK(snapshot->defaults.execution.env_var_count > 0u);
    mcp_shell_policy_snapshot_destroy(snapshot);
    return 0;
}

static int test_start_environment_snapshot(void)
{
    const char *name = "MCP_S3_START_ENV_SNAPSHOT_TEST";
    const char *initial = "initial-parent-value";
    struct mcp_shell_policy_snapshot *snapshot = NULL;
    size_t index;
    bool found = false;

#ifdef _WIN32
    CHECK(_putenv_s(name, initial) == 0);
#else
    CHECK(setenv(name, initial, 1) == 0);
#endif
    CHECK(mcp_shell_policy_snapshot_create_hard(&snapshot, "(unit-hard)", false) == 0);
    CHECK(snapshot != NULL);
#ifdef _WIN32
    CHECK(_putenv_s(name, "changed-parent-value") == 0);
#else
    CHECK(setenv(name, "changed-parent-value", 1) == 0);
#endif
    for (index = 0; index < snapshot->startup_env_var_count; index++) {
        if (strcmp(snapshot->startup_env_vars[index].name, name) == 0) {
            CHECK(strcmp(snapshot->startup_env_vars[index].value, initial) == 0);
            found = true;
            break;
        }
    }
    CHECK(found);
    mcp_shell_policy_snapshot_destroy(snapshot);
    return 0;
}

static int test_utf8_byte_length_fallback(void)
{
    json_t *root = valid_root();
    struct mcp_shell_policy_snapshot *snapshot = NULL;
    json_t *execution;
    char *value;
    size_t index;
    size_t length = 4098u;

    CHECK(root != NULL);
    value = malloc(length);
    CHECK(value != NULL);
    for (index = 0; index < length; index += 2u)
        memcpy(&value[index], "\xc3\xa9", 2u);
    execution = json_object_get(json_object_get(root, "defaults"), "execution");
    CHECK(json_object_set_new(execution, "shell_path", json_stringn(value, length)) == 0);
    free(value);

    CHECK(parse_root(root, &snapshot) == 0);
    CHECK(snapshot->defaults.execution.shell_path.source ==
          MCP_SHELL_POLICY_SOURCE_HARD_FALLBACK);
    CHECK(snapshot->defaults.execution.shell_path.max_bytes == 4096u);
    CHECK(strcmp(mcp_shell_policy_snapshot_diagnostic(
                     snapshot, MCP_SHELL_POLICY_FIELD_SHELL_PATH),
                 "default_out_of_bounds") == 0);
    json_decref(root);
    mcp_shell_policy_snapshot_destroy(snapshot);
    return 0;
}

static int test_field_fallbacks(void)
{
    json_t *root = valid_root();
    struct mcp_shell_policy_snapshot *snapshot = NULL;
    json_t *defaults;
    json_t *bounds;
    json_t *limits;

    CHECK(root != NULL);
    defaults = json_object_get(root, "defaults");
    CHECK(json_object_set_new(defaults, "command_length", json_integer(0)) == 0);
    bounds = json_object_get(root, "bounds");
    limits = json_object_get(bounds, "limits");
    CHECK(json_object_set_new(json_object_get(limits, "memory_bytes"),
                              "max",
                              json_integer(UINT64_C(34359738368))) == 0);

    CHECK(parse_root(root, &snapshot) == 0);
    CHECK(snapshot->defaults.command_length.value == UINT64_C(65536));
    CHECK(snapshot->defaults.command_length.min == UINT64_C(1));
    CHECK(snapshot->defaults.command_length.max == UINT64_C(65536));
    CHECK(snapshot->defaults.command_length.source == MCP_SHELL_POLICY_SOURCE_HARD_FALLBACK);
    CHECK(strcmp(mcp_shell_policy_snapshot_diagnostic(
                     snapshot, MCP_SHELL_POLICY_FIELD_COMMAND_LENGTH),
                 "default_out_of_bounds") == 0);
    CHECK(snapshot->defaults.limits.memory_bytes.value == UINT64_C(34359738367));
    CHECK(snapshot->defaults.limits.memory_bytes.max == UINT64_C(34359738367));
    CHECK(snapshot->defaults.limits.memory_bytes.source == MCP_SHELL_POLICY_SOURCE_HARD_FALLBACK);
    CHECK(strcmp(mcp_shell_policy_snapshot_diagnostic(
                     snapshot, MCP_SHELL_POLICY_FIELD_MEMORY_BYTES),
                 "bound_above_hard_max") == 0);
    CHECK(snapshot->defaults.timeout_ms.value == UINT64_C(300000));
    CHECK(snapshot->defaults.timeout_ms.source == MCP_SHELL_POLICY_SOURCE_JSON);

    json_decref(root);
    mcp_shell_policy_snapshot_destroy(snapshot);
    return 0;
}

static int test_strict_type_rejection(void)
{
    json_t *root = valid_root();
    struct mcp_shell_policy_snapshot *snapshot = NULL;
    char error[256];

    CHECK(root != NULL);
    CHECK(json_object_set_new(json_object_get(root, "defaults"),
                              "timeout_ms",
                              json_string("300000")) == 0);
    error[0] = '\0';
    CHECK(mcp_shell_policy_snapshot_parse_json(
              &snapshot, root, "(unit-test)", true, error, sizeof(error)) != 0);
    CHECK(snapshot == NULL);
    CHECK(strstr(error, "defaults.timeout_ms") != NULL);
    json_decref(root);
    return 0;
}

int main(void)
{
    if (test_field_directory() != 0 || test_valid_config_and_token_copy() != 0 ||
        test_hard_profile() != 0 || test_start_environment_snapshot() != 0 ||
        test_utf8_byte_length_fallback() != 0 ||
        test_field_fallbacks() != 0 || test_strict_type_rejection() != 0)
        return 1;
    return 0;
}
