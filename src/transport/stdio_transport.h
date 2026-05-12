#ifndef MCP_SRC_TRANSPORT_STDIO_TRANSPORT_H
#define MCP_SRC_TRANSPORT_STDIO_TRANSPORT_H

#include <stddef.h>

#include <uv.h>

struct mcp_stdio_transport;

struct mcp_stdio_transport_config {
    size_t max_line_bytes;
};

typedef void (*mcp_stdio_line_cb)(void *arg, const char *line, size_t len);
typedef void (*mcp_stdio_exit_cb)(void *arg);

int mcp_stdio_transport_create(struct mcp_stdio_transport **out,
                               uv_loop_t *loop,
                               struct mcp_stdio_transport_config config);
void mcp_stdio_transport_destroy(struct mcp_stdio_transport *transport);

int mcp_stdio_transport_open(struct mcp_stdio_transport *transport,
                             int stdin_fd,
                             int stdout_fd);
int mcp_stdio_transport_start(struct mcp_stdio_transport *transport,
                              mcp_stdio_line_cb on_line,
                              mcp_stdio_exit_cb on_exit,
                              void *arg);
int mcp_stdio_transport_send(struct mcp_stdio_transport *transport, const char *data, size_t len);
int mcp_stdio_transport_send_str(struct mcp_stdio_transport *transport, const char *text);
void mcp_stdio_transport_close_output(struct mcp_stdio_transport *transport);

#endif
