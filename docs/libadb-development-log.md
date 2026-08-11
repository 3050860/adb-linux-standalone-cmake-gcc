# Журнал разработки libadb.so

Файл дополняется **перед каждым коммитом этапа**. Сообщения коммитов: `libadb-phase-N`.
Проектная спецификация: `docs/libadb-api-proposal3.md`.

> **О нумерации этапов.** После завершения этапа 9 план в §15 спецификации был
> пересмотрен и разбит на четыре части. Записи ниже сделаны по ходу работ и
> ссылаются на **старые** номера будущих этапов; менять их задним числом нет
> смысла — это журнал. Соответствие для этапов, которые ещё не сделаны:
>
> | Старый номер | Новый номер | Содержание |
> |---|---|---|
> | 12 | 10 | Авторизация (`AuthOptions`) |
> | 14 | 11 | Упаковка: version script, pkg-config, CMake export |
> | 15 | 14 | Документация по публичному API (`docs/libadb-usage.md`) |
> | — | 12 | Часть 2: статическая линковка BoringSSL и libziparchive |
> | 16 | 13 | Часть 3: deb-пакеты, руководство по использованию, `examples/` |
> | 10 | 15 | Парсер AXML (`AndroidManifest.xml`) |
> | 13 | 16 | C ABI |
> | 11 | 17 | `ShellSession` |
>
> Этапы 0–9 сохранили свои номера.

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

## Этап 6 — статистика передачи (§14 п.5)

**Что сделано**

Закрыт пункт 5 §14 («`SyncConnection` работает в режиме `quiet` — нужен колбэк
прогресса и возможность прерывания»).

Новый внутренний интерфейс `adb/lib/include/sync_progress.h`:

```cpp
struct SyncProgressObserver {
    std::function<void(const std::string& path, uint64_t done, uint64_t total)> on_progress;
    std::function<void(uint64_t bytes)> on_wire_bytes;
    std::function<bool()> should_abort;
};
const SyncProgressObserver* adb_sync_set_observer(const SyncProgressObserver*);
const SyncProgressObserver* adb_sync_observer();
```

Наблюдатель ставится **на текущий поток** (`thread_local`) — так же, как уже
сделаны `adb_set_current_device()` и `adb_install_set_status_sink()`: sync
целиком выполняется в потоке, который позвал `do_sync_push_fd()`/
`do_sync_pull_fd()`, а таких потоков в библиотеке много.

Точки перехвата:

- `SyncConnection::ReportProgress()` — единственное место, куда стягивается
  прогресс всех четырёх путей передачи (`SendSmallFile`, `SendLargeFile`,
  `SendLargeFileLegacy`, `sync_recv`, `sync_recv_v2`).
- `SyncConnection::WriteOrDie()` — единственная запись в сокет при push,
  отсюда `bytes_on_wire` (после сжатия).
- Чтения `ReadFdExactly(sc.fd, …)` в `sync_recv`/`sync_recv_v2` — `bytes_on_wire`
  для pull.
- `copy_to_file()` в `helpers.cpp` — тот же наблюдатель: `install` льёт apk
  через `pm install-write` именно этим циклом, и без хука фаза `Transfer`
  установки была бы полностью «слепой».
- Проверки `should_abort()` вставлены в циклы push (обе версии) и pull (обе
  версии). Гранулярность — один блок `SYNC_DATA_MAX` (64 КиБ); используется
  этапом 7 (stall-таймаут) и этапом 8 (отмена).

`do_sync_push_fd()`/`do_sync_pull_fd()` получили необязательный выходной
параметр `uint64_t* bytes_transferred`, `AdbFileSync::push/pull` — наблюдателя
и тот же выходной параметр (оба со значениями по умолчанию, поэтому `adirect`
не изменился ни на строку).

`Device::push/pull/install`: `TransferCounters` + `make_observer()` в
`device.cpp`. `Result::transfer` теперь заполняется целиком — `bytes`,
`bytes_on_wire`, `duration`, `mib_per_sec`; тот же поток идёт в
`OperationProgress` и в `*Options::on_progress`.

**Принятые решения и грабли**

