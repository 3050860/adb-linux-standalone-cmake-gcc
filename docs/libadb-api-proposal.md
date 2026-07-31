# libadb.so — предложение по публичному API

Статус: **черновик для ревью**. Код ещё не менялся.

Цель: превратить `adb/lib/*` в разделяемую библиотеку `libadb.so` с устойчивым публичным
API/ABI, покрывающую команды `push`, `pull`, `shell`, `install`, `uninstall`, с настройками
логирования, параллелизма и таймаутов. `adirect` становится первым клиентом этой библиотеки.

---

## 1. Базовые принципы

1. **Публичные заголовки не тащат внутренности adb.** Сейчас `AdbDevice.h` включает
   `adb.h`, `transport.h`, `socket.h`, а `IadbListener.h` использует `ConnectionState`
   из `adb.h`. Это делает невозможным использование библиотеки извне. Решение — фасад с
   PIMPL: наружу только `include/libadb/*.h`, внутри — существующие классы `AdbManager` /
   `AdbDevice` / `AdbSession` / `AdbFileSync` / `AdbInstaller`.
2. **Всё наружу — через `LIBADB_API`** (`__attribute__((visibility("default")))`), сборка с
   `-fvisibility=hidden` + version script. Иначе из `.so` полезут все символы adb, boringssl,
   protobuf, zstd и будут конфликтовать с символами приложения.
3. **Никаких исключений через границу библиотеки.** Все API возвращают статус; внутри
   `try/catch` на входных точках.
4. **Никаких C++ типов в C ABI.** C-слой — тонкая обёртка над C++ фасадом.
5. **Никакого spdlog в ABI.** spdlog header-only и версионно-хрупкий: если он окажется в
   публичном интерфейсе, любое обновление spdlog у пользователя = ODR/ABI-проблема.
   Наружу отдаём callback, а «удобная» интеграция со spdlog — отдельный inline-заголовок
   (см. §4.2).

Дерево файлов:

```
include/libadb/libadb.h            # C++ API (основной)
include/libadb/libadb_c.h          # C ABI
include/libadb/spdlog_sink.hpp     # опциональный inline-адаптер под spdlog (не в .so)
include/libadb/version.h.in        # генерируется CMake -> version.h
adb/lib/src/api/client.cpp         # реализация фасада (C++)
adb/lib/src/api/c_api.cpp          # реализация C ABI
adb/lib/libadb.map                 # version script (список экспорта)
adb/lib/libadb.pc.in               # pkg-config
```

---

## 2. C++ API

