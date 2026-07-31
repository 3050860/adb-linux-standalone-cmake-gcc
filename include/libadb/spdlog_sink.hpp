/*
 * libadb — адаптер LogSink для spdlog.
 *
 * НЕ является частью ABI: header-only, компилируется у вызывающего его
 * собственной версией spdlog. Именно поэтому spdlog не появляется в
 * libadb.h — разные версии header-only библиотеки у .so и у приложения
 * означали бы ODR/ABI-проблемы.
 *
 * Использование:
 *   #include <libadb/spdlog_sink.hpp>
 *   libadb::set_log_sink(libadb::make_spdlog_sink(my_logger));
 */
#pragma once

#include <memory>
#include <string>
#include <string_view>

#include <spdlog/spdlog.h>

#include "libadb/libadb.h"

namespace libadb {

inline spdlog::level::level_enum to_spdlog_level(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return spdlog::level::trace;
        case LogLevel::Debug: return spdlog::level::debug;
        case LogLevel::Info:  return spdlog::level::info;
        case LogLevel::Warn:  return spdlog::level::warn;
        case LogLevel::Error: return spdlog::level::err;
        case LogLevel::Off:   return spdlog::level::off;
    }
    return spdlog::level::info;
}

inline LogSink make_spdlog_sink(std::shared_ptr<spdlog::logger> logger) {
    return [logger = std::move(logger)](LogLevel level, const std::string& serial,
                                        std::string_view message) {
        if (level == LogLevel::Off || !logger) return;
        const auto l = to_spdlog_level(level);
        if (serial.empty()) {
            logger->log(l, "{}", message);
        } else {
            logger->log(l, "[{}] {}", serial, message);
        }
    };
}

}  // namespace libadb
