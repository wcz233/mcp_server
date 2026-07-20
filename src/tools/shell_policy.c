#include "tools/shell_policy.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define MCP_SHELL_POLICY_HARD_SHELL_PATH "cmd.exe"
#define MCP_SHELL_POLICY_HARD_SHELL_ARG "/C"
#define MCP_SHELL_POLICY_HARD_WORKING_DIRECTORY "."
#else
#define MCP_SHELL_POLICY_HARD_SHELL_PATH "/bin/sh"
#define MCP_SHELL_POLICY_HARD_SHELL_ARG "-c"
#define MCP_SHELL_POLICY_HARD_WORKING_DIRECTORY "/tmp/mcp-shell"
#endif

#define POLICY_OFFSET(member) offsetof(struct mcp_shell_policy_defaults, member)

#ifndef MCP_SHELL_EXEC_DEFAULT_CONFIG
#define MCP_SHELL_EXEC_DEFAULT_CONFIG ""
#endif

#ifndef MCP_SHELL_EXEC_INSTALLED_CONFIG
#define MCP_SHELL_EXEC_INSTALLED_CONFIG ""
#endif

static int parse_policy_field(struct mcp_shell_policy_snapshot *snapshot,
                              const json_t *root,
                              const struct mcp_shell_policy_field_descriptor *field,
                              char *error,
                              size_t error_size);
static bool validate_policy_field(const struct mcp_shell_policy_snapshot *snapshot,
                                  const struct mcp_shell_policy_field_descriptor *field);
static json_t *serialize_policy_field(const struct mcp_shell_policy_snapshot *snapshot,
                                      const struct mcp_shell_policy_field_descriptor *field);

#define BOOLEAN_FIELD(field_id, field_path, hard_value, field_capability, member)                 \
    {                                                                                             \
        field_id, field_path, NULL, MCP_SHELL_POLICY_TYPE_BOOLEAN, hard_value, 0u, 1u, NULL, 0u, \
            0u, true, field_capability, POLICY_OFFSET(member), parse_policy_field,                \
            validate_policy_field, serialize_policy_field                                         \
    }

#define NUMERIC_FIELD(field_id, field_path, bound_path, hard_minimum, hard_maximum,               \
                      field_capability, member)                                                    \
    {                                                                                              \
        field_id, field_path, bound_path, MCP_SHELL_POLICY_TYPE_UINT64, hard_maximum,              \
            hard_minimum, hard_maximum, NULL, 0u, 0u, true, field_capability,                      \
            POLICY_OFFSET(member), parse_policy_field, validate_policy_field,                     \
            serialize_policy_field                                                                 \
    }

#define STRING_FIELD(field_id, field_path, bound_path, hard_value, hard_minimum, hard_maximum,    \
                     field_capability, member)                                                     \
    {                                                                                              \
        field_id, field_path, bound_path, MCP_SHELL_POLICY_TYPE_STRING, 0u, 0u, 0u, hard_value,    \
            hard_minimum, hard_maximum, true, field_capability, POLICY_OFFSET(member),             \
            parse_policy_field, validate_policy_field, serialize_policy_field                      \
    }

