#include "mcp/plugin/plugin_abi.h"

#include <errno.h>
#include <fcntl.h>
#include <jansson.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <windows.h>
#else
#include <dirent.h>
#include <pthread.h>
#include <sys/select.h>
#include <unistd.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifdef _WIN32
#ifndef S_ISDIR
#define S_ISDIR(mode) (((mode) & _S_IFMT) == _S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(mode) (((mode) & _S_IFMT) == _S_IFREG)
#endif
#define stat _stat64
#define MFT_OPEN_BINARY _O_BINARY
#else
#define MFT_OPEN_BINARY 0
#endif

#ifdef _WIN32
struct dirent {
    char d_name[PATH_MAX];
};

typedef struct mft_win_dir {
    HANDLE handle;
    WIN32_FIND_DATAA data;
    int first;
    struct dirent entry;
} DIR;

static int mft_win_path_has_trailing_separator(const char *path)
{
    size_t len;

    if (!path)
        return 0;
    len = strlen(path);
    return len > 0 && (path[len - 1] == '/' || path[len - 1] == '\\');
}

static DIR *opendir(const char *path)
{
    char pattern[PATH_MAX];
    DIR *dir;
    int len;

    if (!path) {
        errno = EINVAL;
        return NULL;
    }
    len = snprintf(pattern,
                   sizeof(pattern),
                   "%s%s*",
                   path,
                   mft_win_path_has_trailing_separator(path) ? "" : "\\");
    if (len < 0 || (size_t)len >= sizeof(pattern)) {
        errno = ENAMETOOLONG;
        return NULL;
    }

    dir = calloc(1, sizeof(*dir));
    if (!dir)
        return NULL;
    dir->handle = FindFirstFileA(pattern, &dir->data);
    if (dir->handle == INVALID_HANDLE_VALUE) {
        free(dir);
        errno = ENOENT;
        return NULL;
    }
    dir->first = 1;
    return dir;
}

static struct dirent *readdir(DIR *dir)
{
    size_t len;

    if (!dir) {
        errno = EINVAL;
        return NULL;
    }
    if (dir->first) {
        dir->first = 0;
    } else if (!FindNextFileA(dir->handle, &dir->data)) {
        return NULL;
    }

    len = strlen(dir->data.cFileName);
    if (len >= sizeof(dir->entry.d_name)) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    memcpy(dir->entry.d_name, dir->data.cFileName, len + 1);
    return &dir->entry;
}

static int closedir(DIR *dir)
{
    int rc = 0;

    if (!dir) {
        errno = EINVAL;
        return -1;
    }
    if (!FindClose(dir->handle))
        rc = -1;
    free(dir);
    return rc;
}
#define chmod(path, mode) _chmod((path), (int)(mode))
#define close(fd) _close(fd)
#define mkdir(path, mode) _mkdir(path)
#define open _open
#define write _write
#define lseek _lseeki64
#ifndef ssize_t
typedef intptr_t ssize_t;
#endif
#ifndef mode_t
typedef int mode_t;
#endif
#endif

#ifdef MCP_FILE_TRANSFER_PLUGIN_BUILTIN
#define MFT_PLUGIN_EXPORT
#define MFT_PLUGIN_INIT mcp_file_transfer_plugin_init
#define MFT_PLUGIN_INVOKE mcp_file_transfer_plugin_invoke
#define MFT_PLUGIN_SHUTDOWN mcp_file_transfer_plugin_shutdown
#else
#define MFT_PLUGIN_EXPORT MCP_PLUGIN_EXPORT
#define MFT_PLUGIN_INIT mcp_plugin_init
#define MFT_PLUGIN_INVOKE mcp_plugin_invoke
#define MFT_PLUGIN_SHUTDOWN mcp_plugin_shutdown
#endif

#define MFT_MAGIC "MFT1"
#define MFT_VERSION 1u
#define MFT_CAP_WHOLE_FILE "mft.v1.whole_file"
#define MFT_CAP_RESUME "mft.v1.resume"
#define MFT_CAP_BLOCK_ACK "mft.v1.block_ack"
#define MFT_LOGICAL_BLOCK_SIZE (64u * 1024u * 1024u)
#define MFT_MAX_CHUNK 32768u
#define MFT_MAX_CONCURRENT_STREAMS 4u
#define MFT_MAX_BLOCK_RETRIES 3u
#define MFT_BLOCK_TIMEOUT_MS 10000u
#define MFT_PUMP_RETRY_MS 50u
#define MFT_PUMP_MAX_BLOCKS 4u
#define MFT_MAX_IN_FLIGHT_BLOCKS 4u
#define MFT_DEFAULT_TIMEOUT_MS 30000u
#define MFT_MIN_TIMEOUT_MS 1000u
#define MFT_MAX_TIMEOUT_MS 600000u

enum mft_frame_type {
    MFT_FRAME_HELLO = 1,
    MFT_FRAME_OFFER = 2,
    MFT_FRAME_FETCH_REQUEST = 3,
    MFT_FRAME_ACCEPT = 4,
    MFT_FRAME_DATA = 5,
    MFT_FRAME_ACK = 6,
    MFT_FRAME_WINDOW_UPDATE = 7,
    MFT_FRAME_COMPLETE = 8,
    MFT_FRAME_ABORT = 9,
};

struct sha256_ctx {
    uint32_t state[8];
    uint64_t bitlen;
    unsigned char data[64];
    size_t datalen;
};

struct transfer_context;
static int touch_transfer_activity(struct transfer_context *ctx);

struct block_entry {
    uint64_t offset;
    uint64_t size;
    unsigned int retries;
    char hash[65];
    bool sent;
    bool ack_waiting;
    bool acked;
    bool resend_pending;
    bool received_ok;
    bool receive_waiting;
    bool receive_failed;
    bool receive_hash_started;
    bool receive_hash_failed;
    uint64_t send_next_offset;
    uint64_t receive_next_offset;
    unsigned long long ack_deadline_ms;
    unsigned long long receive_deadline_ms;
    struct sha256_ctx receive_hash_ctx;
};

struct manifest_entry {
    char *relpath;
    char type;
    uint64_t size;
    uint64_t mtime_ns;
    unsigned int mode;
    char hash[65];
    struct block_entry *blocks;
    size_t block_count;
};

struct mft_plugin {
    const struct mcp_plugin_host_api *host;
    unsigned long long next_transfer;
#ifdef _WIN32
    CRITICAL_SECTION lock;
    CONDITION_VARIABLE timer_cv;
    HANDLE timer_thread;
#else
    pthread_mutex_t lock;
    pthread_cond_t timer_cond;
    pthread_t timer_thread;
#endif
    bool sync_initialized;
    bool timer_started;
    bool timer_stop;
};

struct transfer_context {
    char *invocation_id;
    char *transfer_id;
    char *local_path;
    char *remote_path;
    char *source_name;
    char direction[8];
    unsigned int server_id;
    uint32_t timeout_ms;
    unsigned long long started_ms;
    unsigned long long deadline_ms;
    FILE *send_fp;
    size_t send_entry_index;
    size_t send_block_index;
    size_t recv_entry_index;
    size_t recv_block_index;
    uint64_t bytes_total;
    uint64_t bytes_transferred;
    unsigned int files_skipped;
    unsigned int files_transferred;
    bool send_complete_sent;
    bool send_paused;
    bool pumping;
    bool cancelled;
    bool free_after_pump;
    bool completed;
    unsigned long long send_pump_deadline_ms;
    struct manifest_entry *entries;
    size_t entry_count;
    json_t *accept;
    struct transfer_context *next;
};

struct pending_block_ack {
    unsigned int server_id;
    char *transfer_id;
    uint32_t stream_id;
    size_t block_index;
    uint64_t offset;
    uint64_t size;
    bool ok;
    struct pending_block_ack *next;
};

struct pending_transfer_error {
    struct transfer_context *ctx;
    const char *message;
    bool notify_peer;
    struct pending_transfer_error *next;
};

struct pending_pump {
    char *transfer_id;
    struct pending_pump *next;
};

static struct mft_plugin g_plugin;

static struct transfer_context *find_transfer(const char *transfer_id);
static void unlink_transfer(struct transfer_context *ctx);
static void release_transfer_after_unlink(struct transfer_context *ctx);
static void complete_transfer_error_unlinked(struct transfer_context *ctx,
                                             const char *message,
                                             bool notify_peer);
static int send_pump(struct transfer_context *ctx);
static void schedule_send_pump(struct transfer_context *ctx, unsigned int delay_ms);
static bool accept_item_is_receive(json_t *accept, size_t index);

#ifdef _WIN32
static void mft_lock(void)
{
    if (g_plugin.sync_initialized)
        EnterCriticalSection(&g_plugin.lock);
}

static void mft_unlock(void)
{
    if (g_plugin.sync_initialized)
        LeaveCriticalSection(&g_plugin.lock);
}

static void mft_signal_timer(void)
{
    if (g_plugin.sync_initialized)
        WakeConditionVariable(&g_plugin.timer_cv);
}
#else
static void mft_lock(void)
{
    if (g_plugin.sync_initialized)
        pthread_mutex_lock(&g_plugin.lock);
}

static void mft_unlock(void)
{
    if (g_plugin.sync_initialized)
        pthread_mutex_unlock(&g_plugin.lock);
}

static void mft_signal_timer(void)
{
    if (g_plugin.sync_initialized)
        pthread_cond_signal(&g_plugin.timer_cond);
}
#endif

static char *mft_strdup(const char *value)
{
    size_t len;
    char *copy;

    if (!value)
        return NULL;
    len = strlen(value);
    copy = malloc(len + 1);
    if (!copy)
        return NULL;
    memcpy(copy, value, len + 1);
    return copy;
}

static uint32_t rotr32(uint32_t value, uint32_t count)
{
    return (value >> count) | (value << (32u - count));
}

