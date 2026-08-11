# libadb — Руководство по использованию

**Версия:** 1.0.0 | **API:** C++20

Библиотека управляет Android-устройствами по TCP напрямую из кода — без adb-сервера.
Поддержаны: push/pull, shell, install, uninstall, события, таймауты, отмена, async.

---

## Установка

```bash
sudo dpkg -i libadb1_*.deb libadb-dev_*.deb
```

**CMake:**
```cmake
find_package(libadb 1.0 REQUIRED)
target_link_libraries(myapp PRIVATE libadb::adb)
```

**pkg-config:**
```bash
g++ -std=c++20 app.cpp $(pkg-config --cflags --libs libadb) -o app
```

---

## Быстрый старт

```cpp
#include "libadb/libadb.h"
int main() {
    auto& client = libadb::Client::instance();
    client.initialize();

    libadb::Status status;
    auto device = client.connect("192.168.1.10", &status);
    if (!device) { fprintf(stderr, "%s\n", libadb::to_string(status)); return 1; }

    auto r = device->shell("echo hello", {});
    printf("%s", r.output.c_str());
    return r.exit_code;
}
```

---

## Инициализация (Options)

```cpp
libadb::Options opts;

// Авторизация (§5)
opts.auth.key_files                   = {"/path/to/adbkey"};
opts.auth.private_keys_pem            = {pem_string};
opts.auth.use_default_key_store       = false;
opts.auth.generate_ephemeral_if_empty = true;
opts.auth.save_generated_key_to       = "/path/to/save";

// Таймауты (§6) — все ms{0} означает «без ограничения»
opts.timeouts.connect        = libadb::ms{20000};
opts.timeouts.shell          = libadb::ms{60000};
opts.timeouts.push.stall     = libadb::ms{30000};  // нет прогресса → StallTimeout
opts.timeouts.push.total     = libadb::ms{0};
opts.timeouts.install.commit = libadb::ms{15*60000};
opts.timeouts.slot_acquire   = libadb::ms{30000};  // ms::max() = бесконечно

// События (§8)
opts.on_event          = handler_fn;
opts.progress_interval = libadb::ms{200};
opts.event_queue_limit = 10000;

// Параллелизм
opts.max_connections      = 4;
opts.async_worker_threads = 4;
opts.max_parallel         = 0;  // 0 = без ограничения

// Логирование
opts.log.enabled   = true;
opts.log.file_path = "/var/log/adb.log";
opts.log.level     = libadb::LogLevel::Warn;
opts.log_sink      = my_sink_fn;

client.initialize(opts);  // можно повторно (обновляет настройки)
// → Status::InvalidArgument при ошибке ключа

// Диагностика загруженных ключей
for (auto& fp : libadb::auth_key_fingerprints())
    printf("key: %s\n", fp.c_str());
```

---

## Подключение

```cpp
libadb::Status status;
auto device = client.connect("192.168.1.10:5555", &status);
// Возможные статусы: ConnectTimeout, AuthRequired, Offline,
//                   SlotBusy, SlotTimeout, ConnectFailed
```

`AuthRequired` — устройство ждёт подтверждения ключа на экране.

---

## Push / Pull

```cpp
libadb::PushOptions opts;
opts.compression     = libadb::Compression::Any;
opts.sync_only_newer = true;
opts.on_progress = [](const std::string&, uint64_t done, uint64_t total) {
    if (total) printf("\r%llu%%", done * 100 / total);
};
opts.on_start = [](libadb::OperationId id, const std::string&) { /* для отмены */ };
opts.timeout  = libadb::TransferTimeout{.total = libadb::ms{0}, .stall = libadb::ms{30000}};

auto r = device->push("/local/file", "/remote/path", opts);
// r.transfer: bytes, bytes_on_wire, duration, mib_per_sec

auto r2 = device->pull("/remote/path", "/local/dir/", {});
```

---

## Shell

```cpp
libadb::ShellOptions opts;
opts.capture_output = true;
opts.on_output = [](const std::string&, std::string_view chunk, bool err) {
    fwrite(chunk.data(), 1, chunk.size(), err ? stderr : stdout);
};
opts.timeout = libadb::ms{10000};

auto r = device->shell("getprop ro.build.version.release", opts);
// r.exit_code, r.output

auto val = device->get_prop("ro.build.version.release");
```

---

## Установка APK (§10)

```cpp
libadb::InstallOptions opts;
opts.reinstall             = true;
opts.grant_permissions     = true;
opts.allow_downgrade_retry = true;
opts.on_conflict           = libadb::ConflictPolicy::Reinstall;
opts.package_name          = "com.example.app";
opts.package_name_source   = libadb::PackageNameSource::Both;
opts.kind                  = libadb::InstallKind::Auto;
// Auto: 1 файл→Single, N→SplitSet, *.apks/*.zip→Bundle (авто-распаковка)

libadb::InstallTimeout it;
it.commit                      = libadb::ms{15 * 60000};
it.commit_healthcheck          = libadb::HealthCheckMode::Transport;
it.commit_healthcheck_interval = libadb::ms{10000};
opts.timeout = it;

auto r = device->install("/tmp/app.apk", opts);
// r.remote_code: "INSTALL_FAILED_UPDATE_INCOMPATIBLE" …  r.retries: повторы

device->install("/tmp/app.apks");                              // Bundle
device->install({"base.apk", "split_arm64.apk"}, opts);       // SplitSet
libadb::InstallOptions mo; mo.kind = libadb::InstallKind::MultiPackage;
device->install({"a.apk", "b.apk"}, mo);
```

