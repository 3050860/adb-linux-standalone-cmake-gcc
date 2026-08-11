// examples/cpp/shell.cpp — выполнить команду и получить вывод.
// Запуск: ./ex_shell 192.168.1.10 "cat /proc/version"
#include <cstdio>
#include "libadb/libadb.h"

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <host:port> <command>\n", argv[0]);
        return 1;
    }

    auto& client = libadb::Client::instance();
    client.initialize();

    libadb::Status status;
    auto device = client.connect(argv[1], &status);
    if (!device) {
        fprintf(stderr, "connect: %s\n", libadb::to_string(status));
        return 1;
    }

    libadb::ShellOptions opts;
    // Потоковый вывод по мере поступления.
    opts.capture_output = false;
    opts.on_output = [](const std::string&, std::string_view chunk, bool is_stderr) {
        fprintf(is_stderr ? stderr : stdout, "%.*s",
                static_cast<int>(chunk.size()), chunk.data());
    };

    auto result = device->shell(argv[2], opts);
    return result.exit_code;
}
