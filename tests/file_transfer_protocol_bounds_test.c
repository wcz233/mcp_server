#define MCP_FILE_TRANSFER_PLUGIN_BUILTIN 1
#include "../src/plugins/file_transfer/file_transfer_plugin.c"

#include <stdio.h>

#define TEST_HASH "0000000000000000000000000000000000000000000000000000000000000000"

static json_t *wrap_entries(json_t *entries)
{
    json_t *manifest = json_object();

    if (!manifest) {
        json_decref(entries);
        return NULL;
    }
    if (json_object_set_new(manifest, "entries", entries) != 0) {
        json_decref(manifest);
        return NULL;
    }
    return manifest;
}

static json_t *make_directory_manifest(size_t count)
{
    json_t *array = json_array();
    size_t i;

    if (!array)
        return NULL;
    for (i = 0; i < count; i++) {
        char relpath[32];
        json_t *item;

        snprintf(relpath, sizeof(relpath), "dir-%lu", (unsigned long)i);
        item = json_pack("{s:s,s:s,s:I,s:I,s:i}",
                         "relpath",
                         relpath,
                         "type",
                         "directory",
                         "size",
                         (json_int_t)0,
                         "mtime_ns",
                         (json_int_t)0,
                         "mode",
                         0755);
        if (!item || json_array_append_new(array, item) != 0) {
            json_decref(array);
            return NULL;
        }
    }
    return wrap_entries(array);
}

static json_t *make_file_manifest(size_t block_count)
{
    json_t *blocks = json_array();
    json_t *array = json_array();
    json_t *item;
    size_t i;

    if (!blocks || !array)
        goto fail;
    for (i = 0; i < block_count; i++) {
        json_t *block = json_pack("{s:I,s:I,s:s}",
                                  "offset",
                                  (json_int_t)((uint64_t)i * MFT_LOGICAL_BLOCK_SIZE),
                                  "size",
                                  (json_int_t)MFT_LOGICAL_BLOCK_SIZE,
                                  "hash",
                                  TEST_HASH);

        if (!block || json_array_append_new(blocks, block) != 0) {
            goto fail;
        }
    }
    item = json_pack("{s:s,s:s,s:I,s:I,s:i,s:s,s:I,s:o}",
                     "relpath",
                     "file.bin",
                     "type",
                     "file",
                     "size",
                     (json_int_t)((uint64_t)block_count * MFT_LOGICAL_BLOCK_SIZE),
                     "mtime_ns",
                     (json_int_t)0,
                     "mode",
                     0644,
                     "hash",
                     TEST_HASH,
                     "block_size",
                     (json_int_t)MFT_LOGICAL_BLOCK_SIZE,
                     "blocks",
                     blocks);
    blocks = NULL;
    if (!item || json_array_append_new(array, item) != 0) {
        goto fail;
    }
    return wrap_entries(array);

fail:
    json_decref(blocks);
    json_decref(array);
    return NULL;
}

static int manifest_result(json_t *manifest, int expected_success, size_t expected_count)
{
    struct manifest_entry *entries = NULL;
    size_t count = 0;
    int rc = manifest_to_entries(manifest, false, &entries, &count);
    int success = rc == 0;

    if (success)
        free_entries(entries, count);
    return success == expected_success && (!success || count == expected_count) ? 0 : -1;
}

static int test_crc32_contract(void)
{
    static const unsigned char input[] = "123456789";
    struct transfer_context ctx = {0};
    struct receive_write_job *job = NULL;
    unsigned char field[64] = {0};
    char crc[9];
    json_t *manifest = make_file_manifest(1);
    json_t *item;
    json_t *block;
    struct manifest_entry *entries = NULL;
    size_t count = 0;
    int rc = -1;

    bytes_crc32(input, sizeof(input) - 1, crc);
    if (strcmp(crc, "cbf43926") != 0 || !manifest)
        goto cleanup;
    memcpy(field, crc, 8);
    if (!valid_crc32_field(field))
        goto cleanup;
    field[8] = 1;
    if (valid_crc32_field(field))
        goto cleanup;
    field[8] = 0;
    job = calloc(1, sizeof(*job) + sizeof(input) - 1);
    if (!job)
        goto cleanup;
    ctx.crc32_enabled = true;
    job->ctx = &ctx;
    job->req.data = job;
    job->data_len = sizeof(input) - 1;
    memcpy(job->data, input, job->data_len);
    strcpy(job->chunk_hash, "00000000");
    receive_write_work(&job->req);
    if (job->result != RECEIVE_WRITE_CHUNK_MISMATCH)
        goto cleanup;
    strcpy(job->chunk_hash, crc);
    receive_write_work(&job->req);
    if (job->result != RECEIVE_WRITE_OK)
        goto cleanup;
    item = json_array_get(json_object_get(manifest, "entries"), 0);
    block = json_array_get(json_object_get(item, "blocks"), 0);
    if (json_object_set_new(block, "hash", json_string(crc)) != 0 ||
        manifest_to_entries(manifest, true, &entries, &count) != 0 ||
        count != 1 || !entries[0].block_crc32 ||
        strcmp(entries[0].blocks[0].hash, crc) != 0)
        goto cleanup;
    free_entries(entries, count);
    entries = NULL;
    count = 0;
    if (manifest_to_entries(manifest, false, &entries, &count) == 0)
        goto cleanup;
    rc = 0;

cleanup:
    free(job);
    free_entries(entries, count);
    json_decref(manifest);
    return rc;
}