static const struct mcp_shell_policy_field_descriptor policy_fields[] = {
    BOOLEAN_FIELD(MCP_SHELL_POLICY_FIELD_SHELL_ENABLED,
                  "defaults.shell_enabled",
                  true,
                  MCP_SHELL_POLICY_CAPABILITY_PORTABLE,
                  shell_enabled),
    NUMERIC_FIELD(MCP_SHELL_POLICY_FIELD_COMMAND_LENGTH,
                  "defaults.command_length",
                  "bounds.command_length",
                  UINT64_C(1),
                  UINT64_C(65536),
                  MCP_SHELL_POLICY_CAPABILITY_PORTABLE,
                  command_length),
    NUMERIC_FIELD(MCP_SHELL_POLICY_FIELD_TIMEOUT_MS,
                  "defaults.timeout_ms",
                  "bounds.timeout_ms",
                  UINT64_C(1),
                  UINT64_C(3600000),
                  MCP_SHELL_POLICY_CAPABILITY_PORTABLE,
                  timeout_ms),
    NUMERIC_FIELD(MCP_SHELL_POLICY_FIELD_OUTPUT_BYTES,
                  "defaults.output_bytes",
                  "bounds.output_bytes",
                  UINT64_C(0),
                  UINT64_C(1048576),
                  MCP_SHELL_POLICY_CAPABILITY_PORTABLE,
                  output_bytes),
    NUMERIC_FIELD(MCP_SHELL_POLICY_FIELD_READ_CHUNK_SIZE,
                  "defaults.once_read_stdout_err_chunk_size",
                  "bounds.once_read_stdout_err_chunk_size",
                  UINT64_C(64),
                  UINT64_C(65536),
                  MCP_SHELL_POLICY_CAPABILITY_PORTABLE,
                  once_read_stdout_err_chunk_size),
    BOOLEAN_FIELD(MCP_SHELL_POLICY_FIELD_CAPTURE_STDERR,
                  "defaults.capture_stderr",
                  true,
                  MCP_SHELL_POLICY_CAPABILITY_PORTABLE,
                  capture_stderr),
    BOOLEAN_FIELD(MCP_SHELL_POLICY_FIELD_MERGE_STDERR,
                  "defaults.merge_stderr_to_stdout",
                  false,
                  MCP_SHELL_POLICY_CAPABILITY_PORTABLE,
                  merge_stderr_to_stdout),
    {MCP_SHELL_POLICY_FIELD_EXECUTION_MODE,
     "defaults.execution.mode",
     "bounds.execution.mode",
     MCP_SHELL_POLICY_TYPE_MODE,
     MCP_SHELL_POLICY_MODE_SHELL,
     0u,
     MCP_SHELL_POLICY_MODE_SHELL_MASK | MCP_SHELL_POLICY_MODE_EXEC_MASK,
     NULL,
     0u,
     0u,
     true,
     MCP_SHELL_POLICY_CAPABILITY_PORTABLE,
     POLICY_OFFSET(execution.mode),
     parse_policy_field,
     validate_policy_field,
     serialize_policy_field},
    STRING_FIELD(MCP_SHELL_POLICY_FIELD_SHELL_PATH,
                 "defaults.execution.shell_path",
                 "bounds.execution.shell_path",
                 MCP_SHELL_POLICY_HARD_SHELL_PATH,
                 1u,
                 4096u,
                 MCP_SHELL_POLICY_CAPABILITY_PORTABLE,
                 execution.shell_path),
    STRING_FIELD(MCP_SHELL_POLICY_FIELD_SHELL_ARG,
                 "defaults.execution.shell_arg",
                 "bounds.execution.shell_arg",
                 MCP_SHELL_POLICY_HARD_SHELL_ARG,
                 0u,
                 65536u,
                 MCP_SHELL_POLICY_CAPABILITY_PORTABLE,
                 execution.shell_arg),
    STRING_FIELD(MCP_SHELL_POLICY_FIELD_WORKING_DIRECTORY,
                 "defaults.execution.working_directory",
                 "bounds.execution.working_directory",
                 MCP_SHELL_POLICY_HARD_WORKING_DIRECTORY,
                 1u,
                 4096u,
                 MCP_SHELL_POLICY_CAPABILITY_PORTABLE,
                 execution.working_directory),
    BOOLEAN_FIELD(MCP_SHELL_POLICY_FIELD_INHERIT_ENV,
                  "defaults.execution.inherit_env",
                  false,
                  MCP_SHELL_POLICY_CAPABILITY_PORTABLE,
                  execution.inherit_env),
    BOOLEAN_FIELD(MCP_SHELL_POLICY_FIELD_REQUEST_CWD_ALLOWED,
                  "defaults.execution.request_cwd_allowed",
                  true,
                  MCP_SHELL_POLICY_CAPABILITY_PORTABLE,
                  execution.request_cwd_allowed),
    BOOLEAN_FIELD(MCP_SHELL_POLICY_FIELD_REQUEST_ENV_ALLOWED,
                  "defaults.execution.request_env_allowed",
                  true,
                  MCP_SHELL_POLICY_CAPABILITY_PORTABLE,
                  execution.request_env_allowed),
    BOOLEAN_FIELD(MCP_SHELL_POLICY_FIELD_KILL_PROCESS_GROUP,
                  "defaults.execution.kill_process_group_on_timeout",
                  true,
                  MCP_SHELL_POLICY_CAPABILITY_PLATFORM_DEPENDENT,
                  execution.kill_process_group_on_timeout),
    STRING_FIELD(MCP_SHELL_POLICY_FIELD_RUN_AS_USER,
                 "defaults.execution.run_as_user",
                 "bounds.execution.run_as_user",
                 "",
                 0u,
                 MCP_SHELL_POLICY_IDENTITY_MAX_BYTES,
                 MCP_SHELL_POLICY_CAPABILITY_UNIX_ONLY,
                 execution.run_as_user),
    STRING_FIELD(MCP_SHELL_POLICY_FIELD_RUN_AS_GROUP,
                 "defaults.execution.run_as_group",
                 "bounds.execution.run_as_group",
                 "",
                 0u,
                 MCP_SHELL_POLICY_IDENTITY_MAX_BYTES,
                 MCP_SHELL_POLICY_CAPABILITY_UNIX_ONLY,
                 execution.run_as_group),
    {MCP_SHELL_POLICY_FIELD_ENV,
     "defaults.execution.env",
     "bounds.execution.allowed_env",
     MCP_SHELL_POLICY_TYPE_ENV,
     0u,
     0u,
     0u,
     NULL,
     0u,
     0u,
     true,
     MCP_SHELL_POLICY_CAPABILITY_PORTABLE,
     POLICY_OFFSET(execution.env_vars),
     parse_policy_field,
     validate_policy_field,
     serialize_policy_field},
    NUMERIC_FIELD(MCP_SHELL_POLICY_FIELD_CPU_SECONDS,
                  "defaults.limits.cpu_seconds",
                  "bounds.limits.cpu_seconds",
                  UINT64_C(0),
                  UINT64_C(3600),
                  MCP_SHELL_POLICY_CAPABILITY_UNIX_ONLY,
                  limits.cpu_seconds),
    NUMERIC_FIELD(MCP_SHELL_POLICY_FIELD_MEMORY_BYTES,
                  "defaults.limits.memory_bytes",
                  "bounds.limits.memory_bytes",
                  UINT64_C(0),
                  UINT64_C(34359738367),
                  MCP_SHELL_POLICY_CAPABILITY_PLATFORM_DEPENDENT,
                  limits.memory_bytes),
    NUMERIC_FIELD(MCP_SHELL_POLICY_FIELD_FILE_SIZE_BYTES,
                  "defaults.limits.file_size_bytes",
                  "bounds.limits.file_size_bytes",
                  UINT64_C(0),
                  UINT64_C(17179869184),
                  MCP_SHELL_POLICY_CAPABILITY_UNIX_ONLY,
                  limits.file_size_bytes),
    NUMERIC_FIELD(MCP_SHELL_POLICY_FIELD_OPEN_FILES,
                  "defaults.limits.open_files",
                  "bounds.limits.open_files",
                  UINT64_C(0),
                  UINT64_C(65536),
                  MCP_SHELL_POLICY_CAPABILITY_UNIX_ONLY,
                  limits.open_files),
    NUMERIC_FIELD(MCP_SHELL_POLICY_FIELD_PROCESSES,
                  "defaults.limits.processes",
                  "bounds.limits.processes",
                  UINT64_C(0),
                  UINT64_C(2048),
                  MCP_SHELL_POLICY_CAPABILITY_UNIX_ONLY,
                  limits.processes),
    BOOLEAN_FIELD(MCP_SHELL_POLICY_FIELD_REQUIRE_NON_ROOT,
                  "defaults.isolation.require_non_root",
                  false,
                  MCP_SHELL_POLICY_CAPABILITY_PLATFORM_DEPENDENT,
                  require_non_root),
};

static char *policy_strdup(const char *value)
{
    size_t length;
    char *copy;

    if (!value)
        return NULL;
    length = strlen(value);
    copy = malloc(length + 1u);
    if (!copy)
        return NULL;
    memcpy(copy, value, length + 1u);
    return copy;
}

static void policy_error(char *error, size_t error_size, const char *format, ...)
{
    va_list arguments;

    if (!error || error_size == 0u)
        return;
    va_start(arguments, format);
    vsnprintf(error, error_size, format, arguments);
    va_end(arguments);
    error[error_size - 1u] = '\0';
}

static void *policy_field_value(struct mcp_shell_policy_snapshot *snapshot,
                                const struct mcp_shell_policy_field_descriptor *field)
{
    return (unsigned char *)&snapshot->defaults + field->value_offset;
}

static const void *policy_field_const_value(
    const struct mcp_shell_policy_snapshot *snapshot,
    const struct mcp_shell_policy_field_descriptor *field)
{
    return (const unsigned char *)&snapshot->defaults + field->value_offset;
}

