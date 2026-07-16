#include "tools/schema.h"

#include <jansson.h>
#include <stdbool.h>
#include <stdlib.h>

#ifndef MCP_SHELL_EXEC_DEFAULT_CONFIG
#define MCP_SHELL_EXEC_DEFAULT_CONFIG "config/tools/shell_exec.json"
#endif

#ifndef MCP_SHELL_EXEC_INSTALLED_CONFIG
#define MCP_SHELL_EXEC_INSTALLED_CONFIG "share/mcp_server/config/tools/shell_exec.json"
#endif

#define MCP_SHELL_EXEC_MIN_TIMEOUT_MS 1u
#define MCP_SHELL_EXEC_MAX_TIMEOUT_MS 30000u
#define MCP_SHELL_EXEC_DEFAULT_TIMEOUT_MS 5000u
#define MCP_SHELL_EXEC_DEFAULT_OUTPUT_BYTES 16384u

struct shell_schema_config {
    bool config_loaded;
    unsigned int default_timeout_ms;
    unsigned int max_timeout_ms;
    unsigned int max_output_bytes;
};

static json_t *schema_with_properties(json_t *properties, json_t *required)
{
    json_t *schema = json_object();

    json_object_set_new(schema, "type", json_string("object"));
    json_object_set_new(schema, "properties", properties);
    if (required)
        json_object_set_new(schema, "required", required);

    return schema;
}

static unsigned int clamp_uint(unsigned long parsed,
                               unsigned int default_value,
                               unsigned int min_value,
                               unsigned int max_value)
{
    if (parsed < (unsigned long)min_value || parsed > (unsigned long)max_value)
        return default_value;
    return (unsigned int)parsed;
}

static unsigned int env_uint(const char *name,
                             unsigned int default_value,
                             unsigned int min_value,
                             unsigned int max_value)
{
    const char *value = getenv(name);
    char *end = NULL;
    unsigned long parsed;

    if (!value || value[0] == '\0')
        return default_value;

    parsed = strtoul(value, &end, 10);
    if (!end || *end != '\0')
        return default_value;

    return clamp_uint(parsed, default_value, min_value, max_value);
}

static const char *shell_schema_env_config_path(void)
{
    const char *env_path = getenv("MCP_SHELL_EXEC_CONFIG");

    if (env_path && env_path[0] != '\0')
        return env_path;
    return NULL;
}

static void shell_schema_config_defaults(struct shell_schema_config *cfg)
{
    cfg->config_loaded = false;
    cfg->default_timeout_ms = MCP_SHELL_EXEC_DEFAULT_TIMEOUT_MS;
    cfg->max_timeout_ms = MCP_SHELL_EXEC_MAX_TIMEOUT_MS;
    cfg->max_output_bytes = MCP_SHELL_EXEC_DEFAULT_OUTPUT_BYTES;
}

static int shell_schema_config_parse_json(struct shell_schema_config *cfg, const json_t *root)
{
    json_t *value;

    if (!json_is_object(root))
        return -1;

    value = json_object_get(root, "default_timeout_ms");
    if (json_is_integer(value))
        cfg->default_timeout_ms =
            clamp_uint((unsigned long)json_integer_value(value),
                       cfg->default_timeout_ms,
                       MCP_SHELL_EXEC_MIN_TIMEOUT_MS,
                       MCP_SHELL_EXEC_MAX_TIMEOUT_MS);

    value = json_object_get(root, "max_timeout_ms");
    if (json_is_integer(value))
        cfg->max_timeout_ms = clamp_uint((unsigned long)json_integer_value(value),
                                         cfg->max_timeout_ms,
                                         MCP_SHELL_EXEC_MIN_TIMEOUT_MS,
                                         MCP_SHELL_EXEC_MAX_TIMEOUT_MS);

    value = json_object_get(root, "max_output_bytes");
    if (json_is_integer(value))
        cfg->max_output_bytes =
            clamp_uint((unsigned long)json_integer_value(value), cfg->max_output_bytes, 256u, 1048576u);

    if (cfg->default_timeout_ms > cfg->max_timeout_ms)
        cfg->default_timeout_ms = cfg->max_timeout_ms;

    return 0;
}

