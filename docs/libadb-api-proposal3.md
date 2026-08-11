# libadb.so — публичный API и план разработки (версия 3, итоговая)

Документ самодостаточный: содержит полное описание API, правил сборки, версионирования и
план работ. Предыдущие черновики не требуются для чтения.

Цель: на основе кода этого проекта собрать разделяемую библиотеку `libadb.so`, пригодную
для использования из чужого C++ и C кода, с командами `push`, `pull`, `shell`, `install`,
`uninstall`, настраиваемым логированием, лимитом подключений, гибкими таймаутами,
системой событий, синхронным и асинхронным режимами работы.

---

## 1. Правила ведения работ

1. **`adirect` не трогаем.** Существующая утилита и её сборка остаются как есть.
   Аналог на новом API появится отдельным примером `examples/adirect2`.
2. **Каждый этап — отдельный коммит** с сообщением по шаблону `libadb-phase-%d`
   (`libadb-phase-0`, `libadb-phase-1`, …).
3. **Перед коммитом этапа** в конец файла `docs/libadb-development-log.md` дописывается
   подробная запись: что за этап, какие файлы добавлены/изменены, какие решения приняты,
   что проверено, что осталось.
4. **После всех этапов** — отдельный документ по использованию библиотеки
   (`docs/libadb-usage.md`).
5. **Каталог `examples/`** с примерами на C++ и на чистом C, максимально покрывающими API.
   Примеры собираются CMake и работают **без установки библиотеки в систему**
   (rpath `$ORIGIN`). Отдельный пример `adirect2` повторяет функциональность `adirect`.
6. Вся документация — на русском языке, кроме самого кода.

---

## 2. Структура файлов

```
include/libadb/libadb.h            # публичный C++ API
include/libadb/libadb_c.h          # публичный C ABI
include/libadb/spdlog_sink.hpp     # опциональный inline-адаптер под spdlog (не в .so)
include/libadb/version.h.in        # -> build/include/libadb/version.h (configure_file)
adb/lib/src/api/client.cpp         # реализация фасада
adb/lib/src/api/device.cpp
adb/lib/src/api/operation.cpp
adb/lib/src/api/events.cpp
adb/lib/src/api/install.cpp
adb/lib/src/api/shell_session.cpp
adb/lib/src/api/c_api.cpp          # реализация C ABI
adb/lib/src/internal/...            # существующие AdbManager/AdbDevice/... (становятся внутренними)
adb/lib/libadb.map                 # version script (что экспортируем)
adb/lib/libadb.pc.in               # pkg-config
examples/CMakeLists.txt
examples/cpp/...                   # примеры на C++
examples/c/...                     # примеры на C
examples/adirect2/...              # аналог adirect на публичном API
docs/libadb-api-proposal3.md       # этот документ
docs/libadb-development-log.md     # журнал этапов
docs/libadb-usage.md               # руководство по использованию (по итогам работ)
```

Цели сборки:

```
adb_core (STATIC, -fPIC)   — весь существующий код adb + внутренние классы lib
   └── libadb.so (SHARED)  — только фасад + экспорт, -fvisibility=hidden + version script
   └── adirect (как сейчас, из статических объектов; не меняется)
examples/* → линкуются с libadb.so
```

**Важно:** публичные заголовки не включают `adb.h`, `transport.h`, `socket.h`,
`android-base/*` и вообще ничего внутреннего. Только STL и стандартные заголовки.
Всё состояние скрыто за PIMPL.

---

## 3. Базовые типы

```cpp
// include/libadb/libadb.h
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

using ms = std::chrono::milliseconds;

LIBADB_API const char* version();          // "1.0.0"
LIBADB_API uint32_t    version_number();   // major<<16 | minor<<8 | patch

// ---------------------------------------------------------------- статусы
enum class Status {
    Ok = 0,
    InvalidArgument, NotInitialized, NotImplemented, Unsupported, Internal,
    // подключение
    ConnectFailed, ConnectTimeout, AuthRequired, Unauthorized, Offline,
    DeviceLost, ConnectionClosed,      // ConnectionClosed = close_all()/shutdown()
    // лимит подключений
    SlotBusy, SlotTimeout, DeviceBusy,
    // выполнение
    CommandTimeout, StallTimeout, Canceled,
    IoError, LocalFileError, RemoteError,
    // установка
    SignatureMismatch, VersionDowngrade, InsufficientStorage, InvalidApk, MissingSplit,
};
LIBADB_API const char* to_string(Status);

enum class Command { Connect, Shell, Push, Pull, Install, Uninstall, ShellSession };
enum class Phase   { None, Connecting, Prepare, CreateSession, Transfer, Commit,
                     Finalize, Abandon };
LIBADB_API const char* to_string(Command);
LIBADB_API const char* to_string(Phase);

using OperationId = uint64_t;

// ---------------------------------------------------------------- результат
struct TransferStats {
    uint64_t bytes = 0;              // передано полезных байт
    uint64_t bytes_on_wire = 0;      // после сжатия (сколько реально ушло в сеть)
    ms       duration{0};            // время фазы передачи
    double   mib_per_sec = 0.0;      // средняя скорость
    double   peak_mib_per_sec = 0.0;
    ms       stalled_total{0};       // суммарное время без прогресса
};

struct Result {
    Status      status = Status::Ok;
    int         exit_code = 0;       // shell / install / uninstall
    std::string error;               // человекочитаемая причина
    std::string remote_code;         // "INSTALL_FAILED_UPDATE_INCOMPATIBLE" и т.п.
    std::string output;              // сырой вывод (если capture_output)
    Phase       phase = Phase::None; // где закончили/сломались
    int         retries = 0;         // сколько было повторов/переустановок
    ms          duration{0};         // полное время операции
    TransferStats transfer;          // заполняется для push/pull/install

    bool ok() const { return status == Status::Ok; }
    explicit operator bool() const { return ok(); }
};

// ---------------------------------------------------------------- адрес
struct LIBADB_API DeviceAddress {
    std::string host;                // "192.168.151.231"
    uint16_t    port = 0;            // 0 -> Options::default_port (5555)

    DeviceAddress() = default;
    DeviceAddress(std::string host, uint16_t port = 0);
    static std::optional<DeviceAddress> parse(std::string_view s);  // "ip" | "ip:port"
    std::string to_string() const;   // всегда с портом
};

// ---------------------------------------------------------------- колбэки
using ProgressFn = std::function<void(const std::string& serial, uint64_t done,
                                     uint64_t total)>;
using OutputFn   = std::function<void(const std::string& serial, std::string_view chunk,
                                     bool is_stderr)>;
using StartedFn  = std::function<void(OperationId)>;
```

---

## 4. Логирование

Два независимых канала.

```cpp
enum class LogLevel { Trace = 0, Debug, Info, Warn, Error, Off };

// Канал 1: внутренний лог библиотеки (транспорт, sockets, sync, ошибки протокола).
struct LogOptions {
    bool        enabled       = false;            // ВЫКЛЮЧЕН по умолчанию
    std::string file_path     = "/tmp/adb.log";   // дефолт, переопределяется
    LogLevel    level         = LogLevel::Warn;
    size_t      max_file_size = 5 * 1024 * 1024;
    size_t      max_files     = 3;
    std::string trace_tags;                       // "sync,transport" | "all" (как ADB_TRACE)
    bool        also_stderr   = false;
};

// Канал 2: человекочитаемые сообщения уровня приложения.
using LogSink = std::function<void(LogLevel level, const std::string& serial,
                                  std::string_view message)>;
```

