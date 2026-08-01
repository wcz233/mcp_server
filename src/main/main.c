#include "mcp/core/server.h"
#include "core/server_internal.h"

#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

static bool env_bool(const char *name, bool default_value)
{
    const char *value = getenv(name);

    if (!value)
        return default_value;
    if (value[0] == '0' || value[0] == 'n' || value[0] == 'N' ||
        value[0] == 'f' || value[0] == 'F')
        return false;
    return true;
}

static const char *env_str(const char *name, const char *default_value)
{
    const char *value = getenv(name);

    if (!value || value[0] == '\0')
        return default_value;
    return value;
}

static unsigned int env_uint(const char *name, unsigned int default_value)
{
    const char *value = getenv(name);
    char *end = NULL;
    unsigned long parsed;

    if (!value || value[0] == '\0')
        return default_value;

    parsed = strtoul(value, &end, 10);
    if (!end || *end != '\0' || parsed > 65535ul)
        return default_value;

    return (unsigned int)parsed;
}

static int stdin_fileno_value(void)
{
#ifdef _WIN32
    return _fileno(stdin);
#else
    return fileno(stdin);
#endif
}

static int stdout_fileno_value(void)
{
#ifdef _WIN32
    return _fileno(stdout);
#else
    return fileno(stdout);
#endif
}

struct shutdown_signal_context {
    struct mcp_server *server;
    uv_signal_t sigint_handle;
    uv_signal_t sigterm_handle;
#ifdef _WIN32
    uv_signal_t sigbreak_handle;
#endif
    bool sigint_initialized;
    bool sigterm_initialized;
#ifdef _WIN32
    bool sigbreak_initialized;
#endif
    bool closing;
};

static void shutdown_signal_close_cb(uv_handle_t *handle)
{
    (void)handle;
}

static void close_shutdown_signal(uv_signal_t *handle, bool *initialized)
{
    if (!*initialized)
        return;

    uv_signal_stop(handle);
    if (!uv_is_closing((uv_handle_t *)handle))
        uv_close((uv_handle_t *)handle, shutdown_signal_close_cb);
    *initialized = false;
}

static void close_shutdown_signals(struct shutdown_signal_context *ctx)
{
    close_shutdown_signal(&ctx->sigint_handle, &ctx->sigint_initialized);
    close_shutdown_signal(&ctx->sigterm_handle, &ctx->sigterm_initialized);
#ifdef _WIN32
    close_shutdown_signal(&ctx->sigbreak_handle, &ctx->sigbreak_initialized);
#endif
}

static void shutdown_signal_cb(uv_signal_t *handle, int signum)
{
    struct shutdown_signal_context *ctx = handle->data;

    (void)signum;
    if (!ctx || ctx->closing)
        return;

    ctx->closing = true;
    close_shutdown_signals(ctx);
    mcp_server_request_shutdown(ctx->server);
}

static int start_shutdown_signal(uv_loop_t *loop,
                                 uv_signal_t *handle,
                                 bool *initialized,
                                 struct shutdown_signal_context *ctx,
                                 int signum)
{
    int rc = uv_signal_init(loop, handle);

    if (rc != 0)
        return rc;

    *initialized = true;
    handle->data = ctx;

    rc = uv_signal_start(handle, shutdown_signal_cb, signum);
    if (rc != 0)
        return rc;

    uv_unref((uv_handle_t *)handle);
    return 0;
}

static int install_shutdown_signals(uv_loop_t *loop, struct shutdown_signal_context *ctx)
{
    int rc;

    rc = start_shutdown_signal(loop,
                               &ctx->sigint_handle,
                               &ctx->sigint_initialized,
                               ctx,
                               SIGINT);
    if (rc != 0)
        return rc;

    rc = start_shutdown_signal(loop,
                               &ctx->sigterm_handle,
                               &ctx->sigterm_initialized,
                               ctx,
                               SIGTERM);
    if (rc != 0)
        return rc;

#ifdef _WIN32
    rc = start_shutdown_signal(loop,
                               &ctx->sigbreak_handle,
                               &ctx->sigbreak_initialized,
                               ctx,
                               SIGBREAK);
    if (rc != 0)
        return rc;
#endif

    return 0;
}

static int prepare_stdio(void)
{
#ifdef _WIN32
    if (_setmode(stdin_fileno_value(), _O_BINARY) == -1)
        return -1;
    if (_setmode(stdout_fileno_value(), _O_BINARY) == -1)
        return -1;
#endif
    return 0;
}

static int close_loop(uv_loop_t *loop)
{
    int rc = uv_loop_close(loop);

    if (rc != 0) {
        fprintf(stderr, "uv_loop_close: %s\n", uv_strerror(rc));
        return -1;
    }

    return 0;
}

