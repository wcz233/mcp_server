#include "tools/shell_exec.h"

#include "common/platform.h"
#include "tools/tool_result.h"

#include <ctype.h>
#include <errno.h>
#include <jansson.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
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

#ifndef MCP_SHELL_EXEC_DEFAULT_CONFIG
#define MCP_SHELL_EXEC_DEFAULT_CONFIG "config/tools/shell_exec.json"
#endif

#ifndef MCP_SHELL_EXEC_INSTALLED_CONFIG
#define MCP_SHELL_EXEC_INSTALLED_CONFIG "share/mcp_server/config/tools/shell_exec.json"
#endif

#define MCP_SHELL_EXEC_MIN_COMMAND_LENGTH 64u
#define MCP_SHELL_EXEC_MAX_COMMAND_LENGTH 65535u
#define MCP_SHELL_EXEC_MIN_TIMEOUT_MS 1u
#define MCP_SHELL_EXEC_MAX_TIMEOUT_MS 30000u
#define MCP_SHELL_EXEC_DEFAULT_COMMAND_LENGTH 3500u
#define MCP_SHELL_EXEC_DEFAULT_TIMEOUT_MS 5000u
#define MCP_SHELL_EXEC_DEFAULT_OUTPUT_BYTES 16384u
#define MCP_SHELL_EXEC_DEFAULT_CHUNK_SIZE 1024u
#define MCP_SHELL_EXEC_DEFAULT_CPU_SECONDS 5u
#define MCP_SHELL_EXEC_DEFAULT_MEMORY_BYTES 134217728u
#define MCP_SHELL_EXEC_DEFAULT_FILE_SIZE_BYTES 10485760u
#define MCP_SHELL_EXEC_DEFAULT_OPEN_FILES 64u
#define MCP_SHELL_EXEC_DEFAULT_PROCESSES 16u