static json_t *policy_json_path_get(const json_t *root, const char *path)
{
    const json_t *current = root;
    const char *position = path;

    while (position && position[0] != '\0') {
        const char *separator = strchr(position, '.');
        size_t length = separator ? (size_t)(separator - position) : strlen(position);
        char key[96];

        if (!json_is_object(current) || length == 0u || length >= sizeof(key))
            return NULL;
        memcpy(key, position, length);
        key[length] = '\0';
        current = json_object_get(current, key);
        if (!current)
            return NULL;
        position = separator ? separator + 1 : NULL;
    }
    return (json_t *)current;
}

static bool key_is_listed(const char *key, const char *const *keys, size_t count)
{
    size_t index;

    for (index = 0; index < count; index++) {
        if (strcmp(key, keys[index]) == 0)
            return true;
    }
    return false;
}

static int validate_object_shape(const json_t *object,
                                 const char *path,
                                 const char *const *allowed,
                                 size_t allowed_count,
                                 const char *const *required,
                                 size_t required_count,
                                 char *error,
                                 size_t error_size)
{
    const char *key;
    json_t *value;
    size_t index;

    if (!json_is_object(object)) {
        policy_error(error, error_size, "%s must be an object", path);
        return -1;
    }

    json_object_foreach((json_t *)object, key, value) {
        (void)value;
        if (!key_is_listed(key, allowed, allowed_count)) {
            policy_error(error, error_size, "%s contains unknown field %s", path, key);
            return -1;
        }
    }
    for (index = 0; index < required_count; index++) {
        if (!json_object_get(object, required[index])) {
            policy_error(error, error_size, "%s.%s is required", path, required[index]);
            return -1;
        }
    }
    return 0;
}

static int validate_policy_structure(const json_t *root, char *error, size_t error_size)
{
    static const char *const root_keys[] = {"version", "control", "defaults", "bounds"};
    static const char *const control_keys[] = {"token"};
    static const char *const defaults_keys[] = {"shell_enabled",
                                                 "command_length",
                                                 "timeout_ms",
                                                 "output_bytes",
                                                 "once_read_stdout_err_chunk_size",
                                                 "capture_stderr",
                                                 "merge_stderr_to_stdout",
                                                 "execution",
                                                 "limits",
                                                 "isolation"};
    static const char *const execution_keys[] = {"mode",
                                                  "shell_path",
                                                  "shell_arg",
                                                  "working_directory",
                                                  "inherit_env",
                                                  "request_cwd_allowed",
                                                  "request_env_allowed",
                                                  "kill_process_group_on_timeout",
                                                  "run_as_user",
                                                  "run_as_group",
                                                  "env"};
    static const char *const limit_keys[] = {
        "cpu_seconds", "memory_bytes", "file_size_bytes", "open_files", "processes"};
    static const char *const isolation_keys[] = {"require_non_root"};
    static const char *const bounds_keys[] = {"command_length",
                                              "timeout_ms",
                                              "output_bytes",
                                              "once_read_stdout_err_chunk_size",
                                              "execution",
                                              "limits"};
    static const char *const execution_bound_keys[] = {"mode",
                                                        "shell_path",
                                                        "shell_arg",
                                                        "working_directory",
                                                        "allowed_env",
                                                        "run_as_user",
                                                        "run_as_group"};
    json_t *control;
    json_t *defaults;
    json_t *execution;
    json_t *limits;
    json_t *isolation;
    json_t *bounds;

    if (validate_object_shape(root,
                              "config",
                              root_keys,
                              sizeof(root_keys) / sizeof(root_keys[0]),
                              root_keys,
                              sizeof(root_keys) / sizeof(root_keys[0]),
                              error,
                              error_size) != 0)
        return -1;

    control = json_object_get(root, "control");
    if (validate_object_shape(control,
                              "control",
                              control_keys,
                              1u,
                              control_keys,
                              1u,
                              error,
                              error_size) != 0)
        return -1;

    defaults = json_object_get(root, "defaults");
    if (validate_object_shape(defaults,
                              "defaults",
                              defaults_keys,
                              sizeof(defaults_keys) / sizeof(defaults_keys[0]),
                              defaults_keys,
                              sizeof(defaults_keys) / sizeof(defaults_keys[0]),
                              error,
                              error_size) != 0)
        return -1;
    execution = json_object_get(defaults, "execution");
    if (validate_object_shape(execution,
                              "defaults.execution",
                              execution_keys,
                              sizeof(execution_keys) / sizeof(execution_keys[0]),
                              execution_keys,
                              sizeof(execution_keys) / sizeof(execution_keys[0]),
                              error,
                              error_size) != 0)
        return -1;
    limits = json_object_get(defaults, "limits");
    if (validate_object_shape(limits,
                              "defaults.limits",
                              limit_keys,
                              sizeof(limit_keys) / sizeof(limit_keys[0]),
                              limit_keys,
                              sizeof(limit_keys) / sizeof(limit_keys[0]),
                              error,
                              error_size) != 0)
        return -1;
    isolation = json_object_get(defaults, "isolation");
    if (validate_object_shape(isolation,
                              "defaults.isolation",
                              isolation_keys,
                              1u,
                              isolation_keys,
                              1u,
                              error,
                              error_size) != 0)
        return -1;

    bounds = json_object_get(root, "bounds");
    if (validate_object_shape(bounds,
                              "bounds",
                              bounds_keys,
                              sizeof(bounds_keys) / sizeof(bounds_keys[0]),
                              bounds_keys,
                              sizeof(bounds_keys) / sizeof(bounds_keys[0]),
                              error,
                              error_size) != 0)
        return -1;
    execution = json_object_get(bounds, "execution");
    if (validate_object_shape(execution,
                              "bounds.execution",
                              execution_bound_keys,
                              sizeof(execution_bound_keys) / sizeof(execution_bound_keys[0]),
                              execution_bound_keys,
                              sizeof(execution_bound_keys) / sizeof(execution_bound_keys[0]),
                              error,
                              error_size) != 0)
        return -1;
    limits = json_object_get(bounds, "limits");
    return validate_object_shape(limits,
                                 "bounds.limits",
                                 limit_keys,
                                 sizeof(limit_keys) / sizeof(limit_keys[0]),
                                 limit_keys,
                                 sizeof(limit_keys) / sizeof(limit_keys[0]),
                                 error,
                                 error_size);
}

static void policy_env_destroy(struct mcp_shell_policy_env_var *vars, size_t count)
{
    size_t index;

    for (index = 0; index < count; index++) {
        free(vars[index].name);
        free(vars[index].value);
    }
    free(vars);
}

