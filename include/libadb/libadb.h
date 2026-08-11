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
// Таймауты (§6)
// ---------------------------------------------------------------------------

// Передача данных: общий дедлайн и/или отсутствие прогресса.
// Срабатывает тот, который наступит раньше; оба нулевые — без таймаута.
// По умолчанию активен только stall: абсолютное время передачи на одни и те же
// устройства плавает в разы из-за топологии сети, поэтому жёсткий total даёт
// ложные срывы, а stall ловит именно «канал умер».
struct TransferTimeout {
    ms total{0};       // дедлайн на всю передачу; 0 = не ограничивать
    ms stall{30000};   // нет прогресса дольше этого; 0 = не ограничивать
};

// Как проверять, что устройство живо, пока ждём ответа на install-commit.
enum class HealthCheckMode {
    None = 0,   // не проверять
    Transport,  // (дефолт) дешёвая проверка живости соединения/adbd
    Shell,      // короткая служебная shell-команда — строже, но лишний процесс
};

LIBADB_API const char* to_string(HealthCheckMode);

// Установка неоднородна по наблюдаемости, поэтому таймаут у каждой фазы свой.
struct InstallTimeout {
    ms prepare{60000};          // распаковка .apks, чтение локальных файлов
    ms create_session{30000};   // pm install-create
    TransferTimeout transfer;   // pm install-write: байты идут на устройство

    // Ожидание ответа pm после коммита: прогресса нет, только общий дедлайн.
    // Значение щедрое — dexopt большого приложения на слабом устройстве это минуты.
    ms commit{15 * 60000};

    // Пока ждём commit, проверяем, что устройство живо. На каждую успешную
    // проверку летит событие OperationHeartbeat.
    HealthCheckMode commit_healthcheck = HealthCheckMode::Transport;
    ms commit_healthcheck_interval{10000};  // 0 = не проверять
    int commit_healthcheck_failures = 3;    // сколько подряд считать потерей устройства
};

struct Timeouts {
    ms connect{15000};       // до состояния "device", включая авторизацию
    ms slot_acquire{30000};  // ожидание свободного слота подключения (§7);
                             // 0 = не ждать вовсе, ms::max() = ждать бесконечно
    ms shell{60000};         // обычная shell-команда
    TransferTimeout push;
    TransferTimeout pull;
    InstallTimeout install;
    ms uninstall{120000};
    ms close{5000};          // грациозное закрытие соединения
};

// ---------------------------------------------------------------------------
// Опции отдельных операций
// ---------------------------------------------------------------------------

// У каждой операции таймаут можно переопределить точечно: std::nullopt —
// взять из Client::timeouts().

struct PushOptions {
    Compression compression = Compression::Any;
    bool sync_only_newer = false;  // как `adb push --sync`: пропускать совпадающие файлы
    ProgressFn on_progress;
    StartedFn on_start;            // получить OperationId до начала работы
    std::optional<TransferTimeout> timeout;
};

struct PullOptions {
    Compression compression = Compression::None;
    ProgressFn on_progress;
    StartedFn on_start;
    std::optional<TransferTimeout> timeout;
};

struct ShellOptions {
    bool capture_output = true;  // складывать вывод в Result::output
    OutputFn on_output;          // и/или отдавать его потоком
    StartedFn on_start;
    std::optional<ms> timeout;   // 0 = ждать бесконечно
};

// Что именно ставим (§10).
enum class InstallKind {
    // 1 apk -> Single; N apk -> SplitSet; *.apks/*.zip -> Bundle.
    Auto = 0,
    Single,
    SplitSet,      // несколько частей ОДНОГО пакета (base + split_config.*)
    Bundle,        // .apks (bundletool): распаковать и поставить как SplitSet
    MultiPackage,  // несколько независимых пакетов в одной атомарной сессии
};

LIBADB_API const char* to_string(InstallKind);

// Что делать, если пакет уже установлен и подписан другим ключом.
enum class ConflictPolicy {
    Fail = 0,           // (дефолт) вернуть ошибку с кодом pm
    Reinstall,          // uninstall + install заново (данные приложения теряются)
    ReinstallKeepData,  // ЗАРЕЗЕРВИРОВАНО: возвращает Status::NotImplemented
};

LIBADB_API const char* to_string(ConflictPolicy);

// Откуда брать имя пакета, если оно нужно (для ConflictPolicy != Fail).
enum class PackageNameSource {
    Explicit = 0,  // (дефолт) только InstallOptions::package_name
    Auto,          // определять автоматически (ошибка pm, затем AndroidManifest.xml)
    Both,          // задано явно — использовать; иначе определять
};

LIBADB_API const char* to_string(PackageNameSource);

struct InstallOptions {
    bool reinstall = true;             // -r
    bool allow_downgrade = false;      // -d
    bool grant_permissions = false;    // -g
    std::vector<std::string> extra_args;  // всё остальное, дословно для `pm`