static void shell_schema_config_load(struct shell_schema_config *cfg)
{
    const char *env_path;
    const char *candidates[] = {
        MCP_SHELL_EXEC_DEFAULT_CONFIG,
        MCP_SHELL_EXEC_INSTALLED_CONFIG,
    };
    size_t index;
    json_error_t error;
    json_t *root = NULL;

    shell_schema_config_defaults(cfg);

    env_path = shell_schema_env_config_path();
    if (env_path) {
        root = json_load_file(env_path, JSON_REJECT_DUPLICATES, &error);
    } else {
        for (index = 0; index < sizeof(candidates) / sizeof(candidates[0]); index++) {
            root = json_load_file(candidates[index], JSON_REJECT_DUPLICATES, &error);
            if (root)
                break;
        }
    }

    if (root) {
        if (shell_schema_config_parse_json(cfg, root) == 0)
            cfg->config_loaded = true;
        json_decref(root);
    } else if (!env_path) {
        cfg->config_loaded = true;
    }

    if (!cfg->config_loaded)
        return;

    cfg->default_timeout_ms = env_uint("MCP_SHELL_EXEC_TIMEOUT_MS",
                                       cfg->default_timeout_ms,
                                       MCP_SHELL_EXEC_MIN_TIMEOUT_MS,
                                       MCP_SHELL_EXEC_MAX_TIMEOUT_MS);
    cfg->max_timeout_ms = env_uint("MCP_SHELL_MAX_TIMEOUT_MS",
                                   cfg->max_timeout_ms,
                                   MCP_SHELL_EXEC_MIN_TIMEOUT_MS,
                                   MCP_SHELL_EXEC_MAX_TIMEOUT_MS);
    cfg->max_output_bytes =
        env_uint("MCP_SHELL_OUTPUT_LIMIT", cfg->max_output_bytes, 256u, 1048576u);

    if (cfg->default_timeout_ms > cfg->max_timeout_ms)
        cfg->default_timeout_ms = cfg->max_timeout_ms;
}

json_t *mcp_schema_empty_object(void)
{
    return json_pack("{s:s,s:o}", "type", "object", "properties", json_object());
}

static json_t *sandbox_schema_nullable(json_t *typed)
{
    json_t *options = json_array();
    json_t *null_type = json_pack("{s:s}", "type", "null");
    json_t *schema = json_object();

    if (!typed || !options || !null_type || !schema)
        goto fail;
    if (json_array_append_new(options, typed) != 0)
        goto fail;
    typed = NULL;
    if (json_array_append_new(options, null_type) != 0)
        goto fail;
    null_type = NULL;
    if (json_object_set_new(schema, "anyOf", options) != 0)
        goto fail;
    options = NULL;
    return schema;

fail:
    json_decref(typed);
    json_decref(options);
    json_decref(null_type);
    json_decref(schema);
    return NULL;
}

static json_t *sandbox_schema_nullable_boolean(void)
{
    return sandbox_schema_nullable(json_pack("{s:s}", "type", "boolean"));
}

static json_t *sandbox_schema_nullable_integer(json_int_t minimum, json_int_t maximum)
{
    return sandbox_schema_nullable(json_pack("{s:s,s:I,s:I}",
                                             "type",
                                             "integer",
                                             "minimum",
                                             minimum,
                                             "maximum",
                                             maximum));
}

static json_t *sandbox_schema_nullable_string(size_t minimum, size_t maximum)
{
    json_t *typed = json_pack("{s:s,s:I}", "type", "string", "minLength", (json_int_t)minimum);

    if (!typed)
        return NULL;
    if (maximum > 0u &&
        json_object_set_new(typed, "maxLength", json_integer((json_int_t)maximum)) != 0) {
        json_decref(typed);
        return NULL;
    }
    return sandbox_schema_nullable(typed);
}

static json_t *sandbox_schema_object(json_t *properties)
{
    json_t *schema = json_object();

    if (!properties || !schema)
        goto fail;
    if (json_object_set_new(schema, "type", json_string("object")) != 0 ||
        json_object_set_new(schema, "properties", properties) != 0 ||
        json_object_set_new(schema, "additionalProperties", json_false()) != 0)
        goto fail;
    properties = NULL;
    return schema;

fail:
    json_decref(properties);
    json_decref(schema);
    return NULL;
}

