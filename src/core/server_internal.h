#ifndef MCP_SRC_CORE_SERVER_INTERNAL_H
#define MCP_SRC_CORE_SERVER_INTERNAL_H

#include <stdint.h>

#include "core/in_flight.h"
#include "mcp/core/server.h"
#include "mcp/gateway/gateway.h"
#include "mcp/registry/tool_registry.h"
#include "discovery/server_discovery.h"
#include "listener/framed_listener.h"
#include "network/network_access_policy.h"
#include "plugin/plugin_manager.h"
#if MCP_TCP_SECURITY_MTLS
#include "security/tls_context.h"
#endif
#include "transport/peer_transport.h"
#include "transport/stdio_transport.h"
#include "transport/udp_transport.h"

struct mcp_shell_job_store;
struct mcp_shell_sandbox_control;
struct mcp_shell_policy_snapshot;

enum mcp_session_state {
    MCP_SESSION_NOT_INITIALIZED = 0,
    MCP_SESSION_AWAIT_CLIENT_INITIALIZED,
    MCP_SESSION_INITIALIZED,
};

struct mcp_client_session {
    enum mcp_session_state state;
    struct mcp_reply_target reply_to;
    json_t *tool_snapshot;
    uint32_t peer_server_id;
    bool data_channel;
    struct mcp_client_session *next;
};

struct mcp_server {
    uv_loop_t *loop;
    struct mcp_server_config config;

    struct mcp_stdio_transport *stdio;
    struct mcp_udp_transport *udp;
    struct mcp_framed_listener *pipe_listener;
    struct mcp_framed_listener *tcp_listener;
    bool stdio_started;
    bool core_async_initialized;
    uv_async_t core_async;
    enum mcp_session_state session_state;
    bool shutting_down;

    struct mcp_message_node *queue_head;
    struct mcp_message_node *queue_tail;

    struct mcp_in_flight_map in_flight;
    struct mcp_tool_registry *registry;
    struct mcp_gateway *gateway;
    struct mcp_plugin_manager *plugin_manager;
    struct mcp_peer_transport *peer_transport;
    struct mcp_server_discovery *discovery;
    struct mcp_network_access_policy *network_access_policy;
#if MCP_TCP_SECURITY_MTLS
    struct mcp_tls_context *tls_context;
#endif
    bool tcp_mtls_enabled;
    struct mcp_shell_policy_snapshot *shell_policy_snapshot;
    struct mcp_shell_sandbox_control *sandbox_control;
    struct mcp_shell_job_store *shell_jobs;
    char *tcp_host;
    unsigned int tcp_port;

    struct mcp_client_session stdio_session;
    struct mcp_client_session *peer_sessions;
};

int mcp_server_send_result(struct mcp_server *server, json_t *id, json_t *result);
int mcp_server_send_error(struct mcp_server *server, json_t *id, int code, const char *message);
bool mcp_server_udp_enabled(const struct mcp_server *server);
bool mcp_server_stdio_enabled(const struct mcp_server *server);
bool mcp_server_pipe_enabled(const struct mcp_server *server);
bool mcp_server_tcp_enabled(const struct mcp_server *server);
void mcp_server_request_shutdown(struct mcp_server *server);

void mcp_server_complete_async_ok(struct mcp_server *server,
                                  const char *id_key,
                                  json_t *result);
int mcp_server_start_discovery(struct mcp_server *server,
                               const struct mcp_server_discovery_config *config);
bool mcp_server_discovery_enabled(const struct mcp_server *server);
int mcp_server_configure_tcp_security(struct mcp_server *server,
                                      bool mtls_enabled,
                                      const char *ca_file,
                                      const char *certificate_file,
                                      const char *private_key_file);
bool mcp_server_tcp_security_mtls(const struct mcp_server *server);
bool mcp_server_network_access_enabled(const struct mcp_server *server);
bool mcp_server_network_address_allowed(const struct mcp_server *server,
                                        const struct sockaddr *addr);
bool mcp_server_network_ip_allowed(const struct mcp_server *server, const char *ip);
size_t mcp_server_network_discovery_peer_count(const struct mcp_server *server);
bool mcp_server_network_discovery_peer_at(const struct mcp_server *server,
                                          size_t index,
                                          const char **ip,
                                          unsigned int *discovery_port);

#endif
