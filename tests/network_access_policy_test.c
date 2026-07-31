#include "network/network_access_policy.h"

#include <stdio.h>
#include <string.h>

static const char *valid_config =
    "{"
    "\"version\":1,"
    "\"enabled\":true,"
    "\"allowlist\":{\"ips\":[\"127.0.0.1\",\"192.168.16.136\"]},"
    "\"discovery\":{\"peers\":[{\"ip\":\"192.168.16.136\",\"discovery_port\":18767}]}"
    "}";

static int parse_policy(const char *text, struct mcp_network_access_policy **out, char *error)
{
    json_error_t json_error;
    json_t *root = json_loads(text, JSON_REJECT_DUPLICATES, &json_error);
    int rc;

    if (!root)
        return -1;
    rc = mcp_network_access_policy_parse_json(out, root, error, 256);
    json_decref(root);
    return rc;
}

static int expect_invalid(const char *text, const char *message)
{
    struct mcp_network_access_policy *policy = NULL;
    char error[256] = {0};

    if (parse_policy(text, &policy, error) == 0) {
        fprintf(stderr, "expected invalid config: %s\n", message);
        mcp_network_access_policy_destroy(policy);
        return -1;
    }
    return 0;
}

int main(void)
{
    struct mcp_network_access_policy *policy = NULL;
    struct sockaddr_in allowed;
    struct sockaddr_in denied;
    const char *peer_ip = NULL;
    unsigned int peer_port = 0;
    char error[256] = {0};

    if (parse_policy(valid_config, &policy, error) != 0) {
        fprintf(stderr, "valid config rejected: %s\n", error);
        return 1;
    }
    if (!mcp_network_access_policy_enabled(policy) ||
        !mcp_network_access_policy_allows_ip(policy, "127.0.0.1") ||
        mcp_network_access_policy_allows_ip(policy, "127.0.0.2") ||
        mcp_network_access_policy_peer_count(policy) != 1 ||
        !mcp_network_access_policy_peer_at(policy, 0, &peer_ip, &peer_port) ||
        strcmp(peer_ip, "192.168.16.136") != 0 ||
        peer_port != 18767) {
        fputs("valid policy contract mismatch\n", stderr);
        mcp_network_access_policy_destroy(policy);
        return 1;
    }
    uv_ip4_addr("127.0.0.1", 1, &allowed);
    uv_ip4_addr("127.0.0.2", 1, &denied);
    if (!mcp_network_access_policy_allows_sockaddr(policy, (const struct sockaddr *)&allowed) ||
        mcp_network_access_policy_allows_sockaddr(policy, (const struct sockaddr *)&denied)) {
        fputs("sockaddr matching failed\n", stderr);
        mcp_network_access_policy_destroy(policy);
        return 1;
    }
    mcp_network_access_policy_destroy(policy);

    if (expect_invalid(
            "{\"version\":1,\"enabled\":true,\"allowlist\":{\"ips\":[]},\"discovery\":{\"peers\":[]}}",
            "enabled empty allowlist") != 0 ||
        expect_invalid(
            "{\"version\":1,\"enabled\":true,"
            "\"allowlist\":{\"ips\":[\"127.0.0.1\",\"127.0.0.1\"]},"
            "\"discovery\":{\"peers\":[]}}",
            "duplicate allowlist address") != 0 ||
        expect_invalid(
            "{\"version\":1,\"enabled\":true,"
            "\"allowlist\":{\"ips\":[\"127.0.0.1\"]},"
            "\"discovery\":{\"peers\":[{\"ip\":\"127.0.0.2\","
            "\"discovery_port\":18767}]}}",
            "peer outside allowlist") != 0 ||
        expect_invalid(
            "{\"version\":1,\"enabled\":true,\"allowlist\":{\"ips\":[\"localhost\"]},\"discovery\":{\"peers\":[]}}",
            "hostname") != 0 ||
        expect_invalid(
            "{\"version\":1,\"enabled\":false,\"allowlist\":{\"ips\":[]},\"discovery\":{\"peers\":[]},\"extra\":true}",
            "unknown field") != 0)
        return 1;

    if (mcp_network_access_policy_create_hard(&policy) != 0 ||
        mcp_network_access_policy_enabled(policy) ||
        !mcp_network_access_policy_allows_ip(policy, "203.0.113.10")) {
        fputs("hard profile mismatch\n", stderr);
        mcp_network_access_policy_destroy(policy);
        return 1;
    }
    mcp_network_access_policy_destroy(policy);
    return 0;
}
