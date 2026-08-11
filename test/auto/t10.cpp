// Находим пакет, соответствующий test/main.apk (по размеру apk на устройстве),
// затем проверяем полный цикл uninstall -> install.
#include <sys/stat.h>

#include <cstdio>
#include <set>
#include <sstream>
#include <string>

#include "libadb/libadb.h"

static std::set<std::string> packages(libadb::Device& device) {
    std::set<std::string> result;
    std::istringstream stream(device.shell("pm list packages -3").output);
    std::string line;
    while (std::getline(stream, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.rfind("package:", 0) == 0) result.insert(line.substr(8));
    }
    return result;
}

int main() {
    const char* apk = "test/main.apk";
    struct stat st{};
    stat(apk, &st);
    const long local_size = st.st_size;
    printf("local apk size=%ld\n", local_size);

    auto& client = libadb::Client::instance();
    libadb::Options options;
    options.log.level = libadb::LogLevel::Warn;
    client.initialize(options);
    auto device = client.connect("192.168.177.248");
    if (!device) {
        printf("connect failed\n");
        return 1;
    }

    // Ищем пакет с таким же размером apk — это наш main.apk.
    std::string target;
    for (const auto& package : packages(*device)) {
        auto r = device->shell("for p in $(pm path " + package +
                               " | sed 's/package://'); do stat -c %s $p; done");
        std::string sizes = r.output;
        printf("  %s sizes: %s", package.c_str(), sizes.c_str());
        if (sizes.find(std::to_string(local_size)) != std::string::npos) target = package;
    }
    printf("matched package: '%s'\n", target.c_str());
    if (target.empty()) {
        printf("не нашли — прекращаю, чтобы не удалить чужой пакет\n");
        client.shutdown();
        return 0;
    }

    auto r = device->uninstall(target);
    printf("uninstall: status=%s ms=%ld output='%s'\n", libadb::to_string(r.status),
           (long)r.duration.count(), r.output.c_str());
    printf("present after uninstall: %d\n", (int)packages(*device).count(target));

    r = device->install(apk);
    printf("install back: status=%s ms=%ld error='%s'\n", libadb::to_string(r.status),
           (long)r.duration.count(), r.error.c_str());
    printf("present after install: %d\n", (int)packages(*device).count(target));

    client.shutdown();
    return 0;
}