static int sandbox_schema_add(json_t *properties, const char *name, json_t *schema)
{
    if (!schema)
        return -1;
    if (json_object_set_new(properties, name, schema) != 0) {
        json_decref(schema);
        return -1;
    }
    return 0;
}

json_t *mcp_schema_sandbox_ctl(void)
{
    json_t *root_properties = json_object();
    json_t *override_properties = json_object();
    json_t *execution_properties = json_object();
    json_t *limit_properties = json_object();
    json_t *isolation_properties = json_object();
    json_t *action = NULL;
    json_t *allowed_env_typed = NULL;
    json_t *allowed_env_items = NULL;
    json_t *schema = NULL;
    json_t *required = NULL;

    if (!root_properties || !override_properties || !execution_properties || !limit_properties ||
        !isolation_properties)
        goto fail;

    action = json_pack("{s:s,s:[s,s,s]}",
                       "type",
                       "string",
                       "enum",
                       "get",
                       "update",
                       "reset");
    if (sandbox_schema_add(root_properties, "action", action) != 0)
        goto fail;
    action = NULL;
    if (sandbox_schema_add(root_properties,
                           "token",
                           json_pack("{s:s}", "type", "string")) != 0 ||
        sandbox_schema_add(root_properties,
                           "expected_revision",
                           json_pack("{s:s,s:I}", "type", "integer", "minimum", (json_int_t)0)) !=
            0 ||
        sandbox_schema_add(root_properties, "shell_enabled", sandbox_schema_nullable_boolean()) !=
            0 ||
        sandbox_schema_add(root_properties,
                           "sandbox_enabled",
                           json_pack("{s:s}", "type", "boolean")) != 0)
        goto fail;

    if (sandbox_schema_add(override_properties,
                           "max_command_length",
                           sandbox_schema_nullable_integer(64, 65535)) != 0 ||
        sandbox_schema_add(override_properties,
                           "default_timeout_ms",
                           sandbox_schema_nullable_integer(1, 300000)) != 0 ||
        sandbox_schema_add(override_properties,
                           "max_timeout_ms",
                           sandbox_schema_nullable_integer(1, 300000)) != 0 ||
        sandbox_schema_add(override_properties,
                           "max_output_bytes",
                           sandbox_schema_nullable_integer(256, 2147483648LL)) != 0 ||
        sandbox_schema_add(override_properties,
                           "capture_stderr",
                           sandbox_schema_nullable_boolean()) != 0 ||
        sandbox_schema_add(override_properties,
                           "merge_stderr",
                           sandbox_schema_nullable_boolean()) != 0)
        goto fail;

    if (sandbox_schema_add(execution_properties,
                           "working_directory",
                           sandbox_schema_nullable_string(1u, 4096u)) != 0 ||
        sandbox_schema_add(execution_properties,
                           "request_cwd_allowed",
                           sandbox_schema_nullable_boolean()) != 0 ||
        sandbox_schema_add(execution_properties,
                           "clear_environment",
                           sandbox_schema_nullable_boolean()) != 0 ||
        sandbox_schema_add(execution_properties,
                           "request_env_allowed",
                           sandbox_schema_nullable_boolean()) != 0 ||
        sandbox_schema_add(execution_properties,
                           "kill_process_group_on_timeout",
                           sandbox_schema_nullable_boolean()) != 0 ||
        sandbox_schema_add(execution_properties,
                           "run_as_user",
                           sandbox_schema_nullable_string(0u, 0u)) != 0 ||
        sandbox_schema_add(execution_properties,
                           "run_as_group",
                           sandbox_schema_nullable_string(0u, 0u)) != 0)
        goto fail;

    allowed_env_items = json_pack("{s:s,s:I,s:I}",
                                  "type",
                                  "string",
                                  "minLength",
                                  (json_int_t)1,
                                  "maxLength",
                                  (json_int_t)255);
    allowed_env_typed = json_pack("{s:s,s:I,s:o}",
                                  "type",
                                  "array",
                                  "maxItems",
                                  (json_int_t)128,
                                  "items",
                                  allowed_env_items);
    allowed_env_items = NULL;
    if (sandbox_schema_add(execution_properties,
                           "allowed_env",
                           sandbox_schema_nullable(allowed_env_typed)) != 0)
        goto fail;
    allowed_env_typed = NULL;

    if (sandbox_schema_add(limit_properties,
                           "max_cpu_seconds",
                           sandbox_schema_nullable_integer(0, 3600)) != 0 ||
        sandbox_schema_add(limit_properties,
                           "max_memory_bytes",
                           sandbox_schema_nullable_integer(0, 2147483647)) != 0 ||
        sandbox_schema_add(limit_properties,
                           "max_file_size_bytes",
                           sandbox_schema_nullable_integer(0, 2147483647)) != 0 ||
        sandbox_schema_add(limit_properties,
                           "max_open_files",
                           sandbox_schema_nullable_integer(0, 1048576)) != 0 ||
        sandbox_schema_add(limit_properties,
                           "max_processes",
                           sandbox_schema_nullable_integer(0, 1048576)) != 0 ||
        sandbox_schema_add(isolation_properties,
                           "require_non_root",
                           sandbox_schema_nullable_boolean()) != 0)
        goto fail;

    if (sandbox_schema_add(override_properties,
                           "execution",
                           sandbox_schema_object(execution_properties)) != 0)
        goto fail;
    execution_properties = NULL;
    if (sandbox_schema_add(override_properties,
                           "limits",
                           sandbox_schema_object(limit_properties)) != 0)
        goto fail;
    limit_properties = NULL;
    if (sandbox_schema_add(override_properties,
                           "isolation",
                           sandbox_schema_object(isolation_properties)) != 0)
        goto fail;
    isolation_properties = NULL;
    if (sandbox_schema_add(root_properties,
                           "overrides",
                           sandbox_schema_object(override_properties)) != 0)
        goto fail;
    override_properties = NULL;

    schema = sandbox_schema_object(root_properties);
    root_properties = NULL;
    if (!schema)
        goto fail;
    required = json_pack("[s,s]", "action", "token");
    if (!required || json_object_set_new(schema, "required", required) != 0)
        goto fail;
    required = NULL;
    return schema;

fail:
    json_decref(root_properties);
    json_decref(override_properties);
    json_decref(execution_properties);
    json_decref(limit_properties);
    json_decref(isolation_properties);
    json_decref(action);
    json_decref(allowed_env_typed);
    json_decref(allowed_env_items);
    json_decref(required);
    json_decref(schema);
    return NULL;
}

