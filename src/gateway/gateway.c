#include "mcp/gateway/gateway.h"

#include "core/server_internal.h"
#include "discovery/server_discovery.h"
#include "mcp/registry/tool_registry.h"
#include "plugin/plugin_manager.h"
#include "protocol/jsonrpc.h"
#include "tools/tool_result.h"

#include <jansson.h>
#include <stdlib.h>
#include <string.h>

struct mcp_gateway {
    struct mcp_tool_registry *registry;
    unsigned long calls_total;
    unsigned long local_calls;
    unsigned long remote_calls;
    unsigned long rejected_calls;
};

static bool json_integer_in_uint_range(json_t *value, unsigned int *out)
{
    json_int_t raw;

    if (!json_is_integer(value))
        return false;

    raw = json_integer_value(value);
    if (raw < 0 || raw > 4294967295LL)
        return false;

    *out = (unsigned int)raw;
    return true;
}

static bool snapshot_contains_tool(const json_t *snapshot, const char *tool_name)
{
    json_t *tools;
    json_t *tool;
    size_t index;

    if (!snapshot)
        return false;

    tools = json_object_get(snapshot, "tools");
    if (!json_is_array(tools))
        return false;

    json_array_foreach(tools, index, tool) {
        json_t *name = json_object_get(tool, "name");
        if (json_is_string(name) && strcmp(json_string_value(name), tool_name) == 0)
            return true;
    }

    return false;
}

static int gateway_proxy_tool_call(struct mcp_gateway *gateway,
                                   struct mcp_server *server,
                                   const char *id_key,
                                   json_t *arguments,
                                   json_t **out_result)
{
    json_t *server_id_value;
    json_t *tool_name_value;
    json_t *tool_args;
    bool created_tool_args = false;
    unsigned int server_id;
    const char *tool_name;
    enum mcp_discovery_proxy_kind kind = MCP_DISCOVERY_PROXY_TOOL;
    int rc;

    server_id_value = json_object_get(arguments, "server_id");
    tool_name_value = json_object_get(arguments, "tool_name");
    tool_args = json_object_get(arguments, "args");
    if (!tool_args)
        tool_args = json_object_get(arguments, "arguments");
    if (!tool_args) {
        tool_args = json_object();
        created_tool_args = true;
    }

    if (!json_integer_in_uint_range(server_id_value, &server_id) ||
        server_id == 0 ||
        !json_is_string(tool_name_value) ||
        !json_is_object(tool_args)) {
        gateway->rejected_calls++;
        *out_result = mcp_tool_result_text(
            "gateway.proxy_tool requires server_id > 0, tool_name, and object args.",
            true);
        if (created_tool_args)
            json_decref(tool_args);
        return MCP_GATEWAY_TOOL_ERROR;
    }

    tool_name = json_string_value(tool_name_value);
    if (strcmp(tool_name, "tools_list") == 0)
        kind = MCP_DISCOVERY_PROXY_TOOLS_LIST;
    else if (!mcp_server_discovery_server_has_tool(server->discovery, server_id, tool_name)) {
        gateway->rejected_calls++;
        *out_result = mcp_tool_result_text(
            "Remote tool is not cached for this server. Call gateway.proxy_tool with tool_name=\"tools_list\" first.",
            true);
        if (created_tool_args)
            json_decref(tool_args);
        return MCP_GATEWAY_TOOL_ERROR;
    }

    rc = mcp_server_discovery_call_remote_tool(server->discovery,
                                               id_key,
                                               server_id,
                                               kind,
                                               tool_name,
                                               tool_args);
    if (created_tool_args)
        json_decref(tool_args);

    if (rc != 0) {
        gateway->rejected_calls++;
        *out_result = mcp_tool_result_text("Remote server is not available for proxy calls.", true);
        return MCP_GATEWAY_TOOL_ERROR;
    }

    gateway->remote_calls++;
    return MCP_GATEWAY_PENDING;
}

