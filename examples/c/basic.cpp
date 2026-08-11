// examples/c/basic.cpp — использование libadb из C-стиля кода (C++11-совместимо).
//
// Примечание: полноценный C ABI (libadb_c.h) запланирован в версии 1.1 (этап 16).
// До его появления C-код должен компилироваться как C++. Функционально это
// покрывает большинство use-case'ов: заголовки libadb используют только STL,
// никаких исключений через границу API нет.
//
// Сборка из командной строки (пример):
//   g++ -std=c++17 basic.cpp $(pkg-config --cflags --libs libadb) -o ex_c
//
// Запуск: ./ex_c 192.168.1.10:5555 "getprop ro.build.version.release"
#include <cstdio>
#include <cstdlib>
#include "libadb/libadb.h"

/* Все вызовы выглядят как C-функции — никаких методов классов в main. */

static void on_output(const std::string& /*serial*/, std::string_view chunk, bool is_stderr) {
    fwrite(chunk.data(), 1, chunk.size(), is_stderr ? stderr : stdout);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <host:port> <command>\n", argv[0]);
        return 1;
    }

    libadb::Options opts;
    /* Ключ авторизации — из ~/.android/adbkey, как обычный adb. */
    libadb::Client::instance().initialize(opts);

    libadb::Status status = libadb::Status::Ok;
    libadb::DevicePtr device = libadb::Client::instance().connect(argv[1], &status);
    if (!device) {
        fprintf(stderr, "connect failed: %s\n", libadb::to_string(status));
        return 1;
    }

    libadb::ShellOptions shell_opts;
    shell_opts.capture_output = false;
    shell_opts.on_output      = on_output;

    libadb::Result result = device->shell(argv[2], shell_opts);
    device->close();
    libadb::Client::instance().shutdown();

    return result.exit_code;
}
