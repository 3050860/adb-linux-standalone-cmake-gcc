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

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>



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

// ---------------------------------------------------------------------------
// Колбэки операций
// ---------------------------------------------------------------------------

using ms = std::chrono::milliseconds;

// total == 0 означает «размер неизвестен».
using ProgressFn =
    std::function<void(const std::string& serial, uint64_t done, uint64_t total)>;

// Поток вывода shell/install по мере поступления.
using OutputFn =
    std::function<void(const std::string& serial, std::string_view chunk, bool is_stderr)>;

// Вызывается ДО начала работы операции и отдаёт её идентификатор: позволяет
// отменить синхронный вызов из другого потока (§9).
using StartedFn = std::function<void(OperationId id, const std::string& serial)>;

// ---------------------------------------------------------------------------
// Результат операции
// ---------------------------------------------------------------------------

// Сжатие полезной нагрузки при push/pull.
// Any — выбрать лучшее из поддерживаемого устройством (обычно zstd/lz4).
enum class Compression { None = 0, Any, Brotli, Lz4, Zstd };

struct TransferStats {
    uint64_t bytes = 0;          // передано полезных байт
    uint64_t bytes_on_wire = 0;  // сколько реально ушло в сеть (после сжатия)
    ms duration{0};              // время фазы передачи
    double mib_per_sec = 0.0;    // средняя скорость
};

struct Result {
    Status status = Status::Ok;
    int exit_code = 0;         // shell / install / uninstall
    std::string error;         // человекочитаемая причина
    std::string remote_code;   // "INSTALL_FAILED_UPDATE_INCOMPATIBLE" и т.п.
    std::string output;        // сырой вывод, если запрошен capture_output
    Phase phase = Phase::None; // где закончили или сломались
    int retries = 0;           // сколько было повторов
    ms duration{0};            // полное время операции
    TransferStats transfer;    // заполняется для push/pull

    bool ok() const { return status == Status::Ok; }
    explicit operator bool() const { return ok(); }
};

// ---------------------------------------------------------------------------
// События (§8)
// ---------------------------------------------------------------------------

// ВНИМАНИЕ: порядок значений — часть ABI (по нему строится EventMask).
// Новые типы добавлять только в конец.
enum class EventType {
    DeviceConnecting = 0,
    DeviceConnected,
    DeviceAuthRequired,
    DeviceUnauthorized,
    DeviceDisconnected,
    DeviceLost,

    SlotWaiting,
    SlotAcquired,
    SlotTimeout,

    OperationStarted,
    OperationPhaseChanged,
    OperationProgress,
    OperationHeartbeat,
    OperationRetry,
    OperationOutput,
    OperationFinished,
    OperationTimeout,
    OperationCanceled,
    OperationFailed,
    OperationStats,

    ClientShutdown,
    InternalError,
};

LIBADB_API const char* to_string(EventType);

struct Event {
    EventType type = EventType::InternalError;
    ms timestamp{0};        // монотонное время от инициализации клиента
    std::string serial;     // "" для событий уровня клиента
    OperationId op = 0;     // 0, если событие не относится к операции
    Command command = Command::Connect;
    Phase phase = Phase::None;
    Status status = Status::Ok;
    std::string remote_code;
    uint64_t bytes_done = 0;
    uint64_t bytes_total = 0;
    ms elapsed{0};
    std::string message;
    TransferStats stats;    // заполняется для OperationStats/OperationFinished
};

using EventFn = std::function<void(const Event&)>;
using SubscriptionId = uint64_t;

// Битовая маска по EventType: бит N соответствует значению N.
// ~EventMask{0} — все события.
using EventMask = uint64_t;

// Маска из перечисления: event_bit(EventType::OperationProgress) и т.п.
constexpr EventMask event_bit(EventType type) {
    return EventMask{1} << static_cast<unsigned>(type);
}

// ---------------------------------------------------------------------------
// Опции отдельных операций
// ---------------------------------------------------------------------------