struct frame_capture {
    unsigned int calls;
    uint32_t payload_len;
    unsigned char frame_type;
    size_t json_len;
    char json[256];
};

static int capture_frame(void *host_context,
                         unsigned int server_id,
                         const void *payload,
                         uint32_t payload_len)
{
    struct frame_capture *capture = host_context;
    const unsigned char *frame = payload;
    size_t json_len = payload_len >= 8 ? payload_len - 8u : 0;

    (void)server_id;
    capture->calls++;
    capture->payload_len = payload_len;
    if (payload_len < 8)
        return 0;
    capture->frame_type = frame[5];
    if (json_len >= sizeof(capture->json))
        json_len = sizeof(capture->json) - 1u;
    memcpy(capture->json, frame + 8, json_len);
    capture->json[json_len] = '\0';
    capture->json_len = json_len;
    return 0;
}

static int test_invalid_offer_abort(void)
{
    struct frame_capture capture = {0};
    struct mcp_plugin_host_api host = {0};
    json_t *manifest = make_file_manifest(1);
    json_t *payload = NULL;
    json_t *abort = NULL;
    json_t *abort_id;
    json_t *abort_message;
    json_t *item;
    json_t *block;
    int rc = -1;

    if (!manifest)
        return -1;
    item = json_array_get(json_object_get(manifest, "entries"), 0);
    block = json_array_get(json_object_get(item, "blocks"), 0);
    if (json_object_set_new(block, "hash", json_string("2087b8be")) != 0)
        goto cleanup;
    payload = json_pack("{s:s,s:s,s:O}",
                        "transfer_id",
                        "invalid-manifest-offer",
                        "remote_path",
                        ".",
                        "manifest",
                        manifest);
    if (!payload)
        goto cleanup;

    host.host_context = &capture;
    host.peer_transport_send_frame = capture_frame;
    g_plugin.host = &host;
    handle_offer(17, payload);
    if (capture.calls != 1 || capture.frame_type != MFT_FRAME_ABORT)
        goto cleanup;
    abort = json_loadb(capture.json, capture.json_len, JSON_REJECT_DUPLICATES, NULL);
    if (!abort)
        goto cleanup;
    abort_id = json_object_get(abort, "transfer_id");
    abort_message = json_object_get(abort, "message");
    if (!json_is_string(abort_id) || !json_is_string(abort_message) ||
        strcmp(json_string_value(abort_id), "invalid-manifest-offer") != 0 ||
        strcmp(json_string_value(abort_message), "Invalid file transfer manifest.") != 0)
        goto cleanup;
    rc = 0;

cleanup:
    g_plugin.host = NULL;
    json_decref(abort);
    json_decref(payload);
    json_decref(manifest);
    return rc;
}

static json_t *make_sized_payload(size_t text_len)
{
    char *text = malloc(text_len + 1);
    json_t *payload;

    if (!text)
        return NULL;
    memset(text, 'a', text_len);
    text[text_len] = '\0';
    payload = json_pack("{s:s}", "x", text);
    free(text);
    return payload;
}

static int test_outgoing_frame_bound(void)
{
    struct frame_capture capture = {0};
    struct mcp_plugin_host_api host = {0};
    json_t *payload = make_sized_payload(MFT_MAX_FRAME_SIZE - 16u);
    int rc = -1;

    if (!payload)
        return -1;
    host.host_context = &capture;
    host.peer_transport_send_frame = capture_frame;
    g_plugin.host = &host;
    if (send_json_frame(1, MFT_FRAME_HELLO, payload) != 0 ||
        capture.calls != 1 || capture.payload_len != MFT_MAX_FRAME_SIZE)
        goto cleanup;
    json_decref(payload);
    payload = make_sized_payload(MFT_MAX_FRAME_SIZE - 15u);
    if (!payload || send_json_frame(1, MFT_FRAME_HELLO, payload) == 0 || capture.calls != 1)
        goto cleanup;
    rc = 0;

cleanup:
    g_plugin.host = NULL;
    json_decref(payload);
    return rc;
}

