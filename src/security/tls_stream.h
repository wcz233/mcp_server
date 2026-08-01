#ifndef MCP_SRC_SECURITY_TLS_STREAM_H
#define MCP_SRC_SECURITY_TLS_STREAM_H

#include <stdbool.h>
#include <stddef.h>

#include <uv.h>

#include "security/tls_context.h"

struct mcp_tls_stream;

typedef void (*mcp_tls_ready_cb)(void *arg);
typedef void (*mcp_tls_data_cb)(void *arg, const unsigned char *data, size_t len);
typedef void (*mcp_tls_error_cb)(void *arg, int error_code);
typedef void (*mcp_tls_drain_cb)(void *arg);

int mcp_tls_stream_create(struct mcp_tls_stream **out,
                          struct mcp_tls_context *context,
                          bool server,
                          const char *server_name,
                          uv_stream_t *stream,
                          mcp_tls_ready_cb on_ready,
                          mcp_tls_data_cb on_data,
                          mcp_tls_error_cb on_error,
                          mcp_tls_drain_cb on_drain,
                          void *arg);
void mcp_tls_stream_destroy(struct mcp_tls_stream *stream);
int mcp_tls_stream_start(struct mcp_tls_stream *stream);
void mcp_tls_stream_stop(struct mcp_tls_stream *stream);
int mcp_tls_stream_send(struct mcp_tls_stream *stream, const void *data, size_t len);
bool mcp_tls_stream_is_ready(const struct mcp_tls_stream *stream);
const char *mcp_tls_stream_peer_fingerprint(const struct mcp_tls_stream *stream);
size_t mcp_tls_stream_pending_write_bytes(const struct mcp_tls_stream *stream);
void *mcp_tls_stream_callback_arg(const struct mcp_tls_stream *stream);

#endif