Правила:
- `enabled = false` — sink не создаётся, **файл не открывается и не создаётся**,
  внутренние `LOG()/VLOG()` уходят в no-op. Включение в рантайме (`set_log_options`)
  открывает файл лениво.
- Переменная окружения `ADB_TRACE` остаётся аварийным override для отладки: если задана,
  лог включается даже при `enabled = false`.
- `LogLevel` мапится и на внутренний уровень (`SetMinimumLogSeverity`), и на уровень spdlog.
- `LogSink` не обязателен: если задан обработчик событий (§8), а `LogSink` нет, библиотека
  ничего не форматирует; если задан `LogSink`, критичные события дублируются в него текстом.

### spdlog без ABI-связывания

spdlog **не появляется** в публичном интерфейсе (он header-only и версионно-хрупкий:
разные версии у библиотеки и приложения = ODR/ABI-проблемы). Наружу — `std::function`
и C-callback. Удобная интеграция — отдельный header-only адаптер, который компилируется
у вызывающего его собственной версией spdlog:

```cpp
// include/libadb/spdlog_sink.hpp — не часть ABI
#pragma once
#include <memory>
#include <spdlog/spdlog.h>
#include "libadb/libadb.h"

namespace libadb {
inline LogSink make_spdlog_sink(std::shared_ptr<spdlog::logger> logger) {
    return [logger](LogLevel lvl, const std::string& serial, std::string_view msg) {
        auto l = spdlog::level::info;
        switch (lvl) {
            case LogLevel::Trace: l = spdlog::level::trace; break;
            case LogLevel::Debug: l = spdlog::level::debug; break;
            case LogLevel::Info:  l = spdlog::level::info;  break;
            case LogLevel::Warn:  l = spdlog::level::warn;  break;
            case LogLevel::Error: l = spdlog::level::err;   break;
            case LogLevel::Off:   return;
        }
        if (serial.empty()) logger->log(l, "{}", msg);
        else                logger->log(l, "[{}] {}", serial, msg);
    };
}
}  // namespace libadb
```

---

## 5. Авторизация

```cpp
struct AuthOptions {
    // Приватные ключи из файлов (как ~/.android/adbkey). Порядок = порядок попыток.
    std::vector<std::string> key_files;

    // Приватные ключи текстом (PEM, PKCS#8): вызывающий берёт их откуда угодно —
    // из своей БД, из vault, из зашитого ресурса.
    std::vector<std::string> private_keys_pem;

    // Использовать ли стандартный набор (~/.android/adbkey, $ADB_VENDOR_KEYS).
    bool use_default_key_store = true;

    // Если ни одного ключа нет — сгенерировать эфемерный (только в памяти).
    bool generate_ephemeral_if_empty = true;

    // Куда записать сгенерированный ключ (пусто = не записывать).
    std::string save_generated_key_to;
};
```

Реализация: `adb_auth_init()` сейчас умеет только файлы; добавляется загрузка из памяти
(`BIO_new_mem_buf` + `PEM_read_bio_PrivateKey`, публичный ключ выводится из приватного).
Ошибка разбора PEM → `Status::InvalidArgument` из `Client::initialize()` с указанием,
какой именно ключ не разобрался. Если ни один ключ не подошёл — событие
`EventType::DeviceAuthRequired` (нужно подтвердить отпечаток на экране устройства).

---

## 6. Таймауты

### 6.1 Передача данных: общий дедлайн и/или отсутствие прогресса

```cpp
struct TransferTimeout {
    ms total{0};        // дедлайн на всю операцию; 0 = не ограничивать
    ms stall{30'000};   // нет прогресса дольше этого; 0 = не ограничивать
    // Можно задать оба — срабатывает тот, который наступит раньше.
    // Оба нулевые = без таймаута.
};
```

По умолчанию включён только `stall`. Причина: измерения показали, что время передачи
одного и того же файла на одни и те же устройства отличается в 3–4 раза в зависимости от
топологии сети и числа одновременных операций (устройства на общем узком канале делят
полосу линейно). Жёсткий `total` в такой ситуации даёт ложные срывы, а `stall` ловит
именно «канал умер».

### 6.2 Установка: по фазам

`install` неоднороден по наблюдаемости:

| Фаза | Что происходит | Прогресс | Таймаут |
|---|---|---|---|
| `Prepare` | распаковка `.apks`, чтение локальных файлов | локальная работа | `prepare` |
| `CreateSession` | `pm install-create` | быстрый ответ | `create_session` |
| `Transfer` | `pm install-write`: байты идут на устройство | **есть** | `transfer{total,stall}` |
| `Commit` | `pm install-commit`: верификация, dexopt, установка | **нет** | `commit` + health-check |
| `Abandon` | откат сессии при ошибке | быстрый | `create_session` |

```cpp
enum class HealthCheckMode {
    None,       // не проверять
    Transport,  // (дефолт) дешёвая проверка живости соединения/adbd
    Shell,      // короткая служебная shell-команда (`true`) — строже, но лишний процесс
};

struct InstallTimeout {
    ms prepare{60'000};
    ms create_session{30'000};
    TransferTimeout transfer{ .total = ms{0}, .stall = ms{30'000} };

    // Ожидание ответа pm после коммита: прогресса нет, только общий дедлайн.
    // Значение щедрое — dexopt большого приложения на слабом устройстве это минуты.
    ms commit{15 * 60'000};

    // Пока ждём commit, проверяем, что устройство живо.
    HealthCheckMode commit_healthcheck = HealthCheckMode::Transport;
    ms  commit_healthcheck_interval{10'000};   // 0 = не проверять
    int commit_healthcheck_failures = 3;       // сколько подряд считать потерей устройства
};
```

Идея: **не угадывать, сколько устройство будет ставить пакет, а отличать «долго ставится»
от «устройство умерло»**. Долго — ждём до `commit`. Умерло — обрываем сразу со
`Status::DeviceLost`. На каждую успешную проверку летит событие `OperationHeartbeat`
(«живое, ещё ставит»). Разница режимов, которую надо описать в руководстве:
`Transport` ничего не запускает на устройстве и почти бесплатен, но не отличит ситуацию
«adbd жив, а система в дедлоке»; `Shell` это отличит, но создаёт процесс на устройстве
на каждую проверку (на слабых устройствах во время dexopt это заметно) и может сам
ложно сорваться по своему таймауту.

### 6.3 Сводка

```cpp
struct Timeouts {
    ms connect{15'000};        // до состояния "device", включая авторизацию
    ms slot_acquire{30'000};   // ожидание свободного слота подключения (§7);
                               // 0 = не ждать вовсе, ms::max() = ждать бесконечно
    ms shell{60'000};          // обычная shell-команда (для ShellSession см. §10)
    TransferTimeout push{ .total = ms{0}, .stall = ms{30'000} };
    TransferTimeout pull{ .total = ms{0}, .stall = ms{30'000} };
    InstallTimeout  install{};
    ms uninstall{120'000};
    ms close{5'000};           // грациозное закрытие соединения
};
```

