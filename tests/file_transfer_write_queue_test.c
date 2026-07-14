#define MCP_FILE_TRANSFER_PLUGIN_BUILTIN 1
#include "../src/plugins/file_transfer/file_transfer_plugin.c"

#include <stdio.h>

#ifdef _WIN32
#define test_dup _dup
#define test_fileno _fileno
#else
#define test_dup dup
#define test_fileno fileno
#endif

struct test_host_context {
    uv_loop_t loop;
    bool error_called;
    char error[128];
};

static unsigned long long test_now_ms(void *host_context)
{
    (void)host_context;
    return 1000;
}

static void *test_get_loop(void *host_context)
{
    struct test_host_context *context = host_context;

    return &context->loop;
}

static int test_complete_async_error(void *host_context,
                                     const char *invocation_id,
                                     const char *message)
{
    struct test_host_context *context = host_context;

    (void)invocation_id;
    context->error_called = true;
    snprintf(context->error, sizeof(context->error), "%s", message);
    return 0;
}

static int test_send_frame(void *host_context,
                           unsigned int server_id,
                           const void *payload,
                           uint32_t payload_len)
{
    (void)host_context;
    (void)server_id;
    (void)payload;
    (void)payload_len;
    return 0;
}

static struct transfer_context *make_receive_context(FILE *fp)
{
    struct transfer_context *ctx = calloc(1, sizeof(*ctx));
    struct manifest_entry *entry;
    struct block_entry *block;

    if (!ctx)
        return NULL;
    ctx->invocation_id = mft_strdup("test-invocation");
    ctx->transfer_id = mft_strdup("test-transfer");
    ctx->entries = calloc(1, sizeof(*ctx->entries));
    if (!ctx->invocation_id || !ctx->transfer_id || !ctx->entries)
        goto fail;
    ctx->entry_count = 1;
    ctx->server_id = 1;
    strcpy(ctx->direction, "receive");

    entry = &ctx->entries[0];
    entry->relpath = mft_strdup("queue-test.bin");
    entry->type = 'f';
    entry->size = 8u * 1024u * 1024u;
    entry->block_count = 1;
    entry->blocks = calloc(1, sizeof(*entry->blocks));
    entry->receive_fd = test_dup(test_fileno(fp));
    entry->receive_fd_open = entry->receive_fd >= 0;
    if (!entry->relpath || !entry->blocks || !entry->receive_fd_open)
        goto fail;
    block = &entry->blocks[0];
    block->size = entry->size;
    return ctx;

fail:
    free_transfer(ctx);
    return NULL;
}

int main(void)
{
    const char transfer_id[] = "test-transfer";
    const size_t frame_size = 88 + sizeof(transfer_id) - 1 + MFT_MAX_CHUNK;
    struct test_host_context context = {0};
    struct mcp_plugin_host_api host = {0};
    struct transfer_context *ctx;
    unsigned char *frame = NULL;
    unsigned char *data;
    char chunk_hash[65];
    FILE *fp = NULL;
    unsigned int i;
    int rc = 1;

    if (uv_loop_init(&context.loop) != 0)
        return 1;
    fp = tmpfile();
    if (!fp)
        goto cleanup_loop;
    ctx = make_receive_context(fp);
    if (!ctx)
        goto cleanup_file;

    host.host_context = &context;
    host.now_ms = test_now_ms;
    host.get_loop = test_get_loop;
    host.complete_async_error = test_complete_async_error;
    host.peer_transport_send_frame = test_send_frame;
    g_plugin.host = &host;
    g_transfers = ctx;

    frame = calloc(1, frame_size);
    if (!frame)
        goto cleanup_transfer;
    memcpy(frame, MFT_MAGIC, 4);
    frame[4] = MFT_VERSION;
    frame[5] = MFT_FRAME_DATA;
    write_u16_be(frame + 6, (uint16_t)(sizeof(transfer_id) - 1));
    write_u32_be(frame + 8, 1);
    write_u32_be(frame + 20, MFT_MAX_CHUNK);
    memcpy(frame + 88, transfer_id, sizeof(transfer_id) - 1);
    data = frame + 88 + sizeof(transfer_id) - 1;
    memset(data, 0x5a, MFT_MAX_CHUNK);
    bytes_sha256(data, MFT_MAX_CHUNK, chunk_hash);
    memcpy(frame + 24, chunk_hash, 64);

    for (i = 0; i <= MFT_MAX_WRITE_JOBS; i++) {
        write_u64_be(frame + 12, (uint64_t)i * MFT_MAX_CHUNK);
        handle_data(1, frame, frame_size);
    }
    if (!context.error_called ||
        strstr(context.error, "receive queue limit exceeded") == NULL ||
        g_transfers != NULL)
        goto cleanup_jobs;

    uv_run(&context.loop, UV_RUN_DEFAULT);
    if (g_plugin.io_jobs != 0)
        goto cleanup_jobs;
    rc = 0;

cleanup_jobs:
    uv_run(&context.loop, UV_RUN_DEFAULT);
    free(frame);
    memset(&g_plugin, 0, sizeof(g_plugin));
cleanup_transfer:
    if (g_transfers) {
        free_transfer(g_transfers);
        g_transfers = NULL;
    }
cleanup_file:
    fclose(fp);
cleanup_loop:
    if (uv_loop_close(&context.loop) != 0)
        rc = 1;
    return rc;
}
