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

// 2. Функция для получения (и ленивой инициализации) логгера.
// Гарантирует, что логгер создастся только один раз, даже при многопоточном вызове.
static std::shared_ptr<spdlog::logger>& GetFileLogger() {
    static std::shared_ptr<spdlog::logger> logger;
    static std::once_flag init_flag;

    std::call_once(init_flag, []() {
        // Создаем ротируемый sink: макс размер файла 5 МБ, храним 3 старых файла
        auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            LOG_FILE_PATH, 
            1024 * 1024 * 5, // 5 MB
            3                // max files
        );
        
        logger = std::make_shared<spdlog::logger>("adb_file_logger", sink);
        
        // Настраиваем формат вывода. 
        // %Y-%m-%d %H:%M:%S.%e - время, %^%l%$ - цветной уровень (в консоли, в файле будет текст), %v - сообщение
        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] %v");
        
        // Устанавливаем минимальный уровень (например, ловим даже VERBOSE)
        logger->set_level(spdlog::level::trace);
        
        // Автоматически сбрасывать буфер на диск при ошибках и выше
        logger->flush_on(spdlog::level::debug);
    });

    return logger;
}

// 3. Ваша обновленная функция AdbLogger
void AdbLogger(android::base::LogId id, android::base::LogSeverity severity,
               const char* tag, const char* file, unsigned int line,
               const char* message) {
    
    // Получаем наш инициализированный логгер
    auto& logger = GetFileLogger();
    
    // Преобразуем уровень
    auto spd_level = MapSeverity(severity);
    
    // Формируем итоговое сообщение. 
    // Переменная 'message' уже содержит текст от LOG/PLOG. 
    // Мы можем добавить к нему имя файла и строку для удобства отладки.
    std::string formatted_msg = message ? message : "";
    
    // Если вы хотите, чтобы в файле было видно, из какого файла пришла строка:
    logger->log(spd_level, "[{}:{}] [{}] {}", 
                file ? file : "unknown", 
                line, 
                tag ? tag : "GLOBAL", 
                formatted_msg);
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
