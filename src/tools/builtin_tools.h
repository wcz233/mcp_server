#ifndef MCP_SRC_TOOLS_BUILTIN_TOOLS_H
#define MCP_SRC_TOOLS_BUILTIN_TOOLS_H

#include "mcp/core/server.h"
#include "mcp/registry/tool_registry.h"

int mcp_register_builtin_tools(struct mcp_server *server, struct mcp_tool_registry *registry);

#endif