Любую операцию можно переопределить точечно через `*Options::timeout`.

---

## 7. Подключения, слоты и два режима работы

Списки устройств в файлах библиотеку не касаются — это дело вызывающего кода.

### Режим 1 — команда на списке адресов (батч)

```cpp
std::vector<DeviceAddress> devices = { {"192.168.151.231"}, {"192.168.151.232", 5555} };
auto results = client.push_all(devices, {"main.apk"}, "/data/local/tmp/main.apk");
auto batch   = client.push_all_async(devices, {"main.apk"}, "/data/local/tmp/main.apk");
```

Параллелизм ограничен `max_connections`; подключение к очередному адресу — только после
освобождения слота (то есть библиотека никогда не держит больше N соединений).

### Режим 2 — одно устройство из своего потока

```cpp
Status st;
auto dev = client.connect({"192.168.151.231"}, &st);
if (!dev) {
    if (st == Status::SlotTimeout) { /* все слоты заняты — повторить позже */ }
    return;
}
auto r = dev->shell("getprop ro.product.model");
dev->close();   // либо просто выход DevicePtr из области видимости
```

Правила слотов:
- `Options::max_connections` — **глобальный** лимит на процесс (`0` = без ограничения),
  общий для батч-режима и ручных `connect()`.
- Слот занимается в `connect()`, освобождается при `close()` / разрушении `Device`.
- Нет свободных слотов → `connect()` ждёт **не дольше `Timeouts::slot_acquire`**
  и возвращает `nullptr` + `Status::SlotTimeout`. `slot_acquire = 0` → не ждать
  (сразу `Status::SlotBusy`), `ms::max()` → ждать бесконечно. Дефолт 30 с, то есть
  «повиснуть навсегда» по умолчанию невозможно.
- События `SlotWaiting` / `SlotAcquired` / `SlotTimeout` показывают, что упёрлись в лимит.
- `set_max_connections()` в рантайме: уменьшение не рвёт существующие подключения,
  просто новые слоты не выдаются, пока число не опустится ниже лимита.

**Пул асинхронных операций отдельный** (`Options::async_worker_threads`). Это осознанное
решение: долгий `*_async` не должен занимать слоты батча. Следствие, которое обязательно
документируем в руководстве: если приложение использует и синхронное, и асинхронное API,
**фактическое число одновременных подключений к устройствам может складываться из обоих
пулов** — планируйте лимиты с учётом этого.

---

## 8. События

Машинно-читаемые уведомления обо всём, включая таймауты и ошибки.

```cpp
enum class EventType {
    DeviceConnecting, DeviceConnected, DeviceAuthRequired, DeviceUnauthorized,
    DeviceDisconnected, DeviceLost,
    SlotWaiting, SlotAcquired, SlotTimeout,
    OperationStarted, OperationPhaseChanged, OperationProgress, OperationHeartbeat,
    OperationRetry, OperationOutput, OperationFinished, OperationTimeout,
    OperationCanceled, OperationFailed,
    OperationStats,                     // итоговая статистика (скорость и т.п.)
    ClientShutdown, InternalError,
};

struct Event {
    EventType   type;
    ms          timestamp;              // монотонное время от старта клиента
    std::string serial;                 // "" для событий уровня клиента
    OperationId op = 0;                 // 0, если не относится к операции
    Command     command = Command::Connect;
    Phase       phase = Phase::None;
    Status      status = Status::Ok;
    std::string remote_code;
    uint64_t    bytes_done = 0;
    uint64_t    bytes_total = 0;
    ms          elapsed{0};
    std::string message;
    TransferStats stats;                // заполняется для OperationStats/Finished
};

using EventFn = std::function<void(const Event&)>;
using SubscriptionId = uint64_t;
using EventMask = uint64_t;             // битовая маска по EventType; ~0 = всё
```

Гарантии, которые фиксируем в документации:
- События доставляются из **отдельного диспетчер-потока**, а не из потока событий adb и
  не из рабочего потока операции: медленный обработчик не должен тормозить протокол.
- Порядок событий внутри одной `OperationId` сохраняется; между разными операциями — нет.
- `OperationProgress` троттлится (`Options::progress_interval`, по умолчанию 200 мс).
- Очередь событий ограничена (`Options::event_queue_limit`); при переполнении первыми
  выбрасываются `OperationProgress`/`OperationHeartbeat`, а по факту потерь приходит
  `InternalError` с их количеством. Критичные события не теряются.
- Подписчиков может быть несколько (метрики + лог). Исключение из обработчика ловится и
  превращается в `InternalError`; блокировать обработчик надолго нельзя.

---

## 9. Клиент, устройство, операции

