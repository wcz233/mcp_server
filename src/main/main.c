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

int main(void)
{
    uv_loop_t loop;
    struct mcp_server *server = NULL;
    struct mcp_server_config config = {
        .strict_initialized_notification = env_bool("MCP_STRICT_INIT", true),
        .max_line_bytes = 1024 * 1024,
    };
    int stdin_fd;
    int stdout_fd;
    int rc;

    if (prepare_stdio() != 0) {
        fprintf(stderr, "prepare_stdio failed\n");
        return 1;
    }

    stdin_fd = stdin_fileno_value();
    stdout_fd = stdout_fileno_value();
    if (stdin_fd < 0 || stdout_fd < 0) {
        fprintf(stderr, "invalid stdio handles\n");
        return 1;
    }

    rc = uv_loop_init(&loop);
    if (rc != 0) {
        fprintf(stderr, "uv_loop_init: %s\n", uv_strerror(rc));
        return 1;
    }

    rc = mcp_server_init(&server, &loop, config);
    if (rc != 0) {
        fprintf(stderr, "mcp_server_init failed\n");
        uv_loop_close(&loop);
        return 1;
    }

    rc = mcp_server_start_stdio(server, stdin_fd, stdout_fd);
    if (rc != 0) {
        fprintf(stderr, "mcp_server_start_stdio failed\n");
        mcp_server_destroy(server);
        uv_loop_close(&loop);
        return 1;
    }

    uv_run(&loop, UV_RUN_DEFAULT);

    mcp_server_destroy(server);
    uv_loop_close(&loop);
    return 0;
}
