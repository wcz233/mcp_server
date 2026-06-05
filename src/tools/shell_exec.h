#ifndef MCP_SRC_TOOLS_SHELL_EXEC_H
#define MCP_SRC_TOOLS_SHELL_EXEC_H

#include "mcp/tools/tool.h"
#include <stdint.h>
#include <uv.h>

struct mcp_shell_job_store;

int mcp_shell_jobs_create(struct mcp_shell_job_store **out,
                          struct mcp_server *server,
                          uv_loop_t *loop);
void mcp_shell_jobs_shutdown(struct mcp_shell_job_store *store);
void mcp_shell_jobs_destroy(struct mcp_shell_job_store *store);

json_t *mcp_shell_exec_input_schema(void);
uint32_t mcp_shell_exec_registration_timeout_ms(void);
int mcp_tool_system_shell_exec(struct mcp_server *server,
                               const struct mcp_tool_invocation *invocation,
                               json_t **out_result);
int mcp_tool_system_shell_start(struct mcp_server *server,
                                const struct mcp_tool_invocation *invocation,
                                json_t **out_result);
int mcp_tool_system_shell_poll(struct mcp_server *server,
                               const struct mcp_tool_invocation *invocation,
                               json_t **out_result);
int mcp_tool_system_shell_tail(struct mcp_server *server,
                               const struct mcp_tool_invocation *invocation,
                               json_t **out_result);
int mcp_tool_system_shell_wait(struct mcp_server *server,
                               const struct mcp_tool_invocation *invocation,
                               json_t **out_result);
int mcp_tool_system_shell_kill(struct mcp_server *server,
                               const struct mcp_tool_invocation *invocation,
                               json_t **out_result);
int mcp_tool_system_shell_list(struct mcp_server *server,
                               const struct mcp_tool_invocation *invocation,
                               json_t **out_result);

#endif