```cpp
struct Options {
    LogOptions  log;
    AuthOptions auth;
    Timeouts    timeouts;
    LogSink     log_sink;
    EventFn     on_event;
    EventMask   event_mask = ~EventMask{0};
    size_t      max_connections = 0;        // 0 = без ограничения
    size_t      async_worker_threads = 4;   // отдельный пул для *_async
    uint16_t    default_port = 5555;
    ms          progress_interval{200};
    size_t      event_queue_limit = 10'000;
};

// -------------------------------------------------- операции
class LIBADB_API Operation {
public:
    OperationId id() const;
    Command command() const;
    const std::string& serial() const;
    bool  done() const;
    Phase phase() const;
    bool  wait(ms timeout = ms{0});     // 0 = бесконечно; false = не дождались
    Result result() const;              // валиден после done()
    void  cancel();                     // идемпотентно, из любого потока
    bool  canceled() const;
};
using OperationPtr = std::shared_ptr<Operation>;

class LIBADB_API BatchOperation {
public:
    bool   wait(ms timeout = ms{0});
    bool   done() const;
    size_t total() const;
    size_t finished() const;
    std::map<std::string, Result> results() const;    // готовые на момент вызова
    std::vector<OperationPtr> operations() const;     // для точечной отмены
    void   cancel();                                  // отменить всё незавершённое
};
using BatchOperationPtr = std::shared_ptr<BatchOperation>;

// -------------------------------------------------- опции команд
enum class Compression { None, Any, Brotli, Lz4, Zstd };

struct PushOptions {
    Compression compression = Compression::Any;
    bool sync = false;                             // только новые/изменённые
    ProgressFn on_progress;
    StartedFn  on_start;                           // отдаёт OperationId до начала работы
    std::optional<TransferTimeout> timeout;
};
struct PullOptions {
    Compression compression = Compression::None;
    bool copy_attrs = false;
    ProgressFn on_progress;
    StartedFn  on_start;
    std::optional<TransferTimeout> timeout;
};
struct ShellOptions {
    bool capture_output = true;                    // положить в Result::output
    OutputFn  on_output;                           // и/или стримить
    StartedFn on_start;
    std::optional<ms> timeout;
};
struct UninstallOptions {
    bool keep_data = false;                        // pm uninstall -k
    int  user_id = -1;                             // -1 = не указывать --user
    StartedFn on_start;
    std::optional<ms> timeout;
};

// -------------------------------------------------- устройство
class LIBADB_API Device {
public:
    ~Device();
    const std::string& serial() const;             // "192.168.151.231:5555"
    bool is_online() const;
    bool has_feature(const std::string& name) const;
    std::string get_prop(const std::string& name);

    // синхронно
    Result push(const std::vector<std::string>& local_paths,
                const std::string& remote_path, const PushOptions& = {});
    Result pull(const std::vector<std::string>& remote_paths,
                const std::string& local_path, const PullOptions& = {});
    Result shell(const std::string& command, const ShellOptions& = {});
    Result install(const std::vector<std::string>& paths, const InstallOptions& = {});
    Result uninstall(const std::string& package, const UninstallOptions& = {});

    // асинхронно (выполняется в пуле async_worker_threads)
    OperationPtr push_async(const std::vector<std::string>& local_paths,
                            const std::string& remote_path, const PushOptions& = {});
    OperationPtr pull_async(...);
    OperationPtr shell_async(...);
    OperationPtr install_async(...);
    OperationPtr uninstall_async(...);

    // длинная сессия (logcat и подобное), не блокирует Device
    ShellSessionPtr open_shell(const std::string& command,
                              const ShellSessionOptions& = {});

    void cancel_current();                         // отменить текущую операцию
    void close();
};
using DevicePtr = std::shared_ptr<Device>;

// -------------------------------------------------- клиент
// Синглтон: библиотека наследует глобальное состояние adb (цикл событий, список
// транспортов, ключи авторизации), поэтому несколько независимых экземпляров
// в одном процессе не поддерживаются.
class LIBADB_API Client {
public:
    static Status  initialize(const Options&);
    static void    shutdown();                     // close_all() + остановка цикла
    static bool    is_initialized();
    static Client& instance();

    // опции в рантайме
    void set_max_connections(size_t);
    size_t max_connections() const;
    size_t busy_slots() const;
    void set_timeouts(const Timeouts&);
    Timeouts timeouts() const;
    void set_log_options(const LogOptions&);
    void set_log_level(LogLevel);
    void set_log_sink(LogSink);
    SubscriptionId subscribe(EventFn, EventMask = ~EventMask{0});
    void unsubscribe(SubscriptionId);

    // режим 2
    DevicePtr connect(const DeviceAddress&, Status* out = nullptr);
    void disconnect(const DeviceAddress&);
    std::vector<std::string> connected_devices() const;

    // режим 1
    using DeviceTask = std::function<Result(Device&)>;
    std::map<std::string, Result> for_each(const std::vector<DeviceAddress>&,
                                           const DeviceTask&);
    std::map<std::string, Result> push_all(const std::vector<DeviceAddress>&,
        const std::vector<std::string>& local, const std::string& remote,
        const PushOptions& = {});
    std::map<std::string, Result> pull_all(const std::vector<DeviceAddress>&,
        const std::vector<std::string>& remote, const std::string& local,
        const PullOptions& = {});
    std::map<std::string, Result> shell_all(const std::vector<DeviceAddress>&,
        const std::string& command, const ShellOptions& = {});
    std::map<std::string, Result> install_all(const std::vector<DeviceAddress>&,
        const std::vector<std::string>& paths, const InstallOptions& = {});
    std::map<std::string, Result> uninstall_all(const std::vector<DeviceAddress>&,
        const std::string& package, const UninstallOptions& = {});
    // ... и *_async варианты, возвращающие BatchOperationPtr

    // отмена и аварийное закрытие
    void cancel(OperationId);
    void cancel_all();          // отменить операции, соединения оставить
    void close_all();           // порвать ВСЕ соединения: текущие операции немедленно
                                // завершаются со Status::ConnectionClosed, слоты
                                // освобождаются, ShellSession'ы закрываются
};
}  // namespace libadb
```

### Синхронно и асинхронно, отмена

- **Синхронно**: `Result r = dev->push(...)` — поток блокируется. Отмена возможна из
  другого потока: идентификатор операции выдаётся до начала работы через
  `PushOptions::on_start` (или приходит событием `OperationStarted`), дальше
  `Client::cancel(id)`. Есть сокращения `dev->cancel_current()` и `Client::cancel_all()`.
- **Асинхронно**: `auto op = dev->push_async(...)` — работа уходит в пул библиотеки,
  доступны `op->wait(timeout)`, `op->cancel()`, `op->result()`.
- Второй `*_async` на занятом `Device` вернёт операцию, сразу завершённую со
  `Status::DeviceBusy` (последовательность команд на одном устройстве — забота
  вызывающего). Исключение — `ShellSession`, она не занимает `Device`.
- `cancel()` рвёт сессию/сокет операции; завершение — в пределах сотен миллисекунд,
  «мгновенно» не обещаем. Отмена на фазе `Commit` **не отменяет установку на устройстве** —
  `pm` уже получил команду; это отражается в `Result::error`.
- `close_all()` и `cancel_all()` можно звать из любого потока, включая обработчик событий.
  Отдельный код `ConnectionClosed` (не `Canceled`) позволяет отличить «я сам отменил» от
  «соединения аварийно закрыли».

---

## 10. Установка: split APK, `.apks`, конфликт подписи

```cpp
enum class InstallKind {
    Auto,          // 1 apk -> Single; N apk -> SplitSet; *.apks/*.zip -> Bundle
    Single,
    SplitSet,      // несколько частей ОДНОГО пакета (base + split_config.*)
    Bundle,        // .apks (bundletool): распаковать и поставить как SplitSet
    MultiPackage,  // несколько независимых пакетов в одной атомарной сессии
};

enum class ConflictPolicy {
    Fail,               // (дефолт) вернуть ошибку с кодом pm
    Reinstall,          // uninstall + install заново (данные приложения теряются)
    ReinstallKeepData,  // ЗАРЕЗЕРВИРОВАНО: возвращает Status::NotImplemented
};

enum class PackageNameSource {
    Explicit,   // (дефолт) брать только InstallOptions::package_name
    Auto,       // определять автоматически (ошибка pm, затем AndroidManifest.xml)
    Both,       // если задано явно — использовать; иначе определять
};

struct InstallOptions {
    InstallKind kind = InstallKind::Auto;
    std::vector<std::string> flags;         // сырые флаги pm: "-r", "-g", "-d", "-t", ...
    ConflictPolicy on_conflict = ConflictPolicy::Fail;
    PackageNameSource package_name_source = PackageNameSource::Explicit;
    std::string package_name;               // нужен для on_conflict != Fail
    bool allow_downgrade_retry = false;     // при VERSION_DOWNGRADE добавить -d и повторить
    int  user_id = -1;                      // сахар над --user N
    ProgressFn on_progress;                 // фаза Transfer
    OutputFn   on_output;                   // сырой вывод pm
    StartedFn  on_start;
    std::optional<InstallTimeout> timeout;
};
```

Split-установка выполняется сессией: `pm install-create` → `pm install-write` на каждый
файл → `pm install-commit`. `.apks` предварительно распаковывается (в коде уже есть
`expandApks()`).

