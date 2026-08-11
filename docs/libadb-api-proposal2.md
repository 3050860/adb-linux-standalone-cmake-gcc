# libadb.so — предложение по публичному API, версия 2

Статус: **черновик для ревью**. Код ещё не менялся.
Предыдущая версия: `docs/libadb-api-proposal.md` (§7 «Версионирование» и §8 «Что править в
коде» оттуда остаются в силе и здесь не дублируются, кроме уточнений в §11).

## 0. Что изменилось относительно v1

| № замечания | Что сделано |
|---|---|
| 1 | Ключ авторизации: путь(и) к файлам **и** текст PEM напрямую (§3) |
| 2 | Таймауты передачи: `total` + `stall`, оба опциональны и комбинируются; для `install` — по фазам + health-check устройства во время `commit` (§4) |
| 3 | Split APK (`install-multiple`/session), `.apks`, переустановка при конфликте подписи (§5) |
| 4 | Явные синхронный и асинхронный режимы, `Operation`-хендл, отмена из другого потока (§6) |
| 5 | `read_device_list()` убран. Два режима: батч по списку адресов и одиночное устройство с глобальным лимитом подключений и таймаутом ожидания слота (§7) |
| 6 | Система событий (§8) |
| 7 | `Client::close_all()` — принудительный обрыв всех соединений (§9) |
| вопрос 2 | Длинные сессии (`logcat`) — `ShellSession` (§10) |
| вопрос 1 | Объяснение «стабильного контракта» (§11) |
| вопрос 4 | `/tmp/adb.log` — дефолт, внутренний лог по умолчанию **выключен**, файл при этом не открывается (§2) |
| вопрос 6 | `install` — сырые флаги `pm` (§5) |

---

## 1. Общая схема

```
                 ваш код (C++ или C)
                        │
        ┌───────────────┴───────────────┐
        │  libadb::Client (синглтон)    │  опции, лимит подключений, события
        └───────────────┬───────────────┘
              ┌─────────┴─────────┐
              │  libadb::Device   │  занимает слот подключения
              └─────────┬─────────┘
        ┌───────────────┼────────────────────┐
   Result push()   Operation push_async()  ShellSession (logcat)
   (синхронно)     (асинхронно, cancel)    (стрим, живёт долго)
```

Три способа выполнить работу:
1. **Синхронно** — `Result r = dev->push(...)`, поток блокируется.
2. **Асинхронно** — `auto op = dev->push_async(...)`, работа уходит в пул библиотеки,
   `op->cancel()` / `op->wait(timeout)` / `op->result()`.
3. **Батч** — `Client::push_all(addresses, ...)` (синхронно, вернёт все результаты) или
   `Client::push_all_async(...)` → `BatchOperation`. Параллелизм = `max_connections`.

---

## 2. Логирование

Два независимых канала (как в v1), с уточнениями:

```cpp
enum class LogLevel { Trace = 0, Debug, Info, Warn, Error, Off };

struct LogOptions {
    bool        enabled       = false;              // ВЫКЛЮЧЕН по умолчанию
    std::string file_path     = "/tmp/adb.log";     // дефолт, переопределяется
    LogLevel    level         = LogLevel::Warn;
    size_t      max_file_size = 5 * 1024 * 1024;
    size_t      max_files     = 3;
    std::string trace_tags;                         // как ADB_TRACE: "sync,transport" | "all"
    bool        also_stderr   = false;
};
```

`enabled = false` (дефолт) означает: sink не создаётся, файл **не открывается и не
создаётся вообще**, а `LOG()/VLOG()` внутри библиотеки уходят в no-op sink. Включить
можно и в рантайме: `Client::set_log_options(...)` — тогда файл открывается лениво.
Переменная окружения `ADB_TRACE` остаётся как аварийный override для отладки
(если задана — включает лог даже при `enabled = false`).

Второй канал — сообщения уровня приложения (`Connecting…`, `Push successful, time: …`):
`LogSink` (см. v1 §4.2, spdlog-адаптер отдельным inline-заголовком, spdlog не в ABI).
Формально он избыточен рядом с системой событий (§8), но удобен, поэтому остаётся;
можно использовать любой из двух или оба.

---

## 3. Авторизация (замечание 1)

```cpp
struct AuthOptions {
    // Приватные ключи из файлов (как ~/.android/adbkey). Порядок = порядок попыток.
    std::vector<std::string> key_files;

    // Приватные ключи прямо текстом (PEM, PKCS#8). Вызывающий берёт их откуда угодно:
    // из своей БД, из vault, из зашитого ресурса.
    std::vector<std::string> private_keys_pem;

    // Использовать ли стандартный набор (~/.android/adbkey, $ADB_VENDOR_KEYS).
    bool use_default_key_store = true;

    // Если ни одного ключа не нашлось — сгенерировать новый в памяти (никуда не пишем).
    bool generate_ephemeral_if_empty = true;

    // Записать сгенерированный ключ по этому пути (пусто = не записывать).
    std::string save_generated_key_to;
};
```

