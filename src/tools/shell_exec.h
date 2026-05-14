#ifndef MCP_SRC_TOOLS_SHELL_EXEC_H
#define MCP_SRC_TOOLS_SHELL_EXEC_H

#include "mcp/tools/tool.h"
#include <stdint.h>

json_t *mcp_shell_exec_input_schema(void);
uint32_t mcp_shell_exec_registration_timeout_ms(void);
int mcp_tool_system_shell_exec(struct mcp_server *server,
                               const struct mcp_tool_invocation *invocation,
                               json_t **out_result);

#endif
