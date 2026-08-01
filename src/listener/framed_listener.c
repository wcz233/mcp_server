#include "listener/framed_listener.h"

#if MCP_TCP_SECURITY_MTLS
#include "security/tls_stream.h"
#endif

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum listener_kind {
    LISTENER_KIND_NONE = 0,
    LISTENER_KIND_PIPE,
    LISTENER_KIND_TCP,
};

struct write_req {
    uv_write_t req;
    uv_buf_t buf;
};

struct mcp_framed_connection {
    struct mcp_framed_listener *listener;
    enum listener_kind kind;
    uv_pipe_t pipe;
    uv_tcp_t tcp;
    uv_stream_t *stream;
#if MCP_TCP_SECURITY_MTLS
    struct mcp_tls_stream *tls;
#endif
    bool closing;

    char *rx_buf;
    size_t rx_len;
    size_t rx_cap;

    struct mcp_framed_connection *next;
};

struct mcp_framed_listener {
    uv_loop_t *loop;
    struct mcp_framed_listener_config config;
    enum listener_kind kind;

    uv_pipe_t pipe_server;
    uv_tcp_t tcp_server;
    uv_stream_t *server_stream;
    bool initialized;
    bool open;
    bool closing;
    bool destroy_on_close;

    struct mcp_framed_connection *clients;
    mcp_framed_accept_cb on_accept;
    mcp_framed_message_cb on_message;
    mcp_framed_close_cb on_close;
    void *cb_arg;
};

static void maybe_release_listener(struct mcp_framed_listener *listener);

static uint32_t read_u32_be(const char *data)
{
    const unsigned char *bytes = (const unsigned char *)data;

    return ((uint32_t)bytes[0] << 24) |
           ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) |
           (uint32_t)bytes[3];
}

static void write_u32_be(char *data, uint32_t value)
{
    data[0] = (char)((value >> 24) & 0xffu);
    data[1] = (char)((value >> 16) & 0xffu);
    data[2] = (char)((value >> 8) & 0xffu);
    data[3] = (char)(value & 0xffu);
}

static void connection_unlink(struct mcp_framed_connection *conn)
{
    struct mcp_framed_connection **current = &conn->listener->clients;

    while (*current) {
        if (*current == conn) {
            *current = conn->next;
            conn->next = NULL;
            return;
        }
        current = &(*current)->next;
    }
}

static void connection_close_cb(uv_handle_t *handle)
{
    struct mcp_framed_connection *conn = handle->data;
    struct mcp_framed_listener *listener;

    if (!conn)
        return;

    listener = conn->listener;
    connection_unlink(conn);
    if (listener->on_close)
        listener->on_close(listener->cb_arg, conn);

    free(conn->rx_buf);
#if MCP_TCP_SECURITY_MTLS
    mcp_tls_stream_destroy(conn->tls);
#endif
    free(conn);
    maybe_release_listener(listener);
}

static void connection_close_with_cb(struct mcp_framed_connection *conn)
{
    if (!conn || conn->closing)
        return;

    conn->closing = true;
#if MCP_TCP_SECURITY_MTLS
    mcp_tls_stream_stop(conn->tls);
#endif
    if (conn->stream)
        uv_read_stop(conn->stream);
    if (conn->stream && !uv_is_closing((uv_handle_t *)conn->stream)) {
        conn->stream->data = conn;
        uv_close((uv_handle_t *)conn->stream, connection_close_cb);
    }
}

static void server_close_cb(uv_handle_t *handle)
{
    struct mcp_framed_listener *listener = handle->data;

    if (!listener)
        return;

    listener->initialized = false;
    listener->open = false;
    maybe_release_listener(listener);
}

static void maybe_release_listener(struct mcp_framed_listener *listener)
{
    if (!listener || !listener->destroy_on_close)
        return;
    if (listener->initialized || listener->clients)
        return;

    free(listener);
}

static void alloc_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf)
{
    (void)handle;
    (void)suggested_size;

    buf->base = malloc(64 * 1024);
    buf->len = buf->base ? 64 * 1024 : 0;
}

static int rx_reserve(struct mcp_framed_connection *conn, size_t want)
{
    size_t need = conn->rx_len + want;
    size_t cap = conn->rx_cap;
    char *next;

    if (need <= cap)
        return 0;

    if (cap == 0)
        cap = 4096;
    while (cap < need)
        cap *= 2;

    next = realloc(conn->rx_buf, cap);
    if (!next)
        return -1;

    conn->rx_buf = next;
    conn->rx_cap = cap;
    return 0;
}

static void rx_consume(struct mcp_framed_connection *conn, size_t count)
{
    if (count >= conn->rx_len) {
        conn->rx_len = 0;
        return;
    }

    memmove(conn->rx_buf, conn->rx_buf + count, conn->rx_len - count);
    conn->rx_len -= count;
}

