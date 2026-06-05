#include "mcp/core/server.h"

#include "common/platform.h"
#include "core/in_flight.h"
#include "core/server_internal.h"
#include "protocol/jsonrpc.h"
#if MCP_HAS_FILE_TRANSFER_PLUGIN_BUILTIN
#include "plugins/file_transfer/file_transfer_plugin.h"
#endif
#include "tools/builtin_tools.h"
#include "tools/shell_exec.h"
#include "tools/tool_result.h"

#include <jansson.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

struct mcp_message_node {
    struct mcp_jsonrpc_message *message;
    struct mcp_reply_target reply_to;
    struct mcp_message_node *next;
};

static void client_session_cleanup(struct mcp_client_session *session);
static struct mcp_client_session *client_session_for_reply(struct mcp_server *server,
                                                           const struct mcp_reply_target *reply_to,
                                                           bool create_if_missing);
static bool reply_targets_equal(const struct mcp_reply_target *lhs,
                                const struct mcp_reply_target *rhs);
static void close_runtime_handles(struct mcp_server *server);

static void core_async_cb(uv_async_t *handle);
static void stdio_on_line(void *arg, const char *line, size_t len);
static void stdio_on_exit(void *arg);
#if MCP_HAS_TRANSPORT_UDP
static void udp_on_datagram(void *arg,
                            const char *data,
                            size_t len,
                            const struct sockaddr *peer);
static void udp_on_error(void *arg, int status);
#endif
static void framed_on_message(void *arg,
                              struct mcp_framed_connection *conn,
                              const char *data,
                              size_t len);
static void framed_on_close(void *arg, struct mcp_framed_connection *conn);

uv_loop_t *mcp_server_loop(struct mcp_server *server)
{
    return server->loop;
}

static int send_json_object(struct mcp_server *server,
                            const struct mcp_reply_target *reply_to,
                            json_t *object)
{
    char *line = mcp_jsonrpc_dump_line(object);
    int rc;

    if (!line)
        return -1;

    if (reply_to && reply_to->transport == MCP_REPLY_STREAM) {
        size_t len = strlen(line);

        if (len > 0 && line[len - 1] == '\n')
            len--;
        rc = mcp_framed_connection_send(reply_to->stream, line, len);
    } else if (reply_to && reply_to->transport == MCP_REPLY_UDP) {
#if MCP_HAS_TRANSPORT_UDP
        rc = mcp_udp_transport_send(server->udp,
                                    line,
                                    strlen(line),
                                    (const struct sockaddr *)&reply_to->udp_peer);
#else
        rc = -1;
#endif
    } else {
        rc = mcp_stdio_transport_send_str(server->stdio, line);
    }
    free(line);
    return rc;
}

static int send_result_to(struct mcp_server *server,
                          const struct mcp_reply_target *reply_to,
                          json_t *id,
                          json_t *result)
{
    json_t *object = mcp_jsonrpc_build_response(id, result);
    int rc = send_json_object(server, reply_to, object);

    json_decref(object);
    return rc;
}

static int send_error_to(struct mcp_server *server,
                         const struct mcp_reply_target *reply_to,
                         json_t *id,
                         int code,
                         const char *message)
{
    json_t *object = mcp_jsonrpc_build_error(id, code, message);
    int rc = send_json_object(server, reply_to, object);

    json_decref(object);
    return rc;
}

int mcp_server_send_result(struct mcp_server *server, json_t *id, json_t *result)
{
    return send_result_to(server, NULL, id, result);
}

int mcp_server_send_error(struct mcp_server *server, json_t *id, int code, const char *message)
{
    return send_error_to(server, NULL, id, code, message);
}

static bool reply_targets_equal(const struct mcp_reply_target *lhs,
                                const struct mcp_reply_target *rhs)
{
    size_t len;

    if (lhs->transport != rhs->transport)
        return false;
    if (lhs->transport == MCP_REPLY_STDIO)
        return true;
    if (lhs->transport == MCP_REPLY_STREAM)
        return lhs->stream == rhs->stream;
    if (lhs->transport != MCP_REPLY_UDP)
        return false;

    if (lhs->udp_peer.ss_family == AF_INET && rhs->udp_peer.ss_family == AF_INET)
        len = sizeof(struct sockaddr_in);
    else if (lhs->udp_peer.ss_family == AF_INET6 && rhs->udp_peer.ss_family == AF_INET6)
        len = sizeof(struct sockaddr_in6);
    else
        return false;

    return memcmp(&lhs->udp_peer, &rhs->udp_peer, len) == 0;
}

