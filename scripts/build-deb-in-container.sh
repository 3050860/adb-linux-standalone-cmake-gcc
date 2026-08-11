#!/bin/bash
# Сборка deb-пакетов libadb ВНУТРИ контейнера (вызывается из `make packages`).
#
# Ожидает смонтированными:
#   /src  — дерево исходников (read-only)
#   /out  — куда положить готовые .deb
# И переменные окружения: JOBS, CODENAME.
#
# Весь вывод идёт в stdout/stderr как есть — ничего не подавляется и не
# перенаправляется, чтобы в консоли было видно каждый шаг сборки.
set -eu
set -o pipefail

: "${JOBS:=1}"
: "${CODENAME:=unknown}"

# -x: печатаем каждую команду. Так в логе видно, где именно сборка встала.
set -x

echo "########## [$CODENAME] $(cat /etc/os-release | grep PRETTY_NAME) ##########"

########## 1. Зависимости сборки ##########
# Без -qq и без >/dev/null: apt должен рассказывать, что он делает, иначе на
# новом дистрибутиве непонятно, какой пакет не нашёлся.
apt-get update
# libbsd-dev: на Debian 12 (glibc 2.36) из него берётся strlcpy, которого в libc
# нет — он появился только в glibc 2.38 (Ubuntu 24.04+).
apt-get install -y --no-install-recommends \
    build-essential debhelper cmake git pkg-config \
    libprotobuf-dev protobuf-compiler libbrotli-dev liblz4-dev \
    libzstd-dev libspdlog-dev libfmt-dev zlib1g-dev libbsd-dev lintian

echo "########## [$CODENAME] versions ##########"
cmake --version | head -1
g++ --version | head -1
dpkg-query -W -f='${Package} ${Version}\n' debhelper libprotobuf-dev libspdlog-dev || true

########## 2. Копия исходников ##########
# Пакетная сборка пишет в дерево (obj-*/, debian/tmp), а /src монтирован
# read-only, поэтому работаем в копии. Не копируем:
#   build/, packages/, obj-* — артефакты хозяйской системы, только мешают;
#   third_party/boringssl-src — 550 МБ, которые смонтированы отдельно в
#   /boringssl-src (копировать их на каждый дистрибутив бессмысленно).
mkdir -p /work/libadb
tar -C /src -cf - \
    --exclude=./build \
    --exclude=./packages \
    --exclude=./.git \
    --exclude=./obj-x86_64-linux-gnu \
    --exclude=./third_party/boringssl-src \
    . | tar -C /work/libadb -xf -

cd /work/libadb
ls -la

########## 3. Сборка пакетов ##########
# Кеш BoringSSL: смонтирован read-only, поэтому передаём путь напрямую через
# BORINGSSL_SOURCE_DIR (см. debian/rules). ExternalProject только читает
# SOURCE_DIR, собирая в своём каталоге, так что read-only монтирования хватает.
if [ -f /boringssl-src/CMakeLists.txt ]; then
    echo "boringssl cache: /boringssl-src ($(git -C /boringssl-src describe --tags --always 2>/dev/null || echo 'no git info'))"
    export BORINGSSL_SOURCE_DIR=/boringssl-src
else
    echo "boringssl cache: MISSING — BoringSSL будет клонироваться из сети"
fi

# parallel=N уважается debhelper'ом и передаётся в cmake --build.
export DEB_BUILD_OPTIONS="parallel=${JOBS}"
dpkg-buildpackage -us -uc -b

########## 4. Результат ##########
cd /work
ls -la ./*.deb
cp -v ./*.deb /out/
chmod 0644 /out/*.deb

echo "########## [$CODENAME] lintian ##########"
# Не роняем сборку из-за замечаний стиля, но показываем их полностью.
lintian --tag-display-limit 0 ./*.deb || true

echo "########## [$CODENAME] package contents ##########"
for deb in ./*.deb; do
    echo "--- $deb"
    dpkg-deb -I "$deb" | grep -E 'Package|Version|Depends|Installed-Size' || true
    dpkg-deb -c "$deb" | awk '{print $6, $7, $8}'
done

set +x
echo "########## [$CODENAME] DONE ##########"