json_t *mcp_schema_shell_exec(void)
{
    return json_pack("{s:s,s:{s:{s:s,s:s},s:{s:s,s:s}},s:[s]}",
                     "type",
                     "object",
                     "properties",
                     "command",
                     "type",
                     "string",
                     "description",
                     "Command string passed to the configured OS-isolated shell executor without content filtering.",
                     "timeout_ms",
                     "type",
                     "integer",
                     "description",
                     "Optional timeout override in milliseconds.",
                     "required",
                     "command");
}

json_t *mcp_schema_shell_start(void)
{
    struct shell_schema_config cfg;

    shell_schema_config_load(&cfg);

    return json_pack("{s:s,s:{s:{s:s,s:s},s:{s:s,s:s},s:{s:s,s:s,s:i,s:i},s:{s:s,s:s,s:i,s:i},s:{s:s,s:s},s:{s:s,s:s,s:{s:s}}},s:[s]}",
                     "type",
                     "object",
                     "properties",
                     "command",
                     "type",
                     "string",
                     "description",
                     "Command string passed to the configured OS-isolated shell executor.",
                     "label",
                     "type",
                     "string",
                     "description",
                     "Optional human-readable job label.",
                     "timeout_ms",
                     "type",
                     "integer",
                     "description",
                     "Optional job timeout in milliseconds.",
                     "minimum",
                     1,
                     "maximum",
                     (int)cfg.max_timeout_ms,
                     "output_limit_bytes",
                     "type",
                     "integer",
                     "description",
                     "Optional per-stream buffered output limit in bytes.",
                     "minimum",
                     256,
                     "maximum",
                     (int)cfg.max_output_bytes,
                     "cwd",
                     "type",
                     "string",
                     "description",
                     "Optional working directory override.",
                     "env",
                     "type",
                     "object",
                     "description",
                     "Optional string-valued environment overrides.",
                     "additionalProperties",
                     "type",
                     "string",
                     "required",
                     "command");
}