struct PushOptions {
    Compression compression = Compression::Any;
    bool sync_only_newer = false;  // как `adb push --sync`: пропускать совпадающие файлы
    ProgressFn on_progress;
    StartedFn on_start;            // получить OperationId до начала работы
};

struct PullOptions {
    Compression compression = Compression::None;
    ProgressFn on_progress;
    StartedFn on_start;
};

struct ShellOptions {
    bool capture_output = true;  // складывать вывод в Result::output
    OutputFn on_output;          // и/или отдавать его потоком
    StartedFn on_start;
};

struct InstallOptions {
    bool reinstall = true;             // -r
    bool allow_downgrade = false;      // -d
    bool grant_permissions = false;    // -g
    std::vector<std::string> extra_args;  // всё остальное, дословно для `pm`
    ProgressFn on_progress;            // прогресс заливки apk
    OutputFn on_output;                // сырой вывод pm
    StartedFn on_start;
};

struct UninstallOptions {
    bool keep_data = false;  // -k
    StartedFn on_start;
};

// ---------------------------------------------------------------------------
// Настройки клиента
// ---------------------------------------------------------------------------

struct Options {
    uint16_t default_port = 5555;   // подставляется, если в адресе нет порта
    ms connect_timeout{15000};      // ожидание перехода устройства в состояние device
    LogOptions log;                 // канал 1
    LogSink log_sink;               // канал 2

    // Ограничение параллелизма для групповых операций Client::for_each и *_all.
    // 0 — без ограничения (поток на устройство).
    size_t max_parallel = 0;

    // Глобальный на процесс лимит одновременных подключений (§7). Общий для
    // батч-режима и ручных connect(): библиотека никогда не держит открытыми
    // больше max_connections устройств. 0 — без ограничения.
    size_t max_connections = 0;

    // Сколько connect() ждёт освобождения слота, если лимит исчерпан:
    // 0 — не ждать вовсе (сразу Status::SlotBusy),
    // ms::max() — ждать бесконечно, иначе — Status::SlotTimeout по истечении.
    ms slot_acquire{30000};

    // --- события (§8) ---

    // Базовый подписчик: то же, что Client::subscribe(on_event, event_mask),
    // только задаётся сразу в initialize(). Повторный initialize() заменяет
    // именно эту подписку, не затрагивая добавленных через subscribe().
    EventFn on_event;
    EventMask event_mask = ~EventMask{0};

    // Минимальный интервал между OperationProgress одной операции.
    // 0 — не троттлить (все события прогресса доставляются).
    ms progress_interval{200};

    // Ограничение очереди диспетчера. При переполнении первыми выбрасываются
    // OperationProgress/OperationHeartbeat, а о потерях приходит InternalError
    // с их количеством. Критичные события не теряются.
    size_t event_queue_limit = 10000;
};


// ---------------------------------------------------------------------------
// Устройство
// ---------------------------------------------------------------------------

class Client;

// Деталь реализации: фабрика, создающая Device (конструктор приватный).
// Определена внутри библиотеки, приложению не нужна.
namespace internal {
struct DeviceFactory;
}

// Живое подключение к одному устройству. Создаётся только через Client.

// Методы блокирующие; объект можно использовать из одного потока за раз.
class LIBADB_API Device {
  public:
    ~Device();
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    // "192.168.1.10:5555"
    const std::string& serial() const;

    // Подключение живо и устройство в состоянии device.
    bool is_online() const;

    Result push(const std::string& local, const std::string& remote,
                const PushOptions& options = {});
    Result pull(const std::string& remote, const std::string& local,
                const PullOptions& options = {});

    // Выполняет команду и ждёт её завершения. exit_code — код возврата команды.
    Result shell(const std::string& command, const ShellOptions& options = {});

    Result install(const std::string& apk_path, const InstallOptions& options = {});
    Result uninstall(const std::string& package, const UninstallOptions& options = {});

    // getprop одним вызовом; std::nullopt, если свойство отсутствует.
    std::optional<std::string> get_prop(const std::string& name);

    // Закрывает подключение. Дальнейшие операции вернут Status::DeviceLost.
    void close();

