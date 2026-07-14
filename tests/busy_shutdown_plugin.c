#include "mcp/plugin/plugin_abi.h"

#include <stdio.h>

static unsigned int shutdown_calls;

MCP_PLUGIN_EXPORT int mcp_plugin_init(const struct mcp_plugin_host_api *host,
                                      const char *config_json,
                                      char *result_json,
                                      unsigned int result_size)
{
    (void)host;
    (void)config_json;
    snprintf(result_json, result_size, "{\"ok\":true}");
    return 0;
}

MCP_PLUGIN_EXPORT int mcp_plugin_invoke(const char *invocation_id,
                                        const char *tool_name,
                                        const char *arguments_json,
                                        char *result_json,
                                        unsigned int result_size)
{
    (void)invocation_id;
    (void)tool_name;
    (void)arguments_json;
    snprintf(result_json, result_size, "No tools registered.");
    return MCP_PLUGIN_CALL_ERROR;
}

MCP_PLUGIN_EXPORT int mcp_plugin_shutdown(void)
{
    shutdown_calls++;
    return shutdown_calls == 1 ? -1 : 0;
}
