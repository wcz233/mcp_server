#include "tools/shell_exec.h"

#include "common/platform.h"
#include "core/server_internal.h"
#include "tools/shell_policy.h"
#include "tools/tool_result.h"

#include <ctype.h>
#include <errno.h>
#include <jansson.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#else
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define MCP_SHELL_EXEC_MIN_TIMEOUT_MS 1u
#define MCP_SHELL_EXEC_DEFAULT_CHUNK_SIZE 1024u
#define MCP_SHELL_JOB_POLL_MS 50u
#define MCP_SHELL_JOB_DEFAULT_RETENTION_MS 600000u
#define MCP_SHELL_JOB_MAX_RETENTION_MS 3600000u
#define MCP_SHELL_JOB_WAIT_MAX_MS 5000u
#define MCP_SANDBOX_CTL_MAX_TIMEOUT_MS 300000u
#define MCP_SANDBOX_CTL_MAX_OUTPUT_BYTES 2147483648u
#define MCP_SANDBOX_CTL_MAX_WORKING_DIRECTORY_BYTES 4096u
#define MCP_SANDBOX_CTL_MAX_ALLOWED_ENV_ITEMS 128u
#define MCP_SANDBOX_CTL_MAX_ALLOWED_ENV_NAME_BYTES 255u

enum shell_exec_mode {
    SHELL_EXEC_MODE_SHELL = 0,
    SHELL_EXEC_MODE_EXEC = 1,
};

enum shell_job_state {
    SHELL_JOB_RUNNING = 1,
    SHELL_JOB_EXITED,
    SHELL_JOB_TIMED_OUT,
    SHELL_JOB_KILLED,
    SHELL_JOB_FAILED,
};

struct shell_exec_env_var {
    char *name;
    char *value;
};

struct shell_exec_config {
    bool enabled;
    bool capture_stderr;
    bool merge_stderr;
    bool config_loaded;
    bool sandbox_enabled;
    bool shell_enabled;
    bool clear_environment;
    bool kill_process_group_on_timeout;
    bool request_cwd_allowed;
    bool request_env_allowed;
    bool allowed_env_is_set;
    bool require_non_root;
    enum shell_exec_mode mode;
    unsigned int max_command_length;
    unsigned int default_timeout_ms;
    unsigned int max_timeout_ms;
    unsigned int max_output_bytes;
    unsigned int chunk_size;
    uint64_t max_cpu_seconds;
    uint64_t max_memory_bytes;
    uint64_t max_file_size_bytes;
    uint64_t max_open_files;
    uint64_t max_processes;
    size_t working_directory_min_bytes;
    size_t working_directory_max_bytes;
    size_t environment_max_items;
    size_t environment_item_max_bytes;
    json_int_t sandbox_revision;
    char *shell_path;
    char *shell_arg;
    char *working_directory;
    char *run_as_user;
    char *run_as_group;
    struct shell_exec_env_var *env_vars;
    size_t env_var_count;
    char **allowed_env;
    size_t allowed_env_count;
    char *config_path;
    char *load_error;
};

struct shell_exec_buffer {
    char *data;
    size_t len;
    size_t cap;
    bool truncated;
};

struct shell_exec_request {
    char *command;
    unsigned int timeout_ms;
};

struct shell_exec_outcome {
    struct shell_exec_buffer stdout_buf;
    struct shell_exec_buffer stderr_buf;
    int exit_code;
    int signal_number;
    bool timed_out;
    bool spawn_failed;
    char *spawn_error;
};

struct shell_job {
    char *job_id;
    char *label;
    enum shell_job_state state;
    struct shell_exec_buffer stdout_buf;
    struct shell_exec_buffer stderr_buf;
    unsigned int timeout_ms;
    unsigned int output_limit_bytes;
    unsigned int chunk_size;
    unsigned long long started_ms;
    unsigned long long deadline_ms;
    unsigned long long finished_ms;
    char started_at[48];
    char finished_at[48];
    int exit_code;
    int signal_number;
    bool timed_out_requested;
    bool killed_requested;
    bool stdout_open;
    bool stderr_open;
    bool process_reaped;
    bool kill_process_group;
#ifndef _WIN32
    pid_t pid;
    pid_t process_group_id;
    int stdout_fd;
    int stderr_fd;
#else
    int pid;
    int process_group_id;
#endif
    struct shell_job *next;
};

struct mcp_shell_sandbox_control {
    bool enabled;
    const struct mcp_shell_policy_snapshot *policy_snapshot;
    json_int_t revision;
    bool sandbox_enabled;
    json_t *overrides;
};

struct mcp_shell_job_store {
    struct mcp_server *server;
    uv_loop_t *loop;
    uv_timer_t poll_timer;
    bool poll_timer_initialized;
    bool poll_timer_running;
    bool shutting_down;
    unsigned long long next_job_number;
    unsigned int retention_ms;
    struct shell_job *jobs;
};

static int shell_exec_prepare_working_directory(const struct shell_exec_config *cfg);

int mcp_shell_sandbox_control_create(struct mcp_shell_sandbox_control **out,
                                     const struct mcp_shell_policy_snapshot *policy_snapshot)
{
    struct mcp_shell_sandbox_control *control;

    if (!out || !policy_snapshot)
        return -1;
    *out = NULL;

    control = calloc(1, sizeof(*control));
    if (!control)
        return -1;

    control->overrides = json_object();
    if (!control->overrides) {
        free(control);
        return -1;
    }
    control->policy_snapshot = policy_snapshot;
    control->enabled = policy_snapshot->control_enabled;
    control->sandbox_enabled = control->enabled;

    *out = control;
    return 0;
}

void mcp_shell_sandbox_control_destroy(struct mcp_shell_sandbox_control *control)
{
    if (!control)
        return;

    json_decref(control->overrides);
    free(control);
}

bool mcp_shell_sandbox_control_is_enabled(const struct mcp_shell_sandbox_control *control)
{
    return control && control->enabled;
}

static const char *shell_job_state_name(enum shell_job_state state)
{
    switch (state) {
    case SHELL_JOB_RUNNING:
        return "running";
    case SHELL_JOB_EXITED:
        return "exited";
    case SHELL_JOB_TIMED_OUT:
        return "timed_out";
    case SHELL_JOB_KILLED:
        return "killed";
    case SHELL_JOB_FAILED:
        return "failed";
    }
    return "failed";
}

static bool shell_job_is_final(const struct shell_job *job)
{
    return job && job->state != SHELL_JOB_RUNNING;
}

static unsigned int clamp_uint(unsigned long parsed,
                               unsigned int default_value,
                               unsigned int min_value,
                               unsigned int max_value)
{
    if (parsed < (unsigned long)min_value || parsed > (unsigned long)max_value)
        return default_value;
    return (unsigned int)parsed;
}

static unsigned int env_uint(const char *name,
                             unsigned int default_value,
                             unsigned int min_value,
                             unsigned int max_value)
{
    const char *value = getenv(name);
    char *end = NULL;
    unsigned long parsed;

    if (!value || value[0] == '\0')
        return default_value;

    parsed = strtoul(value, &end, 10);
    if (!end || *end != '\0')
        return default_value;

    return clamp_uint(parsed, default_value, min_value, max_value);
}

static int dup_string_field(char **dst, const char *value)
{
    char *copy = NULL;

    if (value) {
        copy = mcp_strdup(value);
        if (!copy)
            return -1;
    }

    free(*dst);
    *dst = copy;
    return 0;
}

static void shell_exec_env_vars_destroy(struct shell_exec_config *cfg)
{
    size_t i;

    if (!cfg)
        return;
    for (i = 0; i < cfg->env_var_count; i++) {
        free(cfg->env_vars[i].name);
        free(cfg->env_vars[i].value);
    }
    free(cfg->env_vars);
    cfg->env_vars = NULL;
    cfg->env_var_count = 0;
}

static void shell_exec_allowed_env_destroy(struct shell_exec_config *cfg)
{
    size_t i;

    if (!cfg)
        return;
    for (i = 0; i < cfg->allowed_env_count; i++)
        free(cfg->allowed_env[i]);
    free(cfg->allowed_env);
    cfg->allowed_env = NULL;
    cfg->allowed_env_count = 0;
}

static void shell_exec_config_destroy(struct shell_exec_config *cfg)
{
    if (!cfg)
        return;

    shell_exec_env_vars_destroy(cfg);
    shell_exec_allowed_env_destroy(cfg);
    free(cfg->shell_path);
    free(cfg->shell_arg);
    free(cfg->working_directory);
    free(cfg->run_as_user);
    free(cfg->run_as_group);
    free(cfg->config_path);
    free(cfg->load_error);
    memset(cfg, 0, sizeof(*cfg));
}

static int set_config_error(struct shell_exec_config *cfg, const char *message)
{
    free(cfg->load_error);
    cfg->load_error = mcp_strdup(message ? message : "Unknown shell_exec config error.");
    return cfg->load_error ? 0 : -1;
}

static bool shell_exec_env_name_is_valid(const char *name)
{
    return name && name[0] != '\0' && !strchr(name, '=');
}

static bool shell_exec_env_name_equals(const char *left, const char *right)
{
#ifdef _WIN32
    return _stricmp(left, right) == 0;
#else
    return strcmp(left, right) == 0;
#endif
}

static int shell_exec_config_set_env_var(struct shell_exec_config *cfg,
                                         const char *name,
                                         const char *value)
{
    struct shell_exec_env_var *next;
    char *name_copy;
    char *value_copy;
    size_t i;

    if (!shell_exec_env_name_is_valid(name))
        return set_config_error(cfg, "shell_exec env names must be non-empty and must not contain '='.");
    if (!value)
        value = "";

    for (i = 0; i < cfg->env_var_count; i++) {
        if (shell_exec_env_name_equals(cfg->env_vars[i].name, name)) {
            value_copy = mcp_strdup(value);
            if (!value_copy)
                return -1;
            free(cfg->env_vars[i].value);
            cfg->env_vars[i].value = value_copy;
            return 0;
        }
    }

    name_copy = mcp_strdup(name);
    value_copy = mcp_strdup(value);
    if (!name_copy || !value_copy) {
        free(name_copy);
        free(value_copy);
        return -1;
    }

    next = realloc(cfg->env_vars, sizeof(*next) * (cfg->env_var_count + 1));
    if (!next) {
        free(name_copy);
        free(value_copy);
        return -1;
    }
    cfg->env_vars = next;
    cfg->env_vars[cfg->env_var_count].name = name_copy;
    cfg->env_vars[cfg->env_var_count].value = value_copy;
    cfg->env_var_count++;
    return 0;
}

enum shell_sandbox_action {
    SHELL_SANDBOX_ACTION_GET = 1,
    SHELL_SANDBOX_ACTION_UPDATE,
    SHELL_SANDBOX_ACTION_RESET,
};

enum shell_sandbox_patch_status {
    SHELL_SANDBOX_PATCH_OK = 0,
    SHELL_SANDBOX_PATCH_INVALID,
    SHELL_SANDBOX_PATCH_UNSUPPORTED,
    SHELL_SANDBOX_PATCH_INTERNAL,
};

struct shell_sandbox_patch_error {
    const char *field;
};

static bool shell_sandbox_key_equals(const char *key, size_t key_length, const char *expected)
{
    size_t expected_length = strlen(expected);

    return key_length == expected_length && memcmp(key, expected, expected_length) == 0;
}

static bool shell_sandbox_json_string_equals(const json_t *value, const char *expected)
{
    return json_is_string(value) &&
           shell_sandbox_key_equals(json_string_value(value), json_string_length(value), expected);
}

static int shell_sandbox_set_error(json_t **out_result,
                                   const char *code,
                                   const char *message,
                                   const char *field,
                                   bool include_revision,
                                   json_int_t revision)
{
    json_t *payload = json_object();

    if (!payload)
        return -1;
    if (json_object_set_new(payload, "code", json_string(code)) != 0 ||
        json_object_set_new(payload, "message", json_string(message)) != 0 ||
        (field && json_object_set_new(payload, "field", json_string(field)) != 0) ||
#ifdef _WIN32
        (strcmp(code, "unsupported_on_platform") == 0 &&
         json_object_set_new(payload, "platform", json_string("windows")) != 0) ||
#else
        (strcmp(code, "unsupported_on_platform") == 0 &&
         json_object_set_new(payload, "platform", json_string("unix")) != 0) ||
#endif
        (include_revision &&
         json_object_set_new(payload, "current_revision", json_integer(revision)) != 0)) {
        json_decref(payload);
        return -1;
    }

    *out_result = mcp_tool_result_json_text(payload, true);
    json_decref(payload);
    return *out_result ? 0 : -1;
}

static int shell_sandbox_invalid_params(json_t **out_result, const char *field)
{
    return shell_sandbox_set_error(out_result,
                                   "invalid_params",
                                   "invalid sandbox control parameters",
                                   field,
                                   false,
                                   0);
}

static int shell_sandbox_internal_error(json_t **out_result)
{
    return shell_sandbox_set_error(out_result,
                                   "internal_error",
                                   "sandbox control internal error",
                                   NULL,
                                   false,
                                   0);
}

static int shell_sandbox_encode_success(json_t *payload, json_t **out_result)
{
    char *dumped;
    json_t *result;
    json_t *is_error;

    dumped = json_dumps(payload, JSON_COMPACT | JSON_ENSURE_ASCII);
    if (!dumped)
        return -1;
    result = mcp_tool_result_text(dumped, false);
    free(dumped);
    if (!result)
        return -1;

    is_error = json_object_get(result, "isError");
    if (!json_is_false(is_error)) {
        json_decref(result);
        return -1;
    }

    *out_result = result;
    return 0;
}

static bool shell_sandbox_token_matches(const struct mcp_shell_sandbox_control *control,
                                        const json_t *provided)
{
    const unsigned char *actual;
    const unsigned char *expected;
    size_t actual_length;
    size_t expected_length;
    size_t difference;
    size_t index;

    if (!control || !control->enabled || !control->policy_snapshot ||
        !json_is_string(provided))
        return false;

    actual = (const unsigned char *)json_string_value(provided);
    actual_length = json_string_length(provided);
    expected = (const unsigned char *)control->policy_snapshot->token;
    expected_length = control->policy_snapshot->token_length;
    difference = actual_length ^ expected_length;
    for (index = 0; index < expected_length; index++) {
        unsigned char actual_byte = index < actual_length ? actual[index] : 0u;

        difference |= (size_t)(actual_byte ^ expected[index]);
    }
    return difference == 0u;
}

static enum shell_sandbox_action shell_sandbox_parse_action(const json_t *value)
{
    if (shell_sandbox_json_string_equals(value, "get"))
        return SHELL_SANDBOX_ACTION_GET;
    if (shell_sandbox_json_string_equals(value, "update"))
        return SHELL_SANDBOX_ACTION_UPDATE;
    if (shell_sandbox_json_string_equals(value, "reset"))
        return SHELL_SANDBOX_ACTION_RESET;
    return 0;
}

static bool shell_sandbox_top_level_is_valid(const json_t *arguments,
                                             enum shell_sandbox_action action,
                                             const char **invalid_field)
{
    const char *key;
    size_t key_length;
    json_t *value;

    json_object_keylen_foreach((json_t *)arguments, key, key_length, value) {
        bool allowed = shell_sandbox_key_equals(key, key_length, "action") ||
                       shell_sandbox_key_equals(key, key_length, "token");

        (void)value;
        if (action == SHELL_SANDBOX_ACTION_UPDATE) {
            allowed = allowed || shell_sandbox_key_equals(key, key_length, "expected_revision") ||
                      shell_sandbox_key_equals(key, key_length, "sandbox_enabled") ||
                      shell_sandbox_key_equals(key, key_length, "overrides");
        } else if (action == SHELL_SANDBOX_ACTION_RESET) {
            allowed = allowed || shell_sandbox_key_equals(key, key_length, "expected_revision");
        }
        if (!allowed) {
            *invalid_field = key;
            return false;
        }
    }
    return true;
}

static json_t *shell_sandbox_override_group(json_t *overrides, const char *name)
{
    json_t *group = json_object_get(overrides, name);

    if (group)
        return group;
    group = json_object();
    if (!group || json_object_set_new(overrides, name, group) != 0) {
        json_decref(group);
        return NULL;
    }
    return group;
}

static enum shell_sandbox_patch_status shell_sandbox_store_patch_value(
    json_t *target,
    const char *key,
    const json_t *value)
{
    json_t *copy;

    if (json_is_null(value)) {
        json_object_del(target, key);
        return SHELL_SANDBOX_PATCH_OK;
    }

    copy = json_deep_copy(value);
    if (!copy)
        return SHELL_SANDBOX_PATCH_INTERNAL;
    if (json_object_set_new(target, key, copy) != 0) {
        json_decref(copy);
        return SHELL_SANDBOX_PATCH_INTERNAL;
    }
    return SHELL_SANDBOX_PATCH_OK;
}

static enum shell_sandbox_patch_status shell_sandbox_record_path(json_t *changed_paths,
                                                                 const char *field)
{
    if (json_array_append_new(changed_paths, json_string(field)) != 0)
        return SHELL_SANDBOX_PATCH_INTERNAL;
    return SHELL_SANDBOX_PATCH_OK;
}

