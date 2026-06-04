#ifndef MCP_SRC_TRANSPORT_PEER_TRANSPORT_H
#define MCP_SRC_TRANSPORT_PEER_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <uv.h>

struct mcp_peer_transport;
struct mcp_peer_connection;

typedef void (*mcp_peer_json_frame_cb)(void *arg,
                                       unsigned int server_id,
                                       const char *data,
                                       size_t len);
typedef void (*mcp_peer_close_cb)(void *arg, unsigned int server_id);
typedef void (*mcp_peer_frame_handler_cb)(void *arg,
                                          unsigned int server_id,
                                          const unsigned char *payload,
                                          size_t len);
typedef void (*mcp_peer_event_cb)(void *arg, unsigned int server_id);
typedef int (*mcp_peer_external_send_cb)(void *arg,
                                         unsigned int server_id,
                                         const void *payload,
                                         size_t len);

int mcp_peer_transport_create(struct mcp_peer_transport **out,
                              uv_loop_t *loop,
                              size_t max_frame_bytes,
                              size_t write_high_watermark);
void mcp_peer_transport_destroy(struct mcp_peer_transport *transport);

int mcp_peer_transport_connect(struct mcp_peer_transport *transport,
                               unsigned int server_id,
                               const struct sockaddr *addr,
                               void *owner,
                               void (*on_connect)(void *owner,
                                                  struct mcp_peer_connection *conn,
                                                  int status));
int mcp_peer_transport_adopt(struct mcp_peer_transport *transport,
                             unsigned int server_id,
                             uv_tcp_t *tcp,
                             void *owner,
                             struct mcp_peer_connection **out);
void mcp_peer_transport_close(struct mcp_peer_transport *transport);

void mcp_peer_connection_close(struct mcp_peer_connection *conn);
void *mcp_peer_connection_owner(struct mcp_peer_connection *conn);
bool mcp_peer_connection_is_connected(const struct mcp_peer_connection *conn);
unsigned int mcp_peer_connection_server_id(const struct mcp_peer_connection *conn);

int mcp_peer_transport_send_frame(struct mcp_peer_transport *transport,
                                  unsigned int server_id,
                                  const void *payload,
                                  size_t len);
int mcp_peer_connection_send_frame(struct mcp_peer_connection *conn,
                                   const void *payload,
                                   size_t len,
                                   bool close_after_write);
size_t mcp_peer_transport_write_queue_bytes(struct mcp_peer_transport *transport,
                                            unsigned int server_id);
bool mcp_peer_transport_write_queue_below_high_watermark(struct mcp_peer_transport *transport,
                                                         unsigned int server_id);

void mcp_peer_transport_set_json_handler(struct mcp_peer_transport *transport,
                                         mcp_peer_json_frame_cb on_json,
                                         mcp_peer_close_cb on_close,
                                         void *arg);
void mcp_peer_transport_set_external_sender(struct mcp_peer_transport *transport,
                                            mcp_peer_external_send_cb send_cb,
                                            void *arg);
int mcp_peer_transport_dispatch_frame(struct mcp_peer_transport *transport,
                                      unsigned int server_id,
                                      const void *payload,
                                      size_t len);
void mcp_peer_transport_notify_connected(struct mcp_peer_transport *transport,
                                         unsigned int server_id);
void mcp_peer_transport_notify_closed(struct mcp_peer_transport *transport,
                                      unsigned int server_id);
int mcp_peer_transport_register_handler(struct mcp_peer_transport *transport,
                                        const char magic[4],
                                        const char *owner,
                                        mcp_peer_frame_handler_cb on_frame,
                                        mcp_peer_event_cb on_peer_connected,
                                        mcp_peer_event_cb on_peer_closed,
                                        void *arg);
int mcp_peer_transport_unregister_handler(struct mcp_peer_transport *transport,
                                          const char magic[4],
                                          const char *owner);
void mcp_peer_transport_unregister_owner(struct mcp_peer_transport *transport,
                                         const char *owner);

int mcp_peer_transport_set_capability(struct mcp_peer_transport *transport,
                                      unsigned int server_id,
                                      const char *capability,
                                      bool enabled);
bool mcp_peer_transport_has_capability(struct mcp_peer_transport *transport,
                                       unsigned int server_id,
                                       const char *capability);

#endif