```cpp
// include/libadb/libadb.h
#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include "libadb/version.h"

#define LIBADB_API __attribute__((visibility("default")))

namespace libadb {

// ---------------------------------------------------------------- версия
LIBADB_API const char* version();          // "1.0.3"
LIBADB_API uint32_t    version_number();   // 0x00010003 (major<<16 | minor<<8 | patch)

// ---------------------------------------------------------------- статусы
enum class Status {
    Ok = 0,
    InvalidArgument,
    NotInitialized,
    ConnectFailed,
    ConnectTimeout,
    AuthRequired,       // устройство не авторизовано
    Offline,
    CommandTimeout,
    IoError,
    RemoteError,        // adbd/pm вернул ошибку
    LocalFileError,
    Unsupported,        // нет нужной feature у устройства
    Canceled,
    Internal,
};
LIBADB_API const char* to_string(Status);

struct Result {
    Status status = Status::Ok;
    int exit_code = 0;                        // shell / install / uninstall
    std::string error;                        // человекочитаемая причина
    std::string output;                       // stdout+stderr, если capture_output
    uint64_t bytes = 0;                       // push/pull: передано байт
    std::chrono::milliseconds duration{0};

    bool ok() const { return status == Status::Ok; }
    explicit operator bool() const { return ok(); }
};

// ---------------------------------------------------------------- логирование
enum class LogLevel { Trace = 0, Debug, Info, Warn, Error, Off };

struct LogOptions {
    // Внутренний лог libadb (то, что сейчас пишется в /tmp/adb.log).
    std::string file_path      = "/tmp/adb.log";
    LogLevel    level          = LogLevel::Warn;
    size_t      max_file_size  = 5 * 1024 * 1024;
    size_t      max_files      = 3;
    // Аналог ADB_TRACE: "sync,sockets,transport" или "all". Пусто = выключено.
    std::string trace_tags;
    bool        also_stderr    = false;
};

// Логгер уровня приложения: то, что сейчас печатает Logger в adirect
// (Connecting..., Push successful, time: ...). Вызывается из рабочих потоков,
// реализация обязана быть потокобезопасной.
using LogSink = std::function<void(LogLevel level, const std::string& serial,
                                   std::string_view message)>;

// ---------------------------------------------------------------- таймауты
struct Timeouts {
    std::chrono::milliseconds connect  {15'000};
    std::chrono::milliseconds shell    {60'000};
    std::chrono::milliseconds push     {600'000};
    std::chrono::milliseconds pull     {600'000};
    std::chrono::milliseconds install  {900'000};
    std::chrono::milliseconds uninstall{120'000};
    // 0 = без таймаута
};

// ---------------------------------------------------------------- опции
struct Options {
    LogOptions  log;
    Timeouts    timeouts;
    LogSink     event_sink;                 // события уровня приложения
    size_t      max_threads   = 0;          // 0 = без ограничения (поток на устройство)
    uint16_t    default_port  = 5555;       // если в адресе нет ":port"
    std::string adb_key_path;               // пусто = ~/.android/adbkey
};

// ---------------------------------------------------------------- колбэки команд
using ProgressFn = std::function<void(const std::string& serial, uint64_t done,
                                      uint64_t total)>;
using OutputFn   = std::function<void(const std::string& serial, std::string_view chunk,
                                      bool is_stderr)>;

enum class Compression { None, Any, Brotli, Lz4, Zstd };

struct PushOptions {
    Compression compression = Compression::Any;
    bool sync = false;                      // только новые/изменённые файлы
    ProgressFn on_progress;
};
struct PullOptions {
    Compression compression = Compression::None;
    bool copy_attrs = false;
    ProgressFn on_progress;
};
struct ShellOptions {
    bool capture_output = true;             // положить в Result::output
    OutputFn on_output;                     // стриминг по мере поступления
};
struct InstallOptions {
    std::vector<std::string> flags;         // -r, -g, -d, --user 0, ...
    OutputFn on_output;
};
struct UninstallOptions {
    bool keep_data = false;                 // -k
};

// ---------------------------------------------------------------- устройство
// Подключение живёт, пока жив объект. Методы потокобезопасны относительно
// разных Device, но один Device не предназначен для параллельных команд.
class LIBADB_API Device {
public:
    ~Device();
    const std::string& serial() const;      // "192.168.1.10:5555"
    bool is_online() const;
    bool has_feature(const std::string& name) const;
    std::string get_prop(const std::string& name);   // сахар над shell getprop

    Result push(const std::vector<std::string>& local_paths,
                const std::string& remote_path, const PushOptions& = {});
    Result pull(const std::vector<std::string>& remote_paths,
                const std::string& local_path, const PullOptions& = {});
    Result shell(const std::string& command, const ShellOptions& = {});
    Result install(const std::vector<std::string>& apk_paths, const InstallOptions& = {});
    Result uninstall(const std::string& package, const UninstallOptions& = {});

    void cancel();                          // прервать текущую команду

private:
    friend class Client;
    Device();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
using DevicePtr = std::shared_ptr<Device>;

// ---------------------------------------------------------------- клиент
// Синглтон: библиотека наследует глобальное состояние adb (fdevent loop,
// глобальный список транспортов, auth-ключи), поэтому несколько независимых
// экземпляров в одном процессе не поддерживаются — это задокументировано.
class LIBADB_API Client {
public:
    static Status initialize(const Options& options);   // поднимает fdevent loop + auth
    static void   shutdown();
    static bool   is_initialized();
    static Client& instance();

    // Опции, меняемые на ходу
    void set_max_threads(size_t n);
    size_t max_threads() const;
    void set_timeouts(const Timeouts&);
    Timeouts timeouts() const;
    void set_log_level(LogLevel);
    void set_event_sink(LogSink);

    // Подключение с таймаутом Timeouts::connect
    DevicePtr connect(const std::string& address, Status* status = nullptr);
    void disconnect(const std::string& address);
    std::vector<std::string> connected_devices() const;

    // ---- пакетное выполнение -------------------------------------------------
    // Параллелизм ограничен max_threads(); подключение к очередному адресу
    // происходит только когда освободился воркер.
    using DeviceTask = std::function<Result(Device&)>;
    std::map<std::string, Result> for_each(const std::vector<std::string>& addresses,
                                           const DeviceTask& task);

    // Сахар: connect + команда + disconnect для каждого адреса
    std::map<std::string, Result> push_all(const std::vector<std::string>& addresses,
                                           const std::vector<std::string>& local_paths,
                                           const std::string& remote_path,
                                           const PushOptions& = {});
    std::map<std::string, Result> pull_all(...);
    std::map<std::string, Result> shell_all(...);
    std::map<std::string, Result> install_all(...);
    std::map<std::string, Result> uninstall_all(...);

    // Утилита: чтение файла со списком адресов (как -f devices.txt),
    // с добавлением default_port и пропуском комментариев.
    static std::vector<std::string> read_device_list(const std::string& path);
};

}  // namespace libadb
```

