#ifndef MCP_CORE_SERVER_H
#define MCP_CORE_SERVER_H

#include <stdbool.h>
#include <stddef.h>

#include <uv.h>

struct mcp_server;

struct mcp_server_config {
    bool strict_initialized_notification;
    size_t max_line_bytes;
};

int mcp_server_init(struct mcp_server **out, uv_loop_t *loop, struct mcp_server_config cfg);
void mcp_server_destroy(struct mcp_server *server);

int mcp_server_start_stdio(struct mcp_server *server, int stdin_fd, int stdout_fd);
int mcp_server_start_udp(struct mcp_server *server, const char *bind_host, unsigned int bind_port);
uv_loop_t *mcp_server_loop(struct mcp_server *server);

#endif