static void sha256_transform(struct sha256_ctx *ctx, const unsigned char data[64])
{
    static const uint32_t k[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
        0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
        0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
        0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
        0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
        0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
        0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
        0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
    };
    uint32_t m[64];
    uint32_t a, b, c, d, e, f, g, h;
    size_t i;

    for (i = 0; i < 16; i++) {
        m[i] = ((uint32_t)data[i * 4] << 24) |
               ((uint32_t)data[i * 4 + 1] << 16) |
               ((uint32_t)data[i * 4 + 2] << 8) |
               (uint32_t)data[i * 4 + 3];
    }
    for (i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(m[i - 15], 7) ^ rotr32(m[i - 15], 18) ^ (m[i - 15] >> 3);
        uint32_t s1 = rotr32(m[i - 2], 17) ^ rotr32(m[i - 2], 19) ^ (m[i - 2] >> 10);
        m[i] = m[i - 16] + s0 + m[i - 7] + s1;
    }

    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];

    for (i = 0; i < 64; i++) {
        uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + s1 + ch + k[i] + m[i];
        uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

static void sha256_init(struct sha256_ctx *ctx)
{
    ctx->datalen = 0;
    ctx->bitlen = 0;
    ctx->state[0] = 0x6a09e667u;
    ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u;
    ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu;
    ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu;
    ctx->state[7] = 0x5be0cd19u;
}

static void sha256_update(struct sha256_ctx *ctx, const unsigned char *data, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++) {
        ctx->data[ctx->datalen++] = data[i];
        if (ctx->datalen == 64) {
            sha256_transform(ctx, ctx->data);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
}

static void sha256_final(struct sha256_ctx *ctx, unsigned char hash[32])
{
    size_t i = ctx->datalen;
    size_t j;

    ctx->data[i++] = 0x80;
    if (i > 56) {
        while (i < 64)
            ctx->data[i++] = 0;
        sha256_transform(ctx, ctx->data);
        i = 0;
    }
    while (i < 56)
        ctx->data[i++] = 0;

    ctx->bitlen += (uint64_t)ctx->datalen * 8u;
    for (j = 0; j < 8; j++)
        ctx->data[63 - j] = (unsigned char)((ctx->bitlen >> (j * 8)) & 0xffu);
    sha256_transform(ctx, ctx->data);

    for (i = 0; i < 4; i++) {
        for (j = 0; j < 8; j++)
            hash[i + j * 4] = (unsigned char)((ctx->state[j] >> (24 - i * 8)) & 0xffu);
    }
}

static void hash_to_hex(const unsigned char hash[32], char out[65])
{
    static const char hex[] = "0123456789abcdef";
    size_t i;

    for (i = 0; i < 32; i++) {
        out[i * 2] = hex[hash[i] >> 4];
        out[i * 2 + 1] = hex[hash[i] & 0x0f];
    }
    out[64] = '\0';
}

static void mft_sleep_ms(unsigned int milliseconds)
{
#ifdef _WIN32
    Sleep(milliseconds);
#else
    struct timeval delay;

    delay.tv_sec = (long)(milliseconds / 1000u);
    delay.tv_usec = (long)((milliseconds % 1000u) * 1000u);
    select(0, NULL, NULL, NULL, &delay);
#endif
}

static int file_sha256(const char *path, char out[65])
{
    FILE *fp;
    unsigned char buf[32768];
    unsigned char hash[32];
    struct sha256_ctx ctx;
    size_t n;

    fp = fopen(path, "rb");
    if (!fp)
        return -1;

    sha256_init(&ctx);
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
        sha256_update(&ctx, buf, n);
    if (ferror(fp)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    sha256_final(&ctx, hash);
    hash_to_hex(hash, out);
    return 0;
}

static void bytes_sha256(const unsigned char *data, size_t len, char out[65])
{
    unsigned char hash[32];
    struct sha256_ctx ctx;

    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, hash);
    hash_to_hex(hash, out);
}

static int fill_file_hashes(FILE *fp, struct manifest_entry *entry)
{
    unsigned char buf[32768];
    unsigned char hash[32];
    struct sha256_ctx file_ctx;
    struct sha256_ctx block_ctx;
    uint64_t remaining = entry->size;
    uint64_t block_remaining = 0;
    size_t block_index = 0;
    bool block_active = false;

    if (fseek(fp, 0, SEEK_SET) != 0)
        return -1;
    sha256_init(&file_ctx);
    while (remaining > 0) {
        size_t want = remaining > sizeof(buf) ? sizeof(buf) : (size_t)remaining;
        size_t n = fread(buf, 1, want, fp);
        size_t used = 0;

        if (n == 0)
            return -1;
        sha256_update(&file_ctx, buf, n);
        remaining -= n;
        while (used < n) {
            size_t take;

            if (!block_active) {
                if (block_index >= entry->block_count)
                    return -1;
                sha256_init(&block_ctx);
                block_remaining = entry->blocks[block_index].size;
                block_active = true;
            }
            take = n - used;
            if (take > block_remaining)
                take = (size_t)block_remaining;
            sha256_update(&block_ctx, buf + used, take);
            used += take;
            block_remaining -= take;
            if (block_remaining == 0) {
                sha256_final(&block_ctx, hash);
                hash_to_hex(hash, entry->blocks[block_index].hash);
                block_index++;
                block_active = false;
            }
        }
    }
    if (ferror(fp) || block_active || block_index != entry->block_count)
        return -1;
    sha256_final(&file_ctx, hash);
    hash_to_hex(hash, entry->hash);
    return 0;
}

static uint16_t read_u16_be(const unsigned char *data)
{
    return (uint16_t)(((uint16_t)data[0] << 8) | (uint16_t)data[1]);
}

static uint32_t read_u32_be(const unsigned char *data)
{
    return ((uint32_t)data[0] << 24) |
           ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) |
           (uint32_t)data[3];
}

static uint64_t read_u64_be(const unsigned char *data)
{
    uint64_t value = 0;
    size_t i;

    for (i = 0; i < 8; i++)
        value = (value << 8) | data[i];
    return value;
}

static void write_u16_be(unsigned char *data, uint16_t value)
{
    data[0] = (unsigned char)((value >> 8) & 0xffu);
    data[1] = (unsigned char)(value & 0xffu);
}

static void write_u32_be(unsigned char *data, uint32_t value)
{
    data[0] = (unsigned char)((value >> 24) & 0xffu);
    data[1] = (unsigned char)((value >> 16) & 0xffu);
    data[2] = (unsigned char)((value >> 8) & 0xffu);
    data[3] = (unsigned char)(value & 0xffu);
}

static void write_u64_be(unsigned char *data, uint64_t value)
{
    size_t i;

    for (i = 0; i < 8; i++)
        data[7 - i] = (unsigned char)((value >> (i * 8)) & 0xffu);
}

static int path_is_safe_rel(const char *path)
{
    const char *p;

    if (!path || path[0] == '\0' || path[0] == '/')
        return 0;
    if (strcmp(path, ".") == 0)
        return 1;
    for (p = path; *p; p++) {
        if (*p == '\\')
            return 0;
    }
    if (strcmp(path, "..") == 0)
        return 0;
    if (strncmp(path, "../", 3) == 0 || strstr(path, "/../") || strstr(path, "/.."))
        return 0;
    return 1;
}

static bool manifest_is_single_file_root(struct manifest_entry *entries, size_t count)
{
    return count == 1 &&
           entries[0].type == 'f' &&
           strcmp(entries[0].relpath, ".") == 0;
}

enum mft_path_separator_mode {
    MFT_PATH_SEPARATOR_POSIX = 0,
    MFT_PATH_SEPARATOR_WINDOWS = 1,
};

static bool path_is_windows_drive_letter(char ch)
{
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
}

static bool path_has_windows_drive_prefix(const char *path)
{
    return path &&
           path_is_windows_drive_letter(path[0]) &&
           path[1] == ':';
}

static enum mft_path_separator_mode path_native_separator_mode(void)
{
#ifdef _WIN32
    return MFT_PATH_SEPARATOR_WINDOWS;
#else
    return MFT_PATH_SEPARATOR_POSIX;
#endif
}

static enum mft_path_separator_mode path_remote_separator_mode(const char *path)
{
    if (path_has_windows_drive_prefix(path) || (path && strchr(path, '\\')))
        return MFT_PATH_SEPARATOR_WINDOWS;
    return MFT_PATH_SEPARATOR_POSIX;
}

static bool path_separators_are_consistent(const char *path,
                                           enum mft_path_separator_mode mode)
{
    bool saw_forward = false;
    bool saw_back = false;

    if (mode != MFT_PATH_SEPARATOR_WINDOWS)
        return true;
    for (; path && *path; path++) {
        if (*path == '/')
            saw_forward = true;
        else if (*path == '\\')
            saw_back = true;
        if (saw_forward && saw_back)
            return false;
    }
    return true;
}

static bool path_is_separator_for_mode(char ch, enum mft_path_separator_mode mode)
{
    if (mode == MFT_PATH_SEPARATOR_WINDOWS)
        return ch == '/' || ch == '\\';
    return ch == '/';
}

static bool path_is_separator(char ch)
{
    return path_is_separator_for_mode(ch, path_native_separator_mode());
}

static char path_join_separator_for_mode(const char *root,
                                         enum mft_path_separator_mode mode)
{
    if (mode == MFT_PATH_SEPARATOR_WINDOWS && root && strchr(root, '\\'))
        return '\\';
    return '/';
}

static int copy_relpath_with_separator(char *out,
                                       size_t out_len,
                                       const char *rel,
                                       char separator)
{
    size_t i;

    if (!out || out_len == 0 || !rel || strlen(rel) >= out_len)
        return -1;
    for (i = 0; rel[i]; i++)
        out[i] = rel[i] == '/' ? separator : rel[i];
    out[i] = '\0';
    return 0;
}

static bool path_has_trailing_slash(const char *path)
{
    size_t len;

    if (!path)
        return false;
    len = strlen(path);
    return len > 0 && path_is_separator(path[len - 1]);
}

static char *path_basename_dup_for_mode(const char *path,
                                        enum mft_path_separator_mode mode)
{
    const char *end;
    const char *start;
    char name[PATH_MAX];
    size_t len;

    if (!path || path[0] == '\0' || !path_separators_are_consistent(path, mode))
        return NULL;
    end = path + strlen(path);
    while (end > path && path_is_separator_for_mode(end[-1], mode))
        end--;
    if (end == path)
        return NULL;
    start = end;
    while (start > path && !path_is_separator_for_mode(start[-1], mode))
        start--;
    len = (size_t)(end - start);
    if (len == 0 || len >= sizeof(name))
        return NULL;
    memcpy(name, start, len);
    name[len] = '\0';
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0 || !path_is_safe_rel(name))
        return NULL;
    return mft_strdup(name);
}

static char *path_basename_dup(const char *path)
{
    return path_basename_dup_for_mode(path, path_native_separator_mode());
}

static char *path_remote_basename_dup(const char *path)
{
    return path_basename_dup_for_mode(path, path_remote_separator_mode(path));
}

static char *path_dirname_dup_for_mode(const char *path,
                                       enum mft_path_separator_mode mode)
{
    const char *end;
    const char *slash;
    size_t len;
    char dir[PATH_MAX];

    if (!path || path[0] == '\0' || !path_separators_are_consistent(path, mode))
        return NULL;
    end = path + strlen(path);
    while (end > path && path_is_separator_for_mode(end[-1], mode))
        end--;
    if (end == path)
        return NULL;
    slash = end;
    while (slash > path && !path_is_separator_for_mode(slash[-1], mode))
        slash--;
    if (slash == path)
        return mft_strdup(".");
    if (slash == path + 1 && path[0] == '/')
        return mft_strdup("/");
    if (mode == MFT_PATH_SEPARATOR_WINDOWS &&
        slash == path + 3 &&
        path_has_windows_drive_prefix(path) &&
        path_is_separator_for_mode(path[2], mode)) {
        memcpy(dir, path, 3);
        dir[3] = '\0';
        return mft_strdup(dir);
    }
    len = (size_t)(slash - path - 1);
    if (len == 0 || len >= sizeof(dir))
        return NULL;
    memcpy(dir, path, len);
    dir[len] = '\0';
    return mft_strdup(dir);
}

static char *path_dirname_dup(const char *path)
{
    return path_dirname_dup_for_mode(path, path_native_separator_mode());
}

static int join_path(char *out, size_t out_len, const char *root, const char *rel)
{
    enum mft_path_separator_mode mode = path_native_separator_mode();
    char rel_path[PATH_MAX];
    char separator;
    const char *infix;
    char sep_text[2] = {0, 0};
    int len;

    if (!root || !rel)
        return -1;
    if (strcmp(rel, ".") == 0)
        len = snprintf(out, out_len, "%s", root);
    else if (path_is_safe_rel(rel)) {
        separator = path_join_separator_for_mode(root, mode);
        sep_text[0] = separator;
        infix = path_has_trailing_slash(root) ? "" : sep_text;
        if (copy_relpath_with_separator(rel_path, sizeof(rel_path), rel, separator) != 0)
            return -1;
        len = snprintf(out,
                       out_len,
                       "%s%s%s",
                       root,
                       infix,
                       rel_path);
    } else {
        return -1;
    }
    return len >= 0 && (size_t)len < out_len ? 0 : -1;
}

static int append_path_component(char *out, size_t out_len, const char *root, const char *name)
{
    char separator;
    const char *infix;
    char sep_text[2] = {0, 0};
    int len;

    if (!root || !name || !path_is_safe_rel(name) || strcmp(name, ".") == 0)
        return -1;
    separator = path_join_separator_for_mode(root, path_native_separator_mode());
    sep_text[0] = separator;
    infix = path_has_trailing_slash(root) ? "" : sep_text;
    len = snprintf(out,
                   out_len,
                   "%s%s%s",
                   root,
                   infix,
                   name);
    return len >= 0 && (size_t)len < out_len ? 0 : -1;
}

static int part_path(char *out, size_t out_len, const char *final_path)
{
    int len = snprintf(out, out_len, "%s.part", final_path);

    return len >= 0 && (size_t)len < out_len ? 0 : -1;
}

static int ensure_parent_dirs(const char *path)
{
    char tmp[PATH_MAX];
    char *p;

    if (!path || strlen(path) >= sizeof(tmp))
        return -1;
    strcpy(tmp, path);
    p = tmp + 1;
#ifdef _WIN32
    if (path_has_windows_drive_prefix(tmp) && path_is_separator_for_mode(tmp[2], MFT_PATH_SEPARATOR_WINDOWS))
        p = tmp + 3;
#endif
    for (; *p; p++) {
        if (!path_is_separator(*p))
            continue;
        *p = '\0';
        if (mkdir(tmp, 0777) != 0 && errno != EEXIST)
            return -1;
        *p = '/';
    }
    return 0;
}

static int ensure_dir(const char *path, unsigned int mode)
{
    if (mkdir(path, mode ? (mode_t)mode : 0777) != 0 && errno != EEXIST)
        return -1;
    chmod(path, mode ? (mode_t)mode : 0777);
    return 0;
}

static bool path_is_existing_dir(const char *path)
{
    struct stat st;

    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int ensure_dir_with_parents(const char *path, unsigned int mode)
{
    if (ensure_parent_dirs(path) != 0)
        return -1;
    return ensure_dir(path, mode);
}

static int resolve_receive_root(char *out,
                                size_t out_len,
                                const char *target_path,
                                const char *source_name,
                                struct manifest_entry *entries,
                                size_t count)
{
    int len;

    if (!target_path || target_path[0] == '\0')
        return -1;

    if (manifest_is_single_file_root(entries, count) &&
        (path_is_existing_dir(target_path) || path_has_trailing_slash(target_path))) {
        if (!source_name)
            return -1;
        if (path_has_trailing_slash(target_path) && !path_is_existing_dir(target_path) &&
            ensure_dir_with_parents(target_path, 0777) != 0)
            return -1;
        return append_path_component(out, out_len, target_path, source_name);
    }

    len = snprintf(out, out_len, "%s", target_path);
    return len >= 0 && (size_t)len < out_len ? 0 : -1;
}

static uint64_t stat_mtime_ns(const struct stat *st)
{
    return (uint64_t)st->st_mtime * 1000000000ull;
}

static int quick_file_matches(const char *path, const struct manifest_entry *entry)
{
    struct stat st;
    char hash[65];

    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return 0;
    if ((uint64_t)st.st_size != entry->size)
        return 0;
    if (file_sha256(path, hash) != 0)
        return 0;
    return strcmp(hash, entry->hash) == 0;
}

static uint64_t validated_resume_offset(const char *final_path,
                                        const struct manifest_entry *entry)
{
    char path[PATH_MAX];
    char hash[65];
    struct stat st;
    uint64_t size;

    if (part_path(path, sizeof(path), final_path) != 0)
        return 0;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0)
        return 0;
    size = (uint64_t)st.st_size;
    if (size > entry->size)
        return 0;
    if (size == entry->size) {
        if (file_sha256(path, hash) == 0 && strcmp(hash, entry->hash) == 0)
            return entry->size;
        return 0;
    }
    return 0;
}

static int write_all_at(const char *path,
                        const unsigned char *data,
                        size_t len,
                        uint64_t offset)
{
    int fd;
    ssize_t written;
    size_t done = 0;

    if (ensure_parent_dirs(path) != 0)
        return -1;
    fd = open(path, O_CREAT | O_WRONLY | MFT_OPEN_BINARY, 0666);
    if (fd < 0)
        return -1;
    while (done < len) {
        if (lseek(fd, (off_t)(offset + done), SEEK_SET) < 0) {
            close(fd);
            return -1;
        }
#ifdef _WIN32
        {
            size_t remaining = len - done;
            unsigned int chunk = remaining > UINT_MAX ? UINT_MAX : (unsigned int)remaining;

            written = write(fd, data + done, chunk);
        }
#else
        written = write(fd, data + done, len - done);
#endif
        if (written <= 0) {
            close(fd);
            return -1;
        }
        done += (size_t)written;
    }
    close(fd);
    return 0;
}

static int truncate_file(const char *path)
{
    int fd;

    if (ensure_parent_dirs(path) != 0)
        return -1;
    fd = open(path, O_CREAT | O_WRONLY | O_TRUNC | MFT_OPEN_BINARY, 0666);
    if (fd < 0)
        return -1;
    close(fd);
    return 0;
}

static int copy_file_range_chunks(FILE *fp,
                                  uint64_t offset,
                                  uint64_t end_offset,
                                  unsigned int server_id,
                                  const char *transfer_id,
                                  struct transfer_context *ctx,
                                  uint32_t stream_id,
                                  uint64_t *out_next_offset)
{
    unsigned char *frame;
    unsigned char buf[MFT_MAX_CHUNK];
    uint64_t pos = offset;
    uint16_t tid_len;
    int rc;

    if (!transfer_id)
        return -1;
    tid_len = (uint16_t)strlen(transfer_id);

    if (fseek(fp, (long)offset, SEEK_SET) != 0)
        return -1;

    while (pos < end_offset) {
        size_t want = (size_t)((end_offset - pos) > MFT_MAX_CHUNK ? MFT_MAX_CHUNK :
                                                              (end_offset - pos));
        size_t n = fread(buf, 1, want, fp);
        char chunk_hash[65];
        size_t header_len = 4 + 1 + 1 + 2 + 4 + 8 + 4 + 64 + tid_len;

        if (n == 0)
            return -1;
        bytes_sha256(buf, n, chunk_hash);
        frame = malloc(header_len + n);
        if (!frame)
            return -1;

        memcpy(frame, MFT_MAGIC, 4);
        frame[4] = MFT_VERSION;
        frame[5] = MFT_FRAME_DATA;
        write_u16_be(frame + 6, tid_len);
        write_u32_be(frame + 8, stream_id);
        write_u64_be(frame + 12, pos);
        write_u32_be(frame + 20, (uint32_t)n);
        memcpy(frame + 24, chunk_hash, 64);
        memcpy(frame + 88, transfer_id, tid_len);
        memcpy(frame + header_len, buf, n);
        mft_unlock();
        rc = g_plugin.host->peer_transport_send_frame(g_plugin.host->host_context,
                                                      server_id,
                                                      frame,
                                                      (uint32_t)(header_len + n));
        mft_lock();
        free(frame);
        if (ctx->cancelled || ctx->free_after_pump) {
            if (out_next_offset)
                *out_next_offset = pos;
            return -3;
        }
        if (rc != 0) {
            if (out_next_offset)
                *out_next_offset = pos;
            return rc == -2 ? -2 : -1;
        }
        if (touch_transfer_activity(ctx) != 0)
            return -1;
        pos += n;
        if (out_next_offset)
            *out_next_offset = pos;
    }
    return 0;
}