Реализация: сейчас `adb_auth_init()` (`adb/client/auth.cpp`) умеет только читать файлы.
Добавляем загрузку из памяти — `crypto::Key` создаётся через `BIO_new_mem_buf` +
`PEM_read_bio_PrivateKey`, публичный ключ выводится из приватного (как и сейчас для
`adbkey.pub`). Ключи складываются в тот же список, из которого `adb_auth` берёт кандидатов,
поэтому дальше всё работает без изменений. Ошибка разбора PEM → `Status::InvalidArgument`
из `Client::initialize()` с текстом, какой именно ключ не разобрался.

Событие `EventType::AuthRequired` (§8) приходит, если ни один ключ не подошёл и
устройство ждёт подтверждения на экране.

---

## 4. Таймауты (замечание 2)

### 4.1 Базовые типы

```cpp
using ms = std::chrono::milliseconds;

// Для операций с измеримым прогрессом (push/pull и фаза передачи install).
struct TransferTimeout {
    ms total{0};        // дедлайн на всю операцию; 0 = не ограничивать
    ms stall{30'000};   // нет ни одного байта прогресса дольше этого; 0 = не ограничивать
    // Оба могут быть заданы одновременно — срабатывает тот, который наступит раньше.
    // Оба нулевые = без таймаута.
};

struct Timeouts {
    ms connect{15'000};        // до состояния "device" (включая auth)
    ms slot_acquire{30'000};   // ожидание свободного слота подключения (§7);
                               // 0 = не ждать вовсе, ms::max() = ждать бесконечно
    ms shell{60'000};          // обычная shell-команда (для ShellSession — см. §10)
    TransferTimeout push{ .total = ms{0},  .stall = ms{30'000} };
    TransferTimeout pull{ .total = ms{0},  .stall = ms{30'000} };
    InstallTimeout  install{};
    ms uninstall{120'000};
    ms close{5'000};           // грациозное закрытие соединения
};
```

Почему `stall` по умолчанию, а `total` — нет: история с «медленными» устройствами на общем
канале показала, что абсолютное время передачи одного и того же файла легко отличается в
4 раза только из-за топологии сети и числа одновременных операций. Жёсткий `total`
в такой ситуации даёт ложные срывы, а `stall` ловит именно то, что нужно — «канал умер».

### 4.2 `install` — фазы (это самое интересное)

`install` не монолитен, у него принципиально разные по наблюдаемости фазы:

| Фаза | Что происходит | Виден ли прогресс | Таймаут |
|---|---|---|---|
| `Prepare` | распаковка `.apks`, чтение локальных файлов | локальная работа | `prepare` |
| `CreateSession` | `pm install-create` | быстрый ответ | `create_session` |
| `Transfer` | `pm install-write` / streamed install: байты идут на устройство | **да, есть прогресс** | `transfer` = `{total, stall}` |
| `Commit` | `pm install-commit`: устройство верифицирует, dexopt, ставит | **нет, только ожидание ответа** | `commit` (только `total`) + health-check |
| `Abandon` | откат сессии при ошибке | быстрый | `create_session` |

```cpp
struct InstallTimeout {
    ms prepare{60'000};
    ms create_session{30'000};
    TransferTimeout transfer{ .total = ms{0}, .stall = ms{30'000} };

    // Ожидание ответа pm после коммита. Прогресса нет, поэтому только общий дедлайн.
    // Ставим щедрое значение: dexopt большого приложения на слабом устройстве — минуты.
    ms commit{15 * 60'000};

    // Пока ждём commit, раз в этот интервал проверяем, что устройство живо
    // (короткая служебная shell-сессия). Если устройство перезагрузилось/отвалилось,
    // обрываем сразу со Status::DeviceLost, не досиживая 15 минут.
    // 0 = не проверять.
    ms commit_healthcheck_interval{10'000};

    // Сколько неудачных health-check подряд считать потерей устройства.
    int commit_healthcheck_failures = 3;
};
```

Ключевая идея для `commit`: **вместо угадывания «сколько ставится пакет» мы отличаем
«долго ставится» от «устройство умерло»**. Первое — нормально и ждём до `commit`,
второе — обрываем сразу. Health-check намеренно делается отдельной короткой сессией
(`shell true` или `host:version`-подобный запрос по тому же транспорту): он не мешает
установке и стоит копейки. Плюс на каждый health-check летит событие
`OperationHeartbeat`, так что вызывающий код видит «оно живое, ещё ставит».

Диагностика при срабатывании таймаута всегда включает фазу и накопленную статистику:
`Result::error = "install timeout in phase Commit after 900000 ms (transferred 42.1 MB in 3.2 s)"`,
плюс `Result::phase` и событие `OperationTimeout` с тем же наполнением.

---

## 5. Установка: split APK и переустановка (замечание 3)

### 5.1 Что именно надо поддержать