**Про чужую подпись — честно.** Если пакет уже установлен и подписан другим ключом,
никакой флаг `pm` не поможет: будет `INSTALL_FAILED_UPDATE_INCOMPATIBLE` («signatures do
not match previously installed version»). Единственный путь без root — удалить пакет и
поставить заново, данные при этом теряются. Поэтому:
- `ConflictPolicy::Reinstall` делает именно это (с событием `OperationRetry` и причиной);
- `ConflictPolicy::ReinstallKeepData` заявлен в API, но возвращает
  `Status::NotImplemented`: `pm uninstall -k` с чужой подписью не спасает и на многих
  прошивках объявлен нерабочим, поэтому обещать сохранение данных мы не будем.

`package_name` для авто-переустановки: решает вызывающий через `PackageNameSource`.
`Auto`/`Both` определяют имя из текста ошибки `pm`, а если не вышло — разбором
`AndroidManifest.xml` (бинарный AXML) из APK; `Explicit` (по умолчанию) — только то, что
передали, иначе `Status::InvalidArgument`.

Ошибки `pm` раскладываются в `Status` + `remote_code`, как минимум:
`INSTALL_FAILED_UPDATE_INCOMPATIBLE` → `SignatureMismatch`,
`INSTALL_FAILED_VERSION_DOWNGRADE` → `VersionDowngrade`,
`INSTALL_FAILED_INSUFFICIENT_STORAGE` → `InsufficientStorage`,
`INSTALL_FAILED_INVALID_APK` / `INSTALL_PARSE_FAILED_*` → `InvalidApk`,
`INSTALL_FAILED_MISSING_SPLIT` → `MissingSplit`,
остальные (`INSTALL_FAILED_TEST_ONLY`, `INSTALL_FAILED_USER_RESTRICTED`, …) →
`RemoteError` с сохранением кода в `remote_code`.

---

## 11. Длинные сессии: `logcat` и прочий стрим

```cpp
struct ShellSessionOptions {
    bool     want_stdin = false;      // писать в stdin (например, `sh -`)
    bool     separate_stderr = true;  // shell_v2, если поддерживается устройством
    ms       idle_timeout{0};         // нет данных дольше N мс -> закрыть; 0 = никогда
    ms       total_timeout{0};        // общий дедлайн; 0 = никогда (для logcat так и надо)
    OutputFn on_output;               // push-режим
    size_t   buffer_limit = 1 << 20;  // лимит внутреннего буфера для pull-режима
};

class LIBADB_API ShellSession {
public:
    OperationId id() const;
    bool alive() const;
    // pull-режим: >0 — байт прочитано, 0 — таймаут ожидания, -1 — сессия закрыта
    ssize_t read(char* buf, size_t len, bool* is_stderr = nullptr, ms timeout = ms{0});
    bool write(const void* data, size_t len);      // stdin, если want_stdin
    bool send_signal(int sig);                     // SIGINT для прерывания logcat
    void close();
    bool wait(ms timeout = ms{0});
    int  exit_code() const;
};
using ShellSessionPtr = std::shared_ptr<ShellSession>;
```

- Сессия **не блокирует** `Device`: можно держать открытый `logcat` и параллельно делать
  `push` на том же устройстве.
- Доступны оба режима: push (`on_output`) и pull (`read()`, удобно для C и своих циклов).
- `idle_timeout` — практичная защита для `logcat` («если долго нет ни строчки — что-то не так»).
- Отмена — `close()`; идентификатор общий с операциями, поэтому `Client::cancel(id)` и
  `close_all()` тоже её закрывают.
- Отдельного API под `logcat` не делаем: это `open_shell("logcat -v threadtime")`.

---

## 12. C ABI

Только opaque-указатели, POD и функции; строки — UTF-8 `const char*`; исключения через
границу не проходят; опции задаются сеттерами (структуру можно расширять без ломки ABI).

