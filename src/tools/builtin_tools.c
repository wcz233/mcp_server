#include "tools/builtin_tools.h"

#include "common/platform.h"
#include "core/server_internal.h"
#include "mcp/embedded/mep.h"
#include "plugin/plugin_manager.h"
#include "tools/schema.h"
#include "tools/shell_exec.h"
#include "tools/system_status.h"
#include "tools/tool_result.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
                      mcp_schema_empty_object(),
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
                      mcp_schema_empty_object(),
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
                      mcp_schema_empty_object(),
                      "builtin",
                      "L0",
                      "system.read",
                      true,
                      true,
                      2000,
                      tool_system_get_status) != 0)
        return -1;

    if (register_tool(registry,
                      "system.sandbox_ctl",
                      "Query and atomically update process-local shell sandbox policy overrides.",
                      mcp_schema_sandbox_ctl(),
                      "builtin",
                      "L4",
                      "system.sandbox.control",
                      false,
                      false,
                      1000,
                      mcp_tool_system_sandbox_ctl) != 0)
        return -1;

    if (register_tool(registry,
                      "system.shell_exec",
                      "Execute a host shell command with OS-level isolation, timeout and structured output; the result does not repeat the command.",
                      mcp_schema_shell_exec(),
                      "builtin",
                      "L4",
                      "system.shell",
                      false,
                      false,
                      mcp_schema_shell_exec_registration_timeout_ms(),
                      mcp_tool_system_shell_exec) != 0)
        return -1;

    if (register_tool(registry,
                      "system.shell_start",
                      "Start a host shell command as an asynchronous tracked job; the result does not repeat the command.",
                      mcp_schema_shell_start(),
                      "builtin",
                      "L4",
                      "system.shell",
                      false,
                      false,
                      1000,
                      mcp_tool_system_shell_start) != 0)
        return -1;

    if (register_tool(registry,
                      "system.shell_poll",
                      "Read status for an asynchronous shell job; the result does not repeat the command.",
                      mcp_schema_shell_job_id(),
                      "builtin",
                      "L4",
                      "system.shell",
                      true,
                      true,
                      1000,
                      mcp_tool_system_shell_poll) != 0)
        return -1;

    if (register_tool(registry,
                      "system.shell_tail",
                      "Read buffered stdout/stderr chunks for an asynchronous shell job; the result does not repeat the command.",
                      mcp_schema_shell_tail(),
                      "builtin",
                      "L4",
                      "system.shell",
                      true,
                      true,
                      1000,
                      mcp_tool_system_shell_tail) != 0)
        return -1;

    if (register_tool(registry,
                      "system.shell_wait",
                      "Refresh and immediately return the current state of an asynchronous shell job; the result does not repeat the command.",
                      mcp_schema_shell_wait(),
                      "builtin",
                      "L4",
                      "system.shell",
                      true,
                      false,
                      5000,
                      mcp_tool_system_shell_wait) != 0)
        return -1;

    if (register_tool(registry,
                      "system.shell_kill",
                      "Terminate a running asynchronous shell job using the job_id returned by system.shell_start and an optional signal (default 15); the result does not repeat the command.",
                      mcp_schema_shell_kill(),
                      "builtin",
                      "L4",
                      "system.shell",
                      false,
                      false,
                      1000,
                      mcp_tool_system_shell_kill) != 0)
        return -1;

    if (register_tool(registry,
                      "system.shell_list",
                      "List active and recently finished asynchronous shell jobs; the result does not repeat commands.",
                      mcp_schema_empty_object(),
                      "builtin",
                      "L4",
                      "system.shell",
                      true,
                      true,
                      1000,
                      mcp_tool_system_shell_list) != 0)
        return -1;
#endif

#if MCP_HAS_TOOL_GATEWAY
    if (register_tool(registry,
                      "gateway.status",
                      "Read gateway routing and transport status.",
                      mcp_schema_empty_object(),
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
                      mcp_schema_gateway_proxy(),
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
                      mcp_schema_server_list_servers(),
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
                      mcp_schema_empty_object(),
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
                      mcp_schema_empty_object(),
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
                      mcp_schema_plugin_insmod(),
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
                      mcp_schema_plugin_rmmod(),
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
                      mcp_schema_empty_object(),
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
