// examples/cpp/push.cpp — минимальный push одного файла на устройство.
// Сборка: cmake -S examples -B build-ex && cmake --build build-ex
// Запуск: ./ex_push 192.168.1.10 /tmp/data.bin /data/local/tmp/data.bin
#include <cstdio>
#include "libadb/libadb.h"

int main(int argc, char** argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <host:port> <local> <remote>\n", argv[0]);
        return 1;
    }

    auto& client = libadb::Client::instance();

    // Минимальная инициализация: ключ из ~/.android/adbkey, таймауты по умолчанию.
    if (client.initialize() != libadb::Status::Ok) {
        fprintf(stderr, "initialize failed\n");
        return 1;
    }

    libadb::Status status;
    auto device = client.connect(argv[1], &status);
    if (!device) {
        fprintf(stderr, "connect: %s\n", libadb::to_string(status));
        return 1;
    }

    libadb::PushOptions opts;
    opts.on_progress = [](const std::string&, uint64_t done, uint64_t total) {
        if (total > 0)
            printf("\r  %3u%%", static_cast<unsigned>(done * 100 / total));
    };

    auto result = device->push(argv[2], argv[3], opts);
    printf("\n");
    if (!result.ok()) {
        fprintf(stderr, "push failed: %s\n", result.error.c_str());
        return 1;
    }
    printf("pushed %llu bytes in %.1f s (%.1f MiB/s)\n",
           (unsigned long long)result.transfer.bytes,
           result.transfer.duration.count() / 1000.0,
           result.transfer.mib_per_sec);
    return 0;
}