static const char *shell_v2_override_path(
    const struct mcp_shell_policy_field_descriptor *field)
{
    static const char prefix[] = "defaults.";

    return field && strncmp(field->path, prefix, sizeof(prefix) - 1u) == 0
               ? field->path + sizeof(prefix) - 1u
               : NULL;
}

static const struct mcp_shell_policy_field_descriptor *shell_v2_field_by_path(
    const char *path)
{
    const struct mcp_shell_policy_field_descriptor *fields;
    size_t count;
    size_t index;

    fields = mcp_shell_policy_field_directory(&count);
    for (index = 0; index < count; index++) {
        const char *candidate = shell_v2_override_path(&fields[index]);

        if (candidate && strcmp(candidate, path) == 0)
            return &fields[index];
    }
    return NULL;
}

static bool shell_v2_path_is_group(const char *path)
{
    const struct mcp_shell_policy_field_descriptor *fields;
    size_t count;
    size_t index;
    size_t length = strlen(path);

    fields = mcp_shell_policy_field_directory(&count);
    for (index = 0; index < count; index++) {
        const char *candidate = shell_v2_override_path(&fields[index]);

        if (candidate && strncmp(candidate, path, length) == 0 && candidate[length] == '.')
            return true;
    }
    return false;
}

static const void *shell_v2_field_value(
    const struct mcp_shell_policy_snapshot *snapshot,
    const struct mcp_shell_policy_field_descriptor *field)
{
    return (const unsigned char *)&snapshot->defaults + field->value_offset;
}

static const char *shell_v2_capability(
    const struct mcp_shell_policy_field_descriptor *field)
{
#ifdef _WIN32
    if (field->capability == MCP_SHELL_POLICY_CAPABILITY_UNIX_ONLY ||
        field->id == MCP_SHELL_POLICY_FIELD_KILL_PROCESS_GROUP ||
        field->id == MCP_SHELL_POLICY_FIELD_MEMORY_BYTES)
        return "unsupported";
    if (field->id == MCP_SHELL_POLICY_FIELD_REQUIRE_NON_ROOT)
        return "reject_only";
#endif
    return "enforced";
}

static enum shell_sandbox_patch_status shell_v2_validate_patch_value(
    const struct mcp_shell_policy_snapshot *snapshot,
    const struct mcp_shell_policy_field_descriptor *field,
    const json_t *value)
{
    const void *current = shell_v2_field_value(snapshot, field);

    if (json_is_null(value))
        return SHELL_SANDBOX_PATCH_OK;
    if (strcmp(shell_v2_capability(field), "unsupported") == 0)
        return SHELL_SANDBOX_PATCH_UNSUPPORTED;

    switch (field->type) {
    case MCP_SHELL_POLICY_TYPE_BOOLEAN:
        return json_is_boolean(value) ? SHELL_SANDBOX_PATCH_OK : SHELL_SANDBOX_PATCH_INVALID;
    case MCP_SHELL_POLICY_TYPE_UINT64: {
        const struct mcp_shell_policy_numeric *number = current;
        json_int_t integer;

        if (!json_is_integer(value))
            return SHELL_SANDBOX_PATCH_INVALID;
        integer = json_integer_value(value);
        return integer >= 0 && (uint64_t)integer >= number->min &&
                       (uint64_t)integer <= number->max
                   ? SHELL_SANDBOX_PATCH_OK
                   : SHELL_SANDBOX_PATCH_INVALID;
    }
    case MCP_SHELL_POLICY_TYPE_STRING: {
        const struct mcp_shell_policy_string *string = current;
        size_t length;

        if (!json_is_string(value))
            return SHELL_SANDBOX_PATCH_INVALID;
        length = json_string_length(value);
        return strlen(json_string_value(value)) == length && length >= string->min_bytes &&
                       length <= string->max_bytes
                   ? SHELL_SANDBOX_PATCH_OK
                   : SHELL_SANDBOX_PATCH_INVALID;
    }
    case MCP_SHELL_POLICY_TYPE_MODE: {
        const struct mcp_shell_policy_mode_value *mode = current;
        unsigned int requested;

        if (!json_is_string(value) ||
            strlen(json_string_value(value)) != json_string_length(value))
            return SHELL_SANDBOX_PATCH_INVALID;
        if (shell_sandbox_json_string_equals(value, "shell"))
            requested = MCP_SHELL_POLICY_MODE_SHELL_MASK;
        else if (shell_sandbox_json_string_equals(value, "exec"))
            requested = MCP_SHELL_POLICY_MODE_EXEC_MASK;
        else
            return SHELL_SANDBOX_PATCH_INVALID;
        return (mode->allowed_mask & requested) != 0u ? SHELL_SANDBOX_PATCH_OK
                                                      : SHELL_SANDBOX_PATCH_INVALID;
    }
    case MCP_SHELL_POLICY_TYPE_ENV: {
        const char *name;
        json_t *item;

        if (!json_is_object(value) ||
            json_object_size(value) > snapshot->environment_max_items)
            return SHELL_SANDBOX_PATCH_INVALID;
        json_object_foreach((json_t *)value, name, item) {
            size_t name_length = strlen(name);
            size_t value_length;

            if (name_length == 0u || strchr(name, '=') || !json_is_string(item))
                return SHELL_SANDBOX_PATCH_INVALID;
            value_length = json_string_length(item);
            if (strlen(json_string_value(item)) != value_length ||
                name_length + 1u + value_length > snapshot->environment_item_max_bytes)
                return SHELL_SANDBOX_PATCH_INVALID;
        }
        return SHELL_SANDBOX_PATCH_OK;
    }
    }
    return SHELL_SANDBOX_PATCH_INTERNAL;
}

static const char *shell_v2_unknown_field(const char *prefix)
{
    if (strcmp(prefix, "execution") == 0)
        return "overrides.execution";
    if (strcmp(prefix, "limits") == 0)
        return "overrides.limits";
    if (strcmp(prefix, "isolation") == 0)
        return "overrides.isolation";
    return "overrides";
}

static enum shell_sandbox_patch_status shell_v2_apply_patch_node(
    const struct mcp_shell_policy_snapshot *snapshot,
    json_t *target,
    const json_t *patch,
    const char *prefix,
    json_t *changed_paths,
    size_t *leaf_count,
    struct shell_sandbox_patch_error *error)
{
    const char *key;
    json_t *value;

    if (!json_is_object(patch)) {
        error->field = shell_v2_unknown_field(prefix);
        return SHELL_SANDBOX_PATCH_INVALID;
    }

    json_object_foreach((json_t *)patch, key, value) {
        char path[128];
        int written = prefix[0] == '\0' ? snprintf(path, sizeof(path), "%s", key)
                                        : snprintf(path, sizeof(path), "%s.%s", prefix, key);
        const struct mcp_shell_policy_field_descriptor *field;
        enum shell_sandbox_patch_status status;

        if (written < 0 || (size_t)written >= sizeof(path)) {
            error->field = shell_v2_unknown_field(prefix);
            return SHELL_SANDBOX_PATCH_INVALID;
        }
        field = shell_v2_field_by_path(path);
        if (field) {
            json_t *group = target;
            const char *name = path;
            const char *separator = strchr(path, '.');

            status = shell_v2_validate_patch_value(snapshot, field, value);
            if (status != SHELL_SANDBOX_PATCH_OK) {
                error->field = shell_v2_override_path(field);
                return status;
            }
            if (separator) {
                char group_name[32];
                size_t group_length = (size_t)(separator - path);

                if (group_length >= sizeof(group_name))
                    return SHELL_SANDBOX_PATCH_INTERNAL;
                memcpy(group_name, path, group_length);
                group_name[group_length] = '\0';
                group = shell_sandbox_override_group(target, group_name);
                if (!group)
                    return SHELL_SANDBOX_PATCH_INTERNAL;
                name = separator + 1;
            }
            status = shell_sandbox_store_patch_value(group, name, value);
            if (status != SHELL_SANDBOX_PATCH_OK)
                return status;
            if (separator && json_object_size(group) == 0u) {
                char group_name[32];
                size_t group_length = (size_t)(separator - path);

                memcpy(group_name, path, group_length);
                group_name[group_length] = '\0';
                json_object_del(target, group_name);
            }
            if (shell_sandbox_record_path(changed_paths, path) != SHELL_SANDBOX_PATCH_OK)
                return SHELL_SANDBOX_PATCH_INTERNAL;
            (*leaf_count)++;
            continue;
        }

        if (!shell_v2_path_is_group(path) || !json_is_object(value) || prefix[0] != '\0') {
            error->field = shell_v2_unknown_field(prefix);
            return SHELL_SANDBOX_PATCH_INVALID;
        }
        status = shell_v2_apply_patch_node(
            snapshot, target, value, path, changed_paths, leaf_count, error);
        if (status != SHELL_SANDBOX_PATCH_OK)
            return status;
    }
    return SHELL_SANDBOX_PATCH_OK;
}

static enum shell_sandbox_patch_status shell_v2_apply_overrides_patch(
    const struct mcp_shell_policy_snapshot *snapshot,
    json_t *target,
    const json_t *patch,
    json_t *changed_paths,
    size_t *leaf_count,
    struct shell_sandbox_patch_error *error)
{
    return shell_v2_apply_patch_node(
        snapshot, target, patch, "", changed_paths, leaf_count, error);
}

static bool shell_sandbox_policy_is_valid(const struct shell_exec_config *cfg,
                                          const char **invalid_field)
{
    size_t index;

    if (!cfg->config_loaded) {
        if (invalid_field)
            *invalid_field = "config";
        return false;
    }
#define SHELL_SANDBOX_VALIDATE(condition, field)                                         \
    do {                                                                                  \
        if (!(condition)) {                                                               \
            if (invalid_field)                                                            \
                *invalid_field = field;                                                   \
            return false;                                                                 \
        }                                                                                 \
    } while (0)

    SHELL_SANDBOX_VALIDATE(cfg->max_command_length >= 64u &&
                               cfg->max_command_length <= 65535u,
                           "max_command_length");
    SHELL_SANDBOX_VALIDATE(cfg->default_timeout_ms >= 1u &&
                               cfg->default_timeout_ms <= MCP_SANDBOX_CTL_MAX_TIMEOUT_MS,
                           "default_timeout_ms");
    SHELL_SANDBOX_VALIDATE(cfg->max_timeout_ms >= 1u &&
                               cfg->max_timeout_ms <= MCP_SANDBOX_CTL_MAX_TIMEOUT_MS,
                           "max_timeout_ms");
    SHELL_SANDBOX_VALIDATE(cfg->default_timeout_ms <= cfg->max_timeout_ms,
                           "default_timeout_ms");
    SHELL_SANDBOX_VALIDATE(cfg->max_output_bytes >= 256u &&
                               cfg->max_output_bytes <= MCP_SANDBOX_CTL_MAX_OUTPUT_BYTES,
                           "max_output_bytes");
    SHELL_SANDBOX_VALIDATE(cfg->capture_stderr || !cfg->merge_stderr, "capture_stderr");
    SHELL_SANDBOX_VALIDATE(cfg->working_directory && cfg->working_directory[0] != '\0' &&
                               strlen(cfg->working_directory) <=
                                   MCP_SANDBOX_CTL_MAX_WORKING_DIRECTORY_BYTES,
                           "execution.working_directory");
    SHELL_SANDBOX_VALIDATE(cfg->max_cpu_seconds <= 3600u, "limits.max_cpu_seconds");
    SHELL_SANDBOX_VALIDATE(cfg->max_memory_bytes <= 2147483647u,
                           "limits.max_memory_bytes");
    SHELL_SANDBOX_VALIDATE(cfg->max_file_size_bytes <= 2147483647u,
                           "limits.max_file_size_bytes");
    SHELL_SANDBOX_VALIDATE(cfg->max_open_files <= 1048576u, "limits.max_open_files");
    SHELL_SANDBOX_VALIDATE(cfg->max_processes <= 1048576u, "limits.max_processes");
    SHELL_SANDBOX_VALIDATE(cfg->allowed_env_count <= MCP_SANDBOX_CTL_MAX_ALLOWED_ENV_ITEMS,
                           "execution.allowed_env");

    for (index = 0; index < cfg->allowed_env_count; index++) {
        size_t length = strlen(cfg->allowed_env[index]);

        SHELL_SANDBOX_VALIDATE(length > 0u &&
                                   length <= MCP_SANDBOX_CTL_MAX_ALLOWED_ENV_NAME_BYTES &&
                                   !strchr(cfg->allowed_env[index], '='),
                               "execution.allowed_env");
    }
#undef SHELL_SANDBOX_VALIDATE
    return true;
}

static const json_t *shell_v2_json_path_get(const json_t *root, const char *path)
{
    const char *separator = strchr(path, '.');

    if (!separator)
        return json_object_get(root, path);
    {
        char group_name[32];
        size_t group_length = (size_t)(separator - path);
        const json_t *group;

        if (group_length >= sizeof(group_name))
            return NULL;
        memcpy(group_name, path, group_length);
        group_name[group_length] = '\0';
        group = json_object_get(root, group_name);
        return json_is_object(group) ? json_object_get(group, separator + 1) : NULL;
    }
}

static int shell_v2_json_path_set(json_t *root, const char *path, json_t *value)
{
    const char *separator = strchr(path, '.');

    if (!value)
        return -1;
    if (!separator) {
        if (json_object_set_new(root, path, value) != 0) {
            json_decref(value);
            return -1;
        }
        return 0;
    }
    {
        char group_name[32];
        size_t group_length = (size_t)(separator - path);
        json_t *group;

        if (group_length >= sizeof(group_name)) {
            json_decref(value);
            return -1;
        }
        memcpy(group_name, path, group_length);
        group_name[group_length] = '\0';
        group = shell_sandbox_override_group(root, group_name);
        if (!group || json_object_set_new(group, separator + 1, value) != 0) {
            json_decref(value);
            return -1;
        }
        return 0;
    }
}

static json_t *shell_v2_environment_summary(size_t items)
{
    return json_pack("{s:I,s:b}", "items", (json_int_t)items, "valid", true);
}

static json_t *shell_v2_redacted_override_value(
    const struct mcp_shell_policy_field_descriptor *field,
    const json_t *value)
{
    if (field->type == MCP_SHELL_POLICY_TYPE_ENV)
        return shell_v2_environment_summary(json_object_size(value));
    return json_deep_copy(value);
}

static json_t *shell_v2_hard_value(
    const struct mcp_shell_policy_field_descriptor *field)
{
    switch (field->type) {
    case MCP_SHELL_POLICY_TYPE_BOOLEAN:
        return json_boolean(field->hard_default != 0u);
    case MCP_SHELL_POLICY_TYPE_UINT64:
        return json_integer((json_int_t)field->hard_default);
    case MCP_SHELL_POLICY_TYPE_STRING:
        return json_string(field->hard_default_string);
    case MCP_SHELL_POLICY_TYPE_MODE:
        return json_string(field->hard_default == MCP_SHELL_POLICY_MODE_EXEC ? "exec" : "shell");
    case MCP_SHELL_POLICY_TYPE_ENV:
        return shell_v2_environment_summary(
#ifdef _WIN32
            2u
#else
            3u
#endif
        );
    }
    return NULL;
}

static const char *shell_v2_source_name(enum mcp_shell_policy_source source)
{
    switch (source) {
    case MCP_SHELL_POLICY_SOURCE_JSON:
        return "json";
    case MCP_SHELL_POLICY_SOURCE_HARD_FALLBACK:
        return "hard_fallback";
    case MCP_SHELL_POLICY_SOURCE_HARD:
        return "hard_fallback";
    }
    return "hard_fallback";
}

static enum mcp_shell_policy_source shell_v2_field_source(
    const struct mcp_shell_policy_snapshot *snapshot,
    const struct mcp_shell_policy_field_descriptor *field)
{
    const void *value = shell_v2_field_value(snapshot, field);

    switch (field->type) {
    case MCP_SHELL_POLICY_TYPE_UINT64:
        return ((const struct mcp_shell_policy_numeric *)value)->source;
    case MCP_SHELL_POLICY_TYPE_STRING:
        return ((const struct mcp_shell_policy_string *)value)->source;
    case MCP_SHELL_POLICY_TYPE_MODE:
        return ((const struct mcp_shell_policy_mode_value *)value)->source;
    case MCP_SHELL_POLICY_TYPE_BOOLEAN:
        return snapshot->config_loaded ? MCP_SHELL_POLICY_SOURCE_JSON
                                       : MCP_SHELL_POLICY_SOURCE_HARD;
    case MCP_SHELL_POLICY_TYPE_ENV:
        return snapshot->environment_source;
    }
    return MCP_SHELL_POLICY_SOURCE_HARD;
}

