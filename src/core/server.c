#include "mcp/core/server.h"

#include "core/in_flight.h"
#include "core/server_internal.h"
#include "protocol/jsonrpc.h"
#include "tools/builtin_tools.h"

#include <jansson.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

struct mcp_message_node {
    struct mcp_jsonrpc_message *message;
    struct mcp_message_node *next;
};

static void core_async_cb(uv_async_t *handle);
static void stdio_on_line(void *arg, const char *line, size_t len);
static void stdio_on_exit(void *arg);

uv_loop_t *mcp_server_loop(struct mcp_server *server)
{
    return server->loop;
}

static int send_json_line(struct mcp_server *server, json_t *object)
{
    char *line = mcp_jsonrpc_dump_line(object);
    int rc;

    if (!line)
        return -1;

    rc = mcp_stdio_transport_send_str(server->stdio, line);
    free(line);
    return rc;
}

int mcp_server_send_result(struct mcp_server *server, json_t *id, json_t *result)
{
    json_t *object = mcp_jsonrpc_build_response(id, result);
    int rc = send_json_line(server, object);

    json_decref(object);
    return rc;
}

int mcp_server_send_error(struct mcp_server *server, json_t *id, int code, const char *message)
{
    json_t *object = mcp_jsonrpc_build_error(id, code, message);
    int rc = send_json_line(server, object);

    json_decref(object);
    return rc;
}

static bool gate_allows_method(struct mcp_server *server, const char *method)
{
    if (strcmp(method, "ping") == 0)
        return true;
    if (strcmp(method, "initialize") == 0)
        return server->session_state == MCP_SESSION_NOT_INITIALIZED;
    return server->session_state == MCP_SESSION_INITIALIZED;
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

static void queue_message(struct mcp_server *server, struct mcp_jsonrpc_message *message)
{
    struct mcp_message_node *node = calloc(1, sizeof(*node));

    if (!node) {
        mcp_jsonrpc_message_destroy(message);
        return;
    }

    node->message = message;
    if (!server->queue_tail) {
        server->queue_head = node;
        server->queue_tail = node;
    } else {
        server->queue_tail->next = node;
        server->queue_tail = node;
    }

    uv_async_send(&server->core_async);
}

static struct mcp_jsonrpc_message *dequeue_message(struct mcp_server *server)
{
    struct mcp_message_node *node = server->queue_head;
    struct mcp_jsonrpc_message *message;

    if (!node)
        return NULL;

    server->queue_head = node->next;
    if (!server->queue_head)
        server->queue_tail = NULL;

    message = node->message;
    free(node);
    return message;
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

    mcp_stdio_transport_close_output(server->stdio);
    if (!uv_is_closing((uv_handle_t *)&server->core_async))
        uv_close((uv_handle_t *)&server->core_async, NULL);
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

static void handle_cancelled(struct mcp_server *server, json_t *params)
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
        mcp_server_send_error(server, entry->id, -32603, "Cancelled");
        entry = mcp_in_flight_remove(&server->in_flight, key);
        if (entry)
            free_in_flight_entry(entry);
    }

    free(key);
}