static int policy_env_append(struct mcp_shell_policy_env_var **vars,
                             size_t *count,
                             const char *name,
                             const char *value)
{
    struct mcp_shell_policy_env_var *next;

    next = realloc(*vars, (*count + 1u) * sizeof(**vars));
    if (!next)
        return -1;
    *vars = next;
    next[*count].name = policy_strdup(name);
    next[*count].value = policy_strdup(value);
    if (!next[*count].name || !next[*count].value) {
        free(next[*count].name);
        free(next[*count].value);
        return -1;
    }
    (*count)++;
    return 0;
}

static bool policy_env_name_equals(const char *left, const char *right)
{
#ifdef _WIN32
    return _stricmp(left, right) == 0;
#else
    return strcmp(left, right) == 0;
#endif
}

static int policy_env_set(struct mcp_shell_policy_env_var **vars,
                          size_t *count,
                          const char *name,
                          const char *value)
{
    size_t index;

    for (index = 0; index < *count; index++) {
        if (policy_env_name_equals((*vars)[index].name, name)) {
            char *copy = policy_strdup(value);

            if (!copy)
                return -1;
            free((*vars)[index].value);
            (*vars)[index].value = copy;
            return 0;
        }
    }
    return policy_env_append(vars, count, name, value);
}

#ifdef _WIN32
static char *policy_env_utf8_from_wide(const WCHAR *value, int length)
{
    int utf8_length;
    char *utf8;

    utf8_length = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value, length, NULL, 0, NULL, NULL);
    if (utf8_length <= 0)
        return NULL;
    utf8 = malloc((size_t)utf8_length + 1u);
    if (!utf8)
        return NULL;
    if (WideCharToMultiByte(CP_UTF8,
                            WC_ERR_INVALID_CHARS,
                            value,
                            length,
                            utf8,
                            utf8_length,
                            NULL,
                            NULL) != utf8_length) {
        free(utf8);
        return NULL;
    }
    utf8[utf8_length] = '\0';
    return utf8;
}
#endif

static int policy_capture_start_environment(struct mcp_shell_policy_snapshot *snapshot)
{
#ifdef _WIN32
    LPWCH environment = GetEnvironmentStringsW();
    const WCHAR *entry;

    if (!environment)
        return -1;
    for (entry = environment; *entry; entry += wcslen(entry) + 1u) {
        char *utf8;
        char *separator;

        if (*entry == L'=')
            continue;
        utf8 = policy_env_utf8_from_wide(entry, (int)wcslen(entry));
        if (!utf8) {
            FreeEnvironmentStringsW(environment);
            return -1;
        }
        separator = strchr(utf8, '=');
        if (separator && separator != utf8) {
            *separator = '\0';
            if (policy_env_set(&snapshot->startup_env_vars,
                               &snapshot->startup_env_var_count,
                               utf8,
                               separator + 1u) != 0) {
                free(utf8);
                FreeEnvironmentStringsW(environment);
                return -1;
            }
        }
        free(utf8);
    }
    FreeEnvironmentStringsW(environment);
#else
    extern char **environ;
    char **entry;

    for (entry = environ; entry && *entry; entry++) {
        const char *separator = strchr(*entry, '=');
        size_t name_length;
        char *name;

        if (!separator || separator == *entry)
            continue;
        name_length = (size_t)(separator - *entry);
        name = malloc(name_length + 1u);
        if (!name)
            return -1;
        memcpy(name, *entry, name_length);
        name[name_length] = '\0';
        if (policy_env_set(&snapshot->startup_env_vars,
                           &snapshot->startup_env_var_count,
                           name,
                           separator + 1u) != 0) {
            free(name);
            return -1;
        }
        free(name);
    }
#endif
    return 0;
}

static int policy_set_hard_environment(struct mcp_shell_policy_snapshot *snapshot)
{
#ifdef _WIN32
    if (policy_env_append(&snapshot->defaults.execution.env_vars,
                          &snapshot->defaults.execution.env_var_count,
                          "PATH",
                          "%SystemRoot%\\System32;%SystemRoot%") != 0 ||
        policy_env_append(&snapshot->defaults.execution.env_vars,
                          &snapshot->defaults.execution.env_var_count,
                          "SystemRoot",
                          "C:\\Windows") != 0)
        return -1;
#else
    if (policy_env_append(&snapshot->defaults.execution.env_vars,
                          &snapshot->defaults.execution.env_var_count,
                          "PATH",
                          "/usr/bin:/bin") != 0 ||
        policy_env_append(&snapshot->defaults.execution.env_vars,
                          &snapshot->defaults.execution.env_var_count,
                          "HOME",
                          "/tmp/mcp-shell") != 0 ||
        policy_env_append(&snapshot->defaults.execution.env_vars,
                          &snapshot->defaults.execution.env_var_count,
                          "LANG",
                          "C") != 0)
        return -1;
#endif
    return 0;
}

static int policy_initialize_hard_field(
    struct mcp_shell_policy_snapshot *snapshot,
    const struct mcp_shell_policy_field_descriptor *field)
{
    void *value = policy_field_value(snapshot, field);

    switch (field->type) {
    case MCP_SHELL_POLICY_TYPE_BOOLEAN:
        *(bool *)value = field->hard_default != 0u;
        break;
    case MCP_SHELL_POLICY_TYPE_UINT64: {
        struct mcp_shell_policy_numeric *number = value;
        number->value = field->hard_default;
        number->min = field->hard_min;
        number->max = field->hard_max;
        number->source = MCP_SHELL_POLICY_SOURCE_HARD;
        break;
    }
    case MCP_SHELL_POLICY_TYPE_STRING: {
        struct mcp_shell_policy_string *string = value;
        string->value = policy_strdup(field->hard_default_string);
        if (!string->value)
            return -1;
        string->min_bytes = field->hard_min_bytes;
        string->max_bytes = field->hard_max_bytes;
        string->source = MCP_SHELL_POLICY_SOURCE_HARD;
        break;
    }
    case MCP_SHELL_POLICY_TYPE_MODE: {
        struct mcp_shell_policy_mode_value *mode = value;
        mode->value = (enum mcp_shell_policy_mode)field->hard_default;
        mode->allowed_mask = (unsigned int)field->hard_max;
        mode->source = MCP_SHELL_POLICY_SOURCE_HARD;
        break;
    }
    case MCP_SHELL_POLICY_TYPE_ENV:
        break;
    }
    return 0;
}

const struct mcp_shell_policy_field_descriptor *mcp_shell_policy_field_directory(size_t *count)
{
    if (count)
        *count = sizeof(policy_fields) / sizeof(policy_fields[0]);
    return policy_fields;
}