static int send_json_frame(unsigned int server_id, enum mft_frame_type type, json_t *payload)
{
    char *json;
    unsigned char *frame;
    size_t json_len;
    int rc;

    json = json_dumps(payload, JSON_COMPACT | JSON_ENSURE_ASCII);
    if (!json)
        return -1;
    json_len = strlen(json);
    if (json_len > UINT32_MAX - 8) {
        free(json);
        return -1;
    }

    frame = malloc(json_len + 8);
    if (!frame) {
        free(json);
        return -1;
    }
    memcpy(frame, MFT_MAGIC, 4);
    frame[4] = MFT_VERSION;
    frame[5] = (unsigned char)type;
    frame[6] = 0;
    frame[7] = 0;
    memcpy(frame + 8, json, json_len);
    rc = g_plugin.host->peer_transport_send_frame(g_plugin.host->host_context,
                                                  server_id,
                                                  frame,
                                                  (uint32_t)(json_len + 8));
    free(frame);
    free(json);
    return rc;
}

static char *make_transfer_id(void)
{
    unsigned long long now = g_plugin.host->now_ms(g_plugin.host->host_context);
    int len = snprintf(NULL, 0, "mft-%llu-%llu", now, ++g_plugin.next_transfer);
    char *id;

    if (len < 0)
        return NULL;
    id = malloc((size_t)len + 1);
    if (!id)
        return NULL;
    snprintf(id, (size_t)len + 1, "mft-%llu-%llu", now, g_plugin.next_transfer);
    return id;
}

static void free_entries(struct manifest_entry *entries, size_t count)
{
    size_t i;

    for (i = 0; i < count; i++) {
        free(entries[i].relpath);
        free(entries[i].blocks);
    }
    free(entries);
}

static int append_entry(struct manifest_entry **entries,
                        size_t *count,
                        size_t *capacity,
                        const struct manifest_entry *entry)
{
    struct manifest_entry *next;

    if (*count == *capacity) {
        size_t next_capacity = *capacity ? *capacity * 2 : 16;
        next = realloc(*entries, next_capacity * sizeof(**entries));
        if (!next)
            return -1;
        *entries = next;
        *capacity = next_capacity;
    }
    (*entries)[*count] = *entry;
    (*count)++;
    return 0;
}

static int build_entry(const char *path,
                       const char *relpath,
                       struct manifest_entry *entry)
{
    struct stat st;
    FILE *fp = NULL;
    size_t i;

    if (stat(path, &st) != 0)
        return -1;
    memset(entry, 0, sizeof(*entry));
    entry->relpath = mft_strdup(relpath);
    if (!entry->relpath)
        return -1;
    entry->mode = (unsigned int)(st.st_mode & 0777u);
    entry->mtime_ns = stat_mtime_ns(&st);

    if (S_ISDIR(st.st_mode)) {
        entry->type = 'd';
        return 0;
    }
    if (!S_ISREG(st.st_mode)) {
        free(entry->relpath);
        memset(entry, 0, sizeof(*entry));
        return -2;
    }
    entry->type = 'f';
    entry->size = (uint64_t)st.st_size;
    entry->block_count = entry->size == 0 ? 0 :
                             (size_t)((entry->size + MFT_LOGICAL_BLOCK_SIZE - 1) /
                                      MFT_LOGICAL_BLOCK_SIZE);
    if (entry->block_count == 0) {
        if (file_sha256(path, entry->hash) != 0) {
            free(entry->relpath);
            memset(entry, 0, sizeof(*entry));
            return -1;
        }
        return 0;
    }
    entry->blocks = calloc(entry->block_count, sizeof(*entry->blocks));
    if (!entry->blocks) {
        free(entry->relpath);
        memset(entry, 0, sizeof(*entry));
        return -1;
    }
    fp = fopen(path, "rb");
    if (!fp) {
        free(entry->blocks);
        free(entry->relpath);
        memset(entry, 0, sizeof(*entry));
        return -1;
    }
    for (i = 0; i < entry->block_count; i++) {
        uint64_t offset = (uint64_t)i * MFT_LOGICAL_BLOCK_SIZE;
        uint64_t remaining = entry->size - offset;

        entry->blocks[i].offset = offset;
        entry->blocks[i].size = remaining > MFT_LOGICAL_BLOCK_SIZE ?
                                    MFT_LOGICAL_BLOCK_SIZE :
                                    remaining;
    }
    if (fill_file_hashes(fp, entry) != 0) {
        fclose(fp);
        free(entry->blocks);
        free(entry->relpath);
        memset(entry, 0, sizeof(*entry));
        return -1;
    }
    fclose(fp);
    return 0;
}

static int scan_path(const char *root,
                     const char *relpath,
                     struct manifest_entry **entries,
                     size_t *count,
                     size_t *capacity)
{
    char path[PATH_MAX];
    struct manifest_entry entry;
    DIR *dir;
    struct dirent *dent;
    int rc;

    if (join_path(path, sizeof(path), root, relpath) != 0)
        return -1;
    rc = build_entry(path, relpath, &entry);
    if (rc != 0)
        return rc;
    if (append_entry(entries, count, capacity, &entry) != 0) {
        free(entry.relpath);
        return -1;
    }
    if (entry.type != 'd')
        return 0;

    dir = opendir(path);
    if (!dir)
        return -1;
    while ((dent = readdir(dir)) != NULL) {
        char child_rel[PATH_MAX];
        int len;

        if (strcmp(dent->d_name, ".") == 0 || strcmp(dent->d_name, "..") == 0)
            continue;
        if (strcmp(relpath, ".") == 0)
            len = snprintf(child_rel, sizeof(child_rel), "%s", dent->d_name);
        else
            len = snprintf(child_rel, sizeof(child_rel), "%s/%s", relpath, dent->d_name);
        if (len < 0 || (size_t)len >= sizeof(child_rel)) {
            closedir(dir);
            return -1;
        }
        if (scan_path(root, child_rel, entries, count, capacity) != 0) {
            closedir(dir);
            return -1;
        }
    }
    closedir(dir);
    return 0;
}

static json_t *entries_to_manifest(struct manifest_entry *entries, size_t count)
{
    json_t *manifest = json_object();
    json_t *array = json_array();
    size_t i;

    for (i = 0; i < count; i++) {
        struct manifest_entry *entry = &entries[i];
        json_t *item = json_pack("{s:s,s:s,s:I,s:I,s:i}",
                                 "relpath",
                                 entry->relpath,
                                 "type",
                                 entry->type == 'd' ? "directory" : "file",
                                 "size",
                                 (json_int_t)entry->size,
                                 "mtime_ns",
                                 (json_int_t)entry->mtime_ns,
                                 "mode",
                                 (int)entry->mode);
        if (entry->type == 'f') {
            json_t *blocks = json_array();
            size_t block_index;

            json_object_set_new(item, "hash", json_string(entry->hash));
            json_object_set_new(item,
                                "block_size",
                                json_integer((json_int_t)MFT_LOGICAL_BLOCK_SIZE));
            for (block_index = 0; block_index < entry->block_count; block_index++) {
                struct block_entry *block = &entry->blocks[block_index];

                json_array_append_new(blocks,
                                      json_pack("{s:I,s:I,s:s}",
                                                "offset",
                                                (json_int_t)block->offset,
                                                "size",
                                                (json_int_t)block->size,
                                                "hash",
                                                block->hash));
            }
            json_object_set_new(item, "blocks", blocks);
        }
        json_array_append_new(array, item);
    }
    json_object_set_new(manifest, "entries", array);
    return manifest;
}

static int manifest_to_entries(json_t *manifest,
                               struct manifest_entry **out_entries,
                               size_t *out_count)
{
    json_t *array;
    size_t index;
    json_t *item;
    struct manifest_entry *entries = NULL;
    size_t count = 0;

    *out_entries = NULL;
    *out_count = 0;
    array = json_object_get(manifest, "entries");
    if (!json_is_array(array))
        return -1;

    entries = calloc(json_array_size(array), sizeof(*entries));
    if (!entries && json_array_size(array) != 0)
        return -1;

    json_array_foreach(array, index, item) {
        json_t *relpath = json_object_get(item, "relpath");
        json_t *type = json_object_get(item, "type");
        json_t *size = json_object_get(item, "size");
        json_t *mtime_ns = json_object_get(item, "mtime_ns");
        json_t *mode = json_object_get(item, "mode");
        json_t *hash = json_object_get(item, "hash");
        json_t *blocks = json_object_get(item, "blocks");
        struct manifest_entry *entry = &entries[count];

        if (!json_is_string(relpath) ||
            !path_is_safe_rel(json_string_value(relpath)) ||
            !json_is_string(type) ||
            !json_is_integer(size) ||
            !json_is_integer(mtime_ns) ||
            !json_is_integer(mode)) {
            free_entries(entries, count);
            return -1;
        }
        entry->relpath = mft_strdup(json_string_value(relpath));
        if (!entry->relpath) {
            free_entries(entries, count);
            return -1;
        }
        entry->type = strcmp(json_string_value(type), "directory") == 0 ? 'd' : 'f';
        entry->size = (uint64_t)json_integer_value(size);
        entry->mtime_ns = (uint64_t)json_integer_value(mtime_ns);
        entry->mode = (unsigned int)json_integer_value(mode);
        if (entry->type == 'f') {
            size_t block_index;
            uint64_t expected_offset = 0;

            if (!json_is_string(hash) || strlen(json_string_value(hash)) != 64) {
                free_entries(entries, count + 1);
                return -1;
            }
            snprintf(entry->hash, sizeof(entry->hash), "%s", json_string_value(hash));
            if (!json_is_array(blocks)) {
                free_entries(entries, count + 1);
                return -1;
            }
            entry->block_count = json_array_size(blocks);
            if ((entry->size == 0 && entry->block_count != 0) ||
                (entry->size != 0 && entry->block_count == 0)) {
                free_entries(entries, count + 1);
                return -1;
            }
            if (entry->block_count > 0) {
                entry->blocks = calloc(entry->block_count, sizeof(*entry->blocks));
                if (!entry->blocks) {
                    free_entries(entries, count + 1);
                    return -1;
                }
            }
            for (block_index = 0; block_index < entry->block_count; block_index++) {
                json_t *block_item = json_array_get(blocks, block_index);
                json_t *offset = json_object_get(block_item, "offset");
                json_t *block_size = json_object_get(block_item, "size");
                json_t *block_hash = json_object_get(block_item, "hash");
                struct block_entry *block = &entry->blocks[block_index];

                if (!json_is_integer(offset) ||
                    !json_is_integer(block_size) ||
                    !json_is_string(block_hash) ||
                    strlen(json_string_value(block_hash)) != 64) {
                    free_entries(entries, count + 1);
                    return -1;
                }
                block->offset = (uint64_t)json_integer_value(offset);
                block->size = (uint64_t)json_integer_value(block_size);
                if (block->offset != expected_offset ||
                    block->size == 0 ||
                    block->size > MFT_LOGICAL_BLOCK_SIZE ||
                    block->offset + block->size > entry->size) {
                    free_entries(entries, count + 1);
                    return -1;
                }
                snprintf(block->hash, sizeof(block->hash), "%s", json_string_value(block_hash));
                expected_offset += block->size;
            }
            if (expected_offset != entry->size) {
                free_entries(entries, count + 1);
                return -1;
            }
        }
        count++;
    }

    *out_entries = entries;
    *out_count = count;
    return 0;
}

static int send_hello(unsigned int server_id)
{
    json_t *payload = json_pack("{s:i,s:[s,s,s],s:i,s:i,s:i,s:i}",
                                "version",
                                1,
                                "capabilities",
                                MFT_CAP_WHOLE_FILE,
                                MFT_CAP_RESUME,
                                MFT_CAP_BLOCK_ACK,
                                "max_frame_size",
                                1024 * 1024,
                                "initial_session_window",
                                1024 * 1024,
                                "initial_stream_window",
                                256 * 1024,
                                "max_concurrent_streams",
                                MFT_MAX_CONCURRENT_STREAMS);
    int rc = send_json_frame(server_id, MFT_FRAME_HELLO, payload);

    json_decref(payload);
    return rc;
}

static void on_peer_connected(void *user_data, unsigned int server_id)
{
    (void)user_data;
    send_hello(server_id);
}

static void abort_transfers_for_peer(unsigned int server_id);

static void on_peer_closed(void *user_data, unsigned int server_id)
{
    (void)user_data;
    abort_transfers_for_peer(server_id);
}

static void handle_hello(unsigned int server_id, json_t *payload)
{
    json_t *capabilities = json_object_get(payload, "capabilities");
    json_t *cap;
    size_t i;
    int should_reply = 0;

    json_array_foreach(capabilities, i, cap) {
        const char *name;

        if (!json_is_string(cap))
            continue;
        name = json_string_value(cap);
        if (strcmp(name, MFT_CAP_WHOLE_FILE) == 0 ||
            strcmp(name, MFT_CAP_RESUME) == 0 ||
            strcmp(name, MFT_CAP_BLOCK_ACK) == 0) {
            if (!g_plugin.host->peer_transport_has_capability(g_plugin.host->host_context,
                                                              server_id,
                                                              name))
                should_reply = 1;
            g_plugin.host->peer_transport_set_capability(g_plugin.host->host_context,
                                                         server_id,
                                                         name,
                                                         1);
        }
    }
    if (should_reply)
        send_hello(server_id);
}

static int ensure_capability(unsigned int server_id)
{
    unsigned int i;

    if (g_plugin.host->peer_transport_has_capability(g_plugin.host->host_context,
                                                     server_id,
                                                     MFT_CAP_BLOCK_ACK))
        return 0;
    send_hello(server_id);
    for (i = 0; i < 50; i++) {
        if (g_plugin.host->peer_transport_has_capability(g_plugin.host->host_context,
                                                         server_id,
                                                         MFT_CAP_BLOCK_ACK))
            return 0;
        mft_sleep_ms(20);
    }
    return -1;
}

