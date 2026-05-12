#ifndef MCP_SRC_CORE_SERVER_INTERNAL_H
#define MCP_SRC_CORE_SERVER_INTERNAL_H

#include "core/in_flight.h"
#include "mcp/core/server.h"
#include "mcp/gateway/gateway.h"
#include "mcp/registry/tool_registry.h"
#include "transport/stdio_transport.h"

enum mcp_session_state {
    MCP_SESSION_NOT_INITIALIZED = 0,
    MCP_SESSION_AWAIT_CLIENT_INITIALIZED,
    MCP_SESSION_INITIALIZED,
};

struct mcp_server {
    uv_loop_t *loop;
    struct mcp_server_config config;

    struct mcp_stdio_transport *stdio;
    uv_async_t core_async;
    enum mcp_session_state session_state;
    bool shutting_down;

    struct mcp_message_node *queue_head;
    struct mcp_message_node *queue_tail;

    struct mcp_in_flight_map in_flight;
    struct mcp_tool_registry *registry;
    struct mcp_gateway *gateway;

    json_t *session_tool_snapshot;
};

int mcp_server_send_result(struct mcp_server *server, json_t *id, json_t *result);
int mcp_server_send_error(struct mcp_server *server, json_t *id, int code, const char *message);

void mcp_server_complete_async_ok(struct mcp_server *server,
                                  const char *id_key,
                                  json_t *result);

#endif