int mcp_shell_policy_snapshot_create_hard(struct mcp_shell_policy_snapshot **out,
                                          const char *config_path,
                                          bool control_enabled)
{
    struct mcp_shell_policy_snapshot *snapshot;
    size_t index;

    if (!out)
        return -1;
    *out = NULL;
    snapshot = calloc(1, sizeof(*snapshot));
    if (!snapshot)
        return -1;

    snapshot->control_enabled = control_enabled;
    snapshot->version = 2;
    snapshot->config_path = policy_strdup(config_path ? config_path : "(hard_profile)");
    snapshot->environment_max_items = MCP_SHELL_POLICY_ENV_HARD_MAX_ITEMS;
    snapshot->environment_item_max_bytes = MCP_SHELL_POLICY_ENV_HARD_ITEM_MAX_BYTES;
    snapshot->environment_source = MCP_SHELL_POLICY_SOURCE_HARD;
    if (!snapshot->config_path)
        goto fail;

    for (index = 0; index < sizeof(policy_fields) / sizeof(policy_fields[0]); index++) {
        if (policy_initialize_hard_field(snapshot, &policy_fields[index]) != 0)
            goto fail;
    }
    if (policy_set_hard_environment(snapshot) != 0)
        goto fail;
    if (policy_capture_start_environment(snapshot) != 0)
        goto fail;

    *out = snapshot;
    return 0;

fail:
    mcp_shell_policy_snapshot_destroy(snapshot);
    return -1;
}

static int policy_control_gate(bool *enabled)
{
    const char *value = getenv("MCP_ENABLE_SANDBOX_CTL");

    if (!enabled)
        return -1;
    if (!value || value[0] == '\0' || strcmp(value, "0") == 0) {
        *enabled = false;
        return 0;
    }
    if (strcmp(value, "1") == 0) {
        *enabled = true;
        return 0;
    }
    fputs("MCP_ENABLE_SANDBOX_CTL must be unset, empty, 0, or 1\n", stderr);
    return -1;
}

static int policy_parse_loaded_root(struct mcp_shell_policy_snapshot **out,
                                    json_t *root,
                                    const char *path,
                                    bool control_enabled,
                                    bool failure_is_fatal)
{
    char error[256];

    if (mcp_shell_policy_snapshot_parse_json(
            out, root, path, control_enabled, error, sizeof(error)) == 0)
        return 0;
    if (failure_is_fatal) {
        fprintf(stderr, "Invalid shell policy config %s: %s\n", path, error);
        return -1;
    }
    fprintf(stderr,
            "Ignoring invalid automatic shell policy config %s: %s; using hard profile\n",
            path,
            error);
    return mcp_shell_policy_snapshot_create_hard(out, path, false);
}

int mcp_shell_policy_snapshot_create_from_environment(struct mcp_shell_policy_snapshot **out)
{
    const char *explicit_path;
    const char *path = NULL;
    const char *candidates[] = {
        MCP_SHELL_EXEC_DEFAULT_CONFIG,
        MCP_SHELL_EXEC_INSTALLED_CONFIG,
    };
    bool control_enabled;
    bool explicit_config;
    size_t index;
    json_t *root = NULL;
    json_error_t json_error;
    int result;

    if (!out)
        return -1;
    *out = NULL;
    if (policy_control_gate(&control_enabled) != 0)
        return -1;

    explicit_path = getenv("MCP_SHELL_EXEC_CONFIG");
    explicit_config = explicit_path && explicit_path[0] != '\0';
    if (explicit_config) {
        path = explicit_path;
        root = json_load_file(path, JSON_REJECT_DUPLICATES, &json_error);
        if (!root) {
            fprintf(stderr,
                    "Failed to load explicit shell policy config %s%s\n",
                    path,
                    json_error_code(&json_error) == json_error_cannot_open_file
                        ? ": file unavailable"
                        : ": invalid JSON");
            return -1;
        }
    } else {
        for (index = 0; index < sizeof(candidates) / sizeof(candidates[0]); index++) {
            if (candidates[index][0] == '\0')
                continue;
            root = json_load_file(candidates[index], JSON_REJECT_DUPLICATES, &json_error);
            if (root) {
                path = candidates[index];
                break;
            }
            if (json_error_code(&json_error) != json_error_cannot_open_file) {
                path = candidates[index];
                if (control_enabled) {
                    fprintf(stderr, "Invalid automatic shell policy config %s\n", path);
                    return -1;
                }
                fprintf(stderr,
                        "Ignoring invalid automatic shell policy config %s; using hard profile\n",
                        path);
                return mcp_shell_policy_snapshot_create_hard(out, path, false);
            }
        }
    }

    if (!root) {
        if (control_enabled) {
            fputs("MCP_ENABLE_SANDBOX_CTL=1 requires a valid shell policy config\n", stderr);
            return -1;
        }
        return mcp_shell_policy_snapshot_create_hard(out, "(hard_profile)", false);
    }

    result = policy_parse_loaded_root(
        out, root, path, control_enabled, explicit_config || control_enabled);
    json_decref(root);
    return result;
}

static void policy_set_diagnostic(struct mcp_shell_policy_snapshot *snapshot,
                                  const struct mcp_shell_policy_field_descriptor *field,
                                  const char *reason)
{
    snapshot->diagnostics[field->id] = reason;
}

static int parse_boolean_field(struct mcp_shell_policy_snapshot *snapshot,
                               const json_t *root,
                               const struct mcp_shell_policy_field_descriptor *field,
                               char *error,
                               size_t error_size)
{
    json_t *value = policy_json_path_get(root, field->path);

    if (!json_is_boolean(value)) {
        policy_error(error, error_size, "%s must be a boolean", field->path);
        return -1;
    }
    *(bool *)policy_field_value(snapshot, field) = json_is_true(value);
    return 0;
}

static int validate_bound_object(const json_t *bounds,
                                 const char *path,
                                 bool max_optional,
                                 char *error,
                                 size_t error_size)
{
    static const char *const keys[] = {"min", "max"};
    static const char *const required[] = {"min", "max"};

    return validate_object_shape(bounds,
                                 path,
                                 keys,
                                 2u,
                                 required,
                                 max_optional ? 1u : 2u,
                                 error,
                                 error_size);
}

