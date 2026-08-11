# Журнал разработки libadb.so

Файл дополняется **перед каждым коммитом этапа**. Сообщения коммитов: `libadb-phase-N`.
Проектная спецификация: `docs/libadb-api-proposal3.md`.

---

## Этап 0 — проектирование и правила работ (`libadb-phase-0`)

**Что сделано**

Добавлен файл `docs/libadb-api-proposal3.md` — самодостаточная итоговая спецификация
библиотеки. Ссылок на предыдущие черновики (`libadb-api-proposal.md`,
`libadb-api-proposal2.md`) в нём нет, они остаются в репозитории как история обсуждения.

Добавлен этот журнал (`docs/libadb-development-log.md`).

**Принятые решения, зафиксированные в спецификации**

1. `adirect` не изменяется вообще. Аналог на новом API будет отдельным примером
   `examples/adirect2` (этап 16).
2. Сборка делится на `adb_core` (STATIC, `-fPIC`) → `libadb.so` (SHARED, только фасад,
   `-fvisibility=hidden` + version script). Публичные заголовки не включают ничего
   внутреннего (`adb.h`, `transport.h`, `socket.h`, `android-base/*`), состояние — за PIMPL.
3. Логирование: внутренний лог **по умолчанию выключен**, файл `/tmp/adb.log` при этом
   не открывается и не создаётся; путь и уровень переопределяются, `ADB_TRACE` остаётся
   аварийным override. Отдельный канал `LogSink` для сообщений уровня приложения.
   spdlog в ABI не попадает — только inline-адаптер `spdlog_sink.hpp` у вызывающего.
4. Авторизация: ключи как файлами, так и текстом PEM (`AuthOptions::private_keys_pem`).
5. Таймауты передачи: `TransferTimeout{total, stall}` — можно задать по отдельности или
   вместе; по умолчанию активен только `stall` (30 с), потому что абсолютное время
   передачи на одни и те же устройства плавает в 3–4 раза из-за топологии сети.
6. `install` разбит на фазы (`Prepare`, `CreateSession`, `Transfer`, `Commit`, `Abandon`)
   со своими таймаутами. Для `Commit`, где прогресса принципиально нет, вместо угадывания
   времени установки применяется health-check: два режима на выбор вызывающего —
   `Transport` (дешёвый, дефолт) и `Shell` (строже, но создаёт процесс на устройстве).
   Особенности эксплуатации обоих режимов обязательны к описанию в руководстве (этап 15).
7. Синхронный и асинхронный режимы существуют оба. У каждой операции есть `OperationId`
   (выдаётся через `on_start` до начала работы и в событии `OperationStarted`), поэтому
   синхронный вызов можно отменить из другого потока: `Client::cancel(id)`,
   `Device::cancel_current()`, `Client::cancel_all()`. Асинхронный режим использует
   **отдельный пул** (`async_worker_threads`); документируем следствие: при смешанном
   использовании число одновременных подключений складывается из обоих пулов.
8. Списки устройств из файлов из API убраны. Два режима: батч по `std::vector<DeviceAddress>`
   и одиночный `connect()` с глобальным лимитом `max_connections` и ожиданием слота не
   дольше `Timeouts::slot_acquire` (дефолт 30 с, `Status::SlotTimeout` вместо зависания).
9. Система событий (`EventType`/`Event`/`subscribe`) с доставкой из отдельного
   диспетчер-потока, троттлингом прогресса и ограниченной очередью. Реализуется в два
   этапа: сначала без статистики (этап 5), затем `TransferStats`/`OperationStats` (этап 6).
10. `Client::close_all()` рвёт все соединения; текущие операции завершаются со
    `Status::ConnectionClosed` (отдельный код, отличимый от `Canceled`).
11. Установка: split-APK через сессию `install-create`/`install-write`/`install-commit`,
    `.apks`, `MultiPackage`. При чужой подписи — `ConflictPolicy::Reinstall`
    (удалить и поставить заново, данные теряются). `ConflictPolicy::ReinstallKeepData`
    в API присутствует, но возвращает `Status::NotImplemented`.
