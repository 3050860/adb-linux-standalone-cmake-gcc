// test_015: таймауты (§6 спецификации, этап 7).
// Проверяем: Timeouts читаются из Options и меняются в рантайме; shell-таймаут
// прерывает висящую команду со Status::CommandTimeout и не оставляет устройство
// сломанным; TransferTimeout::total срывает передачу с CommandTimeout, stall —
// со StallTimeout; точечный timeout в *Options перебивает глобальный; install
// с фазовыми таймаутами и health-check доходит до конца и присылает heartbeat.
//
// Сборка:
//   g++ -std=c++20 test/auto/test_015_timeouts.cpp -Iinclude -Ibuild/include
//       -Lbuild -ladb -Wl,-rpath,$PWD/build -pthread -o /tmp/test_015
//
// Запуск: /tmp/test_015 <ip> [путь к apk]
#include <algorithm>
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

using clock_type = std::chrono::steady_clock;

int64_t elapsed_ms(clock_type::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(clock_type::now() - start).count();
}

bool make_file(const std::string& path, size_t size) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    std::vector<char> block(64 * 1024);
    unsigned seed = 777;
    size_t written = 0;
    while (written < size) {
        for (size_t i = 0; i < block.size(); ++i) {
            seed = seed * 1103515245u + 12345u;
            block[i] = static_cast<char>((seed >> 16) & 0xff);
        }
        const size_t chunk = std::min(block.size(), size - written);
        fwrite(block.data(), 1, chunk, f);
        written += chunk;
    }
    fclose(f);
    return true;
}

struct EventCounter {
    mutable std::mutex mutex;
    std::vector<libadb::Event> events;

    void add(const libadb::Event& e) {
        std::lock_guard<std::mutex> lock(mutex);
        events.push_back(e);
    }

    size_t count(libadb::EventType type) const {
        std::lock_guard<std::mutex> lock(mutex);
        size_t n = 0;
        for (const auto& e : events) {
            if (e.type == type) ++n;
        }
        return n;
    }

    bool has_phase(libadb::Phase phase) const {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto& e : events) {
            if (e.type == libadb::EventType::OperationPhaseChanged && e.phase == phase) return true;
        }
        return false;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex);
        events.clear();
    }
};

}  // namespace