    // --- §10 ---
    InstallKind kind = InstallKind::Auto;
    ConflictPolicy on_conflict = ConflictPolicy::Fail;
    PackageNameSource package_name_source = PackageNameSource::Explicit;

    // Имя пакета: нужно для on_conflict != Fail. При Explicit и пустом значении
    // конфликт подписи вернёт Status::InvalidArgument (переустанавливать
    // «что-нибудь» библиотека не станет).
    std::string package_name;

    // При INSTALL_FAILED_VERSION_DOWNGRADE добавить -d и повторить один раз.
    bool allow_downgrade_retry = false;

    // Сахар над `--user N`; -1 — не указывать.
    int user_id = -1;

    ProgressFn on_progress;            // прогресс заливки apk
    OutputFn on_output;                // сырой вывод pm
    StartedFn on_start;
    std::optional<InstallTimeout> timeout;
};

struct UninstallOptions {
    bool keep_data = false;  // -k
    int user_id = -1;        // сахар над `--user N`; -1 — не указывать
    StartedFn on_start;
    std::optional<ms> timeout;
};

// ---------------------------------------------------------------------------
// Авторизация (§5)
// ---------------------------------------------------------------------------

// Устройство пускает к себе только по ключу, отпечаток которого подтвердили на
// экране. Ключи можно брать из файлов, из памяти (текстом PEM) или сгенерировать
// эфемерный — библиотека не навязывает ~/.android/adbkey.
struct AuthOptions {
    // Приватные ключи из файлов (как ~/.android/adbkey).
    // Порядок = порядок попыток.
    std::vector<std::string> key_files;

    // Приватные ключи текстом: PEM (`BEGIN RSA PRIVATE KEY`) или PKCS#8
    // (`BEGIN PRIVATE KEY`). Вызывающий берёт их откуда угодно — из своей БД,
    // из vault, из зашитого ресурса. Публичный ключ выводится из приватного.
    std::vector<std::string> private_keys_pem;

    // Использовать ли стандартный набор (~/.android/adbkey, $ADB_VENDOR_KEYS).
    // false удобно сервису под своим пользователем: домашнего каталога может не
    // быть вовсе.
    bool use_default_key_store = true;

    // Если ни одного ключа нет — сгенерировать эфемерный (только в памяти).
    bool generate_ephemeral_if_empty = true;

    // Куда записать сгенерированный ключ (пусто = не записывать).
    // Рядом создаётся файл `<путь>.pub` с публичным ключом.
    std::string save_generated_key_to;
};

// Отпечатки (SHA-256, hex) загруженных приватных ключей — для диагностики
// «каким ключом мы вообще стучимся». Заполняется после Client::initialize().
LIBADB_API std::vector<std::string> auth_key_fingerprints();

// ---------------------------------------------------------------------------
// Настройки клиента
// ---------------------------------------------------------------------------

struct Options {
    uint16_t default_port = 5555;   // подставляется, если в адресе нет порта
    LogOptions log;                 // канал 1
    LogSink log_sink;               // канал 2

    // Авторизация (§5). Разбирается в Client::initialize(): при ошибке PEM
    // возвращается Status::InvalidArgument с указанием проблемного ключа.
    AuthOptions auth;

    // Все таймауты одним местом (§6). До этапа 7 connect_timeout и slot_acquire
    // жили прямо в Options; теперь это timeouts.connect и timeouts.slot_acquire.
    Timeouts timeouts;

    // Ограничение параллелизма для групповых операций Client::for_each и *_all.
    // 0 — без ограничения (поток на устройство).
    size_t max_parallel = 0;

    // Глобальный на процесс лимит одновременных подключений (§7). Общий для
    // батч-режима и ручных connect(): библиотека никогда не держит открытыми
    // больше max_connections устройств. 0 — без ограничения.
    size_t max_connections = 0;

    // Отдельный пул для *_async (§9). Важное следствие, о котором надо помнить
    // при смешанном использовании: число одновременно открытых подключений
    // складывается из обоих пулов, а лимитом сверху остаётся max_connections.
    size_t async_worker_threads = 4;

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
// Асинхронные операции (§9)
// ---------------------------------------------------------------------------

// Детали реализации: фабрики, создающие Operation/BatchOperation (конструкторы
// приватные). Определены внутри библиотеки, приложению не нужны.
namespace internal {
struct OperationFactory;
}

// Хэндл асинхронной операции. Живёт независимо от Device: если вызывающий
// отпустил Device, операция всё равно доработает (Device держится изнутри).
class LIBADB_API Operation {
  public:
    ~Operation();
    Operation(const Operation&) = delete;
    Operation& operator=(const Operation&) = delete;

    // 0, пока воркер не начал работу (идентификатор выдаётся в момент старта);
    // после этого — тот же id, что приходит в событиях и в on_start.
    OperationId id() const;
    Command command() const;
    const std::string& serial() const;