1. **`bytes` берём у `SyncConnection`, а не из `stat()` локального файла.**
   При `sync_only_newer` часть файлов пропускается, у каталогов размер вообще
   не при чём, а на pull размер на устройстве заранее неизвестен. `stat()`
   остаётся запасным вариантом, если sync ничего не сообщил.
2. **`BytesTransferred()` читает `global_ledger_`, а не `current_ledger_`.**
   `NewTransfer()` обнуляет `current_ledger_` на каждом файле, поэтому по нему
   в многофайловой передаче был бы виден только последний файл.
3. **`bytes_on_wire` при pull считается вместе с заголовком блока**
   (`msg.data.size + sizeof(msg.data)`): это и есть то, что реально прочитано
   из сокета. Поэтому без сжатия `bytes_on_wire` немного больше `bytes` —
   так и должно быть (проверяется тестом).
4. **`total` в прогрессе может прийти нулём** (sync не всегда знает размер) —
   тогда подставляется `TransferCounters::expected`, то есть то, что знаем сами.
5. **Наблюдатель снимается через RAII** (`ScopedSyncObserver` в
   `AdbFileSync.cpp`) и сохраняет предыдущего: наблюдатели вкладываются
   (install → внутренний push на legacy-пути), а `thread_local`, оставленный
   висеть после выхода, указывал бы на мёртвый стек.
6. **`copy_to_file()` не знает пути файла** — отдаёт пустую строку и
   `total = 0`; для install это нормально, ожидаемый размер подставляет
   `Device::install()`.

**Что проверено (`test/auto/test_014_transfer_stats.cpp`, 30 проверок, ALL
PASSED на 192.168.177.249)**

- push 4 МиБ **без** сжатия: `bytes == 4194304`, `bytes_on_wire == 4194824`
  (полезные + заголовки блоков), `duration = 379 ms`, `mib_per_sec = 10.55`;
- `on_progress` вызван 65 раз, значения не идут назад, последнее — 100 %;
  `OperationProgress` — те же 65 событий;
- push того же объёма **со** сжатием (`Compression::Any`, файл из одинаковых
  байт): `bytes` не изменился, `bytes_on_wire == 1361` — то есть хук считает
  именно то, что уходит в сокет, а не полезную нагрузку;
- pull: `bytes == 4194304`, `bytes_on_wire == 4194824`, `mib_per_sec = 10.99`,
  67 событий прогресса;
- `progress_interval = 100 s` → ровно 2 события прогресса (первое и
  завершающее `force`), то есть троттлинг работает и «100 %» не теряется;
- install `test/main.apk` (60 728 027 байт): `transfer.bytes` совпал с размером
  файла до байта, `mib_per_sec = 4.16`, 928 вызовов `on_progress`.

Регрессии: `test_011`, `test_012`, `test_013` — ALL PASSED;
`adirect -f ... shell` на живом устройстве работает как прежде.

**Что осталось**

- `should_abort` пока никем не заполняется: таймауты (этап 7) и отмена
  (этап 8).
- `install` отдаёт прогресс одной сплошной фазой; разбивка по
  `Prepare/CreateSession/Transfer/Commit` со своими таймаутами — этап 7.

## Этап 7 — таймауты и health-check коммита (§6, §14 п.4)

**Что сделано**

Закрыт пункт 4 §14: `AdbSession::waitFor(timeout_ms, &exit_code)` — ждёт
`exit_code_future_` не дольше таймаута и по истечении сам вызывает `abort()`.
Без `abort()` reader-поток остался бы висеть на `adb_read()` до конца жизни
процесса.

Публичный API (§6):

- `TransferTimeout{total, stall}` — по умолчанию `total = 0` (выключен),
  `stall = 30 s`.
- `HealthCheckMode{None, Transport, Shell}` + `to_string()`.
- `InstallTimeout{prepare, create_session, transfer, commit,
  commit_healthcheck, commit_healthcheck_interval, commit_healthcheck_failures}`.
- `Timeouts{connect, slot_acquire, shell, push, pull, install, uninstall, close}`.
- `Options::timeouts` — **`Options::connect_timeout` и `Options::slot_acquire`
  переехали внутрь `Timeouts`** (`timeouts.connect`, `timeouts.slot_acquire`),
  как и предписано §6.3. Тесты `t4`–`t7` и `test_012` обновлены на новые имена.
