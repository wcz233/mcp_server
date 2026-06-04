#include "tools/builtin_tools.h"

#include "common/platform.h"
#include "core/server_internal.h"
#include "mcp/embedded/mep.h"
#include "plugin/plugin_manager.h"
#include "tools/shell_exec.h"
#include "tools/system_status.h"
#include "tools/tool_result.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static json_t *schema_object(void)
{
    return json_pack("{s:s,s:o}", "type", "object", "properties", json_object());
}

static json_t *schema_with_properties(json_t *properties, json_t *required)
{
    json_t *schema = json_object();

    json_object_set_new(schema, "type", json_string("object"));
    json_object_set_new(schema, "properties", properties);
    if (required)
        json_object_set_new(schema, "required", required);

    return schema;
}

static int tool_system_ping(struct mcp_server *server,
                            const struct mcp_tool_invocation *invocation,
                            json_t **out_result)
{
    (void)server;
    (void)invocation;

    *out_result = mcp_tool_result_text("pong", false);
    return 0;
}

static int tool_system_get_time(struct mcp_server *server,
                                const struct mcp_tool_invocation *invocation,
                                json_t **out_result)
{
    char timestamp[48];

    (void)server;
    (void)invocation;

    if (!mcp_format_utc_now(timestamp, sizeof(timestamp))) {
        *out_result = mcp_tool_result_text("Failed to read system time.", true);
        return -1;
    }

    *out_result = mcp_tool_result_text(timestamp, false);
    return 0;
}

static int tool_system_get_status(struct mcp_server *server,
                                  const struct mcp_tool_invocation *invocation,
                                  json_t **out_result)
{
    json_t *status;

    (void)server;
    (void)invocation;

    status = mcp_system_status_json();
    if (!status) {
        *out_result = mcp_tool_result_text("Failed to read system status.", true);
        return -1;
    }

    *out_result = mcp_tool_result_json_text(status, false);
    json_decref(status);
    return 0;
}

static int tool_gateway_status(struct mcp_server *server,
                               const struct mcp_tool_invocation *invocation,
                               json_t **out_result)
{
    json_t *status;

    (void)invocation;

    status = mcp_gateway_status_json(server->gateway);
    json_object_set_new(status,
                        "stdio_transport",
                        json_string(mcp_server_stdio_enabled(server) ? "enabled" : "disabled"));
    json_object_set_new(status,
                        "udp_transport",
                        json_string(mcp_server_udp_enabled(server) ? "enabled" : "disabled"));
    json_object_set_new(status,
                        "pipe_transport",
                        json_string(mcp_server_pipe_enabled(server) ? "enabled" : "disabled"));
    json_object_set_new(status,
                        "tcp_transport",
                        json_string(mcp_server_tcp_enabled(server) ? "enabled" : "disabled"));
    *out_result = mcp_tool_result_json_text(status, false);
    json_decref(status);
    return 0;
}

static int tool_registry_list_tools(struct mcp_server *server,
                                    const struct mcp_tool_invocation *invocation,
                                    json_t **out_result)
{
    json_t *tools;

    (void)invocation;

    tools = mcp_tool_registry_internal_list(server->registry);
    *out_result = mcp_tool_result_json_text(tools, false);
    json_decref(tools);
    return 0;
}

static int tool_server_list_servers(struct mcp_server *server,
                                    const struct mcp_tool_invocation *invocation,
                                    json_t **out_result)
{
    (void)invocation;

    if (!mcp_server_discovery_enabled(server)) {
        *out_result = mcp_tool_result_text("Server discovery is not enabled.", true);
        return 0;
    }

    *out_result = mcp_tool_result_text("Server discovery is handled asynchronously.", true);
    return 0;
}

static int tool_plugin_lsmod(struct mcp_server *server,
                             const struct mcp_tool_invocation *invocation,
                             json_t **out_result)
{
    json_t *plugins;

    (void)invocation;

    plugins = mcp_plugin_manager_lsmod(server->plugin_manager);
    *out_result = mcp_tool_result_json_text(plugins, false);
    json_decref(plugins);
    return 0;
}