    // PIMPL: определение живёт внутри библиотеки. Тип объявлен публично, чтобы
    // внутренние помощники могли его называть, но создать Device всё равно
    // может только Client (конструктор приватный).
    struct Impl;

  private:
    friend class Client;
    friend struct internal::DeviceFactory;
    explicit Device(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};


using DevicePtr = std::shared_ptr<Device>;

// ---------------------------------------------------------------------------
// Клиент
// ---------------------------------------------------------------------------

// Единая точка входа. Один экземпляр на процесс: внутренний event loop adb
// глобален, поэтому Client — синглтон.
class LIBADB_API Client {
  public:
    static Client& instance();

    // Запускает event loop и применяет настройки. Повторный вызов только
    // обновляет то, что можно менять на ходу (логи, таймауты, max_parallel).
    Status initialize(const Options& options = {});
    bool initialized() const;
    const Options& options() const;

    // Подключается к устройству и ждёт готовности (Options::connect_timeout).
    // При ошибке возвращает nullptr, а причина попадает в *status (если задан).
    DevicePtr connect(const DeviceAddress& address, Status* status = nullptr);
    DevicePtr connect(const std::string& address, Status* status = nullptr);

    // Групповая обработка: подключается к каждому адресу (не более max_parallel
    // одновременно), вызывает task и закрывает подключение. Блокируется до конца.
    // Если подключиться не удалось, task получает nullptr и статус ошибки.
    void for_each(const std::vector<std::string>& addresses,
                  const std::function<void(const DevicePtr& device, const std::string& address,
                                           Status status)>& task);

    // Те же операции сразу на списке устройств. Ключ результата — адрес из входного
    // списка (не serial), чтобы вызывающий мог сопоставить с тем, что передал.
    std::map<std::string, Result> push_all(const std::vector<std::string>& addresses,
                                           const std::string& local, const std::string& remote,
                                           const PushOptions& options = {});
    std::map<std::string, Result> pull_all(const std::vector<std::string>& addresses,
                                           const std::string& remote, const std::string& local_dir,
                                           const PullOptions& options = {});
    std::map<std::string, Result> shell_all(const std::vector<std::string>& addresses,
                                            const std::string& command,
                                            const ShellOptions& options = {});
    std::map<std::string, Result> install_all(const std::vector<std::string>& addresses,
                                              const std::string& apk_path,
                                              const InstallOptions& options = {});
    std::map<std::string, Result> uninstall_all(const std::vector<std::string>& addresses,
                                                const std::string& package,
                                                const UninstallOptions& options = {});

    // Лимит одновременных подключений в рантайме. Уменьшение не рвёт уже
    // открытые подключения: просто новые слоты не выдаются, пока число
    // занятых не опустится ниже лимита. 0 — снять ограничение.
    void set_max_connections(size_t limit);
    size_t max_connections() const;

    // Сколько слотов занято прямо сейчас (открытые подключения).
    size_t active_connections() const;

    // --- события (§8) ---

    // Подписка на события. Обработчик вызывается из диспетчер-потока
    // библиотеки (не из потока adb и не из потока операции), поэтому его можно
    // писать без оглядки на реентерабельность протокола — но блокировать
    // надолго нельзя: события копятся в очереди.
    // Возвращает идентификатор для unsubscribe(); 0 — sink не задан.
    SubscriptionId subscribe(EventFn handler, EventMask mask = ~EventMask{0});
    void unsubscribe(SubscriptionId id);

    // Троттлинг OperationProgress в рантайме (0 — не троттлить).
    void set_progress_interval(ms interval);
    ms progress_interval() const;

    // Логирование: то же, что свободные функции, но через клиент.
    void set_log_options(const LogOptions& options);

    void set_log_level(LogLevel level);
    void set_log_sink(LogSink sink);

    // Закрывает все подключения; клиент остаётся работоспособным.
    void close_all();

    // Останавливает event loop. После этого initialize() можно вызвать снова.
    void shutdown();

  private:
    Client();
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace libadb