### Пример использования (C++)

```cpp
#include <libadb/libadb.h>
#include <libadb/spdlog_sink.hpp>

int main() {
    auto app_log = spdlog::rotating_logger_mt("app", "/var/log/myapp.log", 5<<20, 3);

    libadb::Options opt;
    opt.log.file_path = "/var/log/libadb.log";
    opt.log.level     = libadb::LogLevel::Info;
    opt.log.trace_tags = "sync,transport";          // = ADB_TRACE
    opt.max_threads   = 8;
    opt.timeouts.push = std::chrono::minutes(5);
    opt.event_sink    = libadb::make_spdlog_sink(app_log);

    if (libadb::Client::initialize(opt) != libadb::Status::Ok) return 1;

    auto devices = libadb::Client::read_device_list("devices.txt");
    auto results = libadb::Client::instance().push_all(devices, {"main.apk"},
                                                       "/data/local/tmp/main.apk");
    for (auto& [serial, r] : results) {
        app_log->info("{}: {} ({} MB/s)", serial, libadb::to_string(r.status),
                      r.bytes / 1e6 / (r.duration.count() / 1000.0));
    }
    libadb::Client::shutdown();
}
```

---

## 3. C ABI

Только POD-типы, opaque-указатели, коды возврата; строки — `const char*` (UTF-8),
владение всегда остаётся у вызывающего либо освобождается парной `*_free`.

