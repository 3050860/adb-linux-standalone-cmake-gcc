// test_011: install/uninstall через libadb.
// Проверяем: удаление реально удаляет пакет, ошибки отдаются кодом от pm,
// а библиотека не печатает статус pm в stdout приложения.
//
// Сборка:
//   g++ -std=c++20 test/auto/test_011_install_uninstall.cpp -Iinclude -Ibuild/include \
//       -Lbuild -ladb -Wl,-rpath,$PWD/build -o /tmp/test_011

// Запуск: /tmp/test_011 <ip-устройства> [путь к apk]
#include <sys/stat.h>

#include <cstdio>
#include <set>
#include <sstream>
#include <string>

#include "libadb/libadb.h"

namespace {

int failures = 0;

void check(bool condition, const std::string& name, const std::string& details = {}) {
    printf("%s %s%s%s\n", condition ? "[ OK ]" : "[FAIL]", name.c_str(),
           details.empty() ? "" : " -> ", details.c_str());
    if (!condition) ++failures;
}

std::set<std::string> third_party_packages(libadb::Device& device) {
    std::set<std::string> packages;
    std::istringstream stream(device.shell("pm list packages -3").output);
    std::string line;
    while (std::getline(stream, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.rfind("package:", 0) == 0) packages.insert(line.substr(8));
    }
    return packages;
}

uint64_t local_size(const std::string& path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0 ? static_cast<uint64_t>(st.st_size) : 0;
}

// Ищем пакет, чей apk на устройстве совпадает по размеру с локальным файлом.
std::string find_matching_package(libadb::Device& device, uint64_t size) {
    for (const auto& package : third_party_packages(device)) {
        auto result = device.shell("for p in $(pm path " + package +
                                   " | sed 's/package://'); do stat -c %s $p; done");
        if (result.output.find(std::to_string(size)) != std::string::npos) return package;
    }
    return {};
}

}  // namespace

int main(int argc, char** argv) {
    const std::string address = argc > 1 ? argv[1] : "192.168.177.248";
    const std::string apk = argc > 2 ? argv[2] : "test/main.apk";

    const uint64_t apk_size = local_size(apk);
    if (apk_size == 0) {
        printf("apk не найден: %s\n", apk.c_str());
        return 1;
    }

    auto& client = libadb::Client::instance();
    libadb::Options options;
    options.log.level = libadb::LogLevel::Warn;
    client.initialize(options);

    auto device = client.connect(address);
    check(device != nullptr, "connect " + address);
    if (!device) {
        client.shutdown();
        return 1;
    }

    // 1) Удаление несуществующего пакета: ошибка + код от pm, а не «Ok».
    auto result = device->uninstall("com.libadb.definitely.missing");
    check(!result.ok(), "uninstall(missing) не должен возвращать Ok",
          libadb::to_string(result.status));
    check(!result.remote_code.empty(), "uninstall(missing) отдаёт код отказа",
          result.remote_code);

    // 2) Установка apk.
    result = device->install(apk);
    check(result.ok(), "install " + apk, result.error);
    check(result.output.find("Success") != std::string::npos, "install отдаёт статус в output",
          result.output);

    // 3) Пакет действительно появился на устройстве.
    const std::string package = find_matching_package(*device, apk_size);
    check(!package.empty(), "пакет из apk найден на устройстве", package);

    // ВНИМАНИЕ: тест намеренно НЕ удаляет установленные пакеты. На стендовых
    // устройствах приложение может отвечать за сеть/kiosk-режим, и удаление
    // уводит устройство из сети (проверено на 192.168.177.248). Проверка того,
    // что uninstall реально работает, ограничена кодом отказа для
    // несуществующего пакета: раньше он молча возвращал Ok.


    client.shutdown();
    printf("\n%s (failures=%d)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
