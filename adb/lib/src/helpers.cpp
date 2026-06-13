#define TRACE_TAG ADB

#include "sysdeps.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <iostream>

#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>

#include <sys/ioctl.h>
#include <termios.h>

// #include <google/protobuf/text_format.h>

// #include "adb.h"
// #include "adb_auth.h"
// #include "adb_client.h"
// #include "adb_install.h"
// #include "adb_io.h"
// #include "adb_unique_fd.h"
// #include "adb_utils.h"
// #include "app_processes.pb.h"
// #include "bugreport.h"
// #include "client/file_sync_client.h"
// #include "commandline.h"
// #include "incremental_server.h"
// #include "services.h"
// #include "shell_protocol.h"
// #include "socket_spec.h"
// #include "sysdeps/chrono.h"

#include "helpers.h"

DefaultStandardStreamsCallback DEFAULT_STANDARD_STREAMS_CALLBACK(nullptr, nullptr);

int send_shell_command(const std::string& command, bool disable_shell_protocol,
                       StandardStreamsCallbackInterface* callback) {
    unique_fd fd;
    bool use_shell_protocol = false;

    LOG(INFO) << "send_shell_command: " << command;
    // while (true) {
    //     bool attempt_connection = true;

    //     // Use shell protocol if it's supported and the caller doesn't explicitly
    //     // disable it.
    //     if (!disable_shell_protocol) {
    //         auto&& features = adb_get_feature_set(nullptr);
    //         if (features) {
    //             use_shell_protocol = CanUseFeature(*features, kFeatureShell2);
    //         } else {
    //             // Device was unreachable.
    //             attempt_connection = false;
    //         }
    //     }

    //     if (attempt_connection) {
    //         std::string error;
    //         std::string service_string = ShellServiceString(use_shell_protocol, "", command);

    //         fd.reset(adb_connect(service_string, &error));
    //         if (fd >= 0) {
    //             break;
    //         }
    //     }

    //     fprintf(stderr, "- waiting for device -\n");
    //     if (!wait_for_device("wait-for-device")) {
    //         return 1;
    //     }
    // }

    // return read_and_dump(fd.get(), use_shell_protocol, callback);
    return 0;
}