```c
// include/libadb/libadb_c.h
#ifdef __cplusplus
extern "C" {
#endif

typedef struct libadb_options libadb_options;
typedef struct libadb_device  libadb_device;
typedef struct libadb_results libadb_results;

typedef enum {
    LIBADB_OK = 0, LIBADB_ERR_INVALID_ARGUMENT, LIBADB_ERR_NOT_INITIALIZED,
    LIBADB_ERR_CONNECT_FAILED, LIBADB_ERR_CONNECT_TIMEOUT, LIBADB_ERR_AUTH_REQUIRED,
    LIBADB_ERR_OFFLINE, LIBADB_ERR_COMMAND_TIMEOUT, LIBADB_ERR_IO,
    LIBADB_ERR_REMOTE, LIBADB_ERR_LOCAL_FILE, LIBADB_ERR_UNSUPPORTED,
    LIBADB_ERR_CANCELED, LIBADB_ERR_INTERNAL
} libadb_status;

typedef enum { LIBADB_LOG_TRACE=0, LIBADB_LOG_DEBUG, LIBADB_LOG_INFO,
               LIBADB_LOG_WARN,  LIBADB_LOG_ERROR, LIBADB_LOG_OFF } libadb_log_level;

typedef enum { LIBADB_CMD_CONNECT=0, LIBADB_CMD_SHELL, LIBADB_CMD_PUSH,
               LIBADB_CMD_PULL, LIBADB_CMD_INSTALL, LIBADB_CMD_UNINSTALL } libadb_cmd;

typedef void (*libadb_log_fn)(libadb_log_level lvl, const char* serial,
                              const char* msg, void* user);
typedef void (*libadb_output_fn)(const char* serial, const char* data, size_t len,
                                 int is_stderr, void* user);
typedef void (*libadb_progress_fn)(const char* serial, uint64_t done, uint64_t total,
                                   void* user);

/* --- версия ------------------------------------------------------------- */
const char* libadb_version(void);
uint32_t    libadb_version_number(void);
const char* libadb_status_str(libadb_status);

/* --- опции -------------------------------------------------------------- */
libadb_options* libadb_options_new(void);
void libadb_options_free(libadb_options*);
void libadb_options_set_log_file(libadb_options*, const char* path);
void libadb_options_set_log_level(libadb_options*, libadb_log_level);
void libadb_options_set_log_rotation(libadb_options*, size_t max_size, size_t max_files);
void libadb_options_set_trace_tags(libadb_options*, const char* tags);
void libadb_options_set_log_callback(libadb_options*, libadb_log_fn, void* user);
void libadb_options_set_max_threads(libadb_options*, size_t n);
void libadb_options_set_timeout_ms(libadb_options*, libadb_cmd, uint32_t ms);
void libadb_options_set_default_port(libadb_options*, uint16_t);
void libadb_options_set_adb_key_path(libadb_options*, const char* path);

/* --- жизненный цикл ----------------------------------------------------- */
libadb_status libadb_init(const libadb_options*);   /* NULL = значения по умолчанию */
void          libadb_shutdown(void);
/* Изменение на ходу */
void libadb_set_max_threads(size_t n);
void libadb_set_timeout_ms(libadb_cmd, uint32_t ms);
void libadb_set_log_level(libadb_log_level);

/* --- одно устройство ---------------------------------------------------- */
libadb_status libadb_device_open(const char* address, libadb_device** out);
void          libadb_device_close(libadb_device*);
const char*   libadb_device_serial(const libadb_device*);

libadb_status libadb_push(libadb_device*, const char* local, const char* remote,
                          libadb_progress_fn, void* user, uint64_t* out_bytes);
libadb_status libadb_pull(libadb_device*, const char* remote, const char* local,
                          libadb_progress_fn, void* user, uint64_t* out_bytes);
libadb_status libadb_shell(libadb_device*, const char* command,
                           libadb_output_fn, void* user, int* out_exit_code);
libadb_status libadb_install(libadb_device*, const char* const* apks, size_t apk_count,
                             const char* const* flags, size_t flag_count,
                             libadb_output_fn, void* user);
libadb_status libadb_uninstall(libadb_device*, const char* package, int keep_data);
void          libadb_device_cancel(libadb_device*);

/* Последняя ошибка для текущего потока (валидна до следующего вызова API) */
const char* libadb_last_error(void);

/* --- пачка устройств ---------------------------------------------------- */
/* Выполняет команду на списке адресов, соблюдая max_threads.
   Результаты забираются из libadb_results; освобождать libadb_results_free. */
libadb_status libadb_push_all(const char* const* addresses, size_t count,
                              const char* local, const char* remote,
                              libadb_progress_fn, void* user, libadb_results** out);
size_t        libadb_results_count(const libadb_results*);
const char*   libadb_results_serial(const libadb_results*, size_t i);
libadb_status libadb_results_status(const libadb_results*, size_t i);
int           libadb_results_exit_code(const libadb_results*, size_t i);
uint64_t      libadb_results_bytes(const libadb_results*, size_t i);
uint64_t      libadb_results_duration_ms(const libadb_results*, size_t i);
const char*   libadb_results_error(const libadb_results*, size_t i);
void          libadb_results_free(libadb_results*);

#ifdef __cplusplus
}
#endif
```

Важно для C-потребителя: `libadb.so` внутри — C++ и тянет libstdc++. Для чистого C
это нормально (линкер подтянет `libstdc++.so.6` как зависимость `.so`), достаточно
`gcc main.c -ladb`.

---

## 4. Логирование

### 4.1 Два независимых канала