static void client_session_cleanup(struct mcp_client_session *session)
{
    if (!session)
        return;

    json_decref(session->tool_snapshot);
    session->tool_snapshot = NULL;
    session->state = MCP_SESSION_NOT_INITIALIZED;
}

static struct mcp_client_session *client_session_for_reply(struct mcp_server *server,
                                                           const struct mcp_reply_target *reply_to,
                                                           bool create_if_missing)
{
    struct mcp_reply_target stdio_reply = {0};
    struct mcp_client_session *session;
    struct mcp_client_session *created;

    stdio_reply.transport = MCP_REPLY_STDIO;
    if (!reply_to)
        reply_to = &stdio_reply;

    if (reply_to->transport == MCP_REPLY_STDIO)
        return &server->stdio_session;

    for (session = server->peer_sessions; session; session = session->next) {
        if (reply_targets_equal(&session->reply_to, reply_to))
            return session;
    }

    if (!create_if_missing)
        return NULL;

    created = calloc(1, sizeof(*created));
    if (!created)
        return NULL;

    created->reply_to = *reply_to;
    created->state = MCP_SESSION_NOT_INITIALIZED;
    created->next = server->peer_sessions;
    server->peer_sessions = created;
    return created;
}

static void client_session_remove_reply(struct mcp_server *server,
                                        const struct mcp_reply_target *reply_to)
{
    struct mcp_client_session **current = &server->peer_sessions;

    while (*current) {
        struct mcp_client_session *session = *current;

        if (reply_targets_equal(&session->reply_to, reply_to)) {
            *current = session->next;
            client_session_cleanup(session);
            free(session);
            return;
        }

        current = &session->next;
    }
}

static bool gate_allows_method(const struct mcp_client_session *session, const char *method)
{
    if (strcmp(method, "ping") == 0)
        return true;
    if (strcmp(method, "initialize") == 0)
        return session && session->state == MCP_SESSION_NOT_INITIALIZED;
    return session && session->state == MCP_SESSION_INITIALIZED;
}

static json_t *build_initialize_result(void)
{
    json_t *result = json_object();
    json_t *capabilities = json_object();
    json_t *server_info = json_object();

    json_object_set_new(result, "protocolVersion", json_string("2024-11-05"));

    json_object_set_new(capabilities, "tools", json_object());
    json_object_set_new(capabilities, "resources", json_object());
    json_object_set_new(capabilities, "prompts", json_object());
    json_object_set_new(result, "capabilities", capabilities);

    json_object_set_new(server_info, "name", json_string("mcp_server"));
    json_object_set_new(server_info, "version", json_string(MCP_SERVER_VERSION));
    json_object_set_new(result, "serverInfo", server_info);

    return result;
}

static void queue_message(struct mcp_server *server,
                          struct mcp_jsonrpc_message *message,
                          const struct mcp_reply_target *reply_to)
{
    struct mcp_message_node *node = calloc(1, sizeof(*node));

    if (!node) {
        mcp_jsonrpc_message_destroy(message);
        return;
    }

    node->message = message;
    if (reply_to)
        node->reply_to = *reply_to;
    else
        node->reply_to.transport = MCP_REPLY_STDIO;
    if (!server->queue_tail) {
        server->queue_head = node;
        server->queue_tail = node;
    } else {
        server->queue_tail->next = node;
        server->queue_tail = node;
    }

    uv_async_send(&server->core_async);
}

static struct mcp_message_node *dequeue_message(struct mcp_server *server)
{
    struct mcp_message_node *node = server->queue_head;

    if (!node)
        return NULL;

    server->queue_head = node->next;
    if (!server->queue_head)
        server->queue_tail = NULL;

    node->next = NULL;
    return node;
}