static int tool_plugin_insmod(struct mcp_server *server,
                              const struct mcp_tool_invocation *invocation,
                              json_t **out_result)
{
    json_t *package_path;
    json_t *enable;
    json_t *payload = NULL;
    const char *error = NULL;

    package_path = json_object_get(invocation->arguments, "package_path");
    enable = json_object_get(invocation->arguments, "enable");
    if (!json_is_string(package_path)) {
        *out_result = mcp_tool_result_text("plugin_tools.insmod requires package_path.", true);
        return 0;
    }

    if (mcp_plugin_manager_insmod(server->plugin_manager,
                                  json_string_value(package_path),
                                  !json_is_false(enable),
                                  &payload,
                                  &error) != 0) {
        *out_result = mcp_tool_result_text(error ? error : "Failed to load plugin.", true);
        return 0;
    }

    *out_result = mcp_tool_result_json_text(payload, false);
    json_decref(payload);
    return 0;
}

static int tool_plugin_rmmod(struct mcp_server *server,
                             const struct mcp_tool_invocation *invocation,
                             json_t **out_result)
{
    json_t *plugin_id;
    json_t *payload = NULL;
    const char *error = NULL;

    plugin_id = json_object_get(invocation->arguments, "plugin_id");
    if (!json_is_string(plugin_id)) {
        *out_result = mcp_tool_result_text("plugin_tools.rmmod requires plugin_id.", true);
        return 0;
    }

    if (mcp_plugin_manager_rmmod(server->plugin_manager,
                                 json_string_value(plugin_id),
                                 &payload,
                                 &error) != 0) {
        *out_result = mcp_tool_result_text(error ? error : "Failed to unload plugin.", true);
        return 0;
    }

    *out_result = mcp_tool_result_json_text(payload, false);
    json_decref(payload);
    return 0;
}

static int tool_embedded_get_protocol_info(struct mcp_server *server,
                                           const struct mcp_tool_invocation *invocation,
                                           json_t **out_result)
{
    json_t *info;

    (void)server;
    (void)invocation;

    info = json_pack("{s:i,s:i,s:[s,s,s,s],s:[i,i,i,i,i,i]}",
                     "version",
                     MCP_MEP_VERSION,
                     "sof",
                     MCP_MEP_SOF,
                     "frame_types",
                     "REQ",
                     "RESP",
                     "HELLO",
                     "CAPS",
                     "commands",
                     MCP_MEP_CMD_PING,
                     MCP_MEP_CMD_GET_DEVICE_INFO,
                     MCP_MEP_CMD_GET_STATUS,
                     MCP_MEP_CMD_READ_REGISTER,
                     MCP_MEP_CMD_WRITE_REGISTER,
                     MCP_MEP_CMD_TOOL_CALL);
    *out_result = mcp_tool_result_json_text(info, false);
    json_decref(info);
    return 0;
}

static int register_tool(struct mcp_tool_registry *registry,
                         const char *name,
                         const char *description,
                         json_t *schema,
                         const char *source,
                         const char *risk,
                         const char *permission,
                         bool idempotent,
                         bool retryable,
                         uint32_t timeout_ms,
                         mcp_tool_handler_fn handler)
{
    struct mcp_tool_descriptor descriptor;
    int rc;

    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.name = name;
    descriptor.description = description;
    descriptor.input_schema = schema;
    descriptor.source = source;
    descriptor.version = MCP_SERVER_VERSION;
    descriptor.risk_level = risk;
    descriptor.permission = permission;
    descriptor.idempotent = idempotent;
    descriptor.retryable = retryable;
    descriptor.cancelable = false;
    descriptor.enabled = true;
    descriptor.timeout_ms = timeout_ms;
    descriptor.route = MCP_TOOL_ROUTE_LOCAL_BUILTIN;
    descriptor.handler = handler;

    rc = mcp_tool_registry_register(registry, &descriptor);
    json_decref(schema);
    return rc;
}

