// test_014: статистика передачи (§8/§9, этап 6).
// Проверяем: промежуточный прогресс приходит порциями (а не одним событием в
// конце), bytes/bytes_on_wire/mib_per_sec заполнены, сжатие уменьшает
// bytes_on_wire, троттлинг прогресса реально режет число событий, install
// сообщает объём залитого apk.
//
// Сборка:
//   g++ -std=c++20 test/auto/test_014_transfer_stats.cpp -Iinclude -Ibuild/include
//       -Lbuild -ladb -Wl,-rpath,$PWD/build -pthread -o /tmp/test_014
//
// Запуск: /tmp/test_014 <ip> [путь к apk]
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
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

// Файл со случайными данными: сжимаемый мусор из нулей исказил бы проверку
// bytes_on_wire (zstd сожмёт его в ничто и «по проводу» будет пара килобайт).
bool make_file(const std::string& path, size_t size, bool compressible) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    std::vector<char> block(64 * 1024);
    size_t written = 0;
    unsigned seed = 12345;
    while (written < size) {
        for (size_t i = 0; i < block.size(); ++i) {
            if (compressible) {
                block[i] = 'A';
            } else {
                seed = seed * 1103515245u + 12345u;
                block[i] = static_cast<char>((seed >> 16) & 0xff);
            }
        }
        const size_t chunk = std::min(block.size(), size - written);
        fwrite(block.data(), 1, chunk, f);
        written += chunk;
    }
    fclose(f);
    return true;
}

struct ProgressLog {
    mutable std::mutex mutex;
    std::vector<std::pair<uint64_t, uint64_t>> points;  // done, total

    void add(uint64_t done, uint64_t total) {
        std::lock_guard<std::mutex> lock(mutex);
        points.emplace_back(done, total);
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex);
        return points.size();
    }

    bool monotonic() const {
        std::lock_guard<std::mutex> lock(mutex);
        uint64_t last = 0;
        for (const auto& p : points) {
            if (p.first < last) return false;
            last = p.first;
        }
        return true;
    }

    uint64_t max_done() const {
        std::lock_guard<std::mutex> lock(mutex);
        uint64_t best = 0;
        for (const auto& p : points) best = std::max(best, p.first);
        return best;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex);
        points.clear();
    }
};

}  // namespace

