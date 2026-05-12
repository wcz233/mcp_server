#ifndef MCP_PLUGIN_PLUGIN_ABI_H
#define MCP_PLUGIN_PLUGIN_ABI_H

#ifdef _WIN32
#define MCP_PLUGIN_EXPORT __declspec(dllexport)
#else
#define MCP_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

#define MCP_PLUGIN_ABI_VERSION "1.0"

struct mcp_plugin_host_api {
    void (*log_info)(const char *message);
    void (*log_error)(const char *message);
    int (*check_permission)(const char *permission);
    unsigned long long (*now_ms)(void);
};

typedef int (*mcp_plugin_init_fn)(const struct mcp_plugin_host_api *host,
                                  const char *config_json,
                                  char *result_json,
                                  unsigned int result_size);
typedef int (*mcp_plugin_invoke_fn)(const char *tool_name,
                                    const char *arguments_json,
                                    char *result_json,
                                    unsigned int result_size);
typedef int (*mcp_plugin_shutdown_fn)(void);

#endif