1. Один `.apk` → `pm install` (или `install-create`/`write`/`commit`, единообразно).
2. **Набор split-APK** (`base.apk` + `split_config.arm64_v8a.apk` + …) →
   сессия `pm install-create --multi-package`-стиля: `install-create` → по одному
   `install-write` на каждый файл → `install-commit`. Это то, что делает `adb install-multiple`.
3. `.apks` (бандл bundletool) → распаковать (`expandApks()` уже есть в `AdbInstaller`) и
   поставить как набор split из п.2.
4. `.apex` — упоминаю для полноты, отдельным флагом `--apex`, если понадобится.

```cpp
enum class InstallKind {
    Auto,        // определить по входу: 1 apk -> Single; N apk -> SplitSet; *.apks -> Bundle
    Single,
    SplitSet,    // несколько частей ОДНОГО пакета (base + splits)
    Bundle,      // .apks/.zip, распаковать и поставить как SplitSet
    MultiPackage // несколько независимых пакетов в одной атомарной сессии (pm --multi-package)
};
```

`Auto` покрывает 99% случаев, явное указание — чтобы не гадать в спорных ситуациях
(например, несколько независимых apk, которые надо поставить атомарно).

### 5.2 Конфликт подписи и переустановка

Факт: **если пакет уже установлен и подписан другим ключом, никакой флаг `pm` не поможет** —
`pm install -r` вернёт `INSTALL_FAILED_UPDATE_INCOMPATIBLE` («signatures do not match
previously installed version»). Единственный путь на не-root устройстве — сначала удалить
пакет, потом поставить заново. Данные при этом теряются (`pm uninstall -k` формально
сохраняет данные, но с чужой подписью он всё равно не спасает и на многих прошивках
объявлен нерабочим — на него полагаться нельзя, и это надо честно задокументировать).

Поэтому предлагаю политику, а не флаг:

```cpp
enum class ConflictPolicy {
    Fail,                    // (дефолт) вернуть ошибку с кодом pm
    Reinstall,               // uninstall + install заново (данные приложения теряются)
    ReinstallKeepData,       // попытаться uninstall -k + install; если не вышло — как Reinstall
};

struct InstallOptions {
    InstallKind kind = InstallKind::Auto;

    // Сырые флаги pm, как сейчас: "-r", "-g", "-d", "-t", "--user", "0", ...
    std::vector<std::string> flags;

    ConflictPolicy on_conflict = ConflictPolicy::Fail;

    // Нужен для on_conflict != Fail (что удалять).
    // Если пусто — библиотека попытается определить сама (см. ниже).
    std::string package_name;

    // Автоматически добавлять -d при INSTALL_FAILED_VERSION_DOWNGRADE и повторять.
    bool allow_downgrade_retry = false;

    // Ставить в конкретного пользователя (сахар над --user N; -1 = не указывать).
    int user_id = -1;

    OutputFn on_output;      // сырой вывод pm
    ProgressFn on_progress;  // фаза Transfer
};
```

Как определяется `package_name`, если не задан (по убыванию надёжности):
1. Из текста ошибки `pm` — она обычно содержит имя пакета.
2. Разбором `AndroidManifest.xml` из APK: `libziparchive` в проекте уже есть, нужен
   маленький парсер бинарного AXML только для атрибута `package` (~150 строк).
   Предлагаю сделать это на шаге 2 реализации, а на шаге 1 — требовать явное
   `package_name` для `Reinstall*` и возвращать `Status::InvalidArgument` без него.

Что попадает в `Result`, чтобы вызывающий мог решать сам:

```cpp
struct Result {
    Status status = Status::Ok;
    int exit_code = 0;
    std::string error;              // человекочитаемо
    std::string remote_code;        // "INSTALL_FAILED_UPDATE_INCOMPATIBLE" и т.п.
    std::string output;             // сырой вывод pm/shell (если capture_output)
    uint64_t bytes = 0;
    ms duration{0};
    Phase phase = Phase::None;      // на какой фазе закончили/сломались
    int retries = 0;                // сколько раз переустанавливали/повторяли
    bool ok() const { return status == Status::Ok; }
};
```

Распознаём и раскладываем в `Status` + `remote_code` как минимум:
`INSTALL_FAILED_UPDATE_INCOMPATIBLE`, `INSTALL_FAILED_VERSION_DOWNGRADE`,
`INSTALL_FAILED_INSUFFICIENT_STORAGE`, `INSTALL_FAILED_INVALID_APK`,
`INSTALL_FAILED_MISSING_SPLIT`, `INSTALL_PARSE_FAILED_*`, `INSTALL_FAILED_TEST_ONLY`
(подсказка: добавить `-t`), `INSTALL_FAILED_USER_RESTRICTED`.
Отдельный `Status::SignatureMismatch` — чтобы не заставлять всех парсить строки.

Каждая переустановка сопровождается событиями `OperationRetry` (с причиной) и
`OperationPhaseChanged`, чтобы в логе вызывающего было видно, почему пакет снесли.

---

