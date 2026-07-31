#include "sysdeps.h"
#include "adb_trace.h"

#include <string>
#include <unordered_map>
#include <vector>

#include <android-base/logging.h>
#include <android-base/strings.h>

#include "adb.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <mutex>

constexpr const char* LOG_FILE_PATH = "/tmp/adb.log";

// Вспомогательная функция для маппинга уровней логирования
static spdlog::level::level_enum MapSeverity(android::base::LogSeverity severity) {
    switch (severity) {
        case android::base::VERBOSE: return spdlog::level::trace;
        case android::base::DEBUG:   return spdlog::level::debug;
        case android::base::INFO:    return spdlog::level::info;
        case android::base::WARNING: return spdlog::level::warn;
        case android::base::ERROR:   return spdlog::level::err;
        case android::base::FATAL:
        case android::base::FATAL_WITHOUT_ABORT: return spdlog::level::critical;
        default: return spdlog::level::info;
    }
}

// 2. Конфигурируемый файловый логгер.
//
// Настройки задаются через adb_log_configure(); по умолчанию действуют значения
// AdbLogSettings (историческое поведение adb/adirect), кроме enabled — он берётся
// из adb_log_default_enabled(), и в libadb.so это false.
//
// Пока лог выключен, файл не открывается и не создаётся: sink конструируется
// лениво, при первой фактической записи.

#include <cstdio>
#include <memory>
#include <stdexcept>

namespace {

std::mutex g_log_mutex;
std::shared_ptr<spdlog::logger> g_logger;
bool g_logger_dirty = true;

// Настройки живут в куче и намеренно не удаляются: логировать могут статические
// деструкторы, и порядок уничтожения глобалов не должен приводить к обращению
// к мёртвому объекту.
AdbLogSettings& LogSettingsLocked() {
    static AdbLogSettings* settings = [] {
        auto* s = new AdbLogSettings();
        s->enabled = adb_log_default_enabled();
        return s;
    }();
    return *settings;
}

// Возвращает логгер или nullptr, если лог выключен. Вызывать под g_log_mutex.
std::shared_ptr<spdlog::logger> GetFileLoggerLocked() {
    AdbLogSettings& s = LogSettingsLocked();
    if (!s.enabled) return nullptr;
    if (g_logger && !g_logger_dirty) return g_logger;

    try {
        auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                s.file_path, s.max_file_size, s.max_files);
        auto logger = std::make_shared<spdlog::logger>("adb_file_logger", std::move(sink));
        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] %v");
        logger->set_level(spdlog::level::trace);
        logger->flush_on(spdlog::level::debug);
        g_logger = std::move(logger);
        g_logger_dirty = false;
    } catch (const std::exception& e) {
        // Путь недоступен (нет прав, нет каталога). Процесс не роняем:
        // выключаем лог и работаем дальше.
        fprintf(stderr, "adb: cannot open log file %s: %s\n", s.file_path.c_str(), e.what());
        s.enabled = false;
        g_logger.reset();
        g_logger_dirty = true;
    }
    return g_logger;
}

}  // namespace

// Слабое определение: adb и adirect пишут в файл, как раньше. libadb.so
// переопределяет этот символ (adb/lib/src/api/globals.cpp) и отключает лог.
__attribute__((weak)) bool adb_log_default_enabled() {
    return true;
}

void adb_log_configure(const AdbLogSettings& settings) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    AdbLogSettings& current = LogSettingsLocked();
    const bool reopen = !settings.enabled || current.file_path != settings.file_path ||
                        current.max_file_size != settings.max_file_size ||
                        current.max_files != settings.max_files;
    current = settings;
    if (reopen) {
        g_logger.reset();
        g_logger_dirty = true;
    }
}

AdbLogSettings adb_log_current_settings() {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    return LogSettingsLocked();
}

void adb_log_open(const char* reason) {
    std::shared_ptr<spdlog::logger> logger;
    {
        std::lock_guard<std::mutex> lock(g_log_mutex);
        logger = GetFileLoggerLocked();
    }
    if (!logger) return;
    logger->info("--- log opened: {} ---", reason ? reason : "");
    logger->flush();
}

