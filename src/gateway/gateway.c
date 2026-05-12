#include "mcp/gateway/gateway.h"

#include "mcp/registry/tool_registry.h"
#include "protocol/jsonrpc.h"

#include <jansson.h>
#include <stdlib.h>
#include <string.h>

struct mcp_gateway {
    struct mcp_tool_registry *registry;
    unsigned long calls_total;
    unsigned long local_calls;
    unsigned long rejected_calls;
};

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
                     const char *invocation_id,
                     const char *tool_name,
                     json_t *arguments,
                     const json_t *session_snapshot,
                     json_t **out_result,
                     json_t **out_error)
{
    const struct mcp_tool_descriptor *descriptor;
    struct mcp_tool_invocation invocation;

    *out_result = NULL;
    *out_error = NULL;
    gateway->calls_total++;

    if (!snapshot_contains_tool(session_snapshot, tool_name)) {
        gateway->rejected_calls++;
        *out_result = mcp_tool_result_text(
            "Tool is not visible in current session snapshot. Refresh tools/list or start a new session.",
            true);
        return MCP_GATEWAY_TOOL_ERROR;
    }

    descriptor = mcp_tool_registry_find(gateway->registry, tool_name);
    if (!descriptor) {
        gateway->rejected_calls++;
        *out_result = mcp_tool_result_text("Tool is not loaded or has been disabled.", true);
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
    return json_pack("{s:s,s:I,s:I,s:I}",
                     "state",
                     "ready",
                     "calls_total",
                     (json_int_t)gateway->calls_total,
                     "local_calls",
                     (json_int_t)gateway->local_calls,
                     "rejected_calls",
                     (json_int_t)gateway->rejected_calls);
}