## 6. Синхронно / асинхронно и отмена (замечание 4)

В v1 `cancel()` действительно был бессмысленным: синхронный `push()` держит поток, а
отменять некому. Исправляю: у каждой операции есть идентификатор и хендл.

```cpp
using OperationId = uint64_t;

enum class Command { Connect, Shell, Push, Pull, Install, Uninstall, ShellSession };
enum class Phase   { None, Connecting, Prepare, CreateSession, Transfer, Commit,
                     Finalize, Abandon };

// Хендл асинхронной операции. Живёт независимо от Device.
class LIBADB_API Operation {
public:
    OperationId id() const;
    Command command() const;
    const std::string& serial() const;

    bool  done() const;
    Phase phase() const;
    // Ждать завершения. timeout=0 -> бесконечно. Возвращает false, если не дождались.
    bool  wait(ms timeout = ms{0});
    Result result() const;          // валиден после done() == true

    void cancel();                  // идемпотентно, можно из любого потока
    bool canceled() const;
};
using OperationPtr = std::shared_ptr<Operation>;
```

### 6.1 Синхронный режим (по умолчанию, как сейчас в adirect)

```cpp
Result r = dev->push({"main.apk"}, "/data/local/tmp/main.apk");
```
Поток блокируется до конца/таймаута. Отменить можно из **другого** потока, и для этого
нужен идентификатор — он выдаётся до начала работы:

```cpp
struct PushOptions {
    Compression compression = Compression::Any;
    bool sync = false;
    ProgressFn on_progress;
    std::function<void(OperationId)> on_start;   // вызывается ДО начала передачи
    std::optional<TransferTimeout> timeout;      // переопределить Timeouts::push
};
...
OperationId id = 0;
PushOptions o; o.on_start = [&](OperationId i){ id = i; };  // сохранить куда-то
Result r = dev->push(..., o);        // блокируется
// из другого потока:
libadb::Client::instance().cancel(id);
```

Плюс два удобных сокращения, чтобы не таскать id в простых случаях:
`dev->cancel_current()` (отменить то, что сейчас выполняется на этом Device) и
`Client::cancel_all()`. Событие `OperationStarted` (§8) тоже несёт `OperationId`, так что
подписчик событий получает id без `on_start`.

### 6.2 Асинхронный режим

```cpp
auto op = dev->push_async({"main.apk"}, "/data/local/tmp/main.apk", opts);
// ... своя работа ...
if (!op->wait(ms{5000})) op->cancel();
Result r = op->result();
```
Операция выполняется в пуле библиотеки. `Device` в это время занят: второй
`*_async` на том же `Device` вернёт операцию, завершённую с `Status::DeviceBusy`
(последовательность команд на одном устройстве — забота вызывающего; исключение —
`ShellSession`, см. §10).

### 6.3 Батч

```cpp
class LIBADB_API BatchOperation {
public:
    bool wait(ms timeout = ms{0});
    bool done() const;
    size_t total() const, finished() const;
    std::map<std::string, Result> results() const;   // готовые на момент вызова
    void cancel();                                   // отменить всё незавершённое
    std::vector<OperationPtr> operations() const;    // для точечной отмены
};
using BatchOperationPtr = std::shared_ptr<BatchOperation>;
```

### 6.4 Семантика отмены

`cancel()` → abort сессии/транспорта операции → операция завершается с
`Status::Canceled` «в течение сотен миллисекунд» (гарантию «мгновенно» не даём: рвём
socket, ждём выхода из блокирующего чтения). Отмена на фазе `Commit` установки **не
отменяет установку на устройстве** — pm уже получил команду; это честно документируем,
`Result::error` это поясняет.

---

## 7. Устройства, слоты и лимит подключений (замечание 5)

`read_device_list()` удалён. Адрес — отдельный тип, чтобы не разбирать строки на каждом шагу:

```cpp
struct DeviceAddress {
    std::string host;                 // "192.168.151.231"
    uint16_t    port = 0;             // 0 -> Options::default_port (5555)
    DeviceAddress(std::string host, uint16_t port = 0);
    static DeviceAddress parse(std::string_view s);   // "1.2.3.4" | "1.2.3.4:5555"
    std::string to_string() const;                    // всегда с портом
};
```

### Режим 1 — батч по списку адресов

```cpp
std::vector<DeviceAddress> devices = { {"192.168.151.231"}, {"192.168.151.232"}, ... };

auto results = client.push_all(devices, {"main.apk"}, "/data/local/tmp/main.apk");
// или асинхронно:
auto batch = client.push_all_async(devices, {"main.apk"}, "/data/local/tmp/main.apk");
```
Параллелизм ограничен `max_connections`; подключение к очередному адресу — только после
освобождения слота (это уже реализовано в `AdbManager::runOnDevices`).
Универсальная форма — `for_each`:

```cpp
using DeviceTask = std::function<Result(Device&)>;
std::map<std::string, Result> for_each(const std::vector<DeviceAddress>&, const DeviceTask&);
BatchOperationPtr for_each_async(const std::vector<DeviceAddress>&, const DeviceTask&);
```

