#include "tools/builtin_tools.h"

#include "common/platform.h"
#include "core/server_internal.h"
#include "mcp/embedded/mep.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/utsname.h>
#include <unistd.h>
#endif

static json_t *schema_object(void)
{
    return json_pack("{s:s,s:o}", "type", "object", "properties", json_object());
}

static json_t *schema_with_properties(json_t *properties, json_t *required)
{
    json_t *schema = json_object();

    json_object_set_new(schema, "type", json_string("object"));
    json_object_set_new(schema, "properties", properties);
    if (required)
        json_object_set_new(schema, "required", required);

    return schema;
}

json_t *mcp_tool_result_text(const char *text, bool is_error)
{
    return json_pack("{s:[{s:s,s:s}],s:b}",
                     "content",
                     "type",
                     "text",
                     "text",
                     text ? text : "",
                     "isError",
                     is_error);
}

json_t *mcp_tool_result_json_text(json_t *value, bool is_error)
{
    char *dumped = json_dumps(value, JSON_COMPACT | JSON_ENSURE_ASCII);
    json_t *result;

    if (!dumped)
        return mcp_tool_result_text("{}", is_error);

    result = mcp_tool_result_text(dumped, is_error);
    free(dumped);
    return result;
}

static bool env_enabled(const char *name)
{
    const char *value = getenv(name);

    if (!value)
        return false;
    if (value[0] == '\0' || value[0] == '0' || value[0] == 'n' || value[0] == 'N' ||
        value[0] == 'f' || value[0] == 'F')
        return false;

    return true;
}

static int tool_system_ping(struct mcp_server *server,
                            const struct mcp_tool_invocation *invocation,
                            json_t **out_result)
{
    (void)server;
    (void)invocation;

    *out_result = mcp_tool_result_text("pong", false);
    return 0;
}

static int tool_system_get_time(struct mcp_server *server,
                                const struct mcp_tool_invocation *invocation,
                                json_t **out_result)
{
    char timestamp[48];

    (void)server;
    (void)invocation;

    if (!mcp_format_utc_now(timestamp, sizeof(timestamp))) {
        *out_result = mcp_tool_result_text("Failed to read system time.", true);
        return -1;
    }

    *out_result = mcp_tool_result_text(timestamp, false);
    return 0;
}

static int tool_system_get_status(struct mcp_server *server,
                                  const struct mcp_tool_invocation *invocation,
                                  json_t **out_result)
{
    json_t *status = json_object();
    char hostname[256] = {0};

    (void)server;
    (void)invocation;

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

    *out_result = mcp_tool_result_json_text(status, false);
    json_decref(status);
    return 0;
}

static int command_has_forbidden_token(const char *command)
{
    static const char *forbidden[] = {
        " rm ",
        " del ",
        " format ",
        " shutdown ",
        " reboot ",
        " Remove-Item ",
        " rmdir ",
        NULL,
    };
    size_t i;
    char wrapped[4096];

    if (strlen(command) > 3500)
        return 1;

    snprintf(wrapped, sizeof(wrapped), " %s ", command);
    for (i = 0; forbidden[i]; i++) {
        if (strstr(wrapped, forbidden[i]))
            return 1;
    }

    return 0;
}

static int tool_system_shell_exec(struct mcp_server *server,
                                  const struct mcp_tool_invocation *invocation,
                                  json_t **out_result)
{
    json_t *command_value;
    const char *command;
    FILE *pipe;
    char buffer[256];
    char *output = NULL;
    size_t output_len = 0;
    size_t output_cap = 0;

    (void)server;

    if (!env_enabled("MCP_ENABLE_SHELL_EXEC")) {
        *out_result = mcp_tool_result_text(
            "system.shell_exec is disabled. Set MCP_ENABLE_SHELL_EXEC=1 to enable it for a trusted session.",
            true);
        return 0;
    }

    command_value = json_object_get(invocation->arguments, "command");
    if (!json_is_string(command_value)) {
        *out_result = mcp_tool_result_text("Invalid params: command must be a string.", true);
        return 0;
    }

    command = json_string_value(command_value);
    if (command_has_forbidden_token(command)) {
        *out_result = mcp_tool_result_text("Command rejected by the built-in shell safety filter.", true);
        return 0;
    }

#ifdef _WIN32
    pipe = _popen(command, "r");
#else
    pipe = popen(command, "r");
#endif
    if (!pipe) {
        *out_result = mcp_tool_result_text("Failed to start command.", true);
        return 0;
    }

    while (fgets(buffer, sizeof(buffer), pipe)) {
        size_t chunk_len = strlen(buffer);
        char *next;

        if (output_len + chunk_len > 16 * 1024)
            break;

        if (output_len + chunk_len + 1 > output_cap) {
            output_cap = output_cap == 0 ? 1024 : output_cap * 2;
            while (output_len + chunk_len + 1 > output_cap)
                output_cap *= 2;
            next = realloc(output, output_cap);
            if (!next) {
                free(output);
#ifdef _WIN32
                _pclose(pipe);
#else
                pclose(pipe);
#endif
                *out_result = mcp_tool_result_text("Failed to collect command output.", true);
                return -1;
            }
            output = next;
        }

        memcpy(output + output_len, buffer, chunk_len);
        output_len += chunk_len;
        output[output_len] = '\0';
    }

#ifdef _WIN32
    _pclose(pipe);
#else
    pclose(pipe);
#endif

    if (!output)
        output = mcp_strdup("");
    *out_result = mcp_tool_result_text(output, false);
    free(output);
    return 0;
}