static int test_frame_bounds(void)
{
    unsigned char *max_frame = calloc(1, MFT_MAX_FRAME_SIZE + 1u);
    unsigned char frame[96] = {0};
    struct data_frame_view view;
    const size_t frame_size = 90;

    if (!max_frame)
        return -1;
    memcpy(max_frame, MFT_MAGIC, 4);
    max_frame[4] = MFT_VERSION;
    if (!valid_frame_envelope(max_frame, MFT_MAX_FRAME_SIZE) ||
        valid_frame_envelope(max_frame, MFT_MAX_FRAME_SIZE + 1u)) {
        free(max_frame);
        return -1;
    }
    free(max_frame);

    memcpy(frame, MFT_MAGIC, 4);
    frame[4] = MFT_VERSION;
    frame[5] = MFT_FRAME_DATA;
    write_u16_be(frame + 6, 1);
    write_u32_be(frame + 8, 1);
    write_u64_be(frame + 12, 0);
    write_u32_be(frame + 20, 1);
    memcpy(frame + 24, TEST_HASH, 64);
    frame[88] = 't';
    frame[89] = 'x';
    if (parse_data_frame(frame, frame_size, &view) != 0 ||
        view.data_len != 1 || view.data[0] != 'x' ||
        parse_data_frame(frame, frame_size - 1, &view) == 0 ||
        parse_data_frame(frame, frame_size + 1, &view) == 0)
        return -1;
    write_u32_be(frame + 20, 0);
    if (parse_data_frame(frame, frame_size - 1, &view) == 0)
        return -1;
    write_u32_be(frame + 20, 1);
    write_u64_be(frame + 12, UINT64_MAX);
    if (parse_data_frame(frame, frame_size, &view) == 0)
        return -1;
    return 0;
}

static int test_manifest_count_bounds(void)
{
    json_t *manifest = make_directory_manifest(MFT_MAX_MANIFEST_ENTRIES);

    if (!manifest || manifest_result(manifest, 1, MFT_MAX_MANIFEST_ENTRIES) != 0) {
        json_decref(manifest);
        return -1;
    }
    json_decref(manifest);
    manifest = make_directory_manifest(MFT_MAX_MANIFEST_ENTRIES + 1u);
    if (!manifest || manifest_result(manifest, 0, 0) != 0) {
        json_decref(manifest);
        return -1;
    }
    json_decref(manifest);
    return 0;
}

static int test_manifest_block_bounds(void)
{
    json_t *manifest = make_file_manifest(MFT_MAX_BLOCKS_PER_FILE);

    if (!manifest || manifest_result(manifest, 1, 1) != 0) {
        json_decref(manifest);
        return -1;
    }
    json_decref(manifest);
    manifest = make_file_manifest(MFT_MAX_BLOCKS_PER_FILE + 1u);
    if (!manifest || manifest_result(manifest, 0, 0) != 0) {
        json_decref(manifest);
        return -1;
    }
    json_decref(manifest);
    return 0;
}

static int test_manifest_integer_bounds(void)
{
    json_t *manifest = make_file_manifest(1);
    json_t *array;
    json_t *item;
    json_t *block;

    if (!manifest)
        return -1;
    array = json_object_get(manifest, "entries");
    item = json_array_get(array, 0);
    block = json_array_get(json_object_get(item, "blocks"), 0);
    json_object_set_new(item, "type", json_string("other"));
    if (manifest_result(manifest, 0, 0) != 0)
        goto fail;
    json_object_set_new(item, "type", json_string("file"));
    json_object_set_new(item, "size", json_integer(-1));
    if (manifest_result(manifest, 0, 0) != 0)
        goto fail;
    json_object_set_new(item, "size", json_integer((json_int_t)MFT_LOGICAL_BLOCK_SIZE));
    json_object_set_new(item, "mtime_ns", json_integer(-1));
    if (manifest_result(manifest, 0, 0) != 0)
        goto fail;
    json_object_set_new(item, "mtime_ns", json_integer(0));
    json_object_set_new(item, "mode", json_integer(-1));
    if (manifest_result(manifest, 0, 0) != 0)
        goto fail;
#if UINT_MAX < INT64_MAX
    json_object_set_new(item, "mode", json_integer((json_int_t)UINT_MAX + 1));
    if (manifest_result(manifest, 0, 0) != 0)
        goto fail;
#endif
    json_object_set_new(item, "mode", json_integer(0644));
    json_object_set_new(item, "block_size", json_integer(-1));
    if (manifest_result(manifest, 0, 0) != 0)
        goto fail;
    json_object_set_new(item,
                        "block_size",
                        json_integer((json_int_t)MFT_LOGICAL_BLOCK_SIZE));
    json_object_set_new(block, "offset", json_integer(-1));
    if (manifest_result(manifest, 0, 0) != 0)
        goto fail;
    json_object_set_new(block, "offset", json_integer(0));
    json_object_set_new(block, "size", json_integer(-1));
    if (manifest_result(manifest, 0, 0) != 0)
        goto fail;
    json_object_set_new(block,
                        "size",
                        json_integer((json_int_t)MFT_LOGICAL_BLOCK_SIZE));
    if (manifest_result(manifest, 1, 1) != 0)
        goto fail;
    json_decref(manifest);
    return 0;

fail:
    json_decref(manifest);
    return -1;
}

