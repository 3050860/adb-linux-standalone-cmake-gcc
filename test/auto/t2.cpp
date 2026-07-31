#include <cassert>
#include <cstdio>
#include <string>
#include <vector>
#include "libadb/libadb.h"
int main(int argc, char**) {
    // 1. По умолчанию лог выключен: файл не должен появиться.
    auto o = libadb::log_options();
    printf("default: enabled=%d path=%s level=%s\n", o.enabled, o.file_path.c_str(),
           libadb::to_string(o.level));
    assert(!o.enabled);

    // 2. LogSink: собираем сообщения приложения.
    std::vector<std::string> got;
    libadb::set_log_sink([&](libadb::LogLevel l, const std::string& s, std::string_view m) {
        got.push_back(std::string(libadb::to_string(l)) + "|" + s + "|" + std::string(m));
    });
    libadb::log(libadb::LogLevel::Info, "192.168.1.5:5555", "hello");
    libadb::log(libadb::LogLevel::Off, "", "must be dropped");
    assert(got.size() == 1 && got[0] == "info|192.168.1.5:5555|hello");
    printf("sink ok: %s\n", got[0].c_str());

    // 3. Включаем внутренний лог в свой файл (только если запрошено).
    if (argc > 1) {
        libadb::LogOptions n;
        n.enabled = true;
        n.file_path = "/tmp/libadb-test.log";
        n.level = libadb::LogLevel::Trace;
        n.trace_tags = "sync,transport";
        libadb::set_log_options(n);
        libadb::flush_log();
        printf("enabled: path=%s\n", libadb::log_options().file_path.c_str());
    }
    return 0;
}
