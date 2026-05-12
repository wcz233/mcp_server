#ifndef MCP_SRC_PROTOCOL_JSONRPC_H
#define MCP_SRC_PROTOCOL_JSONRPC_H

#include <jansson.h>
#include <stdbool.h>
#include <stddef.h>

enum mcp_jsonrpc_message_type {
    MCP_JSONRPC_REQUEST = 1,
    MCP_JSONRPC_NOTIFICATION = 2,
};

struct mcp_jsonrpc_message {
    enum mcp_jsonrpc_message_type type;
    char *method;
    json_t *id;
    json_t *params;
};

void mcp_jsonrpc_message_destroy(struct mcp_jsonrpc_message *message);

int mcp_jsonrpc_parse_line(const char *line,
                           size_t len,
                           struct mcp_jsonrpc_message **out_message,
                           json_t **out_error);

char *mcp_jsonrpc_dump_line(json_t *object);

json_t *mcp_jsonrpc_build_response(json_t *id, json_t *result);
json_t *mcp_jsonrpc_build_error(json_t *id, int code, const char *message);
json_t *mcp_jsonrpc_build_error_with_data(json_t *id,
                                          int code,
                                          const char *message,
                                          json_t *data);

bool mcp_jsonrpc_id_to_key(json_t *id, char **out_key);

#endif