static json_t *shell_v2_field_bounds(
    const struct mcp_shell_policy_snapshot *snapshot,
    const struct mcp_shell_policy_field_descriptor *field,
    bool sandbox_enabled)
{
    const void *value = shell_v2_field_value(snapshot, field);

    switch (field->type) {
    case MCP_SHELL_POLICY_TYPE_UINT64: {
        const struct mcp_shell_policy_numeric *number = value;
        uint64_t minimum = sandbox_enabled ? number->min : field->hard_min;
        uint64_t maximum = sandbox_enabled ? number->max : field->hard_max;

        return json_pack("{s:I,s:I}",
                         "min",
                         (json_int_t)minimum,
                         "max",
                         (json_int_t)maximum);
    }
    case MCP_SHELL_POLICY_TYPE_STRING: {
        const struct mcp_shell_policy_string *string = value;
        size_t minimum = sandbox_enabled ? string->min_bytes : field->hard_min_bytes;
        size_t maximum = sandbox_enabled ? string->max_bytes : field->hard_max_bytes;

        return json_pack("{s:I,s:I}",
                         "min_bytes",
                         (json_int_t)minimum,
                         "max_bytes",
                         (json_int_t)maximum);
    }
    case MCP_SHELL_POLICY_TYPE_MODE: {
        const struct mcp_shell_policy_mode_value *mode = value;
        unsigned int allowed = sandbox_enabled
                                   ? mode->allowed_mask
                                   : MCP_SHELL_POLICY_MODE_SHELL_MASK |
                                         MCP_SHELL_POLICY_MODE_EXEC_MASK;
        json_t *values = json_array();
        json_t *bounds = json_object();

        if (!values || !bounds)
            goto mode_fail;
        if ((allowed & MCP_SHELL_POLICY_MODE_SHELL_MASK) != 0u &&
            json_array_append_new(values, json_string("shell")) != 0)
            goto mode_fail;
        if ((allowed & MCP_SHELL_POLICY_MODE_EXEC_MASK) != 0u &&
            json_array_append_new(values, json_string("exec")) != 0)
            goto mode_fail;
        if (json_object_set_new(bounds, "allowed", values) != 0)
            goto mode_fail;
        return bounds;

    mode_fail:
        json_decref(values);
        json_decref(bounds);
        return NULL;
    }
    case MCP_SHELL_POLICY_TYPE_ENV:
        return json_pack("{s:I,s:I}",
                         "max_items",
                         (json_int_t)(sandbox_enabled ? snapshot->environment_max_items
                                                     : MCP_SHELL_POLICY_ENV_HARD_MAX_ITEMS),
                         "item_max_bytes",
                         (json_int_t)(sandbox_enabled
                                          ? snapshot->environment_item_max_bytes
                                          : MCP_SHELL_POLICY_ENV_HARD_ITEM_MAX_BYTES));
    case MCP_SHELL_POLICY_TYPE_BOOLEAN:
        return json_pack("{s:[b,b]}", "allowed", false, true);
    }
    return NULL;
}

static json_t *shell_v2_state_json(const struct mcp_shell_policy_snapshot *snapshot,
                                   json_int_t revision,
                                   bool sandbox_enabled,
                                   const json_t *overrides)
{
    const struct mcp_shell_policy_field_descriptor *fields;
    size_t field_count;
    size_t index;
    json_t *root = json_object();
    json_t *base = json_object();
    json_t *effective = json_object();
    json_t *saved_overrides = json_object();
    json_t *field_status = json_object();
    json_t *capabilities = json_object();
    json_t *diagnostics = json_array();
    json_t *pending_status = NULL;
    const json_t *shell_override;
    const json_t *effective_shell;

    if (!snapshot || !root || !base || !effective || !saved_overrides || !field_status ||
        !capabilities || !diagnostics)
        goto fail;

    fields = mcp_shell_policy_field_directory(&field_count);
    for (index = 0; index < field_count; index++) {
        const struct mcp_shell_policy_field_descriptor *field = &fields[index];
        const char *path = shell_v2_override_path(field);
        const json_t *override = shell_v2_json_path_get(overrides, path);
        const char *diagnostic = mcp_shell_policy_snapshot_diagnostic(snapshot, field->id);
        const char *source;
        json_t *base_value;
        json_t *effective_value;
        json_t *status;
        json_t *bounds;

        pending_status = json_object();
        status = pending_status;
        if (!path || !status)
            goto fail;
        base_value = field->serializer(snapshot, field);
        if (!base_value)
            goto fail;
        if (shell_v2_json_path_set(base, path, base_value) != 0)
            goto fail;

        if (!sandbox_enabled) {
            effective_value = shell_v2_hard_value(field);
            source = "hard_fallback";
        } else if (override) {
            effective_value = shell_v2_redacted_override_value(field, override);
            source = "runtime";
        } else {
            effective_value = field->serializer(snapshot, field);
            source = shell_v2_source_name(shell_v2_field_source(snapshot, field));
        }
        if (!effective_value || shell_v2_json_path_set(effective, path, effective_value) != 0)
            goto fail;
        if (override &&
            shell_v2_json_path_set(saved_overrides,
                                   path,
                                   shell_v2_redacted_override_value(field, override)) != 0)
            goto fail;

        if (json_object_set_new(status, "source", json_string(source)) != 0 ||
            json_object_set_new(status,
                                "diagnostic",
                                diagnostic ? json_string(diagnostic) : json_null()) != 0 ||
            json_object_set_new(status,
                                "capability",
                                json_string(shell_v2_capability(field))) != 0)
            goto fail;
        bounds = shell_v2_field_bounds(snapshot, field, sandbox_enabled);
        if (bounds && json_object_set_new(status, "bounds", bounds) != 0)
            goto fail;
        if (json_object_set_new(field_status, path, status) != 0)
            goto fail;
        pending_status = NULL;
        if (shell_v2_json_path_set(
                capabilities, path, json_string(shell_v2_capability(field))) != 0)
            goto fail;

        if (diagnostic) {
            json_t *item = json_pack("{s:s,s:s,s:s}",
                                     "field",
                                     path,
                                     "source",
                                     "hard_fallback",
                                     "reason",
                                     diagnostic);

            if (!item || json_array_append_new(diagnostics, item) != 0) {
                json_decref(item);
                goto fail;
            }
        }
    }

    if (!json_is_true(shell_v2_json_path_get(effective, "capture_stderr")) &&
        shell_v2_json_path_set(effective, "merge_stderr_to_stdout", json_false()) != 0)
        goto fail;

    shell_override = shell_v2_json_path_get(overrides, "shell_enabled");
    effective_shell = shell_v2_json_path_get(effective, "shell_enabled");

#define SHELL_V2_SET(name, value)                                                         \
    do {                                                                                  \
        if (json_object_set_new(root, name, value) != 0)                                  \
            goto fail;                                                                     \
    } while (0)

    SHELL_V2_SET("revision", json_integer(revision));
    SHELL_V2_SET("persistence", json_string("process"));
    SHELL_V2_SET("applies_to", json_string("new_executions"));
    SHELL_V2_SET("sandbox_enabled", json_boolean(sandbox_enabled));
    SHELL_V2_SET("shell_enabled", json_boolean(json_is_true(effective_shell)));
    SHELL_V2_SET("shell_enabled_override",
                 shell_override ? json_boolean(json_is_true(shell_override)) : json_null());
    SHELL_V2_SET("config_loaded", json_boolean(snapshot->config_loaded));
    SHELL_V2_SET("config_version", json_integer(snapshot->version));
    SHELL_V2_SET("policy_valid", json_true());
    SHELL_V2_SET("config_path", json_string(snapshot->config_path ? snapshot->config_path : ""));
    SHELL_V2_SET("base", base);
    base = NULL;
    SHELL_V2_SET("overrides", saved_overrides);
    saved_overrides = NULL;
    SHELL_V2_SET("effective", effective);
    effective = NULL;
    SHELL_V2_SET("field_status", field_status);
    field_status = NULL;
    SHELL_V2_SET("diagnostics", diagnostics);
    diagnostics = NULL;
    SHELL_V2_SET("capabilities", capabilities);
    capabilities = NULL;

#undef SHELL_V2_SET
    return root;

fail:
#undef SHELL_V2_SET
    json_decref(base);
    json_decref(effective);
    json_decref(saved_overrides);
    json_decref(field_status);
    json_decref(capabilities);
    json_decref(diagnostics);
    json_decref(pending_status);
    json_decref(root);
    return NULL;
}

static bool shell_sandbox_revision_can_increment(json_int_t revision)
{
    return revision >= 0 && revision < (json_int_t)LLONG_MAX;
}

static void shell_sandbox_audit_log(const char *action,
                                    json_int_t old_revision,
                                    json_int_t new_revision,
                                    bool sandbox_enabled,
                                    bool shell_enabled,
                                    const json_t *changed_paths)
{
    char timestamp[48];
    char *paths;

    if (!mcp_format_utc_now(timestamp, sizeof(timestamp)))
        strcpy(timestamp, "1970-01-01T00:00:00Z");
    paths = json_dumps(changed_paths, JSON_COMPACT | JSON_ENSURE_ASCII);
    fprintf(stderr,
            "sandbox_ctl audit time=%s action=%s old_revision=%" JSON_INTEGER_FORMAT
            " new_revision=%" JSON_INTEGER_FORMAT " sandbox_enabled=%s shell_enabled=%s "
            "changed=%s\n",
            timestamp,
            action,
            old_revision,
            new_revision,
            sandbox_enabled ? "true" : "false",
            shell_enabled ? "true" : "false",
            paths ? paths : "[]");
    free(paths);
}

int mcp_tool_system_sandbox_ctl(struct mcp_server *server,
                                const struct mcp_tool_invocation *invocation,
                                json_t **out_result)
{
    struct mcp_shell_sandbox_control *control = server ? server->sandbox_control : NULL;
    const json_t *arguments = invocation ? invocation->arguments : NULL;
    const json_t *token = json_is_object(arguments) ? json_object_get(arguments, "token") : NULL;
    const json_t *action_value;
    enum shell_sandbox_action action;
    const char *invalid_field = NULL;
    json_t *state = NULL;
    json_t *temporary_overrides = NULL;
    json_t *changed_paths = NULL;
    json_t *value;
    json_int_t expected_revision;
    json_int_t next_revision;
    bool temporary_sandbox_enabled;
    bool effective_shell_enabled;
    size_t leaf_count = 0u;
    int rc = -1;

    if (!out_result)
        return -1;
    *out_result = NULL;

    if (!shell_sandbox_token_matches(control, token)) {
        return shell_sandbox_set_error(out_result,
                                       "unauthorized",
                                       "sandbox control authentication failed",
                                       NULL,
                                       false,
                                       0);
    }

    action_value = json_object_get(arguments, "action");
    action = shell_sandbox_parse_action(action_value);
    if (action == 0)
        return shell_sandbox_invalid_params(out_result, "action");
    if (!shell_sandbox_top_level_is_valid(arguments, action, &invalid_field))
        return shell_sandbox_invalid_params(out_result, invalid_field);

    if (action == SHELL_SANDBOX_ACTION_GET) {
        state = shell_v2_state_json(control->policy_snapshot,
                                    control->revision,
                                    control->sandbox_enabled,
                                    control->overrides);
        if (!state)
            return shell_sandbox_internal_error(out_result);
        rc = shell_sandbox_encode_success(state, out_result);
        json_decref(state);
        return rc == 0 ? 0 : shell_sandbox_internal_error(out_result);
    }

    value = json_object_get(arguments, "expected_revision");
    if (!json_is_integer(value) || json_integer_value(value) < 0)
        return shell_sandbox_invalid_params(out_result, "expected_revision");
    expected_revision = json_integer_value(value);
    if (expected_revision != control->revision) {
        return shell_sandbox_set_error(out_result,
                                       "revision_conflict",
                                       "sandbox policy revision changed",
                                       NULL,
                                       true,
                                       control->revision);
    }

    if (!shell_sandbox_revision_can_increment(control->revision)) {
        return shell_sandbox_internal_error(out_result);
    }

    temporary_overrides = action == SHELL_SANDBOX_ACTION_RESET
                              ? json_object()
                              : json_deep_copy(control->overrides);
    changed_paths = json_array();
    if (!temporary_overrides || !changed_paths)
        goto internal_error;
    temporary_sandbox_enabled = control->sandbox_enabled;

    if (action == SHELL_SANDBOX_ACTION_RESET) {
        temporary_sandbox_enabled = true;
        if (shell_sandbox_record_path(changed_paths, "sandbox_enabled") !=
                SHELL_SANDBOX_PATCH_OK ||
            shell_sandbox_record_path(changed_paths, "overrides") != SHELL_SANDBOX_PATCH_OK)
            goto internal_error;
    } else {
        struct shell_sandbox_patch_error patch_error = {0};
        enum shell_sandbox_patch_status patch_status;

        value = json_object_get(arguments, "sandbox_enabled");
        if (value) {
            if (!json_is_boolean(value)) {
                invalid_field = "sandbox_enabled";
                goto invalid_params;
            }
            temporary_sandbox_enabled = json_is_true(value);
            if (shell_sandbox_record_path(changed_paths, "sandbox_enabled") !=
                SHELL_SANDBOX_PATCH_OK)
                goto internal_error;
            leaf_count++;
        }

        value = json_object_get(arguments, "overrides");
        if (value) {
            patch_status = shell_v2_apply_overrides_patch(control->policy_snapshot,
                                                          temporary_overrides,
                                                          value,
                                                          changed_paths,
                                                          &leaf_count,
                                                          &patch_error);
            if (patch_status == SHELL_SANDBOX_PATCH_INVALID) {
                invalid_field = patch_error.field;
                goto invalid_params;
            }
            if (patch_status == SHELL_SANDBOX_PATCH_UNSUPPORTED) {
                json_decref(temporary_overrides);
                json_decref(changed_paths);
                return shell_sandbox_set_error(out_result,
                                               "unsupported_on_platform",
                                               "sandbox field is unsupported on this platform",
                                               patch_error.field,
                                               false,
                                               0);
            }
            if (patch_status == SHELL_SANDBOX_PATCH_INTERNAL)
                goto internal_error;
        }
        if (leaf_count == 0u) {
            invalid_field = "overrides";
            goto invalid_params;
        }
    }

    next_revision = control->revision + 1;
    state = shell_v2_state_json(control->policy_snapshot,
                                next_revision,
                                temporary_sandbox_enabled,
                                temporary_overrides);
    if (!state)
        goto internal_error;
    effective_shell_enabled = json_is_true(json_object_get(state, "shell_enabled"));
    if (shell_sandbox_encode_success(state, out_result) != 0)
        goto internal_error;
    json_decref(state);
    state = NULL;

    json_decref(control->overrides);
    control->overrides = temporary_overrides;
    temporary_overrides = NULL;
    control->sandbox_enabled = temporary_sandbox_enabled;
    control->revision = next_revision;
    shell_sandbox_audit_log(action == SHELL_SANDBOX_ACTION_RESET ? "reset" : "update",
                            expected_revision,
                            next_revision,
                            temporary_sandbox_enabled,
                            effective_shell_enabled,
                            changed_paths);
    json_decref(changed_paths);
    return 0;

invalid_params:
    json_decref(temporary_overrides);
    json_decref(changed_paths);
    return shell_sandbox_invalid_params(out_result, invalid_field);

internal_error:
    json_decref(state);
    json_decref(temporary_overrides);
    json_decref(changed_paths);
    json_decref(*out_result);
    *out_result = NULL;
    return shell_sandbox_internal_error(out_result);
}

static int shell_exec_buffer_reserve(struct shell_exec_buffer *buffer, size_t want)
{
    char *next;
    size_t next_cap;

    if (want <= buffer->cap)
        return 0;

    next_cap = buffer->cap == 0 ? 512u : buffer->cap;
    while (next_cap < want)
        next_cap *= 2u;

    next = realloc(buffer->data, next_cap);
    if (!next)
        return -1;

    buffer->data = next;
    buffer->cap = next_cap;
    return 0;
}

static const struct mcp_shell_policy_field_descriptor *shell_v2_field(
    enum mcp_shell_policy_field_id id)
{
    const struct mcp_shell_policy_field_descriptor *fields;
    size_t count;

    fields = mcp_shell_policy_field_directory(&count);
    if ((size_t)id >= count || fields[id].id != id)
        return NULL;
    return &fields[id];
}

static const json_t *shell_v2_effective_override(
    const struct mcp_shell_sandbox_control *control,
    const struct mcp_shell_policy_field_descriptor *field)
{
    const char *path;

    if (!control->sandbox_enabled)
        return NULL;
    path = shell_v2_override_path(field);
    return path ? shell_v2_json_path_get(control->overrides, path) : NULL;
}

static bool shell_v2_effective_bool(const struct mcp_shell_sandbox_control *control,
                                    enum mcp_shell_policy_field_id id)
{
    const struct mcp_shell_policy_field_descriptor *field = shell_v2_field(id);
    const json_t *override;

    if (!field)
        return false;
    if (!control->sandbox_enabled)
        return field->hard_default != 0u;
    override = shell_v2_effective_override(control, field);
    if (override)
        return json_is_true(override);
    return *(const bool *)shell_v2_field_value(control->policy_snapshot, field);
}

static uint64_t shell_v2_effective_uint64(const struct mcp_shell_sandbox_control *control,
                                          enum mcp_shell_policy_field_id id)
{
    const struct mcp_shell_policy_field_descriptor *field = shell_v2_field(id);
    const json_t *override;
    const struct mcp_shell_policy_numeric *number;

    if (!field)
        return 0u;
    if (!control->sandbox_enabled)
        return field->hard_default;
    override = shell_v2_effective_override(control, field);
    if (override)
        return (uint64_t)json_integer_value(override);
    number = shell_v2_field_value(control->policy_snapshot, field);
    return number->value;
}

