#define TRACE_TAG ADB

#include "sysdeps.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <thread>
#include <iostream>

#include <android-base/errors.h>
#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/stringprintf.h>

#include "adb.h"
#include "adb_auth.h"
#include "adb_client.h"
#include "adb_listeners.h"
#include "adb_utils.h"
#include "commandline.h"
#include "sysdeps/chrono.h"
#include "transport.h"
int main(int argc, char* argv[], char* envp[]) {
    __adb_argv = const_cast<const char**>(argv);
    __adb_envp = const_cast<const char**>(envp);

    adb_trace_init(argv);
    
    return 0;
}