// Проверка install/uninstall с реальным apk + групповой install_all.
#include <cstdio>
#include <string>
#include <vector>

#include "libadb/libadb.h"

int main(int argc, char** argv) {
    const char* apk = argc > 1 ? argv[1] : "test/main.apk";
    std::vector<std::string> addrs = {"192.168.177.248", "192.168.177.249"};

    auto& client = libadb::Client::instance();
    libadb::Options options;
    options.connect_timeout = libadb::ms{8000};
    client.initialize(options);

    auto device = client.connect(addrs[0]);
    if (!device) {
        printf("connect failed\n");
        return 1;
    }

    // Имя пакета до установки не знаем: возьмём из вывода pm после установки.
    auto r = device->install(apk);
    printf("install: status=%s ms=%ld remote_code='%s' error='%s'\n", libadb::to_string(r.status),
           (long)r.duration.count(), r.remote_code.c_str(), r.error.c_str());

    // Повторная установка того же apk без -r (ожидаем ALREADY_EXISTS либо Ok при -r по умолчанию)
    libadb::InstallOptions install_options;
    install_options.reinstall = false;
    r = device->install(apk, install_options);
    printf("install(no -r): status=%s remote_code='%s' error='%s'\n", libadb::to_string(r.status),
           r.remote_code.c_str(), r.error.c_str());

    // Ищем свежеустановленный пакет
    auto pkgs = device->shell("pm list packages -3");
    printf("third-party packages:\n%s", pkgs.output.c_str());

    std::string package;
    size_t pos = pkgs.output.find("package:");
    if (pos != std::string::npos) {
        size_t end = pkgs.output.find('\n', pos);
        package = pkgs.output.substr(pos + 8, end - pos - 8);
        while (!package.empty() && (package.back() == '\r' || package.back() == ' '))
            package.pop_back();
    }
    printf("detected package='%s'\n", package.c_str());

    if (!package.empty()) {
        r = device->uninstall(package);
        printf("uninstall(%s): status=%s ms=%ld error='%s'\n", package.c_str(),
               libadb::to_string(r.status), (long)r.duration.count(), r.error.c_str());
    }

    // Групповая установка на все устройства
    printf("install_all:\n");
    for (const auto& [address, result] : client.install_all(addrs, apk)) {
        printf("  %s -> status=%s ms=%ld error='%s'\n", address.c_str(),
               libadb::to_string(result.status), (long)result.duration.count(),
               result.error.c_str());
    }
    if (!package.empty()) {
        for (const auto& [address, result] : client.uninstall_all(addrs, package)) {
            printf("  uninstall %s -> status=%s\n", address.c_str(),
                   libadb::to_string(result.status));
        }
    }

    client.shutdown();
    return 0;
}
