#include "protocol/jsonrpc.h"

#include "common/platform.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static bool json_is_string_value(json_t *value, const char *expected)
{
    return json_is_string(value) && strcmp(json_string_value(value), expected) == 0;
}

static bool jsonrpc_validate_id(json_t *id)
{
    return json_is_integer(id) || json_is_string(id);
}

void mcp_jsonrpc_message_destroy(struct mcp_jsonrpc_message *message)
{
    if (!message)
        return;

    free(message->method);
    json_decref(message->id);
    json_decref(message->params);
    free(message);
}

json_t *mcp_jsonrpc_build_response(json_t *id, json_t *result)
{
    json_t *object = json_object();

    json_object_set_new(object, "jsonrpc", json_string("2.0"));
    json_object_set(object, "id", id ? id : json_null());

    if (result)
        json_object_set(object, "result", result);
    else
        json_object_set_new(object, "result", json_object());

    return object;
}

json_t *mcp_jsonrpc_build_error_with_data(json_t *id,
                                          int code,
                                          const char *message,
                                          json_t *data)
{
    json_t *object = json_object();
    json_t *error = json_object();

    json_object_set_new(object, "jsonrpc", json_string("2.0"));
    json_object_set(object, "id", id ? id : json_null());

    json_object_set_new(error, "code", json_integer(code));
    json_object_set_new(error, "message", json_string(message ? message : "Error"));
    if (data)
        json_object_set(error, "data", data);
    json_object_set_new(object, "error", error);

    return object;
}

json_t *mcp_jsonrpc_build_error(json_t *id, int code, const char *message)
{
    return mcp_jsonrpc_build_error_with_data(id, code, message, NULL);
}

char *mcp_jsonrpc_dump_line(json_t *object)
{
    char *json = json_dumps(object, JSON_COMPACT | JSON_ENSURE_ASCII);
    char *line;
    size_t len;

    if (!json)
        return NULL;

    len = strlen(json);
    line = malloc(len + 2);
    if (!line) {
        free(json);
        return NULL;
    }

    memcpy(line, json, len);
    line[len] = '\n';
    line[len + 1] = '\0';
    free(json);

    return line;
}

bool mcp_jsonrpc_id_to_key(json_t *id, char **out_key)
{
    char *key;

    *out_key = NULL;
    if (!id || !jsonrpc_validate_id(id))
        return false;

    if (json_is_integer(id)) {
        long long value = json_integer_value(id);
        int len = snprintf(NULL, 0, "i:%lld", value);

        if (len < 0)
            return false;

        key = malloc((size_t)len + 1);
        if (!key)
            return false;

        snprintf(key, (size_t)len + 1, "i:%lld", value);
        *out_key = key;
        return true;
    }

    {
        const char *value = json_string_value(id);
        size_t len = strlen(value);

        key = malloc(len + 3);
        if (!key)
            return false;

        key[0] = 's';
        key[1] = ':';
        memcpy(key + 2, value, len);
        key[len + 2] = '\0';
        *out_key = key;
        return true;
    }
}

int mcp_jsonrpc_parse_line(const char *line,
                           size_t len,
                           struct mcp_jsonrpc_message **out_message,
                           json_t **out_error)
{
    json_error_t json_error;
    json_t *root;
    json_t *jsonrpc;
    json_t *method;
    json_t *id;
    json_t *params;
    struct mcp_jsonrpc_message *message;

    *out_message = NULL;
    *out_error = NULL;

    root = json_loadb(line, len, JSON_REJECT_DUPLICATES, &json_error);
    if (!root) {
        *out_error = mcp_jsonrpc_build_error(NULL, -32700, "Parse error");
        return -1;
    }

    if (!json_is_object(root)) {
        *out_error = mcp_jsonrpc_build_error(NULL, -32600, "Invalid Request");
        json_decref(root);
        return -1;
    }

    jsonrpc = json_object_get(root, "jsonrpc");
    method = json_object_get(root, "method");
    id = json_object_get(root, "id");
    params = json_object_get(root, "params");

    if (!json_is_string_value(jsonrpc, "2.0") || !json_is_string(method)) {
        *out_error = mcp_jsonrpc_build_error(NULL, -32600, "Invalid Request");
        json_decref(root);
        return -1;
    }

    if (id && !jsonrpc_validate_id(id)) {
        *out_error = mcp_jsonrpc_build_error(NULL, -32600, "Invalid Request");
        json_decref(root);
        return -1;
    }

    message = calloc(1, sizeof(*message));
    if (!message) {
        *out_error = mcp_jsonrpc_build_error(NULL, -32603, "Internal error");
        json_decref(root);
        return -1;
    }

    message->method = mcp_strdup(json_string_value(method));
    if (!message->method) {
        free(message);
        *out_error = mcp_jsonrpc_build_error(NULL, -32603, "Internal error");
        json_decref(root);
        return -1;
    }

    if (id) {
        message->type = MCP_JSONRPC_REQUEST;
        message->id = json_incref(id);
    } else {
        message->type = MCP_JSONRPC_NOTIFICATION;
        message->id = NULL;
    }

    if (params)
        message->params = json_incref(params);
    else
        message->params = json_object();

    *out_message = message;
    json_decref(root);
    return 0;
}