int mcp_gateway_create(struct mcp_gateway **out, struct mcp_tool_registry *registry)
{
    struct mcp_gateway *gateway;

    *out = NULL;
    gateway = calloc(1, sizeof(*gateway));
    if (!gateway)
        return -1;

    gateway->registry = registry;
    *out = gateway;
    return 0;
}

void mcp_gateway_destroy(struct mcp_gateway *gateway)
{
    free(gateway);
}

int mcp_gateway_call(struct mcp_gateway *gateway,
                     struct mcp_server *server,
                     const char *id_key,
                     const char *invocation_id,
                     const char *tool_name,
                     json_t *arguments,
                     const json_t *session_snapshot,
                     json_t **out_result,
                     json_t **out_error)
{
    const struct mcp_tool_descriptor *descriptor;
    struct mcp_tool_invocation invocation;
    int rc;

    *out_result = NULL;
    *out_error = NULL;
    gateway->calls_total++;

    if (!snapshot_contains_tool(session_snapshot, tool_name)) {
        gateway->rejected_calls++;
        *out_result = mcp_tool_result_text(
            "Tool is not visible in current session snapshot. Call tools/list to refresh this session.",
            true);
        return MCP_GATEWAY_TOOL_ERROR;
    }

    if (strcmp(tool_name, MCP_GATEWAY_PROXY_TOOL) == 0)
        return gateway_proxy_tool_call(gateway,
                                       server,
                                       id_key,
                                       arguments,
                                       out_result);

    descriptor = mcp_tool_registry_find(gateway->registry, tool_name);
    if (!descriptor) {
        gateway->rejected_calls++;
        *out_result = mcp_tool_result_text("Tool is not loaded or has been disabled.", true);
        return MCP_GATEWAY_TOOL_ERROR;
    }

    if (descriptor->route == MCP_TOOL_ROUTE_LOCAL_MODULE) {
        rc = mcp_plugin_manager_invoke(server->plugin_manager,
                                       descriptor,
                                       id_key,
                                       invocation_id,
                                       tool_name,
                                       arguments,
                                       out_result);
        if (rc == MCP_PLUGIN_CALL_PENDING) {
            gateway->local_calls++;
            return MCP_GATEWAY_PENDING;
        }
        if (rc == MCP_PLUGIN_CALL_OK) {
            gateway->local_calls++;
            return MCP_GATEWAY_OK;
        }
        gateway->rejected_calls++;
        return MCP_GATEWAY_TOOL_ERROR;
    }

    if (descriptor->route != MCP_TOOL_ROUTE_LOCAL_BUILTIN || !descriptor->handler) {
        gateway->rejected_calls++;
        *out_result = mcp_tool_result_text(
            "This route is declared but not implemented in the initial server build.",
            true);
        return MCP_GATEWAY_TOOL_ERROR;
    }

    invocation.invocation_id = invocation_id;
    invocation.tool_name = tool_name;
    invocation.arguments = arguments;
    invocation.descriptor = descriptor;

    if (descriptor->handler(server, &invocation, out_result) != 0) {
        gateway->rejected_calls++;
        if (!*out_result)
            *out_result = mcp_tool_result_text("Tool execution failed.", true);
        return MCP_GATEWAY_TOOL_ERROR;
    }

    gateway->local_calls++;
    return MCP_GATEWAY_OK;
}

json_t *mcp_gateway_status_json(struct mcp_gateway *gateway)
{
    return json_pack("{s:s,s:I,s:I,s:I,s:I}",
                     "state",
                     "ready",
                     "calls_total",
                     (json_int_t)gateway->calls_total,
                     "local_calls",
                     (json_int_t)gateway->local_calls,
                     "remote_calls",
                     (json_int_t)gateway->remote_calls,
                     "rejected_calls",
                     (json_int_t)gateway->rejected_calls);
}