12. Имя пакета: `PackageNameSource{Explicit, Auto, Both}` — вызывающий сам решает,
    полагаться на автоопределение или передать имя. Парсер `AndroidManifest.xml` (AXML) —
    отдельный этап 10.
13. Длинные сессии (`logcat`) — `ShellSession` с push- и pull-режимами чтения,
    `idle_timeout`, `send_signal()`; сессия не блокирует `Device`.
14. C ABI считается стабильным контрактом (opaque-хендлы, POD, сеттеры вместо структур),
    C++ API — удобным, но привязанным к MAJOR. Версионирование: semver + `VERSION`/
    `SOVERSION`, version script `libadb.map` с `local: *`, генерируемый `version.h`,
    pkg-config и CMake export, контроль `abidiff`.

**План этапов** — таблица в §15 спецификации (этапы 1–16).

**Что проверено:** документация, изменений в коде нет, сборка не затронута.

**Дальше:** этап 1 — разделение целей сборки (`adb_core` + `libadb.so`), генерация
`include/libadb/version.h`, каркас публичных заголовков.

---

## Этап 1 — цели сборки и каркас API (`libadb-phase-1`)

**Что сделано**

Добавлены цели сборки, не затрагивающие существующие `adb` и `adirect`:

- `adb_core` — STATIC, `POSITION_INDEPENDENT_CODE ON`, `CXX_VISIBILITY_PRESET hidden`.
  Список исходников — это `ADIRECT_SOURCES` минус `adb/lib/src/adirect.cpp`
  (`list(REMOVE_ITEM ...)`), поэтому набор кода библиотеки и `adirect` не разъезжается
  при правках списка. Include-пути и зависимости объявлены `PUBLIC`.
- `adb_shared` — SHARED, `OUTPUT_NAME adb` → `libadb.so.1.0.0` + симлинки
  `libadb.so.1`, `libadb.so` (`VERSION`/`SOVERSION` из `PROJECT_VERSION`).
  Содержит только файлы фасада из `adb/lib/src/api/`, `adb_core` подключён `PRIVATE`.

`project(ADB)` → `project(ADB VERSION 1.0.0)`: без этого `PROJECT_VERSION*` пустые.
`include/libadb/version.h.in` → `configure_file` → `${CMAKE_BINARY_DIR}/include/libadb/version.h`
(`LIBADB_VERSION_MAJOR/MINOR/PATCH/STRING/NUMBER`). Сгенерированный `version.h` в
репозиторий не попадает; IDE до первого запуска CMake ругается на его отсутствие — это
нормально.

Публичный заголовок `include/libadb/libadb.h`: включает только STL и `libadb/version.h`,
`LIBADB_API` = `visibility("default")`. Реализованы `version()`, `version_number()`,
`Status`, `Command`, `Phase` + три `to_string()`, `DeviceAddress`. Остальной API
добавляется последующими этапами.

`adb/lib/libadb.map` — version script: `global` = `libadb_*` (C ABI) и mangled-паттерны
namespace `libadb` (`_ZN6libadb*`, `_ZNK6libadb*`, `_ZTIN/_ZTSN/_ZTVN6libadb*`),
`local: *`. Дополнительно `-Wl,--exclude-libs,ALL`. Прописан `LINK_DEPENDS`, чтобы
правка `.map` вызывала перелинковку.

**Решения по ходу**

1. `adirect` собирается из своих исходников и **не** линкуется с `adb_core`. Объекты
   компилируются дважды, зато поведение `adirect` гарантированно не меняется (требование
   спецификации). Сокращать дублирование — только после `examples/adirect2` (этап 16).
2. `__adb_argv`/`__adb_envp` определены в `adirect.cpp` и `client/main.cpp`, а нужны
   `adb_client.cpp` (перезапуск себя как adb-сервера через `execve`). Для `.so` добавлен
   `adb/lib/src/api/globals.cpp` с определениями (`__adb_argv = {"libadb", nullptr}`,
   `__adb_envp = nullptr`): в библиотеке этот путь не используется, но символы обязаны
   существовать.
