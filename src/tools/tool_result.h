#ifndef MCP_SRC_TOOLS_TOOL_RESULT_H
#define MCP_SRC_TOOLS_TOOL_RESULT_H

#include <jansson.h>
#include <stdbool.h>

json_t *mcp_tool_result_text(const char *text, bool is_error);
json_t *mcp_tool_result_json_text(json_t *value, bool is_error);

#endif
