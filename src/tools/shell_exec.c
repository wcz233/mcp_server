#include "tools/shell_exec.h"

#include "common/platform.h"
#include "core/server_internal.h"
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
    unsigned int max_cpu_seconds;
    unsigned int max_memory_bytes;
    unsigned int max_file_size_bytes;
    unsigned int max_open_files;
    unsigned int max_processes;
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
    char *command;
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
    char *token;
    size_t token_length;
    json_int_t revision;
    bool sandbox_enabled;
    bool shell_enabled_is_set;
    bool shell_enabled;
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

static void shell_sandbox_token_zero(char *token, size_t token_length)
{
    volatile unsigned char *bytes = (volatile unsigned char *)token;
    size_t i;

    for (i = 0; i < token_length; i++)
        bytes[i] = 0;
}

int mcp_shell_sandbox_control_create(struct mcp_shell_sandbox_control **out)
{
    const char *enable_value;
    const char *token;
    struct mcp_shell_sandbox_control *control;

    if (!out)
        return -1;
    *out = NULL;

    enable_value = getenv("MCP_ENABLE_SANDBOX_CTL");
    if (enable_value && enable_value[0] != '\0' &&
        strcmp(enable_value, "0") != 0 && strcmp(enable_value, "1") != 0) {
        fputs("MCP_ENABLE_SANDBOX_CTL must be unset, empty, 0, or 1\n", stderr);
        return -1;
    }

    control = calloc(1, sizeof(*control));
    if (!control)
        return -1;

    control->overrides = json_object();
    if (!control->overrides) {
        free(control);
        return -1;
    }
    control->sandbox_enabled = true;

    control->enabled = enable_value && strcmp(enable_value, "1") == 0;
    if (control->enabled) {
        token = getenv("MCP_SANDBOX_CTL_TOKEN");
        if (!token || token[0] == '\0') {
            fputs("MCP_SANDBOX_CTL_TOKEN must be non-empty when MCP_ENABLE_SANDBOX_CTL=1\n",
                  stderr);
            json_decref(control->overrides);
            free(control);
            return -1;
        }

        control->token = mcp_strdup(token);
        if (!control->token) {
            json_decref(control->overrides);
            free(control);
            return -1;
        }
        control->token_length = strlen(token);
    }

    *out = control;
    return 0;
}