static void process_rx(struct mcp_framed_connection *conn)
{
    while (conn->rx_len >= 4) {
        uint32_t frame_len = read_u32_be(conn->rx_buf);
        size_t total_len;

        if (frame_len == 0 || frame_len > conn->listener->config.max_frame_bytes) {
            connection_close_with_cb(conn);
            return;
        }

        total_len = (size_t)frame_len + 4;
        if (conn->rx_len < total_len)
            return;

        if (conn->listener->on_message)
            conn->listener->on_message(conn->listener->cb_arg,
                                       conn,
                                       conn->rx_buf + 4,
                                       (size_t)frame_len);
        rx_consume(conn, total_len);
    }
}

static void append_plaintext(struct mcp_framed_connection *conn,
                             const char *data,
                             size_t len)
{
    if (rx_reserve(conn, len) != 0) {
        connection_close_with_cb(conn);
        return;
    }

    memcpy(conn->rx_buf + conn->rx_len, data, len);
    conn->rx_len += len;
    process_rx(conn);
}

static void read_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
    struct mcp_framed_connection *conn = stream->data;

    if (nread < 0) {
        free(buf->base);
        connection_close_with_cb(conn);
        return;
    }

    if (nread == 0) {
        free(buf->base);
        return;
    }

    append_plaintext(conn, buf->base, (size_t)nread);
    free(buf->base);
}

#if MCP_TCP_SECURITY_MTLS
static void tls_ready_cb(void *arg)
{
    (void)arg;
}

static void tls_data_cb(void *arg, const unsigned char *data, size_t len)
{
    append_plaintext(arg, (const char *)data, len);
}

static void tls_error_cb(void *arg, int error_code)
{
    (void)error_code;
    connection_close_with_cb(arg);
}
#endif

static void write_cb(uv_write_t *req, int status)
{
    struct write_req *wr = (struct write_req *)req;
    struct mcp_framed_connection *conn = req->handle->data;

    free(wr->buf.base);
    free(wr);

    if (status < 0)
        connection_close_with_cb(conn);
}

static struct mcp_framed_connection *connection_create(struct mcp_framed_listener *listener)
{
    struct mcp_framed_connection *conn = calloc(1, sizeof(*conn));

    if (!conn)
        return NULL;

    conn->listener = listener;
    conn->kind = listener->kind;
    if (conn->kind == LISTENER_KIND_PIPE) {
        if (uv_pipe_init(listener->loop, &conn->pipe, 0) != 0) {
            free(conn);
            return NULL;
        }
        conn->stream = (uv_stream_t *)&conn->pipe;
    } else {
        if (uv_tcp_init(listener->loop, &conn->tcp) != 0) {
            free(conn);
            return NULL;
        }
        conn->stream = (uv_stream_t *)&conn->tcp;
    }

    conn->stream->data = conn;
    return conn;
}

static void on_connection(uv_stream_t *server_stream, int status)
{
    struct mcp_framed_listener *listener = server_stream->data;
    struct mcp_framed_connection *conn;

    if (status < 0)
        return;

    conn = connection_create(listener);
    if (!conn)
        return;

    if (uv_accept(server_stream, conn->stream) != 0) {
        if (!uv_is_closing((uv_handle_t *)conn->stream))
            uv_close((uv_handle_t *)conn->stream, connection_close_cb);
        return;
    }

    if (conn->kind == LISTENER_KIND_TCP && listener->on_accept) {
        struct sockaddr_storage peer;
        int peer_len = sizeof(peer);

        memset(&peer, 0, sizeof(peer));
        if (uv_tcp_getpeername(&conn->tcp, (struct sockaddr *)&peer, &peer_len) != 0 ||
            !listener->on_accept(listener->cb_arg, (const struct sockaddr *)&peer)) {
            connection_close_with_cb(conn);
            return;
        }
    }

    conn->next = listener->clients;
    listener->clients = conn;

    if (conn->kind == LISTENER_KIND_TCP) {
#if MCP_TCP_SECURITY_MTLS
        if (listener->config.tls_context) {
            if (mcp_tls_stream_create(&conn->tls,
                                      listener->config.tls_context,
                                      true,
                                      NULL,
                                      conn->stream,
                                      tls_ready_cb,
                                      tls_data_cb,
                                      tls_error_cb,
                                      NULL,
                                      conn) != 0 ||
                mcp_tls_stream_start(conn->tls) != 0)
                connection_close_with_cb(conn);
        } else
#endif
        {
            if (uv_read_start(conn->stream, alloc_cb, read_cb) != 0)
                connection_close_with_cb(conn);
        }
    } else if (uv_read_start(conn->stream, alloc_cb, read_cb) != 0) {
        connection_close_with_cb(conn);
    }
}

int mcp_framed_listener_create(struct mcp_framed_listener **out,
                               uv_loop_t *loop,
                               struct mcp_framed_listener_config config)
{
    struct mcp_framed_listener *listener;

    *out = NULL;
    if (!loop || config.max_frame_bytes == 0)
        return -1;

    listener = calloc(1, sizeof(*listener));
    if (!listener)
        return -1;

    listener->loop = loop;
    listener->config = config;
    *out = listener;
    return 0;
}

void mcp_framed_listener_destroy(struct mcp_framed_listener *listener)
{
    if (!listener)
        return;

    if (listener->initialized || listener->clients) {
        listener->destroy_on_close = true;
        mcp_framed_listener_close(listener);
        return;
    }

    free(listener);
}