```c
// include/libadb/libadb_c.h
#ifdef __cplusplus
extern "C" {
#endif

typedef struct libadb_options       libadb_options;
typedef struct libadb_device        libadb_device;
typedef struct libadb_op            libadb_op;
typedef struct libadb_batch         libadb_batch;
typedef struct libadb_results       libadb_results;
typedef struct libadb_event         libadb_event;
typedef struct libadb_shell_session libadb_shell_session;
typedef struct libadb_install_opts  libadb_install_opts;

typedef enum {
    LIBADB_OK = 0, LIBADB_ERR_INVALID_ARGUMENT, LIBADB_ERR_NOT_INITIALIZED,
    LIBADB_ERR_NOT_IMPLEMENTED, LIBADB_ERR_UNSUPPORTED, LIBADB_ERR_INTERNAL,
    LIBADB_ERR_CONNECT_FAILED, LIBADB_ERR_CONNECT_TIMEOUT, LIBADB_ERR_AUTH_REQUIRED,
    LIBADB_ERR_UNAUTHORIZED, LIBADB_ERR_OFFLINE, LIBADB_ERR_DEVICE_LOST,
    LIBADB_ERR_CONNECTION_CLOSED, LIBADB_ERR_SLOT_BUSY, LIBADB_ERR_SLOT_TIMEOUT,
    LIBADB_ERR_DEVICE_BUSY, LIBADB_ERR_COMMAND_TIMEOUT, LIBADB_ERR_STALL_TIMEOUT,
    LIBADB_ERR_CANCELED, LIBADB_ERR_IO, LIBADB_ERR_LOCAL_FILE, LIBADB_ERR_REMOTE,
    LIBADB_ERR_SIGNATURE_MISMATCH, LIBADB_ERR_VERSION_DOWNGRADE,
    LIBADB_ERR_INSUFFICIENT_STORAGE, LIBADB_ERR_INVALID_APK, LIBADB_ERR_MISSING_SPLIT
} libadb_status;

typedef enum { LIBADB_LOG_TRACE=0, LIBADB_LOG_DEBUG, LIBADB_LOG_INFO,
               LIBADB_LOG_WARN, LIBADB_LOG_ERROR, LIBADB_LOG_OFF } libadb_log_level;
typedef enum { LIBADB_CMD_CONNECT=0, LIBADB_CMD_SHELL, LIBADB_CMD_PUSH, LIBADB_CMD_PULL,
               LIBADB_CMD_INSTALL, LIBADB_CMD_UNINSTALL } libadb_cmd;
typedef enum { LIBADB_HEALTHCHECK_NONE=0, LIBADB_HEALTHCHECK_TRANSPORT,
               LIBADB_HEALTHCHECK_SHELL } libadb_healthcheck_mode;

typedef void (*libadb_log_fn)(libadb_log_level, const char* serial, const char* msg, void* user);
typedef void (*libadb_event_fn)(const libadb_event*, void* user);
typedef void (*libadb_output_fn)(const char* serial, const char* data, size_t len,
                                 int is_stderr, void* user);
typedef void (*libadb_progress_fn)(const char* serial, uint64_t done, uint64_t total, void* user);

/* версия */
const char* libadb_version(void);
uint32_t    libadb_version_number(void);
const char* libadb_status_str(libadb_status);

/* опции */
libadb_options* libadb_options_new(void);
void libadb_options_free(libadb_options*);
void libadb_options_set_log_enabled(libadb_options*, int);
void libadb_options_set_log_file(libadb_options*, const char* path);
void libadb_options_set_log_level(libadb_options*, libadb_log_level);
void libadb_options_set_log_rotation(libadb_options*, size_t max_size, size_t max_files);
void libadb_options_set_trace_tags(libadb_options*, const char* tags);
void libadb_options_set_log_callback(libadb_options*, libadb_log_fn, void* user);
void libadb_options_set_event_callback(libadb_options*, libadb_event_fn, uint64_t mask, void* user);
void libadb_options_add_key_file(libadb_options*, const char* path);
void libadb_options_add_key_pem(libadb_options*, const char* pem, size_t len);
void libadb_options_set_use_default_keys(libadb_options*, int);
void libadb_options_set_max_connections(libadb_options*, size_t);
void libadb_options_set_async_workers(libadb_options*, size_t);
void libadb_options_set_default_port(libadb_options*, uint16_t);
void libadb_options_set_timeout_ms(libadb_options*, libadb_cmd, uint32_t ms);
void libadb_options_set_transfer_timeout_ms(libadb_options*, libadb_cmd /*PUSH|PULL*/,
                                            uint32_t total_ms, uint32_t stall_ms);
void libadb_options_set_install_timeout_ms(libadb_options*, uint32_t prepare_ms,
                                           uint32_t transfer_total_ms, uint32_t transfer_stall_ms,
                                           uint32_t commit_ms);
void libadb_options_set_install_healthcheck(libadb_options*, libadb_healthcheck_mode,
                                           uint32_t interval_ms, int max_failures);
void libadb_options_set_slot_timeout_ms(libadb_options*, uint32_t ms);
void libadb_options_set_progress_interval_ms(libadb_options*, uint32_t ms);

/* жизненный цикл */
libadb_status libadb_init(const libadb_options* /* NULL = дефолты */);
void          libadb_shutdown(void);
void libadb_set_max_connections(size_t);
void libadb_set_log_level(libadb_log_level);
uint64_t libadb_subscribe(libadb_event_fn, uint64_t mask, void* user);
void     libadb_unsubscribe(uint64_t subscription);
const char* libadb_last_error(void);      /* для текущего потока */

/* события: геттеры */
int         libadb_event_type(const libadb_event*);
const char* libadb_event_serial(const libadb_event*);
uint64_t    libadb_event_op(const libadb_event*);
int         libadb_event_command(const libadb_event*);
int         libadb_event_phase(const libadb_event*);
libadb_status libadb_event_status(const libadb_event*);
const char* libadb_event_remote_code(const libadb_event*);
uint64_t    libadb_event_bytes_done(const libadb_event*);
uint64_t    libadb_event_bytes_total(const libadb_event*);
uint64_t    libadb_event_elapsed_ms(const libadb_event*);
const char* libadb_event_message(const libadb_event*);
double      libadb_event_mib_per_sec(const libadb_event*);

/* одно устройство */
libadb_status libadb_device_open(const char* address, libadb_device** out);
void          libadb_device_close(libadb_device*);
const char*   libadb_device_serial(const libadb_device*);
libadb_status libadb_push(libadb_device*, const char* local, const char* remote,
                          libadb_progress_fn, void* user, uint64_t* out_bytes);
libadb_status libadb_pull(libadb_device*, const char* remote, const char* local,
                          libadb_progress_fn, void* user, uint64_t* out_bytes);
libadb_status libadb_shell(libadb_device*, const char* command,
                           libadb_output_fn, void* user, int* out_exit_code);
libadb_status libadb_uninstall(libadb_device*, const char* package, int keep_data);

/* установка (в т.ч. split) */
libadb_install_opts* libadb_install_opts_new(void);
void libadb_install_opts_free(libadb_install_opts*);
void libadb_install_opts_set_kind(libadb_install_opts*, int kind);
void libadb_install_opts_add_flag(libadb_install_opts*, const char* flag);
void libadb_install_opts_set_conflict_policy(libadb_install_opts*, int policy);
void libadb_install_opts_set_package_name(libadb_install_opts*, const char* name);
void libadb_install_opts_set_package_name_source(libadb_install_opts*, int source);
libadb_status libadb_install(libadb_device*, const char* const* paths, size_t count,
                             const libadb_install_opts* /* NULL = дефолты */,
                             libadb_progress_fn, libadb_output_fn, void* user);

/* асинхронные операции и отмена */
libadb_status libadb_push_async(libadb_device*, const char* local, const char* remote,
                                libadb_op** out);
int           libadb_op_wait(libadb_op*, uint32_t timeout_ms);   /* 1 = завершилась */
void          libadb_op_cancel(libadb_op*);
libadb_status libadb_op_status(const libadb_op*);
uint64_t      libadb_op_id(const libadb_op*);
uint64_t      libadb_op_bytes(const libadb_op*);
void          libadb_op_free(libadb_op*);
void libadb_cancel(uint64_t op_id);
void libadb_cancel_all(void);
void libadb_close_all(void);

/* батч по списку адресов */
libadb_status libadb_push_all(const char* const* addresses, size_t count,
                              const char* local, const char* remote,
                              libadb_progress_fn, void* user, libadb_results** out);
libadb_status libadb_shell_all(const char* const* addresses, size_t count,
                               const char* command, libadb_output_fn, void* user,
                               libadb_results** out);
size_t        libadb_results_count(const libadb_results*);
const char*   libadb_results_serial(const libadb_results*, size_t i);
libadb_status libadb_results_status(const libadb_results*, size_t i);
int           libadb_results_exit_code(const libadb_results*, size_t i);
uint64_t      libadb_results_bytes(const libadb_results*, size_t i);
uint64_t      libadb_results_duration_ms(const libadb_results*, size_t i);
const char*   libadb_results_error(const libadb_results*, size_t i);
const char*   libadb_results_output(const libadb_results*, size_t i);
void          libadb_results_free(libadb_results*);

/* длинные сессии */
libadb_status libadb_shell_open(libadb_device*, const char* cmd, int want_stdin,
                                libadb_shell_session** out);
ssize_t libadb_shell_read(libadb_shell_session*, char* buf, size_t len,
                          int* is_stderr, uint32_t timeout_ms);
int     libadb_shell_write(libadb_shell_session*, const void* data, size_t len);
int     libadb_shell_send_signal(libadb_shell_session*, int sig);
void    libadb_shell_close(libadb_shell_session*);

#ifdef __cplusplus
}
#endif
```

Из чистого C всё работает: `libadb.so` внутри C++ и тянет `libstdc++.so.6` как свою
зависимость, вызывающему достаточно `gcc app.c -ladb`.

---

## 13. Версионирование библиотеки

### 13.1 Три имени

```
libadb.so.1.2.0   ← реальный файл
libadb.so.1       ← SONAME, это имя ищет ld.so в рантайме
libadb.so         ← имя для линковки (-ladb)
```

SONAME записан внутри файла (`readelf -d libadb.so.1.2.0 | grep SONAME`) и попадает в
`NEEDED` приложения. **SONAME меняется только при несовместимом изменении ABI**: тогда
старые бинарники продолжают работать с `libadb.so.1`, новые берут `libadb.so.2`, и обе
версии могут стоять в системе одновременно.

Semver:
- **MAJOR** — сломали ABI (удалили/изменили символ, поменяли layout публичной структуры,
  порядок значений `enum`, добавили virtual-метод) → `SOVERSION` растёт.
- **MINOR** — только добавили символы → ABI совместим.
- **PATCH** — исправления без изменения интерфейса.

