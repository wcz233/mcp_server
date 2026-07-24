#include "mcp/registry/tool_registry.h"

#include "common/json_counter.h"
#include "common/platform.h"

#include <jansson.h>
#include <stdlib.h>
#include <string.h>

struct mcp_registered_tool {
    struct mcp_tool_descriptor descriptor;
};

struct mcp_tool_registry {
    struct mcp_registered_tool *tools;
    size_t count;
    size_t capacity;
    uint64_t version;
};

static void descriptor_cleanup(struct mcp_tool_descriptor *descriptor)
{
    free((char *)descriptor->name);
    free((char *)descriptor->description);
    free((char *)descriptor->source);
    free((char *)descriptor->version);
    free((char *)descriptor->risk_level);
    free((char *)descriptor->permission);
    json_decref(descriptor->input_schema);
}

static int dup_string_field(const char *value, const char **out)
{
    *out = NULL;
    if (!value)
        return 0;

    *out = mcp_strdup(value);
    return *out ? 0 : -1;
}

static int descriptor_copy(struct mcp_tool_descriptor *dst,
                           const struct mcp_tool_descriptor *src)
{
    memset(dst, 0, sizeof(*dst));

    if (dup_string_field(src->name, &dst->name) != 0 ||
        dup_string_field(src->description, &dst->description) != 0 ||
        dup_string_field(src->source, &dst->source) != 0 ||
        dup_string_field(src->version, &dst->version) != 0 ||
        dup_string_field(src->risk_level, &dst->risk_level) != 0 ||
        dup_string_field(src->permission, &dst->permission) != 0) {
        descriptor_cleanup(dst);
        return -1;
    }

    if (src->input_schema)
        dst->input_schema = json_incref(src->input_schema);
    else
        dst->input_schema = json_object();
    dst->idempotent = src->idempotent;
    dst->retryable = src->retryable;
    dst->cancelable = src->cancelable;
    dst->enabled = src->enabled;
    dst->timeout_ms = src->timeout_ms;
    dst->route = src->route;
    dst->handler = src->handler;
    dst->handler_data = src->handler_data;

    return 0;
}

int mcp_tool_registry_create(struct mcp_tool_registry **out)
{
    struct mcp_tool_registry *registry;

    *out = NULL;
    registry = calloc(1, sizeof(*registry));
    if (!registry)
        return -1;

    registry->version = 1;
    *out = registry;
    return 0;
}

void mcp_tool_registry_destroy(struct mcp_tool_registry *registry)
{
    size_t i;

    if (!registry)
        return;

    for (i = 0; i < registry->count; i++)
        descriptor_cleanup(&registry->tools[i].descriptor);

    free(registry->tools);
    free(registry);
}

static int ensure_capacity(struct mcp_tool_registry *registry)
{
    struct mcp_registered_tool *next;
    size_t next_capacity;

    if (registry->count < registry->capacity)
        return 0;

    next_capacity = registry->capacity == 0 ? 16 : registry->capacity * 2;
    next = realloc(registry->tools, next_capacity * sizeof(*next));
    if (!next)
        return -1;

    registry->tools = next;
    registry->capacity = next_capacity;
    return 0;
}

int mcp_tool_registry_register(struct mcp_tool_registry *registry,
                               const struct mcp_tool_descriptor *descriptor)
{
    if (!registry || !descriptor || !descriptor->name || !descriptor->description)
        return -1;
    if (mcp_tool_registry_find(registry, descriptor->name))
        return -1;
    if (ensure_capacity(registry) != 0)
        return -1;
    if (descriptor_copy(&registry->tools[registry->count].descriptor, descriptor) != 0)
        return -1;

    registry->count++;
    mcp_json_counter_increment(&registry->version);
    return 0;
}

int mcp_tool_registry_unregister(struct mcp_tool_registry *registry, const char *name)
{
    size_t i;

    if (!registry || !name)
        return -1;

    for (i = 0; i < registry->count; i++) {
        struct mcp_tool_descriptor *descriptor = &registry->tools[i].descriptor;

        if (strcmp(descriptor->name, name) != 0)
            continue;

        descriptor_cleanup(descriptor);
        if (i + 1 < registry->count) {
            memmove(&registry->tools[i],
                    &registry->tools[i + 1],
                    (registry->count - i - 1) * sizeof(registry->tools[i]));
        }
        registry->count--;
        mcp_json_counter_increment(&registry->version);
        return 0;
    }

    return -1;
}