void adb_log_flush() {

    std::shared_ptr<spdlog::logger> logger;
    {
        std::lock_guard<std::mutex> lock(g_log_mutex);
        logger = g_logger;
    }
    if (logger) logger->flush();
}

// 3. Приёмник сообщений libbase: LOG()/VLOG() -> файловый лог.
void AdbLogger(android::base::LogId id, android::base::LogSeverity severity,
               const char* tag, const char* file, unsigned int line,
               const char* message) {
    std::shared_ptr<spdlog::logger> logger;
    bool also_stderr = false;
    {
        std::lock_guard<std::mutex> lock(g_log_mutex);
        logger = GetFileLoggerLocked();
        also_stderr = LogSettingsLocked().also_stderr;
    }

    if (also_stderr) {
        android::base::StderrLogger(id, severity, tag, file, line, message);
    }
    if (!logger) return;

    logger->log(MapSeverity(severity), "[{}:{}] [{}] {}", file ? file : "unknown", line,
                tag ? tag : "GLOBAL", message ? message : "");
}


const char* adb_device_banner = "host";

// void AdbLogger(android::base::LogId id, android::base::LogSeverity severity,
//                const char* tag, const char* file, unsigned int line,
//                const char* message) {
//     android::base::StderrLogger(id, severity, tag, file, line, message);
// }

int adb_trace_mask;

std::string get_trace_setting() {
    const char* setting = getenv("ADB_TRACE");
    if (setting == nullptr) {
        setting = "";
    }
    return setting;
}

// Split the space separated list of tags from the trace setting and build the
// trace mask from it. note that '1' and 'all' are special cases to enable all
// tracing.
//
// adb's trace setting comes from the ADB_TRACE environment variable, whereas
// adbd's comes from the system property persist.adb.trace_mask.
static void setup_trace_mask() {
    const std::string trace_setting = get_trace_setting();
    if (trace_setting.empty()) {
        return;
    }

    std::unordered_map<std::string, int> trace_flags = {{"1", -1},
                                                        {"all", -1},
                                                        {"adb", ADB},
                                                        {"sockets", SOCKETS},
                                                        {"packets", PACKETS},
                                                        {"rwx", RWX},
                                                        {"usb", USB},
                                                        {"sync", SYNC},
                                                        {"sysdeps", SYSDEPS},
                                                        {"transport", TRANSPORT},
                                                        {"jdwp", JDWP},
                                                        {"services", SERVICES},
                                                        {"auth", AUTH},
                                                        {"fdevent", FDEVENT},
                                                        {"shell", SHELL},
                                                        {"incremental", INCREMENTAL}};

    std::vector<std::string> elements = android::base::Split(trace_setting, ", ");
    for (const auto& elem : elements) {
        const auto& flag = trace_flags.find(elem);
        if (flag == trace_flags.end()) {
            LOG(ERROR) << "Unknown trace flag: " << elem;
            continue;
        }

        if (flag->second == -1) {
            // -1 is used for the special values "1" and "all" that enable all
            // tracing.
            adb_trace_mask = ~0;
            break;
        } else {
            adb_trace_mask |= 1 << flag->second;
        }
    }

    if (adb_trace_mask != 0) {
        android::base::SetMinimumLogSeverity(android::base::VERBOSE);
    }
}

void adb_trace_init(char** argv) {


    // adb historically ignored $ANDROID_LOG_TAGS but passed it through to logcat.
    // If set, move it out of the way so that libbase logging doesn't try to parse it.
    std::string log_tags;
    char* ANDROID_LOG_TAGS = getenv("ANDROID_LOG_TAGS");
    if (ANDROID_LOG_TAGS) {
        log_tags = ANDROID_LOG_TAGS;
        unsetenv("ANDROID_LOG_TAGS");
    }

    android::base::InitLogging(argv, &AdbLogger);
    // Ensure LOG(INFO) messages pass through libbase filter to AdbLogger
    android::base::SetMinimumLogSeverity(android::base::INFO);
    if (!log_tags.empty()) setenv("ANDROID_LOG_TAGS", log_tags.c_str(), 1);
    setup_trace_mask();
    VLOG(ADB) << adb_version();
}

void adb_trace_enable(AdbTrace trace_tag) {
    adb_trace_mask |= (1 << trace_tag);
}