### Режим 2 — одно устройство из своего потока

```cpp
Status st;
auto dev = client.connect({"192.168.151.231"}, &st);
if (!dev) {
    if (st == Status::SlotTimeout) { /* все слоты заняты, подождать и повторить */ }
    return;
}
auto r = dev->shell("getprop ro.product.model");
dev->close();     // или просто выход DevicePtr из области видимости
```

Правила слотов:
- `Options::max_connections` (переименовано из `max_threads`, т.к. речь о подключениях,
  а не о потоках; `0` = без ограничения) — **глобальный** лимит на процесс, общий для
  батч-режима и ручных `connect()`.
- Слот занимается в `connect()` и освобождается при `close()`/разрушении `Device`.
- Если слотов нет, `connect()` **ждёт не дольше `Timeouts::slot_acquire`** и возвращает
  `nullptr` + `Status::SlotTimeout`. `slot_acquire = 0` → не ждать вообще (сразу
  `Status::SlotBusy`), `ms::max()` → ждать бесконечно. Дефолт 30 с — то есть «повиснуть»
  по умолчанию нельзя.
- События `SlotWaiting` / `SlotAcquired` / `SlotTimeout` — видно, что упёрлись в лимит.
- Изменение `set_max_connections()` в рантайме: уменьшение не рвёт существующие
  подключения, просто новые слоты не выдаются, пока число не опустится ниже лимита.

---

## 8. События (замечание 6)

Машинно-читаемые уведомления обо всём, что происходит, включая таймауты и ошибки.

```cpp
enum class EventType {
    DeviceConnecting, DeviceConnected, DeviceAuthRequired, DeviceUnauthorized,
    DeviceDisconnected, DeviceLost,
    SlotWaiting, SlotAcquired, SlotTimeout,
    OperationStarted, OperationPhaseChanged, OperationProgress, OperationHeartbeat,
    OperationRetry, OperationOutput, OperationFinished, OperationTimeout,
    OperationCanceled, OperationFailed,
    ClientShutdown, InternalError,
};

struct Event {
    EventType     type;
    ms            timestamp;            // от старта клиента (steady)
    std::string   serial;               // "" для событий уровня клиента
    OperationId   op = 0;               // 0, если не относится к операции
    Command       command = Command::Connect;
    Phase         phase = Phase::None;
    Status        status = Status::Ok;
    std::string   remote_code;          // например INSTALL_FAILED_*
    uint64_t      bytes_done = 0;
    uint64_t      bytes_total = 0;
    ms            elapsed{0};
    std::string   message;              // человекочитаемая деталь
};

using EventFn = std::function<void(const Event&)>;
using SubscriptionId = uint64_t;

// Подписчиков может быть несколько (например, метрики + лог).
SubscriptionId Client::subscribe(EventFn, EventMask mask = EventMask::all());
void           Client::unsubscribe(SubscriptionId);
```

Гарантии доставки, которые предлагаю зафиксировать в документации:
- События доставляются из **отдельного диспетчер-потока**, а не из fdevent-потока и не из
  рабочего потока операции. Это принципиально: медленный обработчик не должен тормозить
  протокол (мы это уже проходили — синхронный flush логов в looper-потоке).
- Порядок событий в рамках одной `OperationId` сохраняется; между разными операциями —
  не гарантируется.
- `OperationProgress` **троттлится**: `Options::progress_interval{ms 200}` и/или
  `progress_min_bytes`. Иначе на гигабитном канале это миллионы событий.
- Очередь событий ограничена (`Options::event_queue_limit = 10000`); при переполнении
  сначала выбрасываются `OperationProgress`/`Heartbeat` (и в конце приходит
  `InternalError` с числом потерянных), критичные события не теряются.
- Обработчик вызывается для всех подписчиков последовательно; блокировать его надолго
  нельзя (документируем; исключения из обработчика ловим и превращаем в `InternalError`).

`LogSink` из §2 остаётся как «человеческий» канал: по умолчанию, если задан `event_sink`
и не задан `LogSink`, библиотека сама форматирует события в текст.

---

## 9. Клиент (замечания 5 и 7)

