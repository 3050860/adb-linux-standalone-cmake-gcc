#include <spdlog/sinks/basic_file_sink.h>
#include "libadb/spdlog_sink.hpp"
int main() {
    auto lg = spdlog::basic_logger_mt("app", "/tmp/libadb-spdlog.log", true);
    lg->set_level(spdlog::level::trace);
    libadb::set_log_sink(libadb::make_spdlog_sink(lg));
    libadb::log(libadb::LogLevel::Warn, "192.168.1.7:5555", "via spdlog sink");
    libadb::log(libadb::LogLevel::Debug, "", "no serial");
    lg->flush();
    return 0;
}