static const char *shell_v2_effective_string(const struct mcp_shell_sandbox_control *control,
                                             enum mcp_shell_policy_field_id id)
{
    const struct mcp_shell_policy_field_descriptor *field = shell_v2_field(id);
    const json_t *override;
    const struct mcp_shell_policy_string *string;

    if (!field)
        return NULL;
    if (!control->sandbox_enabled)
        return field->hard_default_string;
    override = shell_v2_effective_override(control, field);
    if (override)
        return json_string_value(override);
    string = shell_v2_field_value(control->policy_snapshot, field);
    return string->value;
}

static void shell_v2_effective_string_bounds(
    const struct mcp_shell_sandbox_control *control,
    enum mcp_shell_policy_field_id id,
    size_t *minimum,
    size_t *maximum)
{
    const struct mcp_shell_policy_field_descriptor *field = shell_v2_field(id);
    const struct mcp_shell_policy_string *string;

    if (!field || !control->sandbox_enabled) {
        *minimum = field ? field->hard_min_bytes : 0u;
        *maximum = field ? field->hard_max_bytes : 0u;
        return;
    }
    string = shell_v2_field_value(control->policy_snapshot, field);
    *minimum = string->min_bytes;
    *maximum = string->max_bytes;
}

static bool shell_exec_utf8_is_valid(const char *value, size_t length)
{
    json_t *validated = json_stringn(value, length);

    if (!validated)
        return false;
    json_decref(validated);
    return true;
}

static bool shell_exec_environment_is_valid(const struct shell_exec_config *cfg)
{
    size_t index;

    if (cfg->env_var_count > cfg->environment_max_items)
        return false;
    for (index = 0; index < cfg->env_var_count; index++) {
        const struct shell_exec_env_var *item = &cfg->env_vars[index];
        size_t name_length;
        size_t value_length;

        if (!shell_exec_env_name_is_valid(item->name) || !item->value)
            return false;
        name_length = strlen(item->name);
        value_length = strlen(item->value);
        if (name_length > SIZE_MAX - value_length - 1u ||
            name_length + 1u + value_length > cfg->environment_item_max_bytes ||
            !shell_exec_utf8_is_valid(item->name, name_length) ||
            !shell_exec_utf8_is_valid(item->value, value_length))
            return false;
    }
    return true;
}

static enum mcp_shell_policy_mode shell_v2_effective_mode(
    const struct mcp_shell_sandbox_control *control)
{
    const struct mcp_shell_policy_field_descriptor *field =
        shell_v2_field(MCP_SHELL_POLICY_FIELD_EXECUTION_MODE);
    const json_t *override;
    const struct mcp_shell_policy_mode_value *mode;

    if (!field || !control->sandbox_enabled)
        return MCP_SHELL_POLICY_MODE_SHELL;
    override = shell_v2_effective_override(control, field);
    if (override)
        return strcmp(json_string_value(override), "exec") == 0 ? MCP_SHELL_POLICY_MODE_EXEC
                                                                 : MCP_SHELL_POLICY_MODE_SHELL;
    mode = shell_v2_field_value(control->policy_snapshot, field);
    return mode->value;
}

static int shell_v2_copy_effective_environment(
    const struct mcp_shell_sandbox_control *control,
    struct shell_exec_config *cfg)
{
    const struct mcp_shell_policy_field_descriptor *field =
        shell_v2_field(MCP_SHELL_POLICY_FIELD_ENV);
    const json_t *override = field ? shell_v2_effective_override(control, field) : NULL;
    size_t index;

    if (!control->sandbox_enabled) {
#ifdef _WIN32
        if (shell_exec_config_set_env_var(cfg,
                                          "PATH",
                                          "%SystemRoot%\\System32;%SystemRoot%") != 0 ||
            shell_exec_config_set_env_var(cfg, "SystemRoot", "C:\\Windows") != 0)
            return -1;
#else
        if (shell_exec_config_set_env_var(cfg, "PATH", "/usr/bin:/bin") != 0 ||
            shell_exec_config_set_env_var(cfg, "HOME", "/tmp/mcp-shell") != 0 ||
            shell_exec_config_set_env_var(cfg, "LANG", "C") != 0)
            return -1;
#endif
        return 0;
    }
    if (override) {
        const char *name;
        json_t *value;

        json_object_foreach((json_t *)override, name, value) {
            if (shell_exec_config_set_env_var(cfg, name, json_string_value(value)) != 0)
                return -1;
        }
        return 0;
    }
    for (index = 0; index < control->policy_snapshot->defaults.execution.env_var_count;
         index++) {
        const struct mcp_shell_policy_env_var *item =
            &control->policy_snapshot->defaults.execution.env_vars[index];

        if (shell_exec_config_set_env_var(cfg, item->name, item->value) != 0)
            return -1;
    }
    return 0;
}

static int shell_v2_copy_start_environment(
    const struct mcp_shell_policy_snapshot *snapshot,
    struct shell_exec_config *cfg)
{
    size_t index;

    for (index = 0; index < snapshot->startup_env_var_count; index++) {
        const struct mcp_shell_policy_env_var *item = &snapshot->startup_env_vars[index];

        if (shell_exec_config_set_env_var(cfg, item->name, item->value) != 0)
            return -1;
    }
    return 0;
}

static int shell_exec_config_from_v2_control(
    const struct mcp_shell_sandbox_control *control,
    struct shell_exec_config *cfg)
{
    uint64_t value;

    if (!control || !control->policy_snapshot)
        return -1;
    memset(cfg, 0, sizeof(*cfg));
    cfg->config_loaded = true;
    cfg->sandbox_revision = control->revision;
    cfg->sandbox_enabled = control->sandbox_enabled;
    cfg->shell_enabled = shell_v2_effective_bool(control, MCP_SHELL_POLICY_FIELD_SHELL_ENABLED);
    cfg->enabled = cfg->shell_enabled;
    cfg->capture_stderr =
        shell_v2_effective_bool(control, MCP_SHELL_POLICY_FIELD_CAPTURE_STDERR);
    cfg->merge_stderr =
        cfg->capture_stderr &&
        shell_v2_effective_bool(control, MCP_SHELL_POLICY_FIELD_MERGE_STDERR);
    cfg->clear_environment = true;
    cfg->request_cwd_allowed =
        shell_v2_effective_bool(control, MCP_SHELL_POLICY_FIELD_REQUEST_CWD_ALLOWED);
    cfg->request_env_allowed =
        shell_v2_effective_bool(control, MCP_SHELL_POLICY_FIELD_REQUEST_ENV_ALLOWED);
    cfg->kill_process_group_on_timeout =
        shell_v2_effective_bool(control, MCP_SHELL_POLICY_FIELD_KILL_PROCESS_GROUP);
    cfg->require_non_root =
        shell_v2_effective_bool(control, MCP_SHELL_POLICY_FIELD_REQUIRE_NON_ROOT);
    cfg->mode = shell_v2_effective_mode(control) == MCP_SHELL_POLICY_MODE_EXEC
                    ? SHELL_EXEC_MODE_EXEC
                    : SHELL_EXEC_MODE_SHELL;

    value = shell_v2_effective_uint64(control, MCP_SHELL_POLICY_FIELD_COMMAND_LENGTH);
    if (value > UINT_MAX)
        goto fail;
    cfg->max_command_length = (unsigned int)value;
    value = shell_v2_effective_uint64(control, MCP_SHELL_POLICY_FIELD_TIMEOUT_MS);
    if (value > UINT_MAX)
        goto fail;
    cfg->default_timeout_ms = (unsigned int)value;
    cfg->max_timeout_ms = (unsigned int)value;
    value = shell_v2_effective_uint64(control, MCP_SHELL_POLICY_FIELD_OUTPUT_BYTES);
    if (value > UINT_MAX)
        goto fail;
    cfg->max_output_bytes = (unsigned int)value;
    value = shell_v2_effective_uint64(control, MCP_SHELL_POLICY_FIELD_READ_CHUNK_SIZE);
    if (value > UINT_MAX)
        goto fail;
    cfg->chunk_size = (unsigned int)value;
    cfg->max_cpu_seconds =
        shell_v2_effective_uint64(control, MCP_SHELL_POLICY_FIELD_CPU_SECONDS);
    cfg->max_memory_bytes =
        shell_v2_effective_uint64(control, MCP_SHELL_POLICY_FIELD_MEMORY_BYTES);
    cfg->max_file_size_bytes =
        shell_v2_effective_uint64(control, MCP_SHELL_POLICY_FIELD_FILE_SIZE_BYTES);
    cfg->max_open_files =
        shell_v2_effective_uint64(control, MCP_SHELL_POLICY_FIELD_OPEN_FILES);
    cfg->max_processes =
        shell_v2_effective_uint64(control, MCP_SHELL_POLICY_FIELD_PROCESSES);
    shell_v2_effective_string_bounds(control,
                                     MCP_SHELL_POLICY_FIELD_WORKING_DIRECTORY,
                                     &cfg->working_directory_min_bytes,
                                     &cfg->working_directory_max_bytes);
    cfg->environment_max_items = control->sandbox_enabled
                                     ? control->policy_snapshot->environment_max_items
                                     : MCP_SHELL_POLICY_ENV_HARD_MAX_ITEMS;
    cfg->environment_item_max_bytes =
        control->sandbox_enabled ? control->policy_snapshot->environment_item_max_bytes
                                 : MCP_SHELL_POLICY_ENV_HARD_ITEM_MAX_BYTES;

    cfg->shell_path =
        mcp_strdup(shell_v2_effective_string(control, MCP_SHELL_POLICY_FIELD_SHELL_PATH));
    cfg->shell_arg =
        mcp_strdup(shell_v2_effective_string(control, MCP_SHELL_POLICY_FIELD_SHELL_ARG));
    cfg->working_directory = mcp_strdup(
        shell_v2_effective_string(control, MCP_SHELL_POLICY_FIELD_WORKING_DIRECTORY));
    cfg->run_as_user =
        mcp_strdup(shell_v2_effective_string(control, MCP_SHELL_POLICY_FIELD_RUN_AS_USER));
    cfg->run_as_group =
        mcp_strdup(shell_v2_effective_string(control, MCP_SHELL_POLICY_FIELD_RUN_AS_GROUP));
    cfg->config_path = mcp_strdup(control->policy_snapshot->config_path
                                      ? control->policy_snapshot->config_path
                                      : "(hard_profile)");
    if (!cfg->shell_path || !cfg->shell_arg || !cfg->working_directory || !cfg->run_as_user ||
        !cfg->run_as_group || !cfg->config_path)
        goto fail;
    if (shell_v2_effective_bool(control, MCP_SHELL_POLICY_FIELD_INHERIT_ENV) &&
        shell_v2_copy_start_environment(control->policy_snapshot, cfg) != 0)
        goto fail;
    if (shell_v2_copy_effective_environment(control, cfg) != 0)
        goto fail;

#ifdef _WIN32
    cfg->require_non_root = false;
    cfg->max_cpu_seconds = 0u;
    cfg->max_memory_bytes = 0u;
    cfg->max_file_size_bytes = 0u;
    cfg->max_open_files = 0u;
    cfg->max_processes = 0u;
    cfg->run_as_user[0] = '\0';
    cfg->run_as_group[0] = '\0';
#endif
    return 0;

fail:
    shell_exec_config_destroy(cfg);
    return -1;
}

static int shell_exec_apply_request_overrides(
    struct shell_exec_config *cfg,
    const struct mcp_tool_invocation *invocation,
    char **out_error)
{
    json_t *cwd = json_object_get(invocation->arguments, "cwd");
    json_t *env = json_object_get(invocation->arguments, "env");

    *out_error = NULL;
    if (cwd) {
        const char *value;
        size_t length;

        if (!cfg->request_cwd_allowed) {
            *out_error = mcp_strdup("Invalid params: cwd is disabled by the effective v2 policy.");
            return *out_error ? 0 : -1;
        }
        if (!json_is_string(cwd)) {
            *out_error = mcp_strdup("Invalid params: cwd must be a string.");
            return *out_error ? 0 : -1;
        }
        value = json_string_value(cwd);
        length = json_string_length(cwd);
        if (strlen(value) != length || length < cfg->working_directory_min_bytes ||
            length > cfg->working_directory_max_bytes) {
            *out_error = mcp_strdup("Invalid params: cwd is outside the effective byte bounds.");
            return *out_error ? 0 : -1;
        }
        if (dup_string_field(&cfg->working_directory, value) != 0)
            return -1;
    }
    if (env) {
        const char *name;
        json_t *value;

        if (!cfg->request_env_allowed) {
            *out_error = mcp_strdup("Invalid params: env is disabled by the effective v2 policy.");
            return *out_error ? 0 : -1;
        }
        if (!json_is_object(env)) {
            *out_error = mcp_strdup("Invalid params: env must be an object of string values.");
            return *out_error ? 0 : -1;
        }
        json_object_foreach(env, name, value) {
            const char *text;

            if (!shell_exec_env_name_is_valid(name) || !json_is_string(value)) {
                *out_error =
                    mcp_strdup("Invalid params: env must contain valid names and string values.");
                return *out_error ? 0 : -1;
            }
            text = json_string_value(value);
            if (strlen(text) != json_string_length(value)) {
                *out_error = mcp_strdup("Invalid params: env values must not contain NUL.");
                return *out_error ? 0 : -1;
            }
            if (shell_exec_config_set_env_var(cfg, name, text) != 0)
                return -1;
        }
        if (!shell_exec_environment_is_valid(cfg)) {
            *out_error =
                mcp_strdup("Invalid params: final environment exceeds the effective bounds.");
            return *out_error ? 0 : -1;
        }
    }
    return 0;
}

static int shell_exec_buffer_append(struct shell_exec_buffer *buffer,
                                    const char *data,
                                    size_t len,
                                    size_t limit)
{
    size_t remain;
    size_t copy_len;

    if (!buffer || !data || len == 0)
        return 0;

    if (buffer->len >= limit) {
        buffer->truncated = true;
        return 0;
    }

    remain = limit - buffer->len;
    copy_len = len > remain ? remain : len;
    if (shell_exec_buffer_reserve(buffer, buffer->len + copy_len + 1) != 0)
        return -1;

    memcpy(buffer->data + buffer->len, data, copy_len);
    buffer->len += copy_len;
    buffer->data[buffer->len] = '\0';
    if (copy_len < len)
        buffer->truncated = true;

    return 0;
}

#ifdef _WIN32
static bool shell_exec_windows_is_valid_utf8(const char *data, size_t len)
{
    if (!data || len == 0)
        return true;
    if (len > (size_t)INT_MAX)
        return false;

    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data, (int)len, NULL, 0) > 0;
}

static int shell_exec_windows_convert_codepage_to_utf8(const char *data,
                                                       size_t len,
                                                       UINT code_page,
                                                       char **out_utf8,
                                                       size_t *out_len,
                                                       size_t *out_consumed)
{
    const DWORD flags = code_page == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0;
    size_t trim;

    *out_utf8 = NULL;
    *out_len = 0;
    *out_consumed = 0;

    if (!data || len == 0) {
        *out_utf8 = mcp_strdup("");
        return *out_utf8 ? 0 : -1;
    }
    if (len > (size_t)INT_MAX)
        return -1;

    for (trim = 0; trim <= 4 && trim < len; trim++) {
        const int source_len = (int)(len - trim);
        int wide_len;
        int utf8_len;
        WCHAR *wide_buf;
        char *utf8_buf;

        if (source_len <= 0)
            break;

        wide_len = MultiByteToWideChar(code_page, flags, data, source_len, NULL, 0);
        if (wide_len <= 0)
            continue;

        wide_buf = malloc(sizeof(WCHAR) * (size_t)wide_len);
        if (!wide_buf)
            return -1;

        if (MultiByteToWideChar(code_page, flags, data, source_len, wide_buf, wide_len) != wide_len) {
            free(wide_buf);
            continue;
        }

        utf8_len = WideCharToMultiByte(CP_UTF8, 0, wide_buf, wide_len, NULL, 0, NULL, NULL);
        if (utf8_len <= 0) {
            free(wide_buf);
            continue;
        }

        utf8_buf = malloc((size_t)utf8_len + 1u);
        if (!utf8_buf) {
            free(wide_buf);
            return -1;
        }

        if (WideCharToMultiByte(CP_UTF8, 0, wide_buf, wide_len, utf8_buf, utf8_len, NULL, NULL) != utf8_len) {
            free(utf8_buf);
            free(wide_buf);
            continue;
        }

        utf8_buf[utf8_len] = '\0';
        free(wide_buf);
        *out_utf8 = utf8_buf;
        *out_len = (size_t)utf8_len;
        *out_consumed = (size_t)source_len;
        return 0;
    }

    return -1;
}

