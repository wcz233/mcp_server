#include "transport/udp_transport.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct udp_send_req {
    uv_udp_send_t req;
    uv_buf_t buf;
    struct sockaddr_storage peer;
};

struct mcp_udp_transport {
    uv_loop_t *loop;
    struct mcp_udp_transport_config config;

    uv_udp_t socket;
    bool initialized;
    bool opened;
    bool receiving;
    bool closing;
    bool destroy_on_close;

    mcp_udp_datagram_cb on_datagram;
    mcp_udp_error_cb on_error;
    void *cb_arg;
};

static void close_cb(uv_handle_t *handle)
{
    struct mcp_udp_transport *transport = handle->data;

    if (transport) {
        transport->initialized = false;
        transport->opened = false;
        transport->receiving = false;
        if (transport->destroy_on_close)
            free(transport);
    }
}

static void alloc_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf)
{
    struct mcp_udp_transport *transport = handle->data;
    size_t size = transport->config.max_datagram_bytes;

    (void)suggested_size;

    if (size == 0)
        size = 64 * 1024;
#ifdef _WIN32
    if (size > ULONG_MAX)
        size = ULONG_MAX;
#endif

    buf->base = malloc(size);
#ifdef _WIN32
    buf->len = buf->base ? (unsigned long)size : 0;
#else
    buf->len = buf->base ? size : 0;
#endif
}

static void send_cb(uv_udp_send_t *req, int status)
{
    struct udp_send_req *send_req = (struct udp_send_req *)req;
    struct mcp_udp_transport *transport = req->handle->data;

    if (status < 0 && transport->on_error)
        transport->on_error(transport->cb_arg, status);

    free(send_req->buf.base);
    free(send_req);
}

static void recv_cb(uv_udp_t *handle,
                    ssize_t nread,
                    const uv_buf_t *buf,
                    const struct sockaddr *addr,
                    unsigned int flags)
{
    struct mcp_udp_transport *transport = handle->data;

    (void)flags;

    if (nread < 0) {
        free(buf->base);
        if (transport->on_error)
            transport->on_error(transport->cb_arg, (int)nread);
        return;
    }

    if (nread == 0 || !addr) {
        free(buf->base);
        return;
    }

    if ((size_t)nread > transport->config.max_datagram_bytes) {
        free(buf->base);
        if (transport->on_error)
            transport->on_error(transport->cb_arg, UV_EMSGSIZE);
        return;
    }

    if (transport->on_datagram)
        transport->on_datagram(transport->cb_arg, buf->base, (size_t)nread, addr);

    free(buf->base);
}

static int sockaddr_from_host_port(const char *host,
                                   unsigned int port,
                                   struct sockaddr_storage *out_addr)
{
    struct sockaddr_in addr4;
    struct sockaddr_in6 addr6;

    if (port > 65535)
        return -1;

    memset(out_addr, 0, sizeof(*out_addr));

    if (uv_ip4_addr(host, (int)port, &addr4) == 0) {
        memcpy(out_addr, &addr4, sizeof(addr4));
        return 0;
    }

    if (uv_ip6_addr(host, (int)port, &addr6) == 0) {
        memcpy(out_addr, &addr6, sizeof(addr6));
        return 0;
    }

    return -1;
}

static size_t peer_sockaddr_size(const struct sockaddr *addr)
{
    if (addr->sa_family == AF_INET)
        return sizeof(struct sockaddr_in);
    if (addr->sa_family == AF_INET6)
        return sizeof(struct sockaddr_in6);
    return 0;
}

int mcp_udp_transport_create(struct mcp_udp_transport **out,
                             uv_loop_t *loop,
                             struct mcp_udp_transport_config config)
{
    struct mcp_udp_transport *transport;

    *out = NULL;

    if (!loop || config.max_datagram_bytes == 0)
        return -1;

    transport = calloc(1, sizeof(*transport));
    if (!transport)
        return -1;

    transport->loop = loop;
    transport->config = config;

    *out = transport;
    return 0;
}

void mcp_udp_transport_destroy(struct mcp_udp_transport *transport)
{
    if (!transport)
        return;

    if (transport->initialized) {
        transport->destroy_on_close = true;
        if (!uv_is_closing((uv_handle_t *)&transport->socket))
            uv_close((uv_handle_t *)&transport->socket, close_cb);
        return;
    }

    free(transport);
}

int mcp_udp_transport_open(struct mcp_udp_transport *transport,
                           const char *bind_host,
                           unsigned int bind_port)
{
    struct sockaddr_storage addr;

    if (!transport || transport->opened || !bind_host)
        return -1;
    if (sockaddr_from_host_port(bind_host, bind_port, &addr) != 0)
        return -1;
    if (uv_udp_init(transport->loop, &transport->socket) != 0)
        return -1;

    transport->initialized = true;
    transport->socket.data = transport;

    if (uv_udp_bind(&transport->socket, (const struct sockaddr *)&addr, 0) != 0)
        return -1;

    transport->opened = true;
    return 0;
}

int mcp_udp_transport_start(struct mcp_udp_transport *transport,
                            mcp_udp_datagram_cb on_datagram,
                            mcp_udp_error_cb on_error,
                            void *arg)
{
    int rc;

    if (!transport || !transport->opened || transport->receiving)
        return -1;

    transport->on_datagram = on_datagram;
    transport->on_error = on_error;
    transport->cb_arg = arg;

    rc = uv_udp_recv_start(&transport->socket, alloc_cb, recv_cb);
    if (rc != 0)
        return -1;

    transport->receiving = true;
    return 0;
}

int mcp_udp_transport_send(struct mcp_udp_transport *transport,
                           const char *data,
                           size_t len,
                           const struct sockaddr *peer)
{
    struct udp_send_req *send_req;
    size_t peer_len;
    int rc;

    if (!transport || !data || len == 0 || !peer || !transport->opened)
        return -1;
    peer_len = peer_sockaddr_size(peer);
    if (peer_len == 0)
        return -1;
#ifdef _WIN32
    if (len > ULONG_MAX)
        return -1;
#endif

    send_req = calloc(1, sizeof(*send_req));
    if (!send_req)
        return -1;

    send_req->buf.base = malloc(len);
    if (!send_req->buf.base) {
        free(send_req);
        return -1;
    }

    memcpy(send_req->buf.base, data, len);
    memcpy(&send_req->peer, peer, peer_len);
#ifdef _WIN32
    send_req->buf.len = (unsigned long)len;
#else
    send_req->buf.len = len;
#endif

    rc = uv_udp_send(&send_req->req,
                     &transport->socket,
                     &send_req->buf,
                     1,
                     (const struct sockaddr *)&send_req->peer,
                     send_cb);
    if (rc != 0) {
        free(send_req->buf.base);
        free(send_req);
        return -1;
    }

    return 0;
}

void mcp_udp_transport_close(struct mcp_udp_transport *transport)
{
    if (!transport || transport->closing)
        return;

    transport->closing = true;
    if (transport->receiving) {
        uv_udp_recv_stop(&transport->socket);
        transport->receiving = false;
    }

    if (transport->initialized && !uv_is_closing((uv_handle_t *)&transport->socket))
        uv_close((uv_handle_t *)&transport->socket, close_cb);
}

bool mcp_udp_transport_is_open(const struct mcp_udp_transport *transport)
{
    return transport && transport->opened && !transport->closing;
}
