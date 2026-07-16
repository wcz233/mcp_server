#ifndef MCP_SRC_TOOLS_SHELL_EXEC_H
#define MCP_SRC_TOOLS_SHELL_EXEC_H

#include "mcp/tools/tool.h"
#include <uv.h>

struct mcp_shell_job_store;
struct mcp_shell_sandbox_control;

int mcp_shell_sandbox_control_create(struct mcp_shell_sandbox_control **out);
void mcp_shell_sandbox_control_destroy(struct mcp_shell_sandbox_control *control);
bool mcp_shell_sandbox_control_is_enabled(const struct mcp_shell_sandbox_control *control);

int mcp_shell_jobs_create(struct mcp_shell_job_store **out,
                          struct mcp_server *server,
                          uv_loop_t *loop);
void mcp_shell_jobs_shutdown(struct mcp_shell_job_store *store);
void mcp_shell_jobs_destroy(struct mcp_shell_job_store *store);

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