static int prepare_accept(const char *target_root,
                          struct manifest_entry *entries,
                          size_t count,
                          json_t **out_accept)
{
    json_t *accept = json_array();
    size_t i;

    for (i = 0; i < count; i++) {
        struct manifest_entry *entry = &entries[i];
        char final_path[PATH_MAX];
        char tmp_part[PATH_MAX];
        const char *decision = "receive";
        uint64_t resume_offset = 0;

        if (join_path(final_path, sizeof(final_path), target_root, entry->relpath) != 0) {
            json_decref(accept);
            return -1;
        }
        if (entry->type == 'd') {
            if (ensure_dir_with_parents(final_path, entry->mode) != 0) {
                json_decref(accept);
                return -1;
            }
            decision = "create_directory";
        } else {
            if (quick_file_matches(final_path, entry))
                decision = "skip";
            else {
                resume_offset = validated_resume_offset(final_path, entry);
                if (part_path(tmp_part, sizeof(tmp_part), final_path) != 0 ||
                    ensure_parent_dirs(tmp_part) != 0) {
                    json_decref(accept);
                    return -1;
                }
                if (resume_offset == 0 && truncate_file(tmp_part) != 0) {
                    json_decref(accept);
                    return -1;
                }
            }
        }
        json_array_append_new(accept,
                              json_pack("{s:s,s:s,s:I,s:i}",
                                        "relpath",
                                        entry->relpath,
                                        "decision",
                                        decision,
                                        "resume_offset",
                                        (json_int_t)resume_offset,
                                        "stream_id",
                                        (int)(i + 1)));
    }

    *out_accept = accept;
    return 0;
}

static int part_path_for_relpath(const char *target_root,
                                 const char *relpath,
                                 char *out,
                                 size_t out_len)
{
    char final_path[PATH_MAX];

    if (!target_root || !relpath ||
        join_path(final_path, sizeof(final_path), target_root, relpath) != 0 ||
        part_path(out, out_len, final_path) != 0)
        return -1;
    return 0;
}

static int send_block_ack_raw(unsigned int server_id,
                              const char *transfer_id,
                              uint32_t stream_id,
                              size_t block_index,
                              uint64_t offset,
                              uint64_t size,
                              bool ok)
{
    json_t *payload;
    int rc;

    payload = json_pack("{s:s,s:i,s:i,s:I,s:I,s:b}",
                        "transfer_id",
                        transfer_id,
                        "stream_id",
                        (int)stream_id,
                        "block_index",
                        (int)block_index,
                        "offset",
                        (json_int_t)offset,
                        "size",
                        (json_int_t)size,
                        "ok",
                        ok);
    if (!payload)
        return -1;
    if (!ok) {
        json_t *missing = json_array();

        if (!missing) {
            json_decref(payload);
            return -1;
        }
        json_array_append_new(missing,
                              json_pack("{s:i,s:i,s:I,s:I}",
                                        "stream_id",
                                        (int)stream_id,
                                        "block_index",
                                        (int)block_index,
                                        "offset",
                                        (json_int_t)offset,
                                        "size",
                                        (json_int_t)size));
        json_object_set_new(payload, "missing", missing);
    }
    rc = send_json_frame(server_id, MFT_FRAME_ACK, payload);
    json_decref(payload);
    return rc;
}

static void free_pending_acks(struct pending_block_ack *ack)
{
    while (ack) {
        struct pending_block_ack *next = ack->next;

        free(ack->transfer_id);
        free(ack);
        ack = next;
    }
}

static int mark_block_nack(struct transfer_context *ctx,
                           uint32_t stream_id,
                           size_t block_index)
{
    struct manifest_entry *entry;
    struct block_entry *block;

    if (!ctx || stream_id == 0 || stream_id > ctx->entry_count)
        return -1;
    entry = &ctx->entries[stream_id - 1];
    if (block_index >= entry->block_count)
        return -1;
    block = &entry->blocks[block_index];
    if (block->retries >= MFT_MAX_BLOCK_RETRIES)
        return -2;
    block->retries++;
    block->receive_waiting = true;
    block->receive_hash_started = false;
    block->receive_hash_failed = true;
    block->receive_next_offset = block->offset;
    block->receive_deadline_ms =
        g_plugin.host->now_ms(g_plugin.host->host_context) + MFT_BLOCK_TIMEOUT_MS;
    return 0;
}

static void handle_ack(unsigned int server_id, json_t *payload)
{
    json_t *transfer_id = json_object_get(payload, "transfer_id");
    json_t *stream_id_json = json_object_get(payload, "stream_id");
    json_t *block_index_json = json_object_get(payload, "block_index");
    json_t *ok_json = json_object_get(payload, "ok");
    struct transfer_context *ctx;
    struct manifest_entry *entry;
    struct block_entry *block;
    uint32_t stream_id;
    size_t block_index;

    (void)server_id;
    if (!json_is_string(transfer_id) ||
        !json_is_integer(stream_id_json) ||
        !json_is_integer(block_index_json) ||
        !json_is_boolean(ok_json))
        return;
    mft_lock();
    ctx = find_transfer(json_string_value(transfer_id));
    if (!ctx || strcmp(ctx->direction, "send") != 0) {
        mft_unlock();
        return;
    }
    stream_id = (uint32_t)json_integer_value(stream_id_json);
    if (stream_id == 0 || stream_id > ctx->entry_count) {
        mft_unlock();
        return;
    }
    entry = &ctx->entries[stream_id - 1];
    block_index = (size_t)json_integer_value(block_index_json);
    if (entry->type != 'f' || block_index >= entry->block_count) {
        mft_unlock();
        return;
    }
    block = &entry->blocks[block_index];
    if (json_boolean_value(ok_json)) {
        if (!block->acked) {
            block->acked = true;
            block->ack_waiting = false;
            block->resend_pending = false;
            block->ack_deadline_ms = 0;
            ctx->bytes_transferred += block->size;
        }
    } else {
        block->ack_waiting = false;
        block->ack_deadline_ms = 0;
        if (block->retries >= MFT_MAX_BLOCK_RETRIES) {
            unlink_transfer(ctx);
            mft_unlock();
            complete_transfer_error_unlinked(ctx,
                                             "File transfer block retry limit exceeded.",
                                             true);
            return;
        }
        block->retries++;
        block->resend_pending = true;
        block->send_next_offset = block->offset;
        if (block_index < ctx->send_block_index || stream_id - 1 < ctx->send_entry_index) {
            ctx->send_entry_index = stream_id - 1;
            ctx->send_block_index = block_index;
        }
    }
    touch_transfer_activity(ctx);
    schedule_send_pump(ctx, MFT_PUMP_RETRY_MS);
    mft_unlock();
    mft_signal_timer();
}

static bool entry_should_receive(struct transfer_context *ctx, size_t index)
{
    return ctx && ctx->entries[index].type == 'f' && accept_item_is_receive(ctx->accept, index);
}

static int advance_receive_wait(struct transfer_context *ctx)
{
    unsigned long long now;

    if (!ctx || strcmp(ctx->direction, "receive") != 0)
        return -1;
    now = g_plugin.host->now_ms(g_plugin.host->host_context);
    while (ctx->recv_entry_index < ctx->entry_count) {
        struct manifest_entry *entry = &ctx->entries[ctx->recv_entry_index];

        if (!entry_should_receive(ctx, ctx->recv_entry_index)) {
            ctx->recv_entry_index++;
            ctx->recv_block_index = 0;
            continue;
        }
        while (ctx->recv_block_index < entry->block_count &&
               entry->blocks[ctx->recv_block_index].received_ok)
            ctx->recv_block_index++;
        if (ctx->recv_block_index < entry->block_count) {
            struct block_entry *block = &entry->blocks[ctx->recv_block_index];

            if (!block->receive_waiting) {
                block->receive_waiting = true;
                block->receive_deadline_ms = now + MFT_BLOCK_TIMEOUT_MS;
            }
            return 0;
        }
        ctx->recv_entry_index++;
        ctx->recv_block_index = 0;
    }
    return 0;
}

static void mark_receive_resume_blocks(struct transfer_context *ctx)
{
    size_t i;

    if (!ctx || !ctx->accept)
        return;
    for (i = 0; i < ctx->entry_count; i++) {
        struct manifest_entry *entry = &ctx->entries[i];
        json_t *accept_item = json_array_get(ctx->accept, i);
        json_t *resume = accept_item ? json_object_get(accept_item, "resume_offset") : NULL;
        uint64_t resume_offset = json_is_integer(resume) ? (uint64_t)json_integer_value(resume) : 0;
        size_t block_index;

        if (!entry_should_receive(ctx, i))
            continue;
        for (block_index = 0; block_index < entry->block_count; block_index++) {
            struct block_entry *block = &entry->blocks[block_index];

            if (resume_offset >= block->offset + block->size)
                block->received_ok = true;
        }
    }
}

static int finalize_received(const char *target_root,
                             struct manifest_entry *entries,
                             size_t count,
                             json_t *accept,
                             unsigned int *files_skipped,
                             unsigned int *files_transferred)
{
    size_t i;

    *files_skipped = 0;
    *files_transferred = 0;
    for (i = 0; i < count; i++) {
        struct manifest_entry *entry = &entries[i];
        char final_path[PATH_MAX];
        char tmp_part[PATH_MAX];
        struct stat st;
        json_t *decision_item = json_array_get(accept, i);
        json_t *decision = decision_item ? json_object_get(decision_item, "decision") : NULL;

        if (entry->type == 'd')
            continue;
        if (json_is_string(decision) && strcmp(json_string_value(decision), "skip") == 0) {
            (*files_skipped)++;
            continue;
        }
        if (join_path(final_path, sizeof(final_path), target_root, entry->relpath) != 0 ||
            part_path(tmp_part, sizeof(tmp_part), final_path) != 0)
            return -1;
        if (stat(tmp_part, &st) != 0 || !S_ISREG(st.st_mode) ||
            (uint64_t)st.st_size != entry->size)
            return -1;
        chmod(tmp_part, entry->mode ? (mode_t)entry->mode : 0666);
        if (rename(tmp_part, final_path) != 0)
            return -1;
        (*files_transferred)++;
    }
    return 0;
}

static int send_transfer_complete(struct transfer_context *ctx)
{
    json_t *complete;

    if (g_plugin.host && g_plugin.host->log_info) {
        char msg[256];

        snprintf(msg,
                 sizeof(msg),
                 "mft send complete transfer=%s bytes=%llu",
                 ctx && ctx->transfer_id ? ctx->transfer_id : "",
                 (unsigned long long)(ctx ? ctx->bytes_transferred : 0));
        g_plugin.host->log_info(g_plugin.host->host_context, msg);
    }
    complete = json_pack("{s:s,s:I,s:I,s:i,s:i}",
                         "transfer_id",
                         ctx->transfer_id,
                         "bytes_total",
                         (json_int_t)ctx->bytes_total,
                         "bytes_transferred",
                         (json_int_t)ctx->bytes_transferred,
                         "files_transferred",
                         (int)ctx->files_transferred,
                         "files_skipped",
                         (int)ctx->files_skipped);
    if (!complete)
        return -1;
    mft_unlock();
    if (send_json_frame(ctx->server_id, MFT_FRAME_COMPLETE, complete) != 0) {
        mft_lock();
        json_decref(complete);
        return -1;
    }
    mft_lock();
    json_decref(complete);
    if (ctx->cancelled || ctx->free_after_pump)
        return -3;
    return touch_transfer_activity(ctx);
}

static int open_send_file_for_entry(struct transfer_context *ctx)
{
    char path[PATH_MAX];
    struct manifest_entry *entry = &ctx->entries[ctx->send_entry_index];

    if (join_path(path, sizeof(path), ctx->local_path, entry->relpath) != 0)
        return -1;
    ctx->send_fp = fopen(path, "rb");
    return ctx->send_fp ? 0 : -1;
}

static bool accept_item_is_receive(json_t *accept, size_t index)
{
    json_t *accept_item = json_array_get(accept, index);
    json_t *decision = accept_item ? json_object_get(accept_item, "decision") : NULL;

    return json_is_string(decision) && strcmp(json_string_value(decision), "receive") == 0;
}

static bool send_blocks_all_acked(struct transfer_context *ctx)
{
    size_t i;

    for (i = 0; i < ctx->entry_count; i++) {
        struct manifest_entry *entry = &ctx->entries[i];
        size_t block_index;

        if (entry->type != 'f' || !accept_item_is_receive(ctx->accept, i))
            continue;
        for (block_index = 0; block_index < entry->block_count; block_index++) {
            if (!entry->blocks[block_index].acked)
                return false;
        }
    }
    return true;
}

static bool receive_blocks_all_ok(struct transfer_context *ctx)
{
    size_t i;

    if (!ctx)
        return false;
    for (i = 0; i < ctx->entry_count; i++) {
        struct manifest_entry *entry = &ctx->entries[i];
        size_t block_index;

        if (entry->type != 'f' || !entry_should_receive(ctx, i))
            continue;
        for (block_index = 0; block_index < entry->block_count; block_index++) {
            if (!entry->blocks[block_index].received_ok)
                return false;
        }
    }
    return true;
}

static void log_first_missing_receive_block(struct transfer_context *ctx)
{
    size_t i;

    if (!ctx || !g_plugin.host || !g_plugin.host->log_error)
        return;
    for (i = 0; i < ctx->entry_count; i++) {
        struct manifest_entry *entry = &ctx->entries[i];
        size_t block_index;

        if (entry->type != 'f' || !entry_should_receive(ctx, i))
            continue;
        for (block_index = 0; block_index < entry->block_count; block_index++) {
            struct block_entry *block = &entry->blocks[block_index];

            if (!block->received_ok) {
                char msg[256];

                snprintf(msg,
                         sizeof(msg),
                         "mft missing block transfer=%s stream=%lu block=%lu offset=%llu size=%llu next=%llu retries=%u hash_started=%d hash_failed=%d",
                         ctx->transfer_id ? ctx->transfer_id : "",
                         (unsigned long)(i + 1),
                         (unsigned long)block_index,
                         (unsigned long long)block->offset,
                         (unsigned long long)block->size,
                         (unsigned long long)block->receive_next_offset,
                         block->retries,
                         block->receive_hash_started ? 1 : 0,
                         block->receive_hash_failed ? 1 : 0);
                g_plugin.host->log_error(g_plugin.host->host_context, msg);
                return;
            }
        }
    }
}

static unsigned int count_send_blocks_in_flight(struct transfer_context *ctx)
{
    unsigned int count = 0;
    size_t i;

    if (!ctx)
        return 0;
    for (i = 0; i < ctx->entry_count; i++) {
        struct manifest_entry *entry = &ctx->entries[i];
        size_t block_index;

        if (entry->type != 'f')
            continue;
        for (block_index = 0; block_index < entry->block_count; block_index++) {
            struct block_entry *block = &entry->blocks[block_index];

            if (block->ack_waiting && !block->acked)
                count++;
        }
    }
    return count;
}

static int send_block_frames(struct transfer_context *ctx,
                             struct manifest_entry *entry,
                             struct block_entry *block,
                             uint32_t stream_id)
{
    uint64_t block_end = block->offset + block->size;
    uint64_t next_offset = block->send_next_offset;
    int rc;

    (void)entry;
    if (block->send_next_offset == 0 || block->send_next_offset < block->offset ||
        block->send_next_offset > block_end)
        block->send_next_offset = block->offset;
    rc = copy_file_range_chunks(ctx->send_fp,
                                block->send_next_offset,
                                block_end,
                                ctx->server_id,
                                ctx->transfer_id,
                                ctx,
                                stream_id,
                                &next_offset);
    if (rc != 0) {
        block->send_next_offset = next_offset;
        return rc;
    }
    block->send_next_offset = block_end;
    return 0;
}