static int parse_numeric_field(struct mcp_shell_policy_snapshot *snapshot,
                               const json_t *root,
                               const struct mcp_shell_policy_field_descriptor *field,
                               char *error,
                               size_t error_size)
{
    struct mcp_shell_policy_numeric *number = policy_field_value(snapshot, field);
    json_t *default_json = policy_json_path_get(root, field->path);
    json_t *bounds = policy_json_path_get(root, field->bounds_path);
    json_t *min_json;
    json_t *max_json;
    json_int_t default_signed;
    json_int_t min_signed;
    json_int_t max_signed;
    uint64_t default_value;
    uint64_t minimum;
    uint64_t maximum;
    const char *fallback = NULL;

    if (!json_is_integer(default_json)) {
        policy_error(error, error_size, "%s must be an integer", field->path);
        return -1;
    }
    if (validate_bound_object(bounds, field->bounds_path, false, error, error_size) != 0)
        return -1;
    min_json = json_object_get(bounds, "min");
    max_json = json_object_get(bounds, "max");
    if (!json_is_integer(min_json) || !json_is_integer(max_json)) {
        policy_error(error, error_size, "%s min and max must be integers", field->bounds_path);
        return -1;
    }

    default_signed = json_integer_value(default_json);
    min_signed = json_integer_value(min_json);
    max_signed = json_integer_value(max_json);
    if (min_signed < 0 || (uint64_t)min_signed < field->hard_min)
        fallback = "bound_below_hard_min";
    else if (max_signed < 0)
        fallback = "invalid_bound_order";
    else if ((uint64_t)max_signed > field->hard_max)
        fallback = "bound_above_hard_max";
    else if (min_signed > max_signed)
        fallback = "invalid_bound_order";

    minimum = fallback ? field->hard_min : (uint64_t)min_signed;
    maximum = fallback ? field->hard_max : (uint64_t)max_signed;
    if (!fallback &&
        (default_signed < 0 || (uint64_t)default_signed < minimum ||
         (uint64_t)default_signed > maximum))
        fallback = "default_out_of_bounds";

    if (fallback) {
        number->value = field->hard_default;
        number->min = field->hard_min;
        number->max = field->hard_max;
        number->source = MCP_SHELL_POLICY_SOURCE_HARD_FALLBACK;
        policy_set_diagnostic(snapshot, field, fallback);
        return 0;
    }

    default_value = (uint64_t)default_signed;
    number->value = default_value;
    number->min = minimum;
    number->max = maximum;
    number->source = MCP_SHELL_POLICY_SOURCE_JSON;
    return 0;
}

static bool json_string_is_plain(const json_t *value, size_t *length)
{
    const char *text;
    size_t bytes;

    if (!json_is_string(value))
        return false;
    text = json_string_value(value);
    bytes = json_string_length(value);
    if (!text || memchr(text, '\0', bytes) != NULL)
        return false;
    if (length)
        *length = bytes;
    return true;
}

static int parse_string_field(struct mcp_shell_policy_snapshot *snapshot,
                              const json_t *root,
                              const struct mcp_shell_policy_field_descriptor *field,
                              char *error,
                              size_t error_size)
{
    struct mcp_shell_policy_string *string = policy_field_value(snapshot, field);
    json_t *default_json = policy_json_path_get(root, field->path);
    json_t *bounds = policy_json_path_get(root, field->bounds_path);
    json_t *min_json;
    json_t *max_json;
    json_int_t min_signed;
    json_int_t max_signed;
    size_t minimum;
    size_t maximum;
    size_t default_length;
    bool max_optional = field->id == MCP_SHELL_POLICY_FIELD_RUN_AS_USER ||
                        field->id == MCP_SHELL_POLICY_FIELD_RUN_AS_GROUP;
    static const char *const bound_keys[] = {"min_bytes", "max_bytes"};
    static const char *const required_keys[] = {"min_bytes", "max_bytes"};
    const char *fallback = NULL;
    char *copy;

    if (!json_string_is_plain(default_json, &default_length)) {
        policy_error(error, error_size, "%s must be a string without embedded NUL", field->path);
        return -1;
    }
    if (validate_object_shape(bounds,
                              field->bounds_path,
                              bound_keys,
                              2u,
                              required_keys,
                              max_optional ? 1u : 2u,
                              error,
                              error_size) != 0)
        return -1;
    min_json = json_object_get(bounds, "min_bytes");
    max_json = json_object_get(bounds, "max_bytes");
    if (!json_is_integer(min_json) || (max_json && !json_is_integer(max_json))) {
        policy_error(error, error_size, "%s byte bounds must be integers", field->bounds_path);
        return -1;
    }

    min_signed = json_integer_value(min_json);
    max_signed = max_json ? json_integer_value(max_json) : (json_int_t)field->hard_max_bytes;
    if (min_signed < 0 || (uint64_t)min_signed < field->hard_min_bytes)
        fallback = "bound_below_hard_min";
    else if (max_signed < 0)
        fallback = "invalid_bound_order";
    else if ((uint64_t)max_signed > field->hard_max_bytes)
        fallback = "bound_above_hard_max";
    else if (min_signed > max_signed)
        fallback = "invalid_bound_order";

    minimum = fallback ? field->hard_min_bytes : (size_t)min_signed;
    maximum = fallback ? field->hard_max_bytes : (size_t)max_signed;
    if (!fallback && (default_length < minimum || default_length > maximum))
        fallback = "default_out_of_bounds";
    if (fallback) {
        string->source = MCP_SHELL_POLICY_SOURCE_HARD_FALLBACK;
        policy_set_diagnostic(snapshot, field, fallback);
        return 0;
    }

    copy = malloc(default_length + 1u);
    if (!copy) {
        policy_error(error, error_size, "out of memory while parsing %s", field->path);
        return -1;
    }
    memcpy(copy, json_string_value(default_json), default_length);
    copy[default_length] = '\0';
    free(string->value);
    string->value = copy;
    string->min_bytes = minimum;
    string->max_bytes = maximum;
    string->source = MCP_SHELL_POLICY_SOURCE_JSON;
    return 0;
}

static int policy_mode_from_json(const json_t *value, enum mcp_shell_policy_mode *mode)
{
    size_t length;
    const char *text;

    if (!json_string_is_plain(value, &length))
        return -1;
    text = json_string_value(value);
    if (length == 5u && memcmp(text, "shell", 5u) == 0) {
        *mode = MCP_SHELL_POLICY_MODE_SHELL;
        return 0;
    }
    if (length == 4u && memcmp(text, "exec", 4u) == 0) {
        *mode = MCP_SHELL_POLICY_MODE_EXEC;
        return 0;
    }
    return -1;
}