int main(void)
{
    uv_loop_t loop;
    struct mcp_server *server = NULL;
    struct shutdown_signal_context signal_ctx = {0};
    struct mcp_server_config config = {
        .strict_initialized_notification = env_bool("MCP_STRICT_INIT", true),
        .stdio_eof_shutdown = true,
        .max_line_bytes = 1024 * 1024,
    };
    bool stdio_enabled = env_bool("MCP_ENABLE_STDIO", true);
    bool udp_enabled = env_bool("MCP_ENABLE_UDP", false);
    bool pipe_enabled = env_bool("MCP_ENABLE_PIPE", false);
    bool tcp_enabled = env_bool("MCP_ENABLE_TCP", false);
    bool discovery_enabled = env_bool("MCP_ENABLE_DISCOVERY", true);
    const char *udp_host = env_str("MCP_UDP_HOST", "127.0.0.1");
    unsigned int udp_port = env_uint("MCP_UDP_PORT", 8765);
    const char *pipe_path = env_str("MCP_PIPE_PATH",
#ifdef _WIN32
                                    "\\\\.\\pipe\\mcp-server"
#else
                                    "/tmp/mcp-server.sock"
#endif
    );
    const char *tcp_host = env_str("MCP_TCP_HOST", "127.0.0.1");
    unsigned int tcp_port = env_uint("MCP_TCP_PORT",
                                     env_uint("MCP_TCP_LISTEN_PORT", 8765));
    const char *discovery_bind_host = env_str("MCP_DISCOVERY_BIND_HOST", "0.0.0.0");
    unsigned int discovery_port = env_uint("MCP_DISCOVERY_PORT", 0);
    unsigned int broadcast_port = env_uint("MCP_UDP_BROADCAST_LISTEN_PORT", 0);
    const char *discovery_advertise_host = env_str("MCP_DISCOVERY_ADVERTISE_HOST", NULL);
    const char *discovery_hosts = env_str("MCP_DISCOVERY_HOSTS", NULL);
    bool force_stdio_eof_shutdown = env_bool("MCP_STDIO_EOF_SHUTDOWN", false);
    int stdin_fd;
    int stdout_fd;
    int rc;

#ifndef _WIN32
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        fprintf(stderr, "ignore SIGPIPE failed\n");
        return 1;
    }
#endif

    if (!stdio_enabled && !udp_enabled && !pipe_enabled && !tcp_enabled)
        stdio_enabled = true;

    config.stdio_eof_shutdown = force_stdio_eof_shutdown ||
                                !(pipe_enabled || tcp_enabled || udp_enabled);

    if (stdio_enabled && prepare_stdio() != 0) {
        fprintf(stderr, "prepare_stdio failed\n");
        return 1;
    }

    stdin_fd = -1;
    stdout_fd = -1;
    if (stdio_enabled) {
        stdin_fd = stdin_fileno_value();
        stdout_fd = stdout_fileno_value();
        if (stdin_fd < 0 || stdout_fd < 0) {
            fprintf(stderr, "invalid stdio handles\n");
            return 1;
        }
    }

    rc = uv_loop_init(&loop);
    if (rc != 0) {
        fprintf(stderr, "uv_loop_init: %s\n", uv_strerror(rc));
        return 1;
    }

    rc = mcp_server_init(&server, &loop, config);
    if (rc != 0) {
        fprintf(stderr, "mcp_server_init failed\n");
        close_loop(&loop);
        return 1;
    }
    signal_ctx.server = server;

    if (stdio_enabled) {
        rc = mcp_server_start_stdio(server, stdin_fd, stdout_fd);
        if (rc != 0) {
            fprintf(stderr, "mcp_server_start_stdio failed\n");
            mcp_server_destroy(server);
            close_loop(&loop);
            return 1;
        }
    }

    if (udp_enabled) {
        rc = mcp_server_start_udp(server, udp_host, udp_port);
        if (rc != 0) {
            fprintf(stderr, "mcp_server_start_udp failed for %s:%u\n", udp_host, udp_port);
            mcp_server_destroy(server);
            close_loop(&loop);
            return 1;
        }
    }

    if (pipe_enabled) {
        rc = mcp_server_start_pipe(server, pipe_path);
        if (rc != 0) {
            fprintf(stderr, "mcp_server_start_pipe failed for %s\n", pipe_path);
            mcp_server_destroy(server);
            close_loop(&loop);
            return 1;
        }
    }

    if (tcp_enabled) {
        rc = mcp_server_start_tcp(server, tcp_host, tcp_port);
        if (rc != 0) {
            fprintf(stderr, "mcp_server_start_tcp failed for %s:%u\n", tcp_host, tcp_port);
            mcp_server_destroy(server);
            close_loop(&loop);
            return 1;
        }
    }

    if (tcp_enabled && discovery_enabled) {
        unsigned int effective_discovery_port = discovery_port ? discovery_port : tcp_port;
        unsigned int effective_broadcast_port = broadcast_port ? broadcast_port : tcp_port;

        rc = mcp_server_start_discovery(
            server,
            &(struct mcp_server_discovery_config){
                .bind_host = discovery_bind_host,
                .discovery_port = effective_discovery_port,
                .broadcast_port = effective_broadcast_port,
                .tcp_host = tcp_host,
                .tcp_port = tcp_port,
                .advertise_host = discovery_advertise_host,
                .explicit_hosts = discovery_hosts,
            });
        if (rc != 0) {
            fprintf(stderr, "mcp_server_start_discovery failed for UDP %s:%u\n",
                    discovery_bind_host,
                    effective_discovery_port);
            mcp_server_destroy(server);
            close_loop(&loop);
            return 1;
        }
    }

    rc = install_shutdown_signals(&loop, &signal_ctx);
    if (rc != 0) {
        fprintf(stderr, "install_shutdown_signals: %s\n", uv_strerror(rc));
        close_shutdown_signals(&signal_ctx);
        uv_run(&loop, UV_RUN_DEFAULT);
        mcp_server_destroy(server);
        close_loop(&loop);
        return 1;
    }

    uv_run(&loop, UV_RUN_DEFAULT);

    close_shutdown_signals(&signal_ctx);
    uv_run(&loop, UV_RUN_DEFAULT);

    mcp_server_destroy(server);
    return close_loop(&loop) == 0 ? 0 : 1;
}