static void schedule_send_pump(struct transfer_context *ctx, unsigned int delay_ms)
{
    unsigned long long now;

    if (!ctx || !g_plugin.host || !g_plugin.host->now_ms)
        return;
    now = g_plugin.host->now_ms(g_plugin.host->host_context);
    ctx->send_paused = true;
    ctx->send_pump_deadline_ms = now > (unsigned long long)-1 - delay_ms ?
                                     (unsigned long long)-1 :
                                     now + delay_ms;
    mft_signal_timer();
}

static int send_pump(struct transfer_context *ctx)
{
    struct manifest_entry *entry;
    struct block_entry *block;
    unsigned int blocks_sent = 0;
    unsigned int blocks_in_flight;
    int result = 0;

    if (!ctx)
        return -1;
    if (ctx->cancelled || ctx->free_after_pump)
        return 0;
    if (ctx->pumping) {
        schedule_send_pump(ctx, MFT_PUMP_RETRY_MS);
        return 0;
    }
    ctx->pumping = true;
    ctx->send_paused = false;
    ctx->send_pump_deadline_ms = 0;
    blocks_in_flight = count_send_blocks_in_flight(ctx);
    if (g_plugin.host && g_plugin.host->log_info) {
        char msg[256];

        snprintf(msg,
                 sizeof(msg),
                 "mft send pump transfer=%s entry=%lu block=%lu inflight=%u",
                 ctx->transfer_id ? ctx->transfer_id : "",
                 (unsigned long)ctx->send_entry_index,
                 (unsigned long)ctx->send_block_index,
                 blocks_in_flight);
        g_plugin.host->log_info(g_plugin.host->host_context, msg);
    }
    while (ctx->send_entry_index < ctx->entry_count) {
        entry = &ctx->entries[ctx->send_entry_index];
        if (entry->type != 'f') {
            ctx->send_entry_index++;
            ctx->send_block_index = 0;
            continue;
        }
        if (!accept_item_is_receive(ctx->accept, ctx->send_entry_index)) {
            ctx->send_entry_index++;
            ctx->send_block_index = 0;
            continue;
        }
        if (!ctx->send_fp && open_send_file_for_entry(ctx) != 0) {
            result = -1;
            goto done;
        }
        if (ctx->send_block_index >= entry->block_count) {
            fclose(ctx->send_fp);
            ctx->send_fp = NULL;
            ctx->send_entry_index++;
            ctx->send_block_index = 0;
            continue;
        }

        block = &entry->blocks[ctx->send_block_index];
        if (block->acked) {
            ctx->send_block_index++;
            continue;
        }
        if (block->ack_waiting && !block->resend_pending) {
            ctx->send_block_index++;
            continue;
        }
        if (blocks_in_flight >= MFT_MAX_IN_FLIGHT_BLOCKS)
            goto done;
        result = send_block_frames(ctx,
                                   entry,
                                   block,
                                   (uint32_t)(ctx->send_entry_index + 1));
        if (result == -2) {
            schedule_send_pump(ctx, MFT_PUMP_RETRY_MS);
            result = 0;
            goto done;
        }
        if (result == -3) {
            result = 0;
            goto done;
        }
        if (result != 0) {
            goto done;
        }
        block->sent = true;
        block->ack_waiting = true;
        block->resend_pending = false;
        block->send_next_offset = block->offset;
        block->ack_deadline_ms =
            g_plugin.host->now_ms(g_plugin.host->host_context) + MFT_BLOCK_TIMEOUT_MS;
        ctx->send_block_index++;
        touch_transfer_activity(ctx);
        blocks_sent++;
        blocks_in_flight++;
        if (blocks_sent >= MFT_PUMP_MAX_BLOCKS) {
            schedule_send_pump(ctx, MFT_PUMP_RETRY_MS);
            goto done;
        }
    }

    if (!ctx->send_complete_sent && send_blocks_all_acked(ctx)) {
        ctx->send_complete_sent = true;
        result = send_transfer_complete(ctx);
    }

done:
    ctx->pumping = false;
    if (ctx->free_after_pump) {
        release_transfer_after_unlink(ctx);
        return 0;
    }
    return result;
}

static int prepare_send_entries(struct transfer_context *ctx,
                                json_t *accept)
{
    size_t i;

    ctx->accept = json_incref(accept);
    ctx->send_entry_index = 0;
    ctx->send_block_index = 0;
    ctx->bytes_total = 0;
    ctx->bytes_transferred = 0;
    ctx->files_skipped = 0;
    ctx->files_transferred = 0;
    if (g_plugin.host && g_plugin.host->log_info) {
        char msg[256];

        snprintf(msg,
                 sizeof(msg),
                 "mft send prepare transfer=%s entries=%lu",
                 ctx->transfer_id ? ctx->transfer_id : "",
                 (unsigned long)ctx->entry_count);
        g_plugin.host->log_info(g_plugin.host->host_context, msg);
    }

    for (i = 0; i < ctx->entry_count; i++) {
        struct manifest_entry *entry = &ctx->entries[i];
        json_t *accept_item = json_array_get(accept, i);
        json_t *decision = accept_item ? json_object_get(accept_item, "decision") : NULL;
        json_t *resume = accept_item ? json_object_get(accept_item, "resume_offset") : NULL;
        uint64_t resume_offset = json_is_integer(resume) ? (uint64_t)json_integer_value(resume) : 0;
        size_t block_index;

        if (entry->type != 'f')
            continue;
        ctx->bytes_total += entry->size;
        if (json_is_string(decision) && strcmp(json_string_value(decision), "skip") == 0) {
            ctx->files_skipped++;
            continue;
        }
        if (json_is_string(decision) && strcmp(json_string_value(decision), "receive") == 0)
            ctx->files_transferred++;
        for (block_index = 0; block_index < entry->block_count; block_index++) {
            struct block_entry *block = &entry->blocks[block_index];

            if (resume_offset >= block->offset + block->size) {
                block->sent = true;
                block->acked = true;
                ctx->bytes_transferred += block->size;
            }
        }
    }
    schedule_send_pump(ctx, MFT_PUMP_RETRY_MS);
    return 0;
}

static int send_summary(const char *invocation_id,
                        const char *transfer_id,
                        unsigned int server_id,
                        const char *direction,
                        size_t files_total,
                        unsigned int files_transferred,
                        unsigned int files_skipped,
                        uint64_t bytes_total,
                        uint64_t bytes_transferred,
                        unsigned long long started_ms)
{
    unsigned long long now = g_plugin.host->now_ms(g_plugin.host->host_context);
    json_t *summary = json_pack("{s:s,s:i,s:s,s:i,s:i,s:i,s:i,s:I,s:I,s:I,s:[]}",
                                "transfer_id",
                                transfer_id,
                                "server_id",
                                (int)server_id,
                                "direction",
                                direction,
                                "files_total",
                                (int)files_total,
                                "files_transferred",
                                (int)files_transferred,
                                "files_skipped",
                                (int)files_skipped,
                                "files_failed",
                                0,
                                "bytes_total",
                                (json_int_t)bytes_total,
                                "bytes_transferred",
                                (json_int_t)bytes_transferred,
                                "duration_ms",
                                (json_int_t)(now - started_ms),
                                "errors");
    char *dump;
    int rc;

    if (!summary)
        return -1;
    dump = json_dumps(summary, JSON_COMPACT | JSON_ENSURE_ASCII);
    rc = dump ? g_plugin.host->complete_async_ok(g_plugin.host->host_context,
                                                 invocation_id,
                                                 dump) : -1;
    free(dump);
    json_decref(summary);
    return rc;
}

static struct transfer_context *g_transfers;

static struct transfer_context *find_transfer(const char *transfer_id)
{
    struct transfer_context *ctx;

    for (ctx = g_transfers; ctx; ctx = ctx->next) {
        if (strcmp(ctx->transfer_id, transfer_id) == 0)
            return ctx;
    }
    return NULL;
}

static void free_transfer(struct transfer_context *ctx)
{
    if (!ctx)
        return;
    free(ctx->invocation_id);
    free(ctx->transfer_id);
    free(ctx->local_path);
    free(ctx->remote_path);
    free(ctx->source_name);
    if (ctx->send_fp)
        fclose(ctx->send_fp);
    free_entries(ctx->entries, ctx->entry_count);
    json_decref(ctx->accept);
    free(ctx);
}

static void release_transfer_after_unlink(struct transfer_context *ctx)
{
    if (!ctx)
        return;
    if (ctx->pumping) {
        ctx->cancelled = true;
        ctx->free_after_pump = true;
        return;
    }
    free_transfer(ctx);
}

static void unlink_transfer(struct transfer_context *ctx)
{
    struct transfer_context **current = &g_transfers;

    while (*current) {
        if (*current == ctx) {
            *current = ctx->next;
            ctx->next = NULL;
            return;
        }
        current = &(*current)->next;
    }
}

static int add_transfer(struct transfer_context *ctx)
{
    ctx->next = g_transfers;
    g_transfers = ctx;
    mft_signal_timer();
    return 0;
}

static int send_abort_frame(unsigned int server_id,
                            const char *transfer_id,
                            const char *message)
{
    json_t *payload;
    int rc;

    if (!transfer_id)
        return -1;
    payload = json_pack("{s:s,s:s}",
                        "transfer_id",
                        transfer_id,
                        "message",
                        message ? message : "File transfer aborted.");
    if (!payload)
        return -1;
    rc = send_json_frame(server_id, MFT_FRAME_ABORT, payload);
    json_decref(payload);
    return rc;
}

static void complete_transfer_error_unlinked(struct transfer_context *ctx,
                                             const char *message,
                                             bool notify_peer)
{
    char error[256];
    bool defer_free;

    if (!ctx)
        return;
    snprintf(error, sizeof(error), "%s", message ? message : "File transfer failed.");
    mft_lock();
    ctx->cancelled = true;
    if (ctx->completed) {
        mft_unlock();
        return;
    }
    ctx->completed = true;
    defer_free = ctx->pumping;
    if (defer_free)
        ctx->free_after_pump = true;
    mft_unlock();
    if (notify_peer)
        send_abort_frame(ctx->server_id, ctx->transfer_id, error);
    if (ctx->invocation_id && g_plugin.host->complete_async_error)
        g_plugin.host->complete_async_error(g_plugin.host->host_context,
                                            ctx->invocation_id,
                                            error);
    if (!defer_free)
        free_transfer(ctx);
}

static void complete_transfer_error_by_id(const char *transfer_id,
                                          const char *message,
                                          bool notify_peer)
{
    struct transfer_context *ctx;

    if (!transfer_id)
        return;
    mft_lock();
    ctx = find_transfer(transfer_id);
    if (ctx)
        unlink_transfer(ctx);
    mft_unlock();
    if (ctx)
        complete_transfer_error_unlinked(ctx, message, notify_peer);
}

static void abort_transfers_for_peer(unsigned int server_id)
{
    struct transfer_context *ctx = g_transfers;

    mft_lock();
    while (ctx) {
        struct transfer_context *next = ctx->next;

        if (ctx->server_id == server_id) {
            unlink_transfer(ctx);
            ctx->cancelled = true;
            mft_unlock();
            complete_transfer_error_unlinked(ctx, "Peer closed during file transfer.", false);
            mft_lock();
        }
        ctx = next;
    }
    mft_unlock();
}

static void recompute_next_package_deadline(unsigned long long *next_deadline)
{
    struct transfer_context *ctx;

    *next_deadline = 0;
    for (ctx = g_transfers; ctx; ctx = ctx->next) {
        size_t i;

        for (i = 0; i < ctx->entry_count; i++) {
            struct manifest_entry *entry = &ctx->entries[i];
            size_t block_index;

            if (ctx->send_paused) {
                unsigned long long candidate = ctx->send_pump_deadline_ms;
                if (!*next_deadline || candidate < *next_deadline)
                    *next_deadline = candidate;
            }
            if (entry->type != 'f')
                continue;
            for (block_index = 0; block_index < entry->block_count; block_index++) {
                struct block_entry *block = &entry->blocks[block_index];
                unsigned long long candidate = 0;

                if (block->ack_waiting && !block->acked)
                    candidate = block->ack_deadline_ms;
                if (block->receive_waiting && !block->received_ok &&
                    (!candidate || block->receive_deadline_ms < candidate))
                    candidate = block->receive_deadline_ms;
                if (candidate && (!*next_deadline || candidate < *next_deadline))
                    *next_deadline = candidate;
            }
        }
    }
}

static void queue_pending_ack(struct pending_block_ack **head,
                              unsigned int server_id,
                              const char *transfer_id,
                              uint32_t stream_id,
                              size_t block_index,
                              uint64_t offset,
                              uint64_t size,
                              bool ok)
{
    struct pending_block_ack *ack = calloc(1, sizeof(*ack));

    if (!ack)
        return;
    ack->transfer_id = mft_strdup(transfer_id);
    if (!ack->transfer_id) {
        free(ack);
        return;
    }
    ack->server_id = server_id;
    ack->stream_id = stream_id;
    ack->block_index = block_index;
    ack->offset = offset;
    ack->size = size;
    ack->ok = ok;
    ack->next = *head;
    *head = ack;
}

static void send_pending_acks(struct pending_block_ack *acks)
{
    struct pending_block_ack *ack;

    for (ack = acks; ack; ack = ack->next) {
        send_block_ack_raw(ack->server_id,
                           ack->transfer_id,
                           ack->stream_id,
                           ack->block_index,
                           ack->offset,
                           ack->size,
                           ack->ok);
    }
}

static void queue_pending_error(struct pending_transfer_error **head,
                                struct transfer_context *ctx,
                                const char *message,
                                bool notify_peer)
{
    struct pending_transfer_error *error = calloc(1, sizeof(*error));

    if (!error)
        return;
    error->ctx = ctx;
    error->message = message;
    error->notify_peer = notify_peer;
    error->next = *head;
    *head = error;
}

static void complete_pending_errors(struct pending_transfer_error *errors)
{
    while (errors) {
        struct pending_transfer_error *next = errors->next;

        complete_transfer_error_unlinked(errors->ctx,
                                         errors->message,
                                         errors->notify_peer);
        free(errors);
        errors = next;
    }
}

static void run_pending_pumps(struct pending_pump *pumps)
{
    while (pumps) {
        struct pending_pump *next = pumps->next;
        struct transfer_context *ctx;

        mft_lock();
        ctx = find_transfer(pumps->transfer_id);
        if (ctx && send_pump(ctx) != 0) {
            unlink_transfer(ctx);
            mft_unlock();
            complete_transfer_error_unlinked(ctx, "Failed to continue file transfer data.", true);
        } else {
            mft_unlock();
        }
        free(pumps->transfer_id);
        free(pumps);
        pumps = next;
    }
}