- `Client::set_timeouts()/timeouts()`.
- Точечные `std::optional<...> timeout` в `PushOptions`, `PullOptions`,
  `ShellOptions`, `InstallOptions`, `UninstallOptions`.

Реализация:

- `internal::current_timeouts()` — операции читают таймауты на старте, поэтому
  `set_timeouts()` действует на всё, что начнётся после вызова, и не ломает уже
  идущие операции.
- `TransferDeadline` в `device.cpp` — общий и stall-дедлайн; `touch()` на каждом
  событии прогресса, `should_abort` наблюдателя (этап 6) отдаёт `true` по
  истечении. Результат: `Status::CommandTimeout` для `total`,
  `Status::StallTimeout` для `stall`, `Result::transfer` при этом заполнен тем,
  что успело пройти.
- `Device::Impl::run_shell(..., ms timeout)` → `AdbSession::waitFor()`;
  по таймауту `Status::CommandTimeout`, `exit_code = -1`.
- `install` теперь выполняется в **рабочем потоке**, а вызывающий поток крутит
  `monitor_install()`: шаг 100 мс, отслеживание фаз, дедлайны фаз, health-check.

**Как определяются фазы install**

Установка — это один блокирующий `install_multiple_app()`, отдельных сигналов
«install-create ответил» / «install-write закончился» у него нет. Поэтому фазы
выводятся из наблюдаемых признаков:

- байты ещё не пошли → `CreateSession` (дедлайн `create_session`);
- байты идут → `Transfer` (дедлайн `transfer.total`/`transfer.stall`);
- байты кончились и тишина дольше секунды → `Commit`.

На фазе `Commit` прогресса нет принципиально, поэтому вместо угадывания времени
установки работает health-check: раз в `commit_healthcheck_interval` проверяем
живость и публикуем `OperationHeartbeat`; `commit_healthcheck_failures` неудач
подряд → `Status::DeviceLost`. Режимы: `Transport` (проверка транспорта и
состояния устройства, ничего не запускает на устройстве) и `Shell` (короткая
команда `true` со своим таймаутом 10 с — строже, но создаёт процесс на
устройстве на каждую проверку).

**Принятые решения и грабли**

1. **`install` в отдельном потоке, а не таймер поверх блокирующего вызова.**
   Иначе фазовые дедлайны и heartbeat невозможны: `install_multiple_app()`
   не возвращает управление до конца.
2. **`adb_install_set_status_sink()` и `adb_sync_set_observer()` ставятся
   ВНУТРИ рабочего потока.** Оба перехвата `thread_local`; поставленные в
   вызывающем потоке, они бы просто не действовали на поток установки — вывод
   `pm` уехал бы в stdout, а прогресса не было бы вовсе.
3. **`worker.join()` выполняется всегда**, даже когда мы уже решили прерваться:
   рабочий поток держит ссылки на `result.output`, наблюдателя и `AdbDevice`.
4. **Причина срыва хранится отдельно от `expired()`.** После прерывания
   `expired()` уже неинформативен, а `Result` формируется позже — поэтому
   `TransferDeadline::reason` и `InstallMonitor::reason/phase/message`
   запоминаются в момент решения.
5. **Прерванная передача — не `IoError`.** Ветка `deadline.reason != Ok`
   ставит именно `CommandTimeout`/`StallTimeout` и глотает сообщение sync об
   обрыве (`take_error()`), чтобы оно не подменяло причину.
6. **Убраны `Error()` из веток прерывания в `file_sync_client.cpp`.**
   `SyncConnection::Error()` печатает в **stderr приложения**
   (`LinePrinter` игнорирует `quiet_` для типа ERROR), и на каждом таймауте в
   консоль вызывающего лезло `adb: error: transfer of '...' aborted`.
   Прерывание случается только по нашей же просьбе, причина уже в `Result`.
7. **Фаза `Transfer` у install определяется «прогресс был свежее секунды».**
   Порог меньше давал ложные переходы в `Commit` на паузах между блоками.
8. **`HealthCheckMode::Shell` использует свой таймаут (10 с), а не
   `Timeouts::shell`**: проверка живости не должна ждать столько же, сколько
   сама установка.

**Что проверено (`test/auto/test_015_timeouts.cpp`, 28 проверок, ALL PASSED на
192.168.177.249)**