| Канал | Что | Настройка |
|---|---|---|
| Внутренний лог libadb | то, что сейчас идёт в `/tmp/adb.log` через `AdbLogger`/spdlog: транспорт, sockets, sync, ошибки протокола | `LogOptions` (путь, уровень, ротация, `trace_tags`) |
| События уровня приложения | то, что сейчас печатает `Logger` в `adirect`: «Connecting…», «Push successful, time: …» | `Options::event_sink` / `libadb_options_set_log_callback` |

Изменения в коде: путь `/tmp/adb.log` перестаёт быть константой `LOG_FILE_PATH` в
`adb/adb_trace.cpp`, sink создаётся в `Client::initialize()` из `LogOptions`;
`trace_tags` подаётся в существующий `setup_trace_mask()` вместо переменной окружения
(`ADB_TRACE` остаётся как override для отладки). `LogLevel` мапится в
`android::base::SetMinimumLogSeverity` + уровень spdlog.

### 4.2 spdlog без ABI-связывания

В `.so` уходит только `std::function`/C-callback. Удобный адаптер — отдельный
header-only файл, который компилируется в коде пользователя его версией spdlog:

```cpp
// include/libadb/spdlog_sink.hpp  (не часть ABI)
#pragma once
#include <memory>
#include <spdlog/spdlog.h>
#include "libadb/libadb.h"

namespace libadb {
inline LogSink make_spdlog_sink(std::shared_ptr<spdlog::logger> logger) {
    return [logger](LogLevel lvl, const std::string& serial, std::string_view msg) {
        spdlog::level::level_enum l = spdlog::level::info;
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

Если очень нужно передавать `std::shared_ptr<spdlog::logger>` напрямую — это возможно
только при жёсткой фиксации версии spdlog и одинаковых флагах компиляции у библиотеки и
приложения. Рекомендую не делать этого в ABI, а оставить адаптер выше.

---

## 5. Таймауты (потребует новой логики)

Сейчас реального таймаута нет — только `wait_for_device(15000)` в `adirect`.
Что нужно добавить:

1. `AdbSession::wait(std::chrono::milliseconds)` — вместо бесконечного
   `future.get()` использовать `wait_for`, по истечении — `abort()` и `Status::CommandTimeout`.
2. Для `push`/`pull`: сторожевой поток на команду. Важно, что таймаут должен быть
   «по прогрессу», а не только по общему времени, иначе на медленных группах устройств
   (см. историю с топологией) большие файлы будут ложно срываться. Предлагаю:
   общий дедлайн `Timeouts::push` + опционально `stall_timeout` (нет прогресса N мс).
3. `install`/`uninstall` — таймаут на `exec`-сессию `pm`.
4. `connect` — вынести существующее ожидание `kCsDevice` из `adirect` в
   `Client::connect()` с `Timeouts::connect`.
5. Отмена: `Device::cancel()` / `libadb_device_cancel()` — тот же механизм, что и таймаут
   (abort сессии), чтобы вызывающий мог прервать долгий push.

---

## 6. Параллелизм

Уже сделано: `AdbManager::setMaxThreads()` + `AdbManager::runOnDevices()`.
В публичном API это `Options::max_threads` / `Client::set_max_threads()` и
`Client::for_each()` / `*_all()`. Семантика: одновременно не более N устройств,
подключение к следующему — только после освобождения воркера.

Замечание из практики: `max_threads` полезно держать разным для разных групп устройств
(на общем узком канале любой `N > 1` только растягивает времена отдельных команд), поэтому
сеттер должен работать в рантайме, а не только в `initialize()`.

---

## 7. Версионирование `.so` в Linux — как это принято делать

### 7.1 Три имени одной библиотеки

```
libadb.so.1.2.0   ← real name, реальный файл
libadb.so.1       ← soname, симлинк; именно это имя ld.so ищет в рантайме
libadb.so         ← linker name, симлинк; нужен только при сборке (-ladb)
```

SONAME записан внутри `.so` (`readelf -d libadb.so.1.2.0 | grep SONAME`) и попадает в
`NEEDED` у приложения. Правило: **SONAME меняется только при несовместимом изменении ABI.**
Тогда старые бинарники продолжают работать со старой `libadb.so.1`, а новые берут
`libadb.so.2` — обе версии могут стоять в системе одновременно.

Semver для C/C++ библиотеки:
- **MAJOR** — сломали ABI (убрали/изменили символ, поменяли layout публичной структуры,
  сигнатуру, значения enum) → `SOVERSION` 1 → 2.
- **MINOR** — только добавили символы/поля в конце «расширяемых» структур → ABI совместим.
- **PATCH** — фиксы без изменения интерфейса.

В CMake:

```cmake
project(ADB VERSION 1.2.0)