static void free_in_flight_entry(struct mcp_in_flight_entry *entry)
{
    if (entry->op_free && entry->op_ctx)
        entry->op_free(entry->op_ctx);
    json_decref(entry->id);
    free(entry->id_key);
    free(entry->invocation_id);
    free(entry);
}

static void maybe_shutdown(struct mcp_server *server)
{
    if (!server->shutting_down)
        return;
    if (server->queue_head)
        return;
    if (server->in_flight.size != 0)
        return;

    mcp_server_discovery_close(server->discovery);
#if MCP_HAS_TRANSPORT_UDP
    mcp_udp_transport_close(server->udp);
#endif
    mcp_framed_listener_close(server->pipe_listener);
    mcp_framed_listener_close(server->tcp_listener);
    mcp_stdio_transport_close_output(server->stdio);
    if (!uv_is_closing((uv_handle_t *)&server->core_async))
        uv_close((uv_handle_t *)&server->core_async, NULL);
}

void mcp_server_request_shutdown(struct mcp_server *server)
{
    if (!server || server->shutting_down)
        return;

    server->shutting_down = true;
    if (server->core_async_initialized &&
        !uv_is_closing((uv_handle_t *)&server->core_async))
        uv_async_send(&server->core_async);
}

static void close_runtime_handles(struct mcp_server *server)
{
    if (!server)
        return;

    server->shutting_down = true;
    mcp_server_discovery_close(server->discovery);
#if MCP_HAS_TRANSPORT_UDP
    mcp_udp_transport_close(server->udp);
#endif
    mcp_framed_listener_close(server->pipe_listener);
    mcp_framed_listener_close(server->tcp_listener);
    mcp_stdio_transport_close(server->stdio);
    mcp_shell_jobs_shutdown(server->shell_jobs);
    if (server->core_async_initialized &&
        !uv_is_closing((uv_handle_t *)&server->core_async))
        uv_close((uv_handle_t *)&server->core_async, NULL);

    while (server->loop && uv_loop_alive(server->loop))
        uv_run(server->loop, UV_RUN_DEFAULT);
}

static bool tool_visible_in_snapshot(const json_t *snapshot, const char *tool_name)
{
    size_t index;
    json_t *tool;
    json_t *tools;

    if (!snapshot)
        return false;

    tools = json_object_get(snapshot, "tools");
    if (!json_is_array(tools))
        return false;

    json_array_foreach(tools, index, tool) {
        json_t *name = json_object_get(tool, "name");
        if (json_is_string(name) && strcmp(json_string_value(name), tool_name) == 0)
            return true;
    }

    return false;
}

static int refresh_tool_snapshot(struct mcp_client_session *session,
                                 struct mcp_tool_registry *registry)
{
    json_t *snapshot = mcp_tool_registry_public_list(registry);

    if (!snapshot)
        return -1;
    json_decref(session->tool_snapshot);
    session->tool_snapshot = snapshot;
    return 0;
}

static bool tool_call_mutates_registry(const char *tool_name)
{
    return strcmp(tool_name, "plugin_tools.insmod") == 0 ||
           strcmp(tool_name, "plugin_tools.rmmod") == 0;
}

static bool tool_call_is_list_servers(const char *tool_name)
{
    return strcmp(tool_name, MCP_SERVER_LIST_SERVERS_TOOL) == 0;
}

static int register_builtin_plugins(struct mcp_server *server)
{
#if MCP_HAS_FILE_TRANSFER_PLUGIN_BUILTIN
    static const struct mcp_builtin_plugin_descriptor file_transfer = {
        "mcp_file_transfer_plugin",
        "builtin:mcp_file_transfer_plugin",
        mcp_file_transfer_plugin_init,
        mcp_file_transfer_plugin_invoke,
        mcp_file_transfer_plugin_shutdown,
    };

    if (mcp_plugin_manager_register_builtin(server->plugin_manager, &file_transfer, "{}") != 0)
        return -1;
#else
    (void)server;
#endif

    return 0;
}