static void scan_package_timeouts(unsigned long long now,
                                  struct pending_block_ack **pending_acks,
                                  struct pending_transfer_error **pending_errors,
                                  struct pending_pump **pending_pumps)
{
    struct transfer_context *ctx = g_transfers;

    while (ctx) {
        struct transfer_context *next = ctx->next;
        bool failed = false;
        size_t i;

        if (ctx->send_paused &&
            (!ctx->send_pump_deadline_ms || now >= ctx->send_pump_deadline_ms)) {
            struct pending_pump *pump = calloc(1, sizeof(*pump));

            if (pump) {
                pump->transfer_id = mft_strdup(ctx->transfer_id);
                if (pump->transfer_id) {
                    pump->next = *pending_pumps;
                    *pending_pumps = pump;
                } else {
                    free(pump);
                }
            }
            ctx->send_paused = false;
            ctx->send_pump_deadline_ms = 0;
        }
        for (i = 0; !failed && i < ctx->entry_count; i++) {
            struct manifest_entry *entry = &ctx->entries[i];
            size_t block_index;

            if (entry->type != 'f')
                continue;
            for (block_index = 0; block_index < entry->block_count; block_index++) {
                struct block_entry *block = &entry->blocks[block_index];

                if (block->ack_waiting && !block->acked &&
                    block->ack_deadline_ms && now >= block->ack_deadline_ms) {
                    failed = true;
                    break;
                }
                if (block->receive_waiting && !block->received_ok &&
                    block->receive_deadline_ms && now >= block->receive_deadline_ms) {
                    if (block->retries >= MFT_MAX_BLOCK_RETRIES) {
                        failed = true;
                        break;
                    }
                    block->retries++;
                    block->receive_deadline_ms = now + MFT_BLOCK_TIMEOUT_MS;
                    queue_pending_ack(pending_acks,
                                      ctx->server_id,
                                      ctx->transfer_id,
                                      (uint32_t)(i + 1),
                                      block_index,
                                      block->offset,
                                      block->size,
                                      false);
                }
            }
        }
        if (failed) {
            unlink_transfer(ctx);
            queue_pending_error(pending_errors,
                                ctx,
                                "File transfer package task timed out.",
                                true);
        }
        ctx = next;
    }
}

#ifdef _WIN32
static DWORD WINAPI mft_timer_thread_main(LPVOID arg)
#else
static void *mft_timer_thread_main(void *arg)
#endif
{
    (void)arg;
    for (;;) {
        unsigned long long now;
        unsigned long long next_deadline;

        mft_lock();
        if (g_plugin.timer_stop) {
            mft_unlock();
            break;
        }
        recompute_next_package_deadline(&next_deadline);
        if (!next_deadline) {
#ifdef _WIN32
            SleepConditionVariableCS(&g_plugin.timer_cv, &g_plugin.lock, INFINITE);
#else
            pthread_cond_wait(&g_plugin.timer_cond, &g_plugin.lock);
#endif
            mft_unlock();
            continue;
        }
        now = g_plugin.host->now_ms(g_plugin.host->host_context);
        if (now < next_deadline) {
            unsigned long long wait_ms = next_deadline - now;
#ifdef _WIN32
            SleepConditionVariableCS(&g_plugin.timer_cv,
                                     &g_plugin.lock,
                                     wait_ms > INFINITE - 1 ? INFINITE - 1 : (DWORD)wait_ms);
#else
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += (time_t)(wait_ms / 1000ull);
            ts.tv_nsec += (long)((wait_ms % 1000ull) * 1000000ull);
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&g_plugin.timer_cond, &g_plugin.lock, &ts);
#endif
            mft_unlock();
            continue;
        }
        {
            struct pending_block_ack *pending_acks = NULL;
            struct pending_transfer_error *pending_errors = NULL;
            struct pending_pump *pending_pumps = NULL;

            scan_package_timeouts(now, &pending_acks, &pending_errors, &pending_pumps);
            mft_unlock();
            run_pending_pumps(pending_pumps);
            send_pending_acks(pending_acks);
            free_pending_acks(pending_acks);
            complete_pending_errors(pending_errors);
            mft_signal_timer();
            continue;
        }
        mft_unlock();
    }
#ifndef _WIN32
    return NULL;
#else
    return 0;
#endif
}

static int start_timer_thread(void)
{
    if (g_plugin.timer_started)
        return 0;
#ifdef _WIN32
    g_plugin.timer_thread = CreateThread(NULL, 0, mft_timer_thread_main, NULL, 0, NULL);
    if (!g_plugin.timer_thread)
        return -1;
#else
    if (pthread_create(&g_plugin.timer_thread, NULL, mft_timer_thread_main, NULL) != 0)
        return -1;
#endif
    g_plugin.timer_started = true;
    return 0;
}

static void stop_timer_thread(void)
{
    if (!g_plugin.timer_started)
        return;
    mft_lock();
    g_plugin.timer_stop = true;
    mft_signal_timer();
    mft_unlock();
#ifdef _WIN32
    WaitForSingleObject(g_plugin.timer_thread, INFINITE);
    CloseHandle(g_plugin.timer_thread);
    g_plugin.timer_thread = NULL;
#else
    pthread_join(g_plugin.timer_thread, NULL);
#endif
    g_plugin.timer_started = false;
}

static uint32_t clamp_timeout_ms(unsigned long long value)
{
    if (value < MFT_MIN_TIMEOUT_MS)
        return MFT_MIN_TIMEOUT_MS;
    if (value > MFT_MAX_TIMEOUT_MS)
        return MFT_MAX_TIMEOUT_MS;
    return (uint32_t)value;
}

static int start_transfer_timer(struct transfer_context *ctx)
{
    unsigned long long now;

    if (!ctx || !g_plugin.host || !g_plugin.host->now_ms || ctx->timeout_ms == 0)
        return -1;
    now = g_plugin.host->now_ms(g_plugin.host->host_context);
    ctx->deadline_ms = now > (unsigned long long)-1 - ctx->timeout_ms ?
                           (unsigned long long)-1 :
                           now + ctx->timeout_ms;
    return 0;
}

static int touch_transfer_activity(struct transfer_context *ctx)
{
    return start_transfer_timer(ctx);
}

static void expire_due_transfers(void)
{
    struct transfer_context *ctx;
    unsigned long long now;

    if (!g_plugin.host || !g_plugin.host->now_ms)
        return;
    now = g_plugin.host->now_ms(g_plugin.host->host_context);
    mft_lock();
    ctx = g_transfers;
    while (ctx) {
        struct transfer_context *next = ctx->next;

        if (ctx->deadline_ms && now >= ctx->deadline_ms) {
            unlink_transfer(ctx);
            mft_unlock();
            complete_transfer_error_unlinked(ctx, "File transfer timed out.", true);
            mft_lock();
        }
        ctx = next;
    }
    mft_unlock();
}

static uint32_t payload_timeout_ms(json_t *payload)
{
    json_t *value = json_object_get(payload, "timeout_ms");

    if (!json_is_integer(value) || json_integer_value(value) <= 0)
        return MFT_DEFAULT_TIMEOUT_MS;
    return clamp_timeout_ms((unsigned long long)json_integer_value(value));
}

static int start_send(const char *invocation_id,
                      unsigned int server_id,
                      const char *local_path,
                      const char *remote_path,
                      uint32_t timeout_ms)
{
    struct transfer_context *ctx;
    json_t *manifest = NULL;
    json_t *payload = NULL;
    struct stat st;
    uint64_t bytes_total = 0;
    size_t i;
    size_t capacity = 0;

    if (ensure_capability(server_id) != 0)
        return -2;

    if (g_plugin.host && g_plugin.host->log_info) {
        char msg[256];

        snprintf(msg,
                 sizeof(msg),
                 "mft start send path=%s remote=%s",
                 local_path ? local_path : "",
                 remote_path ? remote_path : "");
        g_plugin.host->log_info(g_plugin.host->host_context, msg);
    }

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return -1;
    ctx->invocation_id = mft_strdup(invocation_id);
    ctx->transfer_id = make_transfer_id();
    ctx->local_path = mft_strdup(local_path);
    ctx->remote_path = mft_strdup(remote_path);
    ctx->server_id = server_id;
    ctx->timeout_ms = timeout_ms;
    ctx->started_ms = g_plugin.host->now_ms(g_plugin.host->host_context);
    strcpy(ctx->direction, "send");
    if (!ctx->invocation_id || !ctx->transfer_id || !ctx->local_path || !ctx->remote_path)
        goto fail;

    if (stat(local_path, &st) != 0)
        goto fail;
    if (g_plugin.host && g_plugin.host->log_info) {
        char msg[256];

        snprintf(msg,
                 sizeof(msg),
                 "mft start send stat ok size=%llu dir=%d",
                 (unsigned long long)st.st_size,
                 S_ISDIR(st.st_mode) ? 1 : 0);
        g_plugin.host->log_info(g_plugin.host->host_context, msg);
    }
    if (S_ISDIR(st.st_mode)) {
        ctx->source_name = path_basename_dup(local_path);
        if (!ctx->source_name)
            goto fail;
        free(ctx->local_path);
        ctx->local_path = path_dirname_dup(local_path);
        if (!ctx->local_path)
            goto fail;
        if (scan_path(ctx->local_path, ctx->source_name, &ctx->entries, &ctx->entry_count, &capacity) != 0)
            goto fail;
    } else {
        if (scan_path(local_path, ".", &ctx->entries, &ctx->entry_count, &capacity) != 0)
            goto fail;
        if (manifest_is_single_file_root(ctx->entries, ctx->entry_count)) {
            ctx->source_name = path_basename_dup(local_path);
            if (!ctx->source_name)
                goto fail;
        }
    }
    if (manifest_is_single_file_root(ctx->entries, ctx->entry_count) && !ctx->source_name)
        goto fail;
    if (g_plugin.host && g_plugin.host->log_info) {
        char msg[256];

        snprintf(msg,
                 sizeof(msg),
                 "mft start send scan done entries=%lu",
                 (unsigned long)ctx->entry_count);
        g_plugin.host->log_info(g_plugin.host->host_context, msg);
    }
    for (i = 0; i < ctx->entry_count; i++) {
        if (ctx->entries[i].type == 'f')
            bytes_total += ctx->entries[i].size;
    }
    manifest = entries_to_manifest(ctx->entries, ctx->entry_count);
    if (!manifest)
        goto fail;
    if (g_plugin.host && g_plugin.host->log_info) {
        char msg[256];

        snprintf(msg,
                 sizeof(msg),
                 "mft start send manifest ready entries=%lu bytes=%llu",
                 (unsigned long)ctx->entry_count,
                 (unsigned long long)bytes_total);
        g_plugin.host->log_info(g_plugin.host->host_context, msg);
    }
    payload = json_pack("{s:s,s:s,s:o,s:I,s:i}",
                        "transfer_id",
                        ctx->transfer_id,
                        "remote_path",
                        remote_path,
                        "manifest",
                        manifest,
                        "bytes_total",
                        (json_int_t)bytes_total,
                        "max_concurrent_streams",
                        (int)MFT_MAX_CONCURRENT_STREAMS);
    manifest = NULL;
    if (!payload)
        goto fail;
    if (json_object_set_new(payload, "timeout_ms", json_integer((json_int_t)timeout_ms)) != 0)
        goto fail;
    if (ctx->source_name &&
        json_object_set_new(payload, "source_name", json_string(ctx->source_name)) != 0)
        goto fail;
    if (start_transfer_timer(ctx) != 0)
        goto fail;
    mft_lock();
    add_transfer(ctx);
    mft_unlock();
    if (g_plugin.host && g_plugin.host->log_info) {
        char msg[256];

        snprintf(msg,
                 sizeof(msg),
                 "mft start send offer tx=%s bytes=%llu",
                 ctx->transfer_id ? ctx->transfer_id : "",
                 (unsigned long long)bytes_total);
        g_plugin.host->log_info(g_plugin.host->host_context, msg);
    }
    if (send_json_frame(server_id, MFT_FRAME_OFFER, payload) != 0) {
        mft_lock();
        unlink_transfer(ctx);
        mft_unlock();
        goto fail;
    }
    json_decref(payload);
    return 0;

fail:
    json_decref(payload);
    json_decref(manifest);
    free_transfer(ctx);
    return -1;
}

static int start_recv(const char *invocation_id,
                      unsigned int server_id,
                      const char *remote_path,
                      const char *local_path,
                      uint32_t timeout_ms)
{
    struct transfer_context *ctx;
    json_t *payload = NULL;

    if (ensure_capability(server_id) != 0)
        return -2;

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return -1;
    ctx->invocation_id = mft_strdup(invocation_id);
    ctx->transfer_id = make_transfer_id();
    ctx->local_path = mft_strdup(local_path);
    ctx->remote_path = mft_strdup(remote_path);
    ctx->source_name = path_remote_basename_dup(remote_path);
    ctx->server_id = server_id;
    ctx->timeout_ms = timeout_ms;
    ctx->started_ms = g_plugin.host->now_ms(g_plugin.host->host_context);
    strcpy(ctx->direction, "receive");
    if (!ctx->invocation_id || !ctx->transfer_id || !ctx->local_path || !ctx->remote_path ||
        !ctx->source_name)
        goto fail;

    payload = json_pack("{s:s,s:s,s:s,s:i}",
                        "transfer_id",
                        ctx->transfer_id,
                        "remote_path",
                        remote_path,
                        "local_path",
                        local_path,
                        "timeout_ms",
                        (int)timeout_ms);
    if (!payload || start_transfer_timer(ctx) != 0)
        goto fail;
    mft_lock();
    add_transfer(ctx);
    mft_unlock();
    if (send_json_frame(server_id, MFT_FRAME_FETCH_REQUEST, payload) != 0) {
        mft_lock();
        unlink_transfer(ctx);
        mft_unlock();
        goto fail;
    }
    json_decref(payload);
    return 0;

fail:
    json_decref(payload);
    free_transfer(ctx);
    return -1;
}