```cmake
project(ADB VERSION 1.0.0)

add_library(adb_shared SHARED ${LIBADB_API_SOURCES})
target_link_libraries(adb_shared PRIVATE adb_core)
set_target_properties(adb_shared PROPERTIES
    OUTPUT_NAME adb
    VERSION   ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
target_link_options(adb_shared PRIVATE
    -Wl,--version-script=${CMAKE_SOURCE_DIR}/adb/lib/libadb.map
    -Wl,--exclude-libs,ALL
    -Wl,--no-undefined)
configure_file(include/libadb/version.h.in include/libadb/version.h @ONLY)
```

Все три симлинка CMake создаёт сам при `install(TARGETS ...)`.

### 13.2 Version script

`adb/lib/libadb.map` задаёт ровно то, что экспортируется:

```
LIBADB_1.0 {
  global:
    libadb_*;
    _ZN6libadb*;      /* публичные символы namespace libadb */
  local:
    *;                /* всё остальное скрыть */
};

/* при добавлении функций в 1.1 */
LIBADB_1.1 { global: libadb_new_function; } LIBADB_1.0;
```

Без `local: *` наружу вылезут символы boringssl/protobuf/zstd/libbase и начнут
конфликтовать с теми же библиотеками в приложении — это самая частая причина загадочных
падений. Тяжёлые зависимости линкуем **статически внутрь** `libadb.so`.

### 13.3 Версия в коде

```c
/* include/libadb/version.h — генерируется из version.h.in */
#define LIBADB_VERSION_MAJOR  @PROJECT_VERSION_MAJOR@
#define LIBADB_VERSION_MINOR  @PROJECT_VERSION_MINOR@
#define LIBADB_VERSION_PATCH  @PROJECT_VERSION_PATCH@
#define LIBADB_VERSION_STRING "@PROJECT_VERSION@"
#define LIBADB_VERSION_NUMBER ((@PROJECT_VERSION_MAJOR@<<16)|(@PROJECT_VERSION_MINOR@<<8)|@PROJECT_VERSION_PATCH@)
```

Приложение может сверить заголовки и `.so`:
`(libadb_version_number() >> 16) == LIBADB_VERSION_MAJOR`.

### 13.4 Установка и совместимость

```cmake
install(TARGETS adb_shared EXPORT libadbTargets LIBRARY DESTINATION lib)
install(DIRECTORY include/libadb DESTINATION include)
install(EXPORT libadbTargets NAMESPACE libadb:: DESTINATION lib/cmake/libadb)
configure_file(adb/lib/libadb.pc.in libadb.pc @ONLY)
install(FILES ${CMAKE_BINARY_DIR}/libadb.pc DESTINATION lib/pkgconfig)
```

Потребитель: `pkg-config --cflags --libs libadb` или `find_package(libadb 1.0 REQUIRED)` +
`target_link_libraries(app libadb::adb)`. Если библиотека лежит не в системном пути —
`-Wl,-rpath,'$ORIGIN/../lib'` (именно так собираются наши примеры, чтобы не требовать
установки). Контроль ABI между релизами — `abidiff` (libabigail) в CI.

### 13.5 Что такое «стабильный контракт»

Речь про двоичную совместимость: «приложение, собранное с версией X, работает с новой
`libadb.so` без перекомпиляции». ABI ломают, в частности: удаление/переименование
экспортируемой функции, изменение сигнатуры, добавление поля в середину публичной
структуры или изменение её размера, вставка значений в середину `enum`, добавление
virtual-метода, изменение inline-функции в заголовке, смена компилятора или
`_GLIBCXX_USE_CXX11_ABI` при наличии `std::string`/`std::function` в интерфейсе.

Отсюда разделение обещаний:
- **C ABI — стабильный контракт.** Opaque-указатели, POD, сеттеры вместо структур; новые
  возможности = новые функции. Такую `.so` можно обновлять под уже собранными бинарниками.
- **C++ API — удобный, но привязан к MAJOR.** В нём STL-типы и классы, стабилизировать их
  без большой боли невозможно; совместимость обещаем внутри одного MAJOR, при смене MAJOR
  требуется пересборка. Свой код пересобирается вместе с библиотекой, сторонним
  потребителям предлагается C API.

---

## 14. Что нужно поправить во внутреннем коде

1. `__adb_argv` / `__adb_envp` определены в `adirect.cpp` — нужны свои определения внутри
   библиотеки (`adirect` при этом продолжает собираться как раньше).
2. `adb_get_feature_set()` (`adb/lib/src/adb_client.cpp`) кеширует набор фич в глобальном
   `static` — при параллельной работе фичи одного устройства «протекают» на другие.
   Нужен кеш на транспорт или `thread_local`.
3. `AdbDevice::createSession()` закрывает предыдущие сессии устройства — это мешает и
   `ShellSession` (logcat + push одновременно), и health-check во время `commit`.
   Нужен честный список активных сессий.
4. `AdbSession::wait()` ждёт бесконечно (`future.get()`) — нужна версия с таймаутом и
   `abort()` по его истечении.
5. `SyncConnection` работает в режиме `quiet` — нужен колбэк прогресса и возможность
   прерывания (для `stall`-таймаута и `cancel()`).
6. Путь `/tmp/adb.log` зашит константой в `adb/adb_trace.cpp` — параметризуется через
   `LogOptions`, включая режим «лог выключен, файл не открываем».
7. `adb/lib/include/interface.h` — мёртвый дубль `IadbListener.h`, удалить.
8. Логика `pull` с раскладкой `<dir>/<ip>/<file>` — политика утилиты, в библиотеку не
   переносится (останется в примере `adirect2`).

---

## 15. План работ (этап = коммит `libadb-phase-N`)

План разбит на четыре части. Нумерация этапов сквозная и **не** совпадает с
первоначальной редакцией документа: порядок пересмотрен по итогам этапов 0–9.
Перед каждым коммитом — запись в `docs/libadb-development-log.md`.

### Часть 1. Библиотека версии 1.0

| Этап | Содержание | Статус |
|---|---|---|
| 0 | Этот документ + `docs/libadb-development-log.md` | готово |
| 1 | Сборка: `adb_core` (STATIC, PIC), `libadb.so` (visibility hidden, version script, SOVERSION), `version.h.in`, каркас публичных заголовков. `adirect` не меняется | готово |
| 2 | Фасад `Client`/`Device` (PIMPL) + синхронные `push/pull/shell/install/uninstall` поверх существующих внутренних классов | готово |
| 3 | Логирование: `LogOptions` (включая «выключено = файл не открываем»), `LogSink`, `spdlog_sink.hpp` | готово |
| 4 | Слоты подключений: `max_connections`, `slot_acquire`, оба режима работы (батч и одиночный `connect`) | готово |
| 5 | События: диспетчер-поток, подписки, маски, троттлинг прогресса — **без статистики** | готово |
| 6 | Статистика: `TransferStats`, `OperationStats`, скорость МБ/с в `Result` и событиях | готово |
| 7 | Таймауты: `total`/`stall` для передачи, фазы `install`, health-check коммита (`Transport`/`Shell`) | готово |
| 8 | Отмена и асинхронный режим: `Operation`, `BatchOperation`, отдельный async-пул, `cancel/cancel_all/close_all` | готово |
| 9 | Установка: split/`.apks`/multi-package, разбор ошибок `pm`, `ConflictPolicy` (`ReinstallKeepData` → `NotImplemented`) | готово |
| 10 | Авторизация: ключи из файлов и из текста PEM (`AuthOptions`) — **был этапом 12** | |
| 11 | Упаковка: `libadb.map`, `libadb.pc.in`, CMake export, установка, `abidiff` — **был этапом 14** | |
| 14 | Документация по реализованному публичному API библиотеки (`docs/libadb-usage.md`): всё, что вошло в 1.0, включая особенности health-check и суммирование пулов подключений — **был этапом 15** | |

