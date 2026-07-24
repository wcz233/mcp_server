#include "transport/peer_transport.h"

#include "common/platform.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct peer_write_req {
    uv_write_t req;
    uv_buf_t buf;
    struct mcp_peer_connection *conn;
    size_t queued_bytes;
    bool close_after_write;
};

struct peer_thread_send_req {
    uint32_t server_id;
    char *payload;
    size_t len;
    int rc;
    bool done;
    uv_mutex_t mutex;
    uv_cond_t cond;
    struct peer_thread_send_req *next;
};

struct peer_connect_req {
    uv_connect_t req;
    struct mcp_peer_connection *conn;
    void (*on_connect)(void *owner, struct mcp_peer_connection *conn, int status);
};

struct peer_handler {
    char magic[4];
    char *owner;
    mcp_peer_frame_handler_cb on_frame;
    mcp_peer_event_cb on_peer_connected;
    mcp_peer_event_cb on_peer_closed;
    void *arg;
    struct peer_handler *next;
};

struct peer_capability {
    uint32_t server_id;
    char *name;
    struct peer_capability *next;
};

struct external_peer {
    uint32_t server_id;
    struct external_peer *next;
};

struct mcp_peer_connection {
    struct mcp_peer_transport *transport;
    uint32_t server_id;
    void *owner;
    uv_tcp_t tcp;
    bool initialized;
    bool connected;
    bool connecting;
    bool closing;
    bool close_notified;
    char *rx_buf;
    size_t rx_len;
    size_t rx_cap;
    size_t write_queue_bytes;
    struct mcp_peer_connection *next;
};

struct mcp_peer_transport {
    uv_loop_t *loop;
    uv_thread_t loop_thread;
    bool loop_thread_valid;
    bool closing;
    size_t max_frame_bytes;
    size_t write_high_watermark;
    uv_async_t send_async;
    bool send_async_initialized;
    uv_mutex_t send_mutex;
    bool send_mutex_initialized;
    struct peer_thread_send_req *pending_sends_head;
    struct peer_thread_send_req *pending_sends_tail;
    mcp_peer_json_frame_cb on_json;
    mcp_peer_close_cb on_close;
    void *json_arg;
    mcp_peer_external_send_cb external_send;
    void *external_send_arg;
    struct mcp_peer_connection *connections;
    struct peer_handler *handlers;
    struct peer_capability *capabilities;
    struct external_peer *external_peers;
};

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

static void thread_send_async_cb(uv_async_t *handle);

static void thread_send_complete(struct peer_thread_send_req *req, int rc)
{
    uv_mutex_lock(&req->mutex);
    req->rc = rc;
    req->done = true;
    uv_cond_signal(&req->cond);
    uv_mutex_unlock(&req->mutex);
}

static void fail_pending_thread_sends(struct mcp_peer_transport *transport)
{
    struct peer_thread_send_req *req;

    if (!transport || !transport->send_mutex_initialized)
        return;

    uv_mutex_lock(&transport->send_mutex);
    req = transport->pending_sends_head;
    transport->pending_sends_head = NULL;
    transport->pending_sends_tail = NULL;
    uv_mutex_unlock(&transport->send_mutex);

    while (req) {
        struct peer_thread_send_req *next = req->next;

        req->next = NULL;
        thread_send_complete(req, -1);
        req = next;
    }
}

static void send_async_close_cb(uv_handle_t *handle)
{
    struct mcp_peer_transport *transport = handle->data;

    if (transport)
        transport->send_async_initialized = false;
}

static bool peer_transport_on_loop_thread(struct mcp_peer_transport *transport)
{
    uv_thread_t current;

    if (!transport || !transport->loop_thread_valid)
        return true;
    current = uv_thread_self();
    return uv_thread_equal(&current, &transport->loop_thread) != 0;
}

static struct mcp_peer_connection *find_connection(struct mcp_peer_transport *transport,
                                                   uint32_t server_id)
{
    struct mcp_peer_connection *conn;

    for (conn = transport ? transport->connections : NULL; conn; conn = conn->next) {
        if (conn->server_id == server_id)
            return conn;
    }

    return NULL;
}