static int shell_exec_windows_normalize_buffer(struct shell_exec_buffer *buffer)
{
    const UINT code_pages[] = {CP_OEMCP, CP_ACP};
    size_t i;

    if (!buffer || !buffer->data || buffer->len == 0 || shell_exec_windows_is_valid_utf8(buffer->data, buffer->len))
        return 0;

    for (i = 0; i < sizeof(code_pages) / sizeof(code_pages[0]); i++) {
        char *utf8 = NULL;
        size_t utf8_len = 0;
        size_t consumed = 0;
        size_t original_len;

        if (i > 0 && code_pages[i] == code_pages[i - 1])
            continue;
        if (shell_exec_windows_convert_codepage_to_utf8(buffer->data,
                                                        buffer->len,
                                                        code_pages[i],
                                                        &utf8,
                                                        &utf8_len,
                                                        &consumed) != 0)
            continue;

        original_len = buffer->len;
        free(buffer->data);
        buffer->data = utf8;
        buffer->len = utf8_len;
        buffer->cap = utf8_len + 1u;
        if (consumed < original_len)
            buffer->truncated = true;
        return 0;
    }

    return -1;
}

static int shell_exec_windows_normalize_outcome(struct shell_exec_outcome *outcome)
{
    if (!outcome)
        return -1;
    if (shell_exec_windows_normalize_buffer(&outcome->stdout_buf) != 0)
        return -1;
    if (shell_exec_windows_normalize_buffer(&outcome->stderr_buf) != 0)
        return -1;
    return 0;
}
#endif

static void shell_exec_buffer_destroy(struct shell_exec_buffer *buffer)
{
    if (!buffer)
        return;
    free(buffer->data);
    memset(buffer, 0, sizeof(*buffer));
}

static void shell_exec_outcome_destroy(struct shell_exec_outcome *outcome)
{
    if (!outcome)
        return;
    shell_exec_buffer_destroy(&outcome->stdout_buf);
    shell_exec_buffer_destroy(&outcome->stderr_buf);
    free(outcome->spawn_error);
    memset(outcome, 0, sizeof(*outcome));
}

static void shell_exec_request_destroy(struct shell_exec_request *request)
{
    if (!request)
        return;

    free(request->command);
    memset(request, 0, sizeof(*request));
}

static bool command_has_nul_before_len(const char *command, size_t len)
{
    return memchr(command, '\0', len) != NULL;
}

static bool command_has_disallowed_control(const char *command)
{
    const unsigned char *cursor = (const unsigned char *)command;

    while (*cursor) {
        if ((*cursor < 0x20u && *cursor != '\t' && *cursor != '\n' && *cursor != '\r') ||
            *cursor == 0x7fu)
            return true;
        cursor++;
    }

    return false;
}

static int shell_exec_request_parse_command(struct shell_exec_request *request,
                                            const char *command,
                                            size_t command_length,
                                            unsigned int max_command_length,
                                            char **out_error)
{
    *out_error = NULL;

    if (!command || command_length == 0u) {
        *out_error = mcp_strdup("Invalid params: command must be a non-empty string.");
        return 0;
    }

    if (command_length > max_command_length) {
        *out_error = mcp_strdup("Command exceeds configured maximum length.");
        return 0;
    }

    if (command_has_nul_before_len(command, command_length) || command_has_disallowed_control(command)) {
        *out_error = mcp_strdup("Invalid params: command contains unsupported control characters.");
        return 0;
    }

    request->command = malloc(command_length + 1u);
    if (!request->command)
        return -1;
    memcpy(request->command, command, command_length);
    request->command[command_length] = '\0';

    return 0;
}

static bool shell_exec_resolve_timeout(const struct shell_exec_config *cfg,
                                       const struct mcp_tool_invocation *invocation,
                                       unsigned int *out_timeout_ms)
{
    json_t *value = json_object_get(invocation->arguments, "timeout_ms");
    json_int_t parsed;

    if (!value) {
        *out_timeout_ms = cfg->default_timeout_ms;
        return true;
    }
    if (!json_is_integer(value))
        return false;

    parsed = json_integer_value(value);
    if (parsed < (json_int_t)MCP_SHELL_EXEC_MIN_TIMEOUT_MS ||
        parsed > (json_int_t)cfg->max_timeout_ms)
        return false;
    *out_timeout_ms = (unsigned int)parsed;
    return true;
}

static bool shell_job_string_arg(json_t *arguments, const char *name, const char **out)
{
    json_t *value = json_object_get(arguments, name);

    *out = NULL;
    if (!value)
        return true;
    if (!json_is_string(value))
        return false;

    *out = json_string_value(value);
    return true;
}

static bool shell_job_uint_arg(json_t *arguments,
                               const char *name,
                               unsigned int default_value,
                               unsigned int min_value,
                               unsigned int max_value,
                               unsigned int *out)
{
    json_t *value = json_object_get(arguments, name);
    json_int_t raw;

    if (!value) {
        *out = default_value;
        return true;
    }
    if (!json_is_integer(value))
        return false;

    raw = json_integer_value(value);
    if (raw < (json_int_t)min_value || raw > (json_int_t)max_value)
        return false;

    *out = (unsigned int)raw;
    return true;
}

static const char *shell_exec_mode_name(enum shell_exec_mode mode)
{
    return mode == SHELL_EXEC_MODE_EXEC ? "exec" : "shell";
}

static char *shell_exec_join_env_assignment(const char *name, const char *value)
{
    size_t name_len = strlen(name);
    size_t value_len = value ? strlen(value) : 0u;
    char *entry = malloc(name_len + value_len + 2u);

    if (!entry)
        return NULL;
    memcpy(entry, name, name_len);
    entry[name_len] = '=';
    if (value_len > 0u)
        memcpy(entry + name_len + 1u, value, value_len);
    entry[name_len + 1u + value_len] = '\0';
    return entry;
}

static void shell_exec_envp_destroy(char **envp)
{
    size_t i;

    if (!envp)
        return;
    for (i = 0; envp[i]; i++)
        free(envp[i]);
    free(envp);
}

static int shell_exec_envp_append(char ***envp, size_t *count, char *entry)
{
    char **next = realloc(*envp, sizeof(*next) * (*count + 2u));

    if (!next)
        return -1;
    *envp = next;
    (*envp)[*count] = entry;
    (*count)++;
    (*envp)[*count] = NULL;
    return 0;
}

#ifndef _WIN32
extern char **environ;
#endif

static char **shell_exec_build_envp(const struct shell_exec_config *cfg)
{
    char **envp = NULL;
    size_t count = 0;
    size_t i;

    if (!cfg->clear_environment) {
#ifndef _WIN32
        char **current;

        for (current = environ; current && *current; current++) {
            char *entry;
            const char *equals = strchr(*current, '=');
            size_t name_len;
            bool allowed = false;

            if (!equals)
                continue;
            name_len = (size_t)(equals - *current);
            for (i = 0; i < cfg->allowed_env_count; i++) {
                if (strlen(cfg->allowed_env[i]) == name_len &&
                    strncmp(cfg->allowed_env[i], *current, name_len) == 0) {
                    allowed = true;
                    break;
                }
            }
            if (!allowed && cfg->allowed_env_is_set)
                continue;
            entry = mcp_strdup(*current);
            if (!entry || shell_exec_envp_append(&envp, &count, entry) != 0) {
                free(entry);
                shell_exec_envp_destroy(envp);
                return NULL;
            }
        }
#endif
    }

    for (i = 0; i < cfg->env_var_count; i++) {
        char *entry = shell_exec_join_env_assignment(cfg->env_vars[i].name, cfg->env_vars[i].value);

        if (!entry || shell_exec_envp_append(&envp, &count, entry) != 0) {
            free(entry);
            shell_exec_envp_destroy(envp);
            return NULL;
        }
    }

    if (!envp) {
        envp = calloc(1, sizeof(*envp));
        if (!envp)
            return NULL;
    }

    return envp;
}

static void shell_exec_audit_log(const struct shell_exec_config *cfg,
                                 const struct shell_exec_request *request,
                                 const struct shell_exec_outcome *outcome)
{
    char timestamp[48];

    if (!mcp_format_utc_now(timestamp, sizeof(timestamp)))
        strcpy(timestamp, "unknown-time");

    fprintf(stderr,
            "[shell_exec] time=%s config=\"%s\" sandbox_revision=%" JSON_INTEGER_FORMAT
            " sandbox_enabled=%s shell_enabled=%s mode=%s cwd=\"%s\" run_as_user=\"%s\" "
            "run_as_group=\"%s\" timeout_ms=%u command=\"%s\" exit_code=%d signal=%d "
            "timed_out=%s stdout_bytes=%zu stderr_bytes=%zu truncated=%s\n",
            timestamp,
            cfg && cfg->config_path ? cfg->config_path : "",
            cfg ? cfg->sandbox_revision : 0,
            cfg && cfg->sandbox_enabled ? "true" : "false",
            cfg && cfg->shell_enabled ? "true" : "false",
            cfg ? shell_exec_mode_name(cfg->mode) : "unknown",
            cfg && cfg->working_directory ? cfg->working_directory : "",
            cfg && cfg->run_as_user ? cfg->run_as_user : "",
            cfg && cfg->run_as_group ? cfg->run_as_group : "",
            request ? request->timeout_ms : 0u,
            request && request->command ? request->command : "",
            outcome ? outcome->exit_code : -1,
            outcome ? outcome->signal_number : 0,
            outcome && outcome->timed_out ? "true" : "false",
            outcome ? outcome->stdout_buf.len : 0u,
            outcome ? outcome->stderr_buf.len : 0u,
            outcome && (outcome->stdout_buf.truncated || outcome->stderr_buf.truncated) ? "true" : "false");
}

#ifndef _WIN32
static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return -1;
    return 0;
}

static int read_fd_into_buffer(int fd,
                               struct shell_exec_buffer *buffer,
                               size_t limit,
                               char *chunk,
                               size_t chunk_size,
                               bool *reached_eof)
{
    ssize_t nread;

    *reached_eof = false;

    for (;;) {
        nread = read(fd, chunk, chunk_size);
        if (nread > 0) {
            if (shell_exec_buffer_append(buffer, chunk, (size_t)nread, limit) != 0)
                return -1;
            continue;
        }
        if (nread == 0) {
            *reached_eof = true;
            return 0;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        if (errno == EINTR)
            continue;
        return -1;
    }
}

static bool shell_exec_rlimit_value_is_representable(uint64_t value)
{
    rlim_t converted = (rlim_t)value;

    return (uint64_t)converted == value;
}

static int shell_exec_set_rlimit_value(int resource, uint64_t value)
{
    struct rlimit limit;

    if (value == 0u)
        return 0;
    if (!shell_exec_rlimit_value_is_representable(value)) {
        errno = ERANGE;
        return -1;
    }
    limit.rlim_cur = (rlim_t)value;
    limit.rlim_max = (rlim_t)value;
    return setrlimit(resource, &limit);
}

static const char *shell_exec_unrepresentable_rlimit(const struct shell_exec_config *cfg)
{
    if (!shell_exec_rlimit_value_is_representable(cfg->max_cpu_seconds))
        return "limits.cpu_seconds";
#ifdef RLIMIT_AS
    if (!shell_exec_rlimit_value_is_representable(cfg->max_memory_bytes))
        return "limits.memory_bytes";
#endif
    if (!shell_exec_rlimit_value_is_representable(cfg->max_file_size_bytes))
        return "limits.file_size_bytes";
    if (!shell_exec_rlimit_value_is_representable(cfg->max_open_files))
        return "limits.open_files";
#ifdef RLIMIT_NPROC
    if (!shell_exec_rlimit_value_is_representable(cfg->max_processes))
        return "limits.processes";
#endif
    return NULL;
}

static int shell_exec_apply_unix_rlimits(const struct shell_exec_config *cfg)
{
    if (shell_exec_set_rlimit_value(RLIMIT_CPU, cfg->max_cpu_seconds) != 0)
        return -1;
#ifdef RLIMIT_AS
    if (shell_exec_set_rlimit_value(RLIMIT_AS, cfg->max_memory_bytes) != 0)
        return -1;
#endif
    if (shell_exec_set_rlimit_value(RLIMIT_FSIZE, cfg->max_file_size_bytes) != 0)
        return -1;
    if (shell_exec_set_rlimit_value(RLIMIT_NOFILE, cfg->max_open_files) != 0)
        return -1;
#ifdef RLIMIT_NPROC
    if (shell_exec_set_rlimit_value(RLIMIT_NPROC, cfg->max_processes) != 0)
        return -1;
#endif
    return 0;
}

static int shell_exec_apply_unix_identity(const struct shell_exec_config *cfg)
{
    gid_t target_gid = (gid_t)-1;
    uid_t target_uid = (uid_t)-1;

    if (cfg->run_as_group && cfg->run_as_group[0]) {
        struct group *gr = getgrnam(cfg->run_as_group);

        if (!gr)
            return -1;
        target_gid = gr->gr_gid;
    }

    if (cfg->run_as_user && cfg->run_as_user[0]) {
        struct passwd *pw = getpwnam(cfg->run_as_user);

        if (!pw)
            return -1;
        target_uid = pw->pw_uid;
        if (target_gid == (gid_t)-1)
            target_gid = pw->pw_gid;
        if (initgroups(pw->pw_name, target_gid) != 0)
            return -1;
    }

    if (target_gid != (gid_t)-1 && setgid(target_gid) != 0)
        return -1;
    if (target_uid != (uid_t)-1 && setuid(target_uid) != 0)
        return -1;

    if (cfg->require_non_root && geteuid() == 0)
        return -1;

    return 0;
}

static int shell_exec_prepare_working_directory(const struct shell_exec_config *cfg)
{
#ifdef _WIN32
    int rc;

    if (!cfg->working_directory || cfg->working_directory[0] == '\0')
        return -1;
    rc = _mkdir(cfg->working_directory);
    if (rc == 0 || errno == EEXIST)
        return 0;
    return -1;
#else
    struct stat st;

    if (!cfg->working_directory || cfg->working_directory[0] == '\0')
        return -1;
    if (stat(cfg->working_directory, &st) == 0) {
        if (S_ISDIR(st.st_mode))
            return 0;
        errno = ENOTDIR;
        return -1;
    }
    if (mkdir(cfg->working_directory, 0700) == 0 || errno == EEXIST)
        return 0;
    return -1;
#endif
}

static void shell_exec_child_exec(const struct shell_exec_config *cfg,
                                  const struct shell_exec_request *request,
                                  char **envp)
{
    if (cfg->mode == SHELL_EXEC_MODE_EXEC) {
        char *const argv[] = {(char *)request->command, NULL};

        execve(request->command, argv, envp);
    } else if (cfg->shell_arg[0] == '\0') {
        char *const argv[] = {(char *)cfg->shell_path, request->command, NULL};

        execve(cfg->shell_path, argv, envp);
    } else {
        char *const argv[] = {(char *)cfg->shell_path, (char *)cfg->shell_arg, request->command, NULL};

        execve(cfg->shell_path, argv, envp);
    }
}

static int shell_exec_spawn_unix(const struct shell_exec_config *cfg,
                                 const struct shell_exec_request *request,
                                 struct shell_exec_outcome *outcome)
{
    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};
    char *chunk = NULL;
    char **envp = NULL;
    pid_t pid = -1;
    bool stdout_open = false;
    bool stderr_open = false;
    unsigned long long deadline = mcp_now_ms() + request->timeout_ms;
    int wait_status = 0;
    bool child_exited = false;
    bool killed_for_timeout = false;
    const char *unrepresentable_limit = shell_exec_unrepresentable_rlimit(cfg);

    if (unrepresentable_limit) {
        char message[160];

        snprintf(message,
                 sizeof(message),
                 "%s cannot be represented by rlim_t on this platform.",
                 unrepresentable_limit);
        outcome->spawn_error = mcp_strdup(message);
        return -1;
    }

    if (shell_exec_prepare_working_directory(cfg) != 0) {
        char message[512];

        snprintf(message,
                 sizeof(message),
                 "Failed to prepare shell_exec working directory '%s': %s",
                 cfg->working_directory ? cfg->working_directory : "",
                 strerror(errno));
        outcome->spawn_error = mcp_strdup(message);
        return -1;
    }

    if (pipe(stdout_pipe) != 0)
        return -1;
    if (cfg->capture_stderr && pipe(stderr_pipe) != 0) {
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        return -1;
    }
    envp = shell_exec_build_envp(cfg);
    if (!envp) {
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        if (cfg->capture_stderr) {
            close(stderr_pipe[0]);
            close(stderr_pipe[1]);
        }
        return -1;
    }
    chunk = malloc(cfg->chunk_size);
    if (!chunk) {
        shell_exec_envp_destroy(envp);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        if (cfg->capture_stderr) {
            close(stderr_pipe[0]);
            close(stderr_pipe[1]);
        }
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        shell_exec_envp_destroy(envp);
        free(chunk);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        if (cfg->capture_stderr) {
            close(stderr_pipe[0]);
            close(stderr_pipe[1]);
        }
        return -1;
    }

    if (pid == 0) {
        if (cfg->kill_process_group_on_timeout)
            (void)setpgid(0, 0);

        dup2(stdout_pipe[1], STDOUT_FILENO);
        if (cfg->capture_stderr) {
            dup2(cfg->merge_stderr ? stdout_pipe[1] : stderr_pipe[1], STDERR_FILENO);
        }

        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        if (cfg->capture_stderr) {
            close(stderr_pipe[0]);
            close(stderr_pipe[1]);
        }

        if (chdir(cfg->working_directory) != 0) {
            perror("shell_exec chdir");
            _exit(126);
        }
        if (shell_exec_apply_unix_identity(cfg) != 0) {
            perror("shell_exec setuid/setgid");
            _exit(126);
        }
        if (shell_exec_apply_unix_rlimits(cfg) != 0) {
            perror("shell_exec setrlimit");
            _exit(126);
        }

        shell_exec_child_exec(cfg, request, envp);
        perror("shell_exec exec");
        _exit(127);
    }

    if (cfg->kill_process_group_on_timeout)
        (void)setpgid(pid, pid);

    shell_exec_envp_destroy(envp);
    envp = NULL;
    close(stdout_pipe[1]);
    stdout_pipe[1] = -1;
    stdout_open = true;
    if (set_nonblocking(stdout_pipe[0]) != 0)
        goto fail;

    if (cfg->capture_stderr && !cfg->merge_stderr) {
        close(stderr_pipe[1]);
        stderr_pipe[1] = -1;
        stderr_open = true;
        if (set_nonblocking(stderr_pipe[0]) != 0)
            goto fail;
    } else if (cfg->capture_stderr) {
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);
        stderr_pipe[0] = -1;
        stderr_pipe[1] = -1;
    }

    while (stdout_open || stderr_open || !child_exited) {
        struct pollfd fds[2];
        nfds_t nfds = 0;
        int timeout_ms;
        int poll_rc;
        unsigned long long now = mcp_now_ms();

        if (!child_exited) {
            pid_t waited = waitpid(pid, &wait_status, WNOHANG);
            if (waited == pid)
                child_exited = true;
            else if (waited < 0)
                goto fail;
        }

        if (!child_exited && now >= deadline) {
            if (cfg->kill_process_group_on_timeout)
                kill(-pid, SIGKILL);
            else
                kill(pid, SIGKILL);
            outcome->timed_out = true;
            killed_for_timeout = true;
            deadline = now;
        }

        if (stdout_open) {
            fds[nfds].fd = stdout_pipe[0];
            fds[nfds].events = POLLIN | POLLHUP | POLLERR;
            fds[nfds].revents = 0;
            nfds++;
        }
        if (stderr_open) {
            fds[nfds].fd = stderr_pipe[0];
            fds[nfds].events = POLLIN | POLLHUP | POLLERR;
            fds[nfds].revents = 0;
            nfds++;
        }

        timeout_ms = child_exited ? 0 : (deadline > now ? (int)(deadline - now) : 0);
        poll_rc = nfds > 0 ? poll(fds, nfds, timeout_ms) : 0;
        if (poll_rc < 0) {
            if (errno == EINTR)
                continue;
            goto fail;
        }

        if (stdout_open) {
            bool eof = false;
            if (read_fd_into_buffer(stdout_pipe[0],
                                    &outcome->stdout_buf,
                                    cfg->max_output_bytes,
                                    chunk,
                                    cfg->chunk_size,
                                    &eof) != 0)
                goto fail;
            if (eof) {
                close(stdout_pipe[0]);
                stdout_pipe[0] = -1;
                stdout_open = false;
            }
        }
        if (stderr_open) {
            bool eof = false;
            if (read_fd_into_buffer(stderr_pipe[0],
                                    &outcome->stderr_buf,
                                    cfg->max_output_bytes,
                                    chunk,
                                    cfg->chunk_size,
                                    &eof) != 0)
                goto fail;
            if (eof) {
                close(stderr_pipe[0]);
                stderr_pipe[0] = -1;
                stderr_open = false;
            }
        }

        if (killed_for_timeout && !child_exited) {
            pid_t waited = waitpid(pid, &wait_status, 0);
            if (waited == pid)
                child_exited = true;
            else if (waited < 0)
                goto fail;
        }
    }

    if (WIFEXITED(wait_status))
        outcome->exit_code = WEXITSTATUS(wait_status);
    else if (WIFSIGNALED(wait_status))
        outcome->signal_number = WTERMSIG(wait_status);

    free(chunk);
    return 0;

