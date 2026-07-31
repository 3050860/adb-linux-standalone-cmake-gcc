/*
 * libadb — публичный C++ API.
 *
 * Этот заголовок намеренно не включает ничего из внутренностей adb: только STL.
 * Всё внутреннее состояние скрыто за PIMPL.
 *
 * Спецификация: docs/libadb-api-proposal3.md
 * Состояние: наполняется поэтапно (см. docs/libadb-development-log.md).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>


#include "libadb/version.h"

#define LIBADB_API __attribute__((visibility("default")))

namespace libadb {

// ---------------------------------------------------------------------------
// Версия
// ---------------------------------------------------------------------------

// Версия библиотеки, с которой реально слинковано приложение ("1.0.0").
LIBADB_API const char* version();

// Та же версия числом: major<<16 | minor<<8 | patch.
// Сверка заголовков и .so: (version_number() >> 16) == LIBADB_VERSION_MAJOR.
LIBADB_API uint32_t version_number();

// ---------------------------------------------------------------------------
// Статусы
// ---------------------------------------------------------------------------

// ВНИМАНИЕ: порядок значений — часть ABI. Новые значения добавлять только
// в конец соответствующей группы, не переставляя существующие.
enum class Status {
    Ok = 0,

    // общие
    InvalidArgument,
    NotInitialized,
    NotImplemented,
    Unsupported,
    Internal,

    // подключение
    ConnectFailed,
    ConnectTimeout,
    AuthRequired,
    Unauthorized,
    Offline,
    DeviceLost,
    ConnectionClosed,  // соединения закрыты через close_all()/shutdown()

    // лимит подключений
    SlotBusy,
    SlotTimeout,
    DeviceBusy,

    // выполнение
    CommandTimeout,
    StallTimeout,
    Canceled,
    IoError,
    LocalFileError,
    RemoteError,

    // установка
    SignatureMismatch,
    VersionDowngrade,
    InsufficientStorage,
    InvalidApk,
    MissingSplit,
};

// Команды и фазы выполнения (используются в результатах и событиях).
enum class Command {
    Connect = 0,
    Shell,
    Push,
    Pull,
    Install,
    Uninstall,
    ShellSession,
};

enum class Phase {
    None = 0,
    Connecting,
    Prepare,
    CreateSession,
    Transfer,
    Commit,
    Finalize,
    Abandon,
};

LIBADB_API const char* to_string(Status);
LIBADB_API const char* to_string(Command);
LIBADB_API const char* to_string(Phase);

// Идентификатор операции: выдаётся до начала работы, позволяет отменить
// операцию из другого потока.
using OperationId = uint64_t;

// ---------------------------------------------------------------------------
// Адрес устройства
// ---------------------------------------------------------------------------

// Порт 0 означает «использовать Options::default_port» (по умолчанию 5555).
struct LIBADB_API DeviceAddress {
    std::string host;
    uint16_t port = 0;

    DeviceAddress() = default;
    explicit DeviceAddress(std::string host, uint16_t port = 0);

    // "192.168.1.10" или "192.168.1.10:5555". Возвращает std::nullopt,
    // если строку разобрать не удалось.
    static std::optional<DeviceAddress> parse(std::string_view text);

    // Всегда с портом; если port == 0, подставляется default_port.
    std::string to_string(uint16_t default_port = 5555) const;

    bool empty() const { return host.empty(); }
};

// ---------------------------------------------------------------------------
// Логирование: два независимых канала
// ---------------------------------------------------------------------------

enum class LogLevel { Trace = 0, Debug, Info, Warn, Error, Off };

LIBADB_API const char* to_string(LogLevel);

// Канал 1 — внутренний лог библиотеки (транспорт, sockets, sync, протокол).
// Выключен по умолчанию: файл не открывается и не создаётся.
struct LogOptions {
    bool enabled = false;
    std::string file_path = "/tmp/adb.log";
    LogLevel level = LogLevel::Warn;
    size_t max_file_size = 5 * 1024 * 1024;
    size_t max_files = 3;
    std::string trace_tags;  // "sync,transport" | "all" — как ADB_TRACE
    bool also_stderr = false;
};

// Канал 2 — человекочитаемые сообщения уровня приложения.
// serial пуст, если сообщение не привязано к устройству.
using LogSink =
    std::function<void(LogLevel level, const std::string& serial, std::string_view message)>;

// Настройки глобальные: действуют на процесс целиком, менять можно в любой момент.
// Client (этап 3) будет делегировать свои set_log_* сюда.
LIBADB_API void set_log_options(const LogOptions& options);
LIBADB_API LogOptions log_options();

// Меняет только уровень, не трогая остальные поля.
LIBADB_API void set_log_level(LogLevel level);

// Пустой sink (по умолчанию) — сообщения канала 2 никуда не идут и не форматируются.
LIBADB_API void set_log_sink(LogSink sink);

// Сбрасывает буферы внутреннего лога на диск.
LIBADB_API void flush_log();

// Отправить своё сообщение в LogSink приложения (удобно для единого формата
// логов приложения и библиотеки). Если sink не задан — вызов бесплатный.
LIBADB_API void log(LogLevel level, const std::string& serial, std::string_view message);

}  // namespace libadb

