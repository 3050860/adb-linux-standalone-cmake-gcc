#ifndef __ADB_TRACE_H
#define __ADB_TRACE_H

#include <android-base/logging.h>
#include <android-base/stringprintf.h>

/* IMPORTANT: if you change the following list, don't
 * forget to update the corresponding 'tags' table in
 * the adb_trace_init() function implemented in adb_trace.cpp.
 */
enum AdbTrace {
    ADB = 0, /* 0x001 */
    SOCKETS,
    PACKETS,
    TRANSPORT,
    RWX, /* 0x010 */
    USB,
    SYNC,
    SYSDEPS,
    JDWP, /* 0x100 */
    SERVICES,
    AUTH,
    FDEVENT,
    SHELL,
    INCREMENTAL,
};

#define VLOG_IS_ON(TAG) \
    (true)

#define VLOG(TAG)                 \
        LOG(DEBUG)

// You must define TRACE_TAG before using this macro.
#define D(...) \
    VLOG(ADB) << android::base::StringPrintf(__VA_ARGS__)


extern int adb_trace_mask;
void adb_trace_init(char**);
void adb_trace_enable(AdbTrace trace_tag);

// ---------------------------------------------------------------------------
// Конфигурация файлового лога.
//
// Консольные клиенты (adb, adirect) ничего не настраивают и получают
// историческое поведение: /tmp/adb.log, ротация 5 МБ × 3.
// libadb.so переопределяет adb_log_default_enabled() (см. lib/src/api/globals.cpp)
// и выключает лог по умолчанию: файл не открывается и не создаётся.
// ---------------------------------------------------------------------------

struct AdbLogSettings {
    bool enabled = true;
    std::string file_path = "/tmp/adb.log";
    size_t max_file_size = 5 * 1024 * 1024;
    size_t max_files = 3;
    bool also_stderr = false;
    // Минимальный уровень: android::base::LogSeverity как int
    // (VERBOSE=0 ... FATAL). Значение по умолчанию соответствует VERBOSE,
    // фильтрация выше делается через SetMinimumLogSeverity.
    int min_severity = 0;
};

// Значение enabled по умолчанию. Слабый символ: adb/adirect получают true,
// libadb.so — false.
bool adb_log_default_enabled();

// Применяет настройки. Существующий логгер закрывается, новый файл открывается
// лениво — при первой записи.
void adb_log_configure(const AdbLogSettings& settings);

AdbLogSettings adb_log_current_settings();

// Форсирует открытие файла и пишет строку-маркер. Нужен, чтобы включение лога
// было сразу видно на диске, не дожидаясь первого LOG().
void adb_log_open(const char* reason);

// Сбрасывает буферы на диск (если лог включён).
void adb_log_flush();


// Приёмник сообщений libbase (передаётся в android::base::InitLogging).
void AdbLogger(android::base::LogId id, android::base::LogSeverity severity, const char* tag,
               const char* file, unsigned int line, const char* message);



#endif /* __ADB_TRACE_H */