static struct peer_handler *find_handler(struct mcp_peer_transport *transport,
                                         const char magic[4])
{
    struct peer_handler *handler;

    for (handler = transport ? transport->handlers : NULL; handler; handler = handler->next) {
        if (memcmp(handler->magic, magic, 4) == 0)
            return handler;
    }

    return NULL;
}

static void unlink_connection(struct mcp_peer_connection *conn)
{
    struct mcp_peer_connection **current;

    if (!conn || !conn->transport)
        return;

    current = &conn->transport->connections;
    while (*current) {
        if (*current == conn) {
            *current = conn->next;
            conn->next = NULL;
            return;
        }
        current = &(*current)->next;
    }
}

static void clear_peer_capabilities(struct mcp_peer_transport *transport,
                                    uint32_t server_id)
{
    struct peer_capability **current;

    if (!transport || server_id == 0)
        return;

    current = &transport->capabilities;
    while (*current) {
        struct peer_capability *capability = *current;

        if (capability->server_id == server_id) {
            *current = capability->next;
            free(capability->name);
            free(capability);
            continue;
        }
        current = &capability->next;
    }
}

static void notify_peer_closed(struct mcp_peer_connection *conn)
{
    struct peer_handler *handler;

    if (!conn || conn->close_notified)
        return;

    conn->close_notified = true;
    clear_peer_capabilities(conn->transport, conn->server_id);
    for (handler = conn->transport->handlers; handler; handler = handler->next) {
        if (handler->on_peer_closed)
            handler->on_peer_closed(handler->arg, conn->server_id);
    }
    if (conn->transport->on_close)
        conn->transport->on_close(conn->transport->json_arg, conn->server_id);
}

void mcp_peer_transport_notify_closed(struct mcp_peer_transport *transport,
                                      uint32_t server_id)
{
    struct peer_handler *handler;
    struct external_peer **current;

    if (!transport || server_id == 0)
        return;

    clear_peer_capabilities(transport, server_id);
    current = &transport->external_peers;
    while (*current) {
        struct external_peer *peer = *current;

        if (peer->server_id == server_id) {
            *current = peer->next;
            free(peer);
            break;
        }
        current = &peer->next;
    }

    for (handler = transport->handlers; handler; handler = handler->next) {
        if (handler->on_peer_closed)
            handler->on_peer_closed(handler->arg, server_id);
    }
    if (transport->on_close)
        transport->on_close(transport->json_arg, server_id);
}

static void peer_close_cb(uv_handle_t *handle)
{
    struct mcp_peer_connection *conn = handle->data;

    if (!conn)
        return;

    notify_peer_closed(conn);
    unlink_connection(conn);
    free(conn->rx_buf);
    free(conn);
}

void mcp_peer_connection_close(struct mcp_peer_connection *conn)
{
    if (!conn || conn->closing)
        return;

    conn->closing = true;
    conn->connected = false;
    conn->connecting = false;
    if (conn->initialized && !uv_is_closing((uv_handle_t *)&conn->tcp)) {
        uv_read_stop((uv_stream_t *)&conn->tcp);
        uv_close((uv_handle_t *)&conn->tcp, peer_close_cb);
    } else {
        notify_peer_closed(conn);
        unlink_connection(conn);
        free(conn->rx_buf);
        free(conn);
    }
}

static int rx_reserve(struct mcp_peer_connection *conn, size_t want)
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

static void rx_consume(struct mcp_peer_connection *conn, size_t count)
{
    if (count >= conn->rx_len) {
        conn->rx_len = 0;
        return;
    }

    memmove(conn->rx_buf, conn->rx_buf + count, conn->rx_len - count);
    conn->rx_len -= count;
}

static void handle_payload(struct mcp_peer_connection *conn,
                           const unsigned char *payload,
                           size_t len)
{
    struct peer_handler *handler;

    if (len > 0 && payload[0] == '{') {
        if (conn->transport->on_json)
            conn->transport->on_json(conn->transport->json_arg,
                                     conn->server_id,
                                     (const char *)payload,
                                     len);
        return;
    }

    if (len < 4) {
        mcp_peer_connection_close(conn);
        return;
    }

    handler = find_handler(conn->transport, (const char *)payload);
    if (!handler || !handler->on_frame) {
        mcp_peer_connection_close(conn);
        return;
    }

    handler->on_frame(handler->arg, conn->server_id, payload, len);
}