fail:
    if (pid > 0) {
        if (cfg->kill_process_group_on_timeout)
            kill(-pid, SIGKILL);
        else
            kill(pid, SIGKILL);
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
            ;
    }
    if (stdout_pipe[0] >= 0)
        close(stdout_pipe[0]);
    if (stdout_pipe[1] >= 0)
        close(stdout_pipe[1]);
    if (stderr_pipe[0] >= 0)
        close(stderr_pipe[0]);
    if (stderr_pipe[1] >= 0)
        close(stderr_pipe[1]);
    free(chunk);
    shell_exec_envp_destroy(envp);
    return -1;
}
#else
static WCHAR *shell_exec_windows_utf8_to_wide(const char *value);

static int shell_exec_prepare_working_directory(const struct shell_exec_config *cfg)
{
    WCHAR *working_directory;
    int rc;

    if (!cfg->working_directory || cfg->working_directory[0] == '\0')
        return -1;
    working_directory = shell_exec_windows_utf8_to_wide(cfg->working_directory);
    if (!working_directory)
        return -1;
    rc = _wmkdir(working_directory);
    free(working_directory);
    if (rc == 0 || errno == EEXIST)
        return 0;
    return -1;
}

static void shell_exec_free_environment_block(WCHAR *block)
{
    free(block);
}

static WCHAR *shell_exec_windows_utf8_to_wide(const char *value)
{
    int length;
    WCHAR *wide;

    if (!value || strlen(value) > (size_t)INT_MAX)
        return NULL;
    length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, NULL, 0);
    if (length <= 0)
        return NULL;
    wide = malloc(sizeof(*wide) * (size_t)length);
    if (!wide)
        return NULL;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, wide, length) != length) {
        free(wide);
        return NULL;
    }
    return wide;
}

static int shell_exec_windows_env_pointer_compare(const void *left, const void *right)
{
    const struct shell_exec_env_var *const *left_item = left;
    const struct shell_exec_env_var *const *right_item = right;

    return _stricmp((*left_item)->name, (*right_item)->name);
}

static WCHAR *shell_exec_build_environment_block(const struct shell_exec_config *cfg)
{
    const struct shell_exec_env_var **items = NULL;
    size_t total = 1u;
    size_t i;
    WCHAR *block;
    WCHAR *cursor;

    if (cfg->env_var_count > 0u) {
        items = malloc(sizeof(*items) * cfg->env_var_count);
        if (!items)
            return NULL;
        for (i = 0; i < cfg->env_var_count; i++)
            items[i] = &cfg->env_vars[i];
        qsort(items,
              cfg->env_var_count,
              sizeof(*items),
              shell_exec_windows_env_pointer_compare);
    }
    for (i = 0; i < cfg->env_var_count; i++) {
        int name_length = MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, items[i]->name, -1, NULL, 0);
        int value_length = MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, items[i]->value, -1, NULL, 0);

        if (name_length <= 0 || value_length <= 0 ||
            total > SIZE_MAX - (size_t)name_length - (size_t)value_length) {
            free(items);
            return NULL;
        }
        total += (size_t)name_length + (size_t)value_length;
    }

    block = calloc(total, sizeof(*block));
    if (!block) {
        free(items);
        return NULL;
    }

    cursor = block;
    for (i = 0; i < cfg->env_var_count; i++) {
        int name_length = MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, items[i]->name, -1, cursor, (int)(total - (size_t)(cursor - block)));

        if (name_length <= 0)
            goto fail;
        cursor += name_length - 1;
        *cursor++ = L'=';
        {
            int value_length = MultiByteToWideChar(CP_UTF8,
                                                   MB_ERR_INVALID_CHARS,
                                                   items[i]->value,
                                                   -1,
                                                   cursor,
                                                   (int)(total - (size_t)(cursor - block)));

            if (value_length <= 0)
                goto fail;
            cursor += value_length;
        }
    }
    *cursor = L'\0';
    free(items);
    return block;

fail:
    free(items);
    free(block);
    return NULL;
}

static char *shell_exec_build_windows_command_line(const struct shell_exec_config *cfg,
                                                   const struct shell_exec_request *request)
{
    const char *prefix = cfg->mode == SHELL_EXEC_MODE_EXEC ? "" : cfg->shell_path;
    const char *arg = cfg->mode == SHELL_EXEC_MODE_EXEC ? "" : cfg->shell_arg;
    size_t total = strlen(prefix) + strlen(arg) + strlen(request->command) + 8u;
    char *command_line = malloc(total);

    if (!command_line)
        return NULL;
    if (cfg->mode == SHELL_EXEC_MODE_EXEC)
        snprintf(command_line, total, "%s", request->command);
    else if (arg[0] == '\0')
        snprintf(command_line, total, "\"%s\" %s", prefix, request->command);
    else
        snprintf(command_line, total, "\"%s\" %s %s", prefix, arg, request->command);
    return command_line;
}

static int shell_exec_spawn_windows(const struct shell_exec_config *cfg,
                                    const struct shell_exec_request *request,
                                    struct shell_exec_outcome *outcome)
{
    SECURITY_ATTRIBUTES attrs;
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    HANDLE stdout_read = NULL;
    HANDLE stdout_write = NULL;
    HANDLE stderr_read = NULL;
    HANDLE stderr_write = NULL;
    HANDLE job = NULL;
    char *chunk = NULL;
    char *command_line_utf8 = NULL;
    WCHAR *command_line = NULL;
    WCHAR *working_directory = NULL;
    WCHAR *environment_block = NULL;
    bool process_started = false;
    unsigned long long deadline = mcp_now_ms() + request->timeout_ms;

    if (shell_exec_prepare_working_directory(cfg) != 0) {
        char message[512];

        snprintf(message,
                 sizeof(message),
                 "Failed to prepare shell_exec working directory '%s': %s",
                 cfg->working_directory ? cfg->working_directory : "",
                 strerror(errno));
        outcome->spawn_error = mcp_strdup(message);
        return -1;
    }

    if ((cfg->run_as_user && cfg->run_as_user[0]) || (cfg->run_as_group && cfg->run_as_group[0]) ||
        cfg->require_non_root) {
        outcome->spawn_error = mcp_strdup(
            "Windows low-privilege token execution is not implemented; remove run_as_user/run_as_group or run this server under a restricted account.");
        return -1;
    }

    memset(&attrs, 0, sizeof(attrs));
    memset(&startup, 0, sizeof(startup));
    memset(&process, 0, sizeof(process));

    attrs.nLength = sizeof(attrs);
    attrs.bInheritHandle = TRUE;

    if (!CreatePipe(&stdout_read, &stdout_write, &attrs, 0))
        return -1;
    SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);

    if (cfg->capture_stderr && !cfg->merge_stderr) {
        if (!CreatePipe(&stderr_read, &stderr_write, &attrs, 0))
            goto fail;
        SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);
    }

    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = stdout_write;
    startup.hStdError = cfg->capture_stderr ? (cfg->merge_stderr ? stdout_write : stderr_write)
                                            : GetStdHandle(STD_ERROR_HANDLE);

    chunk = malloc(cfg->chunk_size);
    if (!chunk)
        goto fail;

    command_line_utf8 = shell_exec_build_windows_command_line(cfg, request);
    if (!command_line_utf8)
        goto fail;
    command_line = shell_exec_windows_utf8_to_wide(command_line_utf8);
    working_directory = shell_exec_windows_utf8_to_wide(cfg->working_directory);
    if (!command_line || !working_directory)
        goto fail;

    environment_block = shell_exec_build_environment_block(cfg);
    if (!environment_block) {
        outcome->spawn_error =
            mcp_strdup("Failed to serialize the final Windows environment block.");
        goto fail;
    }

    if (!CreateProcessW(NULL,
                        command_line,
                        NULL,
                        NULL,
                        TRUE,
                        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                        environment_block,
                        working_directory,
                        &startup,
                        &process))
        goto fail;
    process_started = true;

    job = CreateJobObjectA(NULL, NULL);
    if (!job)
        goto fail;
    if (!AssignProcessToJobObject(job, process.hProcess))
        goto fail;

    CloseHandle(stdout_write);
    stdout_write = NULL;
    if (cfg->capture_stderr && !cfg->merge_stderr) {
        CloseHandle(stderr_write);
        stderr_write = NULL;
    }

    for (;;) {
        DWORD wait_rc = WaitForSingleObject(process.hProcess, 10);
        DWORD available = 0;
        DWORD read_count = 0;
        bool any_read = false;

        if (PeekNamedPipe(stdout_read, NULL, 0, NULL, &available, NULL) && available > 0) {
            DWORD to_read = available < cfg->chunk_size ? available : cfg->chunk_size;

            if (!ReadFile(stdout_read, chunk, to_read, &read_count, NULL))
                goto fail;
            if (read_count > 0 &&
                shell_exec_buffer_append(&outcome->stdout_buf,
                                         chunk,
                                         (size_t)read_count,
                                         cfg->max_output_bytes) != 0)
                goto fail;
            any_read = true;
        }
        if (cfg->capture_stderr && !cfg->merge_stderr &&
            PeekNamedPipe(stderr_read, NULL, 0, NULL, &available, NULL) && available > 0) {
            DWORD to_read = available < cfg->chunk_size ? available : cfg->chunk_size;

            if (!ReadFile(stderr_read, chunk, to_read, &read_count, NULL))
                goto fail;
            if (read_count > 0 &&
                shell_exec_buffer_append(&outcome->stderr_buf,
                                         chunk,
                                         (size_t)read_count,
                                         cfg->max_output_bytes) != 0)
                goto fail;
            any_read = true;
        }

        if (wait_rc == WAIT_OBJECT_0 && !any_read)
            break;
        if (wait_rc == WAIT_TIMEOUT && mcp_now_ms() >= deadline) {
            if (job)
                TerminateJobObject(job, 1);
            else
                TerminateProcess(process.hProcess, 1);
            outcome->timed_out = true;
            WaitForSingleObject(process.hProcess, INFINITE);
            break;
        }
        if (wait_rc != WAIT_TIMEOUT && wait_rc != WAIT_OBJECT_0)
            goto fail;
    }

    if (!GetExitCodeProcess(process.hProcess, (DWORD *)&outcome->exit_code))
        goto fail;

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (job)
        CloseHandle(job);
    CloseHandle(stdout_read);
    if (stderr_read)
        CloseHandle(stderr_read);
    free(chunk);
    free(command_line_utf8);
    free(command_line);
    free(working_directory);
    shell_exec_free_environment_block(environment_block);
    return 0;

fail:
    if (process_started) {
        if (job)
            TerminateJobObject(job, 1);
        else
            TerminateProcess(process.hProcess, 1);
    }
    if (process.hThread)
        CloseHandle(process.hThread);
    if (process.hProcess)
        CloseHandle(process.hProcess);
    if (job)
        CloseHandle(job);
    if (stdout_read)
        CloseHandle(stdout_read);
    if (stdout_write)
        CloseHandle(stdout_write);
    if (stderr_read)
        CloseHandle(stderr_read);
    if (stderr_write)
        CloseHandle(stderr_write);
    free(chunk);
    free(command_line_utf8);
    free(command_line);
    free(working_directory);
    shell_exec_free_environment_block(environment_block);
    return -1;
}
#endif

static int shell_exec_spawn(const struct shell_exec_config *cfg,
                            const struct shell_exec_request *request,
                            struct shell_exec_outcome *outcome)
{
#ifdef _WIN32
    return shell_exec_spawn_windows(cfg, request, outcome);
#else
    return shell_exec_spawn_unix(cfg, request, outcome);
#endif
}

static int shell_job_make_id(struct mcp_shell_job_store *store, char **out)
{
    int len;
    char *id;

    *out = NULL;
    len = snprintf(NULL, 0, "job-%llu", ++store->next_job_number);
    if (len < 0)
        return -1;

    id = malloc((size_t)len + 1u);
    if (!id)
        return -1;

    snprintf(id, (size_t)len + 1u, "job-%llu", store->next_job_number);
    *out = id;
    return 0;
}

static void shell_job_mark_finished(struct shell_job *job, enum shell_job_state state)
{
    if (!job || job->finished_ms != 0)
        return;

    job->state = state;
    job->finished_ms = mcp_now_ms();
    if (!mcp_format_utc_now(job->finished_at, sizeof(job->finished_at)))
        snprintf(job->finished_at, sizeof(job->finished_at), "unknown-time");
}

static void shell_job_close_pipes(struct shell_job *job)
{
#ifndef _WIN32
    if (!job)
        return;
    if (job->stdout_fd >= 0) {
        close(job->stdout_fd);
        job->stdout_fd = -1;
    }
    if (job->stderr_fd >= 0) {
        close(job->stderr_fd);
        job->stderr_fd = -1;
    }
    job->stdout_open = false;
    job->stderr_open = false;
#else
    (void)job;
#endif
}

static void shell_job_free(struct shell_job *job)
{
    if (!job)
        return;

    shell_job_close_pipes(job);
    free(job->job_id);
    free(job->label);
    shell_exec_buffer_destroy(&job->stdout_buf);
    shell_exec_buffer_destroy(&job->stderr_buf);
    free(job);
}

static struct shell_job *shell_job_find(struct mcp_shell_job_store *store,
                                        const char *job_id)
{
    struct shell_job *job;

    if (!store || !job_id)
        return NULL;
    for (job = store->jobs; job; job = job->next) {
        if (strcmp(job->job_id, job_id) == 0)
            return job;
    }
    return NULL;
}