    bool done() const;
    Phase phase() const;

    // timeout == 0 — ждать бесконечно. false — не дождались.
    bool wait(ms timeout = ms{0}) const;

    // Валиден после done(); до этого — пустой Result со Status::Ok.
    Result result() const;

    // Идемпотентно, из любого потока.
    void cancel();
    bool canceled() const;

    struct Impl;

  private:
    friend struct internal::OperationFactory;
    explicit Operation(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
using OperationPtr = std::shared_ptr<Operation>;

// Групповая асинхронная операция: набор Operation по списку адресов.
class LIBADB_API BatchOperation {
  public:
    ~BatchOperation();
    BatchOperation(const BatchOperation&) = delete;
    BatchOperation& operator=(const BatchOperation&) = delete;

    bool wait(ms timeout = ms{0}) const;
    bool done() const;
    size_t total() const;
    size_t finished() const;

    // Готовые на момент вызова результаты; ключ — адрес из входного списка.
    std::map<std::string, Result> results() const;

    // Для точечной отмены.
    std::vector<OperationPtr> operations() const;

    void cancel();

    struct Impl;

  private:
    friend struct internal::OperationFactory;
    explicit BatchOperation(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
using BatchOperationPtr = std::shared_ptr<BatchOperation>;

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
// enable_shared_from_this: асинхронная операция должна продлевать жизнь
// устройства, иначе вызывающий, отпустивший DevicePtr, снёс бы объект
// из-под работающего воркера.
class LIBADB_API Device : public std::enable_shared_from_this<Device> {
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

    // Несколько файлов: части одного пакета (SplitSet), содержимое .apks
    // (Bundle) или независимые пакеты одной атомарной сессией (MultiPackage).
    // Что именно — определяет InstallOptions::kind (по умолчанию Auto, §10).
    Result install(const std::vector<std::string>& paths, const InstallOptions& options = {});

    Result uninstall(const std::string& package, const UninstallOptions& options = {});

    // --- асинхронный режим (§9) ---
    //
    // Работа уходит в отдельный пул библиотеки (Options::async_worker_threads).
    // Второй *_async на занятом устройстве вернёт операцию, уже завершённую со
    // Status::DeviceBusy: последовательность команд на одном устройстве —
    // забота вызывающего.
    OperationPtr push_async(const std::string& local, const std::string& remote,
                            const PushOptions& options = {});
    OperationPtr pull_async(const std::string& remote, const std::string& local,
                            const PullOptions& options = {});
    OperationPtr shell_async(const std::string& command, const ShellOptions& options = {});
    OperationPtr install_async(const std::string& apk_path, const InstallOptions& options = {});
    OperationPtr install_async(const std::vector<std::string>& paths,
                               const InstallOptions& options = {});
    OperationPtr uninstall_async(const std::string& package, const UninstallOptions& options = {});

    // Занято ли устройство асинхронной операцией прямо сейчас.
    bool busy() const;

    // Отменяет операции этого устройства (и синхронные, и асинхронные).
    // Возвращает число помеченных операций.
    size_t cancel_current();

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

    // Таймауты в рантайме (§6). Действуют на операции, начатые после вызова;
    // уже идущие продолжают жить со своими значениями.
    void set_timeouts(const Timeouts& timeouts);
    Timeouts timeouts() const;

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

    // --- асинхронные групповые операции (§9) ---

    BatchOperationPtr push_all_async(const std::vector<std::string>& addresses,
                                     const std::string& local, const std::string& remote,
                                     const PushOptions& options = {});
    BatchOperationPtr pull_all_async(const std::vector<std::string>& addresses,
                                     const std::string& remote, const std::string& local_dir,
                                     const PullOptions& options = {});
    BatchOperationPtr shell_all_async(const std::vector<std::string>& addresses,
                                      const std::string& command,
                                      const ShellOptions& options = {});
    BatchOperationPtr install_all_async(const std::vector<std::string>& addresses,
                                        const std::string& apk_path,
                                        const InstallOptions& options = {});
    BatchOperationPtr uninstall_all_async(const std::vector<std::string>& addresses,
                                          const std::string& package,
                                          const UninstallOptions& options = {});

    // --- отмена (§9) ---

    // Отменяет операцию по идентификатору (полученному через on_start или
    // событие OperationStarted). true — операция найдена и помечена.
    // Завершение — в пределах сотен миллисекунд, «мгновенно» не обещаем.
    bool cancel(OperationId id);

    // Отменяет все текущие операции; соединения остаются открытыми.
    // Возвращает число помеченных операций.
    size_t cancel_all();

    // Сколько операций выполняется прямо сейчас (синхронных и асинхронных).
    size_t active_operations() const;

    // Закрывает все подключения; клиент остаётся работоспособным. Текущие
    // операции завершаются со Status::ConnectionClosed — это отдельный код,
    // отличимый от Canceled («я сам отменил»).
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