void mcp_shell_sandbox_control_destroy(struct mcp_shell_sandbox_control *control)
{
    if (!control)
        return;

    shell_sandbox_token_zero(control->token, control->token_length);
    free(control->token);
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

static bool shell_exec_env_name_is_valid(const char *name)
{
    return name && name[0] != '\0' && !strchr(name, '=');
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
    cfg->sandbox_enabled = true;
    cfg->capture_stderr = true;
    cfg->merge_stderr = false;
    cfg->clear_environment = true;
    cfg->kill_process_group_on_timeout = true;
    cfg->request_cwd_allowed = true;
    cfg->request_env_allowed = true;
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

        value = json_object_get(execution, "request_cwd_allowed");
        if (json_is_boolean(value))
            cfg->request_cwd_allowed = json_is_true(value);

        value = json_object_get(execution, "request_env_allowed");
        if (json_is_boolean(value))
            cfg->request_env_allowed = json_is_true(value);

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
            cfg->allowed_env_is_set = cfg->allowed_env_count > 0u;
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
    size_t actual_length;
    size_t difference;
    size_t index;

    if (!control || !control->enabled || !json_is_string(provided))
        return false;

    actual = (const unsigned char *)json_string_value(provided);
    actual_length = json_string_length(provided);
    difference = actual_length ^ control->token_length;
    for (index = 0; index < control->token_length; index++) {
        unsigned char actual_byte = index < actual_length ? actual[index] : 0u;

        difference |= (size_t)(actual_byte ^ (unsigned char)control->token[index]);
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
                      shell_sandbox_key_equals(key, key_length, "shell_enabled") ||
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

static enum shell_sandbox_patch_status shell_sandbox_patch_boolean(
    json_t *target,
    const char *key,
    const json_t *value,
    const char *field,
    bool unsupported,
    struct shell_sandbox_patch_error *error)
{
    if (!json_is_null(value) && !json_is_boolean(value)) {
        error->field = field;
        return SHELL_SANDBOX_PATCH_INVALID;
    }
    if (!json_is_null(value) && unsupported) {
        error->field = field;
        return SHELL_SANDBOX_PATCH_UNSUPPORTED;
    }
    return shell_sandbox_store_patch_value(target, key, value);
}

static enum shell_sandbox_patch_status shell_sandbox_patch_integer(
    json_t *target,
    const char *key,
    const json_t *value,
    json_int_t minimum,
    json_int_t maximum,
    const char *field,
    bool unsupported,
    struct shell_sandbox_patch_error *error)
{
    json_int_t integer;

    if (json_is_null(value))
        return shell_sandbox_store_patch_value(target, key, value);
    if (!json_is_integer(value)) {
        error->field = field;
        return SHELL_SANDBOX_PATCH_INVALID;
    }
    integer = json_integer_value(value);
    if (integer < minimum || integer > maximum) {
        error->field = field;
        return SHELL_SANDBOX_PATCH_INVALID;
    }
    if (unsupported) {
        error->field = field;
        return SHELL_SANDBOX_PATCH_UNSUPPORTED;
    }
    return shell_sandbox_store_patch_value(target, key, value);
}

static enum shell_sandbox_patch_status shell_sandbox_patch_string(
    json_t *target,
    const char *key,
    const json_t *value,
    size_t minimum_length,
    size_t maximum_length,
    const char *field,
    bool unsupported,
    bool allow_empty_when_unsupported,
    struct shell_sandbox_patch_error *error)
{
    size_t length;

    if (json_is_null(value))
        return shell_sandbox_store_patch_value(target, key, value);
    if (!json_is_string(value)) {
        error->field = field;
        return SHELL_SANDBOX_PATCH_INVALID;
    }
    length = json_string_length(value);
    if (strlen(json_string_value(value)) != length || length < minimum_length ||
        (maximum_length > 0u && length > maximum_length)) {
        error->field = field;
        return SHELL_SANDBOX_PATCH_INVALID;
    }
    if (unsupported && !(allow_empty_when_unsupported && length == 0u)) {
        error->field = field;
        return SHELL_SANDBOX_PATCH_UNSUPPORTED;
    }
    return shell_sandbox_store_patch_value(target, key, value);
}

static enum shell_sandbox_patch_status shell_sandbox_patch_allowed_env(
    json_t *target,
    const json_t *value,
    struct shell_sandbox_patch_error *error)
{
    size_t index;
    json_t *item;

    if (json_is_null(value))
        return shell_sandbox_store_patch_value(target, "allowed_env", value);
    if (!json_is_array(value) || json_array_size(value) > MCP_SANDBOX_CTL_MAX_ALLOWED_ENV_ITEMS) {
        error->field = "execution.allowed_env";
        return SHELL_SANDBOX_PATCH_INVALID;
    }
    json_array_foreach(value, index, item) {
        const char *name;
        size_t length;

        if (!json_is_string(item)) {
            error->field = "execution.allowed_env";
            return SHELL_SANDBOX_PATCH_INVALID;
        }
        name = json_string_value(item);
        length = json_string_length(item);
        if (length == 0u || length > MCP_SANDBOX_CTL_MAX_ALLOWED_ENV_NAME_BYTES ||
            strlen(name) != length || strchr(name, '=')) {
            error->field = "execution.allowed_env";
            return SHELL_SANDBOX_PATCH_INVALID;
        }
    }
#ifdef _WIN32
    error->field = "execution.allowed_env";
    return SHELL_SANDBOX_PATCH_UNSUPPORTED;
#else
    return shell_sandbox_store_patch_value(target, "allowed_env", value);
#endif
}

static enum shell_sandbox_patch_status shell_sandbox_record_path(json_t *changed_paths,
                                                                 const char *field)
{
    if (json_array_append_new(changed_paths, json_string(field)) != 0)
        return SHELL_SANDBOX_PATCH_INTERNAL;
    return SHELL_SANDBOX_PATCH_OK;
}

static enum shell_sandbox_patch_status shell_sandbox_patch_top_leaf(
    json_t *overrides,
    const char *key,
    size_t key_length,
    const json_t *value,
    struct shell_sandbox_patch_error *error,
    const char **field)
{
    if (shell_sandbox_key_equals(key, key_length, "max_command_length")) {
        *field = "max_command_length";
        return shell_sandbox_patch_integer(overrides,
                                           "max_command_length",
                                           value,
                                           64,
                                           65535,
                                           *field,
                                           false,
                                           error);
    }
    if (shell_sandbox_key_equals(key, key_length, "default_timeout_ms")) {
        *field = "default_timeout_ms";
        return shell_sandbox_patch_integer(overrides,
                                           "default_timeout_ms",
                                           value,
                                           1,
                                           MCP_SANDBOX_CTL_MAX_TIMEOUT_MS,
                                           *field,
                                           false,
                                           error);
    }
    if (shell_sandbox_key_equals(key, key_length, "max_timeout_ms")) {
        *field = "max_timeout_ms";
        return shell_sandbox_patch_integer(overrides,
                                           "max_timeout_ms",
                                           value,
                                           1,
                                           MCP_SANDBOX_CTL_MAX_TIMEOUT_MS,
                                           *field,
                                           false,
                                           error);
    }
    if (shell_sandbox_key_equals(key, key_length, "max_output_bytes")) {
        *field = "max_output_bytes";
        return shell_sandbox_patch_integer(overrides,
                                           "max_output_bytes",
                                           value,
                                           256,
                                           MCP_SANDBOX_CTL_MAX_OUTPUT_BYTES,
                                           *field,
                                           false,
                                           error);
    }
    if (shell_sandbox_key_equals(key, key_length, "capture_stderr")) {
        *field = "capture_stderr";
        return shell_sandbox_patch_boolean(
            overrides, "capture_stderr", value, *field, false, error);
    }
    if (shell_sandbox_key_equals(key, key_length, "merge_stderr")) {
        *field = "merge_stderr";
        return shell_sandbox_patch_boolean(
            overrides, "merge_stderr", value, *field, false, error);
    }

    error->field = "overrides";
    return SHELL_SANDBOX_PATCH_INVALID;
}

static enum shell_sandbox_patch_status shell_sandbox_patch_execution_leaf(
    json_t *group,
    const char *key,
    size_t key_length,
    const json_t *value,
    struct shell_sandbox_patch_error *error,
    const char **field)
{
#ifdef _WIN32
    const bool unix_only = true;
#else
    const bool unix_only = false;
#endif

    if (shell_sandbox_key_equals(key, key_length, "working_directory")) {
        *field = "execution.working_directory";
        return shell_sandbox_patch_string(group,
                                          "working_directory",
                                          value,
                                          1u,
                                          MCP_SANDBOX_CTL_MAX_WORKING_DIRECTORY_BYTES,
                                          *field,
                                          false,
                                          false,
                                          error);
    }
    if (shell_sandbox_key_equals(key, key_length, "request_cwd_allowed")) {
        *field = "execution.request_cwd_allowed";
        return shell_sandbox_patch_boolean(
            group, "request_cwd_allowed", value, *field, unix_only, error);
    }
    if (shell_sandbox_key_equals(key, key_length, "clear_environment")) {
        *field = "execution.clear_environment";
        return shell_sandbox_patch_boolean(
            group, "clear_environment", value, *field, false, error);
    }
    if (shell_sandbox_key_equals(key, key_length, "allowed_env")) {
        *field = "execution.allowed_env";
        return shell_sandbox_patch_allowed_env(group, value, error);
    }
    if (shell_sandbox_key_equals(key, key_length, "request_env_allowed")) {
        *field = "execution.request_env_allowed";
        return shell_sandbox_patch_boolean(
            group, "request_env_allowed", value, *field, unix_only, error);
    }
    if (shell_sandbox_key_equals(key, key_length, "kill_process_group_on_timeout")) {
        *field = "execution.kill_process_group_on_timeout";
        return shell_sandbox_patch_boolean(
            group, "kill_process_group_on_timeout", value, *field, unix_only, error);
    }
    if (shell_sandbox_key_equals(key, key_length, "run_as_user")) {
        *field = "execution.run_as_user";
        return shell_sandbox_patch_string(
            group, "run_as_user", value, 0u, 0u, *field, unix_only, true, error);
    }
    if (shell_sandbox_key_equals(key, key_length, "run_as_group")) {
        *field = "execution.run_as_group";
        return shell_sandbox_patch_string(
            group, "run_as_group", value, 0u, 0u, *field, unix_only, true, error);
    }

    error->field = "overrides.execution";
    return SHELL_SANDBOX_PATCH_INVALID;
}

static enum shell_sandbox_patch_status shell_sandbox_patch_limits_leaf(
    json_t *group,
    const char *key,
    size_t key_length,
    const json_t *value,
    struct shell_sandbox_patch_error *error,
    const char **field)
{
#ifdef _WIN32
    const bool unsupported = true;
#else
    const bool unsupported = false;
#endif

    if (shell_sandbox_key_equals(key, key_length, "max_cpu_seconds")) {
        *field = "limits.max_cpu_seconds";
        return shell_sandbox_patch_integer(
            group, "max_cpu_seconds", value, 0, 3600, *field, unsupported, error);
    }
    if (shell_sandbox_key_equals(key, key_length, "max_memory_bytes")) {
        *field = "limits.max_memory_bytes";
        return shell_sandbox_patch_integer(
            group, "max_memory_bytes", value, 0, 2147483647, *field, unsupported, error);
    }
    if (shell_sandbox_key_equals(key, key_length, "max_file_size_bytes")) {
        *field = "limits.max_file_size_bytes";
        return shell_sandbox_patch_integer(group,
                                           "max_file_size_bytes",
                                           value,
                                           0,
                                           2147483647,
                                           *field,
                                           unsupported,
                                           error);
    }
    if (shell_sandbox_key_equals(key, key_length, "max_open_files")) {
        *field = "limits.max_open_files";
        return shell_sandbox_patch_integer(
            group, "max_open_files", value, 0, 1048576, *field, unsupported, error);
    }
    if (shell_sandbox_key_equals(key, key_length, "max_processes")) {
        *field = "limits.max_processes";
        return shell_sandbox_patch_integer(
            group, "max_processes", value, 0, 1048576, *field, unsupported, error);
    }

    error->field = "overrides.limits";
    return SHELL_SANDBOX_PATCH_INVALID;
}

static enum shell_sandbox_patch_status shell_sandbox_patch_isolation_leaf(
    json_t *group,
    const char *key,
    size_t key_length,
    const json_t *value,
    struct shell_sandbox_patch_error *error,
    const char **field)
{
    if (shell_sandbox_key_equals(key, key_length, "require_non_root")) {
        *field = "isolation.require_non_root";
        return shell_sandbox_patch_boolean(
            group, "require_non_root", value, *field, false, error);
    }

    error->field = "overrides.isolation";
    return SHELL_SANDBOX_PATCH_INVALID;
}

static enum shell_sandbox_patch_status shell_sandbox_apply_group_patch(
    json_t *overrides,
    const char *group_name,
    const json_t *patch,
    json_t *changed_paths,
    size_t *leaf_count,
    struct shell_sandbox_patch_error *error)
{
    json_t *group;
    const char *key;
    size_t key_length;
    json_t *value;
    enum shell_sandbox_patch_status status;

    if (!json_is_object(patch)) {
        error->field = group_name;
        return SHELL_SANDBOX_PATCH_INVALID;
    }
    group = shell_sandbox_override_group(overrides, group_name);
    if (!group)
        return SHELL_SANDBOX_PATCH_INTERNAL;

    json_object_keylen_foreach((json_t *)patch, key, key_length, value) {
        const char *field = NULL;

        if (strcmp(group_name, "execution") == 0) {
            status = shell_sandbox_patch_execution_leaf(
                group, key, key_length, value, error, &field);
        } else if (strcmp(group_name, "limits") == 0) {
            status = shell_sandbox_patch_limits_leaf(group, key, key_length, value, error, &field);
        } else {
            status = shell_sandbox_patch_isolation_leaf(
                group, key, key_length, value, error, &field);
        }
        if (status != SHELL_SANDBOX_PATCH_OK)
            return status;
        status = shell_sandbox_record_path(changed_paths, field);
        if (status != SHELL_SANDBOX_PATCH_OK)
            return status;
        (*leaf_count)++;
    }

    if (json_object_size(group) == 0u)
        json_object_del(overrides, group_name);
    return SHELL_SANDBOX_PATCH_OK;
}

static enum shell_sandbox_patch_status shell_sandbox_apply_overrides_patch(
    json_t *overrides,
    const json_t *patch,
    json_t *changed_paths,
    size_t *leaf_count,
    struct shell_sandbox_patch_error *error)
{
    const char *key;
    size_t key_length;
    json_t *value;

    if (!json_is_object(patch)) {
        error->field = "overrides";
        return SHELL_SANDBOX_PATCH_INVALID;
    }

    json_object_keylen_foreach((json_t *)patch, key, key_length, value) {
        enum shell_sandbox_patch_status status;
        const char *field = NULL;

        if (shell_sandbox_key_equals(key, key_length, "execution")) {
            status = shell_sandbox_apply_group_patch(overrides,
                                                     "execution",
                                                     value,
                                                     changed_paths,
                                                     leaf_count,
                                                     error);
        } else if (shell_sandbox_key_equals(key, key_length, "limits")) {
            status = shell_sandbox_apply_group_patch(
                overrides, "limits", value, changed_paths, leaf_count, error);
        } else if (shell_sandbox_key_equals(key, key_length, "isolation")) {
            status = shell_sandbox_apply_group_patch(overrides,
                                                     "isolation",
                                                     value,
                                                     changed_paths,
                                                     leaf_count,
                                                     error);
        } else {
            status = shell_sandbox_patch_top_leaf(
                overrides, key, key_length, value, error, &field);
            if (status == SHELL_SANDBOX_PATCH_OK) {
                status = shell_sandbox_record_path(changed_paths, field);
                if (status == SHELL_SANDBOX_PATCH_OK)
                    (*leaf_count)++;
            }
        }
        if (status != SHELL_SANDBOX_PATCH_OK)
            return status;
    }
    return SHELL_SANDBOX_PATCH_OK;
}

static int shell_sandbox_config_copy(struct shell_exec_config *destination,
                                     const struct shell_exec_config *source)
{
    size_t index;

    *destination = *source;
    destination->shell_path = NULL;
    destination->shell_arg = NULL;
    destination->working_directory = NULL;
    destination->run_as_user = NULL;
    destination->run_as_group = NULL;
    destination->env_vars = NULL;
    destination->env_var_count = 0u;
    destination->allowed_env = NULL;
    destination->allowed_env_count = 0u;
    destination->config_path = NULL;
    destination->load_error = NULL;

    if (dup_string_field(&destination->shell_path, source->shell_path) != 0 ||
        dup_string_field(&destination->shell_arg, source->shell_arg) != 0 ||
        dup_string_field(&destination->working_directory, source->working_directory) != 0 ||
        dup_string_field(&destination->run_as_user, source->run_as_user) != 0 ||
        dup_string_field(&destination->run_as_group, source->run_as_group) != 0 ||
        dup_string_field(&destination->config_path, source->config_path) != 0 ||
        dup_string_field(&destination->load_error, source->load_error) != 0)
        goto fail;

    for (index = 0; index < source->env_var_count; index++) {
        if (shell_exec_config_set_env_var(destination,
                                          source->env_vars[index].name,
                                          source->env_vars[index].value) != 0)
            goto fail;
    }
    for (index = 0; index < source->allowed_env_count; index++) {
        if (shell_exec_config_add_allowed_env(destination, source->allowed_env[index]) != 0)
            goto fail;
    }
    return 0;

fail:
    shell_exec_config_destroy(destination);
    return -1;
}

static int shell_sandbox_apply_allowed_env(struct shell_exec_config *cfg, const json_t *value)
{
    size_t index;
    json_t *item;

    shell_exec_allowed_env_destroy(cfg);
    cfg->allowed_env_is_set = true;
    json_array_foreach(value, index, item) {
        if (shell_exec_config_add_allowed_env(cfg, json_string_value(item)) != 0)
            return -1;
    }
    return 0;
}

static int shell_sandbox_apply_normalized_overrides(struct shell_exec_config *cfg,
                                                    const json_t *overrides,
                                                    bool shell_enabled_is_set,
                                                    bool shell_enabled)
{
    json_t *execution;
    json_t *limits;
    json_t *isolation;
    json_t *value;

    if (shell_enabled_is_set)
        cfg->enabled = shell_enabled;

#define SHELL_SANDBOX_APPLY_UINT(name, member)                                            \
    do {                                                                                  \
        value = json_object_get(overrides, name);                                         \
        if (json_is_integer(value))                                                       \
            cfg->member = (unsigned int)json_integer_value(value);                        \
    } while (0)
#define SHELL_SANDBOX_APPLY_BOOL(name, member)                                            \
    do {                                                                                  \
        value = json_object_get(overrides, name);                                         \
        if (json_is_boolean(value))                                                       \
            cfg->member = json_is_true(value);                                            \
    } while (0)

    SHELL_SANDBOX_APPLY_UINT("max_command_length", max_command_length);
    SHELL_SANDBOX_APPLY_UINT("default_timeout_ms", default_timeout_ms);
    SHELL_SANDBOX_APPLY_UINT("max_timeout_ms", max_timeout_ms);
    SHELL_SANDBOX_APPLY_UINT("max_output_bytes", max_output_bytes);
    SHELL_SANDBOX_APPLY_BOOL("capture_stderr", capture_stderr);
    SHELL_SANDBOX_APPLY_BOOL("merge_stderr", merge_stderr);

    execution = json_object_get(overrides, "execution");
    if (execution) {
        value = json_object_get(execution, "working_directory");
        if (json_is_string(value) &&
            dup_string_field(&cfg->working_directory, json_string_value(value)) != 0)
            return -1;
        value = json_object_get(execution, "request_cwd_allowed");
        if (json_is_boolean(value))
            cfg->request_cwd_allowed = json_is_true(value);
        value = json_object_get(execution, "clear_environment");
        if (json_is_boolean(value))
            cfg->clear_environment = json_is_true(value);
        value = json_object_get(execution, "allowed_env");
        if (json_is_array(value) && shell_sandbox_apply_allowed_env(cfg, value) != 0)
            return -1;
        value = json_object_get(execution, "request_env_allowed");
        if (json_is_boolean(value))
            cfg->request_env_allowed = json_is_true(value);
        value = json_object_get(execution, "kill_process_group_on_timeout");
        if (json_is_boolean(value))
            cfg->kill_process_group_on_timeout = json_is_true(value);
        value = json_object_get(execution, "run_as_user");
        if (json_is_string(value) && dup_string_field(&cfg->run_as_user, json_string_value(value)) != 0)
            return -1;
        value = json_object_get(execution, "run_as_group");
        if (json_is_string(value) && dup_string_field(&cfg->run_as_group, json_string_value(value)) != 0)
            return -1;
    }

    limits = json_object_get(overrides, "limits");
    if (limits) {
        value = json_object_get(limits, "max_cpu_seconds");
        if (json_is_integer(value))
            cfg->max_cpu_seconds = (unsigned int)json_integer_value(value);
        value = json_object_get(limits, "max_memory_bytes");
        if (json_is_integer(value))
            cfg->max_memory_bytes = (unsigned int)json_integer_value(value);
        value = json_object_get(limits, "max_file_size_bytes");
        if (json_is_integer(value))
            cfg->max_file_size_bytes = (unsigned int)json_integer_value(value);
        value = json_object_get(limits, "max_open_files");
        if (json_is_integer(value))
            cfg->max_open_files = (unsigned int)json_integer_value(value);
        value = json_object_get(limits, "max_processes");
        if (json_is_integer(value))
            cfg->max_processes = (unsigned int)json_integer_value(value);
    }

    isolation = json_object_get(overrides, "isolation");
    if (isolation) {
        value = json_object_get(isolation, "require_non_root");
        if (json_is_boolean(value))
            cfg->require_non_root = json_is_true(value);
    }

#undef SHELL_SANDBOX_APPLY_BOOL
#undef SHELL_SANDBOX_APPLY_UINT
    return 0;
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

static char *shell_sandbox_current_working_directory(void)
{
#ifdef _WIN32
    return _getcwd(NULL, 0);
#else
    return getcwd(NULL, 0);
#endif
}

static int shell_sandbox_apply_bypass_profile(struct shell_exec_config *cfg)
{
    char *working_directory = shell_sandbox_current_working_directory();

    if (!working_directory)
        return -1;

    free(cfg->working_directory);
    cfg->working_directory = working_directory;
    free(cfg->run_as_user);
    cfg->run_as_user = NULL;
    free(cfg->run_as_group);
    cfg->run_as_group = NULL;
    shell_exec_allowed_env_destroy(cfg);
    cfg->allowed_env_is_set = false;

    cfg->max_command_length = MCP_SHELL_EXEC_MAX_COMMAND_LENGTH;
    cfg->default_timeout_ms = MCP_SANDBOX_CTL_MAX_TIMEOUT_MS;
    cfg->max_timeout_ms = MCP_SANDBOX_CTL_MAX_TIMEOUT_MS;
    cfg->max_output_bytes = MCP_SANDBOX_CTL_MAX_OUTPUT_BYTES;
    cfg->capture_stderr = true;
    cfg->clear_environment = false;
    cfg->request_cwd_allowed = true;
    cfg->request_env_allowed = true;
    cfg->kill_process_group_on_timeout = true;
    cfg->max_cpu_seconds = 0u;
    cfg->max_memory_bytes = 0u;
    cfg->max_file_size_bytes = 0u;
    cfg->max_open_files = 0u;
    cfg->max_processes = 0u;
    cfg->require_non_root = false;
    return 0;
}

static int shell_sandbox_apply_effective_policy(struct shell_exec_config *cfg,
                                                json_int_t revision,
                                                bool sandbox_enabled,
                                                bool shell_enabled_is_set,
                                                bool shell_enabled,
                                                const json_t *overrides)
{
    cfg->sandbox_revision = revision;
    cfg->sandbox_enabled = sandbox_enabled;
    if (shell_sandbox_apply_normalized_overrides(
            cfg, overrides, shell_enabled_is_set, shell_enabled) != 0)
        return -1;
    cfg->shell_enabled = cfg->enabled;
    if (!shell_sandbox_policy_is_valid(cfg, NULL))
        return 1;
    if (!sandbox_enabled && shell_sandbox_apply_bypass_profile(cfg) != 0)
        return -1;
    return 0;
}

static json_t *shell_sandbox_allowed_env_json(const struct shell_exec_config *cfg)
{
    json_t *array = json_array();
    size_t index;

    if (!array)
        return NULL;
    for (index = 0; index < cfg->allowed_env_count; index++) {
        if (json_array_append_new(array, json_string(cfg->allowed_env[index])) != 0) {
            json_decref(array);
            return NULL;
        }
    }
    return array;
}

static const char *shell_sandbox_parent_environment_mode(const struct shell_exec_config *cfg)
{
    if (cfg->clear_environment || (cfg->allowed_env_is_set && cfg->allowed_env_count == 0u))
        return "none";
    if (cfg->allowed_env_is_set)
        return "allowlist";
    return "all";
}

static json_t *shell_sandbox_policy_json(const struct shell_exec_config *cfg,
                                         bool include_sandbox_enabled,
                                         bool sandbox_enabled)
{
    json_t *root = json_object();
    json_t *execution = json_object();
    json_t *limits = json_object();
    json_t *isolation = json_object();
    json_t *allowed_env = shell_sandbox_allowed_env_json(cfg);

    if (!root || !execution || !limits || !isolation || !allowed_env)
        goto fail;

#define SHELL_SANDBOX_SET(object, name, value)                                            \
    do {                                                                                  \
        if (json_object_set_new(object, name, value) != 0)                                \
            goto fail;                                                                     \
    } while (0)

    SHELL_SANDBOX_SET(root, "shell_enabled", json_boolean(cfg->enabled));
    if (include_sandbox_enabled)
        SHELL_SANDBOX_SET(root, "sandbox_enabled", json_boolean(sandbox_enabled));
    SHELL_SANDBOX_SET(root, "max_command_length", json_integer(cfg->max_command_length));
    SHELL_SANDBOX_SET(root, "default_timeout_ms", json_integer(cfg->default_timeout_ms));
    SHELL_SANDBOX_SET(root, "max_timeout_ms", json_integer(cfg->max_timeout_ms));
    SHELL_SANDBOX_SET(root, "max_output_bytes", json_integer(cfg->max_output_bytes));
    SHELL_SANDBOX_SET(root, "capture_stderr", json_boolean(cfg->capture_stderr));
    SHELL_SANDBOX_SET(root, "merge_stderr", json_boolean(cfg->merge_stderr));

    SHELL_SANDBOX_SET(execution,
                      "working_directory",
                      json_string(cfg->working_directory ? cfg->working_directory : ""));
    SHELL_SANDBOX_SET(execution,
                      "request_cwd_allowed",
                      json_boolean(cfg->request_cwd_allowed));
    SHELL_SANDBOX_SET(execution, "clear_environment", json_boolean(cfg->clear_environment));
    if (json_object_set_new(execution, "allowed_env", allowed_env) != 0)
        goto fail;
    allowed_env = NULL;
    SHELL_SANDBOX_SET(execution,
                      "request_env_allowed",
                      json_boolean(cfg->request_env_allowed));
    SHELL_SANDBOX_SET(execution,
                      "kill_process_group_on_timeout",
                      json_boolean(cfg->kill_process_group_on_timeout));
    SHELL_SANDBOX_SET(execution,
                      "run_as_user",
                      cfg->run_as_user ? json_string(cfg->run_as_user) : json_null());
    SHELL_SANDBOX_SET(execution,
                      "run_as_group",
                      cfg->run_as_group ? json_string(cfg->run_as_group) : json_null());
#ifndef _WIN32
    SHELL_SANDBOX_SET(execution,
                      "parent_environment_mode",
                      json_string(shell_sandbox_parent_environment_mode(cfg)));
#endif

    SHELL_SANDBOX_SET(limits, "max_cpu_seconds", json_integer(cfg->max_cpu_seconds));
    SHELL_SANDBOX_SET(limits, "max_memory_bytes", json_integer(cfg->max_memory_bytes));
    SHELL_SANDBOX_SET(limits,
                      "max_file_size_bytes",
                      json_integer(cfg->max_file_size_bytes));
    SHELL_SANDBOX_SET(limits, "max_open_files", json_integer(cfg->max_open_files));
    SHELL_SANDBOX_SET(limits, "max_processes", json_integer(cfg->max_processes));
    SHELL_SANDBOX_SET(isolation, "require_non_root", json_boolean(cfg->require_non_root));

    if (json_object_set_new(root, "execution", execution) != 0)
        goto fail;
    execution = NULL;
    if (json_object_set_new(root, "limits", limits) != 0)
        goto fail;
    limits = NULL;
    if (json_object_set_new(root, "isolation", isolation) != 0)
        goto fail;
    isolation = NULL;

#undef SHELL_SANDBOX_SET
    return root;

fail:
#undef SHELL_SANDBOX_SET
    json_decref(allowed_env);
    json_decref(execution);
    json_decref(limits);
    json_decref(isolation);
    json_decref(root);
    return NULL;
}

static const char *shell_sandbox_supported_capability(bool policy_valid,
                                                      bool sandbox_enabled)
{
    return policy_valid && sandbox_enabled ? "enforced" : "ignored";
}

static json_t *shell_sandbox_capabilities_json(const struct shell_exec_config *cfg,
                                               bool policy_valid,
                                               bool sandbox_enabled)
{
    const char *supported = shell_sandbox_supported_capability(policy_valid, sandbox_enabled);
    const char *merge = supported;
    const char *allowed_env = supported;
    const char *max_memory = supported;
    const char *max_processes = supported;
    json_t *root = json_object();
    json_t *execution = json_object();
    json_t *limits = json_object();
    json_t *isolation = json_object();

    if (!root || !execution || !limits || !isolation)
        goto fail;

    if (policy_valid && sandbox_enabled && !cfg->capture_stderr)
        merge = "ignored";
#ifdef _WIN32
    allowed_env = "unsupported";
    max_memory = "unsupported";
    max_processes = "unsupported";
#else
    if (policy_valid && sandbox_enabled && cfg->clear_environment)
        allowed_env = "ignored";
#ifndef RLIMIT_AS
    max_memory = "unsupported";
#endif
#ifndef RLIMIT_NPROC
    max_processes = "unsupported";
#else
    if (policy_valid && sandbox_enabled && (!cfg->run_as_user || cfg->run_as_user[0] == '\0'))
        max_processes = "ignored";
#endif
#endif

#define SHELL_SANDBOX_SET_CAP(object, name, status)                                      \
    do {                                                                                  \
        if (json_object_set_new(object, name, json_string(status)) != 0)                  \
            goto fail;                                                                     \
    } while (0)

    SHELL_SANDBOX_SET_CAP(root, "shell_enabled", "enforced");
    SHELL_SANDBOX_SET_CAP(root, "sandbox_enabled", "enforced");
    SHELL_SANDBOX_SET_CAP(root, "max_command_length", supported);
    SHELL_SANDBOX_SET_CAP(root, "default_timeout_ms", supported);
    SHELL_SANDBOX_SET_CAP(root, "max_timeout_ms", supported);
    SHELL_SANDBOX_SET_CAP(root, "max_output_bytes", supported);
    SHELL_SANDBOX_SET_CAP(root, "capture_stderr", supported);
    SHELL_SANDBOX_SET_CAP(root, "merge_stderr", merge);

    SHELL_SANDBOX_SET_CAP(execution, "working_directory", supported);
#ifdef _WIN32
    SHELL_SANDBOX_SET_CAP(execution, "request_cwd_allowed", "unsupported");
#else
    SHELL_SANDBOX_SET_CAP(execution, "request_cwd_allowed", supported);
#endif
    SHELL_SANDBOX_SET_CAP(execution, "clear_environment", supported);
    SHELL_SANDBOX_SET_CAP(execution, "allowed_env", allowed_env);
#ifdef _WIN32
    SHELL_SANDBOX_SET_CAP(execution, "request_env_allowed", "unsupported");
    SHELL_SANDBOX_SET_CAP(execution, "kill_process_group_on_timeout", "unsupported");
    SHELL_SANDBOX_SET_CAP(execution, "run_as_user", "unsupported");
    SHELL_SANDBOX_SET_CAP(execution, "run_as_group", "unsupported");
#else
    SHELL_SANDBOX_SET_CAP(execution, "request_env_allowed", supported);
    SHELL_SANDBOX_SET_CAP(execution, "kill_process_group_on_timeout", supported);
    SHELL_SANDBOX_SET_CAP(execution, "run_as_user", supported);
    SHELL_SANDBOX_SET_CAP(execution, "run_as_group", supported);
#endif

#ifdef _WIN32
    SHELL_SANDBOX_SET_CAP(limits, "max_cpu_seconds", "unsupported");
#else
    SHELL_SANDBOX_SET_CAP(limits, "max_cpu_seconds", supported);
#endif
    SHELL_SANDBOX_SET_CAP(limits, "max_memory_bytes", max_memory);
#ifdef _WIN32
    SHELL_SANDBOX_SET_CAP(limits, "max_file_size_bytes", "unsupported");
    SHELL_SANDBOX_SET_CAP(limits, "max_open_files", "unsupported");
#else
    SHELL_SANDBOX_SET_CAP(limits, "max_file_size_bytes", supported);
    SHELL_SANDBOX_SET_CAP(limits, "max_open_files", supported);
#endif
    SHELL_SANDBOX_SET_CAP(limits, "max_processes", max_processes);
#ifdef _WIN32
    SHELL_SANDBOX_SET_CAP(isolation, "require_non_root", "reject_only");
#else
    SHELL_SANDBOX_SET_CAP(isolation, "require_non_root", supported);
#endif

    if (json_object_set_new(root, "execution", execution) != 0)
        goto fail;
    execution = NULL;
    if (json_object_set_new(root, "limits", limits) != 0)
        goto fail;
    limits = NULL;
    if (json_object_set_new(root, "isolation", isolation) != 0)
        goto fail;
    isolation = NULL;

#undef SHELL_SANDBOX_SET_CAP
    return root;

fail:
#undef SHELL_SANDBOX_SET_CAP
    json_decref(execution);
    json_decref(limits);
    json_decref(isolation);
    json_decref(root);
    return NULL;
}

static json_t *shell_sandbox_saved_overrides_json(const json_t *overrides,
                                                  bool shell_enabled_is_set,
                                                  bool shell_enabled)
{
    json_t *copy = json_deep_copy(overrides);

    if (!copy)
        return NULL;
    if (shell_enabled_is_set &&
        json_object_set_new(copy, "shell_enabled", json_boolean(shell_enabled)) != 0) {
        json_decref(copy);
        return NULL;
    }
    return copy;
}

static json_t *shell_sandbox_state_json(json_int_t revision,
                                        bool sandbox_enabled,
                                        bool shell_enabled_is_set,
                                        bool shell_enabled,
                                        const json_t *overrides,
                                        struct shell_exec_config *cfg)
{
    bool policy_valid = false;
    int effective_status;
    json_t *root = json_object();
    json_t *base = NULL;
    json_t *effective = NULL;
    json_t *saved_overrides = NULL;
    json_t *capabilities = NULL;

    if (!root)
        return NULL;

    saved_overrides =
        shell_sandbox_saved_overrides_json(overrides, shell_enabled_is_set, shell_enabled);
    if (!saved_overrides)
        goto fail;

    if (cfg->config_loaded) {
        base = shell_sandbox_policy_json(cfg, false, true);
        if (!base)
            goto fail;
        effective_status = shell_sandbox_apply_effective_policy(cfg,
                                                                 revision,
                                                                 sandbox_enabled,
                                                                 shell_enabled_is_set,
                                                                 shell_enabled,
                                                                 overrides);
        if (effective_status < 0)
            goto fail;
        policy_valid = effective_status == 0;
        if (policy_valid) {
            effective = shell_sandbox_policy_json(cfg, true, sandbox_enabled);
            if (!effective)
                goto fail;
        }
    }

    capabilities = shell_sandbox_capabilities_json(cfg, policy_valid, sandbox_enabled);
    if (!capabilities)
        goto fail;

#define SHELL_SANDBOX_SET_STATE(name, value)                                              \
    do {                                                                                  \
        if (json_object_set_new(root, name, value) != 0)                                  \
            goto fail;                                                                     \
    } while (0)

    SHELL_SANDBOX_SET_STATE("revision", json_integer(revision));
    SHELL_SANDBOX_SET_STATE("persistence", json_string("process"));
    SHELL_SANDBOX_SET_STATE("applies_to", json_string("new_executions"));
    SHELL_SANDBOX_SET_STATE("shell_enabled",
                            policy_valid ? json_boolean(cfg->enabled) : json_null());
    SHELL_SANDBOX_SET_STATE("shell_enabled_override",
                            shell_enabled_is_set ? json_boolean(shell_enabled) : json_null());
    SHELL_SANDBOX_SET_STATE("sandbox_enabled", json_boolean(sandbox_enabled));
    SHELL_SANDBOX_SET_STATE("config_loaded", json_boolean(cfg->config_loaded));
    SHELL_SANDBOX_SET_STATE("policy_valid", json_boolean(policy_valid));
    SHELL_SANDBOX_SET_STATE("config_path",
                            json_string(cfg->config_path ? cfg->config_path : "(unknown)"));
    if (!cfg->config_loaded) {
        SHELL_SANDBOX_SET_STATE("config_error",
                                json_string(cfg->load_error ? cfg->load_error
                                                            : "shell_exec config unavailable"));
    }
    if (base) {
        if (json_object_set_new(root, "base", base) != 0)
            goto fail;
        base = NULL;
    } else {
        SHELL_SANDBOX_SET_STATE("base", json_null());
    }
    if (json_object_set_new(root, "overrides", saved_overrides) != 0)
        goto fail;
    saved_overrides = NULL;
    if (effective) {
        if (json_object_set_new(root, "effective", effective) != 0)
            goto fail;
        effective = NULL;
    } else {
        SHELL_SANDBOX_SET_STATE("effective", json_null());
    }
    if (json_object_set_new(root, "capabilities", capabilities) != 0)
        goto fail;
    capabilities = NULL;

#undef SHELL_SANDBOX_SET_STATE
    return root;

fail:
#undef SHELL_SANDBOX_SET_STATE
    json_decref(base);
    json_decref(effective);
    json_decref(saved_overrides);
    json_decref(capabilities);
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
    struct shell_exec_config cfg;
    struct shell_exec_config current_cfg;
    json_t *state = NULL;
    json_t *temporary_overrides = NULL;
    json_t *changed_paths = NULL;
    json_t *value;
    json_int_t expected_revision;
    json_int_t next_revision;
    bool temporary_shell_enabled_is_set;
    bool temporary_shell_enabled;
    bool temporary_sandbox_enabled;
    size_t leaf_count = 0u;
    int rc = -1;

    if (!out_result)
        return -1;
    *out_result = NULL;
    memset(&cfg, 0, sizeof(cfg));
    memset(&current_cfg, 0, sizeof(current_cfg));

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
        if (shell_exec_config_load(&cfg) != 0)
            return shell_sandbox_internal_error(out_result);
        state = shell_sandbox_state_json(control->revision,
                                         control->sandbox_enabled,
                                         control->shell_enabled_is_set,
                                         control->shell_enabled,
                                         control->overrides,
                                         &cfg);
        if (!state) {
            shell_exec_config_destroy(&cfg);
            return shell_sandbox_internal_error(out_result);
        }
        rc = shell_sandbox_encode_success(state, out_result);
        json_decref(state);
        shell_exec_config_destroy(&cfg);
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

    if (shell_exec_config_load(&cfg) != 0)
        return shell_sandbox_internal_error(out_result);
    if (!cfg.config_loaded) {
        shell_exec_config_destroy(&cfg);
        return shell_sandbox_set_error(out_result,
                                       "config_unavailable",
                                       "shell sandbox configuration is unavailable",
                                       NULL,
                                       false,
                                       0);
    }
    if (!shell_sandbox_revision_can_increment(control->revision)) {
        shell_exec_config_destroy(&cfg);
        return shell_sandbox_internal_error(out_result);
    }
    if (shell_sandbox_config_copy(&current_cfg, &cfg) != 0)
        goto internal_error;
    if (shell_sandbox_apply_normalized_overrides(&current_cfg,
                                                 control->overrides,
                                                 control->shell_enabled_is_set,
                                                 control->shell_enabled) != 0)
        goto internal_error;
    if (!shell_sandbox_policy_is_valid(&current_cfg, NULL)) {
        shell_exec_config_destroy(&current_cfg);
        shell_exec_config_destroy(&cfg);
        return shell_sandbox_set_error(out_result,
                                       "config_unavailable",
                                       "shell sandbox configuration is unavailable",
                                       NULL,
                                       false,
                                       0);
    }
    shell_exec_config_destroy(&current_cfg);

    temporary_overrides = action == SHELL_SANDBOX_ACTION_RESET
                              ? json_object()
                              : json_deep_copy(control->overrides);
    changed_paths = json_array();
    if (!temporary_overrides || !changed_paths)
        goto internal_error;
    temporary_shell_enabled_is_set = control->shell_enabled_is_set;
    temporary_shell_enabled = control->shell_enabled;
    temporary_sandbox_enabled = control->sandbox_enabled;

    if (action == SHELL_SANDBOX_ACTION_RESET) {
        temporary_shell_enabled_is_set = false;
        temporary_shell_enabled = false;
        temporary_sandbox_enabled = true;
        if (shell_sandbox_record_path(changed_paths, "shell_enabled") !=
                SHELL_SANDBOX_PATCH_OK ||
            shell_sandbox_record_path(changed_paths, "sandbox_enabled") !=
                SHELL_SANDBOX_PATCH_OK ||
            shell_sandbox_record_path(changed_paths, "overrides") != SHELL_SANDBOX_PATCH_OK)
            goto internal_error;
    } else {
        struct shell_sandbox_patch_error patch_error = {0};
        enum shell_sandbox_patch_status patch_status;

        value = json_object_get(arguments, "shell_enabled");
        if (value) {
            if (json_is_null(value)) {
                temporary_shell_enabled_is_set = false;
                temporary_shell_enabled = false;
            } else if (json_is_boolean(value)) {
                temporary_shell_enabled_is_set = true;
                temporary_shell_enabled = json_is_true(value);
            } else {
                invalid_field = "shell_enabled";
                goto invalid_params;
            }
            if (shell_sandbox_record_path(changed_paths, "shell_enabled") !=
                SHELL_SANDBOX_PATCH_OK)
                goto internal_error;
            leaf_count++;
        }

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
            patch_status = shell_sandbox_apply_overrides_patch(temporary_overrides,
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
                shell_exec_config_destroy(&cfg);
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
    state = shell_sandbox_state_json(next_revision,
                                     temporary_sandbox_enabled,
                                     temporary_shell_enabled_is_set,
                                     temporary_shell_enabled,
                                     temporary_overrides,
                                     &cfg);
    if (!state)
        goto internal_error;
    if (!json_is_true(json_object_get(state, "policy_valid"))) {
        json_decref(state);
        json_decref(temporary_overrides);
        json_decref(changed_paths);
        shell_sandbox_policy_is_valid(&cfg, &invalid_field);
        shell_exec_config_destroy(&cfg);
        return shell_sandbox_invalid_params(out_result, invalid_field);
    }
    if (shell_sandbox_encode_success(state, out_result) != 0)
        goto internal_error;
    json_decref(state);
    state = NULL;

    json_decref(control->overrides);
    control->overrides = temporary_overrides;
    temporary_overrides = NULL;
    control->shell_enabled_is_set = temporary_shell_enabled_is_set;
    control->shell_enabled = temporary_shell_enabled;
    control->sandbox_enabled = temporary_sandbox_enabled;
    control->revision = next_revision;
    shell_sandbox_audit_log(action == SHELL_SANDBOX_ACTION_RESET ? "reset" : "update",
                            expected_revision,
                            next_revision,
                            temporary_sandbox_enabled,
                            cfg.enabled,
                            changed_paths);
    json_decref(changed_paths);
    shell_exec_config_destroy(&cfg);
    return 0;

invalid_params:
    json_decref(temporary_overrides);
    json_decref(changed_paths);
    shell_exec_config_destroy(&cfg);
    return shell_sandbox_invalid_params(out_result, invalid_field);

internal_error:
    json_decref(state);
    json_decref(temporary_overrides);
    json_decref(changed_paths);
    shell_exec_config_destroy(&current_cfg);
    shell_exec_config_destroy(&cfg);
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
    free(job->command);
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

    payload = json_pack("{s:s,s:s,s:i,s:i,s:s,s:s,s:i,s:i,s:i,s:i,s:b,s:b,s:i,s:i}",
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
                        "command",
                        job->command ? job->command : "",
                        "timeout_ms",
                        (json_int_t)job->timeout_ms,
                        "deadline_ms",
                        (json_int_t)job->deadline_ms,
                        "stdout_bytes",
                        (json_int_t)job->stdout_buf.len,
                        "stderr_bytes",
                        (json_int_t)job->stderr_buf.len,
                        "stdout_truncated",
                        job->stdout_buf.truncated,
                        "stderr_truncated",
                        job->stderr_buf.truncated,
                        "exit_code",
                        (json_int_t)job->exit_code,
                        "signal",
                        (json_int_t)job->signal_number);
    if (!payload)
        return NULL;

    if (job->label)
        json_object_set_new(payload, "label", json_string(job->label));
    if (job->finished_at[0])
        json_object_set_new(payload, "finished_at", json_string(job->finished_at));
    if (job->state == SHELL_JOB_RUNNING) {
        json_t *rollback = json_pack("{s:s,s:{s:s}}",
                                     "tool_name",
                                     "system.shell_kill",
                                     "args",
                                     "job_id",
                                     job->job_id);
        if (rollback)
            json_object_set_new(payload, "rollback", rollback);
    }

    return payload;
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

static int shell_job_apply_start_overrides(struct shell_exec_config *cfg,
                                           json_t *arguments,
                                           char **out_error)
{
    json_t *cwd;
    json_t *env;
    const char *key;
    json_t *value;

    *out_error = NULL;

    cwd = json_object_get(arguments, "cwd");
    if (cwd) {
        if (!json_is_string(cwd)) {
            *out_error = mcp_strdup("Invalid params: cwd must be a string.");
            return 0;
        }
        if (dup_string_field(&cfg->working_directory, json_string_value(cwd)) != 0)
            return -1;
    }

    env = json_object_get(arguments, "env");
    if (!env)
        return 0;
    if (!json_is_object(env)) {
        *out_error = mcp_strdup("Invalid params: env must be an object of string values.");
        return 0;
    }

    json_object_foreach(env, key, value) {
        if (!shell_exec_env_name_is_valid(key)) {
            *out_error = mcp_strdup("Invalid params: env names must be non-empty and must not contain '='.");
            return 0;
        }
        if (!json_is_string(value)) {
            *out_error = mcp_strdup("Invalid params: env values must be strings.");
            return 0;
        }
        if (shell_exec_config_set_env_var(cfg, key, json_string_value(value)) != 0)
            return -1;
    }

    return 0;
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

static json_t *shell_exec_build_result(const struct shell_exec_config *cfg,
                                       const struct shell_exec_request *request,
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
    if (json_object_set_new(payload, "sandbox_revision", json_integer(cfg->sandbox_revision)) != 0)
        goto fail;
    if (json_object_set_new(payload, "sandbox_enabled", json_boolean(cfg->sandbox_enabled)) != 0)
        goto fail;
    if (json_object_set_new(payload, "shell_enabled", json_boolean(cfg->shell_enabled)) != 0)
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

    if (!control) {
        *out_result = mcp_tool_result_text("Failed to initialize shell sandbox policy.", true);
        goto cleanup;
    }
    {
        int effective_status = shell_sandbox_apply_effective_policy(&cfg,
                                                                     control->revision,
                                                                     control->sandbox_enabled,
                                                                     control->shell_enabled_is_set,
                                                                     control->shell_enabled,
                                                                     control->overrides);
        if (effective_status < 0) {
            *out_result = mcp_tool_result_text("Failed to merge shell sandbox policy.", true);
            goto cleanup;
        }
        if (effective_status > 0) {
            *out_result = mcp_tool_result_text(
                "system.shell_exec is disabled because the effective sandbox policy is invalid.",
                true);
            rc = 0;
            goto cleanup;
        }
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

    if (!shell_exec_resolve_timeout(&cfg, invocation, &request.timeout_ms)) {
        *out_result = mcp_tool_result_text(
            "Invalid params: timeout_ms must be an integer from 1 to the current effective maximum.",
            true);
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
    *out_result = shell_exec_build_result(&cfg,
                                          &request,
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
    struct shell_exec_config cfg;
    struct shell_exec_request request;
    struct shell_job *job = NULL;
    json_t *command_value;
    const char *command;
    const char *label;
    size_t command_length;
    unsigned int timeout_ms;
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

    if (shell_exec_config_load(&cfg) != 0) {
        *out_result = mcp_tool_result_text("Failed to initialize shell_start configuration.", true);
        goto cleanup;
    }
    if (!cfg.config_loaded) {
        char message[512];

        snprintf(message,
                 sizeof(message),
                 "system.shell_start is disabled because configuration failed to load from %s. %s",
                 cfg.config_path ? cfg.config_path : "(unknown)",
                 cfg.load_error ? cfg.load_error : "No details available.");
        *out_result = mcp_tool_result_text(message, true);
        rc = 0;
        goto cleanup;
    }
    if (!cfg.enabled) {
        *out_result = mcp_tool_result_text(
            "system.shell_start is disabled by policy. Enable it in shell_exec.json or MCP_ENABLE_SHELL_EXEC=1 for a trusted session.",
            true);
        rc = 0;
        goto cleanup;
    }
    if (json_object_get(invocation->arguments, "args")) {
        *out_result = mcp_tool_result_text("Invalid params: args is not supported by system.shell_start.", true);
        rc = 0;
        goto cleanup;
    }
    if (shell_job_apply_start_overrides(&cfg, invocation->arguments, &error_message) != 0) {
        *out_result = mcp_tool_result_text("Failed to apply shell_start overrides.", true);
        goto cleanup;
    }
    if (error_message) {
        *out_result = mcp_tool_result_text(error_message, true);
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

    if (!shell_job_uint_arg(invocation->arguments,
                            "timeout_ms",
                            cfg.default_timeout_ms,
                            MCP_SHELL_EXEC_MIN_TIMEOUT_MS,
                            cfg.max_timeout_ms,
                            &timeout_ms)) {
        char message[128];

        snprintf(message,
                 sizeof(message),
                 "Invalid params: timeout_ms must be between 1 and %u for system.shell_start.",
                 cfg.max_timeout_ms);
        *out_result = mcp_tool_result_text(message, true);
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

    request.timeout_ms = timeout_ms;
    job = calloc(1, sizeof(*job));
    if (!job)
        goto cleanup;

#ifndef _WIN32
    job->stdout_fd = -1;
    job->stderr_fd = -1;
#endif
    job->command = mcp_strdup(request.command);
    job->label = label ? mcp_strdup(label) : NULL;
    if (!job->command || (label && !job->label))
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
    unsigned long long deadline;
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

    deadline = mcp_now_ms() + timeout_ms;
    for (;;) {
        shell_jobs_poll_all(server->shell_jobs);
        job = shell_job_find(server->shell_jobs, job_id);
        if (!job) {
            *out_result = mcp_tool_result_text("Shell job was not found.", true);
            return 0;
        }
        if (shell_job_is_final(job) && !shell_job_needs_poll(job))
            break;
        if (mcp_now_ms() >= deadline)
            break;
        uv_sleep(10);
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
