#ifndef MCP_REGISTRY_TOOL_REGISTRY_H
#define MCP_REGISTRY_TOOL_REGISTRY_H

#include <stdbool.h>
#include <stdint.h>

#include <jansson.h>

#include "mcp/tools/tool.h"

struct mcp_tool_registry;

int mcp_tool_registry_create(struct mcp_tool_registry **out);
void mcp_tool_registry_destroy(struct mcp_tool_registry *registry);

int mcp_tool_registry_register(struct mcp_tool_registry *registry,
                               const struct mcp_tool_descriptor *descriptor);
int mcp_tool_registry_unregister(struct mcp_tool_registry *registry, const char *name);
int mcp_tool_registry_set_enabled(struct mcp_tool_registry *registry,
                                  const char *name,
                                  bool enabled);
const struct mcp_tool_descriptor *mcp_tool_registry_find(struct mcp_tool_registry *registry,
                                                         const char *name);
json_t *mcp_tool_registry_public_list(struct mcp_tool_registry *registry);
json_t *mcp_tool_registry_internal_list(struct mcp_tool_registry *registry);
size_t mcp_tool_registry_count(struct mcp_tool_registry *registry);
uint64_t mcp_tool_registry_version(struct mcp_tool_registry *registry);

#endif