- `Timeouts` читаются из `Options` (`shell = 3000`, `connect = 20000`),
  дефолты соответствуют §6 (`push.stall = 30000`, `push.total = 0`,
  health-check `Transport`), `set_timeouts()` меняет их в рантайме;
- `shell("sleep 30")` при `Timeouts::shell = 3 s` срывается ровно через 3000 мс
  со `Status::CommandTimeout`, приходит одно `OperationTimeout`, **устройство
  остаётся online**, следующая команда работает;
- `ShellOptions::timeout = 20 s` перебивает глобальные 3 с: `sleep 5` доходит
  до конца (5042 мс);
- push 48 МиБ с `total = 800 ms` срывается через 830 мс со `CommandTimeout`,
  успев передать ~9 МБ; событие `OperationTimeout` пришло; устройство живо;
- push с `stall = 5 s` и `total = 0` доходит до конца (50 331 648 байт) —
  stall-таймер сбрасывается прогрессом, ложных срывов нет;
- `total = 0, stall = 0` — без ограничения, передача проходит целиком;
- install с фазовыми таймаутами проходит успешно, в событиях есть фазы
  `Transfer` и `Commit`, health-check прислал 37 `OperationHeartbeat`;
- install с `create_session = 1 ms` не ломается (либо `CommandTimeout`, либо
  pm успевает ответить быстрее шага монитора 100 мс).

Регрессии: `test_011`, `test_012`, `test_013`, `test_014` — ALL PASSED;
`adirect` собирается и работает.

**Что осталось**

- `Timeouts::close` и `InstallTimeout::prepare` объявлены, но пока ни на что не
  влияют: закрытие транспорта в текущей реализации не блокирующее, а фаза
  `Prepare` (распаковка `.apks`) появится на этапе 9.
- Отмена операций пользователем (`cancel/cancel_all/close_all`) и асинхронный
  режим — этап 8; механизм прерывания (`should_abort`) для них уже готов.

## Этап 8 — отмена и асинхронный режим (§9)

**Что сделано**

Публичный API:

- `Operation` (`id/command/serial/done/phase/wait/result/cancel/canceled`) и
  `BatchOperation` (`wait/done/total/finished/results/operations/cancel`),
  `OperationPtr`/`BatchOperationPtr`.
- `Device::push_async/pull_async/shell_async/install_async/uninstall_async`,
  `Device::busy()`, `Device::cancel_current()`.
- `Client::push_all_async/pull_all_async/shell_all_async/install_all_async/
  uninstall_all_async`, `Client::cancel(id)`, `Client::cancel_all()`,
  `Client::active_operations()`.
- `Options::async_worker_threads` (4 по умолчанию).
- `Device` теперь наследует `std::enable_shared_from_this<Device>`: асинхронная
  операция держит устройство сама, вызывающий вправе отпустить `DevicePtr`
  сразу после `*_async`.

Внутри:

- `CancelFlag` (`canceled` + `connection_closed`) в `shared_ptr`,
  `OperationRegistry` (`add/remove/cancel/cancel_all/cancel_serial/close/size`),
  `AsyncPool` (`api/async_pool.h` + `operation.cpp`), `PendingCancelFlag`.
- `OperationContext` регистрируется в реестре в конструкторе и снимается в
  деструкторе; `canceled()`/`cancel_status()` доступны всем операциям.
- Проверки отмены: в наблюдателе передачи (`should_abort`), в цикле ожидания
  `run_shell` (порции по 100 мс) и в `monitor_install`.

**Принятые решения и грабли**

1. **`PendingCancelFlag` — флаг «на поток».** Асинхронный воркер вызывает те же
   публичные `Device::push/shell/...`, у которых нет параметра «флаг отмены».
   Тащить его в публичный ABI не хочется, поэтому воркер ставит флаг на свой
   поток, а `OperationContext` его подхватывает и обнуляет — флаг достаётся
   ровно одной, самой внешней операции.
2. **`Operation::id()` до старта равен 0.** Идентификатор рождается внутри
   `OperationContext`, то есть уже в потоке пула, когда хэндл давно отдан
   вызывающему. Поэтому id публикуется через `CancelFlag::operation_id`, а
   `id()` читает его оттуда.