static int extract_tool_call(json_t *params,
                             const char **out_name,
                             json_t **out_arguments)
{
    json_t *name;
    json_t *arguments;

    *out_name = NULL;
    *out_arguments = NULL;

    if (!params || !json_is_object(params))
        return -1;

    name = json_object_get(params, "name");
    if (!json_is_string(name))
        return -1;

    arguments = json_object_get(params, "arguments");
    if (!arguments)
        arguments = json_object_get(params, "args");
    if (!arguments)
        arguments = json_object();
    else
        json_incref(arguments);

    if (!json_is_object(arguments)) {
        json_decref(arguments);
        return -1;
    }

    *out_name = json_string_value(name);
    *out_arguments = arguments;
    return 0;
}

static void handle_cancelled(struct mcp_server *server,
                             const struct mcp_reply_target *reply_to,
                             json_t *params)
{
    json_t *request_id;
    char *key = NULL;
    struct mcp_in_flight_entry *entry;

    if (!params || !json_is_object(params))
        return;

    request_id = json_object_get(params, "request_id");
    if (!request_id)
        request_id = json_object_get(params, "requestId");
    if (!request_id)
        return;

    if (!mcp_jsonrpc_id_to_key(request_id, &key))
        return;

    entry = mcp_in_flight_get(&server->in_flight, key);
    if (entry) {
        entry->cancelled = true;
        if (entry->op_ctx && entry->op_free) {
            entry->op_free(entry->op_ctx);
            entry->op_ctx = NULL;
            entry->op_free = NULL;
        }
        send_error_to(server, &entry->reply_to, entry->id, -32603, "Cancelled");
        entry = mcp_in_flight_remove(&server->in_flight, key);
        if (entry)
            free_in_flight_entry(entry);
    }

    free(key);
    (void)reply_to;
}

static void handle_request(struct mcp_server *server,
                           const struct mcp_reply_target *reply_to,
                           struct mcp_jsonrpc_message *message)
{
    struct mcp_client_session *session;
    json_t *result;
    json_t *error;
    char *id_key = NULL;
    struct mcp_in_flight_entry *entry = NULL;
    const char *tool_name;
    json_t *arguments = NULL;
    int rc;

    session = client_session_for_reply(server, reply_to, true);
    if (!session) {
        send_error_to(server, reply_to, message->id, -32603, "Internal error");
        return;
    }

    if (!gate_allows_method(session, message->method)) {
        send_error_to(server, reply_to, message->id, -32600, "Session not initialized");
        return;
    }

    if (strcmp(message->method, "ping") == 0) {
        result = json_object();
        send_result_to(server, reply_to, message->id, result);
        json_decref(result);
        return;
    }

    if (strcmp(message->method, "initialize") == 0) {
        json_t *peer_identity = json_object_get(message->params, "mcp_peer_identity");

        if (peer_identity && mcp_server_discovery_enabled(server))
            session->peer_server_id =
                mcp_server_discovery_note_peer_identity(server->discovery, peer_identity);

        result = build_initialize_result();
        send_result_to(server, reply_to, message->id, result);
        json_decref(result);

        if (server->config.strict_initialized_notification)
            session->state = MCP_SESSION_AWAIT_CLIENT_INITIALIZED;
        else
            session->state = MCP_SESSION_INITIALIZED;
        return;
    }

    if (strcmp(message->method, "tools/list") == 0) {
        if (refresh_tool_snapshot(session, server->registry) != 0) {
            send_error_to(server, reply_to, message->id, -32603, "Internal error");
            return;
        }
        send_result_to(server, reply_to, message->id, session->tool_snapshot);
        return;
    }