static int shell_job_append(struct mcp_shell_job_store *store, struct shell_job *job)
{
    if (shell_job_make_id(store, &job->job_id) != 0)
        return -1;

    job->next = store->jobs;
    store->jobs = job;
    return 0;
}

static bool shell_job_needs_poll(const struct shell_job *job)
{
    return job &&
           (!job->process_reaped || job->stdout_open || job->stderr_open);
}

static void shell_jobs_cleanup_expired(struct mcp_shell_job_store *store)
{
    struct shell_job **current;
    unsigned long long now;

    if (!store)
        return;

    now = mcp_now_ms();
    current = &store->jobs;
    while (*current) {
        struct shell_job *job = *current;

        if (shell_job_is_final(job) &&
            !shell_job_needs_poll(job) &&
            job->finished_ms > 0 &&
            now - job->finished_ms > store->retention_ms) {
            *current = job->next;
            job->next = NULL;
            shell_job_free(job);
            continue;
        }
        current = &(*current)->next;
    }
}

static json_t *shell_job_status_json(const struct shell_job *job)
{
    json_t *payload;

    if (!job)
        return NULL;

    payload = json_pack("{s:s,s:s,s:I,s:I,s:s,s:I,s:I,s:I,s:I,s:I,s:I}",
                        "job_id",
                        job->job_id,
                        "state",
                        shell_job_state_name(job->state),
                        "pid",
                        (json_int_t)job->pid,
                        "process_group_id",
                        (json_int_t)job->process_group_id,
                        "started_at",
                        job->started_at,
                        "timeout_ms",
                        (json_int_t)job->timeout_ms,
                        "output_bytes",
                        (json_int_t)job->output_limit_bytes,
                        "deadline_ms",
                        (json_int_t)job->deadline_ms,
                        "stdout_bytes",
                        (json_int_t)job->stdout_buf.len,
                        "stderr_bytes",
                        (json_int_t)job->stderr_buf.len,
                        "exit_code",
                        (json_int_t)job->exit_code);
    if (!payload)
        return NULL;

    if (job->stdout_buf.truncated &&
        json_object_set_new(payload, "stdout_truncated", json_true()) != 0)
        goto fail;
    if (job->stderr_buf.truncated &&
        json_object_set_new(payload, "stderr_truncated", json_true()) != 0)
        goto fail;
    if (job->signal_number != 0 &&
        json_object_set_new(payload, "signal", json_integer(job->signal_number)) != 0)
        goto fail;
    if (job->label)
        json_object_set_new(payload, "label", json_string(job->label));
    if (job->finished_at[0])
        json_object_set_new(payload, "finished_at", json_string(job->finished_at));

    return payload;

fail:
    json_decref(payload);
    return NULL;
}

static json_t *shell_job_result(const struct shell_job *job, bool is_error)
{
    json_t *payload = shell_job_status_json(job);
    json_t *result;

    if (!payload)
        return mcp_tool_result_text("Failed to encode shell job status.", true);

    result = mcp_tool_result_json_text(payload, is_error);
    json_decref(payload);
    return result;
}

#ifndef _WIN32
static void shell_job_kill_process(struct shell_job *job, int signal_number)
{
    if (!job || job->process_reaped || job->pid <= 0)
        return;

    if (job->kill_process_group)
        kill(-job->pid, signal_number);
    else
        kill(job->pid, signal_number);
}

static int shell_job_spawn_unix(const struct shell_exec_config *cfg,
                                const struct shell_exec_request *request,
                                struct shell_job *job,
                                char **out_error)
{
    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};
    char **envp = NULL;
    pid_t pid;

    *out_error = NULL;

    if (shell_exec_prepare_working_directory(cfg) != 0) {
        char message[512];

        snprintf(message,
                 sizeof(message),
                 "Failed to prepare shell_exec working directory '%s': %s",
                 cfg->working_directory ? cfg->working_directory : "",
                 strerror(errno));
        *out_error = mcp_strdup(message);
        return 0;
    }

    if (pipe(stdout_pipe) != 0)
        return -1;
    if (cfg->capture_stderr && pipe(stderr_pipe) != 0)
        goto fail;

    envp = shell_exec_build_envp(cfg);
    if (!envp)
        goto fail;

    pid = fork();
    if (pid < 0)
        goto fail;

    if (pid == 0) {
        int null_fd;

        if (cfg->kill_process_group_on_timeout)
            (void)setpgid(0, 0);

        null_fd = open("/dev/null", O_RDONLY);
        if (null_fd >= 0) {
            dup2(null_fd, STDIN_FILENO);
            close(null_fd);
        }
        dup2(stdout_pipe[1], STDOUT_FILENO);
        if (cfg->capture_stderr)
            dup2(cfg->merge_stderr ? stdout_pipe[1] : stderr_pipe[1], STDERR_FILENO);

        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        if (cfg->capture_stderr) {
            close(stderr_pipe[0]);
            close(stderr_pipe[1]);
        }

        if (chdir(cfg->working_directory) != 0) {
            perror("shell_job chdir");
            _exit(126);
        }
        if (shell_exec_apply_unix_identity(cfg) != 0) {
            perror("shell_job setuid/setgid");
            _exit(126);
        }
        if (shell_exec_apply_unix_rlimits(cfg) != 0) {
            perror("shell_job setrlimit");
            _exit(126);
        }

        shell_exec_child_exec(cfg, request, envp);
        perror("shell_job exec");
        _exit(127);
    }

    if (cfg->kill_process_group_on_timeout)
        (void)setpgid(pid, pid);

    shell_exec_envp_destroy(envp);
    envp = NULL;

    close(stdout_pipe[1]);
    stdout_pipe[1] = -1;
    job->stdout_fd = stdout_pipe[0];
    stdout_pipe[0] = -1;
    job->stdout_open = true;
    if (set_nonblocking(job->stdout_fd) != 0)
        goto fail_started;

    if (cfg->capture_stderr && !cfg->merge_stderr) {
        close(stderr_pipe[1]);
        stderr_pipe[1] = -1;
        job->stderr_fd = stderr_pipe[0];
        stderr_pipe[0] = -1;
        job->stderr_open = true;
        if (set_nonblocking(job->stderr_fd) != 0)
            goto fail_started;
    } else {
        if (cfg->capture_stderr) {
            close(stderr_pipe[0]);
            close(stderr_pipe[1]);
            stderr_pipe[0] = -1;
            stderr_pipe[1] = -1;
        }
        job->stderr_fd = -1;
    }

    job->pid = pid;
    job->process_group_id = cfg->kill_process_group_on_timeout ? pid : 0;
    job->kill_process_group = cfg->kill_process_group_on_timeout;
    return 0;

fail_started:
    job->pid = pid;
    job->kill_process_group = cfg->kill_process_group_on_timeout;
    shell_job_kill_process(job, SIGKILL);
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
        ;
fail:
    if (stdout_pipe[0] >= 0)
        close(stdout_pipe[0]);
    if (stdout_pipe[1] >= 0)
        close(stdout_pipe[1]);
    if (stderr_pipe[0] >= 0)
        close(stderr_pipe[0]);
    if (stderr_pipe[1] >= 0)
        close(stderr_pipe[1]);
    shell_exec_envp_destroy(envp);
    return -1;
}

static void shell_job_poll_one(struct shell_job *job)
{
    char *chunk;
    unsigned long long now;

    if (!job || !shell_job_needs_poll(job))
        return;

    chunk = malloc(job->chunk_size ? job->chunk_size : MCP_SHELL_EXEC_DEFAULT_CHUNK_SIZE);
    if (!chunk) {
        shell_job_mark_finished(job, SHELL_JOB_FAILED);
        return;
    }

    if (job->stdout_open) {
        bool eof = false;

        if (read_fd_into_buffer(job->stdout_fd,
                                &job->stdout_buf,
                                job->output_limit_bytes,
                                chunk,
                                job->chunk_size,
                                &eof) != 0) {
            shell_job_mark_finished(job, SHELL_JOB_FAILED);
            eof = true;
        }
        if (eof) {
            close(job->stdout_fd);
            job->stdout_fd = -1;
            job->stdout_open = false;
        }
    }

    if (job->stderr_open) {
        bool eof = false;

        if (read_fd_into_buffer(job->stderr_fd,
                                &job->stderr_buf,
                                job->output_limit_bytes,
                                chunk,
                                job->chunk_size,
                                &eof) != 0) {
            shell_job_mark_finished(job, SHELL_JOB_FAILED);
            eof = true;
        }
        if (eof) {
            close(job->stderr_fd);
            job->stderr_fd = -1;
            job->stderr_open = false;
        }
    }

    free(chunk);

    if (!job->process_reaped) {
        int wait_status = 0;
        pid_t waited = waitpid(job->pid, &wait_status, WNOHANG);

        if (waited == job->pid) {
            job->process_reaped = true;
            if (WIFEXITED(wait_status))
                job->exit_code = WEXITSTATUS(wait_status);
            else if (WIFSIGNALED(wait_status))
                job->signal_number = WTERMSIG(wait_status);

            if (job->timed_out_requested)
                shell_job_mark_finished(job, SHELL_JOB_TIMED_OUT);
            else if (job->killed_requested)
                shell_job_mark_finished(job, SHELL_JOB_KILLED);
            else if (job->state == SHELL_JOB_RUNNING)
                shell_job_mark_finished(job, SHELL_JOB_EXITED);
        } else if (waited < 0 && errno != EINTR) {
            job->process_reaped = true;
            shell_job_mark_finished(job, SHELL_JOB_FAILED);
        }
    }

    now = mcp_now_ms();
    if (job->state == SHELL_JOB_RUNNING && now >= job->deadline_ms) {
        job->timed_out_requested = true;
        shell_job_kill_process(job, SIGKILL);
        job->signal_number = SIGKILL;
        shell_job_mark_finished(job, SHELL_JOB_TIMED_OUT);
    }
}
#else
static int shell_job_spawn_unix(const struct shell_exec_config *cfg,
                                const struct shell_exec_request *request,
                                struct shell_job *job,
                                char **out_error)
{
    (void)cfg;
    (void)request;
    (void)job;
    *out_error = mcp_strdup("system.shell_start is not implemented on Windows in this build.");
    return 0;
}

static void shell_job_kill_process(struct shell_job *job, int signal_number)
{
    (void)job;
    (void)signal_number;
}

static void shell_job_poll_one(struct shell_job *job)
{
    (void)job;
}
#endif

static bool shell_jobs_any_needs_poll(const struct mcp_shell_job_store *store)
{
    const struct shell_job *job;

    if (!store)
        return false;

    for (job = store->jobs; job; job = job->next) {
        if (shell_job_needs_poll(job))
            return true;
    }
    return false;
}

static void shell_jobs_poll_all(struct mcp_shell_job_store *store)
{
    struct shell_job *job;

    if (!store)
        return;

    for (job = store->jobs; job; job = job->next)
        shell_job_poll_one(job);
    shell_jobs_cleanup_expired(store);
}

static void shell_jobs_poll_timer_cb(uv_timer_t *timer)
{
    struct mcp_shell_job_store *store = timer->data;

    shell_jobs_poll_all(store);
    if (!shell_jobs_any_needs_poll(store) && store->poll_timer_running) {
        uv_timer_stop(&store->poll_timer);
        store->poll_timer_running = false;
    }
}

static void shell_jobs_ensure_timer(struct mcp_shell_job_store *store)
{
    if (!store || store->shutting_down || !store->poll_timer_initialized || store->poll_timer_running)
        return;

    if (uv_timer_start(&store->poll_timer,
                       shell_jobs_poll_timer_cb,
                       MCP_SHELL_JOB_POLL_MS,
                       MCP_SHELL_JOB_POLL_MS) == 0)
        store->poll_timer_running = true;
}

static void shell_jobs_timer_close_cb(uv_handle_t *handle)
{
    struct mcp_shell_job_store *store = handle->data;

    if (store)
        store->poll_timer_initialized = false;
}

int mcp_shell_jobs_create(struct mcp_shell_job_store **out,
                          struct mcp_server *server,
                          uv_loop_t *loop)
{
    struct mcp_shell_job_store *store;

    *out = NULL;
    if (!server || !loop)
        return -1;

    store = calloc(1, sizeof(*store));
    if (!store)
        return -1;

    store->server = server;
    store->loop = loop;
    store->retention_ms = env_uint("MCP_SHELL_JOB_RETENTION_MS",
                                   MCP_SHELL_JOB_DEFAULT_RETENTION_MS,
                                   1000u,
                                   MCP_SHELL_JOB_MAX_RETENTION_MS);
    if (uv_timer_init(loop, &store->poll_timer) != 0) {
        free(store);
        return -1;
    }
    store->poll_timer_initialized = true;
    store->poll_timer.data = store;

    *out = store;
    return 0;
}

void mcp_shell_jobs_shutdown(struct mcp_shell_job_store *store)
{
    struct shell_job *job;

    if (!store || store->shutting_down)
        return;

    store->shutting_down = true;
    for (job = store->jobs; job; job = job->next) {
        if (!job->process_reaped) {
            job->killed_requested = true;
            shell_job_kill_process(job, SIGKILL);
            shell_job_mark_finished(job, SHELL_JOB_KILLED);
        }
    }
    shell_jobs_poll_all(store);

    if (store->poll_timer_initialized && !uv_is_closing((uv_handle_t *)&store->poll_timer)) {
        uv_timer_stop(&store->poll_timer);
        store->poll_timer_running = false;
        uv_close((uv_handle_t *)&store->poll_timer, shell_jobs_timer_close_cb);
    }
}

void mcp_shell_jobs_destroy(struct mcp_shell_job_store *store)
{
    struct shell_job *job;

    if (!store)
        return;

    mcp_shell_jobs_shutdown(store);
    job = store->jobs;
    while (job) {
        struct shell_job *next = job->next;

        shell_job_free(job);
        job = next;
    }
    free(store);
}

static json_t *shell_exec_build_result(const struct shell_exec_outcome *outcome,
                                       bool is_error)
{
    json_t *payload = json_object();
#ifdef _WIN32
    json_t *stdout_value = json_stringn(outcome->stdout_buf.data ? outcome->stdout_buf.data : "",
                                        outcome->stdout_buf.len);
    json_t *stderr_value = json_stringn(outcome->stderr_buf.data ? outcome->stderr_buf.data : "",
                                        outcome->stderr_buf.len);
#else
    json_t *stdout_value = json_stringn(outcome->stdout_buf.data ? outcome->stdout_buf.data : "",
                                        outcome->stdout_buf.len);
    json_t *stderr_value = json_stringn(outcome->stderr_buf.data ? outcome->stderr_buf.data : "",
                                        outcome->stderr_buf.len);
#endif
    json_t *result;

    if (!payload || !stdout_value || !stderr_value)
        goto fail;

    if (json_object_set_new(payload, "stdout", stdout_value) != 0)
        goto fail_detach_stdout;
    stdout_value = NULL;
    if (json_object_set_new(payload, "stderr", stderr_value) != 0)
        goto fail_detach_stderr;
    stderr_value = NULL;
    if (json_object_set_new(payload, "exit_code", json_integer(outcome->exit_code)) != 0)
        goto fail;
    if (outcome->timed_out &&
        json_object_set_new(payload, "timed_out", json_true()) != 0)
        goto fail;
    if ((outcome->stdout_buf.truncated || outcome->stderr_buf.truncated) &&
        json_object_set_new(payload, "truncated", json_true()) != 0)
        goto fail;
    if (outcome->stdout_buf.truncated &&
        json_object_set_new(payload, "stdout_truncated", json_true()) != 0)
        goto fail;
    if (outcome->stderr_buf.truncated &&
        json_object_set_new(payload, "stderr_truncated", json_true()) != 0)
        goto fail;
    if (outcome->signal_number != 0 &&
        json_object_set_new(payload, "signal", json_integer(outcome->signal_number)) != 0)
        goto fail;

    result = mcp_tool_result_json_text(payload, is_error);
    json_decref(payload);
    return result;

fail_detach_stderr:
    json_decref(stderr_value);
    stderr_value = NULL;
fail_detach_stdout:
    json_decref(stdout_value);
    stdout_value = NULL;
fail:
    json_decref(stdout_value);
    json_decref(stderr_value);
    json_decref(payload);
    return mcp_tool_result_text("Failed to encode shell_exec result.", true);
}