json_t *mcp_schema_shell_job_id(void)
{
    return schema_with_properties(
        json_pack("{s:{s:s,s:s}}",
                  "job_id",
                  "type",
                  "string",
                  "description",
                  "Shell job id returned by system.shell_start."),
        json_pack("[s]", "job_id"));
}

json_t *mcp_schema_shell_tail(void)
{
    return schema_with_properties(
        json_pack("{s:{s:s,s:s},s:{s:s,s:s},s:{s:s,s:s},s:{s:s,s:s}}",
                  "job_id",
                  "type",
                  "string",
                  "description",
                  "Shell job id returned by system.shell_start.",
                  "stdout_offset",
                  "type",
                  "integer",
                  "description",
                  "Stdout byte offset.",
                  "stderr_offset",
                  "type",
                  "integer",
                  "description",
                  "Stderr byte offset.",
                  "max_bytes",
                  "type",
                  "integer",
                  "description",
                  "Maximum bytes returned per stream."),
        json_pack("[s]", "job_id"));
}

json_t *mcp_schema_shell_wait(void)
{
    return schema_with_properties(
        json_pack("{s:{s:s,s:s},s:{s:s,s:s,s:i,s:i}}",
                  "job_id",
                  "type",
                  "string",
                  "description",
                  "Shell job id returned by system.shell_start.",
                  "timeout_ms",
                  "type",
                  "integer",
                  "description",
                  "Maximum local wait in milliseconds.",
                  "minimum",
                  0,
                  "maximum",
                  5000),
        json_pack("[s]", "job_id"));
}

json_t *mcp_schema_shell_kill(void)
{
    return schema_with_properties(
        json_pack("{s:{s:s,s:s},s:{s:s,s:s,s:i,s:i}}",
                  "job_id",
                  "type",
                  "string",
                  "description",
                  "Shell job id returned by system.shell_start.",
                  "signal",
                  "type",
                  "integer",
                  "description",
                  "POSIX signal number; defaults to SIGTERM.",
                  "minimum",
                  1,
                  "maximum",
                  64),
        json_pack("[s]", "job_id"));
}

json_t *mcp_schema_gateway_proxy(void)
{
    return schema_with_properties(
        json_pack("{s:{s:s,s:s,s:i},s:{s:s,s:s},s:{s:s,s:s},s:{s:s,s:s,s:i,s:i}}",
                  "server_id",
                  "type",
                  "integer",
                  "description",
                  "Stable server id from server.list_servers.",
                  "minimum",
                  1,
                  "tool_name",
                  "type",
                  "string",
                  "description",
                  "Remote tool name, or tools_list to refresh cached remote tools.",
                  "args",
                  "type",
                  "object",
                  "description",
                  "Arguments passed to the remote tool.",
                  "proxy_timeout_ms",
                  "type",
                  "integer",
                  "description",
                  "End-to-end gateway wait timeout in milliseconds for this proxy call.",
                  "minimum",
                  1,
                  "maximum",
                  300000),
        json_pack("[s,s]", "server_id", "tool_name"));
}

json_t *mcp_schema_server_list_servers(void)
{
    return schema_with_properties(
        json_pack("{s:{s:s,s:s,s:i,s:i}}",
                  "wait_ms",
                  "type",
                  "integer",
                  "description",
                  "Discovery response wait window in milliseconds.",
                  "minimum",
                  1,
                  "maximum",
                  5000),
        NULL);
}

json_t *mcp_schema_plugin_insmod(void)
{
    return schema_with_properties(
        json_pack("{s:{s:s,s:s},s:{s:s}}",
                  "package_path",
                  "type",
                  "string",
                  "description",
                  "Path to a module package.",
                  "enable",
                  "type",
                  "boolean"),
        json_pack("[s]", "package_path"));
}

json_t *mcp_schema_plugin_rmmod(void)
{
    return schema_with_properties(json_pack("{s:{s:s}}", "plugin_id", "type", "string"),
                                  json_pack("[s]", "plugin_id"));
}

uint32_t mcp_schema_shell_exec_registration_timeout_ms(void)
{
    struct shell_schema_config cfg;

    shell_schema_config_load(&cfg);
    return cfg.config_loaded ? cfg.default_timeout_ms : MCP_SHELL_EXEC_DEFAULT_TIMEOUT_MS;
}
