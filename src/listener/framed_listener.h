#ifndef MCP_SRC_LISTENER_FRAMED_LISTENER_H
#define MCP_SRC_LISTENER_FRAMED_LISTENER_H

#include <stdbool.h>
#include <stddef.h>

#include <uv.h>

struct mcp_framed_listener;
struct mcp_framed_connection;

struct mcp_framed_listener_config {
    size_t max_frame_bytes;
};

typedef void (*mcp_framed_message_cb)(void *arg,
                                      struct mcp_framed_connection *conn,
                                      const char *data,
                                      size_t len);
typedef void (*mcp_framed_close_cb)(void *arg, struct mcp_framed_connection *conn);
typedef bool (*mcp_framed_accept_cb)(void *arg, const struct sockaddr *peer);

int mcp_framed_listener_create(struct mcp_framed_listener **out,
                               uv_loop_t *loop,
                               struct mcp_framed_listener_config config);
void mcp_framed_listener_destroy(struct mcp_framed_listener *listener);

int mcp_framed_listener_start_pipe(struct mcp_framed_listener *listener,
                                   const char *path,
                                   mcp_framed_message_cb on_message,
                                   mcp_framed_close_cb on_close,
                                   void *arg);
int mcp_framed_listener_start_tcp(struct mcp_framed_listener *listener,
                                  const char *host,
                                  unsigned int port,
                                  mcp_framed_accept_cb on_accept,
                                  mcp_framed_message_cb on_message,
                                  mcp_framed_close_cb on_close,
                                  void *arg);
int mcp_framed_connection_send(struct mcp_framed_connection *conn,
                               const char *data,
                               size_t len);
void mcp_framed_listener_close(struct mcp_framed_listener *listener);
bool mcp_framed_listener_is_open(const struct mcp_framed_listener *listener);

#endif