int mcp_tool_registry_set_enabled(struct mcp_tool_registry *registry,
                                  const char *name,
                                  bool enabled)
{
    size_t i;

    if (!registry || !name)
        return -1;

    for (i = 0; i < registry->count; i++) {
        struct mcp_tool_descriptor *descriptor = &registry->tools[i].descriptor;

        if (strcmp(descriptor->name, name) != 0)
            continue;

        if (descriptor->enabled != enabled) {
            descriptor->enabled = enabled;
            mcp_json_counter_increment(&registry->version);
        }
        return 0;
    }

    return -1;
}

const struct mcp_tool_descriptor *mcp_tool_registry_find(struct mcp_tool_registry *registry,
                                                         const char *name)
{
    size_t i;

    if (!registry || !name)
        return NULL;

    for (i = 0; i < registry->count; i++) {
        struct mcp_tool_descriptor *descriptor = &registry->tools[i].descriptor;
        if (descriptor->enabled && strcmp(descriptor->name, name) == 0)
            return descriptor;
    }

    return NULL;
}

static const char *route_to_string(enum mcp_tool_route route)
{
    switch (route) {
    case MCP_TOOL_ROUTE_LOCAL_BUILTIN:
        return "local_builtin";
    case MCP_TOOL_ROUTE_LOCAL_MODULE:
        return "local_module";
    case MCP_TOOL_ROUTE_EMBEDDED_ENDPOINT:
        return "embedded_endpoint";
    case MCP_TOOL_ROUTE_REMOTE_SERVER:
        return "remote_server";
    default:
        return "unknown";
    }
}

static json_t *descriptor_public_view(const struct mcp_tool_descriptor *descriptor)
{
    json_t *tool = json_object();
    json_t *annotations = json_object();

    json_object_set_new(tool, "name", json_string(descriptor->name));
    json_object_set_new(tool, "description", json_string(descriptor->description));
    json_object_set(tool, "inputSchema", descriptor->input_schema);

    json_object_set_new(annotations, "source", json_string(descriptor->source));
    json_object_set_new(annotations, "route", json_string(route_to_string(descriptor->route)));
    json_object_set_new(annotations, "risk_level", json_string(descriptor->risk_level));
    json_object_set_new(annotations, "permission", json_string(descriptor->permission));
    json_object_set_new(annotations, "idempotent", json_boolean(descriptor->idempotent));
    json_object_set_new(annotations, "retryable", json_boolean(descriptor->retryable));
    json_object_set_new(annotations, "cancelable", json_boolean(descriptor->cancelable));
    json_object_set_new(annotations, "timeout_ms", json_integer(descriptor->timeout_ms));
    json_object_set_new(tool, "annotations", annotations);

    return tool;
}

json_t *mcp_tool_registry_public_list(struct mcp_tool_registry *registry)
{
    json_t *result = json_object();
    json_t *tools = json_array();
    size_t i;

    for (i = 0; registry && i < registry->count; i++) {
        const struct mcp_tool_descriptor *descriptor = &registry->tools[i].descriptor;
        if (descriptor->enabled)
            json_array_append_new(tools, descriptor_public_view(descriptor));
    }

    json_object_set_new(result, "tools", tools);
    json_object_set_new(result,
                        "registryVersion",
                        json_integer((json_int_t)(registry ? registry->version : 0)));
    return result;
}

json_t *mcp_tool_registry_internal_list(struct mcp_tool_registry *registry)
{
    json_t *result = json_object();
    json_t *tools = json_array();
    size_t i;

    for (i = 0; registry && i < registry->count; i++) {
        const struct mcp_tool_descriptor *descriptor = &registry->tools[i].descriptor;
        json_t *tool = descriptor_public_view(descriptor);

        json_object_set_new(tool, "enabled", json_boolean(descriptor->enabled));
        json_object_set_new(tool, "version", json_string(descriptor->version));
        json_array_append_new(tools, tool);
    }

    json_object_set_new(result, "tools", tools);
    json_object_set_new(result,
                        "registryVersion",
                        json_integer((json_int_t)(registry ? registry->version : 0)));
    return result;
}

size_t mcp_tool_registry_count(struct mcp_tool_registry *registry)
{
    return registry ? registry->count : 0;
}

uint64_t mcp_tool_registry_version(struct mcp_tool_registry *registry)
{
    return registry ? registry->version : 0;
}
