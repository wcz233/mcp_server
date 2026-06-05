#ifndef MCP_SRC_PLUGINS_FILE_TRANSFER_FILE_TRANSFER_PLUGIN_H
#define MCP_SRC_PLUGINS_FILE_TRANSFER_FILE_TRANSFER_PLUGIN_H

#include "mcp/plugin/plugin_abi.h"

int mcp_file_transfer_plugin_init(const struct mcp_plugin_host_api *host,
                                  const char *config_json,
                                  char *result_json,
                                  unsigned int result_size);
int mcp_file_transfer_plugin_invoke(const char *invocation_id,
                                    const char *tool_name,
                                    const char *arguments_json,
                                    char *result_json,
                                    unsigned int result_size);
int mcp_file_transfer_plugin_shutdown(void);

#endif
