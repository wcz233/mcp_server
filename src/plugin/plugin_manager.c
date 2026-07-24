#include "plugin/plugin_manager.h"

#include "common/platform.h"
#include "core/server_internal.h"
#include "mcp/registry/tool_registry.h"
#include "tools/tool_result.h"
#include "transport/peer_transport.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MCP_PLUGIN_INIT_SYMBOL "mcp_plugin_init"
#define MCP_PLUGIN_INVOKE_SYMBOL "mcp_plugin_invoke"
#define MCP_PLUGIN_SHUTDOWN_SYMBOL "mcp_plugin_shutdown"

enum plugin_state {
    PLUGIN_STATE_LOADING = 1,
    PLUGIN_STATE_ACTIVE,
    PLUGIN_STATE_DRAINING,
    PLUGIN_STATE_UNLOADING,
    PLUGIN_STATE_UNLOADED,
    PLUGIN_STATE_FAILED,
};

struct plugin_tool {
    char *name;
    struct plugin_tool *next;
};

struct plugin_frame_adapter {
    struct plugin_module *plugin;
    char magic[4];
    mcp_plugin_frame_handler_fn on_frame;
    mcp_plugin_peer_event_fn on_peer_connected;
    mcp_plugin_peer_event_fn on_peer_closed;
    void *user_data;
    struct plugin_frame_adapter *next;
};

struct plugin_pending_call {
    char *invocation_id;
    char *id_key;
    struct plugin_pending_call *next;
};

struct plugin_async_complete {
    struct plugin_module *plugin;
    char *invocation_id;
    char *payload_json;
    char *message;
    bool ok;
    struct plugin_async_complete *next;
};

struct plugin_module {
    struct mcp_plugin_manager *manager;
    char *plugin_id;
    char *path;
    enum plugin_state state;
    uv_lib_t library;
    bool library_open;
    bool is_builtin;
    mcp_plugin_init_fn init;
    mcp_plugin_invoke_fn invoke;
    mcp_plugin_shutdown_fn shutdown;
    struct mcp_plugin_host_api host_api;
    unsigned int in_flight;
    struct plugin_tool *tools;
    struct plugin_frame_adapter *frame_adapters;
    struct plugin_pending_call *pending_calls;
    struct plugin_module *next;
};

struct mcp_plugin_manager {
    struct mcp_server *server;
    uv_thread_t loop_thread;
    bool loop_thread_valid;
    bool closing;
    uv_async_t complete_async;
    bool complete_async_initialized;
    uv_mutex_t complete_mutex;
    bool complete_mutex_initialized;
    struct plugin_async_complete *complete_head;
    struct plugin_async_complete *complete_tail;
    unsigned int next_plugin_id;
    struct plugin_module *plugins;
};

static const char *plugin_state_name(enum plugin_state state)
{
    switch (state) {
    case PLUGIN_STATE_LOADING:
        return "LOADING";
    case PLUGIN_STATE_ACTIVE:
        return "ACTIVE";
    case PLUGIN_STATE_DRAINING:
        return "DRAINING";
    case PLUGIN_STATE_UNLOADING:
        return "UNLOADING";
    case PLUGIN_STATE_UNLOADED:
        return "UNLOADED";
    case PLUGIN_STATE_FAILED:
        return "FAILED";
    }
    return "FAILED";
}

static char *make_plugin_id(struct mcp_plugin_manager *manager)
{
    int len = snprintf(NULL, 0, "plugin_%u", manager->next_plugin_id++);
    char *id;

    if (len < 0)
        return NULL;

    id = malloc((size_t)len + 1);
    if (!id)
        return NULL;

    snprintf(id, (size_t)len + 1, "plugin_%u", manager->next_plugin_id - 1);
    return id;
}

static struct plugin_module *find_plugin(struct mcp_plugin_manager *manager,
                                         const char *plugin_id)
{
    struct plugin_module *plugin;

    for (plugin = manager ? manager->plugins : NULL; plugin; plugin = plugin->next) {
        if (strcmp(plugin->plugin_id, plugin_id) == 0)
            return plugin;
    }

    return NULL;
}

static struct plugin_module *find_active_plugin_for_descriptor(
    struct mcp_plugin_manager *manager,
    const struct mcp_tool_descriptor *descriptor)
{
    if (!descriptor || descriptor->route != MCP_TOOL_ROUTE_LOCAL_MODULE)
        return NULL;
    return find_plugin(manager, (const char *)descriptor->handler_data);
}

static void tool_list_free(struct plugin_tool *tool)
{
    while (tool) {
        struct plugin_tool *next = tool->next;
        free(tool->name);
        free(tool);
        tool = next;
    }
}