static int tool_gateway_status(struct mcp_server *server,
                               const struct mcp_tool_invocation *invocation,
                               json_t **out_result)
{
    json_t *status;

    (void)invocation;

    status = mcp_gateway_status_json(server->gateway);
    json_object_set_new(status, "stdio_transport", json_string("enabled"));
    json_object_set_new(status,
                        "udp_transport",
                        json_string(mcp_server_udp_enabled(server) ? "enabled" : "disabled"));
    *out_result = mcp_tool_result_json_text(status, false);
    json_decref(status);
    return 0;
}

static int tool_registry_list_tools(struct mcp_server *server,
                                    const struct mcp_tool_invocation *invocation,
                                    json_t **out_result)
{
    json_t *tools;

    (void)invocation;

    tools = mcp_tool_registry_internal_list(server->registry);
    *out_result = mcp_tool_result_json_text(tools, false);
    json_decref(tools);
    return 0;
}

static int tool_plugin_lsmod(struct mcp_server *server,
                             const struct mcp_tool_invocation *invocation,
                             json_t **out_result)
{
    json_t *plugins;

    (void)server;
    (void)invocation;

    plugins = json_pack("{s:[],s:s}",
                        "plugins",
                        "note",
                        "Dynamic plugin loading is reserved for the next implementation stage.");
    *out_result = mcp_tool_result_json_text(plugins, false);
    json_decref(plugins);
    return 0;
}

static int tool_plugin_unsupported(struct mcp_server *server,
                                   const struct mcp_tool_invocation *invocation,
                                   json_t **out_result)
{
    (void)server;
    (void)invocation;

    *out_result = mcp_tool_result_text(
        "Dynamic plugin load/unload is not implemented in this initial build.",
        true);
    return 0;
}

static int tool_embedded_get_protocol_info(struct mcp_server *server,
                                           const struct mcp_tool_invocation *invocation,
                                           json_t **out_result)
{
    json_t *info;

    (void)server;
    (void)invocation;

    info = json_pack("{s:i,s:i,s:[s,s,s,s],s:[i,i,i,i,i,i]}",
                     "version",
                     MCP_MEP_VERSION,
                     "sof",
                     MCP_MEP_SOF,
                     "frame_types",
                     "REQ",
                     "RESP",
                     "HELLO",
                     "CAPS",
                     "commands",
                     MCP_MEP_CMD_PING,
                     MCP_MEP_CMD_GET_DEVICE_INFO,
                     MCP_MEP_CMD_GET_STATUS,
                     MCP_MEP_CMD_READ_REGISTER,
                     MCP_MEP_CMD_WRITE_REGISTER,
                     MCP_MEP_CMD_TOOL_CALL);
    *out_result = mcp_tool_result_json_text(info, false);
    json_decref(info);
    return 0;
}

static int register_tool(struct mcp_tool_registry *registry,
                         const char *name,
                         const char *description,
                         json_t *schema,
                         const char *source,
                         const char *risk,
                         const char *permission,
                         bool idempotent,
                         bool retryable,
                         uint32_t timeout_ms,
                         mcp_tool_handler_fn handler)
{
    struct mcp_tool_descriptor descriptor;
    int rc;

    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.name = name;
    descriptor.description = description;
    descriptor.input_schema = schema;
    descriptor.source = source;
    descriptor.version = MCP_SERVER_VERSION;
    descriptor.risk_level = risk;
    descriptor.permission = permission;
    descriptor.idempotent = idempotent;
    descriptor.retryable = retryable;
    descriptor.cancelable = false;
    descriptor.enabled = true;
    descriptor.timeout_ms = timeout_ms;
    descriptor.route = MCP_TOOL_ROUTE_LOCAL_BUILTIN;
    descriptor.handler = handler;

    rc = mcp_tool_registry_register(registry, &descriptor);
    json_decref(schema);
    return rc;
}