3. **`DeviceBusy` через `compare_exchange_strong`**: два `*_async` из разных
   потоков не должны оба «успеть». Синхронные вызовы флаг занятости не
   выставляют — их последовательность остаётся заботой вызывающего (§9).
4. **`AsyncPool` только доращивается.** Уменьшать пул на ходу значит либо
   ждать задачи, либо их прерывать; простаивающий поток почти бесплатен.
   `shutdown()` останавливает пул **до** остановки event loop: иначе
   недоделанная операция полезла бы в уже мёртвый adb.
5. **`close_all()` сначала помечает операции**, потом рвёт транспорты — чтобы
   они успели увидеть `ConnectionClosed`, а не выдать обрыв за `IoError`.
   `Device::close()` делает то же для своего устройства.
6. **Ветки прерывания push/pull проверяют и `op.canceled()`.** При `close_all()`
   запись в сокет падает раньше, чем наблюдатель успевает заметить флаг, и
   `ConnectionClosed` подменялся бы `IoError`.

**Три чужих дефекта, вскрытые отменой**

7. **`AdbSession::abort()` не закрывал поток по-настоящему.** Он лишь закрывал
   наш конец socketpair. Но adb-конец зарегистрирован в epoll, и закрытие
   нашего конца даёт fdevent-циклу событие только при следующей активности —
   до тех пор `asocket` жив, а команда на устройстве не остановлена. Симптом:
   команда, отправленная сразу после прерванной, немедленно получала EOF и
   срывалась по своему таймауту, потому что цикл закрывал старый local socket
   уже после того, как новая сессия переиспользовала тот же номер дескриптора.
   Решение: `adb_shutdown(local_fd_, SHUT_RDWR)` (дескриптор остаётся в epoll,
   цикл гарантированно видит EOF, сам отправляет `A_CLSE` и удаляет `asocket`),
   затем синхронизация с циклом через пустую задачу `fdevent_run_on_looper`, и
   только потом закрытие своего конца. Пробовали закрывать `asocket` напрямую
   из своего потока — получили double free: объект принадлежит fdevent-циклу и
   удаляет себя сам.
8. **`SyncConnection::WriteOrDie()` делал `_exit(1)`.** В консольном adb это
   нормально, для библиотеки — недопустимо: `close_all()` во время передачи
   убивал процесс приложения. Теперь при активном наблюдателе функция
   возвращает `false`, а вызывающие циклы (три места) проверяют результат.
9. **SIGPIPE.** `adb` в `main()` делает `signal(SIGPIPE, SIG_IGN)`; библиотека
   этого не делала, и запись в оборванный сокет убивала процесс сигналом
   (`exit=141`). Добавлен `internal::ensure_sigpipe_ignored()`, вызываемый из
   `initialize()`: ставит `SIG_IGN` **только если** приложение не настроило
   обработчик само (текущий равен `SIG_DFL`).
10. **`AdbSession::waitFor()` больше не вызывает `abort()` сам.** Метод
    рассчитан на ожидание порциями (между ними проверяется отмена), а
    прерванную сессию продолжать нельзя. Решение «рвать или ждать дальше»
    отдано вызывающему.

**Что проверено (`test/auto/test_016_async_cancel.cpp`, 44 проверки, ALL PASSED
на 192.168.177.249 + .248)**

- `shell_async` возвращается за 0 мс, `wait(300ms)` не дожидается «спящей»
  команды, `wait()` дожидается, `Result` содержит вывод, id ненулевой;
- второй `*_async` на занятом устройстве завершён сразу со `DeviceBusy`;
- `Operation::cancel()` на 64-МиБ передаче: завершение за 42 мс, статус
  `Canceled`, передалось 4.9 МБ из 64, событие `OperationCanceled`, устройство
  осталось online;
- `Client::cancel(id)` из другого потока отменяет **синхронный** `push`
  (id получен через `on_start`); после завершения `cancel(id)` возвращает
  `false`, `active_operations() == 0`;
- `cancel_all()` и `Device::cancel_current()` — статус `Canceled`;
- `close_all()` во время передачи → `ConnectionClosed` (отличим от `Canceled`);
- `shell_all_async` по двум адресам: `total/finished/results/operations`
  согласованы, обе команды выполнены.

