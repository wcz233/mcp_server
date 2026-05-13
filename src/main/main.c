#include "mcp/core/server.h"

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
    struct mcp_server_config config = {
        .strict_initialized_notification = env_bool("MCP_STRICT_INIT", true),
        .stdio_eof_shutdown = true,
        .max_line_bytes = 1024 * 1024,
    };
    bool stdio_enabled = env_bool("MCP_ENABLE_STDIO", true);
    bool udp_enabled = env_bool("MCP_ENABLE_UDP", false);
    bool pipe_enabled = env_bool("MCP_ENABLE_PIPE", false);
    bool tcp_enabled = env_bool("MCP_ENABLE_TCP", false);
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
    unsigned int tcp_port = env_uint("MCP_TCP_PORT", 8765);
    int stdin_fd;
    int stdout_fd;
    int rc;

    if (!stdio_enabled && !udp_enabled && !pipe_enabled && !tcp_enabled)
        stdio_enabled = true;

    config.stdio_eof_shutdown = !(pipe_enabled || tcp_enabled || udp_enabled);

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

    uv_run(&loop, UV_RUN_DEFAULT);

    mcp_server_destroy(server);
    return close_loop(&loop) == 0 ? 0 : 1;
}
