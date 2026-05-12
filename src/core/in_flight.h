#ifndef MCP_SRC_CORE_IN_FLIGHT_H
#define MCP_SRC_CORE_IN_FLIGHT_H

#include <jansson.h>
#include <stdbool.h>
#include <stddef.h>

#include <uv.h>

struct uv_timer_s;

enum mcp_reply_transport {
    MCP_REPLY_STDIO = 1,
    MCP_REPLY_UDP = 2,
};

struct mcp_reply_target {
    enum mcp_reply_transport transport;
    struct sockaddr_storage udp_peer;
};

struct mcp_in_flight_entry {
    char *id_key;
    char *invocation_id;
    bool cancelled;
    json_t *id;
    struct mcp_reply_target reply_to;
    struct uv_timer_s *timer;
    void *op_ctx;
    void (*op_free)(void *op_ctx);
    struct mcp_in_flight_entry *next;
};

struct mcp_in_flight_map {
    struct mcp_in_flight_entry *head;
    size_t size;
    unsigned long long next_invocation;
};

void mcp_in_flight_init(struct mcp_in_flight_map *map);
void mcp_in_flight_destroy(struct mcp_in_flight_map *map);

struct mcp_in_flight_entry *mcp_in_flight_put(struct mcp_in_flight_map *map,
                                              const char *id_key,
                                              json_t *id,
                                              const struct mcp_reply_target *reply_to);
struct mcp_in_flight_entry *mcp_in_flight_get(struct mcp_in_flight_map *map,
                                              const char *id_key);
struct mcp_in_flight_entry *mcp_in_flight_remove(struct mcp_in_flight_map *map,
                                                 const char *id_key);

#endif