static void frame_adapter_list_free(struct plugin_frame_adapter *adapter)
{
    while (adapter) {
        struct plugin_frame_adapter *next = adapter->next;
        free(adapter);
        adapter = next;
    }
}

static void pending_call_free(struct plugin_pending_call *call)
{
    if (!call)
        return;

    free(call->invocation_id);
    free(call->id_key);
    free(call);
}

static void pending_call_list_free(struct plugin_pending_call *call)
{
    while (call) {
        struct plugin_pending_call *next = call->next;
        pending_call_free(call);
        call = next;
    }
}

static void plugin_async_complete_free(struct plugin_async_complete *complete)
{
    if (!complete)
        return;
    free(complete->invocation_id);
    free(complete->payload_json);
    free(complete->message);
    free(complete);
}

static void plugin_async_complete_list_free(struct plugin_async_complete *complete)
{
    while (complete) {
        struct plugin_async_complete *next = complete->next;

        plugin_async_complete_free(complete);
        complete = next;
    }
}

static bool plugin_manager_on_loop_thread(struct mcp_plugin_manager *manager)
{
    uv_thread_t current;

    if (!manager || !manager->loop_thread_valid)
        return true;
    current = uv_thread_self();
    return uv_thread_equal(&current, &manager->loop_thread) != 0;
}

static int plugin_add_pending_call(struct plugin_module *plugin,
                                   const char *invocation_id,
                                   const char *id_key)
{
    struct plugin_pending_call *call;

    if (!plugin || !invocation_id || !id_key)
        return -1;

    call = calloc(1, sizeof(*call));
    if (!call)
        return -1;

    call->invocation_id = mcp_strdup(invocation_id);
    call->id_key = mcp_strdup(id_key);
    if (!call->invocation_id || !call->id_key) {
        pending_call_free(call);
        return -1;
    }

    call->next = plugin->pending_calls;
    plugin->pending_calls = call;
    return 0;
}

static struct plugin_pending_call *plugin_take_pending_call(struct plugin_module *plugin,
                                                           const char *invocation_id)
{
    struct plugin_pending_call **current;

    if (!plugin || !invocation_id)
        return NULL;

    current = &plugin->pending_calls;
    while (*current) {
        struct plugin_pending_call *call = *current;

        if (strcmp(call->invocation_id, invocation_id) == 0) {
            *current = call->next;
            call->next = NULL;
            return call;
        }
        current = &call->next;
    }

    return NULL;
}

static bool plugin_has_tool(struct plugin_module *plugin, const char *tool_name)
{
    struct plugin_tool *tool;

    for (tool = plugin ? plugin->tools : NULL; tool; tool = tool->next) {
        if (strcmp(tool->name, tool_name) == 0)
            return true;
    }

    return false;
}

static int plugin_record_tool(struct plugin_module *plugin, const char *tool_name)
{
    struct plugin_tool *tool;

    if (plugin_has_tool(plugin, tool_name))
        return 0;

    tool = calloc(1, sizeof(*tool));
    if (!tool)
        return -1;

    tool->name = mcp_strdup(tool_name);
    if (!tool->name) {
        free(tool);
        return -1;
    }

    tool->next = plugin->tools;
    plugin->tools = tool;
    return 0;
}

static void plugin_remove_tool_record(struct plugin_module *plugin, const char *tool_name)
{
    struct plugin_tool **current = &plugin->tools;

    while (*current) {
        struct plugin_tool *tool = *current;

        if (strcmp(tool->name, tool_name) == 0) {
            *current = tool->next;
            free(tool->name);
            free(tool);
            return;
        }
        current = &tool->next;
    }
}

static void host_log_info(void *host_context, const char *message)
{
    struct plugin_module *plugin = host_context;

    fprintf(stderr,
            "[mcp plugin %s] %s\n",
            plugin && plugin->plugin_id ? plugin->plugin_id : "unknown",
            message ? message : "");
}

static void host_log_error(void *host_context, const char *message)
{
    struct plugin_module *plugin = host_context;

    fprintf(stderr,
            "[mcp plugin %s error] %s\n",
            plugin && plugin->plugin_id ? plugin->plugin_id : "unknown",
            message ? message : "");
}

static int host_check_permission(void *host_context, const char *permission)
{
    (void)host_context;
    (void)permission;
    return 0;
}

static unsigned long long host_now_ms(void *host_context)
{
    (void)host_context;
    return mcp_now_ms();
}

static void *host_get_loop(void *host_context)
{
    struct plugin_module *plugin = host_context;

    if (!plugin)
        return NULL;
    return plugin->manager ? plugin->manager->server->loop : NULL;
}

static json_t *parse_schema_or_object(const char *schema_json)
{
    json_error_t error;
    json_t *schema;

    if (!schema_json || schema_json[0] == '\0')
        return json_object();

    schema = json_loads(schema_json, JSON_REJECT_DUPLICATES, &error);
    if (!json_is_object(schema)) {
        json_decref(schema);
        return NULL;
    }

    return schema;
}

