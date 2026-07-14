#include "file_offset.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>

#ifdef _WIN32
#define test_fileno _fileno
#define test_ftell _ftelli64
#define test_lseek _lseeki64
#else
#define test_fileno fileno
#define test_ftell ftello
#define test_lseek lseek
#endif

static int check_stream_seek(FILE *fp, uint64_t offset)
{
    mft_file_offset_t actual;

    if (mft_seek_stream(fp, offset) != 0)
        return -1;
    actual = (mft_file_offset_t)test_ftell(fp);
    return actual >= 0 && (uint64_t)actual == offset ? 0 : -1;
}

static int check_fd_seek(FILE *fp, uint64_t offset)
{
    mft_file_offset_t actual;
    int fd = test_fileno(fp);

    if (fd < 0 || mft_seek_fd(fd, offset) != 0)
        return -1;
    actual = (mft_file_offset_t)test_lseek(fd, 0, SEEK_CUR);
    return actual >= 0 && (uint64_t)actual == offset ? 0 : -1;
}

int main(void)
{
    const uint64_t large_offset = UINT64_C(2) * 1024 * 1024 * 1024 + 17;
    FILE *fp = tmpfile();

    if (!fp) {
        perror("tmpfile");
        return 1;
    }
    if (check_stream_seek(fp, large_offset) != 0) {
        fprintf(stderr, "stream seek truncated a >2 GiB offset\n");
        fclose(fp);
        return 1;
    }
    if (check_fd_seek(fp, large_offset + 1) != 0) {
        fprintf(stderr, "descriptor seek truncated a >2 GiB offset\n");
        fclose(fp);
        return 1;
    }

    errno = 0;
    if (mft_seek_stream(fp, UINT64_MAX) == 0 || errno != EOVERFLOW) {
        fprintf(stderr, "unrepresentable stream offset was not rejected\n");
        fclose(fp);
        return 1;
    }
    errno = 0;
    if (mft_seek_fd(test_fileno(fp), UINT64_MAX) == 0 || errno != EOVERFLOW) {
        fprintf(stderr, "unrepresentable descriptor offset was not rejected\n");
        fclose(fp);
        return 1;
    }

    fclose(fp);
    return 0;
}