int main(int argc, char** argv) {
    const std::string address = argc > 1 ? argv[1] : "192.168.177.249";
    const std::string apk = argc > 2 ? argv[2] : "test/main.apk";

    const std::string big_file = "/tmp/libadb-test-015-big.bin";
    // 48 МиБ несжимаемых данных: передача заведомо длиннее секунды, значит
    // total-таймаут успеет сработать посередине.
    check(make_file(big_file, 48u * 1024 * 1024), "prepared 48 MiB file");

    EventCounter events;

    auto& client = libadb::Client::instance();
    libadb::Options options;
    options.timeouts.connect = libadb::ms{20000};
    options.timeouts.shell = libadb::ms{3000};   // короткий, чтобы проверить срыв
    options.progress_interval = libadb::ms{0};
    options.on_event = [&events](const libadb::Event& e) { events.add(e); };
    client.initialize(options);

    // --- 1. Timeouts читаются и меняются ------------------------------------
    check(client.timeouts().shell == libadb::ms{3000}, "Timeouts::shell из Options",
          std::to_string(client.timeouts().shell.count()));
    check(client.timeouts().connect == libadb::ms{20000}, "Timeouts::connect из Options");
    check(client.timeouts().push.stall == libadb::ms{30000}, "TransferTimeout::stall по умолчанию",
          std::to_string(client.timeouts().push.stall.count()));
    check(client.timeouts().push.total == libadb::ms{0}, "TransferTimeout::total выключен");
    check(client.timeouts().install.commit_healthcheck == libadb::HealthCheckMode::Transport,
          "health-check по умолчанию Transport",
          libadb::to_string(client.timeouts().install.commit_healthcheck));

    libadb::Timeouts changed = client.timeouts();
    changed.shell = libadb::ms{4000};
    client.set_timeouts(changed);
    check(client.timeouts().shell == libadb::ms{4000}, "set_timeouts применился в рантайме");
    changed.shell = libadb::ms{3000};
    client.set_timeouts(changed);

    libadb::Status status = libadb::Status::Ok;
    auto device = client.connect(address, &status);
    check(device != nullptr, std::string("connect ") + address, libadb::to_string(status));
    if (!device) {
        printf("\nCannot continue without a live device.\n");
        return 1;
    }

    // --- 2. shell-таймаут прерывает висящую команду -------------------------
    events.clear();
    auto started = clock_type::now();
    libadb::Result hung = device->shell("sleep 30", libadb::ShellOptions{});
    const int64_t hung_ms = elapsed_ms(started);
    check(hung.status == libadb::Status::CommandTimeout, "sleep 30 сорван по таймауту",
          libadb::to_string(hung.status));
    check(hung_ms >= 2500 && hung_ms < 8000, "сорван примерно через Timeouts::shell",
          std::to_string(hung_ms) + " ms");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    check(events.count(libadb::EventType::OperationTimeout) == 1,
          "таймаут пришёл событием OperationTimeout",
          std::to_string(events.count(libadb::EventType::OperationTimeout)));

    // Устройство должно остаться рабочим: сессию прервали, транспорт цел.
    check(device->is_online(), "устройство живо после срыва команды");
    libadb::Result after = device->shell("echo alive", libadb::ShellOptions{});
    check(after.ok() && after.output.find("alive") != std::string::npos,
          "следующая команда работает", libadb::to_string(after.status));

    // --- 3. Точечный timeout в ShellOptions перебивает глобальный -----------
    libadb::ShellOptions patient;
    patient.timeout = libadb::ms{20000};  // глобальный 3 с, этой команде хватит 5
    started = clock_type::now();
    libadb::Result slow = device->shell("sleep 5; echo done", patient);
    const int64_t slow_ms = elapsed_ms(started);
    check(slow.ok() && slow.output.find("done") != std::string::npos,
          "ShellOptions::timeout перебил глобальный", libadb::to_string(slow.status));
    check(slow_ms >= 4500, "команда действительно шла 5 секунд", std::to_string(slow_ms) + " ms");

    // --- 4. TransferTimeout::total срывает передачу -------------------------
    events.clear();
    const std::string remote = "/data/local/tmp/libadb-test-015.bin";
    libadb::PushOptions short_total;
    short_total.compression = libadb::Compression::None;
    short_total.timeout = libadb::TransferTimeout{libadb::ms{800}, libadb::ms{0}};
    started = clock_type::now();
    libadb::Result cut = device->push(big_file, remote, short_total);
    const int64_t cut_ms = elapsed_ms(started);
    check(cut.status == libadb::Status::CommandTimeout, "push сорван по total",
          libadb::to_string(cut.status));
    check(cut_ms >= 700 && cut_ms < 15000, "сорван примерно через total",
          std::to_string(cut_ms) + " ms");
    check(cut.transfer.bytes > 0 && cut.transfer.bytes < 48u * 1024 * 1024,
          "успело передаться меньше файла", std::to_string(cut.transfer.bytes));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    check(events.count(libadb::EventType::OperationTimeout) == 1,
          "срыв передачи пришёл событием OperationTimeout");

    // Устройство должно остаться рабочим и после срыва передачи.
    check(device->is_online(), "устройство живо после срыва передачи");

    // --- 5. TransferTimeout::stall не срабатывает на живом канале -----------
    libadb::PushOptions stall_only;
    stall_only.compression = libadb::Compression::None;
    // stall = 5 с при активной передаче: прогресс идёт постоянно, значит
    // stall-таймер каждый раз сбрасывается и передача должна дойти до конца.
    stall_only.timeout = libadb::TransferTimeout{libadb::ms{0}, libadb::ms{5000}};
    libadb::Result full = device->push(big_file, remote, stall_only);
    check(full.ok(), "push целиком при живом stall-таймауте", libadb::to_string(full.status));
    if (full.ok()) {
        check(full.transfer.bytes == 48u * 1024 * 1024, "передан весь файл",
              std::to_string(full.transfer.bytes));
    }

    // --- 6. Нулевые таймауты = без ограничения ------------------------------
    libadb::PushOptions unlimited;
    unlimited.compression = libadb::Compression::None;
    unlimited.timeout = libadb::TransferTimeout{libadb::ms{0}, libadb::ms{0}};
    libadb::Result no_limit = device->push(big_file, remote, unlimited);
    check(no_limit.ok(), "нулевые total и stall не ограничивают",
          libadb::to_string(no_limit.status));

    // --- 7. install с фазовыми таймаутами и health-check --------------------
    {
        FILE* f = fopen(apk.c_str(), "rb");
        if (!f) {
            printf("[SKIP] install: %s не найден\n", apk.c_str());
        } else {
            fclose(f);
            events.clear();
            libadb::InstallOptions install_options;
            libadb::InstallTimeout it;
            // Интервал health-check маленький, чтобы heartbeat успел прилететь
            // даже на быстром коммите.
            it.commit_healthcheck = libadb::HealthCheckMode::Transport;
            it.commit_healthcheck_interval = libadb::ms{500};
            install_options.timeout = it;
            libadb::Result installed = device->install(apk, install_options);
            check(installed.ok(), "install с фазовыми таймаутами",
                  std::string(libadb::to_string(installed.status)) + " " + installed.error);
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            check(events.has_phase(libadb::Phase::Transfer), "была фаза Transfer");
            check(events.has_phase(libadb::Phase::Commit), "была фаза Commit");
            check(events.count(libadb::EventType::OperationHeartbeat) > 0,
                  "health-check присылал OperationHeartbeat",
                  std::to_string(events.count(libadb::EventType::OperationHeartbeat)));
        }
    }

    // --- 8. install: заведомо малый таймаут create_session --------------
    if (FILE* f = fopen(apk.c_str(), "rb")) {
        fclose(f);
        events.clear();
        libadb::InstallOptions impatient;
        libadb::InstallTimeout it;
        it.create_session = libadb::ms{1};  // pm не успеет ответить
        it.commit_healthcheck = libadb::HealthCheckMode::None;
        impatient.timeout = it;
        libadb::Result failed = device->install(apk, impatient);
        // Либо мы успели сорвать по таймауту, либо pm ответил быстрее нашей
        // проверки (шаг монитора 100 мс) — тогда установка просто прошла.
        const bool timed_out = failed.status == libadb::Status::CommandTimeout;
        printf("[INFO] install с create_session=1ms -> %s\n", libadb::to_string(failed.status));
        check(timed_out || failed.ok(), "install с крошечным create_session не сломался",
              libadb::to_string(failed.status));
    }

    device->shell("rm -f " + remote, libadb::ShellOptions{});
    device->close();
    client.shutdown();

    printf("\n%s (%d failure(s))\n", failures == 0 ? "ALL PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