```cpp
struct Options {
    LogOptions   log;
    AuthOptions  auth;
    Timeouts     timeouts;
    LogSink      log_sink;
    EventFn      on_event;
    EventMask    event_mask = EventMask::all();
    size_t       max_connections = 0;      // 0 = без ограничения
    uint16_t     default_port = 5555;
    ms           progress_interval{200};
    size_t       event_queue_limit = 10'000;
    size_t       worker_threads = 0;       // пул для *_async; 0 = max_connections или 4
};

class LIBADB_API Client {
public:
    static Status  initialize(const Options&);
    static void    shutdown();                  // = close_all() + остановка loop
    static bool    is_initialized();
    static Client& instance();

    // ---- опции в рантайме
    void set_max_connections(size_t);
    size_t max_connections() const;
    void set_timeouts(const Timeouts&);
    Timeouts timeouts() const;
    void set_log_options(const LogOptions&);
    void set_log_level(LogLevel);
    void set_log_sink(LogSink);
    SubscriptionId subscribe(EventFn, EventMask = EventMask::all());
    void unsubscribe(SubscriptionId);

    // ---- режим 2: одно устройство
    DevicePtr connect(const DeviceAddress&, Status* out = nullptr);
    void      disconnect(const DeviceAddress&);
    std::vector<std::string> connected_devices() const;
    size_t    busy_slots() const;

    // ---- режим 1: батч
    std::map<std::string, Result> push_all(const std::vector<DeviceAddress>&,
        const std::vector<std::string>& local, const std::string& remote,
        const PushOptions& = {});
    std::map<std::string, Result> pull_all(...);
    std::map<std::string, Result> shell_all(...);
    std::map<std::string, Result> install_all(...);
    std::map<std::string, Result> uninstall_all(...);
    std::map<std::string, Result> for_each(const std::vector<DeviceAddress>&, const DeviceTask&);
    // ... и *_async варианты, возвращающие BatchOperationPtr

    // ---- отмена и аварийное закрытие (замечание 7)
    void cancel(OperationId);
    void cancel_all();               // отменить все операции, соединения оставить
    // Принудительно порвать ВСЕ соединения. Все текущие операции немедленно
    // завершаются со Status::ConnectionClosed, слоты освобождаются,
    // ShellSession'ы закрываются, летят события DeviceDisconnected.
    void close_all();
};
```

`close_all()` и `cancel_all()` можно звать из любого потока, в том числе из обработчика
событий. Синхронные вызовы, ждавшие в `push()`, вернутся с `Status::ConnectionClosed`.
Отдельный код ошибки (не `Canceled`) — чтобы вызывающий отличал «я сам отменил» от
«клиент закрывают/аварийно рвут».

---

## 10. Длинные сессии: `logcat` и прочий стрим (вопрос 2)

Отдельный объект, живущий сколько нужно, без общего таймаута:

```cpp
struct ShellSessionOptions {
    bool     want_stdin = false;      // писать в stdin (например, `sh -`)
    bool     separate_stderr = true;  // shell_v2, если устройство поддерживает
    ms       idle_timeout{0};         // нет данных дольше N мс -> закрыть; 0 = никогда
    ms       total_timeout{0};        // общий дедлайн; 0 = никогда (для logcat так и надо)
    OutputFn on_output;               // push-режим: колбэк на каждый чанк
    size_t   buffer_limit = 1 << 20;  // лимит внутреннего буфера для pull-режима
};

class LIBADB_API ShellSession {
public:
    OperationId id() const;
    bool alive() const;

    // pull-режим: читать самому, удобно для C и для своих циклов.
    // Возвращает число байт; 0 = таймаут ожидания; -1 = сессия закрыта.
    ssize_t read(char* buf, size_t len, bool* is_stderr = nullptr, ms timeout = ms{0});

    bool write(const void* data, size_t len);   // stdin, если want_stdin
    bool send_signal(int sig);                  // SIGINT для прерывания logcat
    void close();                               // = cancel
    bool wait(ms timeout = ms{0});              // дождаться завершения команды
    int  exit_code() const;
};
using ShellSessionPtr = std::shared_ptr<ShellSession>;

// на Device:
ShellSessionPtr Device::open_shell(const std::string& command,
                                   const ShellSessionOptions& = {});
```

Особенности:
- `ShellSession` **не блокирует** `Device`: можно держать открытый `logcat` и параллельно
  делать `push` на том же устройстве (сейчас `AdbDevice::createSession()` этому мешает —
  см. v1 §8.3, надо править).
- Работает и в push-режиме (`on_output`), и в pull-режиме (`read()`), на выбор.
- `idle_timeout` полезен как раз для `logcat`: «если сутки нет ни строчки — что-то не так».
- Отмена = `close()`, идентификатор тот же `OperationId`, так что `Client::cancel(id)`
  и `close_all()` тоже её закрывают.
- Под `logcat` отдельного API не делаем — это просто `open_shell("logcat -v threadtime")`.
  При желании позже добавим сахар `Device::logcat(...)` с разбором строк.

---

## 11. Что значит «стабильный контракт» (вопрос 1)

Речь про **ABI** — двоичную совместимость. Контракт: «приложение, собранное с libadb
версии X, продолжит работать с новой `libadb.so` без перекомпиляции».

Что ломает ABI (даже если код компилируется):
- убрали/переименовали экспортируемую функцию;
- поменяли сигнатуру, тип или порядок аргументов;
- **добавили поле в середину публичной структуры** или изменили её размер, если структура
  создаётся вызывающим (`sizeof` вкомпилирован в приложение);