int mcp_tool_system_shell_exec(struct mcp_server *server,
                               const struct mcp_tool_invocation *invocation,
                               json_t **out_result)
{
    struct shell_exec_config cfg;
    const struct mcp_shell_sandbox_control *control = server ? server->sandbox_control : NULL;
    struct shell_exec_request request;
    struct shell_exec_outcome outcome;
    json_t *command_value;
    const char *command;
    size_t command_length;
    char *error_message = NULL;
    int rc = -1;

    memset(&request, 0, sizeof(request));
    memset(&outcome, 0, sizeof(outcome));
    memset(&cfg, 0, sizeof(cfg));
    *out_result = NULL;

    if (!control) {
        *out_result = mcp_tool_result_text("Failed to initialize shell sandbox policy.", true);
        goto cleanup;
    }
    if (shell_exec_config_from_v2_control(control, &cfg) != 0) {
        *out_result = mcp_tool_result_text("Failed to build effective shell sandbox policy.", true);
        goto cleanup;
    }

    if (!cfg.shell_enabled) {
        *out_result = mcp_tool_result_text(
            "system.shell_exec is disabled by the effective v2 policy.",
            true);
        rc = 0;
        goto cleanup;
    }

    command_value = json_object_get(invocation->arguments, "command");
    if (!json_is_string(command_value)) {
        *out_result = mcp_tool_result_text("Invalid params: command must be a string.", true);
        rc = 0;
        goto cleanup;
    }

    command = json_string_value(command_value);
    command_length = json_string_length(command_value);
    if (shell_exec_request_parse_command(&request,
                                         command,
                                         command_length,
                                         cfg.max_command_length,
                                         &error_message) != 0) {
        *out_result = mcp_tool_result_text("Failed to parse command.", true);
        goto cleanup;
    }
    if (error_message) {
        *out_result = mcp_tool_result_text(error_message, true);
        rc = 0;
        goto cleanup;
    }

    if (!shell_exec_resolve_timeout(&cfg, invocation, &request.timeout_ms)) {
        *out_result = mcp_tool_result_text(
            "Invalid params: timeout_ms must be an integer from 1 to the current effective maximum.",
            true);
        rc = 0;
        goto cleanup;
    }

    if (shell_exec_apply_request_overrides(&cfg, invocation, &error_message) != 0) {
        *out_result = mcp_tool_result_text("Failed to apply shell_exec request overrides.", true);
        goto cleanup;
    }
    if (error_message) {
        *out_result = mcp_tool_result_text(error_message, true);
        rc = 0;
        goto cleanup;
    }
    if (!shell_exec_environment_is_valid(&cfg)) {
        *out_result = mcp_tool_result_text(
            "Invalid params: final environment exceeds the effective bounds.", true);
        rc = 0;
        goto cleanup;
    }

    if (shell_exec_spawn(&cfg, &request, &outcome) != 0) {
        *out_result = mcp_tool_result_text(outcome.spawn_error ? outcome.spawn_error
                                                               : "Failed to start or monitor command.",
                                           true);
        goto cleanup;
    }

    shell_exec_audit_log(&cfg, &request, &outcome);
#ifdef _WIN32
    if (shell_exec_windows_normalize_outcome(&outcome) != 0) {
        *out_result = mcp_tool_result_text("Failed to normalize shell_exec output as UTF-8.", true);
        goto cleanup;
    }
#endif
    *out_result = shell_exec_build_result(&outcome,
                                          outcome.timed_out || outcome.exit_code != 0 ||
                                              outcome.signal_number != 0);
    rc = 0;

cleanup:
    free(error_message);
    shell_exec_outcome_destroy(&outcome);
    shell_exec_request_destroy(&request);
    shell_exec_config_destroy(&cfg);
    return rc;
}

static bool shell_job_id_arg(const struct mcp_tool_invocation *invocation, const char **out_job_id)
{
    json_t *value = json_object_get(invocation->arguments, "job_id");

    if (!json_is_string(value))
        return false;

    *out_job_id = json_string_value(value);
    return true;
}

int mcp_tool_system_shell_start(struct mcp_server *server,
                                const struct mcp_tool_invocation *invocation,
                                json_t **out_result)
{
#ifdef _WIN32
    (void)invocation;
    if (!server || !server->shell_jobs) {
        *out_result = mcp_tool_result_text("system.shell_start job store is not available.", true);
        return 0;
    }
    *out_result = mcp_tool_result_text(
        "system.shell_start is disabled because async jobs are not implemented on Windows in this build.",
        true);
    return 0;
#else
    struct shell_exec_config cfg;
    const struct mcp_shell_sandbox_control *control = server ? server->sandbox_control : NULL;
    struct shell_exec_request request;
    struct shell_job *job = NULL;
    json_t *command_value;
    const char *command;
    const char *label;
    size_t command_length;
    unsigned int output_limit_bytes;
    char *error_message = NULL;
    int rc = -1;

    memset(&request, 0, sizeof(request));
    memset(&cfg, 0, sizeof(cfg));
    *out_result = NULL;

    if (!server || !server->shell_jobs) {
        *out_result = mcp_tool_result_text("system.shell_start job store is not available.", true);
        return 0;
    }

    if (!control) {
        *out_result = mcp_tool_result_text("Failed to initialize shell sandbox policy.", true);
        goto cleanup;
    }
    if (shell_exec_config_from_v2_control(control, &cfg) != 0) {
        *out_result = mcp_tool_result_text("Failed to build effective shell sandbox policy.", true);
        goto cleanup;
    }

    if (!cfg.shell_enabled) {
        *out_result = mcp_tool_result_text(
            "system.shell_start is disabled by the effective v2 policy.",
            true);
        rc = 0;
        goto cleanup;
    }
    if (json_object_get(invocation->arguments, "args")) {
        *out_result = mcp_tool_result_text("Invalid params: args is not supported by system.shell_start.", true);
        rc = 0;
        goto cleanup;
    }

    command_value = json_object_get(invocation->arguments, "command");
    if (!json_is_string(command_value)) {
        *out_result = mcp_tool_result_text("Invalid params: command must be a string.", true);
        rc = 0;
        goto cleanup;
    }

    command = json_string_value(command_value);
    command_length = json_string_length(command_value);
    if (shell_exec_request_parse_command(&request,
                                         command,
                                         command_length,
                                         cfg.max_command_length,
                                         &error_message) != 0) {
        *out_result = mcp_tool_result_text("Failed to parse command.", true);
        goto cleanup;
    }
    if (error_message) {
        *out_result = mcp_tool_result_text(error_message, true);
        rc = 0;
        goto cleanup;
    }

    if (!shell_job_string_arg(invocation->arguments, "label", &label)) {
        *out_result = mcp_tool_result_text("Invalid params: label must be a string.", true);
        rc = 0;
        goto cleanup;
    }

    if (!shell_exec_resolve_timeout(&cfg, invocation, &request.timeout_ms)) {
        *out_result = mcp_tool_result_text(
            "Invalid params: timeout_ms must be an integer from 1 to the current effective maximum.",
            true);
        rc = 0;
        goto cleanup;
    }
    if (!shell_job_uint_arg(invocation->arguments,
                            "output_limit_bytes",
                            cfg.max_output_bytes,
                            256u,
                            cfg.max_output_bytes,
                            &output_limit_bytes)) {
        char message[160];

        snprintf(message,
                 sizeof(message),
                 "Invalid params: output_limit_bytes must be between 256 and %u for system.shell_start.",
                 cfg.max_output_bytes);
        *out_result = mcp_tool_result_text(message, true);
        rc = 0;
        goto cleanup;
    }

    if (shell_exec_apply_request_overrides(&cfg, invocation, &error_message) != 0) {
        *out_result = mcp_tool_result_text("Failed to apply shell_start request overrides.", true);
        goto cleanup;
    }
    if (error_message) {
        *out_result = mcp_tool_result_text(error_message, true);
        rc = 0;
        goto cleanup;
    }
    if (!shell_exec_environment_is_valid(&cfg)) {
        *out_result = mcp_tool_result_text(
            "Invalid params: final environment exceeds the effective bounds.", true);
        rc = 0;
        goto cleanup;
    }

    job = calloc(1, sizeof(*job));
    if (!job)
        goto cleanup;

#ifndef _WIN32
    job->stdout_fd = -1;
    job->stderr_fd = -1;
#endif
    job->label = label ? mcp_strdup(label) : NULL;
    if (label && !job->label)
        goto cleanup;
    job->state = SHELL_JOB_RUNNING;
    job->timeout_ms = request.timeout_ms;
    job->output_limit_bytes = output_limit_bytes;
    job->chunk_size = cfg.chunk_size;
    job->started_ms = mcp_now_ms();
    job->deadline_ms = job->started_ms + job->timeout_ms;
    job->exit_code = -1;
    if (!mcp_format_utc_now(job->started_at, sizeof(job->started_at)))
        snprintf(job->started_at, sizeof(job->started_at), "unknown-time");

    if (shell_job_spawn_unix(&cfg, &request, job, &error_message) != 0) {
        *out_result = mcp_tool_result_text("Failed to start shell job.", true);
        goto cleanup;
    }
    if (error_message) {
        *out_result = mcp_tool_result_text(error_message, true);
        rc = 0;
        goto cleanup;
    }

    if (shell_job_append(server->shell_jobs, job) != 0) {
#ifndef _WIN32
        shell_job_kill_process(job, SIGKILL);
#endif
        *out_result = mcp_tool_result_text("Failed to allocate shell job id.", true);
        goto cleanup;
    }

    shell_jobs_ensure_timer(server->shell_jobs);
    *out_result = shell_job_result(job, false);
    job = NULL;
    rc = 0;

cleanup:
    if (job)
        shell_job_free(job);
    free(error_message);
    shell_exec_request_destroy(&request);
    shell_exec_config_destroy(&cfg);
    return rc;
#endif
}

int mcp_tool_system_shell_poll(struct mcp_server *server,
                               const struct mcp_tool_invocation *invocation,
                               json_t **out_result)
{
    const char *job_id;
    struct shell_job *job;

    if (!shell_job_id_arg(invocation, &job_id)) {
        *out_result = mcp_tool_result_text("system.shell_poll requires job_id.", true);
        return 0;
    }

    shell_jobs_poll_all(server->shell_jobs);
    job = shell_job_find(server->shell_jobs, job_id);
    if (!job) {
        *out_result = mcp_tool_result_text("Shell job was not found.", true);
        return 0;
    }

    *out_result = shell_job_result(job, false);
    return 0;
}

static json_t *shell_job_tail_payload(struct shell_job *job,
                                      unsigned int stdout_offset,
                                      unsigned int stderr_offset,
                                      unsigned int max_bytes)
{
    size_t stdout_start = stdout_offset > job->stdout_buf.len ? job->stdout_buf.len : stdout_offset;
    size_t stderr_start = stderr_offset > job->stderr_buf.len ? job->stderr_buf.len : stderr_offset;
    size_t stdout_len = job->stdout_buf.len - stdout_start;
    size_t stderr_len = job->stderr_buf.len - stderr_start;
    json_t *payload;
    json_t *stdout_value;
    json_t *stderr_value;

    if (stdout_len > max_bytes)
        stdout_len = max_bytes;
    if (stderr_len > max_bytes)
        stderr_len = max_bytes;

    stdout_value = json_stringn(job->stdout_buf.data ? job->stdout_buf.data + stdout_start : "",
                                stdout_len);
    stderr_value = json_stringn(job->stderr_buf.data ? job->stderr_buf.data + stderr_start : "",
                                stderr_len);
    payload = shell_job_status_json(job);
    if (!payload || !stdout_value || !stderr_value) {
        json_decref(payload);
        json_decref(stdout_value);
        json_decref(stderr_value);
        return NULL;
    }

    json_object_set_new(payload, "stdout_offset", json_integer((json_int_t)stdout_start));
    json_object_set_new(payload, "stderr_offset", json_integer((json_int_t)stderr_start));
    json_object_set_new(payload, "stdout", stdout_value);
    json_object_set_new(payload, "stderr", stderr_value);
    json_object_set_new(payload,
                        "next_stdout_offset",
                        json_integer((json_int_t)(stdout_start + stdout_len)));
    json_object_set_new(payload,
                        "next_stderr_offset",
                        json_integer((json_int_t)(stderr_start + stderr_len)));
    return payload;
}

int mcp_tool_system_shell_tail(struct mcp_server *server,
                               const struct mcp_tool_invocation *invocation,
                               json_t **out_result)
{
    const char *job_id;
    struct shell_job *job;
    unsigned int stdout_offset = 0;
    unsigned int stderr_offset = 0;
    unsigned int max_bytes = 4096;
    json_t *payload;

    if (!shell_job_id_arg(invocation, &job_id)) {
        *out_result = mcp_tool_result_text("system.shell_tail requires job_id.", true);
        return 0;
    }

    shell_jobs_poll_all(server->shell_jobs);
    job = shell_job_find(server->shell_jobs, job_id);
    if (!job) {
        *out_result = mcp_tool_result_text("Shell job was not found.", true);
        return 0;
    }

    if (json_object_get(invocation->arguments, "offset")) {
        *out_result = mcp_tool_result_text("Invalid params: offset is not supported by system.shell_tail.", true);
        return 0;
    }

    if (!shell_job_uint_arg(invocation->arguments,
                            "stdout_offset",
                            0,
                            0,
                            job->output_limit_bytes,
                            &stdout_offset) ||
        !shell_job_uint_arg(invocation->arguments,
                            "stderr_offset",
                            0,
                            0,
                            job->output_limit_bytes,
                            &stderr_offset) ||
        !shell_job_uint_arg(invocation->arguments, "max_bytes", 4096, 1, job->output_limit_bytes, &max_bytes)) {
        *out_result = mcp_tool_result_text(
            "Invalid params: offsets and max_bytes must be non-negative integers within the job output limit.",
            true);
        return 0;
    }

    payload = shell_job_tail_payload(job, stdout_offset, stderr_offset, max_bytes);
    if (!payload) {
        *out_result = mcp_tool_result_text("Failed to encode shell job output.", true);
        return -1;
    }

    *out_result = mcp_tool_result_json_text(payload, false);
    json_decref(payload);
    return 0;
}

int mcp_tool_system_shell_wait(struct mcp_server *server,
                               const struct mcp_tool_invocation *invocation,
                               json_t **out_result)
{
    const char *job_id;
    struct shell_job *job;
    unsigned int timeout_ms = 0;
    json_t *payload;

    if (!shell_job_id_arg(invocation, &job_id)) {
        *out_result = mcp_tool_result_text("system.shell_wait requires job_id.", true);
        return 0;
    }
    if (!shell_job_uint_arg(invocation->arguments,
                            "timeout_ms",
                            0,
                            0,
                            MCP_SHELL_JOB_WAIT_MAX_MS,
                            &timeout_ms)) {
        *out_result = mcp_tool_result_text(
            "Invalid params: timeout_ms must be between 0 and 5000 for system.shell_wait.",
            true);
        return 0;
    }

    shell_jobs_poll_all(server->shell_jobs);
    job = shell_job_find(server->shell_jobs, job_id);
    if (!job) {
        *out_result = mcp_tool_result_text("Shell job was not found.", true);
        return 0;
    }

    payload = shell_job_status_json(job);
    if (!payload) {
        *out_result = mcp_tool_result_text("Failed to encode shell job status.", true);
        return -1;
    }
    json_object_set_new(payload,
                        "wait_result",
                        json_string(shell_job_is_final(job) && !shell_job_needs_poll(job)
                                        ? "finished"
                                        : "still_running"));
    *out_result = mcp_tool_result_json_text(payload, false);
    json_decref(payload);
    return 0;
}

int mcp_tool_system_shell_kill(struct mcp_server *server,
                               const struct mcp_tool_invocation *invocation,
                               json_t **out_result)
{
    const char *job_id;
    struct shell_job *job;
    unsigned int signal_number = 15;

    if (!shell_job_id_arg(invocation, &job_id)) {
        *out_result = mcp_tool_result_text("system.shell_kill requires job_id.", true);
        return 0;
    }
    if (!shell_job_uint_arg(invocation->arguments, "signal", 15, 1, 64, &signal_number)) {
        *out_result = mcp_tool_result_text("Invalid params: signal must be an integer from 1 to 64.", true);
        return 0;
    }

    shell_jobs_poll_all(server->shell_jobs);
    job = shell_job_find(server->shell_jobs, job_id);
    if (!job) {
        *out_result = mcp_tool_result_text("Shell job was not found.", true);
        return 0;
    }

    if (!job->process_reaped) {
        job->killed_requested = true;
#ifndef _WIN32
        shell_job_kill_process(job, (int)signal_number);
#endif
        job->signal_number = (int)signal_number;
        shell_job_mark_finished(job, SHELL_JOB_KILLED);
        shell_jobs_ensure_timer(server->shell_jobs);
        shell_jobs_poll_all(server->shell_jobs);
    }

    *out_result = shell_job_result(job, false);
    return 0;
}

int mcp_tool_system_shell_list(struct mcp_server *server,
                               const struct mcp_tool_invocation *invocation,
                               json_t **out_result)
{
    json_t *payload = json_object();
    json_t *jobs = json_array();
    struct shell_job *job;

    (void)invocation;

    if (!payload || !jobs) {
        json_decref(payload);
        json_decref(jobs);
        *out_result = mcp_tool_result_text("Failed to encode shell job list.", true);
        return -1;
    }

    shell_jobs_poll_all(server->shell_jobs);
    for (job = server->shell_jobs ? server->shell_jobs->jobs : NULL; job; job = job->next) {
        json_t *entry = shell_job_status_json(job);

        if (!entry || json_array_append_new(jobs, entry) != 0) {
            json_decref(entry);
            json_decref(payload);
            json_decref(jobs);
            *out_result = mcp_tool_result_text("Failed to encode shell job list.", true);
            return -1;
        }
    }

    json_object_set_new(payload, "jobs", jobs);
    *out_result = mcp_tool_result_json_text(payload, false);
    json_decref(payload);
    return 0;
}
