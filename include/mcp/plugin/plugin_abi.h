#ifndef MCP_PLUGIN_PLUGIN_ABI_H
#define MCP_PLUGIN_PLUGIN_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#define MCP_PLUGIN_EXPORT __declspec(dllexport)
#else
#define MCP_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

#define MCP_PLUGIN_ABI_VERSION "1.1"

enum mcp_plugin_call_status {
    MCP_PLUGIN_CALL_OK = 0,
    MCP_PLUGIN_CALL_ERROR = 1,
    MCP_PLUGIN_CALL_PENDING = 2,
};

typedef void (*mcp_plugin_frame_handler_fn)(void *user_data,
                                            unsigned int server_id,
                                            const void *payload,
                                            uint32_t payload_len);
typedef void (*mcp_plugin_peer_event_fn)(void *user_data, unsigned int server_id);

struct mcp_plugin_tool_descriptor {
    const char *name;
    const char *description;
    const char *input_schema_json;
    const char *source;
    const char *version;
    const char *risk_level;
    const char *permission;
    int idempotent;
    int retryable;
    int cancelable;
    uint32_t timeout_ms;
    const char *handler_name;
};

struct mcp_plugin_host_api {
    const char *abi_version;
    void *host_context;

    void (*log_info)(void *host_context, const char *message);
    void (*log_error)(void *host_context, const char *message);
    int (*check_permission)(void *host_context, const char *permission);
    unsigned long long (*now_ms)(void *host_context);
    void *(*get_loop)(void *host_context);

    int (*register_tool)(void *host_context,
                         const struct mcp_plugin_tool_descriptor *descriptor);
    int (*unregister_tool)(void *host_context, const char *tool_name);

    int (*complete_async_ok)(void *host_context,
                             const char *invocation_id,
                             const char *payload_json);
    int (*complete_async_error)(void *host_context,
                                const char *invocation_id,
                                const char *message);

    int (*peer_transport_send_frame)(void *host_context,
                                     unsigned int server_id,
                                     const void *payload,
                                     uint32_t payload_len);
    int (*peer_transport_register_handler)(void *host_context,
                                           const char magic[4],
                                           mcp_plugin_frame_handler_fn on_frame,
                                           mcp_plugin_peer_event_fn on_peer_connected,
                                           mcp_plugin_peer_event_fn on_peer_closed,
                                           void *user_data);
    int (*peer_transport_unregister_handler)(void *host_context, const char magic[4]);
    int (*peer_transport_has_capability)(void *host_context,
                                         unsigned int server_id,
                                         const char *capability);
    int (*peer_transport_set_capability)(void *host_context,
                                         unsigned int server_id,
                                         const char *capability,
                                         int enabled);
};

typedef int (*mcp_plugin_init_fn)(const struct mcp_plugin_host_api *host,
                                  const char *config_json,
                                  char *result_json,
                                  unsigned int result_size);
typedef int (*mcp_plugin_invoke_fn)(const char *invocation_id,
                                    const char *tool_name,
                                    const char *arguments_json,
                                    char *result_json,
                                    unsigned int result_size);
typedef int (*mcp_plugin_shutdown_fn)(void);

#endif