static void process_rx(struct mcp_peer_connection *conn)
{
    while (conn->rx_len >= 4) {
        uint32_t frame_len = read_u32_be(conn->rx_buf);
        size_t total_len;

        if (frame_len == 0 || frame_len > conn->transport->max_frame_bytes) {
            mcp_peer_connection_close(conn);
            return;
        }

        total_len = (size_t)frame_len + 4;
        if (conn->rx_len < total_len)
            return;

        handle_payload(conn,
                       (const unsigned char *)conn->rx_buf + 4,
                       (size_t)frame_len);
        if (conn->closing)
            return;
        rx_consume(conn, total_len);
    }
}

static void alloc_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf)
{
    (void)handle;
    (void)suggested_size;

    buf->base = malloc(64 * 1024);
    buf->len = buf->base ? 64 * 1024 : 0;
}

static void read_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
    struct mcp_peer_connection *conn = stream->data;

    if (nread < 0) {
        free(buf->base);
        mcp_peer_connection_close(conn);
        return;
    }

    if (nread == 0) {
        free(buf->base);
        return;
    }

    if (rx_reserve(conn, (size_t)nread) != 0) {
        free(buf->base);
        mcp_peer_connection_close(conn);
        return;
    }

    memcpy(conn->rx_buf + conn->rx_len, buf->base, (size_t)nread);
    conn->rx_len += (size_t)nread;
    free(buf->base);
    process_rx(conn);
}

static int start_reading(struct mcp_peer_connection *conn)
{
    if (!conn || !conn->connected)
        return -1;
    return uv_read_start((uv_stream_t *)&conn->tcp, alloc_cb, read_cb);
}

static void notify_peer_connected(struct mcp_peer_connection *conn)
{
    struct peer_handler *handler;

    for (handler = conn->transport->handlers; handler; handler = handler->next) {
        if (handler->on_peer_connected)
            handler->on_peer_connected(handler->arg, conn->server_id);
    }
}

void mcp_peer_transport_notify_connected(struct mcp_peer_transport *transport,
                                         uint32_t server_id)
{
    struct peer_handler *handler;
    struct external_peer *peer;

    if (!transport || server_id == 0)
        return;

    for (peer = transport->external_peers; peer; peer = peer->next) {
        if (peer->server_id == server_id)
            break;
    }
    if (!peer) {
        peer = calloc(1, sizeof(*peer));
        if (peer) {
            peer->server_id = server_id;
            peer->next = transport->external_peers;
            transport->external_peers = peer;
        }
    }

    for (handler = transport->handlers; handler; handler = handler->next) {
        if (handler->on_peer_connected)
            handler->on_peer_connected(handler->arg, server_id);
    }
}

static void connect_cb(uv_connect_t *req, int status)
{
    struct peer_connect_req *connect_req = (struct peer_connect_req *)req;
    struct mcp_peer_connection *conn = connect_req->conn;
    void (*on_connect)(void *, struct mcp_peer_connection *, int) = connect_req->on_connect;
    void *owner = conn->owner;

    conn->connecting = false;
    if (status == 0 && !conn->closing) {
        conn->connected = true;
        if (start_reading(conn) != 0) {
            status = -1;
            conn->connected = false;
        }
    }

    free(connect_req);

    if (status == 0)
        notify_peer_connected(conn);
    if (on_connect)
        on_connect(owner, conn, status);
    if (status != 0)
        mcp_peer_connection_close(conn);
}

