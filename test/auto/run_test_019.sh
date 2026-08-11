#!/bin/bash
# Драйвер test_019 (этап 11): готовит staging-каталог и проверяет ОБА способа
# подключения библиотеки потребителем — pkg-config и find_package(libadb).
#
# Ключевая идея: потребители собираются ТОЛЬКО против установленного дерева,
# без -I/-L на дерево исходников. Так ловятся забытые заголовки, кривые пути в
# CMake-экспорте и утечки BUILD_INTERFACE.
#
# Запуск из корня репозитория:
#   bash test/auto/run_test_019.sh
set -u

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$REPO/build"
STAGE=/tmp/libadb-stage-019
PREFIX="$STAGE/usr"
LIBDIR="$PREFIX/lib/x86_64-linux-gnu"
WORK=/tmp/libadb-test-019-work

failures=0
check() { # check <rc> <name>
    if [ "$1" -eq 0 ]; then echo "[ OK ] $2"; else echo "[FAIL] $2"; failures=$((failures+1)); fi
}

echo "=== 1. install into staging ==="
rm -rf "$STAGE" "$WORK"
mkdir -p "$WORK"
cmake -S "$REPO" -B "$BUILD" -DCMAKE_INSTALL_PREFIX=/usr > "$WORK/cmake.log" 2>&1
check $? "cmake configure with prefix=/usr"
cmake --build "$BUILD" --target adb_shared -j"$(nproc)" > "$WORK/build.log" 2>&1
check $? "build adb_shared"
DESTDIR="$STAGE" cmake --install "$BUILD" > "$WORK/install.log" 2>&1
check $? "cmake --install into $STAGE"

echo
echo "=== 2. staged tree checks ==="
g++ -std=c++20 "$REPO/test/auto/test_019_packaging.cpp" \
    -I"$PREFIX/include" -L"$LIBDIR" -ladb -Wl,-rpath,"$LIBDIR" -pthread \
    -o "$WORK/test_019" 2> "$WORK/compile_checks.log"
check $? "compile test_019 against staged headers only"
if [ -x "$WORK/test_019" ]; then
    "$WORK/test_019" "$PREFIX"
    check $? "test_019 staged tree checks"
else
    sed -n '1,20p' "$WORK/compile_checks.log"
    failures=$((failures+1))
fi

echo
echo "=== 3. consumer via pkg-config ==="
cat > "$WORK/consumer.cpp" <<'CPPEOF'
// Потребитель библиотеки: только публичный заголовок, никаких внутренностей.
#include <cstdio>
#include "libadb/libadb.h"
int main() {
    printf("libadb %s (0x%06x)\n", libadb::version(), libadb::version_number());
    libadb::Options options;                 // структура из публичного API
    options.timeouts.shell = libadb::ms{1000};
    options.auth.use_default_key_store = false;
    auto parsed = libadb::DeviceAddress::parse("192.168.1.10:5555");
    if (!parsed) { printf("parse failed\n"); return 1; }
    printf("parsed %s\n", parsed->to_string().c_str());
    printf("status=%s kind=%s\n", libadb::to_string(libadb::Status::SlotBusy),
           libadb::to_string(libadb::InstallKind::Bundle));
    return 0;
}
CPPEOF

# PKG_CONFIG_SYSROOT_DIR обязателен для staging: без него pkg-config отдаёт
# флаги для настоящего /usr (и опускает -I/usr/include как путь по умолчанию),
# то есть потребитель собирался бы против системных заголовков, а не наших.
export PKG_CONFIG_SYSROOT_DIR="$STAGE"
PC_CFLAGS=$(PKG_CONFIG_PATH="$LIBDIR/pkgconfig" pkg-config --cflags libadb)
PC_LIBS=$(PKG_CONFIG_PATH="$LIBDIR/pkgconfig" pkg-config --libs libadb)
echo "pkg-config cflags: ${PC_CFLAGS:-<empty>}"
echo "pkg-config libs:   ${PC_LIBS:-<empty>}"
g++ -std=c++20 "$WORK/consumer.cpp" $PC_CFLAGS $PC_LIBS -L"$LIBDIR" \
    -Wl,-rpath,"$LIBDIR" -o "$WORK/consumer_pc" 2> "$WORK/compile_pc.log"