- добавили/переставили значения в `enum` (числа вкомпилированы);
- добавили virtual-метод в класс (сдвиг vtable);
- поменяли inline-функцию в заголовке (старый код продолжит использовать старую копию);
- собрали библиотеку другим компилятором/`_GLIBCXX_USE_CXX11_ABI`, если в интерфейсе
  есть `std::string`/`std::vector`/`std::function`.

Отсюда мой тезис:
- **C ABI (`libadb_c.h`) — стабильный контракт.** Только opaque-указатели, POD и функции.
  Опции задаются сеттерами, а не структурой, поэтому их можно расширять бесконечно.
  Новые возможности = новые функции. SONAME не меняем, MAJOR не растёт.
  Такой .so можно обновлять под уже собранными бинарниками.
- **C++ API — «удобный, но привязанный к MAJOR».** В нём `std::string`, `std::function`,
  классы, структуры опций — по-настоящему стабилизировать это нельзя без огромной боли.
  Обещаем совместимость только внутри одного MAJOR и требуем пересборки при его смене.
  На практике это нормально: свой код (`adirect`, ваши сервисы) вы пересобираете вместе
  с библиотекой; сторонним/долгоживущим потребителям даём C API.

Решение можно отложить, но от него зависит, что писать в `libadb.map` и как считать версии.
Никаких затрат на старте: делаем оба слоя, просто в README фиксируем разные обещания.

---

## 12. Полный пример (C++)

```cpp
#include <libadb/libadb.h>
#include <libadb/spdlog_sink.hpp>
using namespace libadb;

int main() {
    auto app_log = spdlog::rotating_logger_mt("app", "/var/log/deploy.log", 5<<20, 3);

    Options o;
    o.log.enabled  = false;                       // /tmp/adb.log не создаётся
    o.auth.private_keys_pem = { my_key_from_vault() };
    o.auth.use_default_key_store = false;
    o.max_connections = 8;
    o.timeouts.slot_acquire = ms{10'000};
    o.timeouts.push  = { .total = ms{0}, .stall = ms{20'000} };
    o.timeouts.install.commit = ms{10 * 60'000};
    o.log_sink = make_spdlog_sink(app_log);
    o.on_event = [&](const Event& e) {
        if (e.type == EventType::OperationTimeout)
            app_log->error("{}: timeout {} phase {}", e.serial, (int)e.command, (int)e.phase);
    };
    if (Client::initialize(o) != Status::Ok) return 1;
    auto& c = Client::instance();

    // Режим 1: батч
    std::vector<DeviceAddress> all;
    for (int i = 231; i <= 253; ++i) all.push_back({"192.168.151." + std::to_string(i)});

    InstallOptions io;
    io.kind = InstallKind::Auto;                 // base.apk + splits -> install-multiple
    io.flags = {"-r", "-g"};
    io.on_conflict = ConflictPolicy::Reinstall;  // чужая подпись -> снести и поставить
    io.package_name = "com.example.main";
    auto results = c.install_all(all, {"base.apk", "split_config.arm64_v8a.apk"}, io);

    // Режим 2: одно устройство + logcat + параллельный push, отмена из другого потока
    Status st;
    if (auto dev = c.connect({"192.168.151.231"}, &st)) {
        auto lc = dev->open_shell("logcat -v threadtime", {
            .on_output = [](auto serial, auto chunk, bool) { /* ... */ } });

        auto op = dev->push_async({"big.zip"}, "/data/local/tmp/big.zip");
        if (!op->wait(ms{60'000})) op->cancel();
        app_log->info("push: {} {} bytes", to_string(op->result().status), op->result().bytes);

        lc->close();
    } else if (st == Status::SlotTimeout) {
        app_log->warn("все {} слотов заняты", c.max_connections());
    }

    c.close_all();      // рвём всё принудительно
    Client::shutdown();
}
```

---

## 13. Статусы (итоговый список)

```cpp
enum class Status {
    Ok = 0,
    InvalidArgument, NotInitialized, Unsupported, Internal,
    // подключение
    ConnectFailed, ConnectTimeout, AuthRequired, Unauthorized, Offline,
    DeviceLost, ConnectionClosed,       // ConnectionClosed = close_all()/shutdown
    // слоты
    SlotBusy, SlotTimeout, DeviceBusy,
    // выполнение
    CommandTimeout, StallTimeout, Canceled,
    IoError, LocalFileError, RemoteError,
    // установка
    SignatureMismatch, VersionDowngrade, InsufficientStorage, InvalidApk, MissingSplit,
};
```

---

## 14. C ABI — дополнения к v1

Всё новое из v2 отражается в C-слое (полный список — при реализации):

