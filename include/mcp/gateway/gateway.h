#ifndef MCP_GATEWAY_GATEWAY_H
#define MCP_GATEWAY_GATEWAY_H

#include <jansson.h>

#include "mcp/tools/tool.h"

struct mcp_server;
struct mcp_tool_registry;

enum mcp_gateway_status {
    MCP_GATEWAY_OK = 0,
    MCP_GATEWAY_TOOL_ERROR = 1,
    MCP_GATEWAY_PROTOCOL_ERROR = -1,
    MCP_GATEWAY_PENDING = 2,
};

#define MCP_GATEWAY_PROXY_TOOL "gateway.proxy_tool"

struct mcp_gateway;

int mcp_gateway_create(struct mcp_gateway **out, struct mcp_tool_registry *registry);
void mcp_gateway_destroy(struct mcp_gateway *gateway);

int mcp_gateway_call(struct mcp_gateway *gateway,
                     struct mcp_server *server,
                     const char *id_key,
                     const char *invocation_id,
                     const char *tool_name,
                     json_t *arguments,
                     const json_t *session_snapshot,
                     json_t **out_result,
                     json_t **out_error);

json_t *mcp_gateway_status_json(struct mcp_gateway *gateway);

#endif
