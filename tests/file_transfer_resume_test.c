#define MCP_FILE_TRANSFER_PLUGIN_BUILTIN 1
#include "../src/plugins/file_transfer/file_transfer_plugin.c"

#include <stdio.h>

static int write_test_data(FILE *fp, const unsigned char *data, size_t size)
{
    clearerr(fp);
    if (truncate_stream(fp, 0) != 0 || mft_seek_stream(fp, 0) != 0)
        return -1;
    if (fwrite(data, 1, size, fp) != size || fflush(fp) != 0)
        return -1;
    return 0;
}

static int test_file_size(FILE *fp, uint64_t *size)
{
#ifdef _WIN32
    struct _stat64 st;

    if (_fstat64(_fileno(fp), &st) != 0 || st.st_size < 0)
        return -1;
#else
    struct stat st;

    if (fstat(fileno(fp), &st) != 0 || st.st_size < 0)
        return -1;
#endif
    *size = (uint64_t)st.st_size;
    return 0;
}

static int check_resume(FILE *fp,
                        const unsigned char *data,
                        size_t data_size,
                        const struct manifest_entry *entry,
                        uint64_t expected_resume)
{
    uint64_t resume_offset = UINT64_MAX;
    uint64_t actual_size = UINT64_MAX;

    if (write_test_data(fp, data, data_size) != 0 ||
        validated_resume_stream(fp, data_size, entry, &resume_offset) != 0 ||
        test_file_size(fp, &actual_size) != 0)
        return -1;
    return resume_offset == expected_resume && actual_size == expected_resume ? 0 : -1;
}

int main(void)
{
    static const unsigned char expected[] = "abcdefghijkl";
    static const unsigned char bad_second[] = "abcdXXXXij";
    static const unsigned char partial_third[] = "abcdefghij";
    struct block_entry blocks[3] = {0};
    struct manifest_entry entry = {0};
    FILE *fp;
    size_t i;
    int rc = 1;

    entry.type = 'f';
    entry.size = sizeof(expected) - 1;
    entry.blocks = blocks;
    entry.block_count = 3;
    for (i = 0; i < entry.block_count; i++) {
        blocks[i].offset = i * 4;
        blocks[i].size = 4;
        bytes_sha256(expected + blocks[i].offset, 4, blocks[i].hash);
    }

    fp = tmpfile();
    if (!fp)
        return 1;
    if (check_resume(fp,
                     bad_second,
                     sizeof(bad_second) - 1,
                     &entry,
                     4) != 0 ||
        check_resume(fp,
                     partial_third,
                     sizeof(partial_third) - 1,
                     &entry,
                     8) != 0 ||
        check_resume(fp,
                     expected,
                     sizeof(expected) - 1,
                     &entry,
                     sizeof(expected) - 1) != 0)
        goto cleanup;
    entry.block_crc32 = true;
    for (i = 0; i < entry.block_count; i++)
        bytes_crc32(expected + blocks[i].offset, 4, blocks[i].hash);
    if (check_resume(fp,
                     bad_second,
                     sizeof(bad_second) - 1,
                     &entry,
                     4) != 0 ||
        check_resume(fp,
                     partial_third,
                     sizeof(partial_third) - 1,
                     &entry,
                     8) != 0 ||
        check_resume(fp,
                     expected,
                     sizeof(expected) - 1,
                     &entry,
                     sizeof(expected) - 1) != 0)
        goto cleanup;
    rc = 0;

cleanup:
    fclose(fp);
    return rc;
}
