#ifndef MCP_SRC_NETWORK_NETWORK_ACCESS_POLICY_H
#define MCP_SRC_NETWORK_NETWORK_ACCESS_POLICY_H

#include <stdbool.h>
#include <stddef.h>

#include <jansson.h>
#include <uv.h>

struct mcp_network_access_policy;

int mcp_network_access_policy_create_hard(struct mcp_network_access_policy **out);
int mcp_network_access_policy_create_from_environment(struct mcp_network_access_policy **out);
int mcp_network_access_policy_parse_json(struct mcp_network_access_policy **out,
                                         const json_t *root,
                                         char *error,
                                         size_t error_size);
void mcp_network_access_policy_destroy(struct mcp_network_access_policy *policy);

bool mcp_network_access_policy_enabled(const struct mcp_network_access_policy *policy);
bool mcp_network_access_policy_allows_ip(const struct mcp_network_access_policy *policy,
                                         const char *ip);
bool mcp_network_access_policy_allows_sockaddr(const struct mcp_network_access_policy *policy,
                                               const struct sockaddr *addr);
size_t mcp_network_access_policy_peer_count(const struct mcp_network_access_policy *policy);
bool mcp_network_access_policy_peer_at(const struct mcp_network_access_policy *policy,
                                       size_t index,
                                       const char **ip,
                                       unsigned int *discovery_port);

#endif