Дополнительно добавлены два диагностических теста, по которым и были найдены
дефекты 7–9: `test/auto/t11.cpp` (команда сразу после прерванной по таймауту) и
`test/auto/t12.cpp` (отмена push).

Регрессии: `test_011`, `test_012`, `test_013`, `test_014`, `test_015` — ALL
PASSED; `adirect` собирается, `shell` и `push` на живом устройстве работают
(важно: правки в `WriteOrDie` его не задели — без наблюдателя поведение прежнее).

**Что осталось**

- Отмена на фазе `Commit` не отменяет установку на самом устройстве: `pm` уже
  получил команду. Это отражено в `Result::error` («the device keeps
  installing»), как и предписано §9.
- `ShellSession` (этап 11) по-прежнему нет, поэтому ограничение «одна сессия на
  устройство» в силе.

## Этап 9 — установка: split / `.apks` / multi-package, разбор ошибок pm (§10)

**Что сделано**

Публичный API:

- `InstallKind{Auto, Single, SplitSet, Bundle, MultiPackage}`,
  `ConflictPolicy{Fail, Reinstall, ReinstallKeepData}`,
  `PackageNameSource{Explicit, Auto, Both}` + `to_string()` для всех трёх.
- `InstallOptions` дополнен: `kind`, `on_conflict`, `package_name_source`,
  `package_name`, `allow_downgrade_retry`, `user_id`.
- `UninstallOptions::user_id` (сахар над `--user N`).
- `Device::install(const std::vector<std::string>&, ...)` и
  `Device::install_async(const std::vector<std::string>&, ...)`.

Реализация:

- `Device::install(paths, options)` — оркестратор: валидация, фаза `Prepare`
  (проверка файлов, распаковка бандла), формирование флагов pm, вызов попытки,
  повторы, очистка. Одиночный `install(apk)` теперь просто делегирует ему.
- `Device::Impl::run_install_attempt()` — ровно одна попытка установки (рабочий
  поток + `monitor_install` из этапа 7). Вынесено отдельно именно для повторов.
- `status_from_install_code()` — раскладка кода pm в `Status` (§10):
  `INSTALL_FAILED_UPDATE_INCOMPATIBLE` → `SignatureMismatch`,
  `INSTALL_FAILED_VERSION_DOWNGRADE` → `VersionDowngrade`,
  `INSTALL_FAILED_INSUFFICIENT_STORAGE` → `InsufficientStorage`,
  `INSTALL_FAILED_INVALID_APK` и всё семейство `INSTALL_PARSE_FAILED_*` →
  `InvalidApk`, `INSTALL_FAILED_MISSING_SPLIT` → `MissingSplit`, остальное →
  `RemoteError` с сохранением кода в `remote_code`.
- `resolve_install_kind()` для `Auto`, `package_name_from_output()` — извлечение
  имени пакета из текста ошибки pm.
- `AdbInstaller`: `install(paths, flags, multi_package)` (при `true` вызывается
  `install_multi_package()` вместо `install_multiple_app()`),
  `expandApks()`/`cleanupExpanded()` стали публичными и статическими.

**Принятые решения и грабли**

1. **`Auto` при нескольких файлах даёт `SplitSet`, а не `MultiPackage`.**
   Части одного пакета — самый частый случай, а `MultiPackage` меняет семантику
   на атомарную сессию; угадывать это по именам файлов было бы гаданием, поэтому
   независимые пакеты вызывающий обязан запросить явно.
2. **Бандл распаковывается в фасаде, а не внутри `AdbInstaller::install()`.**
   Так фаза `Prepare` честно укладывается в свой таймаут
   (`InstallTimeout::prepare`), а состав пакета известен до старта — значит,
   известен и общий размер, то есть прогресс считается по всем частям, а не по
   первому файлу. Повторной распаковки не происходит: `expandApks()` для готовых
   `.apk` работает как passthrough.
3. **`base.apk` в бандле ставится первым.** `pm` ожидает базовый пакет раньше
   split-частей, а `readdir()` возвращает файлы в произвольном порядке.
