#ifndef MCP_SRC_TOOLS_SCHEMA_H
#define MCP_SRC_TOOLS_SCHEMA_H

#include <jansson.h>
#include <stdint.h>

json_t *mcp_schema_empty_object(void);
json_t *mcp_schema_sandbox_ctl(void);
json_t *mcp_schema_shell_exec(void);
json_t *mcp_schema_shell_start(void);
json_t *mcp_schema_shell_job_id(void);
json_t *mcp_schema_shell_tail(void);
json_t *mcp_schema_shell_wait(void);
json_t *mcp_schema_shell_kill(void);
json_t *mcp_schema_gateway_proxy(void);
json_t *mcp_schema_server_list_servers(void);
json_t *mcp_schema_plugin_insmod(void);
json_t *mcp_schema_plugin_rmmod(void);
uint32_t mcp_schema_shell_exec_registration_timeout_ms(void);

#endif
