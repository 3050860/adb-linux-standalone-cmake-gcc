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

