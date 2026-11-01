#!/bin/bash
# test_021: шторм подключений к закрытым портам из многих потоков (устройства не нужны).
# Сборка и запуск против build/; ASan — переменная SAN=1 (библиотека тоже должна быть собрана с ASan).
set -eu
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${BUILD:-$REPO/build}"
FLAGS=""; [ "${SAN:-0}" = 1 ] && FLAGS="-fsanitize=address"
g++ -std=c++20 -g $FLAGS "$REPO/test/auto/test_021_connect_storm.cpp" -I"$REPO/include" -I"$BUILD/include" \
    -L"$BUILD" -ladb -Wl,-rpath,"$BUILD" -pthread -o /tmp/test_021
ASAN_OPTIONS=detect_leaks=0 /tmp/test_021 "$@"