Этапы 12 и 13 — это части 2 и 3 соответственно (см. ниже). Нумерация выбрана
так, чтобы этап 14 (документация по API) закрывал версию 1.0 последним — уже
после того, как решены вопросы со сборкой зависимостей и упаковкой.

### Часть 2. Зависимости: статическая линковка вместо своих `.so` (этап 12)

**Решение принято: линкуем статически, своих `.so` в систему не ставим.**
Рассматривался вариант «переименовать BoringSSL в `bssl`, собрать deb-пакет и
поставить в систему» — он отвергнут, причины ниже.

Что сейчас (замер на собранной библиотеке):

```
readelf -d build/libadb.so.1 | grep NEEDED
  libssl.so          <- BoringSSL, SONAME без версии
  libcrypto.so       <- BoringSSL, SONAME без версии
  libziparchive.so   <- вообще без SONAME
  libprotobuf.so.32, libbrotli*, liblz4, libzstd, libspdlog, libfmt  <- дистрибутивные
```

Почему статика, а не свой пакет:

1. **Конфликт имён с системным OpenSSL.** У BoringSSL SONAME — ровно
   `libssl.so` и `libcrypto.so`, то есть те же имена, что у OpenSSL. Свой пакет
   с такими файлами — риск сломать сторонний софт в системе. Переименование в
   `bssl` проблему решает, но требует патчить сборку BoringSSL и тащить пакет
   через apt на трёх дистрибутивах.
2. **`libziparchive.so` собран без SONAME** — как разделяемая библиотека он для
   установки в систему непригоден в принципе.
3. **Ни BoringSSL, ни libziparchive не имеют стабильного ABI** и не
   версионируются: обновление любой из них ломало бы всех потребителей.
4. **Утечки символов не будет.** У `libadb.so` уже стоят `-fvisibility=hidden`,
   version script `adb/lib/libadb.map` (наружу только `libadb_*` и
   `_ZN6libadb*`) и `-Wl,--exclude-libs,ALL`. Символы статических зависимостей
   попадут внутрь `.so`, но в динамическую таблицу — нет, то есть приложение
   спокойно линкует свой OpenSSL рядом.

Работы этапа 12:

- BoringSSL собирать статически (`libssl.a`, `libcrypto.a`, обязательно
  `-fPIC`; `BUILD_STATIC_LIBS=1` в `ExternalProject_Add` уже есть) и линковать в
  `libadb.so`.
- `system/libziparchive/CMakeLists.txt`: `add_library(ziparchive SHARED ...)` →
  `STATIC` + `POSITION_INDEPENDENT_CODE ON`, линковать в `libadb.so`.
- Проверить, что `libcutils`, `libbase`, `diagnose_usb`, `crypto_utils`,
  `libbuildversion` тоже статические и с `-fPIC` (сейчас так и есть).
- Убедиться, что `adirect` продолжает собираться и работать: он линкуется из тех
  же статических объектов.
- Контроль результата: в `readelf -d libadb.so.1` из `NEEDED` исчезают
  `libssl.so`, `libcrypto.so`, `libziparchive.so`; `nm -D --defined-only` не
  показывает ни одного символа BoringSSL/ziparchive; размер `.so` вырастет —
  зафиксировать в журнале.
- Дистрибутивные зависимости (protobuf, brotli, lz4, zstd, spdlog, fmt)
  остаются динамическими: они есть в apt и версионируются нормально.

### Часть 3. Пакеты, документация, примеры (этап 13)

Инфраструктура сборки deb-пакетов и всё, что нужно потребителю библиотеки.

**Два пакета:**

- `libadb1` — рантайм: `libadb.so.1`, `libadb.so.1.0.0`;
- `libadb-dev` — заголовки (`include/libadb/*.h` и сгенерированный
  `version.h`), симлинк `libadb.so`, `libadb.pc` для pkg-config, CMake-конфиг
  для `find_package(libadb)`.

**pkg-config** — используем: `libadb.pc.in` уже предусмотрен этапом 11, в
`libadb-dev` файл ставится в `/usr/lib/x86_64-linux-gnu/pkgconfig/`.

**Инфраструктура сборки:** каталог `debian/` (`control`, `rules`, `changelog`,
`debhelper-compat`, `libadb1.install`, `libadb-dev.install`, `libadb1.symbols`
или `shlibs`), сборка через `dpkg-buildpackage`. Целевые дистрибутивы —
**Debian 12, Ubuntu 24.04, Ubuntu 26.04**; сборка под каждый в контейнере или
chroot, скрипт сборки положить в репозиторий.

**Зависимости в `control`.** Имена версионированных пакетов между
дистрибутивами различаются (например, `libprotobuf32t64` и `libspdlog1.12` в
Ubuntu 24.04 против других имён в Debian 12), поэтому в `Depends` пишем
`${shlibs:Depends}, ${misc:Depends}` — `dpkg-shlibdeps` подставит правильные
имена на каждом дистрибутиве сам. В `Build-Depends` `-dev`-пакеты перечисляем
явно. После части 2 BoringSSL и ziparchive в зависимости не попадают вовсе.

**Проверка пакетов:** `lintian`; установка в чистый контейнер каждого из трёх
дистрибутивов; сборка и запуск примера **только** против установленного пакета,
без дерева исходников.

**Документация по использованию** — `docs/libadb-usage.md` расширяется до
полного руководства: установка пакета, подключение через pkg-config и CMake, все
сценарии (синхронный и асинхронный режимы, события, таймауты, отмена, установка
split/`.apks`/multi-package, логирование, лимит подключений).

**Примеры в `examples/`:** на C++ и на C (полноценный C-пример — после
появления C ABI, часть 4), плюс `adirect2` как аналог `adirect` на публичном
API. Сборка CMake, работа и из установленного пакета, и без установки
(rpath `$ORIGIN`).

### Часть 4. Библиотека версии 1.1

| Этап | Содержание |
|---|---|
| 15 | Парсер `AndroidManifest.xml` (AXML) → авто-определение `package_name`, `PackageNameSource::Auto`/`Both` — **был этапом 10** |
| 16 | C ABI: `libadb_c.h` + реализация — **был этапом 13** |
| 17 | `ShellSession` (`logcat`), снятие ограничения «одна сессия на устройство» — **был этапом 11** |

Все три этапа только добавляют символы, поэтому `SOVERSION` не меняется:
`libadb.so.1` остаётся, растёт MINOR (1.1.0). Появление C ABI и `ShellSession`
потребует расширить version script (`libadb.map`) новым узлом `LIBADB_1.1`.
