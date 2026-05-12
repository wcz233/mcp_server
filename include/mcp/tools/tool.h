#ifndef MCP_TOOLS_TOOL_H
#define MCP_TOOLS_TOOL_H

#include <jansson.h>
#include <stdbool.h>
#include <stdint.h>

struct mcp_server;
struct mcp_tool_invocation;

typedef int (*mcp_tool_handler_fn)(struct mcp_server *server,
                                   const struct mcp_tool_invocation *invocation,
                                   json_t **out_result);

enum mcp_tool_route {
    MCP_TOOL_ROUTE_LOCAL_BUILTIN = 1,
    MCP_TOOL_ROUTE_LOCAL_MODULE,
    MCP_TOOL_ROUTE_EMBEDDED_ENDPOINT,
    MCP_TOOL_ROUTE_REMOTE_SERVER,
};

struct mcp_tool_descriptor {
    const char *name;
    const char *description;
    json_t *input_schema;
    const char *source;
    const char *version;
    const char *risk_level;
    const char *permission;
    bool idempotent;
    bool retryable;
    bool cancelable;
    bool enabled;
    uint32_t timeout_ms;
    enum mcp_tool_route route;
    mcp_tool_handler_fn handler;
    void *handler_data;
};

struct mcp_tool_invocation {
    const char *invocation_id;
    const char *tool_name;
    json_t *arguments;
    const struct mcp_tool_descriptor *descriptor;
};

json_t *mcp_tool_result_text(const char *text, bool is_error);
json_t *mcp_tool_result_json_text(json_t *value, bool is_error);

#endif
