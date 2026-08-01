#include "security/tls_stream.h"

#include <mbedtls/ssl.h>

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MCP_TLS_READ_CHUNK (16u * 1024u)

struct tls_write_req {
    uv_write_t req;
    uv_buf_t buf;
    struct mcp_tls_stream *stream;
    size_t len;
};

struct mcp_tls_stream {
    uv_stream_t *stream;
    mbedtls_ssl_context ssl;
    bool ready;
    bool stopped;
    bool failed;
    size_t pending_write_bytes;
    unsigned char *encrypted_rx;
    size_t encrypted_rx_len;
    size_t encrypted_rx_cap;
    char peer_fingerprint[MCP_TLS_CERT_FINGERPRINT_HEX_SIZE];
    mcp_tls_ready_cb on_ready;
    mcp_tls_data_cb on_data;
    mcp_tls_error_cb on_error;
    mcp_tls_drain_cb on_drain;
    void *cb_arg;
};

static void tls_process(struct mcp_tls_stream *stream);

static void tls_fail(struct mcp_tls_stream *stream, int error_code)
{
    if (!stream || stream->failed || stream->stopped)
        return;
    stream->failed = true;
    if (stream->on_error)
        stream->on_error(stream->cb_arg, error_code);
}

static int encrypted_rx_reserve(struct mcp_tls_stream *stream, size_t additional)
{
    size_t required = stream->encrypted_rx_len + additional;
    size_t capacity = stream->encrypted_rx_cap;
    unsigned char *next;

    if (required <= capacity)
        return 0;
    if (capacity == 0)
        capacity = MCP_TLS_READ_CHUNK;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2)
            return -1;
        capacity *= 2;
    }
    next = realloc(stream->encrypted_rx, capacity);
    if (!next)
        return -1;
    stream->encrypted_rx = next;
    stream->encrypted_rx_cap = capacity;
    return 0;
}

static int tls_bio_recv(void *ctx, unsigned char *data, size_t len)
{
    struct mcp_tls_stream *stream = ctx;
    size_t count;

    if (!stream || stream->stopped)
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    if (stream->encrypted_rx_len == 0)
        return MBEDTLS_ERR_SSL_WANT_READ;

    count = len < stream->encrypted_rx_len ? len : stream->encrypted_rx_len;
    memcpy(data, stream->encrypted_rx, count);
    if (count < stream->encrypted_rx_len) {
        memmove(stream->encrypted_rx,
                stream->encrypted_rx + count,
                stream->encrypted_rx_len - count);
    }
    stream->encrypted_rx_len -= count;
    return (int)count;
}

static void tls_write_cb(uv_write_t *req, int status)
{
    struct tls_write_req *write_req = (struct tls_write_req *)req;
    struct mcp_tls_stream *stream = write_req->stream;

    if (stream && stream->pending_write_bytes >= write_req->len)
        stream->pending_write_bytes -= write_req->len;
    free(write_req->buf.base);
    free(write_req);
    if (status < 0)
        tls_fail(stream, status);
    else if (stream && !stream->failed && !stream->stopped) {
        tls_process(stream);
        if (stream->pending_write_bytes == 0 && stream->on_drain)
            stream->on_drain(stream->cb_arg);
    }
}

static int tls_bio_send(void *ctx, const unsigned char *data, size_t len)
{
    struct mcp_tls_stream *stream = ctx;
    struct tls_write_req *write_req;

    if (!stream || stream->stopped || stream->failed || !data || len == 0)
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
#ifdef _WIN32
    if (len > ULONG_MAX)
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
#endif

    write_req = calloc(1, sizeof(*write_req));
    if (!write_req)
        return MBEDTLS_ERR_SSL_ALLOC_FAILED;
    write_req->buf.base = malloc(len);
    if (!write_req->buf.base) {
        free(write_req);
        return MBEDTLS_ERR_SSL_ALLOC_FAILED;
    }
    memcpy(write_req->buf.base, data, len);
#ifdef _WIN32
    write_req->buf.len = (unsigned long)len;
#else
    write_req->buf.len = len;
#endif
    write_req->stream = stream;
    write_req->len = len;

    stream->pending_write_bytes += len;
    if (uv_write(&write_req->req, stream->stream, &write_req->buf, 1, tls_write_cb) != 0) {
        stream->pending_write_bytes -= len;
        free(write_req->buf.base);
        free(write_req);
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
    return (int)len;
}

static void tls_process(struct mcp_tls_stream *stream)
{
    unsigned char plaintext[MCP_TLS_READ_CHUNK];
    int rc;

    if (!stream || stream->stopped || stream->failed)
        return;

    if (!stream->ready) {
        rc = mbedtls_ssl_handshake(&stream->ssl);
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE)
            return;
        if (rc != 0 || mbedtls_ssl_get_verify_result(&stream->ssl) != 0) {
            tls_fail(stream, rc != 0 ? rc : MBEDTLS_ERR_SSL_BAD_CERTIFICATE);
            return;
        }
        if (mcp_tls_certificate_fingerprint(mbedtls_ssl_get_peer_cert(&stream->ssl),
                                            stream->peer_fingerprint) != 0) {
            tls_fail(stream, MBEDTLS_ERR_SSL_BAD_CERTIFICATE);
            return;
        }
        stream->ready = true;
        if (stream->on_ready)
            stream->on_ready(stream->cb_arg);
        if (stream->stopped || stream->failed)
            return;
    }

    for (;;) {
        rc = mbedtls_ssl_read(&stream->ssl, plaintext, sizeof(plaintext));
        if (rc > 0) {
            if (stream->on_data)
                stream->on_data(stream->cb_arg, plaintext, (size_t)rc);
            if (stream->stopped || stream->failed)
                return;
            continue;
        }
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE)
            return;
        tls_fail(stream, rc);
        return;
    }
}