enum shell_exec_mode {
    SHELL_EXEC_MODE_SHELL = 0,
    SHELL_EXEC_MODE_EXEC = 1,
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
    bool clear_environment;
    bool kill_process_group_on_timeout;
    bool require_non_root;
    enum shell_exec_mode mode;
    unsigned int max_command_length;
    unsigned int default_timeout_ms;
    unsigned int max_timeout_ms;
    unsigned int max_output_bytes;
    unsigned int chunk_size;
    unsigned int max_cpu_seconds;
    unsigned int max_memory_bytes;
    unsigned int max_file_size_bytes;
    unsigned int max_open_files;
    unsigned int max_processes;
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

static int shell_exec_prepare_working_directory(const struct shell_exec_config *cfg);

static bool env_bool(const char *name, bool default_value)
{
    const char *value = getenv(name);

    if (!value || value[0] == '\0')
        return default_value;
    if (value[0] == '0' || value[0] == 'n' || value[0] == 'N' || value[0] == 'f' ||
        value[0] == 'F')
        return false;
    return true;
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

static int shell_exec_config_set_env_var(struct shell_exec_config *cfg,
                                         const char *name,
                                         const char *value)
{
    struct shell_exec_env_var *next;
    char *name_copy;
    char *value_copy;
    size_t i;

    if (!name || name[0] == '\0' || strchr(name, '='))
        return set_config_error(cfg, "shell_exec env names must be non-empty and must not contain '='.");
    if (!value)
        value = "";

    for (i = 0; i < cfg->env_var_count; i++) {
        if (strcmp(cfg->env_vars[i].name, name) == 0) {
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

static int shell_exec_config_add_allowed_env(struct shell_exec_config *cfg, const char *name)
{
    char **next;
    char *copy;

    if (!name || name[0] == '\0' || strchr(name, '='))
        return set_config_error(cfg, "shell_exec allowed_env entries must be non-empty names.");

    copy = mcp_strdup(name);
    if (!copy)
        return -1;

    next = realloc(cfg->allowed_env, sizeof(*next) * (cfg->allowed_env_count + 1));
    if (!next) {
        free(copy);
        return -1;
    }
    cfg->allowed_env = next;
    cfg->allowed_env[cfg->allowed_env_count++] = copy;
    return 0;
}

static int shell_exec_config_defaults(struct shell_exec_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->enabled = false;
    cfg->capture_stderr = true;
    cfg->merge_stderr = false;
    cfg->clear_environment = true;
    cfg->kill_process_group_on_timeout = true;
    cfg->require_non_root = false;
    cfg->mode = SHELL_EXEC_MODE_SHELL;
    cfg->max_command_length = MCP_SHELL_EXEC_DEFAULT_COMMAND_LENGTH;
    cfg->default_timeout_ms = MCP_SHELL_EXEC_DEFAULT_TIMEOUT_MS;
    cfg->max_timeout_ms = MCP_SHELL_EXEC_MAX_TIMEOUT_MS;
    cfg->max_output_bytes = MCP_SHELL_EXEC_DEFAULT_OUTPUT_BYTES;
    cfg->chunk_size = MCP_SHELL_EXEC_DEFAULT_CHUNK_SIZE;
    cfg->max_cpu_seconds = MCP_SHELL_EXEC_DEFAULT_CPU_SECONDS;
    cfg->max_memory_bytes = MCP_SHELL_EXEC_DEFAULT_MEMORY_BYTES;
    cfg->max_file_size_bytes = MCP_SHELL_EXEC_DEFAULT_FILE_SIZE_BYTES;
    cfg->max_open_files = MCP_SHELL_EXEC_DEFAULT_OPEN_FILES;
    cfg->max_processes = MCP_SHELL_EXEC_DEFAULT_PROCESSES;
#ifdef _WIN32
    cfg->shell_path = mcp_strdup("cmd.exe");
    cfg->shell_arg = mcp_strdup("/C");
    cfg->working_directory = mcp_strdup(".");
    if (shell_exec_config_set_env_var(cfg, "PATH", "%SystemRoot%\\System32;%SystemRoot%") != 0)
        return -1;
    if (shell_exec_config_set_env_var(cfg, "SystemRoot", "C:\\Windows") != 0)
        return -1;
#else
    cfg->shell_path = mcp_strdup("/bin/sh");
    cfg->shell_arg = mcp_strdup("-c");
    cfg->working_directory = mcp_strdup("/tmp/mcp-shell");
    if (shell_exec_config_set_env_var(cfg, "PATH", "/usr/bin:/bin") != 0)
        return -1;
    if (shell_exec_config_set_env_var(cfg, "HOME", "/tmp/mcp-shell") != 0)
        return -1;
    if (shell_exec_config_set_env_var(cfg, "LANG", "C") != 0)
        return -1;
#endif
    if (!cfg->shell_path || !cfg->shell_arg || !cfg->working_directory)
        return -1;
    return 0;
}

static int shell_exec_config_parse_json(struct shell_exec_config *cfg, const json_t *root)
{
    json_t *execution;
    json_t *limits;
    json_t *isolation;
    json_t *value;

    if (!json_is_object(root))
        return set_config_error(cfg, "shell_exec config root must be a JSON object.");

    value = json_object_get(root, "enabled");
    if (json_is_boolean(value))
        cfg->enabled = json_is_true(value);

    value = json_object_get(root, "capture_stderr");
    if (json_is_boolean(value))
        cfg->capture_stderr = json_is_true(value);

    value = json_object_get(root, "merge_stderr");
    if (json_is_boolean(value))
        cfg->merge_stderr = json_is_true(value);

    value = json_object_get(root, "max_command_length");
    if (json_is_integer(value))
        cfg->max_command_length =
            clamp_uint((unsigned long)json_integer_value(value),
                       cfg->max_command_length,
                       MCP_SHELL_EXEC_MIN_COMMAND_LENGTH,
                       MCP_SHELL_EXEC_MAX_COMMAND_LENGTH);

    value = json_object_get(root, "default_timeout_ms");
    if (json_is_integer(value))
        cfg->default_timeout_ms =
            clamp_uint((unsigned long)json_integer_value(value),
                       cfg->default_timeout_ms,
                       MCP_SHELL_EXEC_MIN_TIMEOUT_MS,
                       MCP_SHELL_EXEC_MAX_TIMEOUT_MS);

    value = json_object_get(root, "max_timeout_ms");
    if (json_is_integer(value))
        cfg->max_timeout_ms = clamp_uint((unsigned long)json_integer_value(value),
                                         cfg->max_timeout_ms,
                                         MCP_SHELL_EXEC_MIN_TIMEOUT_MS,
                                         MCP_SHELL_EXEC_MAX_TIMEOUT_MS);

    value = json_object_get(root, "max_output_bytes");
    if (json_is_integer(value))
        cfg->max_output_bytes =
            clamp_uint((unsigned long)json_integer_value(value), cfg->max_output_bytes, 256u, 1048576u);

    value = json_object_get(root, "chunk_size");
    if (json_is_integer(value))
        cfg->chunk_size =
            clamp_uint((unsigned long)json_integer_value(value), cfg->chunk_size, 64u, 65536u);

    execution = json_object_get(root, "execution");
    if (execution) {
        if (!json_is_object(execution))
            return set_config_error(cfg, "shell_exec execution must be a JSON object.");

        value = json_object_get(execution, "mode");
        if (json_is_string(value)) {
            const char *mode = json_string_value(value);
            if (strcmp(mode, "shell") == 0) {
                cfg->mode = SHELL_EXEC_MODE_SHELL;
            } else if (strcmp(mode, "exec") == 0) {
                cfg->mode = SHELL_EXEC_MODE_EXEC;
            } else {
                return set_config_error(cfg, "shell_exec execution.mode must be 'shell' or 'exec'.");
            }
        }

        value = json_object_get(execution, "shell_path");
        if (json_is_string(value) && dup_string_field(&cfg->shell_path, json_string_value(value)) != 0)
            return -1;

        value = json_object_get(execution, "shell_arg");
        if (json_is_string(value) && dup_string_field(&cfg->shell_arg, json_string_value(value)) != 0)
            return -1;

        value = json_object_get(execution, "working_directory");
        if (json_is_string(value) && dup_string_field(&cfg->working_directory, json_string_value(value)) != 0)
            return -1;

        value = json_object_get(execution, "clear_environment");
        if (json_is_boolean(value))
            cfg->clear_environment = json_is_true(value);

        value = json_object_get(execution, "kill_process_group_on_timeout");
        if (json_is_boolean(value))
            cfg->kill_process_group_on_timeout = json_is_true(value);

        value = json_object_get(execution, "run_as_user");
        if (json_is_string(value) && dup_string_field(&cfg->run_as_user, json_string_value(value)) != 0)
            return -1;

        value = json_object_get(execution, "run_as_group");
        if (json_is_string(value) && dup_string_field(&cfg->run_as_group, json_string_value(value)) != 0)
            return -1;

        value = json_object_get(execution, "allowed_env");
        if (value) {
            size_t index;
            json_t *item;

            if (!json_is_array(value))
                return set_config_error(cfg, "shell_exec execution.allowed_env must be an array of strings.");
            shell_exec_allowed_env_destroy(cfg);
            json_array_foreach(value, index, item) {
                if (!json_is_string(item))
                    return set_config_error(cfg, "shell_exec execution.allowed_env must be an array of strings.");
                if (shell_exec_config_add_allowed_env(cfg, json_string_value(item)) != 0)
                    return -1;
            }
        }

        if (value) {
            /* keep clang-tidy quiet about value lifetime in old CMake presets */
        }

        value = json_object_get(execution, "env");
        if (value) {
            const char *key;
            json_t *item;

            if (!json_is_object(value))
                return set_config_error(cfg, "shell_exec execution.env must be a JSON object.");
            shell_exec_env_vars_destroy(cfg);
            json_object_foreach(value, key, item) {
                if (!json_is_string(item))
                    return set_config_error(cfg, "shell_exec execution.env values must be strings.");
                if (shell_exec_config_set_env_var(cfg, key, json_string_value(item)) != 0)
                    return -1;
            }
        }
    }

    limits = json_object_get(root, "limits");
    if (limits) {
        if (!json_is_object(limits))
            return set_config_error(cfg, "shell_exec limits must be a JSON object.");

        value = json_object_get(limits, "max_cpu_seconds");
        if (json_is_integer(value))
            cfg->max_cpu_seconds =
                clamp_uint((unsigned long)json_integer_value(value), cfg->max_cpu_seconds, 0u, 3600u);

        value = json_object_get(limits, "max_memory_bytes");
        if (json_is_integer(value))
            cfg->max_memory_bytes =
                clamp_uint((unsigned long)json_integer_value(value), cfg->max_memory_bytes, 0u, 2147483647u);

        value = json_object_get(limits, "max_file_size_bytes");
        if (json_is_integer(value))
            cfg->max_file_size_bytes =
                clamp_uint((unsigned long)json_integer_value(value), cfg->max_file_size_bytes, 0u, 2147483647u);

        value = json_object_get(limits, "max_open_files");
        if (json_is_integer(value))
            cfg->max_open_files =
                clamp_uint((unsigned long)json_integer_value(value), cfg->max_open_files, 0u, 1048576u);

        value = json_object_get(limits, "max_processes");
        if (json_is_integer(value))
            cfg->max_processes =
                clamp_uint((unsigned long)json_integer_value(value), cfg->max_processes, 0u, 1048576u);
    }

    isolation = json_object_get(root, "isolation");
    if (isolation) {
        if (!json_is_object(isolation))
            return set_config_error(cfg, "shell_exec isolation must be a JSON object.");

        value = json_object_get(isolation, "require_non_root");
        if (json_is_boolean(value))
            cfg->require_non_root = json_is_true(value);
    }

    if (cfg->default_timeout_ms > cfg->max_timeout_ms)
        cfg->default_timeout_ms = cfg->max_timeout_ms;
    if (!cfg->capture_stderr)
        cfg->merge_stderr = false;
    if (!cfg->shell_path || cfg->shell_path[0] == '\0')
        return set_config_error(cfg, "shell_exec execution.shell_path must not be empty.");
    if (!cfg->shell_arg || cfg->shell_arg[0] == '\0')
        return set_config_error(cfg, "shell_exec execution.shell_arg must not be empty.");
    if (!cfg->working_directory || cfg->working_directory[0] == '\0')
        return set_config_error(cfg, "shell_exec execution.working_directory must not be empty.");

    return 0;
}

static const char *shell_exec_env_config_path(void)
{
    const char *env_path = getenv("MCP_SHELL_EXEC_CONFIG");

    if (env_path && env_path[0] != '\0')
        return env_path;
    return NULL;
}

static int shell_exec_config_load(struct shell_exec_config *cfg)
{
    const char *path = NULL;
    const char *env_path;
    const char *candidates[] = {
        MCP_SHELL_EXEC_DEFAULT_CONFIG,
        MCP_SHELL_EXEC_INSTALLED_CONFIG,
    };
    size_t index;
    json_error_t error;
    json_t *root = NULL;

    if (shell_exec_config_defaults(cfg) != 0)
        return -1;

    env_path = shell_exec_env_config_path();
    if (env_path) {
        path = env_path;
        root = json_load_file(path, JSON_REJECT_DUPLICATES, &error);
    } else {
        for (index = 0; index < sizeof(candidates) / sizeof(candidates[0]); index++) {
            root = json_load_file(candidates[index], JSON_REJECT_DUPLICATES, &error);
            if (root) {
                path = candidates[index];
                break;
            }
        }
        if (!path)
            path = "(defaults)";
    }

    cfg->config_path = mcp_strdup(path);
    if (!cfg->config_path)
        return -1;

    if (!root && env_path) {
        char buffer[512];

        snprintf(buffer,
                 sizeof(buffer),
                 "Failed to load shell_exec config from %s: %s",
                 path,
                 error.text[0] ? error.text : "unknown error");
        set_config_error(cfg, buffer);
        return 0;
    }

    if (root) {
        if (shell_exec_config_parse_json(cfg, root) != 0) {
            json_decref(root);
            return 0;
        }
        json_decref(root);
    }

    cfg->enabled = env_bool("MCP_ENABLE_SHELL_EXEC", cfg->enabled);
    cfg->capture_stderr = env_bool("MCP_SHELL_CAPTURE_STDERR", cfg->capture_stderr);
    cfg->merge_stderr = env_bool("MCP_SHELL_MERGE_STDERR", cfg->merge_stderr);
    cfg->clear_environment = env_bool("MCP_SHELL_CLEAR_ENVIRONMENT", cfg->clear_environment);
    cfg->kill_process_group_on_timeout =
        env_bool("MCP_SHELL_KILL_PROCESS_GROUP_ON_TIMEOUT", cfg->kill_process_group_on_timeout);
    cfg->require_non_root = env_bool("MCP_SHELL_REQUIRE_NON_ROOT", cfg->require_non_root);
    cfg->max_command_length = env_uint("MCP_SHELL_MAX_COMMAND_LENGTH",
                                       cfg->max_command_length,
                                       MCP_SHELL_EXEC_MIN_COMMAND_LENGTH,
                                       MCP_SHELL_EXEC_MAX_COMMAND_LENGTH);
    cfg->default_timeout_ms = env_uint("MCP_SHELL_EXEC_TIMEOUT_MS",
                                       cfg->default_timeout_ms,
                                       MCP_SHELL_EXEC_MIN_TIMEOUT_MS,
                                       MCP_SHELL_EXEC_MAX_TIMEOUT_MS);
    cfg->max_timeout_ms = env_uint("MCP_SHELL_MAX_TIMEOUT_MS",
                                   cfg->max_timeout_ms,
                                   MCP_SHELL_EXEC_MIN_TIMEOUT_MS,
                                   MCP_SHELL_EXEC_MAX_TIMEOUT_MS);
    cfg->max_output_bytes =
        env_uint("MCP_SHELL_OUTPUT_LIMIT", cfg->max_output_bytes, 256u, 1048576u);
    cfg->chunk_size = env_uint("MCP_SHELL_CHUNK_SIZE", cfg->chunk_size, 64u, 65536u);
    cfg->max_cpu_seconds =
        env_uint("MCP_SHELL_MAX_CPU_SECONDS", cfg->max_cpu_seconds, 0u, 3600u);
    cfg->max_memory_bytes =
        env_uint("MCP_SHELL_MAX_MEMORY_BYTES", cfg->max_memory_bytes, 0u, 2147483647u);
    cfg->max_file_size_bytes =
        env_uint("MCP_SHELL_MAX_FILE_SIZE_BYTES", cfg->max_file_size_bytes, 0u, 2147483647u);
    cfg->max_open_files =
        env_uint("MCP_SHELL_MAX_OPEN_FILES", cfg->max_open_files, 0u, 1048576u);
    cfg->max_processes =
        env_uint("MCP_SHELL_MAX_PROCESSES", cfg->max_processes, 0u, 1048576u);

    env_path = getenv("MCP_SHELL_PATH");
    if (env_path && env_path[0] != '\0' && dup_string_field(&cfg->shell_path, env_path) != 0)
        return -1;
    env_path = getenv("MCP_SHELL_ARG");
    if (env_path && env_path[0] != '\0' && dup_string_field(&cfg->shell_arg, env_path) != 0)
        return -1;
    env_path = getenv("MCP_SHELL_WORKING_DIRECTORY");
    if (env_path && env_path[0] != '\0' && dup_string_field(&cfg->working_directory, env_path) != 0)
        return -1;
    env_path = getenv("MCP_SHELL_RUN_AS_USER");
    if (env_path && dup_string_field(&cfg->run_as_user, env_path[0] ? env_path : NULL) != 0)
        return -1;
    env_path = getenv("MCP_SHELL_RUN_AS_GROUP");
    if (env_path && dup_string_field(&cfg->run_as_group, env_path[0] ? env_path : NULL) != 0)
        return -1;

    if (cfg->default_timeout_ms > cfg->max_timeout_ms)
        cfg->default_timeout_ms = cfg->max_timeout_ms;
    if (!cfg->capture_stderr)
        cfg->merge_stderr = false;

    cfg->config_loaded = true;
    return 0;
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

static unsigned int shell_exec_resolve_timeout(const struct shell_exec_config *cfg,
                                               const struct mcp_tool_invocation *invocation)
{
    json_t *value = json_object_get(invocation->arguments, "timeout_ms");
    unsigned long parsed;

    if (!json_is_integer(value))
        return cfg->default_timeout_ms;

    parsed = (unsigned long)json_integer_value(value);
    return clamp_uint(parsed, cfg->default_timeout_ms, MCP_SHELL_EXEC_MIN_TIMEOUT_MS, cfg->max_timeout_ms);
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
            if (!allowed && cfg->allowed_env_count > 0)
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
            "[shell_exec] time=%s config=\"%s\" mode=%s cwd=\"%s\" run_as_user=\"%s\" run_as_group=\"%s\" timeout_ms=%u command=\"%s\" exit_code=%d signal=%d timed_out=%s stdout_bytes=%zu stderr_bytes=%zu truncated=%s\n",
            timestamp,
            cfg && cfg->config_path ? cfg->config_path : "",
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

static int shell_exec_set_rlimit_value(int resource, unsigned int value)
{
    struct rlimit limit;

    if (value == 0u)
        return 0;
    limit.rlim_cur = (rlim_t)value;
    limit.rlim_max = (rlim_t)value;
    return setrlimit(resource, &limit);
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
    if (cfg->run_as_user && cfg->run_as_user[0] &&
        shell_exec_set_rlimit_value(RLIMIT_NPROC, cfg->max_processes) != 0)
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
            (void)setsid();

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
static int shell_exec_prepare_working_directory(const struct shell_exec_config *cfg)
{
    int rc;

    if (!cfg->working_directory || cfg->working_directory[0] == '\0')
        return -1;
    rc = _mkdir(cfg->working_directory);
    if (rc == 0 || errno == EEXIST)
        return 0;
    return -1;
}

static void shell_exec_free_environment_block(char *block)
{
    free(block);
}

static char *shell_exec_build_environment_block(const struct shell_exec_config *cfg)
{
    size_t total = 1u;
    size_t i;
    char *block;
    char *cursor;

    for (i = 0; i < cfg->env_var_count; i++)
        total += strlen(cfg->env_vars[i].name) + strlen(cfg->env_vars[i].value) + 2u;

    block = calloc(total, 1u);
    if (!block)
        return NULL;

    cursor = block;
    for (i = 0; i < cfg->env_var_count; i++) {
        size_t name_len = strlen(cfg->env_vars[i].name);
        size_t value_len = strlen(cfg->env_vars[i].value);

        memcpy(cursor, cfg->env_vars[i].name, name_len);
        cursor += name_len;
        *cursor++ = '=';
        memcpy(cursor, cfg->env_vars[i].value, value_len);
        cursor += value_len;
        *cursor++ = '\0';
    }
    *cursor = '\0';
    return block;
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
    else
        snprintf(command_line, total, "\"%s\" %s %s", prefix, arg, request->command);
    return command_line;
}

static int shell_exec_spawn_windows(const struct shell_exec_config *cfg,
                                    const struct shell_exec_request *request,
                                    struct shell_exec_outcome *outcome)
{
    SECURITY_ATTRIBUTES attrs;
    STARTUPINFOA startup;
    PROCESS_INFORMATION process;
    HANDLE stdout_read = NULL;
    HANDLE stdout_write = NULL;
    HANDLE stderr_read = NULL;
    HANDLE stderr_write = NULL;
    HANDLE job = NULL;
    char *chunk = NULL;
    char *command_line = NULL;
    char *environment_block = NULL;
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

    command_line = shell_exec_build_windows_command_line(cfg, request);
    if (!command_line)
        goto fail;

    environment_block = shell_exec_build_environment_block(cfg);
    if (!environment_block)
        goto fail;

    if (!CreateProcessA(NULL,
                        command_line,
                        NULL,
                        NULL,
                        TRUE,
                        CREATE_NO_WINDOW,
                        cfg->clear_environment ? environment_block : NULL,
                        cfg->working_directory,
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
    free(command_line);
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
    free(command_line);
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

static json_t *shell_exec_build_result(const struct shell_exec_request *request,
                                       const struct shell_exec_outcome *outcome,
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

    if (json_object_set_new(payload, "command", json_string(request->command ? request->command : "")) != 0)
        goto fail;
    if (json_object_set_new(payload, "stdout", stdout_value) != 0)
        goto fail_detach_stdout;
    stdout_value = NULL;
    if (json_object_set_new(payload, "stderr", stderr_value) != 0)
        goto fail_detach_stderr;
    stderr_value = NULL;
    if (json_object_set_new(payload, "exit_code", json_integer(outcome->exit_code)) != 0)
        goto fail;
    if (json_object_set_new(payload, "timed_out", json_boolean(outcome->timed_out)) != 0)
        goto fail;
    if (json_object_set_new(payload, "truncated",
                            json_boolean(outcome->stdout_buf.truncated || outcome->stderr_buf.truncated)) != 0)
        goto fail;
    if (json_object_set_new(payload, "signal", json_integer(outcome->signal_number)) != 0)
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

json_t *mcp_shell_exec_input_schema(void)
{
    return json_pack("{s:s,s:{s:{s:s,s:s},s:{s:s,s:s}},s:[s]}",
                     "type",
                     "object",
                     "properties",
                     "command",
                     "type",
                     "string",
                     "description",
                     "Command string passed to the configured OS-isolated shell executor without content filtering.",
                     "timeout_ms",
                     "type",
                     "integer",
                     "description",
                     "Optional timeout override in milliseconds.",
                     "required",
                     "command");
}

int mcp_tool_system_shell_exec(struct mcp_server *server,
                               const struct mcp_tool_invocation *invocation,
                               json_t **out_result)
{
    struct shell_exec_config cfg;
    struct shell_exec_request request;
    struct shell_exec_outcome outcome;
    json_t *command_value;
    const char *command;
    size_t command_length;
    char *error_message = NULL;
    int rc = -1;

    (void)server;

    memset(&request, 0, sizeof(request));
    memset(&outcome, 0, sizeof(outcome));
    memset(&cfg, 0, sizeof(cfg));
    *out_result = NULL;

    if (shell_exec_config_load(&cfg) != 0) {
        *out_result = mcp_tool_result_text("Failed to initialize shell_exec configuration.", true);
        goto cleanup;
    }

    if (!cfg.config_loaded) {
        char message[512];

        snprintf(message,
                 sizeof(message),
                 "system.shell_exec is disabled because configuration failed to load from %s. %s",
                 cfg.config_path ? cfg.config_path : "(unknown)",
                 cfg.load_error ? cfg.load_error : "No details available.");
        *out_result = mcp_tool_result_text(message, true);
        rc = 0;
        goto cleanup;
    }

    if (!cfg.enabled) {
        *out_result = mcp_tool_result_text(
            "system.shell_exec is disabled by policy. Enable it in shell_exec.json or MCP_ENABLE_SHELL_EXEC=1 for a trusted session.",
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

    request.timeout_ms = shell_exec_resolve_timeout(&cfg, invocation);

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
    *out_result = shell_exec_build_result(&request,
                                          &outcome,
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

uint32_t mcp_shell_exec_registration_timeout_ms(void)
{
    struct shell_exec_config cfg;
    uint32_t timeout_ms = MCP_SHELL_EXEC_DEFAULT_TIMEOUT_MS;

    memset(&cfg, 0, sizeof(cfg));
    if (shell_exec_config_load(&cfg) == 0 && cfg.config_loaded)
        timeout_ms = cfg.default_timeout_ms;
    shell_exec_config_destroy(&cfg);
    return timeout_ms;
}