static void handle_offer(unsigned int server_id, json_t *payload)
{
    json_t *transfer_id = json_object_get(payload, "transfer_id");
    json_t *remote_path = json_object_get(payload, "remote_path");
    json_t *source_name = json_object_get(payload, "source_name");
    json_t *manifest = json_object_get(payload, "manifest");
    struct manifest_entry *entries = NULL;
    size_t count = 0;
    json_t *accept = NULL;
    json_t *response;
    struct transfer_context *ctx;
    char target_root[PATH_MAX];
    const char *source_name_value = NULL;
    int created_ctx = 0;
    char *transfer_id_value = NULL;
    int response_rc;

    if (!json_is_string(transfer_id) || !json_is_string(remote_path) ||
        manifest_to_entries(manifest, &entries, &count) != 0)
        return;
    transfer_id_value = mft_strdup(json_string_value(transfer_id));
    if (!transfer_id_value) {
        free_entries(entries, count);
        return;
    }

    mft_lock();
    ctx = find_transfer(transfer_id_value);
    if (json_is_string(source_name))
        source_name_value = json_string_value(source_name);
    else if (ctx && ctx->source_name)
        source_name_value = ctx->source_name;
    if (source_name_value &&
        (strcmp(source_name_value, ".") == 0 || !path_is_safe_rel(source_name_value))) {
        free_entries(entries, count);
        if (ctx) {
            unlink_transfer(ctx);
            mft_unlock();
            complete_transfer_error_unlinked(ctx,
                                             "Invalid source_name in file transfer offer.",
                                             true);
        } else {
            mft_unlock();
            send_abort_frame(server_id,
                             transfer_id_value,
                             "Invalid source_name in file transfer offer.");
        }
        free(transfer_id_value);
        return;
    }

    if (resolve_receive_root(target_root,
                             sizeof(target_root),
                             json_string_value(remote_path),
                             source_name_value,
                             entries,
                             count) != 0 ||
        prepare_accept(target_root, entries, count, &accept) != 0) {
        free_entries(entries, count);
        if (ctx) {
            unlink_transfer(ctx);
            mft_unlock();
            complete_transfer_error_unlinked(ctx,
                                             "Failed to prepare receive target path.",
                                             true);
        } else {
            mft_unlock();
            send_abort_frame(server_id,
                             transfer_id_value,
                             "Failed to prepare receive target path.");
        }
        free(transfer_id_value);
        return;
    }

    if (!ctx) {
        ctx = calloc(1, sizeof(*ctx));
        if (!ctx) {
            free_entries(entries, count);
            json_decref(accept);
            mft_unlock();
            return;
        }
        created_ctx = 1;
        ctx->transfer_id = mft_strdup(transfer_id_value);
        ctx->local_path = mft_strdup(target_root);
        if (source_name_value)
            ctx->source_name = mft_strdup(source_name_value);
        if (!ctx->transfer_id || !ctx->local_path) {
            free_transfer(ctx);
            json_decref(accept);
            free_entries(entries, count);
            mft_unlock();
            send_abort_frame(server_id,
                             transfer_id_value,
                             "Failed to allocate receive context.");
            free(transfer_id_value);
            return;
        }
    } else {
        free_entries(ctx->entries, ctx->entry_count);
        json_decref(ctx->accept);
        ctx->entries = NULL;
        ctx->entry_count = 0;
        ctx->accept = NULL;
        free(ctx->local_path);
        ctx->local_path = mft_strdup(target_root);
        if (!ctx->local_path) {
            free_entries(entries, count);
            json_decref(accept);
            unlink_transfer(ctx);
            mft_unlock();
            complete_transfer_error_unlinked(ctx,
                                             "Failed to allocate receive target path.",
                                             true);
            free(transfer_id_value);
            return;
        }
    }

    ctx->server_id = server_id;
    ctx->timeout_ms = payload_timeout_ms(payload);
    ctx->entries = entries;
    ctx->entry_count = count;
    ctx->accept = json_incref(accept);
    ctx->started_ms = g_plugin.host->now_ms(g_plugin.host->host_context);
    strcpy(ctx->direction, "receive");
    if (created_ctx)
        add_transfer(ctx);

    response = json_pack("{s:s,s:O}", "transfer_id", ctx->transfer_id, "accept", accept);
    if (!response) {
        json_decref(response);
        unlink_transfer(ctx);
        mft_unlock();
        complete_transfer_error_unlinked(ctx, "Failed to send file transfer accept.", true);
        free(transfer_id_value);
        return;
    }
    mft_unlock();
    response_rc = send_json_frame(server_id, MFT_FRAME_ACCEPT, response);
    json_decref(response);
    if (response_rc != 0) {
        complete_transfer_error_by_id(transfer_id_value,
                                      "Failed to send file transfer accept.",
                                      true);
        free(transfer_id_value);
        return;
    }

    mft_lock();
    ctx = find_transfer(transfer_id_value);
    if (ctx) {
        mark_receive_resume_blocks(ctx);
        advance_receive_wait(ctx);
        touch_transfer_activity(ctx);
        mft_signal_timer();
    }
    mft_unlock();
    json_decref(accept);
    free(transfer_id_value);
}

static void handle_fetch_request(unsigned int server_id, json_t *payload)
{
    json_t *transfer_id = json_object_get(payload, "transfer_id");
    json_t *remote_path = json_object_get(payload, "remote_path");
    json_t *local_path = json_object_get(payload, "local_path");
    const char *transfer_id_value;
    struct transfer_context *ctx;
    json_t *manifest = NULL;
    json_t *offer = NULL;
    struct stat st;
    size_t capacity = 0;

    if (!json_is_string(transfer_id) || !json_is_string(remote_path) || !json_is_string(local_path))
        return;
    transfer_id_value = json_string_value(transfer_id);

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return;
    ctx->transfer_id = mft_strdup(transfer_id_value);
    ctx->local_path = mft_strdup(json_string_value(remote_path));
    ctx->remote_path = mft_strdup(json_string_value(local_path));
    ctx->server_id = server_id;
    ctx->timeout_ms = payload_timeout_ms(payload);
    ctx->started_ms = g_plugin.host->now_ms(g_plugin.host->host_context);
    strcpy(ctx->direction, "send");
    if (!ctx->transfer_id || !ctx->local_path || !ctx->remote_path)
        goto fail;
    if (stat(ctx->local_path, &st) != 0)
        goto fail;
    if (S_ISDIR(st.st_mode)) {
        ctx->source_name = path_basename_dup(ctx->local_path);
        if (!ctx->source_name)
            goto fail;
        free(ctx->local_path);
        ctx->local_path = path_dirname_dup(json_string_value(remote_path));
        if (!ctx->local_path)
            goto fail;
        if (scan_path(ctx->local_path, ctx->source_name, &ctx->entries, &ctx->entry_count, &capacity) != 0)
            goto fail;
    } else {
        if (scan_path(json_string_value(remote_path), ".", &ctx->entries, &ctx->entry_count, &capacity) != 0)
            goto fail;
        if (manifest_is_single_file_root(ctx->entries, ctx->entry_count)) {
            ctx->source_name = path_basename_dup(json_string_value(remote_path));
            if (!ctx->source_name)
                goto fail;
        }
    }

    manifest = entries_to_manifest(ctx->entries, ctx->entry_count);
    if (!manifest)
        goto fail;
    offer = json_pack("{s:s,s:s,s:o}",
                      "transfer_id",
                      ctx->transfer_id,
                      "remote_path",
                      ctx->remote_path,
                      "manifest",
                      manifest);
    manifest = NULL;
    if (!offer)
        goto fail;
    if (json_object_set_new(offer, "timeout_ms", json_integer((json_int_t)ctx->timeout_ms)) != 0)
        goto fail;
    if (ctx->source_name &&
        json_object_set_new(offer, "source_name", json_string(ctx->source_name)) != 0)
        goto fail;
    mft_lock();
    add_transfer(ctx);
    mft_unlock();
    if (send_json_frame(server_id, MFT_FRAME_OFFER, offer) != 0) {
        mft_lock();
        unlink_transfer(ctx);
        mft_unlock();
        goto fail;
    }
    json_decref(offer);
    return;

fail:
    json_decref(offer);
    json_decref(manifest);
    send_abort_frame(server_id, transfer_id_value, "Remote path is not a readable regular file or directory.");
    free_transfer(ctx);
}

static void handle_accept(unsigned int server_id, json_t *payload)
{
    json_t *transfer_id = json_object_get(payload, "transfer_id");
    json_t *accept = json_object_get(payload, "accept");
    struct transfer_context *ctx;
    int prepare_failed = 0;
    int pump_failed = 0;
    char *transfer_id_value = NULL;

    if (!json_is_string(transfer_id) || !json_is_array(accept))
        return;
    if (g_plugin.host && g_plugin.host->log_info) {
        char msg[256];

        snprintf(msg,
                 sizeof(msg),
                 "mft handle accept server=%u",
                 server_id);
        g_plugin.host->log_info(g_plugin.host->host_context, msg);
    }
    transfer_id_value = mft_strdup(json_string_value(transfer_id));
    if (!transfer_id_value)
        return;
    mft_lock();
    ctx = find_transfer(transfer_id_value);
    if (!ctx) {
        mft_unlock();
        free(transfer_id_value);
        return;
    }

    (void)server_id;
    if (prepare_send_entries(ctx, accept) != 0)
        prepare_failed = 1;
    else {
        touch_transfer_activity(ctx);
        if (send_pump(ctx) != 0)
            pump_failed = 1;
    }
    mft_unlock();
    if (prepare_failed) {
        complete_transfer_error_by_id(transfer_id_value,
                                      "Failed to send file transfer data.",
                                      true);
        free(transfer_id_value);
        return;
    }
    if (pump_failed) {
        complete_transfer_error_by_id(transfer_id_value,
                                      "Failed to start file transfer data.",
                                      true);
        free(transfer_id_value);
        return;
    }
    free(transfer_id_value);
    mft_signal_timer();
}

static void handle_data(unsigned int server_id,
                        const unsigned char *payload,
                        size_t len)
{
    uint16_t tid_len;
    uint32_t stream_id;
    uint64_t offset;
    uint32_t data_len;
    char chunk_hash[65];
    char actual_hash[65];
    char transfer_id[256];
    struct transfer_context *ctx = NULL;
    char target_root[PATH_MAX];
    char relpath[PATH_MAX];
    char part_path_value[PATH_MAX];
    size_t block_index;
    uint64_t block_offset;
    uint64_t block_size;
    char block_hash[65];
    const unsigned char *chunk_data;
    bool chunk_ok;
    bool is_block_tail = false;
    bool send_ack = false;
    bool ack_ok = false;
    bool block_hash_ok = false;
    bool signal_timer = false;
    const char *error_message = NULL;

    if (len < 88)
        return;
    tid_len = read_u16_be(payload + 6);
    stream_id = read_u32_be(payload + 8);
    offset = read_u64_be(payload + 12);
    data_len = read_u32_be(payload + 20);
    if (tid_len == 0 || tid_len >= sizeof(transfer_id) || len < 88u + tid_len + data_len)
        return;
    memcpy(chunk_hash, payload + 24, 64);
    chunk_hash[64] = '\0';
    memcpy(transfer_id, payload + 88, tid_len);
    transfer_id[tid_len] = '\0';
    chunk_data = payload + 88 + tid_len;
    bytes_sha256(chunk_data, data_len, actual_hash);
    chunk_ok = strcmp(actual_hash, chunk_hash) == 0;

    mft_lock();
    ctx = find_transfer(transfer_id);
    if (!ctx || stream_id == 0 || stream_id > ctx->entry_count) {
        mft_unlock();
        return;
    }
    {
        struct manifest_entry *entry = &ctx->entries[stream_id - 1];
        struct block_entry *block;

        if (entry->type != 'f' || offset >= entry->size) {
            mft_unlock();
            return;
        }
        block_index = (size_t)(offset / MFT_LOGICAL_BLOCK_SIZE);
        if (block_index >= entry->block_count) {
            mft_unlock();
            return;
        }
        block = &entry->blocks[block_index];
        if (offset < block->offset || offset + data_len > block->offset + block->size) {
            mft_unlock();
            return;
        }
        if (strlen(ctx->local_path) >= sizeof(target_root) ||
            strlen(entry->relpath) >= sizeof(relpath)) {
            mft_unlock();
            return;
        }
        snprintf(target_root, sizeof(target_root), "%s", ctx->local_path);
        snprintf(relpath, sizeof(relpath), "%s", entry->relpath);
        block_offset = block->offset;
        block_size = block->size;
        snprintf(block_hash, sizeof(block_hash), "%s", block->hash);
        if (offset == block->offset) {
            sha256_init(&block->receive_hash_ctx);
            block->receive_hash_started = true;
            block->receive_hash_failed = false;
            block->receive_next_offset = block->offset;
        }
        if (!block->receive_hash_started ||
            block->receive_hash_failed ||
            offset != block->receive_next_offset) {
            int nack_rc = mark_block_nack(ctx, stream_id, block_index);

            block->receive_hash_started = false;
            block->receive_hash_failed = true;
            block->receive_next_offset = block->offset;
            if (nack_rc == -2)
                error_message = "File transfer receive retry limit exceeded.";
            else if (nack_rc == 0)
                send_ack = true;
            mft_unlock();
            if (send_ack)
                send_block_ack_raw(server_id,
                                   transfer_id,
                                   stream_id,
                                   block_index,
                                   block_offset,
                                   block_size,
                                   false);
            mft_signal_timer();
            if (error_message)
                complete_transfer_error_by_id(transfer_id, error_message, true);
            return;
        }
    }

    if (!chunk_ok) {
        int nack_rc = mark_block_nack(ctx, stream_id, block_index);

        if (nack_rc == -2)
            error_message = "File transfer receive retry limit exceeded.";
        else if (nack_rc == 0)
            send_ack = true;
        mft_unlock();
        if (send_ack)
            send_block_ack_raw(server_id,
                               transfer_id,
                               stream_id,
                               block_index,
                               block_offset,
                               block_size,
                               false);
        mft_signal_timer();
        if (error_message)
            complete_transfer_error_by_id(transfer_id, error_message, true);
        return;
    }
    if (part_path_for_relpath(target_root, relpath, part_path_value, sizeof(part_path_value)) != 0) {
        mft_unlock();
        complete_transfer_error_by_id(transfer_id, "Failed to write received file data.", true);
        return;
    }
    is_block_tail = offset + data_len == block_offset + block_size;
    mft_unlock();

    if (write_all_at(part_path_value, chunk_data, data_len, offset) != 0) {
        complete_transfer_error_by_id(transfer_id, "Failed to write received file data.", true);
        return;
    }

    mft_lock();
    ctx = find_transfer(transfer_id);
    if (!ctx || stream_id == 0 || stream_id > ctx->entry_count) {
        mft_unlock();
        return;
    }
    {
        struct manifest_entry *entry = &ctx->entries[stream_id - 1];
        struct block_entry *block;

        if (entry->type != 'f' || block_index >= entry->block_count) {
            mft_unlock();
            return;
        }
        block = &entry->blocks[block_index];
        if (block->offset != block_offset || block->size != block_size) {
            mft_unlock();
            return;
        }
        touch_transfer_activity(ctx);
        if (!block->receive_hash_started ||
            block->receive_hash_failed ||
            offset != block->receive_next_offset) {
            int nack_rc = mark_block_nack(ctx, stream_id, block_index);

            block->receive_hash_started = false;
            block->receive_hash_failed = true;
            block->receive_next_offset = block->offset;
            if (nack_rc == -2)
                error_message = "File transfer receive retry limit exceeded.";
            else if (nack_rc == 0)
                send_ack = true;
            signal_timer = true;
        } else {
            sha256_update(&block->receive_hash_ctx, chunk_data, data_len);
            block->receive_next_offset = offset + data_len;
            block->receive_waiting = true;
            block->receive_deadline_ms =
                g_plugin.host->now_ms(g_plugin.host->host_context) + MFT_BLOCK_TIMEOUT_MS;
        }
        if (is_block_tail) {
            block->receive_deadline_ms = 0;
            if (!error_message && !send_ack && !block->receive_hash_failed) {
                unsigned char hash_bytes[32];

                sha256_final(&block->receive_hash_ctx, hash_bytes);
                hash_to_hex(hash_bytes, actual_hash);
                block_hash_ok = strcmp(actual_hash, block_hash) == 0;
                block->receive_hash_started = false;
            }
            if (!error_message && block_hash_ok) {
                if (!block->received_ok) {
                    block->received_ok = true;
                    block->receive_waiting = false;
                    block->receive_deadline_ms = 0;
                    block->receive_next_offset = block->offset;
                    ctx->bytes_transferred += block->size;
                }
                send_ack = true;
                ack_ok = true;
                advance_receive_wait(ctx);
            } else if (!error_message) {
                int nack_rc = mark_block_nack(ctx, stream_id, block_index);

                block->receive_hash_started = false;
                block->receive_hash_failed = true;
                block->receive_next_offset = block->offset;
                if (nack_rc == -2)
                    error_message = "File transfer receive retry limit exceeded.";
                else if (nack_rc == 0)
                    send_ack = true;
            }
            signal_timer = true;
        }
    }
    mft_unlock();
    if (send_ack)
        send_block_ack_raw(server_id,
                           transfer_id,
                           stream_id,
                           block_index,
                           block_offset,
                           block_size,
                           ack_ok);
    if (signal_timer)
        mft_signal_timer();
    if (error_message)
        complete_transfer_error_by_id(transfer_id, error_message, true);
}

