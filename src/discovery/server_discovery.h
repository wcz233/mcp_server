#ifndef MCP_SRC_DISCOVERY_SERVER_DISCOVERY_H
#define MCP_SRC_DISCOVERY_SERVER_DISCOVERY_H

#include <stdbool.h>

#include <jansson.h>
#include <uv.h>

#include "mcp/core/server.h"

struct mcp_server;
struct mcp_server_discovery;

#define MCP_SERVER_LIST_SERVERS_TOOL "server.list_servers"
#define MCP_SERVER_DISCOVERY_OFFLINE_METHOD "mcp.discovery.offline"

int mcp_server_discovery_create(struct mcp_server_discovery **out,
                                struct mcp_server *server,
                                uv_loop_t *loop);
void mcp_server_discovery_destroy(struct mcp_server_discovery *discovery);

int mcp_server_discovery_start(struct mcp_server_discovery *discovery,
                               const struct mcp_server_discovery_config *config);
void mcp_server_discovery_close(struct mcp_server_discovery *discovery);
bool mcp_server_discovery_is_open(const struct mcp_server_discovery *discovery);

int mcp_server_discovery_list_async(struct mcp_server_discovery *discovery,
                                    const char *id_key,
                                    unsigned int wait_ms);
json_t *mcp_server_discovery_snapshot_json(struct mcp_server_discovery *discovery);
bool mcp_server_discovery_handle_offline_notification(struct mcp_server_discovery *discovery,
                                                      json_t *params);

#endif
