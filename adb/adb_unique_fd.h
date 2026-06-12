#pragma once

#include <errno.h>
#include <unistd.h>

#include <android-base/unique_fd.h>

using unique_fd = android::base::unique_fd;
using android::base::borrowed_fd;

template <typename T>
int adb_close(const android::base::unique_fd_impl<T>&)
        __attribute__((__unavailable__("adb_close called on unique_fd")));