int mcp_register_builtin_tools(struct mcp_server *server, struct mcp_tool_registry *registry)
{
    (void)server;

#if MCP_HAS_TOOL_SYSTEM
    if (register_tool(registry,
                      "system.ping",
                      "Check whether the MCP server is responsive.",
                      schema_object(),
                      "builtin",
                      "L0",
                      "system.read",
                      true,
                      true,
                      1000,
                      tool_system_ping) != 0)
        return -1;

    if (register_tool(registry,
                      "system.get_time",
                      "Get current UTC time in RFC3339 format.",
                      schema_object(),
                      "builtin",
                      "L0",
                      "system.read",
                      true,
                      true,
                      1000,
                      tool_system_get_time) != 0)
        return -1;

    if (register_tool(registry,
                      "system.get_status",
                      "Read basic host operating system and memory status.",
                      schema_object(),
                      "builtin",
                      "L0",
                      "system.read",
                      true,
                      true,
                      2000,
                      tool_system_get_status) != 0)
        return -1;

    if (register_tool(registry,
                      "system.shell_exec",
                      "Execute a host shell command when explicitly enabled by environment policy.",
                      schema_with_properties(
                          json_pack("{s:{s:s,s:s}}",
                                    "command",
                                    "type",
                                    "string",
                                    "description",
                                    "Command line to execute."),
                          json_pack("[s]", "command")),
                      "builtin",
                      "L4",
                      "system.shell",
                      false,
                      false,
                      5000,
                      tool_system_shell_exec) != 0)
        return -1;
#endif

#if MCP_HAS_TOOL_GATEWAY
    if (register_tool(registry,
                      "gateway.status",
                      "Read gateway routing and transport status.",
                      schema_object(),
                      "builtin",
                      "L0",
                      "gateway.read",
                      true,
                      true,
                      1000,
                      tool_gateway_status) != 0)
        return -1;
#endif

#if MCP_HAS_TOOL_REGISTRY
    if (register_tool(registry,
                      "registry.list_tools",
                      "List the server-side tool registry with routing metadata.",
                      schema_object(),
                      "builtin",
                      "L0",
                      "registry.read",
                      true,
                      true,
                      1000,
                      tool_registry_list_tools) != 0)
        return -1;
#endif

#if MCP_HAS_TOOL_PLUGIN_MANAGER
    if (register_tool(registry,
                      "plugin_tools.lsmod",
                      "List loaded dynamic tool modules.",
                      schema_object(),
                      "builtin",
                      "L0",
                      "plugin.read",
                      true,
                      true,
                      1000,
                      tool_plugin_lsmod) != 0)
        return -1;

    if (register_tool(registry,
                      "plugin_tools.insmod",
                      "Install or load a dynamic tool module package.",
                      schema_with_properties(
                          json_pack("{s:{s:s,s:s},s:{s:s}}",
                                    "package_path",
                                    "type",
                                    "string",
                                    "description",
                                    "Path to a module package.",
                                    "enable",
                                    "type",
                                    "boolean"),
                          json_pack("[s]", "package_path")),
                      "builtin",
                      "L2",
                      "plugin.write",
                      false,
                      false,
                      5000,
                      tool_plugin_unsupported) != 0)
        return -1;

    if (register_tool(registry,
                      "plugin_tools.rmmod",
                      "Unload a dynamic tool module.",
                      schema_with_properties(
                          json_pack("{s:{s:s}}", "plugin_id", "type", "string"),
                          json_pack("[s]", "plugin_id")),
                      "builtin",
                      "L2",
                      "plugin.write",
                      false,
                      false,
                      5000,
                      tool_plugin_unsupported) != 0)
        return -1;
#endif

#if MCP_HAS_TOOL_EMBEDDED
    if (register_tool(registry,
                      "embedded.get_protocol_info",
                      "Return MCP Embedded Protocol constants supported by this host build.",
                      schema_object(),
                      "builtin",
                      "L0",
                      "embedded.read",
                      true,
                      true,
                      1000,
                      tool_embedded_get_protocol_info) != 0)
        return -1;
#endif

    return 0;
}
