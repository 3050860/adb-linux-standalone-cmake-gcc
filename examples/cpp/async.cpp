// examples/cpp/async.cpp — асинхронный push на несколько устройств одновременно.
// Запуск: ./ex_async /tmp/file.bin /data/local/tmp/file.bin 192.168.1.10 192.168.1.11
#include <cstdio>
#include <string>
#include <vector>
#include "libadb/libadb.h"

int main(int argc, char** argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: %s <local> <remote> <host1> [host2 ...]\n", argv[0]);
        return 1;
    }
    const std::string local  = argv[1];
    const std::string remote = argv[2];
    std::vector<std::string> addresses(argv + 3, argv + argc);

    auto& client = libadb::Client::instance();
    libadb::Options opts;
    opts.async_worker_threads = static_cast<size_t>(addresses.size());
    client.initialize(opts);

    // Асинхронный push сразу на все адреса.
    auto batch = client.push_all_async(addresses, local, remote);
    printf("started %zu operations\n", batch->total());

    // Ждём завершения всего батча (60 секунд).
    if (!batch->wait(libadb::ms{60000})) {
        fprintf(stderr, "timeout waiting for batch\n");
        batch->cancel();
    }

    int failures = 0;
    for (const auto& [addr, result] : batch->results()) {
        if (result.ok()) {
            printf("  %-24s OK  %.1f MiB/s\n", addr.c_str(), result.transfer.mib_per_sec);
        } else {
            printf("  %-24s ERR %s\n", addr.c_str(), result.error.c_str());
            ++failures;
        }
    }
    return failures == 0 ? 0 : 1;
}
