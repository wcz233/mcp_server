#include "network/network_access_policy.h"

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

#ifndef MCP_NETWORK_ACCESS_DEFAULT_CONFIG
#define MCP_NETWORK_ACCESS_DEFAULT_CONFIG ""
#endif

#ifndef MCP_NETWORK_ACCESS_INSTALLED_CONFIG
#define MCP_NETWORK_ACCESS_INSTALLED_CONFIG ""
#endif

struct mcp_network_allowed_ip {
    struct in_addr address;
    char text[INET_ADDRSTRLEN];
};

struct mcp_network_discovery_peer {
    size_t allowed_ip_index;
    unsigned int discovery_port;
};

struct mcp_network_access_policy {
    bool enabled;
    struct mcp_network_allowed_ip *allowed_ips;
    size_t allowed_ip_count;
    struct mcp_network_discovery_peer *peers;
    size_t peer_count;
};

static void policy_error(char *error, size_t error_size, const char *format, ...)
{
    va_list args;

    if (!error || error_size == 0)
        return;
    va_start(args, format);
    vsnprintf(error, error_size, format, args);
    va_end(args);
}

static bool key_is_listed(const char *key, const char *const *keys, size_t key_count)
{
    size_t index;

    for (index = 0; index < key_count; index++) {
        if (strcmp(key, keys[index]) == 0)
            return true;
    }
    return false;
}

static int validate_object_shape(const json_t *object,
                                 const char *path,
                                 const char *const *keys,
                                 size_t key_count,
                                 char *error,
                                 size_t error_size)
{
    const char *key;
    void *iter;
    size_t index;

    if (!json_is_object(object)) {
        policy_error(error, error_size, "%s must be an object", path);
        return -1;
    }

    iter = json_object_iter((json_t *)object);
    while (iter) {
        key = json_object_iter_key(iter);
        if (!key_is_listed(key, keys, key_count)) {
            policy_error(error, error_size, "%s contains unknown field %s", path, key);
            return -1;
        }
        iter = json_object_iter_next((json_t *)object, iter);
    }
    for (index = 0; index < key_count; index++) {
        if (!json_object_get(object, keys[index])) {
            policy_error(error, error_size, "%s.%s is required", path, keys[index]);
            return -1;
        }
    }
    return 0;
}

static int parse_ipv4(const char *text,
                      struct in_addr *address,
                      char *canonical,
                      size_t canonical_size)
{
    struct sockaddr_in addr;
    uint32_t host;

    if (!text || text[0] == '\0' || uv_ip4_addr(text, 0, &addr) != 0)
        return -1;

    host = ntohl(addr.sin_addr.s_addr);
    if ((host >> 24) == 0 || (host >> 28) >= 14)
        return -1;
    if (uv_ip4_name(&addr, canonical, canonical_size) != 0)
        return -1;

    *address = addr.sin_addr;
    return 0;
}

static int find_allowed_ip(const struct mcp_network_access_policy *policy,
                           const struct in_addr *address)
{
    size_t index;

    for (index = 0; index < policy->allowed_ip_count; index++) {
        if (policy->allowed_ips[index].address.s_addr == address->s_addr)
            return (int)index;
    }
    return -1;
}

static int parse_allowed_ips(struct mcp_network_access_policy *policy,
                             const json_t *array,
                             char *error,
                             size_t error_size)
{
    size_t index;

    if (!json_is_array(array)) {
        policy_error(error, error_size, "allowlist.ips must be an array");
        return -1;
    }

    policy->allowed_ip_count = json_array_size(array);
    if (policy->enabled && policy->allowed_ip_count == 0) {
        policy_error(error, error_size, "allowlist.ips must not be empty when enabled");
        return -1;
    }
    if (policy->allowed_ip_count == 0)
        return 0;

    policy->allowed_ips = calloc(policy->allowed_ip_count, sizeof(*policy->allowed_ips));
    if (!policy->allowed_ips) {
        policy_error(error, error_size, "out of memory");
        return -1;
    }

    for (index = 0; index < policy->allowed_ip_count; index++) {
        json_t *value = json_array_get(array, index);
        const char *text;

        if (!json_is_string(value)) {
            policy_error(error, error_size, "allowlist.ips[%zu] must be a string", index);
            return -1;
        }
        text = json_string_value(value);
        if (parse_ipv4(text,
                       &policy->allowed_ips[index].address,
                       policy->allowed_ips[index].text,
                       sizeof(policy->allowed_ips[index].text)) != 0) {
            policy_error(error, error_size, "allowlist.ips[%zu] must be an exact IPv4 address", index);
            return -1;
        }
        if (find_allowed_ip(policy, &policy->allowed_ips[index].address) != (int)index) {
            policy_error(error, error_size, "allowlist.ips contains duplicate address %s", text);
            return -1;
        }
    }
    return 0;
}

