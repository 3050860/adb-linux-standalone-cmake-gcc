#!/bin/bash
# test_020 (этап 13, часть 3): проверка deb-пакетов в ЧИСТОМ контейнере.
#
# Смысл: пакеты проверяются там, где нет ни дерева исходников, ни собранных
# артефактов, ни dev-пакетов сверх объявленных зависимостей. Потребитель
# собирается только из libadb-dev двумя способами — pkg-config и
# find_package(libadb).
#
# Запуск (пакеты должны быть уже собраны, см. debian/build-packages.sh):
#   bash test/auto/run_test_020.sh [образ ...]
# По умолчанию проверяются все три целевых дистрибутива.
set -u

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
PKGDIR="$REPO/packages"

# Кодовое имя каталога в packages/ -> docker-образ, в котором проверяем.
image_for() {
    case "$1" in
        bookworm) echo debian:12 ;;
        trixie)   echo debian:13 ;;
        noble)    echo ubuntu:24.04 ;;
        resolute) echo ubuntu:26.04 ;;
        *)        echo "" ;;
    esac
}

# Список кодовых имён: из аргументов или все, что реально собрано.
CODENAMES=("$@")
if [ ${#CODENAMES[@]} -eq 0 ]; then
    CODENAMES=()
    for d in "$PKGDIR"/*/; do
        [ -d "$d" ] || continue
        ls "$d"/libadb1_*.deb >/dev/null 2>&1 && CODENAMES+=("$(basename "$d")")
    done
fi

failures=0
check() { if [ "$1" -eq 0 ]; then echo "[ OK ] $2"; else echo "[FAIL] $2"; failures=$((failures+1)); fi; }

if [ ${#CODENAMES[@]} -eq 0 ]; then
    echo "[FAIL] в $PKGDIR нет собранных пакетов — соберите их: make packages"
    exit 1
fi
echo "distros: ${CODENAMES[*]}"

# Сценарий внутри контейнера. Обратите внимание: ставим ТОЛЬКО компилятор,
# cmake и pkg-config — никаких -dev пакетов зависимостей. Если libadb-dev
# требует чего-то ещё, установка потребителя развалится, и мы это увидим.
read -r -d '' SCRIPT <<'SCRIPTEOF'
set -eu
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq g++ cmake pkg-config > /dev/null

echo "--- install packages (apt resolves dependencies) ---"
apt-get install -y -qq /debs/libadb1_*.deb /debs/libadb-dev_*.deb > /dev/null
echo "installed: $(dpkg-query -W -f='${Package} ${Version}\n' libadb1 libadb-dev | tr '\n' ' ')"

echo "--- ldconfig knows the library ---"
ldconfig -p | grep -q 'libadb.so.1' && echo "LDCONFIG_OK" || echo "LDCONFIG_MISSING"

cat > /tmp/consumer.cpp <<'CPPEOF'
#include <cstdio>
#include "libadb/libadb.h"
int main() {
    printf("libadb %s\n", libadb::version());
    libadb::Options o;
    o.timeouts.shell = libadb::ms{1000};
    o.auth.use_default_key_store = false;
    auto a = libadb::DeviceAddress::parse("10.0.0.5");
    printf("parsed %s\n", a ? a->to_string().c_str() : "FAILED");
    printf("kind=%s policy=%s\n", libadb::to_string(libadb::InstallKind::Bundle),
           libadb::to_string(libadb::ConflictPolicy::Reinstall));
    return 0;
}
CPPEOF

echo "--- consumer via pkg-config ---"
g++ -std=c++20 /tmp/consumer.cpp $(pkg-config --cflags --libs libadb) -o /tmp/c_pc
/tmp/c_pc && echo "PKGCONFIG_OK"

echo "--- consumer via find_package ---"
mkdir -p /tmp/cm && cp /tmp/consumer.cpp /tmp/cm/main.cpp
cat > /tmp/cm/CMakeLists.txt <<'CMEOF'
cmake_minimum_required(VERSION 3.22)
project(c CXX)
set(CMAKE_CXX_STANDARD 20)
find_package(libadb 1.0 REQUIRED)
add_executable(c main.cpp)
target_link_libraries(c PRIVATE libadb::adb)
CMEOF
cmake -S /tmp/cm -B /tmp/cm/b > /dev/null
cmake --build /tmp/cm/b > /dev/null
/tmp/cm/b/c && echo "CMAKE_OK"

echo "--- no BoringSSL/ziparchive in dependencies or on disk ---"
dpkg -s libadb1 | grep -i '^Depends' | grep -qiE 'ssl|crypto|ziparchive' && echo "DEPS_LEAK" || echo "DEPS_CLEAN"
ls /usr/lib/*/libziparchive.so* 2>/dev/null && echo "ZIP_FILE_PRESENT" || echo "ZIP_FILE_ABSENT"
SCRIPTEOF

for CODENAME in "${CODENAMES[@]}"; do
    IMAGE="$(image_for "$CODENAME")"
    if [ -z "$IMAGE" ]; then
        echo "[FAIL] неизвестное кодовое имя '$CODENAME'"
        failures=$((failures+1))
        continue
    fi

    echo
    echo "================= $CODENAME ($IMAGE) ================="
    LOG="/tmp/libadb-test-020-$CODENAME.log"
    # Пакеты берём ИМЕННО из каталога своего дистрибутива: проверяем то, что
    # собрано для него, а не случайно совпавшее по имени.
    docker run --rm --network host -v "$PKGDIR/$CODENAME:/debs:ro" "$IMAGE" \
        bash -c "$SCRIPT" > "$LOG" 2>&1
    rc=$?
    check $rc "$IMAGE: сценарий выполнен без ошибок"
    if [ $rc -ne 0 ]; then
        echo "--- tail of $LOG ---"
        tail -20 "$LOG"
        continue
    fi

    grep -q LDCONFIG_OK   "$LOG"; check $? "$IMAGE: ldconfig видит libadb.so.1"
    grep -q PKGCONFIG_OK  "$LOG"; check $? "$IMAGE: потребитель собран через pkg-config"
    grep -q CMAKE_OK      "$LOG"; check $? "$IMAGE: потребитель собран через find_package"
    grep -q 'libadb 1.0.0' "$LOG"; check $? "$IMAGE: библиотека сообщает версию 1.0.0"
    grep -q 'parsed 10.0.0.5:5555' "$LOG"; check $? "$IMAGE: публичный API работает"
    grep -q DEPS_CLEAN    "$LOG"; check $? "$IMAGE: в Depends нет BoringSSL/ziparchive"
    grep -q ZIP_FILE_ABSENT "$LOG"; check $? "$IMAGE: libziparchive.so не установлен"
done

echo
if [ "$failures" -eq 0 ]; then
    echo "ALL PASSED (0 failure(s))"
    exit 0
fi
echo "FAILED ($failures failure(s))"
exit 1
