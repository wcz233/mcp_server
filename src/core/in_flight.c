#include "core/in_flight.h"

#include "common/platform.h"

#include <stdlib.h>
#include <string.h>

void mcp_in_flight_init(struct mcp_in_flight_map *map)
{
    map->head = NULL;
    map->size = 0;
    map->next_invocation = 1;
}

static void in_flight_entry_free(struct mcp_in_flight_entry *entry)
{
    if (entry->op_free && entry->op_ctx)
        entry->op_free(entry->op_ctx);

    json_decref(entry->id);
    free(entry->id_key);
    free(entry->invocation_id);
    free(entry);
}

void mcp_in_flight_destroy(struct mcp_in_flight_map *map)
{
    struct mcp_in_flight_entry *current = map->head;

    while (current) {
        struct mcp_in_flight_entry *next = current->next;
        in_flight_entry_free(current);
        current = next;
    }

    map->head = NULL;
    map->size = 0;
}

struct mcp_in_flight_entry *mcp_in_flight_put(struct mcp_in_flight_map *map,
                                              const char *id_key,
                                              json_t *id)
{
    struct mcp_in_flight_entry *entry = calloc(1, sizeof(*entry));
    int len;

    if (!entry)
        return NULL;

    entry->id_key = mcp_strdup(id_key);
    if (!entry->id_key) {
        free(entry);
        return NULL;
    }

    len = snprintf(NULL, 0, "srv_call_%llu", map->next_invocation++);
    if (len < 0) {
        free(entry->id_key);
        free(entry);
        return NULL;
    }

    entry->invocation_id = malloc((size_t)len + 1);
    if (!entry->invocation_id) {
        free(entry->id_key);
        free(entry);
        return NULL;
    }
    snprintf(entry->invocation_id, (size_t)len + 1, "srv_call_%llu", map->next_invocation - 1);

    entry->id = json_incref(id);
    entry->next = map->head;
    map->head = entry;
    map->size++;

    return entry;
}

struct mcp_in_flight_entry *mcp_in_flight_get(struct mcp_in_flight_map *map,
                                              const char *id_key)
{
    struct mcp_in_flight_entry *current;

    for (current = map->head; current; current = current->next) {
        if (strcmp(current->id_key, id_key) == 0)
            return current;
    }

    return NULL;
}

struct mcp_in_flight_entry *mcp_in_flight_remove(struct mcp_in_flight_map *map,
                                                 const char *id_key)
{
    struct mcp_in_flight_entry **previous = &map->head;
    struct mcp_in_flight_entry *current = map->head;

    while (current) {
        if (strcmp(current->id_key, id_key) == 0) {
            *previous = current->next;
            current->next = NULL;
            map->size--;
            return current;
        }

        previous = &current->next;
        current = current->next;
    }

    return NULL;
}
