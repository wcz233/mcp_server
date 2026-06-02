#include "tools/system_status.h"

#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/utsname.h>
#include <unistd.h>
#endif

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
        if (pages > 0 && page_size > 0)
            json_object_set_new(status,
                                "memory_total_bytes",
                                json_integer((json_int_t)pages * page_size));
        if (avail_pages > 0 && page_size > 0)
            json_object_set_new(status,
                                "memory_available_bytes",
                                json_integer((json_int_t)avail_pages * page_size));
    }
#endif

    return status;
}
