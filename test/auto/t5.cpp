// Проверка: initialized(), рестарт после shutdown(), max_parallel, close_all().
#include <cstdio>
#include <chrono>
#include <string>
#include <vector>

#include "libadb/libadb.h"

static long now_ms() {
    static auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                t0)
        .count();
}

int main() {
    std::vector<std::string> addrs = {"192.168.177.248", "192.168.177.249"};
    auto& client = libadb::Client::instance();

    printf("before init: initialized=%d\n", (int)client.initialized());

    libadb::Options options;
    options.timeouts.connect = libadb::ms{8000};
    options.max_parallel = 1;  // строго по одному устройству за раз
    libadb::Status s = client.initialize(options);
    printf("initialize=%s then initialized=%d, default_port=%u, max_parallel=%zu\n",
           libadb::to_string(s), (int)client.initialized(), client.options().default_port,
           client.options().max_parallel);

    printf("for_each with max_parallel=1 (ожидаем строгую последовательность):\n");
    client.for_each(addrs, [](const libadb::DevicePtr& device, const std::string& address,
                              libadb::Status status) {
        printf("  [%ld ms] start %s status=%s\n", now_ms(), address.c_str(),
               libadb::to_string(status));
        if (device) device->shell("sleep 1");
        printf("  [%ld ms] end   %s\n", now_ms(), address.c_str());
    });

    // close_all(): устройство, полученное через connect(), закрывается клиентом.
    auto device = client.connect(addrs[0]);
    printf("connect -> online=%d\n", device ? (int)device->is_online() : -1);
    client.close_all();
    printf("after close_all -> online=%d\n", device ? (int)device->is_online() : -1);

    client.shutdown();
    printf("after shutdown: initialized=%d\n", (int)client.initialized());

    // Рестарт: клиент должен снова заработать.
    client.initialize(options);
    printf("re-initialized=%d\n", (int)client.initialized());
    auto again = client.connect(addrs[0]);
    printf("connect after restart: %s\n",
           again ? again->shell("echo restart-ok").output.c_str() : "<failed>");
    client.shutdown();
    return 0;
}