check $? "compile consumer with pkg-config flags"
if [ -x "$WORK/consumer_pc" ]; then
    OUT=$("$WORK/consumer_pc")
    check $? "run pkg-config consumer"
    echo "$OUT" | grep -q "libadb 1.0.0"
    check $? "consumer reports libadb 1.0.0 ($(echo "$OUT" | head -1))"
    echo "$OUT" | grep -q "parsed 192.168.1.10:5555"
    check $? "consumer used DeviceAddress::parse"
else
    sed -n '1,20p' "$WORK/compile_pc.log"
    failures=$((failures+1))
fi

echo
echo "=== 4. consumer via find_package(libadb) ==="
mkdir -p "$WORK/cmake_consumer"
cp "$WORK/consumer.cpp" "$WORK/cmake_consumer/main.cpp"
cat > "$WORK/cmake_consumer/CMakeLists.txt" <<'CMEOF'
cmake_minimum_required(VERSION 3.25)
project(libadb_consumer CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Ровно то, что обещано потребителю в §13.4: одна строка find_package и один
# таргет. Никаких путей к дереву исходников libadb здесь нет.
find_package(libadb 1.0 REQUIRED)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE libadb::adb)
CMEOF

rm -rf "$WORK/cmake_consumer/build"
cmake -S "$WORK/cmake_consumer" -B "$WORK/cmake_consumer/build" \
      -DCMAKE_PREFIX_PATH="$PREFIX" > "$WORK/cmake_consumer_cfg.log" 2>&1
check $? "find_package(libadb 1.0 REQUIRED) succeeded"
cmake --build "$WORK/cmake_consumer/build" > "$WORK/cmake_consumer_build.log" 2>&1
check $? "build consumer via libadb::adb"

if [ -x "$WORK/cmake_consumer/build/consumer" ]; then
    LD_LIBRARY_PATH="$LIBDIR" "$WORK/cmake_consumer/build/consumer" > "$WORK/cmake_run.log" 2>&1
    check $? "run CMake consumer"
    grep -q "libadb 1.0.0" "$WORK/cmake_run.log"
    check $? "CMake consumer reports libadb 1.0.0"
else
    sed -n '1,25p' "$WORK/cmake_consumer_cfg.log" "$WORK/cmake_consumer_build.log"
    failures=$((failures+1))
fi

# Версионная политика: SameMajorVersion. Запрос несуществующего MAJOR должен
# провалиться — иначе пакет молча подсунул бы несовместимый ABI.
# Отдельный каталог: не трогаем исходники успешного потребителя.
mkdir -p "$WORK/cmake_v2"
cat > "$WORK/cmake_v2/CMakeLists.txt" <<'CMEOF'
cmake_minimum_required(VERSION 3.25)
project(libadb_consumer_v2 CXX)
find_package(libadb 2.0 REQUIRED)
CMEOF
cmake -S "$WORK/cmake_v2" -B "$WORK/cmake_v2/build" \
      -DCMAKE_PREFIX_PATH="$PREFIX" > "$WORK/cmake_v2.log" 2>&1
if [ $? -ne 0 ]; then
    check 0 "find_package(libadb 2.0) корректно отвергнут (SameMajorVersion)"
else
    check 1 "find_package(libadb 2.0) должен был провалиться"
fi

echo
echo "=== 5. restore build tree ==="
# Возвращаем префикс по умолчанию, чтобы обычная сборка проекта не осталась
# настроенной на /usr.
cmake -S "$REPO" -B "$BUILD" -DCMAKE_INSTALL_PREFIX=/opt > "$WORK/restore.log" 2>&1
check $? "build tree reconfigured back to prefix=/opt"

echo
if [ "$failures" -eq 0 ]; then
    echo "ALL PASSED (0 failure(s))"
    exit 0
fi
echo "FAILED ($failures failure(s))"
exit 1
