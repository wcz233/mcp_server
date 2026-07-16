#define MCP_FILE_TRANSFER_PLUGIN_BUILTIN 1
#include "../src/plugins/file_transfer/file_transfer_plugin.c"

#include <stdio.h>

struct negotiation_test_context {
    unsigned long long now_ms;
    bool block_ack;
    bool chunk_window;
    bool crc32;
    unsigned int hello_frames;
    unsigned int fetch_frames;
    unsigned int errors;
    char last_error[128];
};

static unsigned long long negotiation_test_now_ms(void *host_context)
{
    struct negotiation_test_context *context = host_context;

    return context->now_ms;
}

static int negotiation_test_complete_error(void *host_context,
                                           const char *invocation_id,
                                           const char *message)
{
    struct negotiation_test_context *context = host_context;

    (void)invocation_id;
    context->errors++;
    snprintf(context->last_error, sizeof(context->last_error), "%s", message);
    return 0;
}

static int negotiation_test_send_frame(void *host_context,
                                       unsigned int server_id,
                                       const void *payload,
                                       uint32_t payload_len)
{
    struct negotiation_test_context *context = host_context;
    const unsigned char *frame = payload;

    (void)server_id;
    if (payload_len < 8)
        return -1;
    if (frame[5] == MFT_FRAME_HELLO)
        context->hello_frames++;
    else if (frame[5] == MFT_FRAME_FETCH_REQUEST)
        context->fetch_frames++;
    return 0;
}

static int negotiation_test_has_capability(void *host_context,
                                           unsigned int server_id,
                                           const char *capability)
{
    struct negotiation_test_context *context = host_context;

    (void)server_id;
    if (strcmp(capability, MFT_CAP_BLOCK_ACK) == 0)
        return context->block_ack;
    if (strcmp(capability, MFT_CAP_CHUNK_WINDOW) == 0)
        return context->chunk_window;
    if (strcmp(capability, MFT_CAP_CRC32) == 0)
        return context->crc32;
    return 0;
}

static int negotiation_test_set_capability(void *host_context,
                                           unsigned int server_id,
                                           const char *capability,
                                           int enabled)
{
    struct negotiation_test_context *context = host_context;

    (void)server_id;
    if (strcmp(capability, MFT_CAP_BLOCK_ACK) == 0)
        context->block_ack = enabled != 0;
    else if (strcmp(capability, MFT_CAP_CHUNK_WINDOW) == 0)
        context->chunk_window = enabled != 0;
    else if (strcmp(capability, MFT_CAP_CRC32) == 0)
        context->crc32 = enabled != 0;
    return 0;
}

static int invoke_recv(const char *invocation_id)
{
    char result[256] = {0};

    return MFT_PLUGIN_INVOKE(invocation_id,
                             "server.recv",
                             "{\"server_id\":7,\"local_path\":\".\","
                             "\"remote_path\":\"/remote.bin\",\"timeout_ms\":5000}",
                             result,
                             sizeof(result));
}

static void free_test_transfers(void)
{
    while (g_transfers) {
        struct transfer_context *ctx = g_transfers;

        g_transfers = ctx->next;
        ctx->next = NULL;
        free_transfer(ctx);
    }
}

int main(void)
{
    struct negotiation_test_context context = {0};
    struct mcp_plugin_host_api host = {0};
    struct pending_negotiation *expired = NULL;
    json_t *hello = NULL;
    unsigned long long next_deadline = 0;
    int rc = 1;

    context.now_ms = 1000;
    host.host_context = &context;
    host.now_ms = negotiation_test_now_ms;
    host.complete_async_error = negotiation_test_complete_error;
    host.peer_transport_send_frame = negotiation_test_send_frame;
    host.peer_transport_has_capability = negotiation_test_has_capability;
    host.peer_transport_set_capability = negotiation_test_set_capability;
    g_plugin.host = &host;

    if (invoke_recv("resume-on-hello") != MCP_PLUGIN_CALL_PENDING ||
        !g_pending_negotiations || g_transfers || context.hello_frames != 1)
        goto cleanup;
    hello = json_pack("{s:[s,s,s]}",
                      "capabilities",
                      MFT_CAP_BLOCK_ACK,
                      MFT_CAP_CHUNK_WINDOW,
                      MFT_CAP_CRC32);
    if (!hello)
        goto cleanup;
    handle_hello(7, hello);
    json_decref(hello);
    hello = NULL;
    if (!context.block_ack || !context.chunk_window || !context.crc32 ||
        g_pending_negotiations || !g_transfers ||
        !g_transfers->crc32_enabled ||
        context.hello_frames != 2 || context.fetch_frames != 1)
        goto cleanup;
    free_test_transfers();

    context.chunk_window = false;
    context.crc32 = false;
    if (invoke_recv("legacy-block-ack") != MCP_PLUGIN_CALL_PENDING ||
        g_pending_negotiations || !g_transfers ||
        g_transfers->crc32_enabled ||
        context.hello_frames != 2 || context.fetch_frames != 2)
        goto cleanup;
    free_test_transfers();

    context.block_ack = false;
    context.now_ms = 5000;
    if (invoke_recv("negotiation-timeout") != MCP_PLUGIN_CALL_PENDING ||
        !g_pending_negotiations)
        goto cleanup;
    recompute_next_package_deadline(&next_deadline);
    if (next_deadline != 6000)
        goto cleanup;
    scan_negotiation_timeouts(5999, &expired);
    if (expired || !g_pending_negotiations)
        goto cleanup;
    scan_negotiation_timeouts(6000, &expired);
    if (!expired || g_pending_negotiations)
        goto cleanup;
    complete_pending_negotiation_errors(expired,
                                        "MFT1 capability negotiation timed out.");
    expired = NULL;
    if (context.errors != 1 ||
        strcmp(context.last_error, "MFT1 capability negotiation timed out.") != 0)
        goto cleanup;
    rc = 0;

cleanup:
    json_decref(hello);
    free_pending_negotiations(expired);
    free_pending_negotiations(g_pending_negotiations);
    g_pending_negotiations = NULL;
    free_test_transfers();
    memset(&g_plugin, 0, sizeof(g_plugin));
    return rc;
}
