#ifndef MCP_SRC_SECURITY_NETWORK_SECURITY_CONFIG_H
#define MCP_SRC_SECURITY_NETWORK_SECURITY_CONFIG_H

#include <stdbool.h>

struct mcp_network_security_config;

int mcp_network_security_config_create_from_environment(
    struct mcp_network_security_config **out,
    bool required);
void mcp_network_security_config_destroy(struct mcp_network_security_config *config);

bool mcp_network_security_config_enabled(const struct mcp_network_security_config *config);
const char *mcp_network_security_config_ca_file(
    const struct mcp_network_security_config *config);
const char *mcp_network_security_config_certificate_file(
    const struct mcp_network_security_config *config);
const char *mcp_network_security_config_private_key_file(
    const struct mcp_network_security_config *config);

#endif