static void tls_alloc_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf)
{
    (void)handle;
    (void)suggested_size;
    buf->base = malloc(MCP_TLS_READ_CHUNK);
    buf->len = buf->base ? MCP_TLS_READ_CHUNK : 0;
}

static void tls_read_cb(uv_stream_t *uv_stream, ssize_t nread, const uv_buf_t *buf)
{
    struct mcp_tls_stream *stream = uv_stream->data;

    if (nread < 0) {
        free(buf->base);
        tls_fail(stream, (int)nread);
        return;
    }
    if (nread == 0) {
        free(buf->base);
        return;
    }
    if (encrypted_rx_reserve(stream, (size_t)nread) != 0) {
        free(buf->base);
        tls_fail(stream, UV_ENOMEM);
        return;
    }
    memcpy(stream->encrypted_rx + stream->encrypted_rx_len, buf->base, (size_t)nread);
    stream->encrypted_rx_len += (size_t)nread;
    free(buf->base);
    tls_process(stream);
}

int mcp_tls_stream_create(struct mcp_tls_stream **out,
                          struct mcp_tls_context *context,
                          bool server,
                          const char *server_name,
                          uv_stream_t *uv_stream,
                          mcp_tls_ready_cb on_ready,
                          mcp_tls_data_cb on_data,
                          mcp_tls_error_cb on_error,
                          mcp_tls_drain_cb on_drain,
                          void *arg)
{
    struct mcp_tls_stream *stream;
    const mbedtls_ssl_config *config;
    int rc;

    if (out)
        *out = NULL;
    if (!out || !context || !uv_stream)
        return -1;

    stream = calloc(1, sizeof(*stream));
    if (!stream)
        return -1;
    stream->stream = uv_stream;
    stream->on_ready = on_ready;
    stream->on_data = on_data;
    stream->on_error = on_error;
    stream->on_drain = on_drain;
    stream->cb_arg = arg;
    mbedtls_ssl_init(&stream->ssl);

    config = server ? mcp_tls_context_server_config(context)
                    : mcp_tls_context_client_config(context);
    rc = mbedtls_ssl_setup(&stream->ssl, config);
    if (rc != 0)
        goto fail;
    if (!server && server_name && server_name[0] != '\0') {
        rc = mbedtls_ssl_set_hostname(&stream->ssl, server_name);
        if (rc != 0)
            goto fail;
    }
    mbedtls_ssl_set_bio(&stream->ssl, stream, tls_bio_send, tls_bio_recv, NULL);
    *out = stream;
    return 0;

fail:
    mcp_tls_stream_destroy(stream);
    return -1;
}

void mcp_tls_stream_destroy(struct mcp_tls_stream *stream)
{
    if (!stream)
        return;
    mbedtls_ssl_free(&stream->ssl);
    free(stream->encrypted_rx);
    free(stream);
}

int mcp_tls_stream_start(struct mcp_tls_stream *stream)
{
    if (!stream || stream->stopped || stream->failed)
        return -1;
    stream->stream->data = stream;
    if (uv_read_start(stream->stream, tls_alloc_cb, tls_read_cb) != 0)
        return -1;
    tls_process(stream);
    return stream->failed ? -1 : 0;
}

void mcp_tls_stream_stop(struct mcp_tls_stream *stream)
{
    if (!stream || stream->stopped)
        return;
    stream->stopped = true;
    if (stream->stream)
        uv_read_stop(stream->stream);
}

int mcp_tls_stream_send(struct mcp_tls_stream *stream, const void *data, size_t len)
{
    const unsigned char *cursor = data;

    if (!stream || !stream->ready || stream->stopped || stream->failed || !data || len == 0)
        return -1;
    while (len > 0) {
        int rc = mbedtls_ssl_write(&stream->ssl, cursor, len);

        if (rc <= 0)
            return -1;
        cursor += rc;
        len -= (size_t)rc;
    }
    return 0;
}

bool mcp_tls_stream_is_ready(const struct mcp_tls_stream *stream)
{
    return stream && stream->ready && !stream->stopped && !stream->failed;
}

const char *mcp_tls_stream_peer_fingerprint(const struct mcp_tls_stream *stream)
{
    return mcp_tls_stream_is_ready(stream) ? stream->peer_fingerprint : NULL;
}

size_t mcp_tls_stream_pending_write_bytes(const struct mcp_tls_stream *stream)
{
    return stream ? stream->pending_write_bytes : 0;
}

void *mcp_tls_stream_callback_arg(const struct mcp_tls_stream *stream)
{
    return stream ? stream->cb_arg : NULL;
}
