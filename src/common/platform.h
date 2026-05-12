#ifndef MCP_SRC_COMMON_PLATFORM_H
#define MCP_SRC_COMMON_PLATFORM_H

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <uv.h>

static inline char *mcp_strdup(const char *s)
{
#ifdef _WIN32
    return _strdup(s);
#else
    return strdup(s);
#endif
}

static inline bool mcp_format_utc_now(char *out, size_t out_len)
{
    uv_timeval64_t tv;
    struct tm tm_utc;
    char base[32];
    time_t secs;

    if (!out || out_len == 0)
        return false;

    if (uv_gettimeofday(&tv) != 0)
        return false;

    secs = (time_t)tv.tv_sec;
#ifdef _WIN32
    if (gmtime_s(&tm_utc, &secs) != 0)
        return false;
#else
    if (!gmtime_r(&secs, &tm_utc))
        return false;
#endif

    if (strftime(base, sizeof(base), "%Y-%m-%dT%H:%M:%S", &tm_utc) == 0)
        return false;

    if (snprintf(out, out_len, "%s.%03lldZ", base, (long long)(tv.tv_usec / 1000)) < 0)
        return false;

    return true;
}

static inline unsigned long long mcp_now_ms(void)
{
    uv_timeval64_t tv;

    if (uv_gettimeofday(&tv) != 0)
        return 0;

    return ((unsigned long long)tv.tv_sec * 1000ull) +
           ((unsigned long long)tv.tv_usec / 1000ull);
}

#endif