static int host_register_tool(void *host_context,
                              const struct mcp_plugin_tool_descriptor *plugin_descriptor)
{
    struct plugin_module *plugin = host_context;
    struct mcp_plugin_manager *manager;
    struct mcp_tool_descriptor descriptor;
    json_t *schema;
    int rc;

    if (!plugin || !plugin_descriptor || !plugin_descriptor->name ||
        !plugin_descriptor->description)
        return -1;
    if (plugin->state != PLUGIN_STATE_LOADING && plugin->state != PLUGIN_STATE_ACTIVE)
        return -1;

    manager = plugin->manager;
    schema = parse_schema_or_object(plugin_descriptor->input_schema_json);
    if (!schema)
        return -1;

    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.name = plugin_descriptor->name;
    descriptor.description = plugin_descriptor->description;
    descriptor.input_schema = schema;
    descriptor.source = plugin_descriptor->source ? plugin_descriptor->source : plugin->plugin_id;
    descriptor.version = plugin_descriptor->version ? plugin_descriptor->version
                                                    : MCP_SERVER_VERSION;
    descriptor.risk_level = plugin_descriptor->risk_level ? plugin_descriptor->risk_level : "L2";
    descriptor.permission = plugin_descriptor->permission ? plugin_descriptor->permission
                                                          : "plugin.call";
    descriptor.idempotent = plugin_descriptor->idempotent != 0;
    descriptor.retryable = plugin_descriptor->retryable != 0;
    descriptor.cancelable = plugin_descriptor->cancelable != 0;
    descriptor.enabled = true;
    descriptor.timeout_ms = plugin_descriptor->timeout_ms ? plugin_descriptor->timeout_ms : 5000;
    descriptor.route = MCP_TOOL_ROUTE_LOCAL_MODULE;
    descriptor.handler_data = plugin->plugin_id;

    rc = mcp_tool_registry_register(manager->server->registry, &descriptor);
    json_decref(schema);
    if (rc != 0)
        return -1;

    if (plugin_record_tool(plugin, plugin_descriptor->name) != 0) {
        mcp_tool_registry_unregister(manager->server->registry, plugin_descriptor->name);
        return -1;
    }

    return 0;
}

static int host_unregister_tool(void *host_context, const char *tool_name)
{
    struct plugin_module *plugin = host_context;
    struct mcp_plugin_manager *manager;

    if (!plugin || !tool_name || !plugin_has_tool(plugin, tool_name))
        return -1;

    manager = plugin->manager;
    if (mcp_tool_registry_unregister(manager->server->registry, tool_name) != 0)
        return -1;
    plugin_remove_tool_record(plugin, tool_name);
    return 0;
}

static json_t *parse_payload_json(const char *payload_json)
{
    json_error_t error;
    json_t *payload;

    if (!payload_json || payload_json[0] == '\0')
        return json_object();

    payload = json_loads(payload_json, JSON_REJECT_DUPLICATES, &error);
    if (!payload)
        payload = json_pack("{s:s}", "text", payload_json);
    return payload;
}

static int complete_async_ok_on_loop(struct plugin_module *plugin,
                                     const char *invocation_id,
                                     const char *payload_json)
{
    struct mcp_plugin_manager *manager;
    struct plugin_pending_call *call;
    json_t *payload;
    json_t *result;

    if (!plugin || !invocation_id)
        return -1;

    call = plugin_take_pending_call(plugin, invocation_id);
    if (!call)
        return -1;

    manager = plugin->manager;
    payload = parse_payload_json(payload_json);
    if (!payload) {
        pending_call_free(call);
        return -1;
    }

    result = mcp_tool_result_json_text(payload, false);
    json_decref(payload);
    mcp_server_complete_async_ok(manager->server, call->id_key, result);
    json_decref(result);
    pending_call_free(call);
    if (plugin->in_flight > 0)
        plugin->in_flight--;
    return 0;
}

static int complete_async_error_on_loop(struct plugin_module *plugin,
                                        const char *invocation_id,
                                        const char *message)
{
    struct mcp_plugin_manager *manager;
    struct plugin_pending_call *call;
    json_t *result;

    if (!plugin || !invocation_id)
        return -1;

    call = plugin_take_pending_call(plugin, invocation_id);
    if (!call)
        return -1;

    manager = plugin->manager;
    result = mcp_tool_result_text(message ? message : "Plugin async call failed.", true);
    mcp_server_complete_async_ok(manager->server, call->id_key, result);
    json_decref(result);
    pending_call_free(call);
    if (plugin->in_flight > 0)
        plugin->in_flight--;
    return 0;
}

