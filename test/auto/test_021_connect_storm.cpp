// test_021: шторм подключений без устройства. Много потоков одновременно вызывают connect() к адресам,
// где никто не слушает (отказ в соединении), — так сервер netariumd при подключении тысяч новых ТВ с одним
// адресом (эмулятор, смена адресов по DHCP) проверял автоустановку пятью потоками и падал внутри libadb
// (SIGSEGV, разрушенный стек, netariumd 5.15.50 и 5.16 с libadb 1.1.0).
//
// Проверяем: ни один connect не возвращает устройство, у каждого — статус ошибки; процесс не падает;
// connect к одному адресу из многих потоков и к разным адресам вперемешку; close() устройств из других
// потоков (если вдруг подключилось) и close_all() в разгар шторма. Шторм не ждёт таймаутов подключения:
// до 1.1.1 второй connect к адресу, начатый между hasDevice() и connectDevice(), получал чужое устройство
// с неподключённым слушателем, ждал connect_timeout и закрывал устройство первого — прогон шёл 10–14 с.
//
// Устройства не нужны. Порты — закрытые на 127.0.0.1 (по умолчанию 1 и 2; свободность проверяется).
//
// Сборка:
//   g++ -std=c++20 test/auto/test_021_connect_storm.cpp -Iinclude -Ibuild/include
//       -Lbuild -ladb -Wl,-rpath,$PWD/build -pthread -o /tmp/test_021
//
// Запуск: /tmp/test_021 [потоков=16] [подключений на поток=300]
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "libadb/libadb.h"

namespace {

int failures = 0;

void check(bool condition, const std::string& name, const std::string& details = {}) {
    printf("%s %s%s%s\n", condition ? "[ OK ]" : "[FAIL]", name.c_str(),
           details.empty() ? "" : " -> ", details.c_str());
    fflush(stdout);
    if (!condition) ++failures;
}

// Порт на 127.0.0.1, на котором никто не слушает
bool port_closed(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<uint16_t>(port));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const bool refused = ::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0;
    close(fd);
    return refused;
}

struct Result {
    std::atomic<int> attempts{0}, connected{0}, failed{0};
};

void storm(const std::vector<std::string>& addresses, int threads, int per_thread, Result& r,
           bool close_all_midway) {
    std::vector<std::thread> pool;
    std::atomic<bool> go{false};
    for (int t = 0; t < threads; ++t) {
        pool.emplace_back([&, t] {
            while (!go) std::this_thread::yield();
            for (int i = 0; i < per_thread; ++i) {
                const std::string& address = addresses[(t + i) % addresses.size()];
                libadb::Status status = libadb::Status::Ok;
                libadb::DevicePtr d = libadb::Client::instance().connect(address, &status);
                ++r.attempts;
                if (d) {
                    ++r.connected;
                    d->close();
                } else {
                    ++r.failed;
                }
            }
        });
    }
    std::thread closer;
    if (close_all_midway) {
        closer = std::thread([&] {
            while (!go) std::this_thread::yield();
            for (int i = 0; i < 20; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                libadb::Client::instance().close_all();
            }
        });
    }
    go = true;
    for (auto& th : pool) th.join();
    if (closer.joinable()) closer.join();
}

}  // namespace

int main(int argc, char** argv) {
    const int threads = argc > 1 ? atoi(argv[1]) : 16;
    const int per_thread = argc > 2 ? atoi(argv[2]) : 300;
    const std::string a1 = "127.0.0.1:1", a2 = "127.0.0.1:2";
    if (!port_closed(1) || !port_closed(2)) {
        printf("порты 1 и 2 на 127.0.0.1 заняты — тест неприменим\n");
        return 2;
    }
    libadb::Client::instance().initialize();
    libadb::Timeouts timeouts;
    timeouts.connect = std::chrono::milliseconds(2000);
    libadb::Client::instance().set_timeouts(timeouts);

    auto run = [&](const char* name, std::vector<std::string> addresses, bool close_all) {
        Result r;
        const auto start = std::chrono::steady_clock::now();
        storm(addresses, threads, per_thread, r, close_all);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        check(r.attempts == threads * per_thread && r.connected == 0 && r.failed == threads * per_thread && ms < 2000,
              name,
              std::to_string(r.attempts) + " попыток, " + std::to_string(r.failed) + " отказов, " +
                  std::to_string(r.connected) + " подключений, " + std::to_string(ms) + " мс");
    };
    run("один адрес, все потоки разом", {a1}, false);
    run("два адреса вперемешку", {a1, a2}, false);
    run("один адрес и close_all() в разгар", {a1}, true);

    printf(failures ? "FAILED: %d\n" : "ALL PASSED\n", failures);
    return failures ? 1 : 0;
}