static int parse_discovery_peers(struct mcp_network_access_policy *policy,
                                 const json_t *array,
                                 char *error,
                                 size_t error_size)
{
    static const char *const peer_keys[] = {"ip", "discovery_port"};
    size_t index;

    if (!json_is_array(array)) {
        policy_error(error, error_size, "discovery.peers must be an array");
        return -1;
    }

    policy->peer_count = json_array_size(array);
    if (policy->peer_count == 0)
        return 0;
    policy->peers = calloc(policy->peer_count, sizeof(*policy->peers));
    if (!policy->peers) {
        policy_error(error, error_size, "out of memory");
        return -1;
    }

    for (index = 0; index < policy->peer_count; index++) {
        json_t *peer = json_array_get(array, index);
        json_t *ip;
        json_t *port;
        struct in_addr address;
        char canonical[INET_ADDRSTRLEN];
        int allowed_index;
        size_t previous;

        if (validate_object_shape(peer,
                                  "discovery.peers[]",
                                  peer_keys,
                                  sizeof(peer_keys) / sizeof(peer_keys[0]),
                                  error,
                                  error_size) != 0)
            return -1;
        ip = json_object_get(peer, "ip");
        port = json_object_get(peer, "discovery_port");
        if (!json_is_string(ip) ||
            parse_ipv4(json_string_value(ip), &address, canonical, sizeof(canonical)) != 0) {
            policy_error(error, error_size, "discovery.peers[%zu].ip must be an exact IPv4 address", index);
            return -1;
        }
        if (!json_is_integer(port) ||
            json_integer_value(port) < 1 ||
            json_integer_value(port) > 65535) {
            policy_error(error, error_size, "discovery.peers[%zu].discovery_port must be in 1..65535", index);
            return -1;
        }
        allowed_index = find_allowed_ip(policy, &address);
        if (allowed_index < 0) {
            policy_error(error,
                         error_size,
                         "discovery.peers[%zu].ip must be present in allowlist.ips",
                         index);
            return -1;
        }

        policy->peers[index].allowed_ip_index = (size_t)allowed_index;
        policy->peers[index].discovery_port = (unsigned int)json_integer_value(port);
        for (previous = 0; previous < index; previous++) {
            if (policy->peers[previous].allowed_ip_index == policy->peers[index].allowed_ip_index &&
                policy->peers[previous].discovery_port == policy->peers[index].discovery_port) {
                policy_error(error, error_size, "discovery.peers contains a duplicate endpoint");
                return -1;
            }
        }
    }
    return 0;
}

int mcp_network_access_policy_create_hard(struct mcp_network_access_policy **out)
{
    struct mcp_network_access_policy *policy;

    if (!out)
        return -1;
    *out = NULL;
    policy = calloc(1, sizeof(*policy));
    if (!policy)
        return -1;
    *out = policy;
    return 0;
}

int mcp_network_access_policy_parse_json(struct mcp_network_access_policy **out,
                                         const json_t *root,
                                         char *error,
                                         size_t error_size)
{
    static const char *const root_keys[] = {"version", "enabled", "allowlist", "discovery"};
    static const char *const allowlist_keys[] = {"ips"};
    static const char *const discovery_keys[] = {"peers"};
    struct mcp_network_access_policy *policy;
    json_t *version;
    json_t *enabled;
    json_t *allowlist;
    json_t *discovery;

    if (!out)
        return -1;
    *out = NULL;
    if (validate_object_shape(root,
                              "config",
                              root_keys,
                              sizeof(root_keys) / sizeof(root_keys[0]),
                              error,
                              error_size) != 0)
        return -1;

    version = json_object_get(root, "version");
    enabled = json_object_get(root, "enabled");
    allowlist = json_object_get(root, "allowlist");
    discovery = json_object_get(root, "discovery");
    if (!json_is_integer(version) || json_integer_value(version) != 1) {
        policy_error(error, error_size, "version must be 1");
        return -1;
    }
    if (!json_is_boolean(enabled)) {
        policy_error(error, error_size, "enabled must be a boolean");
        return -1;
    }
    if (validate_object_shape(allowlist,
                              "allowlist",
                              allowlist_keys,
                              1,
                              error,
                              error_size) != 0 ||
        validate_object_shape(discovery,
                              "discovery",
                              discovery_keys,
                              1,
                              error,
                              error_size) != 0)
        return -1;

    policy = calloc(1, sizeof(*policy));
    if (!policy) {
        policy_error(error, error_size, "out of memory");
        return -1;
    }
    policy->enabled = json_is_true(enabled);
    if (parse_allowed_ips(policy, json_object_get(allowlist, "ips"), error, error_size) != 0 ||
        parse_discovery_peers(policy,
                              json_object_get(discovery, "peers"),
                              error,
                              error_size) != 0) {
        mcp_network_access_policy_destroy(policy);
        return -1;
    }

    *out = policy;
    return 0;
}

