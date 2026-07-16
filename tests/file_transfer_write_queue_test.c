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
    unsigned int window_updates;
    unsigned int block_acks;
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
    struct test_host_context *context = host_context;
    const unsigned char *frame = payload;

    (void)server_id;
    if (payload_len >= 8) {
        if (frame[5] == MFT_FRAME_WINDOW_UPDATE)
            context->window_updates++;
        else if (frame[5] == MFT_FRAME_ACK)
            context->block_acks++;
    }
    return 0;
}

static struct transfer_context *make_receive_context(FILE *fp, size_t size)
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
    entry->size = size;
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

static int run_parallel_window_test(void)
{
    const char transfer_id[] = "test-transfer";
    const unsigned int chunk_count = 8;
    const size_t total_size = chunk_count * MFT_MAX_CHUNK;
    const size_t frame_size = 88 + sizeof(transfer_id) - 1 + MFT_MAX_CHUNK;
    struct test_host_context context = {0};
    struct mcp_plugin_host_api host = {0};
    struct transfer_context *ctx = NULL;
    unsigned char *frame = NULL;
    unsigned char *expected = NULL;
    unsigned char *actual = NULL;
    unsigned char *data;
    char chunk_hash[65];
    char block_hash[65];
    FILE *fp = NULL;
    unsigned int i;
    int rc = 1;

    if (uv_loop_init(&context.loop) != 0)
        return 1;
    fp = tmpfile();
    if (!fp)
        goto cleanup_loop;
    ctx = make_receive_context(fp, total_size);
    if (!ctx)
        goto cleanup_file;
    ctx->chunk_window_enabled = true;
    ctx->crc32_enabled = true;
    ctx->entries[0].block_crc32 = true;

    expected = malloc(total_size);
    actual = malloc(total_size);
    frame = calloc(1, frame_size);
    if (!expected || !actual || !frame)
        goto cleanup_transfer;
    memset(expected, 0x5a, total_size);
    bytes_crc32(expected, total_size, block_hash);
    snprintf(ctx->entries[0].blocks[0].hash,
             sizeof(ctx->entries[0].blocks[0].hash),
             "%s",
             block_hash);

    host.host_context = &context;
    host.now_ms = test_now_ms;
    host.get_loop = test_get_loop;
    host.complete_async_error = test_complete_async_error;
    host.peer_transport_send_frame = test_send_frame;
    g_plugin.host = &host;
    g_transfers = ctx;

    memcpy(frame, MFT_MAGIC, 4);
    frame[4] = MFT_VERSION;
    frame[5] = MFT_FRAME_DATA;
    write_u16_be(frame + 6, (uint16_t)(sizeof(transfer_id) - 1));
    write_u32_be(frame + 8, 1);
    write_u32_be(frame + 20, MFT_MAX_CHUNK);
    memcpy(frame + 88, transfer_id, sizeof(transfer_id) - 1);
    data = frame + 88 + sizeof(transfer_id) - 1;
    memset(data, 0x5a, MFT_MAX_CHUNK);
    bytes_crc32(data, MFT_MAX_CHUNK, chunk_hash);
    memcpy(frame + 24, chunk_hash, 8);

    for (i = 0; i < chunk_count; i++) {
        write_u64_be(frame + 12, (uint64_t)i * MFT_MAX_CHUNK);
        handle_data(1, frame, frame_size);
    }
    if (context.error_called ||
        ctx->write_active_jobs != MFT_MAX_ACTIVE_WRITE_JOBS ||
        ctx->write_jobs != chunk_count)
        goto cleanup_jobs;

    uv_run(&context.loop, UV_RUN_DEFAULT);
    if (context.error_called || g_plugin.io_jobs != 0 ||
        ctx->write_active_jobs != 0 || ctx->write_jobs != 0 ||
        !ctx->entries[0].blocks[0].received_ok ||
        context.window_updates != 1 || context.block_acks != 1)
        goto cleanup_jobs;
    if (fseek(fp, 0, SEEK_SET) != 0 || fread(actual, 1, total_size, fp) != total_size ||
        memcmp(actual, expected, total_size) != 0)
        goto cleanup_jobs;
    rc = 0;

cleanup_jobs:
    uv_run(&context.loop, UV_RUN_DEFAULT);
cleanup_transfer:
    free(frame);
    free(actual);
    free(expected);
    if (g_transfers == ctx)
        g_transfers = NULL;
    free_transfer(ctx);
    memset(&g_plugin, 0, sizeof(g_plugin));
cleanup_file:
    fclose(fp);
cleanup_loop:
    if (uv_loop_close(&context.loop) != 0)
        rc = 1;
    return rc;
}

static int run_window_credit_test(void)
{
    struct test_host_context context = {0};
    struct mcp_plugin_host_api host = {0};
    struct transfer_context *ctx = calloc(1, sizeof(*ctx));
    json_t *payload = NULL;
    int rc = 1;

    if (!ctx)
        return 1;
    ctx->transfer_id = mft_strdup("window-credit-test");
    ctx->entries = calloc(1, sizeof(*ctx->entries));
    if (!ctx->transfer_id || !ctx->entries)
        goto cleanup;
    ctx->entry_count = 1;
    ctx->entries[0].type = 'f';
    ctx->server_id = 1;
    ctx->timeout_ms = 5000;
    ctx->chunk_window_enabled = true;
    ctx->send_session_window = MFT_INITIAL_SESSION_WINDOW - MFT_INITIAL_STREAM_WINDOW;
    ctx->pumping = true;
    strcpy(ctx->direction, "send");

    host.host_context = &context;
    host.now_ms = test_now_ms;
    g_plugin.host = &host;
    g_transfers = ctx;

    payload = json_pack("{s:s,s:i,s:i}",
                        "transfer_id",
                        ctx->transfer_id,
                        "stream_id",
                        1,
                        "bytes",
                        (int)MFT_INITIAL_STREAM_WINDOW);
    if (!payload)
        goto cleanup;
    handle_window_update(1, payload);
    if (ctx->send_session_window != MFT_INITIAL_SESSION_WINDOW ||
        ctx->entries[0].send_window_bytes != MFT_INITIAL_STREAM_WINDOW ||
        !ctx->send_paused)
        goto cleanup;

    handle_window_update(1, payload);
    if (ctx->send_session_window != MFT_INITIAL_SESSION_WINDOW ||
        ctx->entries[0].send_window_bytes != MFT_INITIAL_STREAM_WINDOW)
        goto cleanup;
    json_decref(payload);
    payload = json_pack("{s:s,s:i,s:i}",
                        "transfer_id",
                        ctx->transfer_id,
                        "stream_id",
                        1,
                        "bytes",
                        (int)MFT_INITIAL_STREAM_WINDOW + 1);
    if (!payload)
        goto cleanup;
    handle_window_update(1, payload);
    if (ctx->send_session_window != MFT_INITIAL_SESSION_WINDOW ||
        ctx->entries[0].send_window_bytes != MFT_INITIAL_STREAM_WINDOW)
        goto cleanup;
    rc = 0;

cleanup:
    json_decref(payload);
    g_transfers = NULL;
    free_transfer(ctx);
    memset(&g_plugin, 0, sizeof(g_plugin));
    return rc;
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
    ctx = make_receive_context(fp, 8u * 1024u * 1024u);
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
    if (rc == 0)
        rc = run_parallel_window_test();
    return rc == 0 ? run_window_credit_test() : rc;
}
