#include "transport/stdio_transport.h"

#include <stdbool.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct write_node {
    uv_write_t req;
    uv_buf_t buf;
    struct write_node *next;
};

struct mcp_stdio_transport {
    uv_loop_t *loop;
    struct mcp_stdio_transport_config config;

    uv_pipe_t stdin_pipe;
    uv_pipe_t stdout_pipe;
    bool stdin_initialized;
    bool stdout_initialized;
    bool opened;
    bool closing;
    bool destroy_on_close;
    bool close_stdout_requested;
    bool close_stdout_allowed;
    unsigned int close_pending;

    char *rx_buf;
    size_t rx_len;
    size_t rx_cap;

    struct write_node *write_head;
    struct write_node *write_tail;
    bool write_busy;

    mcp_stdio_line_cb on_line;
    mcp_stdio_exit_cb on_exit;
    void *cb_arg;
};

static void flush_writes(struct mcp_stdio_transport *transport);
static void transport_free(struct mcp_stdio_transport *transport);

static void close_cb(uv_handle_t *handle)
{
    struct mcp_stdio_transport *transport = handle->data;

    if (!transport)
        return;

    if (handle == (uv_handle_t *)&transport->stdin_pipe)
        transport->stdin_initialized = false;
    else if (handle == (uv_handle_t *)&transport->stdout_pipe)
        transport->stdout_initialized = false;

    if (transport->close_pending > 0)
        transport->close_pending--;
    if (transport->close_pending == 0 && transport->destroy_on_close)
        transport_free(transport);
}

static void request_close(uv_handle_t *handle, bool *initialized, unsigned int *close_pending)
{
    if (!*initialized || uv_is_closing(handle))
        return;

    (*close_pending)++;
    uv_close(handle, close_cb);
}

static void transport_free(struct mcp_stdio_transport *transport)
{
    struct write_node *node = transport->write_head;

    while (node) {
        struct write_node *next = node->next;
        free(node->buf.base);
        free(node);
        node = next;
    }

    free(transport->rx_buf);
    free(transport);
}

static void maybe_close_stdout(struct mcp_stdio_transport *transport)
{
    if (!transport->close_stdout_requested || !transport->close_stdout_allowed)
        return;
    if (transport->write_busy || transport->write_head)
        return;
    if (!transport->stdout_initialized ||
        uv_is_closing((uv_handle_t *)&transport->stdout_pipe))
        return;

    request_close((uv_handle_t *)&transport->stdout_pipe,
                  &transport->stdout_initialized,
                  &transport->close_pending);
}

static void signal_exit(struct mcp_stdio_transport *transport, bool close_stdout_now)
{
    if (transport->closing)
        return;

    transport->closing = true;
    transport->opened = false;
    if (transport->stdin_initialized)
        uv_read_stop((uv_stream_t *)&transport->stdin_pipe);

    request_close((uv_handle_t *)&transport->stdin_pipe,
                  &transport->stdin_initialized,
                  &transport->close_pending);

    transport->close_stdout_requested = true;
    if (close_stdout_now) {
        transport->close_stdout_allowed = true;
        request_close((uv_handle_t *)&transport->stdout_pipe,
                      &transport->stdout_initialized,
                      &transport->close_pending);
    }

    if (transport->on_exit)
        transport->on_exit(transport->cb_arg);
}

static void alloc_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf)
{
    (void)handle;
    (void)suggested_size;

    buf->base = malloc(64 * 1024);
    buf->len = buf->base ? 64 * 1024 : 0;
}

static int rx_reserve(struct mcp_stdio_transport *transport, size_t want)
{
    size_t need = transport->rx_len + want;
    size_t cap = transport->rx_cap;
    char *next;

    if (need <= cap)
        return 0;

    if (cap == 0)
        cap = 4096;
    while (cap < need)
        cap *= 2;

    next = realloc(transport->rx_buf, cap);
    if (!next)
        return -1;

    transport->rx_buf = next;
    transport->rx_cap = cap;
    return 0;
}

static void rx_consume(struct mcp_stdio_transport *transport, size_t count)
{
    if (count >= transport->rx_len) {
        transport->rx_len = 0;
        return;
    }

    memmove(transport->rx_buf, transport->rx_buf + count, transport->rx_len - count);
    transport->rx_len -= count;
}

static void on_write_cb(uv_write_t *req, int status)
{
    struct write_node *node = (struct write_node *)req;
    struct mcp_stdio_transport *transport = req->handle->data;

    if (status < 0)
        signal_exit(transport, true);

    free(node->buf.base);
    free(node);

    transport->write_busy = false;
    flush_writes(transport);
    maybe_close_stdout(transport);
}

static void flush_writes(struct mcp_stdio_transport *transport)
{
    while (transport->write_head && !transport->write_busy) {
        struct write_node *node = transport->write_head;
        int rc;

        transport->write_head = node->next;
        if (!transport->write_head)
            transport->write_tail = NULL;

        transport->write_busy = true;
        rc = uv_write(&node->req,
                      (uv_stream_t *)&transport->stdout_pipe,
                      &node->buf,
                      1,
                      on_write_cb);
        if (rc != 0) {
            transport->write_busy = false;
            signal_exit(transport, true);
            return;
        }
    }

    maybe_close_stdout(transport);
}