**pm → Status:** `INSTALL_FAILED_UPDATE_INCOMPATIBLE`→SignatureMismatch |
`INSTALL_FAILED_VERSION_DOWNGRADE`→VersionDowngrade |
`INSTALL_FAILED_INSUFFICIENT_STORAGE`→InsufficientStorage |
`INSTALL_PARSE_FAILED_*`/`INSTALL_FAILED_INVALID_APK`→InvalidApk |
`INSTALL_FAILED_MISSING_SPLIT`→MissingSplit | остальные→RemoteError

---

## Удаление

```cpp
libadb::UninstallOptions opts;
opts.keep_data = false;
opts.user_id   = 0;   // -1 = не указывать --user
device->uninstall("com.example.app", opts);
```

---

## События (§8)

```cpp
opts.on_event = [](const libadb::Event& e) {
    // e.type, e.serial, e.op, e.command, e.phase
    // e.bytes_done / e.bytes_total    — OperationProgress
    // e.stats.mib_per_sec             — OperationStats / Finished
    // e.message                       — Failed / Retry / Heartbeat
};

auto sid = client.subscribe(fn,
    libadb::event_bit(libadb::EventType::OperationProgress) |
    libadb::event_bit(libadb::EventType::OperationFinished));
client.unsubscribe(sid);
client.set_progress_interval(libadb::ms{500});
```

Доставка из **отдельного диспетчер-потока** — не блокируйте обработчик надолго.

---

## Асинхронный режим (§9)

```cpp
auto op = device->push_async("/tmp/file", "/data/local/tmp/file");
op->wait();                    // бесконечно
op->wait(libadb::ms{5000});   // false — не дождались
op->cancel();
// Status::DeviceBusy — если Device уже занят

auto batch = client.push_all_async(
    {"192.168.1.10","192.168.1.11"}, "/local", "/remote");
batch->wait();
for (auto& [addr, r] : batch->results())
    printf("%s: %s\n", addr.c_str(), libadb::to_string(r.status));
```

---

## Отмена (§9)

```cpp
client.cancel(op_id);      // по OperationId (из on_start / Event::op)
client.cancel_all();       // все операции, соединения остаются
client.close_all();        // разорвать все соединения → ConnectionClosed
device->cancel_current();  // операции этого устройства
```

---

## Лимит подключений (§7)

```cpp
opts.max_connections       = 4;
opts.timeouts.slot_acquire = libadb::ms{30000};   // ms::max() = бесконечно
client.set_max_connections(8);
client.active_connections();
// slot_acquire = 0 → немедленный SlotBusy
```

---

## Логирование

```cpp
opts.log.enabled   = true;           // канал 1: файл
opts.log.file_path = "/var/log/adb.log";
opts.log.level     = libadb::LogLevel::Warn;
opts.log_sink = [](libadb::LogLevel l, const std::string& serial,
                   std::string_view msg) { /* канал 2: в приложение */ };

// spdlog-адаптер (не тянет spdlog в ABI)
#include "libadb/spdlog_sink.hpp"
opts.log_sink = libadb::make_spdlog_sink(spdlog::default_logger());
```

---

## Версионирование

```cpp
libadb::version();          // "1.0.0"
libadb::version_number();   // 0x010000
assert((libadb::version_number() >> 16) == LIBADB_VERSION_MAJOR);
```

SONAME `libadb.so.1` — C++ ABI совместим внутри одного MAJOR.

---

## Справочник статусов

| Status | Смысл |
|---|---|
| Ok | успех |
| InvalidArgument | неверный аргумент / ошибка ключа |
| NotInitialized | `initialize()` не был вызван |
| NotImplemented | `ReinstallKeepData` (зарезервировано) |
| ConnectFailed / ConnectTimeout | нет ответа / таймаут |
| AuthRequired | ждёт подтверждения ключа на экране |
| Offline / DeviceLost | устройство отпало |
| SlotBusy / SlotTimeout | лимит подключений |
| DeviceBusy | второй `*_async` на занятом Device |
| CommandTimeout / StallTimeout | таймаут по времени / прогрессу |
| Canceled / ConnectionClosed | явная отмена / `close_all()` |
| IoError / LocalFileError | ошибка канала / нет файла |
| RemoteError | отказ pm/adbd |
| SignatureMismatch / VersionDowngrade | конфликт подписи / понижение версии |
| InsufficientStorage / InvalidApk / MissingSplit | ошибки установки |

---

## Потокобезопасность

- `Client` — все методы безопасны из любого потока.
- `Device` — **не** потокобезопасен: одна операция за раз.
- `on_event` — диспетчер-поток, не блокировать надолго.

---

## Примеры (`examples/`)

| Файл | Содержание |
|---|---|
| `cpp/push.cpp`    | push с прогрессом |
| `cpp/shell.cpp`   | потоковый вывод shell |
| `cpp/install.cpp` | APK + ConflictPolicy |
| `cpp/events.cpp`  | события, on_start, таймауты |
| `cpp/async.cpp`   | асинхронный батч |
| `c/basic.cpp`     | C-стиль кода |

```bash
# против установленного пакета
cmake -S examples -B build-ex && cmake --build build-ex
# вместе с библиотекой
cmake -S . -B build -DBUILD_EXAMPLES=ON && cmake --build build
```
