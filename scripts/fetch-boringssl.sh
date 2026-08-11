#!/bin/bash
# Готовит локальный кеш исходников BoringSSL, чтобы сборка не тянула их из сети.
#
# Зачем: ExternalProject клонирует ~500 МБ при каждой новой директории сборки.
# Для deb-пакетов это особенно больно — debhelper собирает в свежем
# obj-<arch>/, то есть клон повторяется на каждую сборку пакета, требует сети и
# делает результат невоспроизводимым.
#
# Использование:
#   bash scripts/fetch-boringssl.sh              # в ./third_party/boringssl-src
#   bash scripts/fetch-boringssl.sh /opt/bssl    # в указанный каталог
#
# Далее сборка:
#   cmake -S . -B build -DBORINGSSL_SOURCE_DIR=$PWD/third_party/boringssl-src
set -eu

REPO="$(cd "$(dirname "$0")/.." && pwd)"
DEST="${1:-$REPO/third_party/boringssl-src}"
# Тег должен совпадать с BORINGSSL_GIT_TAG в CMakeLists.txt.
TAG="${BORINGSSL_TAG:-0.20251002.0}"
URL="https://github.com/google/boringssl.git"

if [ -f "$DEST/CMakeLists.txt" ]; then
    CURRENT="$(git -C "$DEST" describe --tags --always 2>/dev/null || echo unknown)"
    if [ "$CURRENT" = "$TAG" ]; then
        echo "BoringSSL $TAG already present in $DEST"
        exit 0
    fi
    echo "BoringSSL in $DEST is '$CURRENT', need '$TAG' — refetching"
    rm -rf "$DEST"
fi

mkdir -p "$(dirname "$DEST")"
echo "Cloning BoringSSL $TAG into $DEST ..."
# --depth 1: история не нужна, нужен только рабочий срез тега.
git clone --depth 1 --branch "$TAG" "$URL" "$DEST"

echo
echo "Done. Configure the project with:"
echo "  cmake -S \"$REPO\" -B \"$REPO/build\" -DBORINGSSL_SOURCE_DIR=\"$DEST\""