static void complete_async_cb(uv_async_t *handle)
{
    struct mcp_plugin_manager *manager = handle->data;
    struct plugin_async_complete *complete;

    if (!manager || !manager->complete_mutex_initialized)
        return;

    for (;;) {
        uv_mutex_lock(&manager->complete_mutex);
        complete = manager->complete_head;
        if (complete) {
            manager->complete_head = complete->next;
            if (!manager->complete_head)
                manager->complete_tail = NULL;
            complete->next = NULL;
        }
        uv_mutex_unlock(&manager->complete_mutex);
        if (!complete)
            break;

        if (complete->ok)
            complete_async_ok_on_loop(complete->plugin,
                                      complete->invocation_id,
                                      complete->payload_json);
        else
            complete_async_error_on_loop(complete->plugin,
                                         complete->invocation_id,
                                         complete->message);
        plugin_async_complete_free(complete);
    }
}

static void complete_async_close_cb(uv_handle_t *handle)
{
    struct mcp_plugin_manager *manager = handle->data;

    if (manager)
        manager->complete_async_initialized = false;
}

static int enqueue_async_complete(struct plugin_module *plugin,
                                  const char *invocation_id,
                                  const char *payload_json,
                                  const char *message,
                                  bool ok)
{
    struct mcp_plugin_manager *manager;
    struct plugin_async_complete *complete;

    if (!plugin || !invocation_id || !plugin->manager)
        return -1;
    manager = plugin->manager;
    if (!manager->complete_mutex_initialized || !manager->complete_async_initialized)
        return -1;

    complete = calloc(1, sizeof(*complete));
    if (!complete)
        return -1;
    complete->plugin = plugin;
    complete->ok = ok;
    complete->invocation_id = mcp_strdup(invocation_id);
    if (ok)
        complete->payload_json = mcp_strdup(payload_json ? payload_json : "");
    else
        complete->message = mcp_strdup(message ? message : "");
    if (!complete->invocation_id || (ok && !complete->payload_json) ||
        (!ok && !complete->message)) {
        plugin_async_complete_free(complete);
        return -1;
    }

    uv_mutex_lock(&manager->complete_mutex);
    if (manager->closing ||
        !manager->complete_async_initialized ||
        uv_is_closing((uv_handle_t *)&manager->complete_async)) {
        uv_mutex_unlock(&manager->complete_mutex);
        plugin_async_complete_free(complete);
        return -1;
    }
    if (manager->complete_tail)
        manager->complete_tail->next = complete;
    else
        manager->complete_head = complete;
    manager->complete_tail = complete;
    uv_async_send(&manager->complete_async);
    uv_mutex_unlock(&manager->complete_mutex);
    return 0;
}

static int host_complete_async_ok(void *host_context,
                                  const char *invocation_id,
                                  const char *payload_json)
{
    struct plugin_module *plugin = host_context;

    if (!plugin || !invocation_id)
        return -1;
    if (!plugin_manager_on_loop_thread(plugin->manager))
        return enqueue_async_complete(plugin, invocation_id, payload_json, NULL, true);
    return complete_async_ok_on_loop(plugin, invocation_id, payload_json);
}

static int host_complete_async_error(void *host_context,
                                     const char *invocation_id,
                                     const char *message)
{
    struct plugin_module *plugin = host_context;

    if (!plugin || !invocation_id)
        return -1;
    if (!plugin_manager_on_loop_thread(plugin->manager))
        return enqueue_async_complete(plugin, invocation_id, NULL, message, false);
    return complete_async_error_on_loop(plugin, invocation_id, message);
}

static int host_peer_send_frame(void *host_context,
                                uint32_t server_id,
                                const void *payload,
                                uint32_t payload_len)
{
    struct plugin_module *plugin = host_context;
    struct mcp_plugin_manager *manager;

    if (!plugin)
        return -1;

    manager = plugin->manager;
    return mcp_peer_transport_send_frame(manager->server->peer_transport,
                                         server_id,
                                         payload,
                                         payload_len);
}

static void host_frame_on_frame(void *arg,
                                uint32_t server_id,
                                const unsigned char *payload,
                                size_t len)
{
    struct plugin_frame_adapter *adapter = arg;

    if (!adapter || !adapter->on_frame || len > UINT32_MAX)
        return;

    adapter->on_frame(adapter->user_data, server_id, payload, (uint32_t)len);
}

static void host_frame_on_peer_connected(void *arg, uint32_t server_id)
{
    struct plugin_frame_adapter *adapter = arg;

    if (adapter && adapter->on_peer_connected)
        adapter->on_peer_connected(adapter->user_data, server_id);
}