    if (strcmp(message->method, "tools/call") == 0) {
        if (extract_tool_call(message->params, &tool_name, &arguments) != 0) {
            send_error_to(server, reply_to, message->id, -32602, "Invalid params");
            return;
        }

        if (!session->tool_snapshot)
            session->tool_snapshot = mcp_tool_registry_public_list(server->registry);

        if (!tool_visible_in_snapshot(session->tool_snapshot, tool_name)) {
            json_decref(arguments);
            result = mcp_tool_result_text(
                "Tool is not visible in current session snapshot. Call tools/list to refresh this session.",
                true);
            send_result_to(server, reply_to, message->id, result);
            json_decref(result);
            return;
        }

        if (!mcp_jsonrpc_id_to_key(message->id, &id_key)) {
            json_decref(arguments);
            send_error_to(server, reply_to, message->id, -32603, "Internal error");
            return;
        }

        entry = mcp_in_flight_put(&server->in_flight, id_key, message->id, reply_to);
        if (!entry) {
            free(id_key);
            json_decref(arguments);
            send_error_to(server, reply_to, message->id, -32603, "Internal error");
            return;
        }

        if (tool_call_is_list_servers(tool_name)) {
            json_t *wait_value = json_object_get(arguments, "wait_ms");
            unsigned int wait_ms = 0;

            if (json_is_integer(wait_value) &&
                json_integer_value(wait_value) > 0 &&
                json_integer_value(wait_value) <= 5000)
                wait_ms = (unsigned int)json_integer_value(wait_value);

            if (mcp_server_discovery_list_async(server->discovery, id_key, wait_ms) == 0) {
                json_decref(arguments);
                free(id_key);
                return;
            }

            json_decref(arguments);
            entry = mcp_in_flight_remove(&server->in_flight, id_key);
            if (entry)
                free_in_flight_entry(entry);
            free(id_key);
            result = mcp_tool_result_text("Server discovery is not enabled.", true);
            send_result_to(server, reply_to, message->id, result);
            json_decref(result);
            return;
        }

        result = NULL;
        error = NULL;
        rc = mcp_gateway_call(server->gateway,
                              server,
                              id_key,
                              entry->invocation_id,
                              tool_name,
                              arguments,
                              session->tool_snapshot,
                              &result,
                              &error);
        json_decref(arguments);

        if (rc == MCP_GATEWAY_PENDING) {
            free(id_key);
            return;
        }

        if (tool_call_mutates_registry(tool_name) &&
            refresh_tool_snapshot(session, server->registry) != 0) {
            if (result)
                json_decref(result);
            if (error)
                json_decref(error);
            entry = mcp_in_flight_remove(&server->in_flight, id_key);
            if (entry)
                free_in_flight_entry(entry);
            free(id_key);
            send_error_to(server, reply_to, message->id, -32603, "Internal error");
            return;
        }

        if (rc == MCP_GATEWAY_OK || rc == MCP_GATEWAY_TOOL_ERROR) {
            send_result_to(server, reply_to, message->id, result);
            json_decref(result);
        } else {
            send_json_object(server, reply_to, error);
            json_decref(error);
        }

        entry = mcp_in_flight_remove(&server->in_flight, id_key);
        if (entry)
            free_in_flight_entry(entry);
        free(id_key);
        return;
    }

    if (strcmp(message->method, "resources/list") == 0) {
        result = json_pack("{s:[]}", "resources");
        send_result_to(server, reply_to, message->id, result);
        json_decref(result);
        return;
    }

    if (strcmp(message->method, "resources/templates/list") == 0) {
        result = json_pack("{s:[]}", "resourceTemplates");
        send_result_to(server, reply_to, message->id, result);
        json_decref(result);
        return;
    }

    if (strcmp(message->method, "prompts/list") == 0) {
        result = json_pack("{s:[]}", "prompts");
        send_result_to(server, reply_to, message->id, result);
        json_decref(result);
        return;
    }

    send_error_to(server, reply_to, message->id, -32601, "Method not found");
}

static void handle_notification(struct mcp_server *server,
                                const struct mcp_reply_target *reply_to,
                                struct mcp_jsonrpc_message *message)
{
    struct mcp_client_session *session;

    if (strcmp(message->method, "notifications/initialized") == 0) {
        session = client_session_for_reply(server, reply_to, false);
        if (session && session->state == MCP_SESSION_AWAIT_CLIENT_INITIALIZED)
            session->state = MCP_SESSION_INITIALIZED;
        return;
    }

    if (strcmp(message->method, "notifications/cancelled") == 0) {
        handle_cancelled(server, reply_to, message->params);
        return;
    }
}

static void core_async_cb(uv_async_t *handle)
{
    struct mcp_server *server = handle->data;
    struct mcp_message_node *node;

    while ((node = dequeue_message(server)) != NULL) {
        if (node->message->type == MCP_JSONRPC_REQUEST)
            handle_request(server, &node->reply_to, node->message);
        else
            handle_notification(server, &node->reply_to, node->message);

        mcp_jsonrpc_message_destroy(node->message);
        free(node);
    }

    maybe_shutdown(server);
}