int mcp_framed_listener_start_pipe(struct mcp_framed_listener *listener,
                                   const char *path,
                                   mcp_framed_message_cb on_message,
                                   mcp_framed_close_cb on_close,
                                   void *arg)
{
    if (!listener || !path || listener->initialized)
        return -1;

    if (uv_pipe_init(listener->loop, &listener->pipe_server, 0) != 0)
        return -1;

    listener->kind = LISTENER_KIND_PIPE;
    listener->server_stream = (uv_stream_t *)&listener->pipe_server;
    listener->server_stream->data = listener;
    listener->initialized = true;
    listener->on_message = on_message;
    listener->on_close = on_close;
    listener->cb_arg = arg;

    if (uv_pipe_bind(&listener->pipe_server, path) != 0)
        return -1;
    if (uv_listen(listener->server_stream, 64, on_connection) != 0)
        return -1;

    listener->open = true;
    return 0;
}

int mcp_framed_listener_start_tcp(struct mcp_framed_listener *listener,
                                  const char *host,
                                  unsigned int port,
                                  mcp_framed_accept_cb on_accept,
                                  mcp_framed_message_cb on_message,
                                  mcp_framed_close_cb on_close,
                                  void *arg)
{
    struct sockaddr_storage addr;
    struct sockaddr_in addr4;
    struct sockaddr_in6 addr6;

    if (!listener || !host || listener->initialized || port > 65535)
        return -1;

    memset(&addr, 0, sizeof(addr));
    if (uv_ip4_addr(host, (int)port, &addr4) == 0) {
        memcpy(&addr, &addr4, sizeof(addr4));
    } else if (uv_ip6_addr(host, (int)port, &addr6) == 0) {
        memcpy(&addr, &addr6, sizeof(addr6));
    } else {
        return -1;
    }

    if (uv_tcp_init(listener->loop, &listener->tcp_server) != 0)
        return -1;

    listener->kind = LISTENER_KIND_TCP;
    listener->server_stream = (uv_stream_t *)&listener->tcp_server;
    listener->server_stream->data = listener;
    listener->initialized = true;
    listener->on_accept = on_accept;
    listener->on_message = on_message;
    listener->on_close = on_close;
    listener->cb_arg = arg;

    if (uv_tcp_bind(&listener->tcp_server, (const struct sockaddr *)&addr, 0) != 0)
        return -1;
    if (uv_listen(listener->server_stream, 128, on_connection) != 0)
        return -1;

    listener->open = true;
    return 0;
}

int mcp_framed_connection_send(struct mcp_framed_connection *conn,
                               const char *data,
                               size_t len)
{
    struct write_req *wr;

    if (!conn || conn->closing || !data || len == 0 || len > UINT32_MAX)
        return -1;
#ifdef _WIN32
    if (len + 4 > ULONG_MAX)
        return -1;
#endif

    if (conn->kind == LISTENER_KIND_TCP) {
#if MCP_TCP_SECURITY_MTLS
        if (conn->tls) {
            char *frame = malloc(len + 4);

            if (!frame)
                return -1;
            write_u32_be(frame, (uint32_t)len);
            memcpy(frame + 4, data, len);
            if (mcp_tls_stream_send(conn->tls, frame, len + 4) != 0) {
                free(frame);
                return -1;
            }
            free(frame);
            return 0;
        }
#endif
    }

    wr = calloc(1, sizeof(*wr));
    if (!wr)
        return -1;

    wr->buf.base = malloc(len + 4);
    if (!wr->buf.base) {
        free(wr);
        return -1;
    }

    write_u32_be(wr->buf.base, (uint32_t)len);
    memcpy(wr->buf.base + 4, data, len);
#ifdef _WIN32
    wr->buf.len = (unsigned long)(len + 4);
#else
    wr->buf.len = len + 4;
#endif

    if (uv_write(&wr->req, conn->stream, &wr->buf, 1, write_cb) != 0) {
        free(wr->buf.base);
        free(wr);
        return -1;
    }

    return 0;
}

const char *mcp_framed_connection_peer_fingerprint(
    const struct mcp_framed_connection *conn)
{
#if MCP_TCP_SECURITY_MTLS
    return conn ? mcp_tls_stream_peer_fingerprint(conn->tls) : NULL;
#else
    (void)conn;
    return NULL;
#endif
}

void mcp_framed_listener_close(struct mcp_framed_listener *listener)
{
    struct mcp_framed_connection *conn;

    if (!listener || listener->closing)
        return;

    listener->closing = true;
    conn = listener->clients;
    while (conn) {
        struct mcp_framed_connection *next = conn->next;
        connection_close_with_cb(conn);
        conn = next;
    }

    if (listener->initialized &&
        listener->server_stream &&
        !uv_is_closing((uv_handle_t *)listener->server_stream))
        uv_close((uv_handle_t *)listener->server_stream, server_close_cb);
}

bool mcp_framed_listener_is_open(const struct mcp_framed_listener *listener)
{
    return listener && listener->open && !listener->closing;
}