int mcp_peer_transport_create(struct mcp_peer_transport **out,
                              uv_loop_t *loop,
                              size_t max_frame_bytes,
                              size_t write_high_watermark)
{
    struct mcp_peer_transport *transport;

    *out = NULL;
    if (!loop || max_frame_bytes == 0)
        return -1;

    transport = calloc(1, sizeof(*transport));
    if (!transport)
        return -1;

    transport->loop = loop;
    transport->loop_thread = uv_thread_self();
    transport->loop_thread_valid = true;
    transport->max_frame_bytes = max_frame_bytes;
    transport->write_high_watermark = write_high_watermark ? write_high_watermark
                                                           : (4 * max_frame_bytes);
    if (uv_mutex_init(&transport->send_mutex) != 0) {
        free(transport);
        return -1;
    }
    transport->send_mutex_initialized = true;
    if (uv_async_init(loop, &transport->send_async, thread_send_async_cb) != 0) {
        uv_mutex_destroy(&transport->send_mutex);
        free(transport);
        return -1;
    }
    transport->send_async_initialized = true;
    transport->send_async.data = transport;
    uv_unref((uv_handle_t *)&transport->send_async);
    *out = transport;
    return 0;
}

void mcp_peer_transport_destroy(struct mcp_peer_transport *transport)
{
    struct peer_handler *handler;
    struct peer_capability *capability;
    struct external_peer *external_peer;

    if (!transport)
        return;

    mcp_peer_transport_close(transport);
    while (transport->loop &&
           (transport->connections || transport->send_async_initialized))
        uv_run(transport->loop, UV_RUN_DEFAULT);
    if (transport->send_mutex_initialized) {
        uv_mutex_destroy(&transport->send_mutex);
        transport->send_mutex_initialized = false;
    }

    while ((handler = transport->handlers) != NULL) {
        transport->handlers = handler->next;
        free(handler->owner);
        free(handler);
    }
    while ((capability = transport->capabilities) != NULL) {
        transport->capabilities = capability->next;
        free(capability->name);
        free(capability);
    }
    while ((external_peer = transport->external_peers) != NULL) {
        transport->external_peers = external_peer->next;
        free(external_peer);
    }
    free(transport);
}

int mcp_peer_transport_connect(struct mcp_peer_transport *transport,
                               uint32_t server_id,
                               const struct sockaddr *addr,
                               void *owner,
                               void (*on_connect)(void *owner,
                                                  struct mcp_peer_connection *conn,
                                                  int status))
{
    struct mcp_peer_connection *conn;
    struct peer_connect_req *connect_req;

    if (!transport || server_id == 0 || !addr || find_connection(transport, server_id))
        return -1;

    conn = calloc(1, sizeof(*conn));
    connect_req = calloc(1, sizeof(*connect_req));
    if (!conn || !connect_req) {
        free(conn);
        free(connect_req);
        return -1;
    }

    if (uv_tcp_init(transport->loop, &conn->tcp) != 0) {
        free(conn);
        free(connect_req);
        return -1;
    }

    conn->transport = transport;
    conn->server_id = server_id;
    conn->owner = owner;
    conn->initialized = true;
    conn->connecting = true;
    conn->tcp.data = conn;
    conn->next = transport->connections;
    transport->connections = conn;

    connect_req->conn = conn;
    connect_req->on_connect = on_connect;
    connect_req->req.data = connect_req;

    if (uv_tcp_connect(&connect_req->req, &conn->tcp, addr, connect_cb) != 0) {
        conn->connecting = false;
        mcp_peer_connection_close(conn);
        free(connect_req);
        return -1;
    }

    return 0;
}

int mcp_peer_transport_adopt(struct mcp_peer_transport *transport,
                             uint32_t server_id,
                             uv_tcp_t *tcp,
                             void *owner,
                             struct mcp_peer_connection **out)
{
    struct mcp_peer_connection *conn;

    if (out)
        *out = NULL;
    if (!transport || server_id == 0 || !tcp || find_connection(transport, server_id))
        return -1;

    conn = calloc(1, sizeof(*conn));
    if (!conn)
        return -1;

    conn->transport = transport;
    conn->server_id = server_id;
    conn->owner = owner;
    conn->initialized = true;
    conn->connected = true;
    conn->tcp = *tcp;
    conn->tcp.data = conn;
    conn->next = transport->connections;
    transport->connections = conn;

    if (start_reading(conn) != 0) {
        mcp_peer_connection_close(conn);
        return -1;
    }

    notify_peer_connected(conn);
    if (out)
        *out = conn;
    return 0;
}

