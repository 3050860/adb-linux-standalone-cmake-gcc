#ifndef __ADB_TRACE_H
#define __ADB_TRACE_H

#include <android-base/logging.h>
#include <android-base/stringprintf.h>

/* IMPORTANT: if you change the following list, don't
 * forget to update the corresponding 'tags' table in
 * the adb_trace_init() function implemented in adb_trace.cpp.
 */
enum AdbTrace {
    ADB = 0, /* 0x001 */
    SOCKETS,
    PACKETS,
    TRANSPORT,
    RWX, /* 0x010 */
    USB,
    SYNC,
    SYSDEPS,
    JDWP, /* 0x100 */
    SERVICES,
    AUTH,
    FDEVENT,
    SHELL,
    INCREMENTAL,
};

#define VLOG_IS_ON(TAG) \
    (true)

#define VLOG(TAG)                 \
        LOG(DEBUG)

// You must define TRACE_TAG before using this macro.
#define D(...) \
    VLOG(ADB) << android::base::StringPrintf(__VA_ARGS__)


extern int adb_trace_mask;
void adb_trace_init(char**);
void adb_trace_enable(AdbTrace trace_tag);

#endif /* __ADB_TRACE_H */
