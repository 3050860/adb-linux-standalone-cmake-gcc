// test_013: система событий (§8 спецификации, этап 5).
// Проверяем: подписка из Options и через subscribe(), маски, порядок событий
// внутри операции, доставка из отдельного потока, OperationId из on_start
// совпадает с событиями, троттлинг прогресса, unsubscribe, ClientShutdown.
//
// Сборка:
//   g++ -std=c++20 test/auto/test_013_events.cpp -Iinclude -Ibuild/include \
//       -Lbuild -ladb -Wl,-rpath,$PWD/build -pthread -o /tmp/test_013
//
// Запуск: /tmp/test_013 <ip-1>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
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

// Потокобезопасный сборщик событий: диспетчер работает в своём потоке.
struct Recorder {
    mutable std::mutex mutex;
    std::vector<libadb::Event> events;
    std::vector<std::thread::id> threads;

    void add(const libadb::Event& event) {
        std::lock_guard<std::mutex> lock(mutex);
        events.push_back(event);
        threads.push_back(std::this_thread::get_id());
    }

    std::vector<libadb::Event> snapshot() const {
        std::lock_guard<std::mutex> lock(mutex);
        return events;
    }

    size_t count(libadb::EventType type) const {
        std::lock_guard<std::mutex> lock(mutex);
        size_t n = 0;
        for (const auto& e : events) {
            if (e.type == type) ++n;
        }
        return n;
    }

    bool has(libadb::EventType type) const { return count(type) > 0; }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex);
        events.clear();
        threads.clear();
    }
};

