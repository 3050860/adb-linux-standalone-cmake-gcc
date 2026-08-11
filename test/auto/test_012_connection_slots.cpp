// test_012: слоты подключений (§7 спецификации).
// Проверяем: лимит соблюдается, слот освобождается при close() и при разрушении
// Device, slot_acquire=0 даёт SlotBusy, конечный таймаут — SlotTimeout,
// ожидающий поток получает слот после освобождения, и батч не превышает лимит.
//
// Сборка:
//   g++ -std=c++20 test/auto/test_012_connection_slots.cpp -Iinclude -Ibuild/include \
//       -Lbuild -ladb -Wl,-rpath,$PWD/build -pthread -o /tmp/test_012
//
// Запуск: /tmp/test_012 <ip-1> [ip-2]
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "libadb/libadb.h"

namespace {

int failures = 0;

void check(bool condition, const std::string& name, const std::string& details = {}) {
    printf("%s %s%s%s\n", condition ? "[ OK ]" : "[FAIL]", name.c_str(),
           details.empty() ? "" : " -> ", details.c_str());
    if (!condition) ++failures;
}

using clock_type = std::chrono::steady_clock;

int64_t elapsed_ms(clock_type::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(clock_type::now() - start).count();
}

}  // namespace

int main(int argc, char** argv) {
    const std::string first = argc > 1 ? argv[1] : "192.168.177.249";
    const std::string second = argc > 2 ? argv[2] : first;

    auto& client = libadb::Client::instance();

    libadb::Options options;
    options.max_connections = 1;   // один слот на весь процесс
    options.timeouts.slot_acquire = libadb::ms{0};  // не ждать: сразу SlotBusy
    client.initialize(options);

    check(client.max_connections() == 1, "limit applied from Options",
          std::to_string(client.max_connections()));
    check(client.active_connections() == 0, "no slots taken before connect");

    // --- 1. Слот занят: второй connect() без ожидания получает SlotBusy -----
    libadb::Status status = libadb::Status::Ok;
    auto device = client.connect(first, &status);
    check(device != nullptr, std::string("connect ") + first, libadb::to_string(status));
    if (!device) {
        printf("\nCannot continue without a live device.\n");
        return 1;
    }
    check(client.active_connections() == 1, "slot is taken",
          std::to_string(client.active_connections()));

    libadb::Status busy_status = libadb::Status::Ok;
    auto busy = client.connect(second, &busy_status);
    check(busy == nullptr && busy_status == libadb::Status::SlotBusy,
          "second connect with slot_acquire=0 -> SlotBusy", libadb::to_string(busy_status));

    // --- 2. Конечный таймаут ожидания -> SlotTimeout ------------------------
    client.set_max_connections(1);
    libadb::Options waiting = options;
    waiting.timeouts.slot_acquire = libadb::ms{700};
    client.initialize(waiting);

    const auto timeout_started = clock_type::now();
    libadb::Status timeout_status = libadb::Status::Ok;
    auto timed_out = client.connect(second, &timeout_status);
    const int64_t waited = elapsed_ms(timeout_started);
    check(timed_out == nullptr && timeout_status == libadb::Status::SlotTimeout,
          "connect waits and returns SlotTimeout", libadb::to_string(timeout_status));
    check(waited >= 600 && waited < 3000, "waited about slot_acquire",
          std::to_string(waited) + "ms");

    // --- 3. Освобождение слота будит ожидающего -----------------------------
    libadb::Options patient = options;
    patient.timeouts.slot_acquire = libadb::ms{15000};
    client.initialize(patient);

    std::atomic<bool> waiter_done{false};
    libadb::Status waiter_status = libadb::Status::Internal;
    std::thread waiter([&] {
        auto got = client.connect(second, &waiter_status);
        waiter_done = true;
        if (got) got->close();
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    check(!waiter_done, "waiter is blocked while the slot is busy");

    device->close();  // освобождаем слот
    waiter.join();
    check(waiter_status == libadb::Status::Ok, "waiter got the slot after close()",
          libadb::to_string(waiter_status));
    check(client.active_connections() == 0, "slot returned after close()",
          std::to_string(client.active_connections()));

    // --- 4. Слот освобождается и без явного close() -------------------------
    {
        libadb::Status scoped_status = libadb::Status::Ok;
        auto scoped = client.connect(first, &scoped_status);
        check(scoped != nullptr && client.active_connections() == 1,
              "slot taken by scoped device", libadb::to_string(scoped_status));
    }
    check(client.active_connections() == 0, "slot returned when Device is destroyed",
          std::to_string(client.active_connections()));

    // --- 5. Батч не превышает лимит ----------------------------------------
    libadb::Options batch = options;
    batch.max_connections = 1;
    batch.max_parallel = 4;                  // потоков больше, чем слотов
    batch.timeouts.slot_acquire = libadb::ms{30000};
    client.initialize(batch);

    std::atomic<size_t> peak{0};
    std::atomic<size_t> connected{0};
    const std::vector<std::string> addresses = {first, second, first, second};

    client.for_each(addresses, [&](const libadb::DevicePtr& dev, const std::string&,
                                   libadb::Status st) {
        if (!dev) return;
        ++connected;
        const size_t now = client.active_connections();
        size_t seen = peak.load();
        while (now > seen && !peak.compare_exchange_weak(seen, now)) {
        }
        (void)st;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    });

    check(connected == addresses.size(), "batch processed every address",
          std::to_string(connected.load()) + "/" + std::to_string(addresses.size()));
    check(peak <= 1, "batch never exceeded max_connections", "peak=" + std::to_string(peak.load()));
    check(client.active_connections() == 0, "all slots returned after batch",
          std::to_string(client.active_connections()));

    // --- 6. Снятие лимита ---------------------------------------------------
    client.set_max_connections(0);
    check(client.max_connections() == 0, "limit removed at runtime");

    libadb::Status a_status = libadb::Status::Ok;
    libadb::Status b_status = libadb::Status::Ok;
    auto a = client.connect(first, &a_status);
    auto b = client.connect(second, &b_status);
    check(a != nullptr && b != nullptr, "two connections without a limit",
          std::string(libadb::to_string(a_status)) + "/" + libadb::to_string(b_status));
    if (a) a->close();
    if (b) b->close();

    client.shutdown();

    printf("\n%s (failures: %d)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