static int parse_mode_field(struct mcp_shell_policy_snapshot *snapshot,
                            const json_t *root,
                            const struct mcp_shell_policy_field_descriptor *field,
                            char *error,
                            size_t error_size)
{
    static const char *const allowed_keys[] = {"allowed"};
    struct mcp_shell_policy_mode_value *mode = policy_field_value(snapshot, field);
    json_t *default_json = policy_json_path_get(root, field->path);
    json_t *bounds = policy_json_path_get(root, field->bounds_path);
    json_t *allowed;
    json_t *item;
    enum mcp_shell_policy_mode default_mode;
    enum mcp_shell_policy_mode allowed_mode;
    size_t index;
    unsigned int mask = 0u;
    const char *fallback = NULL;

    if (policy_mode_from_json(default_json, &default_mode) != 0) {
        policy_error(error, error_size, "%s must be shell or exec", field->path);
        return -1;
    }
    if (validate_object_shape(bounds,
                              field->bounds_path,
                              allowed_keys,
                              1u,
                              allowed_keys,
                              1u,
                              error,
                              error_size) != 0)
        return -1;
    allowed = json_object_get(bounds, "allowed");
    if (!json_is_array(allowed)) {
        policy_error(error, error_size, "%s.allowed must be an array", field->bounds_path);
        return -1;
    }
    json_array_foreach(allowed, index, item) {
        unsigned int bit;
        if (policy_mode_from_json(item, &allowed_mode) != 0) {
            fallback = "invalid_bound_value";
            break;
        }
        bit = 1u << allowed_mode;
        if ((mask & bit) != 0u) {
            fallback = "invalid_bound_value";
            break;
        }
        mask |= bit;
    }
    if (mask == 0u)
        fallback = "invalid_bound_value";
    if (!fallback && (mask & (1u << default_mode)) == 0u)
        fallback = "default_out_of_bounds";
    if (fallback) {
        mode->source = MCP_SHELL_POLICY_SOURCE_HARD_FALLBACK;
        policy_set_diagnostic(snapshot, field, fallback);
        return 0;
    }
    mode->value = default_mode;
    mode->allowed_mask = mask;
    mode->source = MCP_SHELL_POLICY_SOURCE_JSON;
    return 0;
}

static bool policy_env_name_valid(const char *name)
{
    return name && name[0] != '\0' && strchr(name, '=') == NULL;
}

static int parse_environment_field(struct mcp_shell_policy_snapshot *snapshot,
                                   const json_t *root,
                                   const struct mcp_shell_policy_field_descriptor *field,
                                   char *error,
                                   size_t error_size)
{
    static const char *const bound_keys[] = {"max_items", "item_max_bytes"};
    json_t *environment = policy_json_path_get(root, field->path);
    json_t *bounds = policy_json_path_get(root, field->bounds_path);
    json_t *max_items_json;
    json_t *max_bytes_json;
    json_int_t max_items_signed;
    json_int_t max_bytes_signed;
    size_t max_items;
    size_t max_bytes;
    const char *fallback = NULL;
    const char *name;
    json_t *value;
    struct mcp_shell_policy_env_var *vars = NULL;
    size_t count = 0u;

    if (!json_is_object(environment)) {
        policy_error(error, error_size, "%s must be an object", field->path);
        return -1;
    }
    if (validate_object_shape(bounds,
                              field->bounds_path,
                              bound_keys,
                              2u,
                              bound_keys,
                              2u,
                              error,
                              error_size) != 0)
        return -1;
    max_items_json = json_object_get(bounds, "max_items");
    max_bytes_json = json_object_get(bounds, "item_max_bytes");
    if (!json_is_integer(max_items_json) || !json_is_integer(max_bytes_json)) {
        policy_error(error, error_size, "%s bounds must be integers", field->bounds_path);
        return -1;
    }
    max_items_signed = json_integer_value(max_items_json);
    max_bytes_signed = json_integer_value(max_bytes_json);
    if (max_items_signed < 0 ||
        (uint64_t)max_items_signed > MCP_SHELL_POLICY_ENV_HARD_MAX_ITEMS ||
        max_bytes_signed < 0 ||
        (uint64_t)max_bytes_signed > MCP_SHELL_POLICY_ENV_HARD_ITEM_MAX_BYTES)
        fallback = "bound_above_hard_max";
    max_items = fallback ? MCP_SHELL_POLICY_ENV_HARD_MAX_ITEMS : (size_t)max_items_signed;
    max_bytes = fallback ? MCP_SHELL_POLICY_ENV_HARD_ITEM_MAX_BYTES : (size_t)max_bytes_signed;

    json_object_foreach(environment, name, value) {
        size_t value_length;
        size_t item_length;

        if (!policy_env_name_valid(name) || !json_string_is_plain(value, &value_length)) {
            policy_error(error, error_size, "%s entries must be valid string assignments", field->path);
            policy_env_destroy(vars, count);
            return -1;
        }
        item_length = strlen(name) + 1u + value_length;
        if (item_length > max_bytes)
            fallback = "default_out_of_bounds";
        if (policy_env_append(&vars, &count, name, json_string_value(value)) != 0) {
            policy_error(error, error_size, "out of memory while parsing %s", field->path);
            policy_env_destroy(vars, count);
            return -1;
        }
    }
    if (count > max_items)
        fallback = "default_out_of_bounds";
    if (fallback) {
        policy_env_destroy(vars, count);
        snapshot->environment_source = MCP_SHELL_POLICY_SOURCE_HARD_FALLBACK;
        policy_set_diagnostic(snapshot, field, fallback);
        return 0;
    }

    policy_env_destroy(snapshot->defaults.execution.env_vars,
                       snapshot->defaults.execution.env_var_count);
    snapshot->defaults.execution.env_vars = vars;
    snapshot->defaults.execution.env_var_count = count;
    snapshot->environment_max_items = max_items;
    snapshot->environment_item_max_bytes = max_bytes;
    snapshot->environment_source = MCP_SHELL_POLICY_SOURCE_JSON;
    return 0;
}

static int parse_policy_field(struct mcp_shell_policy_snapshot *snapshot,
                              const json_t *root,
                              const struct mcp_shell_policy_field_descriptor *field,
                              char *error,
                              size_t error_size)
{
    switch (field->type) {
    case MCP_SHELL_POLICY_TYPE_BOOLEAN:
        return parse_boolean_field(snapshot, root, field, error, error_size);
    case MCP_SHELL_POLICY_TYPE_UINT64:
        return parse_numeric_field(snapshot, root, field, error, error_size);
    case MCP_SHELL_POLICY_TYPE_STRING:
        return parse_string_field(snapshot, root, field, error, error_size);
    case MCP_SHELL_POLICY_TYPE_MODE:
        return parse_mode_field(snapshot, root, field, error, error_size);
    case MCP_SHELL_POLICY_TYPE_ENV:
        return parse_environment_field(snapshot, root, field, error, error_size);
    }
    policy_error(error, error_size, "unsupported policy field %s", field->path);
    return -1;
}