static void handle_complete(unsigned int server_id, json_t *payload)
{
    json_t *transfer_id = json_object_get(payload, "transfer_id");
    struct transfer_context *ctx;
    unsigned int files_skipped = 0;
    unsigned int files_transferred = 0;
    uint64_t bytes_total = 0;
    uint64_t bytes_transferred = 0;
    size_t i;
    char *summary_invocation_id = NULL;
    char *summary_transfer_id = NULL;
    char summary_direction[8];
    size_t summary_files_total = 0;
    unsigned int summary_files_transferred = 0;
    unsigned int summary_files_skipped = 0;
    uint64_t summary_bytes_total = 0;
    uint64_t summary_bytes_transferred = 0;
    unsigned long long summary_started_ms = 0;
    bool unlinked = false;

    if (!json_is_string(transfer_id))
        return;
    mft_lock();
    ctx = find_transfer(json_string_value(transfer_id));
    if (!ctx) {
        mft_unlock();
        return;
    }
    for (i = 0; i < ctx->entry_count; i++) {
        if (ctx->entries[i].type == 'f')
            bytes_total += ctx->entries[i].size;
    }
    {
        json_t *v;

        v = json_object_get(payload, "bytes_transferred");
        if (json_is_integer(v))
            bytes_transferred = (uint64_t)json_integer_value(v);
        v = json_object_get(payload, "files_transferred");
        if (json_is_integer(v))
            files_transferred = (unsigned int)json_integer_value(v);
        v = json_object_get(payload, "files_skipped");
        if (json_is_integer(v))
            files_skipped = (unsigned int)json_integer_value(v);
    }
    if (strcmp(ctx->direction, "receive") == 0) {
        json_t *ack;

        if (!receive_blocks_all_ok(ctx)) {
            log_first_missing_receive_block(ctx);
            unlink_transfer(ctx);
            mft_unlock();
            complete_transfer_error_unlinked(ctx,
                                             "File transfer completed before all blocks were received.",
                                             true);
            return;
        }
        unlink_transfer(ctx);
        unlinked = true;
        mft_unlock();
        if (finalize_received(ctx->local_path,
                              ctx->entries,
                              ctx->entry_count,
                              ctx->accept,
                              &files_skipped,
                              &files_transferred) != 0)
        {
            complete_transfer_error_unlinked(ctx,
                                             "Failed to finalize received file transfer.",
                                             true);
            return;
        }
        ack = json_pack("{s:s,s:b,s:I,s:I,s:i,s:i}",
                        "transfer_id",
                        ctx->transfer_id,
                        "ack",
                        true,
                        "bytes_total",
                        (json_int_t)bytes_total,
                        "bytes_transferred",
                        (json_int_t)bytes_transferred,
                        "files_transferred",
                        (int)files_transferred,
                        "files_skipped",
                        (int)files_skipped);
        if (!ack) {
            json_decref(ack);
            complete_transfer_error_unlinked(ctx,
                                             "Failed to send file transfer acknowledgement.",
                                             true);
            return;
        }
        if (send_json_frame(server_id, MFT_FRAME_COMPLETE, ack) != 0) {
            json_decref(ack);
            complete_transfer_error_unlinked(ctx,
                                             "Failed to send file transfer acknowledgement.",
                                             true);
            return;
        }
        json_decref(ack);
        mft_lock();
    }
    if (bytes_transferred == 0)
        bytes_transferred = bytes_total;
    if (ctx->invocation_id) {
        summary_invocation_id = mft_strdup(ctx->invocation_id);
        summary_transfer_id = mft_strdup(ctx->transfer_id);
        snprintf(summary_direction,
                 sizeof(summary_direction),
                 "%s",
                 strcmp(ctx->direction, "receive") == 0 ? "recv" : "send");
        summary_files_total = ctx->entry_count;
        summary_files_transferred = files_transferred;
        summary_files_skipped = files_skipped;
        summary_bytes_total = bytes_total;
        summary_bytes_transferred = bytes_transferred;
        summary_started_ms = ctx->started_ms;
    }
    if (!unlinked)
        unlink_transfer(ctx);
    ctx->completed = true;
    release_transfer_after_unlink(ctx);
    mft_unlock();
    if (summary_invocation_id && summary_transfer_id)
        send_summary(summary_invocation_id,
                     summary_transfer_id,
                     server_id,
                     summary_direction,
                     summary_files_total,
                     summary_files_transferred,
                     summary_files_skipped,
                     summary_bytes_total,
                     summary_bytes_transferred,
                     summary_started_ms);
    free(summary_invocation_id);
    free(summary_transfer_id);
}

static void handle_abort(json_t *payload)
{
    json_t *transfer_id = json_object_get(payload, "transfer_id");
    json_t *message = json_object_get(payload, "message");
    struct transfer_context *ctx;

    if (!json_is_string(transfer_id))
        return;
    mft_lock();
    ctx = find_transfer(json_string_value(transfer_id));
    if (!ctx) {
        mft_unlock();
        return;
    }
    unlink_transfer(ctx);
    mft_unlock();
    complete_transfer_error_unlinked(ctx,
                                     json_is_string(message) ? json_string_value(message)
                                                             : "Peer aborted file transfer.",
                                     false);
}

static void on_frame(void *user_data,
                     unsigned int server_id,
                     const void *raw_payload,
                     uint32_t payload_len)
{
    const unsigned char *payload = raw_payload;
    json_t *json = NULL;
    json_error_t error;

    (void)user_data;
    if (payload_len < 8 || memcmp(payload, MFT_MAGIC, 4) != 0 || payload[4] != MFT_VERSION)
        return;
    expire_due_transfers();
    if (payload[5] == MFT_FRAME_DATA) {
        handle_data(server_id, payload, payload_len);
        return;
    }
    json = json_loadb((const char *)payload + 8, payload_len - 8, JSON_REJECT_DUPLICATES, &error);
    if (!json)
        return;
    switch (payload[5]) {
    case MFT_FRAME_HELLO:
        handle_hello(server_id, json);
        break;
    case MFT_FRAME_OFFER:
        handle_offer(server_id, json);
        break;
    case MFT_FRAME_FETCH_REQUEST:
        handle_fetch_request(server_id, json);
        break;
    case MFT_FRAME_ACCEPT:
        handle_accept(server_id, json);
        break;
    case MFT_FRAME_ACK:
        handle_ack(server_id, json);
        break;
    case MFT_FRAME_COMPLETE:
        handle_complete(server_id, json);
        break;
    case MFT_FRAME_ABORT:
        handle_abort(json);
        break;
    default:
        break;
    }
    json_decref(json);
}

static int parse_server_id(json_t *args, unsigned int *out)
{
    json_t *value = json_object_get(args, "server_id");
    json_int_t raw;

    if (!json_is_integer(value))
        return -1;
    raw = json_integer_value(value);
    if (raw <= 0 || raw > 4294967295LL)
        return -1;
    *out = (unsigned int)raw;
    return 0;
}

static uint32_t args_timeout_ms(json_t *args)
{
    json_t *value = json_object_get(args, "timeout_ms");

    if (!json_is_integer(value) || json_integer_value(value) <= 0)
        return MFT_DEFAULT_TIMEOUT_MS;
    return clamp_timeout_ms((unsigned long long)json_integer_value(value));
}

static int register_tools(void)
{
    static const char *schema =
        "{\"type\":\"object\",\"properties\":{\"server_id\":{\"type\":\"integer\",\"minimum\":1},"
        "\"local_path\":{\"type\":\"string\"},\"remote_path\":{\"type\":\"string\"},"
        "\"timeout_ms\":{\"type\":\"integer\",\"minimum\":1000,\"maximum\":600000,"
        "\"description\":\"Optional transfer timeout override in milliseconds.\"}},"
        "\"required\":[\"server_id\",\"local_path\",\"remote_path\"]}";
    struct mcp_plugin_tool_descriptor send_desc = {
        "server.send",
        "Send a local file or directory to a discovered MCP server through MFT1.",
        schema,
        "file_transfer_plugin",
        "1.0",
        "L2",
        "file.transfer",
        0,
        0,
        1,
        30000,
        "server.send",
    };
    struct mcp_plugin_tool_descriptor recv_desc = {
        "server.recv",
        "Receive a remote file or directory from a discovered MCP server through MFT1.",
        schema,
        "file_transfer_plugin",
        "1.0",
        "L2",
        "file.transfer",
        0,
        0,
        1,
        30000,
        "server.recv",
    };

    return g_plugin.host->register_tool(g_plugin.host->host_context, &send_desc) == 0 &&
           g_plugin.host->register_tool(g_plugin.host->host_context, &recv_desc) == 0 ? 0 : -1;
}

MFT_PLUGIN_EXPORT int MFT_PLUGIN_INIT(const struct mcp_plugin_host_api *host,
                                      const char *config_json,
                                      char *result_json,
                                      unsigned int result_size)
{
    (void)config_json;
    g_plugin.host = host;
    if (!host || !host->register_tool || !host->peer_transport_register_handler) {
        snprintf(result_json, result_size, "Missing ABI 1.1 host API.");
        return -1;
    }
#ifdef _WIN32
    InitializeCriticalSection(&g_plugin.lock);
    InitializeConditionVariable(&g_plugin.timer_cv);
    g_plugin.sync_initialized = true;
#else
    {
        pthread_mutexattr_t attr;

        if (pthread_mutexattr_init(&attr) != 0) {
            snprintf(result_json, result_size, "Failed to initialize file transfer synchronization.");
            return -1;
        }
        if (pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) != 0 ||
            pthread_mutex_init(&g_plugin.lock, &attr) != 0 ||
            pthread_cond_init(&g_plugin.timer_cond, NULL) != 0) {
            pthread_mutexattr_destroy(&attr);
            snprintf(result_json, result_size, "Failed to initialize file transfer synchronization.");
            return -1;
        }
        pthread_mutexattr_destroy(&attr);
    }
    g_plugin.sync_initialized = true;
#endif
    if (start_timer_thread() != 0) {
        snprintf(result_json, result_size, "Failed to start file transfer timer thread.");
        return -1;
    }
    if (register_tools() != 0) {
        snprintf(result_json, result_size, "Failed to register server.send/server.recv.");
        return -1;
    }
    if (host->peer_transport_register_handler(host->host_context,
                                              MFT_MAGIC,
                                              on_frame,
                                              on_peer_connected,
                                              on_peer_closed,
                                              NULL) != 0) {
        snprintf(result_json, result_size, "Failed to register MFT1 handler.");
        return -1;
    }
    snprintf(result_json, result_size, "{\"ok\":true}");
    return 0;
}

MFT_PLUGIN_EXPORT int MFT_PLUGIN_INVOKE(const char *invocation_id,
                                        const char *tool_name,
                                        const char *arguments_json,
                                        char *result_json,
                                        unsigned int result_size)
{
    json_error_t error;
    json_t *args = json_loads(arguments_json, JSON_REJECT_DUPLICATES, &error);
    json_t *local_path;
    json_t *remote_path;
    unsigned int server_id;
    uint32_t timeout_ms;
    int rc;

    expire_due_transfers();
    if (!args) {
        snprintf(result_json, result_size, "Invalid arguments JSON.");
        return MCP_PLUGIN_CALL_ERROR;
    }
    local_path = json_object_get(args, "local_path");
    remote_path = json_object_get(args, "remote_path");
    if (parse_server_id(args, &server_id) != 0 ||
        !json_is_string(local_path) ||
        !json_is_string(remote_path)) {
        json_decref(args);
        snprintf(result_json, result_size, "server_id, local_path and remote_path are required.");
        return MCP_PLUGIN_CALL_ERROR;
    }
    timeout_ms = args_timeout_ms(args);

    if (strcmp(tool_name, "server.send") == 0)
        rc = start_send(invocation_id,
                        server_id,
                        json_string_value(local_path),
                        json_string_value(remote_path),
                        timeout_ms);
    else if (strcmp(tool_name, "server.recv") == 0)
        rc = start_recv(invocation_id,
                        server_id,
                        json_string_value(remote_path),
                        json_string_value(local_path),
                        timeout_ms);
    else
        rc = -1;
    json_decref(args);

    if (rc == 0)
        return MCP_PLUGIN_CALL_PENDING;
    if (rc == -2)
        snprintf(result_json, result_size, "MFT1 whole-file capability is not negotiated for server_id %u.", server_id);
    else
        snprintf(result_json, result_size, "File transfer setup failed.");
    return MCP_PLUGIN_CALL_ERROR;
}

MFT_PLUGIN_EXPORT int MFT_PLUGIN_SHUTDOWN(void)
{
    stop_timer_thread();
    while (g_transfers) {
        struct transfer_context *ctx = g_transfers;

        g_transfers = ctx->next;
        ctx->next = NULL;
        free_transfer(ctx);
    }
    if (g_plugin.host && g_plugin.host->peer_transport_unregister_handler)
        g_plugin.host->peer_transport_unregister_handler(g_plugin.host->host_context, MFT_MAGIC);
#ifdef _WIN32
    if (g_plugin.sync_initialized)
        DeleteCriticalSection(&g_plugin.lock);
#else
    if (g_plugin.sync_initialized) {
        pthread_cond_destroy(&g_plugin.timer_cond);
        pthread_mutex_destroy(&g_plugin.lock);
    }
#endif
    memset(&g_plugin, 0, sizeof(g_plugin));
    return 0;
}
