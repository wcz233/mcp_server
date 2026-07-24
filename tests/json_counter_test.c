#include "common/json_counter.h"
#include "mcp/registry/tool_registry.h"

#include <jansson.h>
#include <stdint.h>
#include <stdio.h>

#define CHECK(condition)                                                                      \
    do {                                                                                      \
        if (!(condition)) {                                                                   \
            fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return -1;                                                                        \
        }                                                                                     \
    } while (0)

static int test_saturation(void)
{
    uint64_t counter = MCP_JSON_COUNTER_MAX - 1;

    mcp_json_counter_increment(&counter);
    CHECK(counter == MCP_JSON_COUNTER_MAX);
    mcp_json_counter_increment(&counter);
    CHECK(counter == MCP_JSON_COUNTER_MAX);
    return 0;
}

static int test_registry_json(void)
{
    struct mcp_tool_registry *registry = NULL;
    struct mcp_tool_descriptor descriptor = {0};
    json_t *list = NULL;
    json_t *version;
    int rc = -1;

    CHECK(mcp_tool_registry_create(&registry) == 0);
    CHECK(mcp_tool_registry_version(registry) == UINT64_C(1));

    descriptor.name = "counter.test";
    descriptor.description = "Counter test tool.";
    descriptor.input_schema = json_object();
    descriptor.source = "test";
    descriptor.version = "1";
    descriptor.risk_level = "low";
    descriptor.permission = "none";
    descriptor.enabled = true;
    descriptor.route = MCP_TOOL_ROUTE_LOCAL_BUILTIN;
    CHECK(descriptor.input_schema != NULL);
    CHECK(mcp_tool_registry_register(registry, &descriptor) == 0);
    CHECK(mcp_tool_registry_version(registry) == UINT64_C(2));

    list = mcp_tool_registry_public_list(registry);
    version = list ? json_object_get(list, "registryVersion") : NULL;
    CHECK(json_is_integer(version));
    CHECK(json_integer_value(version) == 2);
    rc = 0;

    json_decref(list);
    json_decref(descriptor.input_schema);
    mcp_tool_registry_destroy(registry);
    return rc;
}

int main(void)
{
    if (test_saturation() != 0 || test_registry_json() != 0)
        return 1;
    return 0;
}
