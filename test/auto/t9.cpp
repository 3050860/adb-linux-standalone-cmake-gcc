// Полный цикл: install -> найти пакет по диффу -> uninstall -> проверить.
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
    auto& client = libadb::Client::instance();
    libadb::Options options;
    options.log.level = libadb::LogLevel::Warn;
    client.initialize(options);

    auto device = client.connect("192.168.177.248");
    if (!device) {
        printf("connect failed\n");
        return 1;
    }

    // 1) удаление несуществующего пакета должно быть ошибкой, а не Ok
    auto r = device->uninstall("com.libadb.definitely.missing");
    printf("uninstall(missing): status=%s ms=%ld code='%s' error='%s'\n",
           libadb::to_string(r.status), (long)r.duration.count(), r.remote_code.c_str(),
           r.error.c_str());

    const auto before = packages(*device);

    r = device->install("test/main.apk");
    printf("install: status=%s ms=%ld code='%s'\n", libadb::to_string(r.status),
           (long)r.duration.count(), r.remote_code.c_str());

    const auto after = packages(*device);
    std::string installed;
    for (const auto& package : after) {
        if (!before.count(package)) installed = package;
    }
    printf("newly installed package: '%s'\n", installed.c_str());

    if (!installed.empty()) {
        r = device->uninstall(installed);
        printf("uninstall(%s): status=%s ms=%ld output='%s'\n", installed.c_str(),
               libadb::to_string(r.status), (long)r.duration.count(), r.output.c_str());
        printf("package still present: %d\n", (int)packages(*device).count(installed));
    } else {
        printf("apk уже был установлен ранее — сравниваю списки: before=%zu after=%zu\n",
               before.size(), after.size());
    }

    client.shutdown();
    return 0;
}
