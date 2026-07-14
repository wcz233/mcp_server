#define MCP_FILE_TRANSFER_PLUGIN_BUILTIN 1
#include "../src/plugins/file_transfer/file_transfer_plugin.c"

#include <stdio.h>

struct deadline_test_context {
    unsigned int errors;
    char last_error[128];
};

static unsigned long long deadline_test_now_ms(void *host_context)
{
    (void)host_context;
    return 1000;
}

static int deadline_test_complete_error(void *host_context,
                                        const char *invocation_id,
                                        const char *message)
{
    struct deadline_test_context *context = host_context;

    (void)invocation_id;
    context->errors++;
    snprintf(context->last_error, sizeof(context->last_error), "%s", message);
    return 0;
}

static int deadline_test_send_frame(void *host_context,
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

static struct transfer_context *make_deadline_transfer(const char *id,
                                                       unsigned long long deadline_ms)
{
    struct transfer_context *ctx = calloc(1, sizeof(*ctx));

    if (!ctx)
        return NULL;
    ctx->invocation_id = mft_strdup(id);
    ctx->transfer_id = mft_strdup(id);
    ctx->deadline_ms = deadline_ms;
    ctx->server_id = 1;
    strcpy(ctx->direction, "receive");
    if (!ctx->invocation_id || !ctx->transfer_id) {
        free_transfer(ctx);
        return NULL;
    }
    return ctx;
}

int main(void)
{
    struct deadline_test_context context = {0};
    struct mcp_plugin_host_api host = {0};
    struct transfer_context *later = NULL;
    struct transfer_context *earlier = NULL;
    struct pending_block_ack *pending_acks = NULL;
    struct pending_transfer_error *pending_errors = NULL;
    struct pending_pump *pending_pumps = NULL;
    unsigned long long next_deadline = 0;
    int rc = 1;

    host.host_context = &context;
    host.now_ms = deadline_test_now_ms;
    host.complete_async_error = deadline_test_complete_error;
    host.peer_transport_send_frame = deadline_test_send_frame;
    g_plugin.host = &host;

    later = make_deadline_transfer("later", 3000);
    earlier = make_deadline_transfer("earlier", 2000);
    if (!later || !earlier)
        goto cleanup;
    later->next = earlier;
    g_transfers = later;

    recompute_next_package_deadline(&next_deadline);
    if (next_deadline != 2000)
        goto cleanup;
    scan_package_timeouts(1999,
                          &pending_acks,
                          &pending_errors,
                          &pending_pumps);
    if (pending_acks || pending_errors || pending_pumps ||
        g_transfers != later || later->next != earlier)
        goto cleanup;

    scan_package_timeouts(2000,
                          &pending_acks,
                          &pending_errors,
                          &pending_pumps);
    if (pending_acks || pending_pumps || !pending_errors ||
        pending_errors->ctx != earlier || g_transfers != later || later->next)
        goto cleanup;
    complete_pending_errors(pending_errors);
    pending_errors = NULL;
    earlier = NULL;
    if (context.errors != 1 || strcmp(context.last_error, "File transfer timed out.") != 0)
        goto cleanup;
    rc = 0;

cleanup:
    free_pending_acks(pending_acks);
    while (pending_pumps) {
        struct pending_pump *next = pending_pumps->next;

        free(pending_pumps->transfer_id);
        free(pending_pumps);
        pending_pumps = next;
    }
    if (pending_errors) {
        struct pending_transfer_error *error;

        for (error = pending_errors; error; error = error->next) {
            if (error->ctx == earlier)
                earlier = NULL;
            if (error->ctx == later)
                later = NULL;
        }
        complete_pending_errors(pending_errors);
    }
    while (g_transfers) {
        struct transfer_context *ctx = g_transfers;

        g_transfers = ctx->next;
        ctx->next = NULL;
        if (ctx == earlier)
            earlier = NULL;
        if (ctx == later)
            later = NULL;
        free_transfer(ctx);
    }
    if (earlier)
        free_transfer(earlier);
    if (later)
        free_transfer(later);
    memset(&g_plugin, 0, sizeof(g_plugin));
    return rc;
}