static void handle_request(struct mcp_server *server, struct mcp_jsonrpc_message *message)
{
    json_t *result;
    json_t *error;
    char *id_key = NULL;
    struct mcp_in_flight_entry *entry = NULL;
    const char *tool_name;
    json_t *arguments = NULL;
    int rc;

    if (!gate_allows_method(server, message->method)) {
        mcp_server_send_error(server, message->id, -32600, "Session not initialized");
        return;
    }

    if (strcmp(message->method, "ping") == 0) {
        result = json_object();
        mcp_server_send_result(server, message->id, result);
        json_decref(result);
        return;
    }

    if (strcmp(message->method, "initialize") == 0) {
        result = build_initialize_result();
        mcp_server_send_result(server, message->id, result);
        json_decref(result);

        if (server->config.strict_initialized_notification)
            server->session_state = MCP_SESSION_AWAIT_CLIENT_INITIALIZED;
        else
            server->session_state = MCP_SESSION_INITIALIZED;
        return;
    }

    if (strcmp(message->method, "tools/list") == 0) {
        if (!server->session_tool_snapshot)
            server->session_tool_snapshot = mcp_tool_registry_public_list(server->registry);
        mcp_server_send_result(server, message->id, server->session_tool_snapshot);
        return;
    }

    if (strcmp(message->method, "tools/call") == 0) {
        if (extract_tool_call(message->params, &tool_name, &arguments) != 0) {
            mcp_server_send_error(server, message->id, -32602, "Invalid params");
            return;
        }

        if (!server->session_tool_snapshot)
            server->session_tool_snapshot = mcp_tool_registry_public_list(server->registry);

        if (!tool_visible_in_snapshot(server->session_tool_snapshot, tool_name)) {
            json_decref(arguments);
            result = mcp_tool_result_text(
                "Tool is not visible in current session snapshot. Refresh tools/list or start a new session.",
                true);
            mcp_server_send_result(server, message->id, result);
            json_decref(result);
            return;
        }

        if (!mcp_jsonrpc_id_to_key(message->id, &id_key)) {
            json_decref(arguments);
            mcp_server_send_error(server, message->id, -32603, "Internal error");
            return;
        }

        entry = mcp_in_flight_put(&server->in_flight, id_key, message->id);
        if (!entry) {
            free(id_key);
            json_decref(arguments);
            mcp_server_send_error(server, message->id, -32603, "Internal error");
            return;
        }

        result = NULL;
        error = NULL;
        rc = mcp_gateway_call(server->gateway,
                              server,
                              entry->invocation_id,
                              tool_name,
                              arguments,
                              server->session_tool_snapshot,
                              &result,
                              &error);
        json_decref(arguments);

        if (rc == MCP_GATEWAY_OK || rc == MCP_GATEWAY_TOOL_ERROR) {
            mcp_server_send_result(server, message->id, result);
            json_decref(result);
        } else {
            send_json_line(server, error);
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
        mcp_server_send_result(server, message->id, result);
        json_decref(result);
        return;
    }

    if (strcmp(message->method, "resources/templates/list") == 0) {
        result = json_pack("{s:[]}", "resourceTemplates");
        mcp_server_send_result(server, message->id, result);
        json_decref(result);
        return;
    }

    if (strcmp(message->method, "prompts/list") == 0) {
        result = json_pack("{s:[]}", "prompts");
        mcp_server_send_result(server, message->id, result);
        json_decref(result);
        return;
    }

    mcp_server_send_error(server, message->id, -32601, "Method not found");
}

static void handle_notification(struct mcp_server *server, struct mcp_jsonrpc_message *message)
{
    if (strcmp(message->method, "notifications/initialized") == 0) {
        if (server->session_state == MCP_SESSION_AWAIT_CLIENT_INITIALIZED)
            server->session_state = MCP_SESSION_INITIALIZED;
        return;
    }

    if (strcmp(message->method, "notifications/cancelled") == 0) {
        handle_cancelled(server, message->params);
        return;
    }
}

static void core_async_cb(uv_async_t *handle)
{
    struct mcp_server *server = handle->data;
    struct mcp_jsonrpc_message *message;

    while ((message = dequeue_message(server)) != NULL) {
        if (message->type == MCP_JSONRPC_REQUEST)
            handle_request(server, message);
        else
            handle_notification(server, message);

        mcp_jsonrpc_message_destroy(message);
    }

    maybe_shutdown(server);
}

static void stdio_on_line(void *arg, const char *line, size_t len)
{
    struct mcp_server *server = arg;
    struct mcp_jsonrpc_message *message = NULL;
    json_t *error = NULL;

    if (server->shutting_down)
        return;

    if (mcp_jsonrpc_parse_line(line, len, &message, &error) != 0) {
        send_json_line(server, error);
        json_decref(error);
        return;
    }

    queue_message(server, message);
}

static void stdio_on_exit(void *arg)
{
    struct mcp_server *server = arg;

    server->shutting_down = true;
    uv_async_send(&server->core_async);
}

void mcp_server_complete_async_ok(struct mcp_server *server, const char *id_key, json_t *result)
{
    struct mcp_in_flight_entry *entry = mcp_in_flight_get(&server->in_flight, id_key);

    if (!entry)
        return;

    mcp_server_send_result(server, entry->id, result);
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
    server->session_state = MCP_SESSION_NOT_INITIALIZED;
    mcp_in_flight_init(&server->in_flight);

    if (mcp_stdio_transport_create(&server->stdio,
                                   loop,
                                   (struct mcp_stdio_transport_config){
                                       .max_line_bytes = config.max_line_bytes,
                                   }) != 0) {
        free(server);
        return -1;
    }

    if (uv_async_init(loop, &server->core_async, core_async_cb) != 0) {
        mcp_stdio_transport_destroy(server->stdio);
        free(server);
        return -1;
    }
    server->core_async.data = server;

    if (mcp_tool_registry_create(&server->registry) != 0 ||
        mcp_gateway_create(&server->gateway, server->registry) != 0 ||
        mcp_register_builtin_tools(server, server->registry) != 0) {
        if (server->gateway)
            mcp_gateway_destroy(server->gateway);
        if (server->registry)
            mcp_tool_registry_destroy(server->registry);
        uv_close((uv_handle_t *)&server->core_async, NULL);
        mcp_stdio_transport_destroy(server->stdio);
        free(server);
        return -1;
    }

    *out = server;
    return 0;
}

void mcp_server_destroy(struct mcp_server *server)
{
    struct mcp_jsonrpc_message *message;

    if (!server)
        return;

    while ((message = dequeue_message(server)) != NULL)
        mcp_jsonrpc_message_destroy(message);

    json_decref(server->session_tool_snapshot);
    mcp_gateway_destroy(server->gateway);
    mcp_tool_registry_destroy(server->registry);
    mcp_in_flight_destroy(&server->in_flight);
    mcp_stdio_transport_destroy(server->stdio);
    free(server);
}

int mcp_server_start_stdio(struct mcp_server *server, int stdin_fd, int stdout_fd)
{
    if (mcp_stdio_transport_open(server->stdio, stdin_fd, stdout_fd) != 0)
        return -1;

    if (mcp_stdio_transport_start(server->stdio, stdio_on_line, stdio_on_exit, server) != 0)
        return -1;

    return 0;
}
