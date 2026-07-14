#ifndef MCP_FILE_TRANSFER_FILE_OFFSET_H
#define MCP_FILE_TRANSFER_FILE_OFFSET_H

#ifndef _WIN32
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#endif

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

#ifdef _WIN32
#include <io.h>
typedef int64_t mft_file_offset_t;
#else
#include <unistd.h>
typedef off_t mft_file_offset_t;
#endif

_Static_assert(sizeof(mft_file_offset_t) >= sizeof(int64_t),
               "file transfer requires 64-bit file offsets");

static int mft_file_offset_from_u64(uint64_t offset, mft_file_offset_t *native_offset)
{
    if (offset > (uint64_t)INT64_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    *native_offset = (mft_file_offset_t)offset;
    if (*native_offset < 0 || (uint64_t)*native_offset != offset) {
        errno = EOVERFLOW;
        return -1;
    }
    return 0;
}

static int mft_seek_stream(FILE *stream, uint64_t offset)
{
    mft_file_offset_t native_offset;

    if (mft_file_offset_from_u64(offset, &native_offset) != 0)
        return -1;
#ifdef _WIN32
    return _fseeki64(stream, native_offset, SEEK_SET);
#else
    return fseeko(stream, native_offset, SEEK_SET);
#endif
}

static int mft_seek_fd(int fd, uint64_t offset)
{
    mft_file_offset_t native_offset;

    if (mft_file_offset_from_u64(offset, &native_offset) != 0)
        return -1;
#ifdef _WIN32
    return _lseeki64(fd, native_offset, SEEK_SET) < 0 ? -1 : 0;
#else
    return lseek(fd, native_offset, SEEK_SET) < 0 ? -1 : 0;
#endif
}

#endif
