// test_022: подключение из процесса, у которого открыто больше FD_SETSIZE (1024) дескрипторов, — как сервер,
// к которому подключены тысячи ТВ. До 1.1.1 socket_network_client_timeout ждала подключение через select():
// FD_SET с номером сокета больше 1023 писал за пределы fd_set на стеке — порча стека и SIGSEGV
// (сервер netariumd падал при автоустановке на тысячи ТВ). Под AddressSanitizer — stack-buffer-overflow.
//
// Проверяем: при 1100 лишних открытых дескрипторах connect к закрытому порту и к порту, где слушает
// обычный TCP-сервер (не adb), возвращает ошибку и не портит память; сокет подключения действительно
// получает номер больше 1023.
//
// Устройства не нужны.
//
// Сборка (лучше с -fsanitize=address и библиотекой, собранной так же):
//   g++ -std=c++20 test/auto/test_022_many_fds.cpp -Iinclude -Ibuild/include
//       -Lbuild -ladb -Wl,-rpath,$PWD/build -pthread -o /tmp/test_022
//
// Запуск: /tmp/test_022
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

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
    fflush(stdout);
    if (!condition) ++failures;
}

// TCP-сервер на 127.0.0.1 со свободным портом: принимает и сразу закрывает (не adb)
int listen_any(int* port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || listen(fd, 16) != 0) return -1;
    socklen_t len = sizeof(a);
    getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
    *port = ntohs(a.sin_port);
    return fd;
}

}  // namespace

int main() {
    rlimit rl{};
    getrlimit(RLIMIT_NOFILE, &rl);
    if (rl.rlim_cur < 4096) {
        rl.rlim_cur = rl.rlim_max < 4096 ? rl.rlim_max : 4096;
        setrlimit(RLIMIT_NOFILE, &rl);
    }
    std::vector<int> held;
    int base = open("/dev/null", O_RDONLY);
    for (int i = 0; i < 1100 && base >= 0; ++i) {
        int fd = dup(base);
        if (fd < 0) break;
        held.push_back(fd);
    }
    // Следующий сокет получит номер не меньше этого
    int probe = socket(AF_INET, SOCK_STREAM, 0);
    check(probe > 1023, "следующий сокет — с номером больше 1023", "номер " + std::to_string(probe));
    close(probe);

    int port = 0;
    const int server = listen_any(&port);
    std::atomic<bool> stop{false};
    std::thread acceptor([&] {
        while (!stop) {
            int c = accept(server, nullptr, nullptr);
            if (c >= 0) close(c);
        }
    });

    libadb::Client::instance().initialize();
    libadb::Timeouts timeouts;
    timeouts.connect = std::chrono::milliseconds(1500);
    libadb::Client::instance().set_timeouts(timeouts);

    for (int i = 0; i < 20; ++i) {
        libadb::Status st = libadb::Status::Ok;
        auto d = libadb::Client::instance().connect("127.0.0.1:1", &st);
        if (d || i == 19)
            check(!d && st != libadb::Status::Ok, "закрытый порт: отказ без порчи памяти (20 раз)",
                  std::string("статус ") + libadb::to_string(st));
        if (d) break;
    }
    {
        libadb::Status st = libadb::Status::Ok;
        auto d = libadb::Client::instance().connect("127.0.0.1:" + std::to_string(port), &st);
        check(!d && st != libadb::Status::Ok, "TCP-сервер не adb: ошибка подключения без порчи памяти",
              std::string("статус ") + libadb::to_string(st));
    }

    stop = true;
    shutdown(server, SHUT_RDWR);
    close(server);
    acceptor.join();
    for (int fd : held) close(fd);
    printf(failures ? "FAILED: %d\n" : "ALL PASSED\n", failures);
    return failures ? 1 : 0;
}
