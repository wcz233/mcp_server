#include "security/network_security_config.h"

#include "common/platform.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef MCP_NETWORK_SECURITY_DEFAULT_CONFIG
#define MCP_NETWORK_SECURITY_DEFAULT_CONFIG ""
#endif

#ifndef MCP_NETWORK_SECURITY_INSTALLED_CONFIG
#define MCP_NETWORK_SECURITY_INSTALLED_CONFIG ""
#endif

struct mcp_network_security_config {
    bool enabled;
    char *ca_file;
    char *certificate_file;
    char *private_key_file;
};

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
        snprintf(error, error_size, "%s must be an object", path);
        return -1;
    }
    iter = json_object_iter((json_t *)object);
    while (iter) {
        key = json_object_iter_key(iter);
        if (!key_is_listed(key, keys, key_count)) {
            snprintf(error, error_size, "%s contains unknown field %s", path, key);
            return -1;
        }
        iter = json_object_iter_next((json_t *)object, iter);
    }
    for (index = 0; index < key_count; index++) {
        if (!json_object_get(object, keys[index])) {
            snprintf(error, error_size, "%s.%s is required", path, keys[index]);
            return -1;
        }
    }
    return 0;
}

static int copy_string(char **out, json_t *value)
{
    const char *text = json_string_value(value);

    *out = mcp_strdup(text);
    return *out ? 0 : -1;
}

static int parse_config(struct mcp_network_security_config **out,
                        json_t *root,
                        char *error,
                        size_t error_size)
{
    static const char *const root_keys[] = {"version", "enabled", "mtls"};
    static const char *const mtls_keys[] = {
        "ca_file", "certificate_file", "private_key_file", "server_name"};
    struct mcp_network_security_config *config = NULL;
    json_t *version;
    json_t *enabled;
    json_t *mtls;
    json_t *ca_file;
    json_t *certificate_file;
    json_t *private_key_file;
    json_t *server_name;

    *out = NULL;
    if (validate_object_shape(root, "root", root_keys, 3, error, error_size) != 0)
        return -1;
    version = json_object_get(root, "version");
    enabled = json_object_get(root, "enabled");
    mtls = json_object_get(root, "mtls");
    if (!json_is_integer(version) || json_integer_value(version) != 1) {
        snprintf(error, error_size, "version must be integer 1");
        return -1;
    }
    if (!json_is_boolean(enabled)) {
        snprintf(error, error_size, "enabled must be a boolean");
        return -1;
    }
    if (validate_object_shape(mtls, "mtls", mtls_keys, 4, error, error_size) != 0)
        return -1;

    ca_file = json_object_get(mtls, "ca_file");
    certificate_file = json_object_get(mtls, "certificate_file");
    private_key_file = json_object_get(mtls, "private_key_file");
    server_name = json_object_get(mtls, "server_name");
    if (!json_is_string(ca_file) || !json_is_string(certificate_file) ||
        !json_is_string(private_key_file) || !json_is_string(server_name)) {
        snprintf(error, error_size, "mtls fields must be strings");
        return -1;
    }
    if (json_is_true(enabled) &&
        (json_string_length(ca_file) == 0 || json_string_length(certificate_file) == 0 ||
         json_string_length(private_key_file) == 0)) {
        snprintf(error, error_size, "enabled mtls requires CA, certificate, and private key files");
        return -1;
    }

    config = calloc(1, sizeof(*config));
    if (!config)
        return -1;
    config->enabled = json_is_true(enabled);
    if (copy_string(&config->ca_file, ca_file) != 0 ||
        copy_string(&config->certificate_file, certificate_file) != 0 ||
        copy_string(&config->private_key_file, private_key_file) != 0) {
        mcp_network_security_config_destroy(config);
        return -1;
    }
    *out = config;
    return 0;
}

static int load_config_file(struct mcp_network_security_config **out,
                            const char *path,
                            bool explicit_path)
{
    json_error_t json_error;
    json_t *root;
    char error[256];

    root = json_load_file(path, JSON_REJECT_DUPLICATES, &json_error);
    if (!root) {
        if (!explicit_path && json_error_code(&json_error) == json_error_cannot_open_file)
            return 1;
        fprintf(stderr, "Invalid network security config %s: %s\n", path, json_error.text);
        return -1;
    }
    if (parse_config(out, root, error, sizeof(error)) != 0) {
        fprintf(stderr, "Invalid network security config %s: %s\n", path, error);
        json_decref(root);
        return -1;
    }
    json_decref(root);
    return 0;
}

int mcp_network_security_config_create_from_environment(
    struct mcp_network_security_config **out,
    bool required)
{
    const char *explicit_path;
    const char *candidates[] = {
        MCP_NETWORK_SECURITY_DEFAULT_CONFIG,
        MCP_NETWORK_SECURITY_INSTALLED_CONFIG,
    };
    size_t index;

    if (!out)
        return -1;
    *out = NULL;
    explicit_path = getenv("MCP_NETWORK_SECURITY_CONFIG");
    if (explicit_path && explicit_path[0] != '\0')
        return load_config_file(out, explicit_path, true);

    for (index = 0; index < sizeof(candidates) / sizeof(candidates[0]); index++) {
        int rc;

        if (candidates[index][0] == '\0')
            continue;
        rc = load_config_file(out, candidates[index], false);
        if (rc <= 0)
            return rc;
    }
    if (required) {
        fputs("MCP_NETWORK_SECURITY_CONFIG is required for mtls TCP\n", stderr);
        return -1;
    }
    return 0;
}

void mcp_network_security_config_destroy(struct mcp_network_security_config *config)
{
    if (!config)
        return;
    free(config->ca_file);
    free(config->certificate_file);
    free(config->private_key_file);
    free(config);
}

bool mcp_network_security_config_enabled(const struct mcp_network_security_config *config)
{
    return config && config->enabled;
}

const char *mcp_network_security_config_ca_file(
    const struct mcp_network_security_config *config)
{
    return config ? config->ca_file : NULL;
}

const char *mcp_network_security_config_certificate_file(
    const struct mcp_network_security_config *config)
{
    return config ? config->certificate_file : NULL;
}

const char *mcp_network_security_config_private_key_file(
    const struct mcp_network_security_config *config)
{
    return config ? config->private_key_file : NULL;
}