add_library(adb_shared SHARED ${LIBADB_SOURCES})
set_target_properties(adb_shared PROPERTIES
    OUTPUT_NAME adb                # -> libadb.so
    VERSION   ${PROJECT_VERSION}   # -> libadb.so.1.2.0
    SOVERSION ${PROJECT_VERSION_MAJOR}   # -> SONAME libadb.so.1
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
target_link_options(adb_shared PRIVATE
    -Wl,--version-script=${CMAKE_SOURCE_DIR}/adb/lib/libadb.map
    -Wl,--exclude-libs,ALL          # не экспортировать символы статических зависимостей
    -Wl,--no-undefined
)
configure_file(include/libadb/version.h.in include/libadb/version.h @ONLY)
```

CMake сам создаёт все три симлинка при `install(TARGETS ...)`.

### 7.2 Version script (важнее, чем кажется)

`adb/lib/libadb.map` задаёт ровно то, что экспортируется, и заодно даёт symbol versioning:

```
LIBADB_1.0 {
  global:
    libadb_*;
    _ZN6libadb*;        /* или явный список C++ символов */
  local:
    *;                  /* всё остальное скрыть */
};

LIBADB_1.1 {
  global:
    libadb_set_stall_timeout_ms;
} LIBADB_1.0;
```

Без `local: *` из `libadb.so` наружу вылезут символы boringssl/protobuf/zstd/libbase и
начнут конфликтовать с теми же библиотеками в приложении — это самая частая причина
загадочных падений. Тяжёлые зависимости линкуем статически внутрь `libadb.so`
(boringssl, ziparchive, protobuf, zstd/lz4/brotli), либо оставляем как публичные `.so`
зависимости — но тогда это часть контракта, и их версии надо фиксировать.

### 7.3 Compile-time и runtime версия

```c
/* include/libadb/version.h (генерируется из version.h.in) */
#define LIBADB_VERSION_MAJOR @PROJECT_VERSION_MAJOR@
#define LIBADB_VERSION_MINOR @PROJECT_VERSION_MINOR@
#define LIBADB_VERSION_PATCH @PROJECT_VERSION_PATCH@
#define LIBADB_VERSION_STRING "@PROJECT_VERSION@"
#define LIBADB_VERSION_NUMBER ((@PROJECT_VERSION_MAJOR@<<16)|(@PROJECT_VERSION_MINOR@<<8)|@PROJECT_VERSION_PATCH@)
```

Приложение может проверить, что заголовки и `.so` совпадают:
`assert((libadb_version_number() >> 16) == LIBADB_VERSION_MAJOR)`.

### 7.4 Установка и поиск в рантайме

```cmake
install(TARGETS adb_shared EXPORT libadbTargets LIBRARY DESTINATION lib)
install(DIRECTORY include/libadb DESTINATION include)
install(EXPORT libadbTargets NAMESPACE libadb:: DESTINATION lib/cmake/libadb)
configure_file(adb/lib/libadb.pc.in libadb.pc @ONLY)
install(FILES ${CMAKE_BINARY_DIR}/libadb.pc DESTINATION lib/pkgconfig)
```

Потребитель: `pkg-config --cflags --libs libadb` или
`find_package(libadb 1.2 REQUIRED)` + `target_link_libraries(app libadb::adb)`.
Если библиотека ставится не в системный путь (`/opt/adb/lib`) — либо `ldconfig`-конфиг,
либо `-Wl,-rpath,'$ORIGIN/../lib'` у приложения.

### 7.5 Контроль совместимости в CI

- `abidiff` из libabigail (или `abi-compliance-checker`) сравнивает `.so` релизов и
  падает, если ABI сломан без поднятия MAJOR.
- Правила расширения структур: добавлять поля **только в конец** и никогда не менять
  порядок/типы; либо передавать структуры с полем `size_t struct_size` в начале
  (как в C API `libadb_options_*` — там структура скрыта за opaque-указателем, поэтому
  расширяется свободно; это ещё один аргумент за сеттеры вместо публичных структур).
- Публичные C++ структуры (`Options`, `Timeouts`, `Result`) по своей природе хрупкие к ABI.
  Если нужна долгая ABI-стабильность — C API считаем «стабильным контрактом», а C++ API
  «удобным, привязанным к MAJOR». Это стоит явно зафиксировать в README.

---

## 8. Что придётся поправить в существующем коде

1. `__adb_argv` / `__adb_envp` сейчас определены в `adirect.cpp` — переносим в библиотеку.
2. `adb_get_feature_set()` (`adb/lib/src/adb_client.cpp`) кеширует FeatureSet в глобальном
   `static` — при параллельной работе фичи одного устройства «протекают» на другие.
   Нужен `thread_local` или кеш на транспорт.
3. `AdbDevice::createSession()` аварийно закрывает прежние сессии устройства — мешает
   нескольким операциям на одном устройстве; переделать на честный список сессий.
4. Публичные заголовки `adb/lib/include/*` становятся внутренними
   (`adb/lib/src/internal/`), наружу — только `include/libadb/`.
5. `interface.h` — мёртвый дубль `IadbListener.h`, удалить.
6. Прогресс `push/pull`: `SyncConnection` сейчас работает в режиме `quiet=true`;
   нужно прокинуть колбэк вместо `LinePrinter`.
7. `mkdirs`-логика формирования пути pull (`<dir>/<ip>/<file>`) — это политика `adirect`,
   в библиотеку не тащим, оставляем в утилите.
8. Сборка: сейчас `adirect` собирается из общего списка исходников. Появляется
   промежуточная цель `adb_core` (STATIC, `-fPIC`), из неё — `libadb.so` (фасад + экспорт)
   и `adirect` (линкуется уже с `libadb.so`, чтобы API проверялся на себе).

---

## 9. Предлагаемый порядок работ

1. Выделить `adb_core` (STATIC, PIC) + `libadb.so` c `-fvisibility=hidden`, version script,
   SOVERSION; `adirect` переводим на линковку с `libadb.so`.
2. Публичные заголовки `include/libadb/libadb.h` + PIMPL-фасад `Client`/`Device`
   поверх `AdbManager`/`AdbDevice`; команды `push/pull/shell/install/uninstall`.
3. `LogOptions` (путь/уровень/ротация/trace_tags) + `LogSink`; убрать хардкод
   `/tmp/adb.log`; `spdlog_sink.hpp`.
4. Таймауты + `cancel()` (сессии, sync, connect).
5. C ABI (`libadb_c.h` + `c_api.cpp`), пример на чистом C в `examples/`.
6. Версионирование: `version.h.in`, `libadb.map`, `libadb.pc.in`, экспорт CMake,
   `abidiff` в CI, `CHANGELOG.md`.
7. `adirect` переписать на публичный API (проверка полноты API) + примеры в `examples/`.

---

## 10. Открытые вопросы к обсуждению

1. **Стабильный контракт — C или C++?** Предлагаю: C ABI — стабильный, C++ — привязан к MAJOR.
2. **`shell` только «выполнить и получить результат», или ещё интерактивный режим** (stdin,
   pty, длинные сессии типа `logcat`)? Сейчас в API заложен только первый вариант плюс
   стриминг вывода; интерактив можно добавить как `ShellSession` позже.
3. **Асинхронный вариант API** (возврат `std::future<Result>` / колбэк по завершении) —
   нужен или достаточно синхронных вызовов + `for_each` с пулом?
4. **Дефолтный путь лога.** Оставляем `/tmp/adb.log` как значение по умолчанию или
   требуем задавать явно?
5. **Таймаут push/pull**: только общий дедлайн, или ещё «нет прогресса N мс» (я за второй,
   с учётом истории с медленными группами)?
6. **Формат `install`**: оставляем «сырые» флаги `pm` строками (как сейчас) или делаем
   типизированные поля (`replace`, `grant_permissions`, `user`, `downgrade`)?
