#include <stdlib.h>

#include "sysdeps/env.h"

#include <android-base/utf8.h>

namespace adb {
namespace sysdeps {

std::optional<std::string> GetEnvironmentVariable(std::string_view var) {
    if (var.empty()) {
        return std::nullopt;
    }
    const char* val = getenv(var.data());
    if (val == nullptr) {
        return std::nullopt;
    }

    return std::make_optional(std::string(val));
}

constexpr char kHostNameEnvVar[] = "HOSTNAME";
constexpr char kUserNameEnvVar[] = "LOGNAME";


std::string GetHostNameUTF8() {
    const auto hostName = GetEnvironmentVariable(kHostNameEnvVar);
    if (hostName && !hostName->empty()) {
        return *hostName;
    }
    char buf[256];
    return (gethostname(buf, sizeof(buf)) == -1) ? "" : buf;
}

std::string GetLoginNameUTF8() {
    const auto userName = GetEnvironmentVariable(kUserNameEnvVar);
    if (userName && !userName->empty()) {
        return *userName;
    }
    const char* login = getlogin();
    return login ? login : "";
}

}  // namespace sysdeps
}  // namespace adb