void mcp_peer_transport_close(struct mcp_peer_transport *transport)
{
    if (!transport)
        return;

    if (transport->send_mutex_initialized) {
        uv_mutex_lock(&transport->send_mutex);
        transport->closing = true;
        uv_mutex_unlock(&transport->send_mutex);
    } else {
        transport->closing = true;
    }
    fail_pending_thread_sends(transport);
    if (transport->send_async_initialized &&
        !uv_is_closing((uv_handle_t *)&transport->send_async))
        uv_close((uv_handle_t *)&transport->send_async, send_async_close_cb);

    while (transport->connections)
        mcp_peer_connection_close(transport->connections);
}

void *mcp_peer_connection_owner(struct mcp_peer_connection *conn)
{
    return conn ? conn->owner : NULL;
}

bool mcp_peer_connection_is_connected(const struct mcp_peer_connection *conn)
{
    return conn && conn->connected && !conn->closing;
}

unsigned int mcp_peer_connection_server_id(const struct mcp_peer_connection *conn)
{
    return conn ? conn->server_id : 0;
}

static void write_cb(uv_write_t *req, int status)
{
    struct peer_write_req *write_req = (struct peer_write_req *)req;
    struct mcp_peer_connection *conn = write_req->conn;
    bool close_after_write = write_req->close_after_write;

    if (conn && conn->write_queue_bytes >= write_req->queued_bytes)
        conn->write_queue_bytes -= write_req->queued_bytes;
    free(write_req->buf.base);
    free(write_req);

    if (status < 0 && conn)
        mcp_peer_connection_close(conn);
    else if (close_after_write && conn)
        mcp_peer_connection_close(conn);
}

int mcp_peer_connection_send_frame(struct mcp_peer_connection *conn,
                                   const void *payload,
                                   size_t len,
                                   bool close_after_write)
{
    struct peer_write_req *write_req;

    if (!conn || !conn->connected || conn->closing || !payload || len == 0 ||
        len > UINT32_MAX ||
        len > conn->transport->max_frame_bytes)
        return -1;
    if (conn->write_queue_bytes + len + 4 > conn->transport->write_high_watermark)
        return -2;

    write_req = calloc(1, sizeof(*write_req));
    if (!write_req)
        return -1;

    write_req->buf.base = malloc(len + 4);
    if (!write_req->buf.base) {
        free(write_req);
        return -1;
    }

    write_req->conn = conn;
    write_req->queued_bytes = len + 4;
    write_req->close_after_write = close_after_write;
    write_u32_be(write_req->buf.base, (uint32_t)len);
    memcpy(write_req->buf.base + 4, payload, len);
#ifdef _WIN32
    write_req->buf.len = (unsigned long)(len + 4);
#else
    write_req->buf.len = len + 4;
#endif

    conn->write_queue_bytes += write_req->queued_bytes;
    if (uv_write(&write_req->req,
                 (uv_stream_t *)&conn->tcp,
                 &write_req->buf,
                 1,
                 write_cb) != 0) {
        conn->write_queue_bytes -= write_req->queued_bytes;
        free(write_req->buf.base);
        free(write_req);
        return -1;
    }

    return 0;
}

static int peer_transport_send_frame_on_loop(struct mcp_peer_transport *transport,
                                             uint32_t server_id,
                                             const void *payload,
                                             size_t len)
{
    if (transport && transport->external_send) {
        int rc = transport->external_send(transport->external_send_arg,
                                          server_id,
                                          payload,
                                          len);
        if (rc == 0 || rc == -2)
            return rc;
    }

    return mcp_peer_connection_send_frame(find_connection(transport, server_id),
                                          payload,
                                          len,
                                          false);
}

static void thread_send_async_cb(uv_async_t *handle)
{
    struct mcp_peer_transport *transport = handle->data;
    struct peer_thread_send_req *req;

    if (!transport || !transport->send_mutex_initialized)
        return;
    for (;;) {
        uv_mutex_lock(&transport->send_mutex);
        req = transport->pending_sends_head;
        if (req) {
            transport->pending_sends_head = req->next;
            if (!transport->pending_sends_head)
                transport->pending_sends_tail = NULL;
            req->next = NULL;
        }
        uv_mutex_unlock(&transport->send_mutex);
        if (!req)
            break;

        if (transport->closing)
            thread_send_complete(req, -1);
        else
            thread_send_complete(req,
                                 peer_transport_send_frame_on_loop(transport,
                                                                   req->server_id,
                                                                   req->payload,
                                                                   req->len));
    }
}

