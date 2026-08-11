# libadb

C++ библиотека для прямого управления Android-устройствами по TCP — без `adb`-сервера и без shell-процессов.

## Что умеет

- **Push / Pull** файлов с прогрессом, сжатием и stall-таймаутом
- **Shell** — выполнение команд, потоковый вывод, таймаут
- **Install / Uninstall** APK — одиночный, split, `.apks` (bundletool), multi-package; ConflictPolicy при конфликте подписи
- **Синхронный и асинхронный** режимы; батч-операции на список устройств
- **Отмена** операций (`cancel`, `cancel_all`, `close_all`)
- **События** — машиночитаемые уведомления о ходе операций, прогрессе, ошибках и таймаутах
- **Таймауты** — stall, total, по фазам install (Prepare/Transfer/Commit) с health-check устройства
- **Авторизация** — ключи из файлов или текстом PEM (vault, БД), эфемерный ключ
- **Лимит подключений** с ожиданием слота
- **Логирование** — файловый лог внутри библиотеки и callback приложению

## Документация

[docs/libadb-usage.md](docs/libadb-usage.md)

## Примеры

[examples/](examples/)

| Файл | Содержание |
|---|---|
| `cpp/push.cpp` | push файла с прогрессом |
| `cpp/shell.cpp` | потоковый вывод shell-команды |
| `cpp/install.cpp` | установка APK с ConflictPolicy |
| `cpp/events.cpp` | подписка на события, on_start, таймауты |
| `cpp/async.cpp` | асинхронный батч на список устройств |
| `c/basic.cpp` | вызов из C-стиля кода |

## Готовые пакеты

[packages/](packages/)

Пакеты `libadb1` (рантайм) и `libadb-dev` (заголовки, pkg-config, CMake) собраны для:

| Каталог | Дистрибутив |
|---|---|
| `packages/bookworm/` | Debian 12 (Bookworm) |
| `packages/trixie/` | Debian 13 (Trixie) |
| `packages/noble/` | Ubuntu 24.04 LTS (Noble Numbat) |
| `packages/resolute/` | Ubuntu 26.04 LTS (Resolute Ratel) |

```bash
# Установка
sudo dpkg -i packages/noble/libadb1_*.deb packages/noble/libadb-dev_*.deb
```

## Быстрый старт

```cpp
#include "libadb/libadb.h"

auto& client = libadb::Client::instance();
client.initialize();

libadb::Status status;
auto device = client.connect("192.168.1.10", &status);

auto r = device->shell("echo hello", {});
printf("%s", r.output.c_str());   // "hello\n"
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

## Сборка из исходников

```bash
make boringssl        # подготовить кеш BoringSSL (один раз, ~550 МБ)
make build            # собрать библиотеку на текущей системе
make packages         # собрать deb-пакеты для всех дистрибутивов (в Docker)
make packages DISTROS=noble   # только для одного
```

Требования: cmake ≥ 3.25, g++ ≥ 12, git, docker (для `make packages`).