4. **Временный каталог распаковки — `/tmp/libadb_apks_*`** (было
   `/tmp/adirect_apks_*`), чистится через `cleanupExpanded()` **в фасаде**, после
   всех повторов: раньше очистка была внутри одной попытки, и второй заход
   (`Reinstall`, downgrade-retry) не нашёл бы файлов.
5. **`ReinstallKeepData` отвергается до обращения к устройству** —
   `Status::NotImplemented`, ничего не передаётся (проверено тестом).
6. **`ConflictPolicy::Reinstall` не удаляет «что-нибудь».** Имя пакета берётся
   строго по `PackageNameSource`: `Explicit` — только переданное (иначе
   `InvalidArgument`), `Auto`/`Both` — плюс разбор текста ошибки pm. Разбор
   `AndroidManifest.xml` (AXML) — этап 10, до тех пор `Auto` умеет только текст.
   Перед переустановкой публикуется `OperationRetry` с честной причиной
   («data will be lost»), в `Result::retries` растёт счётчик.
7. **Ещё пять мест, где внутренний код писал в консоль приложения.**
   `install_multi_package()` печатал `Created parent/child session ID`,
   финальный `Success`, `failed to link sessions`, `Attempting to abandon
   session` и `multi-package install is not supported` прямо в stdout/stderr.
   Всё переведено на уже существующий `report_status()`, поэтому у библиотеки это
   попадает в `Result::output`, а у `adirect` (приёмник не задан) поведение
   осталось прежним — проверено запуском `adirect install`.
8. **`error_exit()` в `install_multi_package()` обойдён валидацией.** Функция
   `[[noreturn]]`-убивает процесс, если последние аргументы не `.apk`/`.apex`;
   фасад проверяет существование и непустоту всех файлов заранее, а после
   распаковки на вход всегда идут `.apk`.

**Что проверено (`test/auto/test_017_install_kinds.cpp`, 37 проверок, ALL PASSED
на 192.168.177.249)**

- пустой список → `InvalidArgument` (фаза `Prepare`), отсутствующий файл →
  `LocalFileError`, `Single` с двумя файлами → `InvalidArgument`;
- `ReinstallKeepData` → `NotImplemented`, `transfer.bytes == 0`;
- `to_string()` для `InstallKind`/`ConflictPolicy`/`PackageNameSource`;
- битый «apk» (текстовый мусор): `remote_code = INSTALL_PARSE_FAILED_NOT_APK`
  разложен в `Status::InvalidApk`, пришло `OperationFailed`;
- одиночный `install` с `user_id = 0` — успех, 60 728 027 байт, фазы `Prepare` и
  `Transfer`, одно `OperationFinished`, статус pm в `output`;
- `.apks` (Auto → Bundle): распаковался, установился, временные каталоги
  `/tmp/libadb_apks_*` удалены;
- `SplitSet` и `MultiPackage` доходят до pm и возвращают внятный результат,
  устройство остаётся online (multi-package на этом устройстве прошёл успешно:
  в `output` видны `Created parent session ID` / `Created child session ID`);
- `ConflictPolicy::Reinstall` с явным именем пакета не мешает обычной установке
  (`retries == 0`), пакет остаётся на устройстве;
- `install_async(vector)` возвращает операцию, дожидается и даёт `Ok`.

Бандл `.apks` для теста готовится из `test/main.apk` (внутри должен быть
`base.apk`), команда приведена в шапке теста.

Регрессии: `test_011`, `test_012`, `test_013`, `test_014`, `test_015`,
`test_016` — ALL PASSED; `adirect install/push/shell` на живом устройстве
работают, консольный вывод не изменился. Внутренних символов в динамической
таблице `.so` нет (105 публичных).

**Что осталось**

- Настоящий конфликт подписи в тесте не воспроизведён: нужен apk, подписанный
  другим ключом. Проверено всё, что можно проверить достоверно (валидация,
  выбор имени пакета, отсутствие лишних повторов); сам путь
  `uninstall + install` устроен так же, как проверенный повтор по downgrade.
- `PackageNameSource::Auto` пока умеет только текст ошибки pm; разбор
  `AndroidManifest.xml` (бинарный AXML) — этап 10.
- Устройство в стенде управляется MDM и переустанавливает пакеты само (см.
  журнал этапа 3), поэтому тесты сознательно не удаляют уже установленные
  пакеты.

