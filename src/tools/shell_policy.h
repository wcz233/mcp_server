#ifndef MCP_SRC_TOOLS_SHELL_POLICY_H
#define MCP_SRC_TOOLS_SHELL_POLICY_H

#include <jansson.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MCP_SHELL_POLICY_TOKEN_MAX_BYTES 4096u
#define MCP_SHELL_POLICY_IDENTITY_MAX_BYTES 255u
#define MCP_SHELL_POLICY_ENV_HARD_MAX_ITEMS 1024u
#define MCP_SHELL_POLICY_ENV_HARD_ITEM_MAX_BYTES 255u

enum mcp_shell_policy_field_id {
    MCP_SHELL_POLICY_FIELD_SHELL_ENABLED = 0,
    MCP_SHELL_POLICY_FIELD_COMMAND_LENGTH,
    MCP_SHELL_POLICY_FIELD_TIMEOUT_MS,
    MCP_SHELL_POLICY_FIELD_OUTPUT_BYTES,
    MCP_SHELL_POLICY_FIELD_READ_CHUNK_SIZE,
    MCP_SHELL_POLICY_FIELD_CAPTURE_STDERR,
    MCP_SHELL_POLICY_FIELD_MERGE_STDERR,
    MCP_SHELL_POLICY_FIELD_EXECUTION_MODE,
    MCP_SHELL_POLICY_FIELD_SHELL_PATH,
    MCP_SHELL_POLICY_FIELD_SHELL_ARG,
    MCP_SHELL_POLICY_FIELD_WORKING_DIRECTORY,
    MCP_SHELL_POLICY_FIELD_INHERIT_ENV,
    MCP_SHELL_POLICY_FIELD_REQUEST_CWD_ALLOWED,
    MCP_SHELL_POLICY_FIELD_REQUEST_ENV_ALLOWED,
    MCP_SHELL_POLICY_FIELD_KILL_PROCESS_GROUP,
    MCP_SHELL_POLICY_FIELD_RUN_AS_USER,
    MCP_SHELL_POLICY_FIELD_RUN_AS_GROUP,
    MCP_SHELL_POLICY_FIELD_ENV,
    MCP_SHELL_POLICY_FIELD_CPU_SECONDS,
    MCP_SHELL_POLICY_FIELD_MEMORY_BYTES,
    MCP_SHELL_POLICY_FIELD_FILE_SIZE_BYTES,
    MCP_SHELL_POLICY_FIELD_OPEN_FILES,
    MCP_SHELL_POLICY_FIELD_PROCESSES,
    MCP_SHELL_POLICY_FIELD_REQUIRE_NON_ROOT,
    MCP_SHELL_POLICY_FIELD_COUNT,
};

enum mcp_shell_policy_field_type {
    MCP_SHELL_POLICY_TYPE_BOOLEAN = 1,
    MCP_SHELL_POLICY_TYPE_UINT64,
    MCP_SHELL_POLICY_TYPE_STRING,
    MCP_SHELL_POLICY_TYPE_MODE,
    MCP_SHELL_POLICY_TYPE_ENV,
};

enum mcp_shell_policy_capability {
    MCP_SHELL_POLICY_CAPABILITY_PORTABLE = 1,
    MCP_SHELL_POLICY_CAPABILITY_PLATFORM_DEPENDENT,
    MCP_SHELL_POLICY_CAPABILITY_UNIX_ONLY,
};

enum mcp_shell_policy_source {
    MCP_SHELL_POLICY_SOURCE_HARD = 1,
    MCP_SHELL_POLICY_SOURCE_JSON,
    MCP_SHELL_POLICY_SOURCE_HARD_FALLBACK,
};

enum mcp_shell_policy_mode {
    MCP_SHELL_POLICY_MODE_SHELL = 0,
    MCP_SHELL_POLICY_MODE_EXEC = 1,
};

#define MCP_SHELL_POLICY_MODE_SHELL_MASK (1u << MCP_SHELL_POLICY_MODE_SHELL)
#define MCP_SHELL_POLICY_MODE_EXEC_MASK (1u << MCP_SHELL_POLICY_MODE_EXEC)

struct mcp_shell_policy_snapshot;
struct mcp_shell_policy_field_descriptor;

typedef int (*mcp_shell_policy_parse_hook)(struct mcp_shell_policy_snapshot *snapshot,
                                           const json_t *root,
                                           const struct mcp_shell_policy_field_descriptor *field,
                                           char *error,
                                           size_t error_size);