3. `DeviceAddress::parse` возвращает `std::optional`: отвергает пустую строку, порт 0,
   порт > 65535 и мусор после числа (`std::from_chars` + проверка `res.ptr`). Поддержан
   IPv6 в скобках (`[::1]:5555`), поиск двоеточия — через `rfind`, поэтому
   `192.168.1.10:5555` и `[::1]` не конфликтуют. `to_string()` подставляет скобки для
   IPv6 обратно.
4. `-Wl,--no-undefined` для `.so` пока не включается: на следующих этапах фасад начнёт
   тянуть внутренности adb, и удобнее ловить недостающие символы разом. Отсутствие
   неразрешённых символов проверяется `ldd -r`.

**Что проверено**

- `cmake ..` + `make adb_shared` — успешно; `libadb_core.a` 42 МБ, `libadb.so.1.0.0` 168 КБ.
- `nm -D --defined-only libadb.so` — ровно 10 символов, все `libadb::*@@LIBADB_1.0`;
  внутренностей adb/base/spdlog/protobuf наружу нет.
- Внешняя программа (`g++ -Iinclude -Ibuild/include -ladb`) собирается и проходит
  проверки: версия совпадает с заголовком, разбор адресов (`192.168.1.10` → `:5555`,
  `[::1]:5555`), отказ на `host:0`, `host:99999`, `host:abc`, `""`, тексты статусов.
- `ldd -r` по `libadb.so` и по тестовой программе — неразрешённых символов нет.
- `make adirect adb` — обе цели собираются, `./adirect --help` работает как раньше.

**Дальше:** этап 2 — подсистема логирования (`LogOptions`, `LogSink`, выключенный по
умолчанию внутренний лог без создания `/tmp/adb.log`).


---

## Этап 2 — логирование (`libadb-phase-2`)

**Что сделано**

Внутренний файловый лог `adb_trace.cpp` стал конфигурируемым. Раньше он был жёстко
зашит: `/tmp/adb.log`, ротация 5 МБ × 3, `once_flag`, и файл создавался при первой же
записи в любом процессе. Теперь есть `AdbLogSettings` + `adb_log_configure()`,
`adb_log_open()`, `adb_log_flush()`, `adb_log_current_settings()`; `AdbLogger()` вынесен
в заголовок (нужен фасаду для `InitLogging`).

Публичный API (`include/libadb/libadb.h`): `LogLevel`, `LogOptions`, `LogSink`,
`set_log_options()`, `log_options()`, `set_log_level()`, `set_log_sink()`, `flush_log()`,
`log()`. Реализация — `adb/lib/src/api/log.cpp`, внутренние помощники —
`adb/lib/src/api/internal.h` (`ensure_logging_initialized`, `emit_log`, `log_sink_wants`).

`include/libadb/spdlog_sink.hpp` — header-only адаптер `make_spdlog_sink()`; в `.so`
не входит, компилируется у приложения его собственной версией spdlog.

**Решения по ходу**

1. **Выключение лога по умолчанию — через weak-символ, а не через статический
   инициализатор.** `adb_trace.cpp` содержит
   `__attribute__((weak)) bool adb_log_default_enabled() { return true; }`, а
   `api/globals.cpp` — сильное определение, возвращающее `false`. Линкер выбирает
   strong, поэтому в `libadb.so` лог выключен с самого первого возможного `LOG()`,
   независимо от порядка инициализации глобалов, а `adb`/`adirect` получают прежнее
   поведение (проверено `nm`: в `adirect` символ `W`, в `.so` — сильное определение).
   Вариант с конструктором глобального объекта отвергнут: чужой статический
   инициализатор мог бы залогировать раньше и создать файл.
2. `LogSettingsLocked()` держит настройки в куче и намеренно не удаляет их: логировать
   могут статические деструкторы, и обращение к уничтоженному объекту недопустимо.
3. Открытие sink остаётся ленивым, но `set_log_options(enabled = true)` сразу пишет
   строку-маркер `--- log opened: ... ---`: включение лога должно быть видно на диске
   немедленно, иначе непонятно, заработало ли оно.
