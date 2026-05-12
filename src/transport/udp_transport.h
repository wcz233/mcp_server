#ifndef MCP_SRC_TRANSPORT_UDP_TRANSPORT_H
#define MCP_SRC_TRANSPORT_UDP_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>

#include <uv.h>

struct mcp_udp_transport;

struct mcp_udp_transport_config {
    size_t max_datagram_bytes;
};

typedef void (*mcp_udp_datagram_cb)(void *arg,
                                    const char *data,
                                    size_t len,
                                    const struct sockaddr *peer);
typedef void (*mcp_udp_error_cb)(void *arg, int status);

int mcp_udp_transport_create(struct mcp_udp_transport **out,
                             uv_loop_t *loop,
                             struct mcp_udp_transport_config config);
void mcp_udp_transport_destroy(struct mcp_udp_transport *transport);

int mcp_udp_transport_open(struct mcp_udp_transport *transport,
                           const char *bind_host,
                           unsigned int bind_port);
int mcp_udp_transport_start(struct mcp_udp_transport *transport,
                            mcp_udp_datagram_cb on_datagram,
                            mcp_udp_error_cb on_error,
                            void *arg);
int mcp_udp_transport_send(struct mcp_udp_transport *transport,
                           const char *data,
                           size_t len,
                           const struct sockaddr *peer);
void mcp_udp_transport_close(struct mcp_udp_transport *transport);
bool mcp_udp_transport_is_open(const struct mcp_udp_transport *transport);

#endif