```c
/* ключи текстом */
void libadb_options_add_key_file(libadb_options*, const char* path);
void libadb_options_add_key_pem(libadb_options*, const char* pem, size_t len);
void libadb_options_set_use_default_keys(libadb_options*, int);

/* таймауты */
void libadb_options_set_transfer_timeout_ms(libadb_options*, libadb_cmd /*PUSH|PULL*/,
                                            uint32_t total_ms, uint32_t stall_ms);
void libadb_options_set_install_timeout_ms(libadb_options*, uint32_t prepare_ms,
                                            uint32_t transfer_total_ms, uint32_t transfer_stall_ms,
                                            uint32_t commit_ms, uint32_t healthcheck_ms);
void libadb_options_set_slot_timeout_ms(libadb_options*, uint32_t ms);
void libadb_options_set_max_connections(libadb_options*, size_t);

/* события */
typedef struct libadb_event libadb_event;      /* opaque, геттеры на поля */
typedef void (*libadb_event_fn)(const libadb_event*, void* user);
uint64_t libadb_subscribe(libadb_event_fn, uint64_t mask, void* user);
void     libadb_unsubscribe(uint64_t subscription);
int         libadb_event_type(const libadb_event*);
const char* libadb_event_serial(const libadb_event*);
uint64_t    libadb_event_op(const libadb_event*);
/* ... остальные геттеры */

/* асинхронные операции */
typedef struct libadb_op libadb_op;
libadb_status libadb_push_async(libadb_device*, const char* local, const char* remote,
                                libadb_op** out);
int           libadb_op_wait(libadb_op*, uint32_t timeout_ms);   /* 1 = завершилась */
void          libadb_op_cancel(libadb_op*);
libadb_status libadb_op_status(const libadb_op*);
uint64_t      libadb_op_id(const libadb_op*);
void          libadb_op_free(libadb_op*);
void          libadb_cancel(uint64_t op_id);
void          libadb_cancel_all(void);
void          libadb_close_all(void);

/* установка split/apks */
libadb_status libadb_install_ex(libadb_device*, const char* const* paths, size_t count,
                                const char* const* flags, size_t flag_count,
                                int kind, int conflict_policy, const char* package_name,
                                libadb_output_fn, void* user);

/* длинные сессии */
typedef struct libadb_shell_session libadb_shell_session;
libadb_status libadb_shell_open(libadb_device*, const char* cmd, int want_stdin,
                                libadb_shell_session** out);
ssize_t       libadb_shell_read(libadb_shell_session*, char* buf, size_t len,
                                int* is_stderr, uint32_t timeout_ms);
int           libadb_shell_write(libadb_shell_session*, const void*, size_t);
void          libadb_shell_close(libadb_shell_session*);
```

---

## 15. Порядок работ (обновлённый)

1. Сборка: `adb_core` (STATIC, PIC) → `libadb.so` (`-fvisibility=hidden`, version script,
   SOVERSION); `adirect` линкуется с `libadb.so`.
2. Фасад `Client`/`Device` (PIMPL) + синхронные `push/pull/shell/install/uninstall`
   поверх существующих `AdbManager`/`AdbDevice`/`AdbFileSync`/`AdbInstaller`.
3. Логирование: `LogOptions` (включая «выключено = файл не открываем»), `LogSink`,
   `spdlog_sink.hpp`.
4. Слоты и лимит подключений: `max_connections` + `slot_acquire` + оба режима работы (§7).
5. События (§8): диспетчер-поток, троттлинг прогресса, подписки.
6. Таймауты (§4): `total`/`stall` для sync-операций, фазы install, health-check коммита.
   Требует правок в `AdbSession::wait()` и `SyncConnection` (прогресс + прерывание).
7. Отмена и асинхронный режим (§6): `Operation`, `BatchOperation`, `cancel/close_all`.
8. Установка (§5): split/`.apks`/multi-package, разбор ошибок `pm`, `ConflictPolicy`,
   (позже) чтение `package` из AXML.
9. `ShellSession` (§10) + снятие ограничения «одна сессия на устройство».
10. Ключи из текста (§3).
11. C ABI (§14) + примеры на C.
12. Версионирование/установка/pkg-config/`abidiff` (v1 §7), `CHANGELOG.md`,
    перевод `adirect` на публичный API.

Пункты 1–4 дают работающую библиотеку, покрывающую текущие потребности `adirect`;
5–9 — то, что нужно для вашего сервиса; 10–12 — «упаковка» и внешние потребители.

---

## 16. Осталось решить

1. `ConflictPolicy::ReinstallKeepData` — оставляем как «попытка, без гарантий», или
   не делаем вовсе, чтобы не создавать ложных ожиданий?
2. Определение `package_name` из APK (парсер AXML) — нужно сразу или достаточно
   требовать его явно от вызывающего?
3. `worker_threads` для async: отдельный пул или тот же, что у батча (я за отдельный,
   иначе долгий async-push может занять все слоты батча)?
4. Health-check во время `commit`: делать служебной shell-сессией (простое, но лишний
   процесс на устройстве) или проверкой живости транспорта (дешевле, но не отличает
   «adbd жив, а система в дедлоке»)? Предлагаю транспорт + опционально shell.
5. Нужны ли события «уровня статистики» (итоговая скорость МБ/с на операцию) отдельным
   типом, или достаточно `OperationFinished` с `bytes`/`duration`?