4. Недоступный путь (нет прав/каталога) не роняет процесс: сообщение в stderr,
   `enabled = false`, работа продолжается.
5. При выключенном логе `SetMinimumLogSeverity(FATAL)` — внутренние `LOG()/VLOG()` не
   тратят время на форматирование строк. При включённом — уровень из `LogOptions::level`.
6. `ADB_TRACE` как аварийный override применяется при загрузке `.so` (объект
   `TraceEnvBootstrap`, объявлен последним в TU, поэтому `g_mutex`/`g_options` уже
   инициализированы). Если переменная не задана, конструктор ничего не делает и файл
   не создаётся.
7. `libadb::internal::*` попадает под шаблон `_ZN6libadb*` в version script, поэтому
   помечен `visibility("hidden")` явно — иначе внутренние помощники утекли бы в ABI.
8. `set_log_*` пока свободные функции (глобальные на процесс): `Client` появится на
   этапе 3 и его `set_log_options/set_log_level/set_log_sink` будут делегировать сюда.
9. Уровень `LogSink` (`g_sink_level`) пока фиксирован `Trace` — фильтрация канала 2
   отдельной настройкой появится вместе с событиями (§8).

**Что проверено**

- Лог выключен по умолчанию: ни `/tmp/adb.log`, ни заданный путь не создаются
  (`test -e` — absent).
- `set_log_options(enabled=true, file_path=/tmp/libadb-test.log)` — файл появляется
  сразу, с маркером `--- log opened: libadb::set_log_options ---`; `/tmp/adb.log`
  по-прежнему не создаётся.
- `ADB_TRACE=all` без единого вызова API включает лог (`--- log opened: ADB_TRACE ---`).
- `LogSink`: сообщение доходит с уровнем и serial; `LogLevel::Off` отбрасывается.
- `make_spdlog_sink` компилируется у приложения и пишет
  `[warning] [192.168.1.7:5555] via spdlog sink`.
- `nm -D` на `.so`: 16 символов, все `libadb::*`, `internal::` — 0.
- Регрессия `adirect`: `ADB_TRACE=all ./adirect --help` пишет `/tmp/adb.log` как раньше;
  `adb`, `adirect`, `adb_shared` собираются без ошибок.

## Этап 3 — Client и Device (синхронный путь)

**Что сделано**

- `include/libadb/libadb.h`: `Client` (синглтон: `initialize/connect/for_each/shutdown`),
  `Device` (`push/pull/shell/install/uninstall/get_prop/serial/is_online/close`),
  `Result` (`status/phase/exit_code/remote_code/output/error/duration/transfer`).
- `adb/lib/src/api/client.cpp` — жизненный цикл `AdbManager`, подключение по адресу,
  повторное подключение после `close()`.
- `adb/lib/src/api/device.cpp` — реализация операций, `FacadeListener` (мост
  `IAdbDeviceListener` -> `Result`/логи/колбэки вывода).

**Найдено и исправлено при проверке на живых устройствах (192.168.177.248/249)**

1. `uninstall` был холостым: путь `uninstall_app()` из кода adb опирается на
   `send_shell_command()`, которая в этой сборке — заглушка, поэтому удаление
   «успешно» завершалось, ничего не удаляя. Теперь `Device::uninstall` выполняет
   `cmd package uninstall` (с откатом на `pm uninstall`, если `cmd` нет) через
   обычный shell-канал и разбирает текст ответа.
2. Fallback через `||` оказался вреден: `cmd package uninstall` печатает `Success`,
   но может вернуть ненулевой код, и тогда запускался ещё и `pm uninstall`, чей
   `Failure [DELETE_FAILED_INTERNAL_ERROR]` затирал успех. Инструмент выбирается
   заранее (`command -v cmd`), а решающим признаком считается `Success`.
3. `remote_code` был пустым: код искали по префиксу `INSTALL_FAILED`, а pm печатает
   `Failure [CODE]`/`Failure [CODE: описание]`. Разбор переписан на этот формат —
   ловятся и `INSTALL_PARSE_FAILED_*`, и `DELETE_FAILED_*`.