static void host_frame_on_peer_closed(void *arg, uint32_t server_id)
{
    struct plugin_frame_adapter *adapter = arg;

    if (adapter && adapter->on_peer_closed)
        adapter->on_peer_closed(adapter->user_data, server_id);
}

static int host_peer_register_handler(void *host_context,
                                      const char magic[4],
                                      mcp_plugin_frame_handler_fn on_frame,
                                      mcp_plugin_peer_event_fn on_peer_connected,
                                      mcp_plugin_peer_event_fn on_peer_closed,
                                      void *user_data)
{
    struct plugin_module *plugin = host_context;
    struct mcp_plugin_manager *manager;
    struct plugin_frame_adapter *adapter;

    if (!plugin || !magic || !on_frame)
        return -1;

    manager = plugin->manager;
    adapter = calloc(1, sizeof(*adapter));
    if (!adapter)
        return -1;

    adapter->plugin = plugin;
    memcpy(adapter->magic, magic, 4);
    adapter->on_frame = on_frame;
    adapter->on_peer_connected = on_peer_connected;
    adapter->on_peer_closed = on_peer_closed;
    adapter->user_data = user_data;
    adapter->next = plugin->frame_adapters;
    plugin->frame_adapters = adapter;

    if (mcp_peer_transport_register_handler(manager->server->peer_transport,
                                            magic,
                                            plugin->plugin_id,
                                            host_frame_on_frame,
                                            host_frame_on_peer_connected,
                                            host_frame_on_peer_closed,
                                            adapter) != 0) {
        plugin->frame_adapters = adapter->next;
        free(adapter);
        return -1;
    }

    return 0;
}

static int host_peer_unregister_handler(void *host_context, const char magic[4])
{
    struct plugin_module *plugin = host_context;
    struct mcp_plugin_manager *manager;
    struct plugin_frame_adapter **current;

    if (!plugin || !magic)
        return -1;

    manager = plugin->manager;
    if (mcp_peer_transport_unregister_handler(manager->server->peer_transport,
                                              magic,
                                              plugin->plugin_id) != 0)
        return -1;

    current = &plugin->frame_adapters;
    while (*current) {
        struct plugin_frame_adapter *adapter = *current;

        if (memcmp(adapter->magic, magic, 4) == 0) {
            *current = adapter->next;
            free(adapter);
            return 0;
        }
        current = &adapter->next;
    }

    return 0;
}

static int host_peer_has_capability(void *host_context,
                                    uint32_t server_id,
                                    const char *capability)
{
    struct plugin_module *plugin = host_context;
    struct mcp_plugin_manager *manager;

    if (!plugin)
        return 0;

    manager = plugin->manager;
    return mcp_peer_transport_has_capability(manager->server->peer_transport,
                                             server_id,
                                             capability) ? 1 : 0;
}

static int host_peer_set_capability(void *host_context,
                                    uint32_t server_id,
                                    const char *capability,
                                    int enabled)
{
    struct plugin_module *plugin = host_context;
    struct mcp_plugin_manager *manager;

    if (!plugin)
        return -1;

    manager = plugin->manager;
    return mcp_peer_transport_set_capability(manager->server->peer_transport,
                                             server_id,
                                             capability,
                                             enabled != 0);
}

static void build_host_api(struct mcp_plugin_manager *manager, struct plugin_module *plugin)
{
    memset(&plugin->host_api, 0, sizeof(plugin->host_api));
    plugin->host_api.abi_version = MCP_PLUGIN_ABI_VERSION;
    plugin->host_api.host_context = plugin;
    plugin->host_api.log_info = host_log_info;
    plugin->host_api.log_error = host_log_error;
    plugin->host_api.check_permission = host_check_permission;
    plugin->host_api.now_ms = host_now_ms;
    plugin->host_api.get_loop = host_get_loop;
    plugin->host_api.register_tool = host_register_tool;
    plugin->host_api.unregister_tool = host_unregister_tool;
    plugin->host_api.complete_async_ok = host_complete_async_ok;
    plugin->host_api.complete_async_error = host_complete_async_error;
    plugin->host_api.peer_transport_send_frame = host_peer_send_frame;
    plugin->host_api.peer_transport_register_handler = host_peer_register_handler;
    plugin->host_api.peer_transport_unregister_handler = host_peer_unregister_handler;
    plugin->host_api.peer_transport_has_capability = host_peer_has_capability;
    plugin->host_api.peer_transport_set_capability = host_peer_set_capability;
    plugin->manager = manager;
}