static void stdio_on_line(void *arg, const char *line, size_t len)
{
    struct mcp_server *server = arg;
    struct mcp_jsonrpc_message *message = NULL;
    struct mcp_reply_target reply_to;
    json_t *error = NULL;

    if (server->shutting_down)
        return;

    memset(&reply_to, 0, sizeof(reply_to));
    reply_to.transport = MCP_REPLY_STDIO;

    if (mcp_jsonrpc_parse_line(line, len, &message, &error) != 0) {
        send_json_object(server, &reply_to, error);
        json_decref(error);
        return;
    }

    queue_message(server, message, &reply_to);
}

static void stdio_on_exit(void *arg)
{
    struct mcp_server *server = arg;

    if (server->config.stdio_eof_shutdown)
        server->shutting_down = true;
    else {
        client_session_cleanup(&server->stdio_session);
        mcp_stdio_transport_close_output(server->stdio);
    }
    uv_async_send(&server->core_async);
}

#if MCP_HAS_TRANSPORT_UDP
static void udp_on_datagram(void *arg,
                            const char *data,
                            size_t len,
                            const struct sockaddr *peer)
{
    struct mcp_server *server = arg;
    struct mcp_jsonrpc_message *message = NULL;
    struct mcp_reply_target reply_to;
    json_t *error = NULL;
    size_t peer_len;

    if (server->shutting_down)
        return;

    if (peer->sa_family == AF_INET)
        peer_len = sizeof(struct sockaddr_in);
    else if (peer->sa_family == AF_INET6)
        peer_len = sizeof(struct sockaddr_in6);
    else
        return;

    memset(&reply_to, 0, sizeof(reply_to));
    reply_to.transport = MCP_REPLY_UDP;
    memcpy(&reply_to.udp_peer, peer, peer_len);

    if (mcp_jsonrpc_parse_line(data, len, &message, &error) != 0) {
        send_json_object(server, &reply_to, error);
        json_decref(error);
        return;
    }

    if (message->type == MCP_JSONRPC_NOTIFICATION &&
        strcmp(message->method, MCP_SERVER_DISCOVERY_OFFLINE_METHOD) == 0 &&
        mcp_server_discovery_handle_offline_notification(server->discovery, message->params)) {
        mcp_jsonrpc_message_destroy(message);
        return;
    }

    queue_message(server, message, &reply_to);
}

static void udp_on_error(void *arg, int status)
{
    (void)arg;
    (void)status;
}
#endif

static bool framed_maybe_dispatch_binary(struct mcp_server *server,
                                         struct mcp_framed_connection *conn,
                                         const char *data,
                                         size_t len)
{
    struct mcp_reply_target reply_to;
    struct mcp_client_session *session;

    if (!data || len == 0 || data[0] == '{')
        return false;

    memset(&reply_to, 0, sizeof(reply_to));
    reply_to.transport = MCP_REPLY_STREAM;
    reply_to.stream = conn;
    session = client_session_for_reply(server, &reply_to, false);
    if (!session || session->peer_server_id == 0)
        return false;

    return mcp_peer_transport_dispatch_frame(server->peer_transport,
                                             session->peer_server_id,
                                             data,
                                             len) == 0;
}

static void framed_on_message(void *arg,
                              struct mcp_framed_connection *conn,
                              const char *data,
                              size_t len)
{
    struct mcp_server *server = arg;
    struct mcp_jsonrpc_message *message = NULL;
    struct mcp_reply_target reply_to;
    json_t *error = NULL;

    if (server->shutting_down)
        return;

    if (framed_maybe_dispatch_binary(server, conn, data, len))
        return;

    memset(&reply_to, 0, sizeof(reply_to));
    reply_to.transport = MCP_REPLY_STREAM;
    reply_to.stream = conn;

    if (mcp_jsonrpc_parse_line(data, len, &message, &error) != 0) {
        send_json_object(server, &reply_to, error);
        json_decref(error);
        return;
    }

    if (message->type == MCP_JSONRPC_NOTIFICATION &&
        strcmp(message->method, MCP_SERVER_DISCOVERY_OFFLINE_METHOD) == 0 &&
        mcp_server_discovery_handle_offline_notification(server->discovery, message->params)) {
        mcp_jsonrpc_message_destroy(message);
        return;
    }

    queue_message(server, message, &reply_to);
}

