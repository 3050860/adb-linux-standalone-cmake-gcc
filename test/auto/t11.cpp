// t11: локализация проблемы «после срыва shell по таймауту следующая команда
// тоже срывается» (этап 8). Запуск: /tmp/t11 <ip>
//
// Сборка:
//   g++ -std=c++20 test/auto/t11.cpp -Iinclude -Ibuild/include
//       -Lbuild -ladb -Wl,-rpath,$PWD/build -pthread -o /tmp/t11
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "libadb/libadb.h"

int main(int argc, char** argv) {
    const std::string address = argc > 1 ? argv[1] : "192.168.177.249";

    libadb::Options options;
    options.timeouts.shell = libadb::ms{2000};
    libadb::Client::instance().initialize(options);

    libadb::Status status = libadb::Status::Ok;
    auto device = libadb::Client::instance().connect(address, &status);
    if (!device) {
        printf("connect failed: %s\n", libadb::to_string(status));
        return 1;
    }

    libadb::Result hung = device->shell("sleep 30", libadb::ShellOptions{});
    printf("1) sleep 30 -> %s\n", libadb::to_string(hung.status));

    for (int i = 0; i < 5; ++i) {
        libadb::Result r = device->shell("echo alive", libadb::ShellOptions{});
        printf("2.%d) echo alive -> %s exit=%d out='%s'\n", i, libadb::to_string(r.status),
               r.exit_code, r.output.c_str());
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    device->close();
    libadb::Client::instance().shutdown();
    return 0;
}