int mcp_plugin_manager_create(struct mcp_plugin_manager **out, struct mcp_server *server)
{
    struct mcp_plugin_manager *manager;

    *out = NULL;
    if (!server)
        return -1;

    manager = calloc(1, sizeof(*manager));
    if (!manager)
        return -1;

    manager->server = server;
    manager->loop_thread = uv_thread_self();
    manager->loop_thread_valid = true;
    manager->next_plugin_id = 1;
    if (uv_mutex_init(&manager->complete_mutex) != 0) {
        free(manager);
        return -1;
    }
    manager->complete_mutex_initialized = true;
    if (uv_async_init(server->loop, &manager->complete_async, complete_async_cb) != 0) {
        uv_mutex_destroy(&manager->complete_mutex);
        free(manager);
        return -1;
    }
    manager->complete_async_initialized = true;
    manager->complete_async.data = manager;
    uv_unref((uv_handle_t *)&manager->complete_async);
    *out = manager;
    return 0;
}

void mcp_plugin_manager_close(struct mcp_plugin_manager *manager)
{
    if (!manager)
        return;

    if (manager->complete_mutex_initialized) {
        uv_mutex_lock(&manager->complete_mutex);
        manager->closing = true;
        uv_mutex_unlock(&manager->complete_mutex);
    } else {
        manager->closing = true;
    }
    if (manager->complete_async_initialized &&
        !uv_is_closing((uv_handle_t *)&manager->complete_async))
        uv_close((uv_handle_t *)&manager->complete_async, complete_async_close_cb);
}

static int init_linked_plugin(struct mcp_plugin_manager *manager,
                              struct plugin_module *plugin,
                              const char *config_json,
                              const char **out_error)
{
    char result_json[4096] = {0};
    int init_rc;

    build_host_api(manager, plugin);
    init_rc = plugin->init(&plugin->host_api,
                           config_json ? config_json : "{}",
                           result_json,
                           sizeof(result_json));
    if (init_rc != 0) {
        *out_error = result_json[0] ? result_json : "Plugin initialization failed.";
        plugin->state = PLUGIN_STATE_FAILED;
        return -1;
    }

    plugin->state = PLUGIN_STATE_ACTIVE;
    return 0;
}

static int unload_plugin(struct mcp_plugin_manager *manager, struct plugin_module *plugin)
{
    struct plugin_tool *tool;

    if (!plugin)
        return 0;
    if (plugin->shutdown && plugin->shutdown() != 0)
        return -1;

    plugin->state = PLUGIN_STATE_UNLOADING;
    for (tool = plugin->tools; tool; tool = tool->next)
        mcp_tool_registry_set_enabled(manager->server->registry, tool->name, false);
    while (plugin->tools)
        host_unregister_tool(plugin, plugin->tools->name);
    mcp_peer_transport_unregister_owner(manager->server->peer_transport, plugin->plugin_id);
    frame_adapter_list_free(plugin->frame_adapters);
    plugin->frame_adapters = NULL;
    pending_call_list_free(plugin->pending_calls);
    plugin->pending_calls = NULL;
    if (plugin->library_open) {
        uv_dlclose(&plugin->library);
        plugin->library_open = false;
    }
    plugin->state = PLUGIN_STATE_UNLOADED;
    return 0;
}

void mcp_plugin_manager_destroy(struct mcp_plugin_manager *manager)
{
    struct plugin_module *plugin;

    if (!manager)
        return;

    mcp_plugin_manager_close(manager);
    while (manager->server &&
           manager->server->loop &&
           manager->complete_async_initialized)
        uv_run(manager->server->loop, UV_RUN_DEFAULT);
    if (manager->complete_mutex_initialized) {
        uv_mutex_lock(&manager->complete_mutex);
        plugin_async_complete_list_free(manager->complete_head);
        manager->complete_head = NULL;
        manager->complete_tail = NULL;
        uv_mutex_unlock(&manager->complete_mutex);
    }
    while ((plugin = manager->plugins) != NULL) {
        manager->plugins = plugin->next;
        (void)unload_plugin(manager, plugin);
        tool_list_free(plugin->tools);
        frame_adapter_list_free(plugin->frame_adapters);
        pending_call_list_free(plugin->pending_calls);
        free(plugin->plugin_id);
        free(plugin->path);
        free(plugin);
    }
    if (manager->complete_mutex_initialized) {
        uv_mutex_destroy(&manager->complete_mutex);
        manager->complete_mutex_initialized = false;
    }
    free(manager);
}

static void unlink_plugin(struct mcp_plugin_manager *manager, struct plugin_module *plugin)
{
    struct plugin_module **current = &manager->plugins;

    while (*current) {
        if (*current == plugin) {
            *current = plugin->next;
            plugin->next = NULL;
            return;
        }
        current = &(*current)->next;
    }
}

