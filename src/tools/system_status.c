#include "tools/system_status.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <stdio.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>
#endif

#ifndef _WIN32
static bool command_candidate_exists(const char *dir,
                                     size_t dir_len,
                                     const char *command,
                                     char *out,
                                     size_t out_len)
{
    struct stat st;
    int len;

    if (!dir || dir_len == 0 || !command || command[0] == '\0')
        return false;

    len = snprintf(out, out_len, "%.*s/%s", (int)dir_len, dir, command);
    if (len < 0 || (size_t)len >= out_len)
        return false;

    return stat(out, &st) == 0 && S_ISREG(st.st_mode) && access(out, X_OK) == 0;
}

static bool find_command_on_path(const char *command, char *out, size_t out_len)
{
    const char *path = getenv("PATH");
    const char *cursor;

    if (!command || command[0] == '\0' || !out || out_len == 0)
        return false;

    if (strchr(command, '/')) {
        struct stat st;
        int len = snprintf(out, out_len, "%s", command);

        if (len < 0 || (size_t)len >= out_len)
            return false;
        return stat(out, &st) == 0 && S_ISREG(st.st_mode) && access(out, X_OK) == 0;
    }

    if (!path || path[0] == '\0')
        path = "/usr/sbin:/usr/bin:/sbin:/bin";

    cursor = path;
    while (*cursor) {
        const char *colon = strchr(cursor, ':');
        size_t len = colon ? (size_t)(colon - cursor) : strlen(cursor);

        if (len == 0) {
            if (command_candidate_exists(".", 1, command, out, out_len))
                return true;
        } else if (command_candidate_exists(cursor, len, command, out, out_len)) {
            return true;
        }

        if (!colon)
            break;
        cursor = colon + 1;
    }

    return false;
}
#else
static bool find_command_on_path(const char *command, char *out, size_t out_len)
{
    char *path = NULL;
    DWORD len;

    if (!command || !out || out_len == 0)
        return false;

    len = SearchPathA(NULL, command, ".exe", (DWORD)out_len, out, &path);
    return len > 0 && len < out_len;
}
#endif

static void add_command_locations(json_t *status)
{
    static const char *commands[] = {
        "sh",
        "ip",
        "ss",
        "uname",
        "python3",
        "python",
    };
    json_t *object = json_object();
    size_t i;

    if (!object)
        return;

    for (i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        char path[4096];

        if (find_command_on_path(commands[i], path, sizeof(path)))
            json_object_set_new(object, commands[i], json_string(path));
    }

    json_object_set_new(status, "commands", object);
}

json_t *mcp_system_status_json(void)
{
    json_t *status = json_object();
    char hostname[256] = {0};

    if (!status)
        return NULL;

#ifdef _WIN32
    {
        DWORD size = sizeof(hostname);
        OSVERSIONINFOEXA osvi;
        MEMORYSTATUSEX mem;

        if (!GetComputerNameA(hostname, &size))
            strcpy(hostname, "unknown");

        memset(&osvi, 0, sizeof(osvi));
        osvi.dwOSVersionInfoSize = sizeof(osvi);
        memset(&mem, 0, sizeof(mem));
        mem.dwLength = sizeof(mem);
        GlobalMemoryStatusEx(&mem);

        json_object_set_new(status, "os", json_string("windows"));
        json_object_set_new(status, "hostname", json_string(hostname));
        json_object_set_new(status, "memory_total_bytes", json_integer((json_int_t)mem.ullTotalPhys));
        json_object_set_new(status, "memory_available_bytes", json_integer((json_int_t)mem.ullAvailPhys));
        add_command_locations(status);
    }
#else
    {
        struct utsname uts;
        long pages = sysconf(_SC_PHYS_PAGES);
        long avail_pages = sysconf(_SC_AVPHYS_PAGES);
        long page_size = sysconf(_SC_PAGE_SIZE);

        if (gethostname(hostname, sizeof(hostname) - 1) != 0)
            strcpy(hostname, "unknown");
        if (uname(&uts) == 0) {
            json_object_set_new(status, "os", json_string(uts.sysname));
            json_object_set_new(status, "kernel", json_string(uts.release));
            json_object_set_new(status, "machine", json_string(uts.machine));
        } else {
            json_object_set_new(status, "os", json_string("unknown"));
        }
        json_object_set_new(status, "hostname", json_string(hostname));
        json_object_set_new(status, "uid", json_integer((json_int_t)getuid()));
        json_object_set_new(status, "gid", json_integer((json_int_t)getgid()));
        if (pages > 0 && page_size > 0)
            json_object_set_new(status,
                                "memory_total_bytes",
                                json_integer((json_int_t)pages * page_size));
        if (avail_pages > 0 && page_size > 0)
            json_object_set_new(status,
                                "memory_available_bytes",
                                json_integer((json_int_t)avail_pages * page_size));
        add_command_locations(status);
    }
#endif

    return status;
}
