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
