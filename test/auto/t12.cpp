// t12: локализация падения при отмене push (этап 8).
// Запуск: /tmp/t12 <ip>
//
// Сборка:
//   g++ -std=c++20 test/auto/t12.cpp -Iinclude -Ibuild/include
//       -Lbuild -ladb -Wl,-rpath,$PWD/build -pthread -o /tmp/t12
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "libadb/libadb.h"

int main(int argc, char** argv) {
    const std::string address = argc > 1 ? argv[1] : "192.168.177.249";
    const std::string path = "/tmp/libadb-t12.bin";

    {
        FILE* f = fopen(path.c_str(), "wb");
        std::vector<char> block(64 * 1024, 'z');
        for (int i = 0; i < 512; ++i) fwrite(block.data(), 1, block.size(), f);  // 32 MiB
        fclose(f);
    }

    libadb::Options options;
    libadb::Client::instance().initialize(options);

    libadb::Status status = libadb::Status::Ok;
    auto device = libadb::Client::instance().connect(address, &status);
    if (!device) {
        printf("connect failed: %s\n", libadb::to_string(status));
        return 1;
    }

    // Сначала shell (как в test_016): его сессия остаётся в списке устройства и
    // будет прервана при создании sync-сессии.
    libadb::Result pre = device->shell("echo before-push", libadb::ShellOptions{});
    printf("pre-shell -> %s '%s'\n", libadb::to_string(pre.status), pre.output.c_str());
    fflush(stdout);

    libadb::PushOptions po;
    po.compression = libadb::Compression::None;

    printf("submitting push_async\n");
    fflush(stdout);
    auto op = device->push_async(path, "/data/local/tmp/libadb-t12.bin", po);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    printf("cancelling\n");
    fflush(stdout);
    op->cancel();
    const bool waited = op->wait(libadb::ms{15000});
    printf("waited=%d status=%s bytes=%lu\n", waited ? 1 : 0,
           libadb::to_string(op->result().status),
           static_cast<unsigned long>(op->result().transfer.bytes));
    fflush(stdout);

    printf("online=%d\n", device->is_online() ? 1 : 0);
    libadb::Result r = device->shell("echo after-cancel", libadb::ShellOptions{});
    printf("shell -> %s '%s'\n", libadb::to_string(r.status), r.output.c_str());

    device->close();
    libadb::Client::instance().shutdown();
    return 0;
}