int mcp_register_builtin_tools(struct mcp_server *server, struct mcp_tool_registry *registry)
{
    (void)server;

#if MCP_HAS_TOOL_SYSTEM
    if (register_tool(registry,
                      "system.ping",
                      "Check whether the MCP server is responsive.",
                      schema_object(),
                      "builtin",
                      "L0",
                      "system.read",
                      true,
                      true,
                      1000,
                      tool_system_ping) != 0)
        return -1;

    if (register_tool(registry,
                      "system.get_time",
                      "Get current UTC time in RFC3339 format.",
                      schema_object(),
                      "builtin",
                      "L0",
                      "system.read",
                      true,
                      true,
                      1000,
                      tool_system_get_time) != 0)
        return -1;

    if (register_tool(registry,
                      "system.get_status",
                      "Read basic host operating system and memory status.",
                      schema_object(),
                      "builtin",
                      "L0",
                      "system.read",
                      true,
                      true,
                      2000,
                      tool_system_get_status) != 0)
        return -1;

    if (register_tool(registry,
                      "system.shell_exec",
                      "Execute a host shell command with OS-level isolation, timeout and structured output.",
                      mcp_shell_exec_input_schema(),
                      "builtin",
                      "L4",
                      "system.shell",
                      false,
                      false,
                      mcp_shell_exec_registration_timeout_ms(),
                      mcp_tool_system_shell_exec) != 0)
        return -1;
#endif

#if MCP_HAS_TOOL_GATEWAY
    if (register_tool(registry,
                      "gateway.status",
                      "Read gateway routing and transport status.",
                      schema_object(),
                      "builtin",
                      "L0",
                      "gateway.read",
                      true,
                      true,
                      1000,
                      tool_gateway_status) != 0)
        return -1;

    if (register_tool(registry,
                      MCP_GATEWAY_PROXY_TOOL,
                      "Proxy a tool call to a discovered remote MCP server by stable server_id.",
                      schema_with_properties(
                          json_pack("{s:{s:s,s:s,s:i},s:{s:s,s:s},s:{s:s,s:s}}",
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
                                    "Arguments passed to the remote tool."),
                          json_pack("[s,s]", "server_id", "tool_name")),
                      "builtin",
                      "L2",
                      "gateway.proxy",
                      false,
                      false,
                      5000,
                      tool_gateway_status) != 0)
        return -1;
#endif

    if (register_tool(registry,
                      MCP_SERVER_LIST_SERVERS_TOOL,
                      "Broadcast-discover online MCP servers and return the current server register.",
                      schema_with_properties(
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
                          NULL),
                      "builtin",
                      "L0",
                      "server.read",
                      true,
                      true,
                      5000,
                      tool_server_list_servers) != 0)
        return -1;

#if MCP_HAS_TOOL_REGISTRY
    if (register_tool(registry,
                      "registry.list_tools",
                      "List the server-side tool registry with routing metadata.",
                      schema_object(),
                      "builtin",
                      "L0",
                      "registry.read",
                      true,
                      true,
                      1000,
                      tool_registry_list_tools) != 0)
        return -1;
#endif

#if MCP_HAS_TOOL_PLUGIN_MANAGER
    if (register_tool(registry,
                      "plugin_tools.lsmod",
                      "List loaded dynamic tool modules.",
                      schema_object(),
                      "builtin",
                      "L0",
                      "plugin.read",
                      true,
                      true,
                      1000,
                      tool_plugin_lsmod) != 0)
        return -1;

    if (register_tool(registry,
                      "plugin_tools.insmod",
                      "Install or load a dynamic tool module package.",
                      schema_with_properties(
                          json_pack("{s:{s:s,s:s},s:{s:s}}",
                                    "package_path",
                                    "type",
                                    "string",
                                    "description",
                                    "Path to a module package.",
                                    "enable",
                                    "type",
                                    "boolean"),
                          json_pack("[s]", "package_path")),
                      "builtin",
                      "L2",
                      "plugin.write",
                      false,
                      false,
                      5000,
                      tool_plugin_insmod) != 0)
        return -1;

    if (register_tool(registry,
                      "plugin_tools.rmmod",
                      "Unload a dynamic tool module.",
                      schema_with_properties(
                          json_pack("{s:{s:s}}", "plugin_id", "type", "string"),
                          json_pack("[s]", "plugin_id")),
                      "builtin",
                      "L2",
                      "plugin.write",
                      false,
                      false,
                      5000,
                      tool_plugin_rmmod) != 0)
        return -1;
#endif

#if MCP_HAS_TOOL_EMBEDDED
    if (register_tool(registry,
                      "embedded.get_protocol_info",
                      "Return MCP Embedded Protocol constants supported by this host build.",
                      schema_object(),
                      "builtin",
                      "L0",
                      "embedded.read",
                      true,
                      true,
                      1000,
                      tool_embedded_get_protocol_info) != 0)
        return -1;
#endif

    return 0;
}