// Ждёт появления события нужного типа (диспетчер асинхронный).
bool wait_for(const Recorder& rec, libadb::EventType type, int timeout_ms = 3000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (rec.has(type)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

std::string types_of(const std::vector<libadb::Event>& events, libadb::OperationId op) {
    std::string out;
    for (const auto& e : events) {
        if (e.op != op) continue;
        if (!out.empty()) out += ",";
        out += libadb::to_string(e.type);
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string address = argc > 1 ? argv[1] : "192.168.177.249";

    Recorder primary;   // подписка из Options
    Recorder masked;    // подписка только на OperationFinished

    auto& client = libadb::Client::instance();

    libadb::Options options;
    options.on_event = [&primary](const libadb::Event& e) { primary.add(e); };
    options.progress_interval = libadb::ms{0};  // без троттлинга: ловим всё
    client.initialize(options);

    const auto main_thread = std::this_thread::get_id();

    // --- 1. Подключение: события устройства ---------------------------------
    libadb::Status status = libadb::Status::Ok;
    auto device = client.connect(address, &status);
    check(device != nullptr, std::string("connect ") + address, libadb::to_string(status));
    if (!device) {
        printf("\nCannot continue without a live device.\n");
        return 1;
    }

    check(wait_for(primary, libadb::EventType::DeviceConnecting), "DeviceConnecting delivered");
    check(wait_for(primary, libadb::EventType::DeviceConnected), "DeviceConnected delivered");

    {
        auto events = primary.snapshot();
        bool serial_ok = false;
        for (const auto& e : events) {
            if (e.type == libadb::EventType::DeviceConnected) serial_ok = !e.serial.empty();
        }
        check(serial_ok, "DeviceConnected carries serial");
    }

    // Доставка идёт не из потока приложения.
    {
        std::lock_guard<std::mutex> lock(primary.mutex);
        bool other_thread = !primary.threads.empty();
        for (auto id : primary.threads) {
            if (id == main_thread) other_thread = false;
        }
        check(other_thread, "events delivered from a dispatcher thread");
    }

    // --- 2. Операция: on_start отдаёт id, события идут по порядку ------------
    primary.clear();
    libadb::OperationId started_id = 0;
    std::string started_serial;

    libadb::ShellOptions shell_options;
    shell_options.capture_output = true;
    shell_options.on_start = [&](libadb::OperationId id, const std::string& serial) {
        started_id = id;
        started_serial = serial;
    };
    libadb::Result shell_result = device->shell("echo libadb-events", shell_options);
    check(shell_result.ok(), "shell ok", libadb::to_string(shell_result.status));
    check(started_id != 0, "on_start reported operation id", std::to_string(started_id));
    check(started_serial == device->serial(), "on_start reported serial", started_serial);

    check(wait_for(primary, libadb::EventType::OperationFinished), "OperationFinished delivered");

    {
        auto events = primary.snapshot();
        // Первое событие операции — OperationStarted, последнее — Finished.
        libadb::EventType first = libadb::EventType::InternalError;
        libadb::EventType last = libadb::EventType::InternalError;
        size_t op_events = 0;
        bool ids_match = true;
        for (const auto& e : events) {
            if (e.op == 0) continue;
            if (e.op != started_id) ids_match = false;
            if (op_events == 0) first = e.type;
            last = e.type;
            ++op_events;
        }
        check(ids_match, "all operation events carry the same id");
        check(first == libadb::EventType::OperationStarted, "first event is OperationStarted",
              libadb::to_string(first));
        check(last == libadb::EventType::OperationFinished, "last event is OperationFinished",
              libadb::to_string(last));
        check(op_events >= 2, "operation produced events", types_of(events, started_id));
    }

    check(primary.has(libadb::EventType::OperationOutput), "OperationOutput delivered");
    {
        auto events = primary.snapshot();
        std::string text;
        for (const auto& e : events) {
            if (e.type == libadb::EventType::OperationOutput) text += e.message;
        }
        check(text.find("libadb-events") != std::string::npos,
              "OperationOutput carries the output", text);
    }

    // --- 3. Маска: подписчик получает только то, что просил -----------------
    primary.clear();
    const libadb::SubscriptionId masked_id = client.subscribe(
        [&masked](const libadb::Event& e) { masked.add(e); },
        libadb::event_bit(libadb::EventType::OperationFinished));
    check(masked_id != 0, "subscribe returned an id");

    device->shell("echo second", libadb::ShellOptions{});
    check(wait_for(masked, libadb::EventType::OperationFinished),
          "masked subscriber got OperationFinished");
    {
        auto events = masked.snapshot();
        bool only_finished = !events.empty();
        for (const auto& e : events) {
            if (e.type != libadb::EventType::OperationFinished) only_finished = false;
        }
        check(only_finished, "masked subscriber got nothing else",
              std::to_string(events.size()) + " event(s)");
    }
    // Подписчик без маски по-прежнему видит всё.
    check(primary.has(libadb::EventType::OperationStarted),
          "unmasked subscriber still gets everything");

    // --- 4. unsubscribe перестаёт доставлять --------------------------------
    client.unsubscribe(masked_id);
    masked.clear();
    device->shell("echo third", libadb::ShellOptions{});
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    check(masked.snapshot().empty(), "unsubscribe stops delivery",
          std::to_string(masked.snapshot().size()) + " event(s)");

    // --- 5. Троттлинг прогресса в рантайме ----------------------------------
    check(client.progress_interval() == libadb::ms{0}, "progress_interval from Options",
          std::to_string(client.progress_interval().count()));
    client.set_progress_interval(libadb::ms{500});
    check(client.progress_interval() == libadb::ms{500}, "progress_interval changed at runtime");
    client.set_progress_interval(libadb::ms{0});

    // --- 6. Ошибочная операция → OperationFailed ----------------------------
    primary.clear();
    libadb::Result bad = device->push("/tmp/libadb-no-such-file-013", "/data/local/tmp/");
    check(!bad.ok(), "push of a missing file failed", libadb::to_string(bad.status));
    check(wait_for(primary, libadb::EventType::OperationFailed), "OperationFailed delivered");
    {
        auto events = primary.snapshot();
        bool status_ok = false;
        for (const auto& e : events) {
            if (e.type == libadb::EventType::OperationFailed) {
                status_ok = e.status == bad.status && e.command == libadb::Command::Push;
            }
        }
        check(status_ok, "OperationFailed carries status and command");
    }

    // --- 7. Событие OperationStats на успешной передаче ---------------------
    primary.clear();
    {
        // Готовим локальный файл, чтобы было что передавать.
        FILE* f = fopen("/tmp/libadb-test-013.bin", "wb");
        if (f) {
            std::vector<char> data(64 * 1024, 'x');
            fwrite(data.data(), 1, data.size(), f);
            fclose(f);
        }
    }
    libadb::Result push_result =
        device->push("/tmp/libadb-test-013.bin", "/data/local/tmp/libadb-test-013.bin");
    check(push_result.ok(), "push ok", libadb::to_string(push_result.status));
    if (push_result.ok()) {
        check(wait_for(primary, libadb::EventType::OperationStats), "OperationStats delivered");
        auto events = primary.snapshot();
        bool stats_ok = false;
        for (const auto& e : events) {
            if (e.type == libadb::EventType::OperationStats) {
                stats_ok = e.stats.bytes == push_result.transfer.bytes && e.stats.bytes > 0;
            }
        }
        check(stats_ok, "OperationStats carries TransferStats",
              std::to_string(push_result.transfer.bytes));
        check(primary.has(libadb::EventType::OperationProgress), "OperationProgress delivered");
        check(primary.has(libadb::EventType::OperationPhaseChanged),
              "OperationPhaseChanged delivered");
    }

    // --- 8. Закрытие и остановка --------------------------------------------
    primary.clear();
    device->close();
    check(wait_for(primary, libadb::EventType::DeviceDisconnected), "DeviceDisconnected delivered");
    device.reset();

    client.shutdown();
    check(primary.has(libadb::EventType::ClientShutdown), "ClientShutdown delivered before stop");

    printf("\n%s (%d failure(s))\n", failures == 0 ? "ALL PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