4. Библиотека писала в stdout приложения: статусные строки pm печатались через
   `fputs(buf, stdout)` в `adb_install.cpp`. Добавлен `adb_install_set_status_sink()`
   (thread_local) — `Device::install` собирает их в `Result::output`, а `adirect`
   (sink не установлен) печатает как раньше.

**Что проверено**

- `connect`/`shell`/`push`/`pull`/`for_each`, повторное подключение после `close()`.
- `install` существующего apk: `Ok`, `output` содержит `Success`, ~15.6 с на 60 МБ apk.
- Битый apk: `RemoteError` + `INSTALL_PARSE_FAILED_NOT_APK`.
- `uninstall` несуществующего пакета: `RemoteError` + `DELETE_FAILED_INTERNAL_ERROR` (~0.1–0.9 с).
- `uninstall` реального пакета: пакет исчезает из `pm list packages -3`, повторный
  `install` возвращает его назад.
- Тест: `test/auto/test_011_install_uninstall.cpp` — ALL PASSED на 249 и 248.

**Важное предупреждение по стенду**

Удаление `cs.netarium` (это и есть `test/main.apk`) увело устройство 192.168.177.248
из сети примерно на пять минут — приложение, судя по поведению, отвечает за
сетевой/kiosk-режим. Устройство вернулось само, пакет установлен обратно. Поэтому
`test_011` намеренно не удаляет уже установленные пакеты: проверка `uninstall`
ограничена несуществующим пакетом.

**Что осталось (этапы 4+)**

- `bytes_on_wire` в `Result::transfer` всегда 0: внутренний `SyncConnection` не
  сообщает объём после сжатия.
- Прогресс-бар sync отключён (`quiet=true`), т.к. писал в stdout; свой прогресс —
  вместе с событиями (§8).

## Этап 4 — слоты подключений (§7)

Порядок этапов относительно таблицы §15 у нас сместился: логирование (§4) вышло
коммитом `libadb-phase-2`, фасад `Client`/`Device` (§9) — `libadb-phase-3`.

**Сначала закрыты два пункта §14**

1. Пункт 2 оказался серьёзнее, чем описан в спецификации. В перехвате
   `adb_get_feature_set()` кеш был объявлен обычной функционально-статической
   переменной, а возвращается на неё *ссылка*: потоки, работающие с разными
   устройствами, писали и читали один объект без синхронизации — гонка и
   «протекание» набора фич с одного устройства на другое. Кеш стал
   `thread_local` (`g_current_adb_device` тоже thread-local, так что этого
   достаточно) и обновляется на каждом вызове: кешировать по указателю нельзя,
   адрес освобождённого `AdbDevice` может быть переиспользован другим.
2. Пункт 7 — мёртвый `adb/lib/include/interface.h` удалён (включений нет ни в
   одном `.cpp`/`.h`/CMake-файле).

**Что сделано по этапу**

- `Options::max_connections` (0 = без ограничения) и `Options::slot_acquire`
  (0 — не ждать, `ms::max()` — ждать бесконечно, иначе `SlotTimeout`).
  `slot_acquire` временно живёт в `Options` рядом с `connect_timeout`; в
  `Timeouts` он переедет на этапе таймаутов.
- `Client::set_max_connections/max_connections/active_connections`.
- `SlotPool` в `client.cpp`: mutex + condition_variable, счётчик занятых.
  Живёт в `Client::Impl` вне `options`, чтобы повторный `initialize()` не
  сбрасывал счётчик занятых слотов.
- Слот занимается в `connect()` **до** создания транспорта (лимит считает
  реально открытые соединения) и возвращается через `Device::Impl::release_slot`.

**Грабли, на которые наступили при реализации**

- `cv_.wait_for(lock, ms::max())` переполняет внутреннее вычисление точки
  времени и возвращается немедленно, поэтому «ждать бесконечно» вынесено в
  отдельную ветку с `cv_.wait()`.
