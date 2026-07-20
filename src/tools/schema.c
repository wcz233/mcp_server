#include "tools/schema.h"
#include "tools/shell_policy.h"

#include <jansson.h>
#include <string.h>

#define MCP_SHELL_EXEC_HARD_TIMEOUT_MS 300000u
#define MCP_SHELL_EXEC_HARD_OUTPUT_BYTES 2147483648u

static json_t *schema_with_properties(json_t *properties, json_t *required)
{
    json_t *schema = json_object();

    json_object_set_new(schema, "type", json_string("object"));
    json_object_set_new(schema, "properties", properties);
    if (required)
        json_object_set_new(schema, "required", required);

    return schema;
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

static json_t *sandbox_schema_policy_field(
    const struct mcp_shell_policy_field_descriptor *field)
{
    json_t *typed = NULL;

    switch (field->type) {
    case MCP_SHELL_POLICY_TYPE_BOOLEAN:
        return sandbox_schema_nullable_boolean();
    case MCP_SHELL_POLICY_TYPE_UINT64:
        return sandbox_schema_nullable_integer((json_int_t)field->hard_min,
                                               (json_int_t)field->hard_max);
    case MCP_SHELL_POLICY_TYPE_STRING:
        return sandbox_schema_nullable_string(field->hard_min_bytes, field->hard_max_bytes);
    case MCP_SHELL_POLICY_TYPE_MODE:
        typed = json_pack("{s:s,s:[s,s]}", "type", "string", "enum", "shell", "exec");
        return sandbox_schema_nullable(typed);
    case MCP_SHELL_POLICY_TYPE_ENV:
        typed = json_pack("{s:s,s:{s:s},s:I}",
                          "type",
                          "object",
                          "additionalProperties",
                          "type",
                          "string",
                          "maxProperties",
                          (json_int_t)MCP_SHELL_POLICY_ENV_HARD_MAX_ITEMS);
        return sandbox_schema_nullable(typed);
    }
    return NULL;
}

json_t *mcp_schema_sandbox_ctl(void)
{
    json_t *root_properties = json_object();
    json_t *override_properties = json_object();
    json_t *execution_properties = json_object();
    json_t *limit_properties = json_object();
    json_t *isolation_properties = json_object();
    json_t *action = NULL;
    json_t *schema = NULL;
    json_t *required = NULL;
    const struct mcp_shell_policy_field_descriptor *fields;
    size_t field_count;
    size_t index;

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
        sandbox_schema_add(root_properties,
                           "sandbox_enabled",
                           json_pack("{s:s}", "type", "boolean")) != 0)
        goto fail;

    fields = mcp_shell_policy_field_directory(&field_count);
    for (index = 0; index < field_count; index++) {
        const char *path = fields[index].path + strlen("defaults.");
        const char *separator = strchr(path, '.');
        json_t *properties = override_properties;
        const char *name = path;

        if (separator) {
            if ((size_t)(separator - path) == strlen("execution") &&
                strncmp(path, "execution", strlen("execution")) == 0)
                properties = execution_properties;
            else if ((size_t)(separator - path) == strlen("limits") &&
                     strncmp(path, "limits", strlen("limits")) == 0)
                properties = limit_properties;
            else if ((size_t)(separator - path) == strlen("isolation") &&
                     strncmp(path, "isolation", strlen("isolation")) == 0)
                properties = isolation_properties;
            else
                goto fail;
            name = separator + 1;
        }
        if (sandbox_schema_add(
                properties, name, sandbox_schema_policy_field(&fields[index])) != 0)
            goto fail;
    }

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
    json_decref(required);
    json_decref(schema);
    return NULL;
}

json_t *mcp_schema_shell_exec(void)
{
    return json_pack("{s:s,s:{s:{s:s,s:s},s:{s:s,s:s,s:i,s:i}},s:[s]}",
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
                     "Optional timeout override in milliseconds; the current effective ceiling may be lower.",
                     "minimum",
                     1,
                     "maximum",
                     (int)MCP_SHELL_EXEC_HARD_TIMEOUT_MS,
                     "required",
                     "command");
}

json_t *mcp_schema_shell_start(void)
{
    return json_pack("{s:s,s:{s:{s:s,s:s},s:{s:s,s:s},s:{s:s,s:s,s:i,s:i},s:{s:s,s:s,s:i,s:I},s:{s:s,s:s},s:{s:s,s:s,s:{s:s}}},s:[s]}",
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
                     "Optional job timeout in milliseconds; the current effective ceiling may be lower.",
                     "minimum",
                     1,
                     "maximum",
                     (int)MCP_SHELL_EXEC_HARD_TIMEOUT_MS,
                     "output_limit_bytes",
                     "type",
                     "integer",
                     "description",
                     "Optional per-stream buffered output limit in bytes; the current effective ceiling may be lower.",
                     "minimum",
                     256,
                     "maximum",
                     (json_int_t)MCP_SHELL_EXEC_HARD_OUTPUT_BYTES,
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
    return MCP_SHELL_EXEC_HARD_TIMEOUT_MS;
}
