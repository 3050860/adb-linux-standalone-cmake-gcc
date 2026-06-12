#pragma once

#include <sys/types.h>

#include <algorithm>

#include "adb_unique_fd.h"

#include <sys/uio.h>
using adb_iovec = struct iovec;
inline ssize_t adb_writev(borrowed_fd fd, const adb_iovec* iov, int iovcnt) {
    return writev(fd.get(), iov, std::min(iovcnt, IOV_MAX));
}

#pragma GCC poison writev
