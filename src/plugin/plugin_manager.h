#ifndef MCP_SRC_PLUGIN_PLUGIN_MANAGER_H
#define MCP_SRC_PLUGIN_PLUGIN_MANAGER_H

#include <stdbool.h>

#include <jansson.h>

#include "mcp/plugin/plugin_abi.h"

struct mcp_plugin_manager;
struct mcp_server;
struct mcp_tool_descriptor;

struct mcp_builtin_plugin_descriptor {
    const char *plugin_id;
    const char *path;
    mcp_plugin_init_fn init;
    mcp_plugin_invoke_fn invoke;
    mcp_plugin_shutdown_fn shutdown;
};

int mcp_plugin_manager_create(struct mcp_plugin_manager **out, struct mcp_server *server);
void mcp_plugin_manager_destroy(struct mcp_plugin_manager *manager);

int mcp_plugin_manager_register_builtin(
    struct mcp_plugin_manager *manager,
    const struct mcp_builtin_plugin_descriptor *descriptor,
    const char *config_json);

int mcp_plugin_manager_insmod(struct mcp_plugin_manager *manager,
                              const char *package_path,
                              bool enable,
                              json_t **out_payload,
                              const char **out_error);
int mcp_plugin_manager_rmmod(struct mcp_plugin_manager *manager,
                             const char *plugin_id,
                             json_t **out_payload,
                             const char **out_error);
json_t *mcp_plugin_manager_lsmod(struct mcp_plugin_manager *manager);

int mcp_plugin_manager_invoke(struct mcp_plugin_manager *manager,
                              const struct mcp_tool_descriptor *descriptor,
                              const char *id_key,
                              const char *invocation_id,
                              const char *tool_name,
                              json_t *arguments,
                              json_t **out_result);

#endif
