#pragma once

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// Make sure that host file mode values match the ones on the device.
static_assert(S_IFMT == 00170000, "");
static_assert(S_IFLNK == 0120000, "");
static_assert(S_IFREG == 0100000, "");
static_assert(S_IFBLK == 0060000, "");
static_assert(S_IFDIR == 0040000, "");
static_assert(S_IFCHR == 0020000, "");