static void framed_on_close(void *arg, struct mcp_framed_connection *conn)
{
    struct mcp_server *server = arg;
    struct mcp_reply_target reply_to;

    memset(&reply_to, 0, sizeof(reply_to));
    reply_to.transport = MCP_REPLY_STREAM;
    reply_to.stream = conn;
    client_session_remove_reply(server, &reply_to);
}

void mcp_server_complete_async_ok(struct mcp_server *server, const char *id_key, json_t *result)
{
    struct mcp_in_flight_entry *entry = mcp_in_flight_get(&server->in_flight, id_key);

    if (!entry)
        return;

    send_result_to(server, &entry->reply_to, entry->id, result);
    entry = mcp_in_flight_remove(&server->in_flight, id_key);
    if (entry)
        free_in_flight_entry(entry);
    maybe_shutdown(server);
}

int mcp_server_init(struct mcp_server **out, uv_loop_t *loop, struct mcp_server_config config)
{
    struct mcp_server *server;

    *out = NULL;

    server = calloc(1, sizeof(*server));
    if (!server)
        return -1;

    server->loop = loop;
    server->config = config;
    server->stdio_session.reply_to.transport = MCP_REPLY_STDIO;
    server->stdio_session.state = MCP_SESSION_NOT_INITIALIZED;
    mcp_in_flight_init(&server->in_flight);

    if (mcp_stdio_transport_create(&server->stdio,
                                   loop,
                                   (struct mcp_stdio_transport_config){
                                       .max_line_bytes = config.max_line_bytes,
                                   }) != 0) {
        free(server);
        return -1;
    }

#if MCP_HAS_TRANSPORT_UDP
    if (mcp_udp_transport_create(&server->udp,
                                 loop,
                                 (struct mcp_udp_transport_config){
                                     .max_datagram_bytes = config.max_line_bytes,
                                 }) != 0) {
        mcp_stdio_transport_destroy(server->stdio);
        free(server);
        return -1;
    }
#endif

    if (uv_async_init(loop, &server->core_async, core_async_cb) != 0) {
        mcp_server_destroy(server);
        return -1;
    }
    server->core_async_initialized = true;
    server->core_async.data = server;

    if (mcp_tool_registry_create(&server->registry) != 0 ||
        mcp_peer_transport_create(&server->peer_transport,
                                  loop,
                                  config.max_line_bytes,
                                  4 * config.max_line_bytes) != 0 ||
        mcp_plugin_manager_create(&server->plugin_manager, server) != 0 ||
        mcp_gateway_create(&server->gateway, server->registry) != 0 ||
        mcp_server_discovery_create(&server->discovery, server, loop) != 0 ||
        mcp_shell_jobs_create(&server->shell_jobs, server, loop) != 0 ||
        register_builtin_plugins(server) != 0 ||
        mcp_register_builtin_tools(server, server->registry) != 0) {
        mcp_server_destroy(server);
        return -1;
    }

    *out = server;
    return 0;
}

void mcp_server_destroy(struct mcp_server *server)
{
    struct mcp_message_node *node;
    struct mcp_client_session *session;

    if (!server)
        return;

    close_runtime_handles(server);

    while ((node = dequeue_message(server)) != NULL) {
        mcp_jsonrpc_message_destroy(node->message);
        free(node);
    }

    client_session_cleanup(&server->stdio_session);
    session = server->peer_sessions;
    while (session) {
        struct mcp_client_session *next = session->next;
        client_session_cleanup(session);
        free(session);
        session = next;
    }
    mcp_shell_jobs_shutdown(server->shell_jobs);
    mcp_gateway_destroy(server->gateway);
    mcp_plugin_manager_destroy(server->plugin_manager);
    mcp_server_discovery_destroy(server->discovery);
    mcp_shell_jobs_destroy(server->shell_jobs);
    mcp_peer_transport_destroy(server->peer_transport);
    mcp_tool_registry_destroy(server->registry);
    mcp_in_flight_destroy(&server->in_flight);
#if MCP_HAS_TRANSPORT_UDP
    mcp_udp_transport_destroy(server->udp);
#endif
    mcp_framed_listener_destroy(server->pipe_listener);
    mcp_framed_listener_destroy(server->tcp_listener);
    mcp_stdio_transport_destroy(server->stdio);
    free(server->tcp_host);
    free(server);
}