static bool validate_policy_field(const struct mcp_shell_policy_snapshot *snapshot,
                                  const struct mcp_shell_policy_field_descriptor *field)
{
    const void *value = policy_field_const_value(snapshot, field);

    switch (field->type) {
    case MCP_SHELL_POLICY_TYPE_BOOLEAN:
        return true;
    case MCP_SHELL_POLICY_TYPE_UINT64: {
        const struct mcp_shell_policy_numeric *number = value;
        return number->min <= number->value && number->value <= number->max;
    }
    case MCP_SHELL_POLICY_TYPE_STRING: {
        const struct mcp_shell_policy_string *string = value;
        size_t length = string->value ? strlen(string->value) : 0u;
        return string->value && string->min_bytes <= length && length <= string->max_bytes;
    }
    case MCP_SHELL_POLICY_TYPE_MODE: {
        const struct mcp_shell_policy_mode_value *mode = value;
        return (mode->allowed_mask & (1u << mode->value)) != 0u;
    }
    case MCP_SHELL_POLICY_TYPE_ENV: {
        size_t index;
        if (snapshot->defaults.execution.env_var_count > snapshot->environment_max_items)
            return false;
        for (index = 0; index < snapshot->defaults.execution.env_var_count; index++) {
            const struct mcp_shell_policy_env_var *item =
                &snapshot->defaults.execution.env_vars[index];
            if (!policy_env_name_valid(item->name) ||
                strlen(item->name) + 1u + strlen(item->value) >
                    snapshot->environment_item_max_bytes)
                return false;
        }
        return true;
    }
    }
    return false;
}

static json_t *serialize_policy_field(const struct mcp_shell_policy_snapshot *snapshot,
                                      const struct mcp_shell_policy_field_descriptor *field)
{
    const void *value = policy_field_const_value(snapshot, field);

    switch (field->type) {
    case MCP_SHELL_POLICY_TYPE_BOOLEAN:
        return json_boolean(*(const bool *)value);
    case MCP_SHELL_POLICY_TYPE_UINT64:
        return json_integer((json_int_t)((const struct mcp_shell_policy_numeric *)value)->value);
    case MCP_SHELL_POLICY_TYPE_STRING:
        return json_string(((const struct mcp_shell_policy_string *)value)->value);
    case MCP_SHELL_POLICY_TYPE_MODE:
        return json_string(((const struct mcp_shell_policy_mode_value *)value)->value ==
                                   MCP_SHELL_POLICY_MODE_SHELL
                               ? "shell"
                               : "exec");
    case MCP_SHELL_POLICY_TYPE_ENV:
        return json_pack("{s:I,s:b}",
                         "items",
                         (json_int_t)snapshot->defaults.execution.env_var_count,
                         "valid",
                         validate_policy_field(snapshot, field));
    }
    return NULL;
}

int mcp_shell_policy_snapshot_parse_json(struct mcp_shell_policy_snapshot **out,
                                         const json_t *root,
                                         const char *config_path,
                                         bool control_enabled,
                                         char *error,
                                         size_t error_size)
{
    struct mcp_shell_policy_snapshot *snapshot = NULL;
    json_t *version;
    json_t *token;
    const char *token_value;
    size_t token_length;
    size_t index;

    if (!out)
        return -1;
    *out = NULL;
    if (error && error_size > 0u)
        error[0] = '\0';
    if (validate_policy_structure(root, error, error_size) != 0)
        return -1;

    version = json_object_get(root, "version");
    if (!json_is_integer(version) || json_integer_value(version) != 2) {
        policy_error(error, error_size, "version must be integer 2");
        return -1;
    }
    token = json_object_get(json_object_get(root, "control"), "token");
    if (!json_string_is_plain(token, &token_length) || token_length == 0u ||
        token_length > MCP_SHELL_POLICY_TOKEN_MAX_BYTES) {
        policy_error(error,
                     error_size,
                     "control.token must contain 1..%u bytes without embedded NUL",
                     MCP_SHELL_POLICY_TOKEN_MAX_BYTES);
        return -1;
    }

    if (mcp_shell_policy_snapshot_create_hard(&snapshot, config_path, control_enabled) != 0) {
        policy_error(error, error_size, "out of memory while creating policy snapshot");
        return -1;
    }
    snapshot->config_loaded = true;
    token_value = json_string_value(token);
    snapshot->token = malloc(token_length + 1u);
    if (!snapshot->token) {
        policy_error(error, error_size, "out of memory while copying control token");
        goto fail;
    }
    memcpy(snapshot->token, token_value, token_length);
    snapshot->token[token_length] = '\0';
    snapshot->token_length = token_length;

    for (index = 0; index < sizeof(policy_fields) / sizeof(policy_fields[0]); index++) {
        if (policy_fields[index].parser(
                snapshot, root, &policy_fields[index], error, error_size) != 0)
            goto fail;
        if (!policy_fields[index].validator(snapshot, &policy_fields[index])) {
            policy_error(error,
                         error_size,
                         "internal validation failed for %s",
                         policy_fields[index].path);
            goto fail;
        }
    }
    if (!snapshot->defaults.capture_stderr)
        snapshot->defaults.merge_stderr_to_stdout = false;

    *out = snapshot;
    return 0;

fail:
    mcp_shell_policy_snapshot_destroy(snapshot);
    return -1;
}

static void policy_token_zero(char *token, size_t length)
{
    volatile unsigned char *bytes = (volatile unsigned char *)token;
    size_t index;

    if (!bytes)
        return;
    for (index = 0; index < length; index++)
        bytes[index] = 0;
}

void mcp_shell_policy_snapshot_destroy(struct mcp_shell_policy_snapshot *snapshot)
{
    size_t index;

    if (!snapshot)
        return;
    policy_token_zero(snapshot->token, snapshot->token_length);
    free(snapshot->token);
    free(snapshot->config_path);
    for (index = 0; index < sizeof(policy_fields) / sizeof(policy_fields[0]); index++) {
        if (policy_fields[index].type == MCP_SHELL_POLICY_TYPE_STRING) {
            struct mcp_shell_policy_string *string =
                policy_field_value(snapshot, &policy_fields[index]);
            free(string->value);
        }
    }
    policy_env_destroy(snapshot->defaults.execution.env_vars,
                       snapshot->defaults.execution.env_var_count);
    policy_env_destroy(snapshot->startup_env_vars, snapshot->startup_env_var_count);
    free(snapshot);
}

const char *mcp_shell_policy_snapshot_diagnostic(
    const struct mcp_shell_policy_snapshot *snapshot,
    enum mcp_shell_policy_field_id field)
{
    if (!snapshot || field < 0 || field >= MCP_SHELL_POLICY_FIELD_COUNT)
        return NULL;
    return snapshot->diagnostics[field];
}
