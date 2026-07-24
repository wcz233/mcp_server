#ifndef MCP_SRC_COMMON_JSON_COUNTER_H
#define MCP_SRC_COMMON_JSON_COUNTER_H

#include <stdint.h>

#define MCP_JSON_COUNTER_MAX ((uint64_t)INT64_MAX)

/* JSON remains numeric through INT64_MAX; JavaScript exactness ends at 2^53 - 1. */
static inline void mcp_json_counter_increment(uint64_t *counter)
{
    if (*counter < MCP_JSON_COUNTER_MAX)
        (*counter)++;
}

#endif
