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
#define MFT_MAX_CHUNK 32768u
#define MFT_MAX_CONCURRENT_STREAMS 4u
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

struct manifest_entry {
    char *relpath;
    char type;
    uint64_t size;
    uint64_t mtime_ns;
    unsigned int mode;
    char hash[65];
};

struct mft_plugin {
    const struct mcp_plugin_host_api *host;
    unsigned long long next_transfer;
};

static struct mft_plugin g_plugin;

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

static bool path_is_separator(char ch)
{
#ifdef _WIN32
    return ch == '/' || ch == '\\';
#else
    return ch == '/';
#endif
}

static bool path_has_trailing_slash(const char *path)
{
    size_t len;

    if (!path)
        return false;
    len = strlen(path);
    return len > 0 && path_is_separator(path[len - 1]);
}

static char *path_basename_dup(const char *path)
{
    const char *end;
    const char *start;
    char name[PATH_MAX];
    size_t len;

    if (!path || path[0] == '\0')
        return NULL;
    end = path + strlen(path);
    while (end > path && path_is_separator(end[-1]))
        end--;
    if (end == path)
        return NULL;
    start = end;
    while (start > path && !path_is_separator(start[-1]))
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

static char *path_dirname_dup(const char *path)
{
    const char *end;
    const char *slash;
    size_t len;
    char dir[PATH_MAX];

    if (!path || path[0] == '\0')
        return NULL;
    end = path + strlen(path);
    while (end > path && path_is_separator(end[-1]))
        end--;
    if (end == path)
        return NULL;
    slash = end;
    while (slash > path && !path_is_separator(slash[-1]))
        slash--;
    if (slash == path)
        return mft_strdup(".");
    if (slash == path + 1 && path[0] == '/')
        return mft_strdup("/");
#ifdef _WIN32
    if (slash == path + 3 && path[1] == ':' && path_is_separator(path[2])) {
        memcpy(dir, path, 3);
        dir[3] = '\0';
        return mft_strdup(dir);
    }
#endif
    len = (size_t)(slash - path - 1);
    if (len == 0 || len >= sizeof(dir))
        return NULL;
    memcpy(dir, path, len);
    dir[len] = '\0';
    return mft_strdup(dir);
}

static int join_path(char *out, size_t out_len, const char *root, const char *rel)
{
    int len;

    if (!root || !rel)
        return -1;
    if (strcmp(rel, ".") == 0)
        len = snprintf(out, out_len, "%s", root);
    else if (path_is_safe_rel(rel))
        len = snprintf(out, out_len, "%s/%s", root, rel);
    else
        return -1;
    return len >= 0 && (size_t)len < out_len ? 0 : -1;
}

static int append_path_component(char *out, size_t out_len, const char *root, const char *name)
{
    int len;

    if (!root || !name || !path_is_safe_rel(name) || strcmp(name, ".") == 0)
        return -1;
    len = snprintf(out,
                   out_len,
                   "%s%s%s",
                   root,
                   path_has_trailing_slash(root) ? "" : "/",
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
    for (p = tmp + 1; *p; p++) {
        if (*p != '/')
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
    struct stat st;

    if (part_path(path, sizeof(path), final_path) != 0)
        return 0;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0)
        return 0;
    if ((uint64_t)st.st_size >= entry->size)
        return 0;
    return (uint64_t)st.st_size;
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
    fd = open(path, O_CREAT | O_WRONLY | MFT_OPEN_BINARY | (offset == 0 ? O_TRUNC : 0), 0666);
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

static int copy_file_range_chunks(FILE *fp,
                                  uint64_t offset,
                                  uint64_t size,
                                  unsigned int server_id,
                                  const char *transfer_id,
                                  uint32_t stream_id)
{
    unsigned char *frame;
    unsigned char buf[MFT_MAX_CHUNK];
    uint64_t pos = offset;

    if (fseek(fp, (long)offset, SEEK_SET) != 0)
        return -1;

    while (pos < size) {
        size_t want = (size_t)((size - pos) > MFT_MAX_CHUNK ? MFT_MAX_CHUNK : (size - pos));
        size_t n = fread(buf, 1, want, fp);
        uint16_t tid_len = (uint16_t)strlen(transfer_id);
        size_t header_len = 4 + 1 + 1 + 2 + 4 + 8 + 4 + tid_len;

        if (n == 0)
            return -1;
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
        memcpy(frame + 24, transfer_id, tid_len);
        memcpy(frame + header_len, buf, n);
        if (g_plugin.host->peer_transport_send_frame(g_plugin.host->host_context,
                                                     server_id,
                                                     frame,
                                                     (uint32_t)(header_len + n)) != 0) {
            free(frame);
            return -1;
        }
        free(frame);
        pos += n;
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

    for (i = 0; i < count; i++)
        free(entries[i].relpath);
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
    if (file_sha256(path, entry->hash) != 0) {
        free(entry->relpath);
        memset(entry, 0, sizeof(*entry));
        return -1;
    }
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
        if (entry->type == 'f')
            json_object_set_new(item, "hash", json_string(entry->hash));
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
            if (!json_is_string(hash) || strlen(json_string_value(hash)) != 64) {
                free_entries(entries, count + 1);
                return -1;
            }
            snprintf(entry->hash, sizeof(entry->hash), "%s", json_string_value(hash));
        }
        count++;
    }

    *out_entries = entries;
    *out_count = count;
    return 0;
}

static int send_hello(unsigned int server_id)
{
    json_t *payload = json_pack("{s:i,s:[s,s],s:i,s:i,s:i,s:i}",
                                "version",
                                1,
                                "capabilities",
                                MFT_CAP_WHOLE_FILE,
                                MFT_CAP_RESUME,
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
            strcmp(name, MFT_CAP_RESUME) == 0) {
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
                                                     MFT_CAP_WHOLE_FILE))
        return 0;
    send_hello(server_id);
    for (i = 0; i < 50; i++) {
        if (g_plugin.host->peer_transport_has_capability(g_plugin.host->host_context,
                                                         server_id,
                                                         MFT_CAP_WHOLE_FILE))
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

static int receive_entry_data(const char *target_root,
                              struct manifest_entry *entries,
                              size_t count,
                              const char *relpath,
                              const unsigned char *data,
                              size_t len,
                              uint64_t offset)
{
    size_t i;

    for (i = 0; i < count; i++) {
        struct manifest_entry *entry = &entries[i];
        char final_path[PATH_MAX];
        char tmp_part[PATH_MAX];

        if (strcmp(entry->relpath, relpath) != 0)
            continue;
        if (entry->type != 'f')
            return -1;
        if (join_path(final_path, sizeof(final_path), target_root, relpath) != 0 ||
            part_path(tmp_part, sizeof(tmp_part), final_path) != 0)
            return -1;
        return write_all_at(tmp_part, data, len, offset);
    }
    return -1;
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
        char hash[65];
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
        if (file_sha256(tmp_part, hash) != 0 || strcmp(hash, entry->hash) != 0)
            return -1;
        chmod(tmp_part, entry->mode ? (mode_t)entry->mode : 0666);
        if (rename(tmp_part, final_path) != 0)
            return -1;
        (*files_transferred)++;
    }
    return 0;
}

static int send_entries(unsigned int server_id,
                        const char *transfer_id,
                        const char *source_root,
                        struct manifest_entry *entries,
                        size_t count,
                        json_t *accept,
                        uint64_t *bytes_transferred)
{
    size_t i;

    *bytes_transferred = 0;
    for (i = 0; i < count; i++) {
        struct manifest_entry *entry = &entries[i];
        json_t *accept_item = json_array_get(accept, i);
        json_t *decision = accept_item ? json_object_get(accept_item, "decision") : NULL;
        json_t *resume = accept_item ? json_object_get(accept_item, "resume_offset") : NULL;
        char path[PATH_MAX];
        FILE *fp;
        uint64_t resume_offset = json_is_integer(resume) ? (uint64_t)json_integer_value(resume) : 0;

        if (entry->type != 'f')
            continue;
        if (json_is_string(decision) && strcmp(json_string_value(decision), "skip") == 0)
            continue;
        if (join_path(path, sizeof(path), source_root, entry->relpath) != 0)
            return -1;
        fp = fopen(path, "rb");
        if (!fp)
            return -1;
        if (copy_file_range_chunks(fp,
                                   resume_offset,
                                   entry->size,
                                   server_id,
                                   transfer_id,
                                   (uint32_t)(i + 1)) != 0) {
            fclose(fp);
            return -1;
        }
        fclose(fp);
        *bytes_transferred += entry->size - resume_offset;
    }
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
    char *dump = json_dumps(summary, JSON_COMPACT | JSON_ENSURE_ASCII);
    int rc = dump ? g_plugin.host->complete_async_ok(g_plugin.host->host_context,
                                                     invocation_id,
                                                     dump) : -1;

    free(dump);
    json_decref(summary);
    return rc;
}

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
    struct manifest_entry *entries;
    size_t entry_count;
    json_t *accept;
    struct transfer_context *next;
};

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
    free_entries(ctx->entries, ctx->entry_count);
    json_decref(ctx->accept);
    free(ctx);
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

static void complete_transfer_error(struct transfer_context *ctx,
                                    const char *message,
                                    bool notify_peer)
{
    char error[256];

    if (!ctx)
        return;
    snprintf(error, sizeof(error), "%s", message ? message : "File transfer failed.");
    unlink_transfer(ctx);
    if (notify_peer)
        send_abort_frame(ctx->server_id, ctx->transfer_id, error);
    if (ctx->invocation_id && g_plugin.host->complete_async_error)
        g_plugin.host->complete_async_error(g_plugin.host->host_context,
                                            ctx->invocation_id,
                                            error);
    free_transfer(ctx);
}

static void abort_transfers_for_peer(unsigned int server_id)
{
    struct transfer_context *ctx = g_transfers;

    while (ctx) {
        struct transfer_context *next = ctx->next;

        if (ctx->server_id == server_id)
            complete_transfer_error(ctx, "Peer closed during file transfer.", false);
        ctx = next;
    }
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

static void expire_due_transfers(void)
{
    struct transfer_context *ctx;
    unsigned long long now;

    if (!g_plugin.host || !g_plugin.host->now_ms)
        return;
    now = g_plugin.host->now_ms(g_plugin.host->host_context);
    ctx = g_transfers;
    while (ctx) {
        struct transfer_context *next = ctx->next;

        if (ctx->deadline_ms && now >= ctx->deadline_ms)
            complete_transfer_error(ctx, "File transfer timed out.", true);
        ctx = next;
    }
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
    for (i = 0; i < ctx->entry_count; i++) {
        if (ctx->entries[i].type == 'f')
            bytes_total += ctx->entries[i].size;
    }
    manifest = entries_to_manifest(ctx->entries, ctx->entry_count);
    if (!manifest)
        goto fail;
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
    add_transfer(ctx);
    if (send_json_frame(server_id, MFT_FRAME_OFFER, payload) != 0) {
        unlink_transfer(ctx);
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
    ctx->source_name = path_basename_dup(remote_path);
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
    add_transfer(ctx);
    if (send_json_frame(server_id, MFT_FRAME_FETCH_REQUEST, payload) != 0) {
        unlink_transfer(ctx);
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

    if (!json_is_string(transfer_id) || !json_is_string(remote_path) ||
        manifest_to_entries(manifest, &entries, &count) != 0)
        return;

    ctx = find_transfer(json_string_value(transfer_id));
    if (json_is_string(source_name))
        source_name_value = json_string_value(source_name);
    else if (ctx && ctx->source_name)
        source_name_value = ctx->source_name;
    if (source_name_value &&
        (strcmp(source_name_value, ".") == 0 || !path_is_safe_rel(source_name_value))) {
        free_entries(entries, count);
        if (ctx)
            complete_transfer_error(ctx, "Invalid source_name in file transfer offer.", true);
        else
            send_abort_frame(server_id,
                             json_string_value(transfer_id),
                             "Invalid source_name in file transfer offer.");
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
        if (ctx)
            complete_transfer_error(ctx, "Failed to prepare receive target path.", true);
        else
            send_abort_frame(server_id,
                             json_string_value(transfer_id),
                             "Failed to prepare receive target path.");
        return;
    }

    if (!ctx) {
        ctx = calloc(1, sizeof(*ctx));
        if (!ctx) {
            free_entries(entries, count);
            json_decref(accept);
            return;
        }
        created_ctx = 1;
        ctx->transfer_id = mft_strdup(json_string_value(transfer_id));
        ctx->local_path = mft_strdup(target_root);
        if (source_name_value)
            ctx->source_name = mft_strdup(source_name_value);
        if (!ctx->transfer_id || !ctx->local_path) {
            free_transfer(ctx);
            json_decref(accept);
            free_entries(entries, count);
            send_abort_frame(server_id,
                             json_string_value(transfer_id),
                             "Failed to allocate receive context.");
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
            complete_transfer_error(ctx, "Failed to allocate receive target path.", true);
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

    response = json_pack("{s:s,s:o}", "transfer_id", ctx->transfer_id, "accept", accept);
    if (!response || send_json_frame(server_id, MFT_FRAME_ACCEPT, response) != 0)
        complete_transfer_error(ctx, "Failed to send file transfer accept.", true);
    json_decref(response);
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
    add_transfer(ctx);
    if (send_json_frame(server_id, MFT_FRAME_OFFER, offer) != 0) {
        unlink_transfer(ctx);
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
    uint64_t bytes_total = 0;
    uint64_t bytes_transferred = 0;
    unsigned int files_skipped = 0;
    unsigned int files_transferred = 0;
    size_t i;
    json_t *complete;

    if (!json_is_string(transfer_id) || !json_is_array(accept))
        return;
    ctx = find_transfer(json_string_value(transfer_id));
    if (!ctx)
        return;

    for (i = 0; i < ctx->entry_count; i++) {
        if (ctx->entries[i].type == 'f')
            bytes_total += ctx->entries[i].size;
    }
    if (send_entries(server_id,
                     ctx->transfer_id,
                     ctx->local_path,
                     ctx->entries,
                     ctx->entry_count,
                     accept,
                     &bytes_transferred) != 0)
    {
        complete_transfer_error(ctx, "Failed to send file transfer data.", true);
        return;
    }
    for (i = 0; i < json_array_size(accept); i++) {
        json_t *item = json_array_get(accept, i);
        json_t *decision = item ? json_object_get(item, "decision") : NULL;
        if (json_is_string(decision) && strcmp(json_string_value(decision), "skip") == 0)
            files_skipped++;
        else if (json_is_string(decision) && strcmp(json_string_value(decision), "receive") == 0)
            files_transferred++;
    }
    complete = json_pack("{s:s,s:I,s:I,s:i,s:i}",
                         "transfer_id",
                         ctx->transfer_id,
                         "bytes_total",
                         (json_int_t)bytes_total,
                         "bytes_transferred",
                         (json_int_t)bytes_transferred,
                         "files_transferred",
                         (int)files_transferred,
                         "files_skipped",
                         (int)files_skipped);
    if (!complete || send_json_frame(server_id, MFT_FRAME_COMPLETE, complete) != 0) {
        json_decref(complete);
        complete_transfer_error(ctx, "Failed to send file transfer completion.", true);
        return;
    }
    json_decref(complete);
}

static void handle_data(unsigned int server_id,
                        const unsigned char *payload,
                        size_t len)
{
    uint16_t tid_len;
    uint32_t stream_id;
    uint64_t offset;
    uint32_t data_len;
    char transfer_id[256];
    struct transfer_context *ctx;

    (void)server_id;

    if (len < 24)
        return;
    tid_len = read_u16_be(payload + 6);
    stream_id = read_u32_be(payload + 8);
    offset = read_u64_be(payload + 12);
    data_len = read_u32_be(payload + 20);
    if (tid_len == 0 || tid_len >= sizeof(transfer_id) || len < 24u + tid_len + data_len)
        return;
    memcpy(transfer_id, payload + 24, tid_len);
    transfer_id[tid_len] = '\0';
    ctx = find_transfer(transfer_id);
    if (!ctx || stream_id == 0 || stream_id > ctx->entry_count)
        return;
    if (receive_entry_data(ctx->local_path,
                           ctx->entries,
                           ctx->entry_count,
                           ctx->entries[stream_id - 1].relpath,
                           payload + 24 + tid_len,
                           data_len,
                           offset) != 0)
        complete_transfer_error(ctx, "Failed to write received file data.", true);
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

    if (!json_is_string(transfer_id))
        return;
    ctx = find_transfer(json_string_value(transfer_id));
    if (!ctx)
        return;
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

        if (finalize_received(ctx->local_path,
                              ctx->entries,
                              ctx->entry_count,
                              ctx->accept,
                              &files_skipped,
                              &files_transferred) != 0)
        {
            complete_transfer_error(ctx, "Failed to finalize received file transfer.", true);
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
        if (!ack || send_json_frame(server_id, MFT_FRAME_COMPLETE, ack) != 0) {
            json_decref(ack);
            complete_transfer_error(ctx, "Failed to send file transfer acknowledgement.", true);
            return;
        }
        json_decref(ack);
    }
    if (bytes_transferred == 0)
        bytes_transferred = bytes_total;
    if (ctx->invocation_id)
        send_summary(ctx->invocation_id,
                     ctx->transfer_id,
                     server_id,
                     strcmp(ctx->direction, "receive") == 0 ? "recv" : "send",
                     ctx->entry_count,
                     files_transferred,
                     files_skipped,
                     bytes_total,
                     bytes_transferred,
                     ctx->started_ms);
    unlink_transfer(ctx);
    free_transfer(ctx);
}

static void handle_abort(json_t *payload)
{
    json_t *transfer_id = json_object_get(payload, "transfer_id");
    json_t *message = json_object_get(payload, "message");
    struct transfer_context *ctx;

    if (!json_is_string(transfer_id))
        return;
    ctx = find_transfer(json_string_value(transfer_id));
    if (!ctx)
        return;
    complete_transfer_error(ctx,
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
    while (g_transfers) {
        struct transfer_context *ctx = g_transfers;

        g_transfers = ctx->next;
        ctx->next = NULL;
        free_transfer(ctx);
    }
    if (g_plugin.host && g_plugin.host->peer_transport_unregister_handler)
        g_plugin.host->peer_transport_unregister_handler(g_plugin.host->host_context, MFT_MAGIC);
    memset(&g_plugin, 0, sizeof(g_plugin));
    return 0;
}