int mcp_plugin_manager_register_builtin(
    struct mcp_plugin_manager *manager,
    const struct mcp_builtin_plugin_descriptor *descriptor,
    const char *config_json)
{
    struct plugin_module *plugin;
    const char *error = NULL;

    if (!manager || !descriptor || !descriptor->plugin_id ||
        !descriptor->init || !descriptor->invoke || !descriptor->shutdown)
        return -1;

    if (find_plugin(manager, descriptor->plugin_id))
        return -1;

    plugin = calloc(1, sizeof(*plugin));
    if (!plugin)
        return -1;

    plugin->plugin_id = mcp_strdup(descriptor->plugin_id);
    plugin->path = mcp_strdup(descriptor->path ? descriptor->path : "builtin");
    plugin->state = PLUGIN_STATE_LOADING;
    plugin->is_builtin = true;
    plugin->init = descriptor->init;
    plugin->invoke = descriptor->invoke;
    plugin->shutdown = descriptor->shutdown;
    if (!plugin->plugin_id || !plugin->path)
        goto fail;

    plugin->next = manager->plugins;
    manager->plugins = plugin;

    if (init_linked_plugin(manager, plugin, config_json, &error) != 0)
        goto fail_linked;

    return 0;

fail_linked:
    (void)unload_plugin(manager, plugin);
    unlink_plugin(manager, plugin);
fail:
    tool_list_free(plugin->tools);
    frame_adapter_list_free(plugin->frame_adapters);
    pending_call_list_free(plugin->pending_calls);
    free(plugin->plugin_id);
    free(plugin->path);
    free(plugin);
    (void)error;
    return -1;
}

int mcp_plugin_manager_insmod(struct mcp_plugin_manager *manager,
                              const char *package_path,
                              bool enable,
                              json_t **out_payload,
                              const char **out_error)
{
    struct plugin_module *plugin;

    (void)enable;
    *out_payload = NULL;
    *out_error = NULL;
    if (!manager || !package_path || package_path[0] == '\0') {
        *out_error = "plugin_tools.insmod requires package_path.";
        return -1;
    }

    plugin = calloc(1, sizeof(*plugin));
    if (!plugin) {
        *out_error = "Failed to allocate plugin record.";
        return -1;
    }

    plugin->plugin_id = make_plugin_id(manager);
    plugin->path = mcp_strdup(package_path);
    plugin->state = PLUGIN_STATE_LOADING;
    if (!plugin->plugin_id || !plugin->path) {
        *out_error = "Failed to allocate plugin metadata.";
        goto fail;
    }

    plugin->next = manager->plugins;
    manager->plugins = plugin;

    if (uv_dlopen(package_path, &plugin->library) != 0) {
        *out_error = uv_dlerror(&plugin->library);
        plugin->state = PLUGIN_STATE_FAILED;
        goto fail_linked;
    }
    plugin->library_open = true;

    if (uv_dlsym(&plugin->library, MCP_PLUGIN_INIT_SYMBOL, (void **)&plugin->init) != 0 ||
        uv_dlsym(&plugin->library, MCP_PLUGIN_INVOKE_SYMBOL, (void **)&plugin->invoke) != 0 ||
        uv_dlsym(&plugin->library, MCP_PLUGIN_SHUTDOWN_SYMBOL, (void **)&plugin->shutdown) != 0) {
        *out_error = "Plugin is missing required entrypoints.";
        plugin->state = PLUGIN_STATE_FAILED;
        goto fail_linked;
    }

    if (init_linked_plugin(manager, plugin, "{}", out_error) != 0)
        goto fail_linked;
    *out_payload = json_pack("{s:s,s:[],s:s}",
                             "plugin_id",
                             plugin->plugin_id,
                             "tools",
                             "note",
                             "newly registered tools are available in this session after plugin_tools.insmod returns");
    if (*out_payload) {
        struct plugin_tool *tool;
        json_t *tools = json_object_get(*out_payload, "tools");

        for (tool = plugin->tools; tool; tool = tool->next)
            json_array_append_new(tools, json_string(tool->name));
    }
    return 0;

fail_linked:
    (void)unload_plugin(manager, plugin);
    unlink_plugin(manager, plugin);
fail:
    tool_list_free(plugin->tools);
    frame_adapter_list_free(plugin->frame_adapters);
    pending_call_list_free(plugin->pending_calls);
    free(plugin->plugin_id);
    free(plugin->path);
    free(plugin);
    return -1;
}

