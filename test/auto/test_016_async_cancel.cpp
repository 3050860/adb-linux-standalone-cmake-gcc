// test_016: асинхронный режим и отмена (§9 спецификации, этап 8).
// Проверяем: push_async/shell_async работают и не блокируют вызывающего;
// wait(timeout) отличает «не дождались» от «готово»; второй *_async на занятом
// устройстве даёт DeviceBusy; Operation::cancel() и Client::cancel(id) реально
// прерывают синхронную и асинхронную работу; close_all() даёт ConnectionClosed
// (а не Canceled); BatchOperation обрабатывает список адресов.
//
// Сборка:
//   g++ -std=c++20 test/auto/test_016_async_cancel.cpp -Iinclude -Ibuild/include
//       -Lbuild -ladb -Wl,-rpath,$PWD/build -pthread -o /tmp/test_016
//
// Запуск: /tmp/test_016 <ip-1> [ip-2]
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
    unsigned seed = 4242;
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

struct EventLog {
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

    void clear() {
        std::lock_guard<std::mutex> lock(mutex);
        events.clear();
    }
};

}  // namespace

int main(int argc, char** argv) {
    const std::string first = argc > 1 ? argv[1] : "192.168.177.249";
    const std::string second = argc > 2 ? argv[2] : first;

    const std::string big_file = "/tmp/libadb-test-016-big.bin";
    check(make_file(big_file, 64u * 1024 * 1024), "prepared 64 MiB file");

    EventLog events;

    auto& client = libadb::Client::instance();
    libadb::Options options;
    options.async_worker_threads = 4;
    options.progress_interval = libadb::ms{0};
    options.timeouts.shell = libadb::ms{60000};
    options.on_event = [&events](const libadb::Event& e) { events.add(e); };
    client.initialize(options);

    libadb::Status status = libadb::Status::Ok;
    auto device = client.connect(first, &status);
    check(device != nullptr, std::string("connect ") + first, libadb::to_string(status));
    if (!device) {
        printf("\nCannot continue without a live device.\n");
        return 1;
    }

    const std::string remote = "/data/local/tmp/libadb-test-016.bin";

    // --- 1. shell_async не блокирует и wait() отдаёт результат ---------------
    auto started = clock_type::now();
    auto op = device->shell_async("sleep 2; echo async-done", libadb::ShellOptions{});
    const int64_t submit_ms = elapsed_ms(started);
    check(submit_ms < 500, "shell_async вернулся сразу", std::to_string(submit_ms) + " ms");
    check(op->command() == libadb::Command::Shell, "Operation::command()");
    check(op->serial() == device->serial(), "Operation::serial()", op->serial());
    check(device->busy(), "устройство помечено занятым");

    // wait с маленьким таймаутом не должен дождаться.
    check(!op->wait(libadb::ms{300}), "wait(300ms) не дождался");
    check(!op->done(), "операция ещё не завершена");

    // --- 2. Второй *_async на занятом устройстве → DeviceBusy ---------------
    auto busy_op = device->shell_async("echo nope", libadb::ShellOptions{});
    check(busy_op->done(), "второй *_async завершён сразу");
    check(busy_op->result().status == libadb::Status::DeviceBusy, "второй *_async → DeviceBusy",
          libadb::to_string(busy_op->result().status));

    // --- 3. wait() без таймаута дожидается и отдаёт Result ------------------
    check(op->wait(), "wait() дождался");
    check(op->done(), "done() после wait()");
    check(op->result().ok(), "результат операции Ok", libadb::to_string(op->result().status));
    check(op->result().output.find("async-done") != std::string::npos,
          "вывод команды попал в Result", op->result().output);
    check(op->id() != 0, "у операции есть OperationId", std::to_string(op->id()));
    check(!device->busy(), "устройство освободилось");

    // --- 4. Operation::cancel() прерывает передачу --------------------------
    events.clear();
    libadb::PushOptions push_options;
    push_options.compression = libadb::Compression::None;
    auto push_op = device->push_async(big_file, remote, push_options);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));  // дать начать
    started = clock_type::now();
    push_op->cancel();
    check(push_op->wait(libadb::ms{10000}), "отменённая передача завершилась");
    const int64_t cancel_ms = elapsed_ms(started);
    check(cancel_ms < 5000, "отмена сработала за сотни мс, а не минуты",
          std::to_string(cancel_ms) + " ms");
    check(push_op->result().status == libadb::Status::Canceled, "статус Canceled",
          libadb::to_string(push_op->result().status));
    check(push_op->canceled(), "Operation::canceled()");
    check(push_op->result().transfer.bytes < 64u * 1024 * 1024, "передалось не всё",
          std::to_string(push_op->result().transfer.bytes));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    check(events.count(libadb::EventType::OperationCanceled) == 1,
          "пришло событие OperationCanceled",
          std::to_string(events.count(libadb::EventType::OperationCanceled)));
    check(device->is_online(), "устройство живо после отмены");

    // --- 5. Client::cancel(id) отменяет СИНХРОННУЮ операцию -----------------
    events.clear();
    std::atomic<libadb::OperationId> sync_id{0};
    libadb::PushOptions with_start;
    with_start.compression = libadb::Compression::None;
    with_start.on_start = [&sync_id](libadb::OperationId id, const std::string&) {
        sync_id.store(id);
    };

    std::atomic<bool> canceller_ok{false};
    std::thread canceller([&] {
        // Ждём, пока синхронный push сообщит свой id, и отменяем его снаружи.
        for (int i = 0; i < 100 && sync_id.load() == 0; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        canceller_ok.store(client.cancel(sync_id.load()));
    });

    libadb::Result sync_canceled = device->push(big_file, remote, with_start);
    canceller.join();
    check(sync_id.load() != 0, "on_start отдал id синхронной операции",
          std::to_string(sync_id.load()));
    check(canceller_ok.load(), "Client::cancel(id) нашёл операцию");
    check(sync_canceled.status == libadb::Status::Canceled, "синхронный push отменён",
          libadb::to_string(sync_canceled.status));

    // После завершения операции её id больше не отменяется.
    check(!client.cancel(sync_id.load()), "cancel() завершённой операции возвращает false");
    check(client.active_operations() == 0, "активных операций не осталось",
          std::to_string(client.active_operations()));

    // --- 6. cancel_all() ----------------------------------------------------
    auto long_op = device->push_async(big_file, remote, push_options);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const size_t canceled_count = client.cancel_all();
    check(canceled_count >= 1, "cancel_all() пометил операции",
          std::to_string(canceled_count));
    check(long_op->wait(libadb::ms{10000}), "операция завершилась после cancel_all()");
    check(long_op->result().status == libadb::Status::Canceled, "статус после cancel_all()",
          libadb::to_string(long_op->result().status));

    // --- 7. Device::cancel_current() ----------------------------------------
    auto current_op = device->push_async(big_file, remote, push_options);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    check(device->cancel_current() >= 1, "cancel_current() пометил операцию устройства");
    check(current_op->wait(libadb::ms{10000}), "операция завершилась после cancel_current()");
    check(current_op->result().status == libadb::Status::Canceled,
          "статус после cancel_current()", libadb::to_string(current_op->result().status));

    // --- 8. close_all() даёт ConnectionClosed, а не Canceled ----------------
    events.clear();
    auto closing_op = device->push_async(big_file, remote, push_options);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    client.close_all();
    check(closing_op->wait(libadb::ms{10000}), "операция завершилась после close_all()");
    check(closing_op->result().status == libadb::Status::ConnectionClosed,
          "close_all() → ConnectionClosed (отличим от Canceled)",
          libadb::to_string(closing_op->result().status));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    check(events.count(libadb::EventType::OperationCanceled) == 1,
          "close_all(): пришло OperationCanceled");
    device.reset();

    // --- 9. BatchOperation по списку адресов --------------------------------
    {
        const std::vector<std::string> addresses{first, second};
        auto batch = client.shell_all_async(addresses, "echo batch-ok");
        check(batch->total() == addresses.size(), "BatchOperation::total()",
              std::to_string(batch->total()));
        check(batch->wait(libadb::ms{60000}), "батч дождался");
        check(batch->done(), "батч завершён");
        check(batch->finished() == batch->total(), "все операции завершены",
              std::to_string(batch->finished()) + "/" + std::to_string(batch->total()));

        auto results = batch->results();
        // Адреса могут совпадать (если второй аргумент не задан), поэтому
        // сравниваем с числом уникальных ключей.
        std::vector<std::string> unique = addresses;
        std::sort(unique.begin(), unique.end());
        unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
        check(results.size() == unique.size(), "результаты по каждому адресу",
              std::to_string(results.size()) + "/" + std::to_string(unique.size()));

        bool all_ok = !results.empty();
        for (const auto& [address, result] : results) {
            if (!result.ok() || result.output.find("batch-ok") == std::string::npos) {
                all_ok = false;
                printf("[INFO] %s -> %s %s\n", address.c_str(), libadb::to_string(result.status),
                       result.error.c_str());
            }
        }
        check(all_ok, "все устройства в батче выполнили команду");

        check(batch->operations().size() == addresses.size(),
              "operations() отдаёт хэндлы для точечной отмены");
    }

    client.shutdown();

    printf("\n%s (%d failure(s))\n", failures == 0 ? "ALL PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