static int test_manifest_layout_bounds(void)
{
    json_t *manifest = make_file_manifest(2);
    json_t *item;
    json_t *blocks;
    json_t *first;
    json_t *second;

    if (!manifest)
        return -1;
    item = json_array_get(json_object_get(manifest, "entries"), 0);
    blocks = json_object_get(item, "blocks");
    first = json_array_get(blocks, 0);
    second = json_array_get(blocks, 1);
    json_object_set_new(second,
                        "offset",
                        json_integer((json_int_t)MFT_LOGICAL_BLOCK_SIZE + 1));
    if (manifest_result(manifest, 0, 0) != 0)
        goto fail;
    json_object_set_new(second,
                        "offset",
                        json_integer((json_int_t)MFT_LOGICAL_BLOCK_SIZE));
    json_object_set_new(first,
                        "size",
                        json_integer((json_int_t)MFT_LOGICAL_BLOCK_SIZE - 1));
    if (manifest_result(manifest, 0, 0) != 0)
        goto fail;
    json_object_set_new(first,
                        "size",
                        json_integer((json_int_t)MFT_LOGICAL_BLOCK_SIZE));
    json_object_set_new(item,
                        "size",
                        json_integer((json_int_t)(2ull * MFT_LOGICAL_BLOCK_SIZE - 1)));
    json_object_set_new(second,
                        "size",
                        json_integer((json_int_t)MFT_LOGICAL_BLOCK_SIZE - 1));
    if (manifest_result(manifest, 1, 1) != 0)
        goto fail;
    json_decref(manifest);
    return 0;

fail:
    json_decref(manifest);
    return -1;
}

static int test_manifest_total_bound(void)
{
    json_t *array = json_array();
    json_t *first = json_pack("{s:s,s:I}",
                              "type",
                              "file",
                              "size",
                              (json_int_t)INT64_MAX);
    json_t *second = json_pack("{s:s,s:I}",
                               "type",
                               "file",
                               "size",
                               (json_int_t)1);

    if (!array || !first || !second) {
        json_decref(first);
        json_decref(second);
        json_decref(array);
        return -1;
    }
    if (json_array_append_new(array, first) != 0) {
        json_decref(second);
        json_decref(array);
        return -1;
    }
    if (validate_manifest_bounds(array) != 0) {
        json_decref(second);
        json_decref(array);
        return -1;
    }
    if (json_array_append_new(array, second) != 0) {
        json_decref(array);
        return -1;
    }
    if (validate_manifest_bounds(array) == 0) {
        json_decref(array);
        return -1;
    }
    json_decref(array);
    return 0;
}

static int test_accept_bounds(void)
{
    struct block_entry block = {0};
    struct manifest_entry entry = {0};
    json_t *accept = json_array();
    json_t *item;

    block.size = 4;
    entry.type = 'f';
    entry.size = 4;
    entry.blocks = &block;
    entry.block_count = 1;
    item = json_pack("{s:s,s:I}", "decision", "receive", "resume_offset", (json_int_t)-1);
    if (!accept || !item) {
        json_decref(item);
        json_decref(accept);
        return -1;
    }
    if (json_array_append_new(accept, item) != 0) {
        json_decref(accept);
        return -1;
    }
    if (validate_accept(accept, &entry, 1) == 0) {
        json_decref(accept);
        return -1;
    }
    json_object_set_new(json_array_get(accept, 0), "resume_offset", json_integer(3));
    if (validate_accept(accept, &entry, 1) == 0) {
        json_decref(accept);
        return -1;
    }
    json_object_set_new(json_array_get(accept, 0), "resume_offset", json_integer(4));
    if (validate_accept(accept, &entry, 1) != 0) {
        json_decref(accept);
        return -1;
    }
    json_decref(accept);
    return 0;
}

int main(void)
{
    if (test_outgoing_frame_bound() != 0 ||
        test_invalid_offer_abort() != 0 ||
        test_crc32_contract() != 0 ||
        test_frame_bounds() != 0 ||
        test_manifest_count_bounds() != 0 ||
        test_manifest_block_bounds() != 0 ||
        test_manifest_integer_bounds() != 0 ||
        test_manifest_layout_bounds() != 0 ||
        test_manifest_total_bound() != 0 ||
        test_accept_bounds() != 0)
        return 1;
    return 0;
}