static int peer_transport_send_frame_threadsafe(struct mcp_peer_transport *transport,
                                                uint32_t server_id,
                                                const void *payload,
                                                size_t len)
{
    struct peer_thread_send_req req;
    int rc = -1;

    if (!transport || !transport->send_async_initialized || !transport->send_mutex_initialized)
        return -1;
    memset(&req, 0, sizeof(req));
    req.server_id = server_id;
    req.len = len;
    req.payload = malloc(len);
    if (!req.payload)
        return -1;
    memcpy(req.payload, payload, len);
    if (uv_mutex_init(&req.mutex) != 0) {
        free(req.payload);
        return -1;
    }
    if (uv_cond_init(&req.cond) != 0) {
        uv_mutex_destroy(&req.mutex);
        free(req.payload);
        return -1;
    }

    uv_mutex_lock(&transport->send_mutex);
    if (transport->closing ||
        !transport->send_async_initialized ||
        uv_is_closing((uv_handle_t *)&transport->send_async)) {
        uv_mutex_unlock(&transport->send_mutex);
        uv_cond_destroy(&req.cond);
        uv_mutex_destroy(&req.mutex);
        free(req.payload);
        return -1;
    }
    if (transport->pending_sends_tail)
        transport->pending_sends_tail->next = &req;
    else
        transport->pending_sends_head = &req;
    transport->pending_sends_tail = &req;
    uv_async_send(&transport->send_async);
    uv_mutex_unlock(&transport->send_mutex);

    uv_mutex_lock(&req.mutex);
    while (!req.done)
        uv_cond_wait(&req.cond, &req.mutex);
    rc = req.rc;
    uv_mutex_unlock(&req.mutex);

    uv_cond_destroy(&req.cond);
    uv_mutex_destroy(&req.mutex);
    free(req.payload);
    return rc;
}

int mcp_peer_transport_send_frame(struct mcp_peer_transport *transport,
                                  uint32_t server_id,
                                  const void *payload,
                                  size_t len)
{
    if (transport && !peer_transport_on_loop_thread(transport))
        return peer_transport_send_frame_threadsafe(transport, server_id, payload, len);
    return peer_transport_send_frame_on_loop(transport, server_id, payload, len);
}

size_t mcp_peer_transport_write_queue_bytes(struct mcp_peer_transport *transport,
                                            uint32_t server_id)
{
    struct mcp_peer_connection *conn = find_connection(transport, server_id);

    return conn ? conn->write_queue_bytes : 0;
}

bool mcp_peer_transport_write_queue_below_high_watermark(struct mcp_peer_transport *transport,
                                                         uint32_t server_id)
{
    struct mcp_peer_connection *conn = find_connection(transport, server_id);

    if (!conn)
        return false;
    return conn->write_queue_bytes < conn->transport->write_high_watermark;
}

void mcp_peer_transport_set_json_handler(struct mcp_peer_transport *transport,
                                         mcp_peer_json_frame_cb on_json,
                                         mcp_peer_close_cb on_close,
                                         void *arg)
{
    if (!transport)
        return;

    transport->on_json = on_json;
    transport->on_close = on_close;
    transport->json_arg = arg;
}

void mcp_peer_transport_set_external_sender(struct mcp_peer_transport *transport,
                                            mcp_peer_external_send_cb send_cb,
                                            void *arg)
{
    if (!transport)
        return;

    transport->external_send = send_cb;
    transport->external_send_arg = arg;
}

int mcp_peer_transport_dispatch_frame(struct mcp_peer_transport *transport,
                                      uint32_t server_id,
                                      const void *payload,
                                      size_t len)
{
    struct peer_handler *handler;
    const unsigned char *bytes = payload;

    if (!transport || server_id == 0 || !payload || len < 4)
        return -1;

    handler = find_handler(transport, (const char *)bytes);
    if (!handler || !handler->on_frame)
        return -1;

    handler->on_frame(handler->arg, server_id, bytes, len);
    return 0;
}