static int load_policy_file(struct mcp_network_access_policy **out,
                            const char *path,
                            bool explicit_config)
{
    json_error_t json_error;
    json_t *root;
    char error[256];

    root = json_load_file(path, JSON_REJECT_DUPLICATES, &json_error);
    if (!root) {
        fprintf(stderr,
                "Failed to load %snetwork access config %s: %s\n",
                explicit_config ? "explicit " : "",
                path,
                json_error_code(&json_error) == json_error_cannot_open_file
                    ? "file unavailable"
                    : "invalid JSON");
        return -1;
    }
    if (mcp_network_access_policy_parse_json(out, root, error, sizeof(error)) != 0) {
        fprintf(stderr, "Invalid network access config %s: %s\n", path, error);
        json_decref(root);
        return -1;
    }
    json_decref(root);
    return 0;
}

int mcp_network_access_policy_create_from_environment(struct mcp_network_access_policy **out)
{
    const char *explicit_path;
    const char *candidates[] = {
        MCP_NETWORK_ACCESS_DEFAULT_CONFIG,
        MCP_NETWORK_ACCESS_INSTALLED_CONFIG,
    };
    size_t index;

    if (!out)
        return -1;
    *out = NULL;
    explicit_path = getenv("MCP_NETWORK_ACCESS_CONFIG");
    if (explicit_path && explicit_path[0] != '\0') {
        if (load_policy_file(out, explicit_path, true) != 0)
            return -1;
    } else {
        for (index = 0; index < sizeof(candidates) / sizeof(candidates[0]); index++) {
            FILE *probe;

            if (candidates[index][0] == '\0')
                continue;
            errno = 0;
            probe = fopen(candidates[index], "rb");
            if (!probe) {
                if (errno == ENOENT)
                    continue;
                fprintf(stderr, "Failed to access network access config %s\n", candidates[index]);
                return -1;
            }
            fclose(probe);
            if (load_policy_file(out, candidates[index], false) != 0)
                return -1;
            break;
        }
        if (!*out && mcp_network_access_policy_create_hard(out) != 0)
            return -1;
    }

    if ((*out)->enabled) {
        const char *legacy_hosts = getenv("MCP_DISCOVERY_HOSTS");

        if (legacy_hosts && legacy_hosts[0] != '\0') {
            fputs("MCP_DISCOVERY_HOSTS cannot be used when network access policy is enabled\n",
                  stderr);
            mcp_network_access_policy_destroy(*out);
            *out = NULL;
            return -1;
        }
    }
    return 0;
}

void mcp_network_access_policy_destroy(struct mcp_network_access_policy *policy)
{
    if (!policy)
        return;
    free(policy->allowed_ips);
    free(policy->peers);
    free(policy);
}

bool mcp_network_access_policy_enabled(const struct mcp_network_access_policy *policy)
{
    return policy && policy->enabled;
}

bool mcp_network_access_policy_allows_ip(const struct mcp_network_access_policy *policy,
                                         const char *ip)
{
    struct in_addr address;
    char canonical[INET_ADDRSTRLEN];

    if (!mcp_network_access_policy_enabled(policy))
        return true;
    if (parse_ipv4(ip, &address, canonical, sizeof(canonical)) != 0)
        return false;
    return find_allowed_ip(policy, &address) >= 0;
}

bool mcp_network_access_policy_allows_sockaddr(const struct mcp_network_access_policy *policy,
                                               const struct sockaddr *addr)
{
    const struct sockaddr_in *addr4;

    if (!mcp_network_access_policy_enabled(policy))
        return true;
    if (!addr || addr->sa_family != AF_INET)
        return false;
    addr4 = (const struct sockaddr_in *)addr;
    return find_allowed_ip(policy, &addr4->sin_addr) >= 0;
}

size_t mcp_network_access_policy_peer_count(const struct mcp_network_access_policy *policy)
{
    if (!mcp_network_access_policy_enabled(policy))
        return 0;
    return policy->peer_count;
}

bool mcp_network_access_policy_peer_at(const struct mcp_network_access_policy *policy,
                                       size_t index,
                                       const char **ip,
                                       unsigned int *discovery_port)
{
    const struct mcp_network_discovery_peer *peer;

    if (!mcp_network_access_policy_enabled(policy) || index >= policy->peer_count)
        return false;
    peer = &policy->peers[index];
    if (ip)
        *ip = policy->allowed_ips[peer->allowed_ip_index].text;
    if (discovery_port)
        *discovery_port = peer->discovery_port;
    return true;
}