- `Impl::close()` начинался с `if (closed) return;`, а при неудачном
  `connectDevice()` флаг `closed` выставляется вручную — слот в этом случае
  остался бы занятым навсегда. Теперь возврат слота выполняется вне этой
  проверки и последним действием, с перемещением `release_slot` (ровно один раз).
- В `for_each` число воркеров ограничивается лимитом слотов: иначе лишние потоки
  просто висели бы в ожидании и упирались в `slot_acquire`.

**Что проверено (`test/auto/test_012_connection_slots.cpp`, 17 проверок, ALL PASSED
на 192.168.177.249 + .248)**

- лимит из `Options` применяется; до `connect()` занятых слотов нет;
- при занятом слоте `slot_acquire=0` → `SlotBusy` немедленно;
- `slot_acquire=700ms` → `SlotTimeout`, фактическое ожидание 700 мс;
- ожидающий в другом потоке `connect()` получает слот сразу после `close()`;
- слот возвращается и без явного `close()` — при разрушении `Device`;
- батч из 4 адресов при `max_parallel=4` и `max_connections=1`: обработаны все 4,
  пик одновременных подключений = 1, после батча занятых слотов 0;
- `set_max_connections(0)` в рантайме снимает ограничение — два подключения разом.

Регрессии: `test_011` — ALL PASSED, `adirect -f ... shell` на двух устройствах
работает как прежде.

## Этап 5 — события без статистики (§8)

**Что сделано**

Публичный API (`include/libadb/libadb.h`):

- `EventType` (23 значения), `Event`, `EventFn`, `SubscriptionId`, `EventMask`,
  `constexpr event_bit(EventType)`, `to_string(EventType)`.
- `StartedFn` и поле `on_start` во всех `*Options`: идентификатор операции
  выдаётся **до** начала работы — на этапе 8 по нему можно будет отменить
  синхронный вызов из другого потока.
- `InstallOptions::on_output` (сырой вывод `pm`), как в §10.
- `Options`: `on_event`, `event_mask`, `progress_interval` (200 мс),
  `event_queue_limit` (10 000).
- `Client::subscribe/unsubscribe`, `set_progress_interval/progress_interval`.

Реализация: `adb/lib/src/api/events.h` + `events.cpp` (новый файл в
`LIBADB_API_SOURCES`).

- `EventBus` — синглтон в куче (как `Client`): очередь `std::deque<Event>`,
  один диспетчер-поток, список подписчиков с индивидуальными масками.
- `OperationContext` — контекст одной операции: `OperationId`, команда, фаза,
  `start/set_phase/progress/heartbeat/output/retry/finish`. Тип финального
  события выбирается по `Result::status`: `Ok` → `OperationFinished`,
  `Canceled`/`ConnectionClosed` → `OperationCanceled`, таймауты →
  `OperationTimeout`, остальное → `OperationFailed`. Плюс `OperationStats`,
  если что-то реально передавалось (статистика по-настоящему появится на
  этапе 6, здесь она берётся из уже существующего `Result::transfer`).
- `ProgressThrottle` — троттлинг `OperationProgress` по
  `Options::progress_interval`; первое и последнее (`force`) события проходят
  всегда, иначе при быстрой передаче не увидеть ни одного.
- События устройств: `DeviceConnecting/Connected/AuthRequired/Unauthorized/
  Disconnected/Lost` из `Client::connect()`, `FacadeListener` и
  `Device::Impl::close()`; `SlotWaiting/SlotAcquired/SlotTimeout` из `SlotPool`;
  `ClientShutdown` из `Client::shutdown()`; `InternalError` из
  `FacadeListener::onError()`.

**Принятые решения и грабли**

1. **`wants(type)` без лока.** Объединённая маска всех подписчиков лежит в
   `std::atomic<EventMask>` и пересчитывается при под/отписке. Смысл: если
   событие никому не нужно, `publish()` не берёт мьютекс и не форматирует
   строки — а `OperationOutput` иначе копировал бы каждый чанк вывода.