int mcp_peer_transport_register_handler(struct mcp_peer_transport *transport,
                                        const char magic[4],
                                        const char *owner,
                                        mcp_peer_frame_handler_cb on_frame,
                                        mcp_peer_event_cb on_peer_connected,
                                        mcp_peer_event_cb on_peer_closed,
                                        void *arg)
{
    struct peer_handler *handler;
    struct mcp_peer_connection *conn;

    if (!transport || !magic || !owner || !on_frame || find_handler(transport, magic))
        return -1;

    handler = calloc(1, sizeof(*handler));
    if (!handler)
        return -1;

    memcpy(handler->magic, magic, 4);
    handler->owner = mcp_strdup(owner);
    if (!handler->owner) {
        free(handler);
        return -1;
    }
    handler->on_frame = on_frame;
    handler->on_peer_connected = on_peer_connected;
    handler->on_peer_closed = on_peer_closed;
    handler->arg = arg;
    handler->next = transport->handlers;
    transport->handlers = handler;

    if (handler->on_peer_connected) {
        struct external_peer *external_peer;

        for (conn = transport->connections; conn; conn = conn->next) {
            if (conn->connected && !conn->closing)
                handler->on_peer_connected(handler->arg, conn->server_id);
        }
        for (external_peer = transport->external_peers; external_peer; external_peer = external_peer->next)
            handler->on_peer_connected(handler->arg, external_peer->server_id);
    }

    return 0;
}

int mcp_peer_transport_unregister_handler(struct mcp_peer_transport *transport,
                                          const char magic[4],
                                          const char *owner)
{
    struct peer_handler **current;

    if (!transport || !magic)
        return -1;

    current = &transport->handlers;
    while (*current) {
        struct peer_handler *handler = *current;

        if (memcmp(handler->magic, magic, 4) == 0 &&
            (!owner || strcmp(handler->owner, owner) == 0)) {
            *current = handler->next;
            free(handler->owner);
            free(handler);
            return 0;
        }
        current = &handler->next;
    }

    return -1;
}

void mcp_peer_transport_unregister_owner(struct mcp_peer_transport *transport,
                                         const char *owner)
{
    struct peer_handler **current;

    if (!transport || !owner)
        return;

    current = &transport->handlers;
    while (*current) {
        struct peer_handler *handler = *current;

        if (strcmp(handler->owner, owner) == 0) {
            *current = handler->next;
            free(handler->owner);
            free(handler);
            continue;
        }
        current = &handler->next;
    }
}

static struct peer_capability *find_capability(struct mcp_peer_transport *transport,
                                               uint32_t server_id,
                                               const char *capability)
{
    struct peer_capability *current;

    for (current = transport ? transport->capabilities : NULL; current; current = current->next) {
        if (current->server_id == server_id && strcmp(current->name, capability) == 0)
            return current;
    }

    return NULL;
}

int mcp_peer_transport_set_capability(struct mcp_peer_transport *transport,
                                      uint32_t server_id,
                                      const char *capability,
                                      bool enabled)
{
    struct peer_capability *current;

    if (!transport || server_id == 0 || !capability || capability[0] == '\0')
        return -1;

    current = find_capability(transport, server_id, capability);
    if (!enabled) {
        struct peer_capability **link = &transport->capabilities;

        while (*link) {
            if (*link == current) {
                *link = current->next;
                free(current->name);
                free(current);
                return 0;
            }
            link = &(*link)->next;
        }
        return 0;
    }

    if (current)
        return 0;

    current = calloc(1, sizeof(*current));
    if (!current)
        return -1;

    current->server_id = server_id;
    current->name = mcp_strdup(capability);
    if (!current->name) {
        free(current);
        return -1;
    }
    current->next = transport->capabilities;
    transport->capabilities = current;
    return 0;
}

bool mcp_peer_transport_has_capability(struct mcp_peer_transport *transport,
                                       uint32_t server_id,
                                       const char *capability)
{
    return transport &&
           server_id != 0 &&
           capability &&
           find_capability(transport, server_id, capability) != NULL;
}