static void read_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
    struct mcp_stdio_transport *transport = stream->data;

    if (nread < 0) {
        free(buf->base);
        signal_exit(transport, false);
        return;
    }

    if (nread == 0) {
        free(buf->base);
        return;
    }

    if (rx_reserve(transport, (size_t)nread) != 0) {
        free(buf->base);
        signal_exit(transport, true);
        return;
    }

    memcpy(transport->rx_buf + transport->rx_len, buf->base, (size_t)nread);
    transport->rx_len += (size_t)nread;
    free(buf->base);

    while (true) {
        char *newline = memchr(transport->rx_buf, '\n', transport->rx_len);
        const char *line = transport->rx_buf;
        size_t line_len;
        size_t consume_len;

        if (!newline)
            break;

        line_len = (size_t)(newline - transport->rx_buf);
        consume_len = line_len + 1;
        if (line_len > 0 && line[line_len - 1] == '\r')
            line_len--;

        if (line_len == 0) {
            rx_consume(transport, consume_len);
            continue;
        }

        if (line_len > transport->config.max_line_bytes) {
            signal_exit(transport, true);
            return;
        }

        if (transport->on_line)
            transport->on_line(transport->cb_arg, line, line_len);

        rx_consume(transport, consume_len);
    }

    if (transport->rx_len > transport->config.max_line_bytes) {
        signal_exit(transport, true);
        return;
    }

    flush_writes(transport);
}

int mcp_stdio_transport_create(struct mcp_stdio_transport **out,
                               uv_loop_t *loop,
                               struct mcp_stdio_transport_config config)
{
    struct mcp_stdio_transport *transport;

    *out = NULL;
    transport = calloc(1, sizeof(*transport));
    if (!transport)
        return -1;

    transport->loop = loop;
    transport->config = config;

    uv_pipe_init(loop, &transport->stdin_pipe, 0);
    uv_pipe_init(loop, &transport->stdout_pipe, 0);
    transport->stdin_initialized = true;
    transport->stdout_initialized = true;
    transport->stdin_pipe.data = transport;
    transport->stdout_pipe.data = transport;

    *out = transport;
    return 0;
}

void mcp_stdio_transport_destroy(struct mcp_stdio_transport *transport)
{
    if (!transport)
        return;

    if ((transport->stdin_initialized &&
         !uv_is_closing((uv_handle_t *)&transport->stdin_pipe)) ||
        (transport->stdout_initialized &&
         !uv_is_closing((uv_handle_t *)&transport->stdout_pipe))) {
        transport->destroy_on_close = true;
        mcp_stdio_transport_close(transport);
        if (transport->close_pending != 0)
            return;
    }

    transport_free(transport);
}

int mcp_stdio_transport_open(struct mcp_stdio_transport *transport,
                             int stdin_fd,
                             int stdout_fd)
{
    if (uv_pipe_open(&transport->stdin_pipe, stdin_fd) != 0)
        return -1;
    if (uv_pipe_open(&transport->stdout_pipe, stdout_fd) != 0)
        return -1;

    transport->opened = true;
    return 0;
}

int mcp_stdio_transport_start(struct mcp_stdio_transport *transport,
                              mcp_stdio_line_cb on_line,
                              mcp_stdio_exit_cb on_exit,
                              void *arg)
{
    int rc;

    if (!transport->opened)
        return -1;

    transport->on_line = on_line;
    transport->on_exit = on_exit;
    transport->cb_arg = arg;

    rc = uv_read_start((uv_stream_t *)&transport->stdin_pipe, alloc_cb, read_cb);
    return rc == 0 ? 0 : -1;
}

int mcp_stdio_transport_send(struct mcp_stdio_transport *transport, const char *data, size_t len)
{
    struct write_node *node;

    if (!transport || transport->closing)
        return -1;
    if (!data || len == 0)
        return 0;
#ifdef _WIN32
    if (len > ULONG_MAX)
        return -1;
#endif

    node = calloc(1, sizeof(*node));
    if (!node)
        return -1;

    node->buf.base = malloc(len);
    if (!node->buf.base) {
        free(node);
        return -1;
    }

    memcpy(node->buf.base, data, len);
#ifdef _WIN32
    node->buf.len = (unsigned long)len;
#else
    node->buf.len = len;
#endif

    if (!transport->write_tail) {
        transport->write_head = node;
        transport->write_tail = node;
    } else {
        transport->write_tail->next = node;
        transport->write_tail = node;
    }

    flush_writes(transport);
    return 0;
}

int mcp_stdio_transport_send_str(struct mcp_stdio_transport *transport, const char *text)
{
    return mcp_stdio_transport_send(transport, text, strlen(text));
}

void mcp_stdio_transport_close(struct mcp_stdio_transport *transport)
{
    if (!transport)
        return;

    transport->closing = true;
    transport->opened = false;

    if (transport->stdin_initialized)
        uv_read_stop((uv_stream_t *)&transport->stdin_pipe);
    request_close((uv_handle_t *)&transport->stdin_pipe,
                  &transport->stdin_initialized,
                  &transport->close_pending);

    transport->close_stdout_requested = true;
    transport->close_stdout_allowed = true;
    if (transport->write_busy || transport->write_head) {
        request_close((uv_handle_t *)&transport->stdout_pipe,
                      &transport->stdout_initialized,
                      &transport->close_pending);
        return;
    }

    maybe_close_stdout(transport);
}

void mcp_stdio_transport_close_output(struct mcp_stdio_transport *transport)
{
    if (!transport)
        return;

    transport->close_stdout_requested = true;
    transport->close_stdout_allowed = true;
    maybe_close_stdout(transport);
}