2. **Диспетчер поднимается лениво**, в первом `subscribe()`. Без подписчиков
   лишнего потока в процессе нет.
3. **Переполнение очереди.** Сначала выбрасывается самое старое «расходное»
   событие (`OperationProgress`/`OperationHeartbeat`); если очередь целиком из
   критичных, а пришло расходное — выбрасывается оно само; если критичное —
   жертвуем самым старым, но не теряем новое. О потерях приходит
   `InternalError` с их количеством, и **только когда очередь разгрузилась**,
   иначе сообщение о переполнении само же переполняет очередь.
4. **Копия списка подписчиков** перед вызовом обработчиков: обработчик вправе
   позвать `subscribe()`/`unsubscribe()` прямо из колбэка.
5. **Исключение из обработчика** ловится (`std::exception` и `...`) и
   превращается в `InternalError` — иначе один плохой подписчик убивал бы
   диспетчер вместе с доставкой всем остальным.
6. **`unsubscribe()` уничтожает `EventFn` вне мьютекса**: деструктор захваченных
   объектов может позвать наши же методы.
7. **`OperationOutput`**: признак `stderr` уложен в `Event::bytes_done` (0/1).
   Отдельное поле означало бы расширение публичной структуры, то есть ломку ABI;
   когда понадобится по-настоящему — только с ростом `SOVERSION`.
8. **События из `FacadeListener::onConnectionStateChanged()`** публикуются
   после освобождения мьютекса слушателя: метод вызывается из fdevent-потока
   adb, и держать чужой обработчик под своим локом нельзя.
9. **`uninstall` и `get_prop` не создают вторую операцию.** Оба работали через
   публичный `Device::shell()`, из-за чего появлялся лишний `OperationId` с
   `Command::Shell`. Появился `Device::Impl::run_shell(command, options, op)`:
   `uninstall` передаёт свой контекст, `get_prop` — `nullptr` (служебный вызов
   в поток событий приложения не попадает вовсе).
10. **`Device::Impl` помечен `LIBADB_INTERNAL`.** Его методы подпадали под
    шаблон `_ZN6libadb*` из `libadb.map` и уезжали в динамическую таблицу
    `.so` (было 4 таких символа ещё до этапа, теперь 0).
11. **`shutdown()` сначала `drain()`, потом `stop()`**: подписчик обязан
    увидеть `ClientShutdown`, а не потерять его вместе с остановкой шины.

**Что проверено (`test/auto/test_013_events.cpp`, 33 проверки, ALL PASSED на
192.168.177.249)**

- подписка из `Options` работает; события приходят **не** из потока приложения;
- `DeviceConnecting`/`DeviceConnected` с непустым `serial`;
- `on_start` отдаёт `OperationId` и `serial`; **все** события операции несут
  этот же id; первое — `OperationStarted`, последнее — `OperationFinished`;
- `OperationOutput` доставляет текст, напечатанный командой;
- маска `event_bit(OperationFinished)` пропускает только это событие, а
  подписчик без маски продолжает получать всё;
- `unsubscribe()` прекращает доставку (0 событий после отписки);
- `progress_interval` читается из `Options` и меняется в рантайме;
- проваленный `push` даёт `OperationFailed` с тем же `Status` и `Command`;
- успешный `push` даёт `OperationProgress`, `OperationPhaseChanged` и
  `OperationStats` с `stats.bytes == Result::transfer.bytes`;
- `close()` → `DeviceDisconnected`, `shutdown()` → `ClientShutdown`.

Регрессии: `test_011` и `test_012` — ALL PASSED, `adirect` собирается.

**Что осталось**

- Промежуточного прогресса передачи пока нет: `SyncConnection` не отдаёт
  колбэк, поэтому `OperationProgress` приходит один раз, по завершении
  (`force`). Настоящий поток прогресса и `bytes_on_wire` — этап 6.
- `OperationHeartbeat` и `OperationRetry` реализованы в `OperationContext`, но
  ещё никем не публикуются: heartbeat нужен health-check'у коммита (этап 7),
  retry — `ConflictPolicy` (этап 9).