int main(int argc, char** argv) {
    const std::string address = argc > 1 ? argv[1] : "192.168.177.249";
    const std::string apk = argc > 2 ? argv[2] : "test/main.apk";

    const std::string random_file = "/tmp/libadb-test-014-random.bin";
    const std::string plain_file = "/tmp/libadb-test-014-plain.bin";
    const size_t file_size = 4u * 1024 * 1024;  // 4 МиБ: хватает на много блоков
    check(make_file(random_file, file_size, false), "prepared incompressible file");
    check(make_file(plain_file, file_size, true), "prepared compressible file");

    ProgressLog event_progress;   // из событий OperationProgress
    ProgressLog callback_progress;  // из PushOptions::on_progress

    auto& client = libadb::Client::instance();
    libadb::Options options;
    options.progress_interval = libadb::ms{0};  // без троттлинга
    options.on_event = [&event_progress](const libadb::Event& e) {
        if (e.type == libadb::EventType::OperationProgress) {
            event_progress.add(e.bytes_done, e.bytes_total);
        }
    };
    client.initialize(options);

    libadb::Status status = libadb::Status::Ok;
    auto device = client.connect(address, &status);
    check(device != nullptr, std::string("connect ") + address, libadb::to_string(status));
    if (!device) {
        printf("\nCannot continue without a live device.\n");
        return 1;
    }

    const std::string remote = "/data/local/tmp/libadb-test-014.bin";

    // --- 1. push без сжатия: прогресс порциями, статистика заполнена ---------
    libadb::PushOptions push_options;
    push_options.compression = libadb::Compression::None;
    push_options.on_progress = [&callback_progress](const std::string&, uint64_t done,
                                                    uint64_t total) {
        callback_progress.add(done, total);
    };
    libadb::Result push_result = device->push(random_file, remote, push_options);
    check(push_result.ok(), "push (no compression)", libadb::to_string(push_result.status));

    // Диспетчер асинхронный — даём ему дослать очередь.
    std::this_thread::sleep_for(std::chrono::milliseconds(400));

    check(push_result.transfer.bytes == file_size, "Result::transfer.bytes == размер файла",
          std::to_string(push_result.transfer.bytes));
    check(push_result.transfer.bytes_on_wire > 0, "bytes_on_wire заполнен",
          std::to_string(push_result.transfer.bytes_on_wire));
    check(push_result.transfer.duration.count() > 0, "duration фазы передачи заполнена",
          std::to_string(push_result.transfer.duration.count()) + " ms");
    check(push_result.transfer.mib_per_sec > 0.0, "mib_per_sec посчитан",
          std::to_string(push_result.transfer.mib_per_sec));
    // Без сжатия «по проводу» немного больше полезных данных (заголовки блоков).
    check(push_result.transfer.bytes_on_wire >= push_result.transfer.bytes,
          "без сжатия bytes_on_wire >= bytes");

    check(callback_progress.size() > 1, "on_progress вызван много раз",
          std::to_string(callback_progress.size()));
    check(callback_progress.monotonic(), "on_progress не идёт назад");
    check(callback_progress.max_done() == file_size, "on_progress дошёл до 100%",
          std::to_string(callback_progress.max_done()));

    check(event_progress.size() > 1, "OperationProgress пришёл много раз",
          std::to_string(event_progress.size()));
    check(event_progress.monotonic(), "OperationProgress не идёт назад");

    // --- 2. push со сжатием: bytes_on_wire меньше полезных байт -------------
    callback_progress.clear();
    libadb::PushOptions zip_options;
    zip_options.compression = libadb::Compression::Any;
    libadb::Result zip_result = device->push(plain_file, remote, zip_options);
    check(zip_result.ok(), "push (compression=Any)", libadb::to_string(zip_result.status));
    if (zip_result.ok()) {
        check(zip_result.transfer.bytes == file_size, "сжатие не меняет полезные байты",
              std::to_string(zip_result.transfer.bytes));
        check(zip_result.transfer.bytes_on_wire > 0, "bytes_on_wire при сжатии заполнен",
              std::to_string(zip_result.transfer.bytes_on_wire));
        // Файл из одинаковых байт жмётся в разы — если бы хук считал не то,
        // что уходит в сокет, это условие не выполнилось бы.
        check(zip_result.transfer.bytes_on_wire < zip_result.transfer.bytes,
              "сжатие уменьшило bytes_on_wire",
              std::to_string(zip_result.transfer.bytes_on_wire) + " < " +
                  std::to_string(zip_result.transfer.bytes));
    }

    // --- 3. pull: статистика и прогресс ------------------------------------
    callback_progress.clear();
    event_progress.clear();
    libadb::PullOptions pull_options;
    pull_options.on_progress = [&callback_progress](const std::string&, uint64_t done,
                                                    uint64_t total) {
        callback_progress.add(done, total);
    };
    libadb::Result pull_result =
        device->pull(remote, "/tmp/libadb-test-014-pulled.bin", pull_options);
    check(pull_result.ok(), "pull", libadb::to_string(pull_result.status));
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    if (pull_result.ok()) {
        check(pull_result.transfer.bytes == file_size, "pull: bytes == размер файла",
              std::to_string(pull_result.transfer.bytes));
        check(pull_result.transfer.bytes_on_wire > 0, "pull: bytes_on_wire заполнен",
              std::to_string(pull_result.transfer.bytes_on_wire));
        check(pull_result.transfer.mib_per_sec > 0.0, "pull: mib_per_sec посчитан",
              std::to_string(pull_result.transfer.mib_per_sec));
        check(callback_progress.size() > 1, "pull: on_progress вызван много раз",
              std::to_string(callback_progress.size()));
        check(event_progress.size() > 1, "pull: OperationProgress пришёл много раз",
              std::to_string(event_progress.size()));
    }

    // --- 4. Троттлинг реально режет число событий ---------------------------
    event_progress.clear();
    client.set_progress_interval(libadb::ms{100000});  // фактически «только первое и последнее»
    libadb::Result throttled = device->push(random_file, remote, zip_options);
    check(throttled.ok(), "push с троттлингом", libadb::to_string(throttled.status));
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    check(event_progress.size() <= 2, "троттлинг оставил только первое и последнее событие",
          std::to_string(event_progress.size()));
    client.set_progress_interval(libadb::ms{0});

    // --- 5. install сообщает объём залитого apk ----------------------------
    {
        FILE* f = fopen(apk.c_str(), "rb");
        if (!f) {
            printf("[SKIP] install: %s не найден\n", apk.c_str());
        } else {
            fclose(f);
            callback_progress.clear();
            libadb::InstallOptions install_options;
            install_options.on_progress = [&callback_progress](const std::string&, uint64_t done,
                                                               uint64_t total) {
                callback_progress.add(done, total);
            };
            libadb::Result install_result = device->install(apk, install_options);
            check(install_result.ok(), "install", libadb::to_string(install_result.status));
            if (install_result.ok()) {
                check(install_result.transfer.bytes > 0, "install: bytes заполнен",
                      std::to_string(install_result.transfer.bytes));
                check(install_result.transfer.mib_per_sec > 0.0, "install: mib_per_sec посчитан",
                      std::to_string(install_result.transfer.mib_per_sec));
                check(callback_progress.size() > 1, "install: on_progress вызван много раз",
                      std::to_string(callback_progress.size()));
            }
        }
    }

    device->shell("rm -f " + remote, libadb::ShellOptions{});
    device->close();
    client.shutdown();

    printf("\n%s (%d failure(s))\n", failures == 0 ? "ALL PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