int mcp_server_start_stdio(struct mcp_server *server, int stdin_fd, int stdout_fd)
{
    if (mcp_stdio_transport_open(server->stdio, stdin_fd, stdout_fd) != 0)
        return -1;

    if (mcp_stdio_transport_start(server->stdio, stdio_on_line, stdio_on_exit, server) != 0)
        return -1;

    server->stdio_started = true;
    return 0;
}

int mcp_server_start_udp(struct mcp_server *server, const char *bind_host, unsigned int bind_port)
{
#if MCP_HAS_TRANSPORT_UDP
    if (!server->udp)
        return -1;

    if (mcp_udp_transport_open(server->udp, bind_host, bind_port) != 0)
        return -1;

    if (mcp_udp_transport_start(server->udp, udp_on_datagram, udp_on_error, server) != 0)
        return -1;

    return 0;
#else
    (void)server;
    (void)bind_host;
    (void)bind_port;
    return -1;
#endif
}

bool mcp_server_udp_enabled(const struct mcp_server *server)
{
#if MCP_HAS_TRANSPORT_UDP
    return server && mcp_udp_transport_is_open(server->udp);
#else
    (void)server;
    return false;
#endif
}

bool mcp_server_stdio_enabled(const struct mcp_server *server)
{
    return server && server->stdio_started;
}

int mcp_server_start_pipe(struct mcp_server *server, const char *path)
{
    if (!server || !path || server->pipe_listener)
        return -1;

    if (mcp_framed_listener_create(&server->pipe_listener,
                                   server->loop,
                                   (struct mcp_framed_listener_config){
                                       .max_frame_bytes = server->config.max_line_bytes,
                                   }) != 0)
        return -1;

    if (mcp_framed_listener_start_pipe(server->pipe_listener,
                                       path,
                                       framed_on_message,
                                       framed_on_close,
                                       server) != 0)
        return -1;

    return 0;
}

int mcp_server_start_tcp(struct mcp_server *server, const char *host, unsigned int port)
{
    if (!server || !host || server->tcp_listener)
        return -1;

    if (mcp_framed_listener_create(&server->tcp_listener,
                                   server->loop,
                                   (struct mcp_framed_listener_config){
                                       .max_frame_bytes = server->config.max_line_bytes,
                                   }) != 0)
        return -1;

    if (mcp_framed_listener_start_tcp(server->tcp_listener,
                                      host,
                                      port,
                                      framed_on_message,
                                      framed_on_close,
                                      server) != 0)
        return -1;

    free(server->tcp_host);
    server->tcp_host = mcp_strdup(host);
    if (!server->tcp_host)
        return -1;
    server->tcp_port = port;
    return 0;
}

bool mcp_server_pipe_enabled(const struct mcp_server *server)
{
    return server && mcp_framed_listener_is_open(server->pipe_listener);
}

bool mcp_server_tcp_enabled(const struct mcp_server *server)
{
    return server && mcp_framed_listener_is_open(server->tcp_listener);
}

int mcp_server_start_discovery(struct mcp_server *server,
                               const struct mcp_server_discovery_config *config)
{
    struct mcp_server_discovery_config effective;

    if (!server || !server->discovery || !config || !mcp_server_tcp_enabled(server))
        return -1;

    effective = *config;
    if (!effective.tcp_host)
        effective.tcp_host = server->tcp_host;
    if (effective.tcp_port == 0)
        effective.tcp_port = server->tcp_port;
    if (effective.discovery_port == 0)
        effective.discovery_port = effective.tcp_port;
    if (effective.broadcast_port == 0)
        effective.broadcast_port = effective.tcp_port;
    if (!effective.bind_host)
        effective.bind_host = "0.0.0.0";

    return mcp_server_discovery_start(server->discovery, &effective);
}

bool mcp_server_discovery_enabled(const struct mcp_server *server)
{
    return server && mcp_server_discovery_is_open(server->discovery);
}