typedef bool (*mcp_shell_policy_validate_hook)(
    const struct mcp_shell_policy_snapshot *snapshot,
    const struct mcp_shell_policy_field_descriptor *field);
typedef json_t *(*mcp_shell_policy_serialize_hook)(
    const struct mcp_shell_policy_snapshot *snapshot,
    const struct mcp_shell_policy_field_descriptor *field);

struct mcp_shell_policy_field_descriptor {
    enum mcp_shell_policy_field_id id;
    const char *path;
    const char *bounds_path;
    enum mcp_shell_policy_field_type type;
    uint64_t hard_default;
    uint64_t hard_min;
    uint64_t hard_max;
    const char *hard_default_string;
    size_t hard_min_bytes;
    size_t hard_max_bytes;
    bool runtime_mutable;
    enum mcp_shell_policy_capability capability;
    size_t value_offset;
    mcp_shell_policy_parse_hook parser;
    mcp_shell_policy_validate_hook validator;
    mcp_shell_policy_serialize_hook serializer;
};

struct mcp_shell_policy_numeric {
    uint64_t value;
    uint64_t min;
    uint64_t max;
    enum mcp_shell_policy_source source;
};

struct mcp_shell_policy_string {
    char *value;
    size_t min_bytes;
    size_t max_bytes;
    enum mcp_shell_policy_source source;
};

struct mcp_shell_policy_mode_value {
    enum mcp_shell_policy_mode value;
    unsigned int allowed_mask;
    enum mcp_shell_policy_source source;
};

struct mcp_shell_policy_env_var {
    char *name;
    char *value;
};

struct mcp_shell_policy_execution {
    struct mcp_shell_policy_mode_value mode;
    struct mcp_shell_policy_string shell_path;
    struct mcp_shell_policy_string shell_arg;
    struct mcp_shell_policy_string working_directory;
    bool inherit_env;
    bool request_cwd_allowed;
    bool request_env_allowed;
    bool kill_process_group_on_timeout;
    struct mcp_shell_policy_string run_as_user;
    struct mcp_shell_policy_string run_as_group;
    struct mcp_shell_policy_env_var *env_vars;
    size_t env_var_count;
};

struct mcp_shell_policy_limits {
    struct mcp_shell_policy_numeric cpu_seconds;
    struct mcp_shell_policy_numeric memory_bytes;
    struct mcp_shell_policy_numeric file_size_bytes;
    struct mcp_shell_policy_numeric open_files;
    struct mcp_shell_policy_numeric processes;
};

struct mcp_shell_policy_defaults {
    bool shell_enabled;
    struct mcp_shell_policy_numeric command_length;
    struct mcp_shell_policy_numeric timeout_ms;
    struct mcp_shell_policy_numeric output_bytes;
    struct mcp_shell_policy_numeric once_read_stdout_err_chunk_size;
    bool capture_stderr;
    bool merge_stderr_to_stdout;
    struct mcp_shell_policy_execution execution;
    struct mcp_shell_policy_limits limits;
    bool require_non_root;
};

struct mcp_shell_policy_snapshot {
    bool control_enabled;
    bool config_loaded;
    int version;
    char *config_path;
    char *token;
    size_t token_length;
    struct mcp_shell_policy_defaults defaults;
    size_t environment_max_items;
    size_t environment_item_max_bytes;
    enum mcp_shell_policy_source environment_source;
    const char *diagnostics[MCP_SHELL_POLICY_FIELD_COUNT];
};

const struct mcp_shell_policy_field_descriptor *mcp_shell_policy_field_directory(size_t *count);

int mcp_shell_policy_snapshot_create_hard(struct mcp_shell_policy_snapshot **out,
                                          const char *config_path,
                                          bool control_enabled);
int mcp_shell_policy_snapshot_create_from_environment(struct mcp_shell_policy_snapshot **out);
int mcp_shell_policy_snapshot_parse_json(struct mcp_shell_policy_snapshot **out,
                                         const json_t *root,
                                         const char *config_path,
                                         bool control_enabled,
                                         char *error,
                                         size_t error_size);
void mcp_shell_policy_snapshot_destroy(struct mcp_shell_policy_snapshot *snapshot);
const char *mcp_shell_policy_snapshot_diagnostic(
    const struct mcp_shell_policy_snapshot *snapshot,
    enum mcp_shell_policy_field_id field);

#endif
