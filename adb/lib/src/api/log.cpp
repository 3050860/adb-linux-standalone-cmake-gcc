// libadb: логирование — внутренний файловый лог и LogSink приложения.
#include <mutex>
#include <shared_mutex>

#include <android-base/logging.h>

#include "adb_trace.h"
#include "api/internal.h"
#include "libadb/libadb.h"

namespace libadb {
namespace {

std::shared_mutex g_mutex;
LogOptions g_options;   // дефолты из структуры: enabled = false
LogSink g_sink;
LogLevel g_sink_level = LogLevel::Trace;

android::base::LogSeverity to_severity(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return android::base::VERBOSE;
        case LogLevel::Debug: return android::base::DEBUG;
        case LogLevel::Info:  return android::base::INFO;
        case LogLevel::Warn:  return android::base::WARNING;
        case LogLevel::Error: return android::base::ERROR;
        case LogLevel::Off:   return android::base::FATAL;
    }
    return android::base::WARNING;
}

// ADB_TRACE — аварийный override: если переменная задана, лог включается даже
// при enabled = false (см. §4 спецификации).
bool trace_env_set() {
    const char* v = getenv("ADB_TRACE");
    return v != nullptr && *v != '\0';
}

// Переносит LogOptions во внутренний слой. Вызывать без удержания g_mutex
// на запись не обязательно — adb_log_configure имеет свой лок.
void apply_locked(const LogOptions& options) {
    const bool force = trace_env_set();

    AdbLogSettings s;
    s.enabled = options.enabled || force;
    s.file_path = options.file_path;
    s.max_file_size = options.max_file_size;
    s.max_files = options.max_files;
    s.also_stderr = options.also_stderr;
    s.min_severity = static_cast<int>(to_severity(options.level));
    adb_log_configure(s);

    internal::ensure_logging_initialized();

    // Фильтр libbase: при выключенном логе поднимаем порог до FATAL, чтобы
    // внутренние LOG()/VLOG() не тратили время на форматирование строк.
    if (!s.enabled) {
        android::base::SetMinimumLogSeverity(android::base::FATAL);
    } else if (force) {
        android::base::SetMinimumLogSeverity(android::base::VERBOSE);
    } else {
        android::base::SetMinimumLogSeverity(to_severity(options.level));
    }

    // Теговая трассировка: "all" | "sync,transport" — как ADB_TRACE.
    if (!options.trace_tags.empty()) {
        adb_trace_mask = 0;
        if (options.trace_tags == "all" || options.trace_tags == "1") {
            adb_trace_mask = ~0;
        } else {
            static const struct {
                const char* name;
                AdbTrace tag;
            } kTags[] = {
                {"adb", ADB},           {"sockets", SOCKETS},   {"packets", PACKETS},
                {"rwx", RWX},           {"usb", USB},           {"sync", SYNC},
                {"sysdeps", SYSDEPS},   {"transport", TRANSPORT}, {"jdwp", JDWP},
                {"services", SERVICES}, {"auth", AUTH},         {"fdevent", FDEVENT},
                {"shell", SHELL},       {"incremental", INCREMENTAL},
            };
            size_t start = 0;
            const std::string& t = options.trace_tags;
            while (start <= t.size()) {
                size_t end = t.find_first_of(", ", start);
                if (end == std::string::npos) end = t.size();
                const std::string name = t.substr(start, end - start);
                if (!name.empty()) {
                    for (const auto& kt : kTags) {
                        if (name == kt.name) {
                            adb_trace_mask |= 1 << kt.tag;
                            break;
                        }
                    }
                }
                start = end + 1;
            }
        }
    }

    // Включение лога сразу видно на диске — не ждём первого LOG().
    if (s.enabled) {
        adb_log_open(force ? "ADB_TRACE" : "libadb::set_log_options");
    }
}


}  // namespace

const char* to_string(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "trace";
        case LogLevel::Debug: return "debug";
        case LogLevel::Info:  return "info";
        case LogLevel::Warn:  return "warn";
        case LogLevel::Error: return "error";
        case LogLevel::Off:   return "off";
    }
    return "unknown";
}

void set_log_options(const LogOptions& options) {
    std::unique_lock lock(g_mutex);
    g_options = options;
    apply_locked(g_options);
}

LogOptions log_options() {
    std::shared_lock lock(g_mutex);
    return g_options;
}

void set_log_level(LogLevel level) {
    std::unique_lock lock(g_mutex);
    g_options.level = level;
    apply_locked(g_options);
}

void set_log_sink(LogSink sink) {
    std::unique_lock lock(g_mutex);
    g_sink = std::move(sink);
}

void flush_log() {
    adb_log_flush();
}

void log(LogLevel level, const std::string& serial, std::string_view message) {
    internal::emit_log(level, serial, message);
}

namespace internal {

void ensure_logging_initialized() {
    static std::once_flag once;
    std::call_once(once, [] {
        // libbase парсит $ANDROID_LOG_TAGS; adb исторически его игнорирует и
        // лишь передаёт дальше в logcat, поэтому убираем на время инициализации.
        std::string log_tags;
        if (const char* v = getenv("ANDROID_LOG_TAGS")) {
            log_tags = v;
            unsetenv("ANDROID_LOG_TAGS");
        }
        const char* argv[] = {"libadb", nullptr};
        android::base::InitLogging(const_cast<char**>(argv), &AdbLogger);
        if (!log_tags.empty()) setenv("ANDROID_LOG_TAGS", log_tags.c_str(), 1);

        // По умолчанию лог выключен — не форматируем ничего лишнего.
        android::base::SetMinimumLogSeverity(android::base::FATAL);
    });
}

bool log_sink_wants(LogLevel level) {
    if (level == LogLevel::Off) return false;
    std::shared_lock lock(g_mutex);
    return static_cast<bool>(g_sink) && level >= g_sink_level;
}

void emit_log(LogLevel level, const std::string& serial, std::string_view message) {
    if (level == LogLevel::Off) return;
    LogSink sink;
    {
        std::shared_lock lock(g_mutex);
        if (!g_sink || level < g_sink_level) return;
        sink = g_sink;  // копия: вызываем без удержания лока
    }
    sink(level, serial, message);
}

}  // namespace internal

namespace {

// ADB_TRACE как аварийный override должен работать без единого вызова API:
// применяем его при загрузке библиотеки. Если переменная не задана, ничего
// не происходит и файл не создаётся.
struct TraceEnvBootstrap {
    TraceEnvBootstrap() {
        if (!trace_env_set()) return;
        LogOptions o;
        o.enabled = true;
        o.level = LogLevel::Trace;
        o.trace_tags = getenv("ADB_TRACE");
        set_log_options(o);
    }
};

// Объявлен последним в TU: g_mutex/g_options/g_sink к этому моменту
// уже инициализированы (порядок внутри единицы трансляции гарантирован).
const TraceEnvBootstrap g_trace_env_bootstrap;

}  // namespace
}  // namespace libadb