int mcp_plugin_manager_rmmod(struct mcp_plugin_manager *manager,
                             const char *plugin_id,
                             json_t **out_payload,
                             const char **out_error)
{
    struct plugin_module *plugin;

    *out_payload = NULL;
    *out_error = NULL;
    if (!manager || !plugin_id || plugin_id[0] == '\0') {
        *out_error = "plugin_tools.rmmod requires plugin_id.";
        return -1;
    }

    plugin = find_plugin(manager, plugin_id);
    if (!plugin || plugin->state != PLUGIN_STATE_ACTIVE) {
        *out_error = "Plugin is not active.";
        return -1;
    }
    if (plugin->is_builtin) {
        *out_error = "Plugin is built into mcp_server.";
        return -1;
    }
    if (plugin->in_flight != 0) {
        *out_error = "Plugin still has in-flight calls.";
        return -1;
    }

    plugin->state = PLUGIN_STATE_DRAINING;
    if (unload_plugin(manager, plugin) != 0) {
        plugin->state = PLUGIN_STATE_ACTIVE;
        *out_error = "Plugin is busy and cannot be unloaded.";
        return -1;
    }
    unlink_plugin(manager, plugin);

    *out_payload = json_pack("{s:s,s:b,s:s}",
                             "plugin_id",
                             plugin_id,
                             "unloaded",
                             true,
                             "note",
                             "unloaded tools are removed from this session after plugin_tools.rmmod returns");
    tool_list_free(plugin->tools);
    frame_adapter_list_free(plugin->frame_adapters);
    pending_call_list_free(plugin->pending_calls);
    free(plugin->plugin_id);
    free(plugin->path);
    free(plugin);
    return 0;
}

json_t *mcp_plugin_manager_lsmod(struct mcp_plugin_manager *manager)
{
    json_t *result = json_object();
    json_t *plugins = json_array();
    struct plugin_module *plugin;

    for (plugin = manager ? manager->plugins : NULL; plugin; plugin = plugin->next) {
        json_t *item = json_object();
        json_t *tools = json_array();
        struct plugin_tool *tool;

        json_object_set_new(item, "plugin_id", json_string(plugin->plugin_id));
        json_object_set_new(item, "path", json_string(plugin->path));
        json_object_set_new(item, "state", json_string(plugin_state_name(plugin->state)));
        json_object_set_new(item, "builtin", json_boolean(plugin->is_builtin));
        json_object_set_new(item, "abi_version", json_string(MCP_PLUGIN_ABI_VERSION));
        json_object_set_new(item, "in_flight", json_integer((json_int_t)plugin->in_flight));
        for (tool = plugin->tools; tool; tool = tool->next)
            json_array_append_new(tools, json_string(tool->name));
        json_object_set_new(item, "tools", tools);
        json_array_append_new(plugins, item);
    }

    json_object_set_new(result, "plugins", plugins);
    return result;
}

int mcp_plugin_manager_invoke(struct mcp_plugin_manager *manager,
                              const struct mcp_tool_descriptor *descriptor,
                              const char *id_key,
                              const char *invocation_id,
                              const char *tool_name,
                              json_t *arguments,
                              json_t **out_result)
{
    struct plugin_module *plugin;
    char *args_json;
    char result_json[64 * 1024] = {0};
    int rc;

    (void)id_key;
    if (!manager || !descriptor || !tool_name || !arguments || !out_result)
        return MCP_PLUGIN_CALL_ERROR;

    plugin = find_active_plugin_for_descriptor(manager, descriptor);
    if (!plugin || plugin->state != PLUGIN_STATE_ACTIVE || !plugin_has_tool(plugin, tool_name)) {
        *out_result = mcp_tool_result_text("Plugin is not active.", true);
        return MCP_PLUGIN_CALL_ERROR;
    }

    args_json = json_dumps(arguments, JSON_COMPACT | JSON_ENSURE_ASCII);
    if (!args_json) {
        *out_result = mcp_tool_result_text("Failed to serialize plugin arguments.", true);
        return MCP_PLUGIN_CALL_ERROR;
    }

    plugin->in_flight++;
    if (plugin_add_pending_call(plugin, invocation_id, id_key) != 0) {
        plugin->in_flight--;
        free(args_json);
        *out_result = mcp_tool_result_text("Failed to track plugin invocation.", true);
        return MCP_PLUGIN_CALL_ERROR;
    }
    rc = plugin->invoke(invocation_id, tool_name, args_json, result_json, sizeof(result_json));
    free(args_json);

    if (rc == MCP_PLUGIN_CALL_PENDING)
        return MCP_PLUGIN_CALL_PENDING;

    pending_call_free(plugin_take_pending_call(plugin, invocation_id));
    if (plugin->in_flight > 0)
        plugin->in_flight--;

    if (rc == MCP_PLUGIN_CALL_OK) {
        json_t *payload = parse_payload_json(result_json);
        if (!payload) {
            *out_result = mcp_tool_result_text("Plugin returned invalid JSON.", true);
            return MCP_PLUGIN_CALL_ERROR;
        }
        *out_result = mcp_tool_result_json_text(payload, false);
        json_decref(payload);
        return MCP_PLUGIN_CALL_OK;
    }

    *out_result = mcp_tool_result_text(result_json[0] ? result_json : "Plugin call failed.", true);
    return MCP_PLUGIN_CALL_ERROR;
}
