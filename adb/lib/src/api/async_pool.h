/*
 * libadb — пул потоков для асинхронных операций (§9). Не устанавливается.
 *
 * Пул отдельный от батч-режима (AdbManager::runOnDevices): смешанное
 * использование складывает число одновременных подключений из обоих пулов,
 * ограничением сверху остаётся Options::max_connections.
 */
#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "api/internal.h"

namespace libadb::internal {

class LIBADB_INTERNAL AsyncPool {
  public:
    static AsyncPool& instance();

    // Поднимает пул до threads потоков (не уменьшает: уже работающие задачи
    // прерывать нельзя, а простаивающие потоки почти бесплатны).
    void configure(size_t threads);

    void submit(std::function<void()> task);

    // Останавливает пул, дождавшись уже взятых задач. Очередь при этом
    // очищается: незапущенные задачи выполняются вызывающим потоком нельзя —
    // они бы полезли в уже остановленный adb.
    void stop();

    size_t threads() const;

  private:
    AsyncPool() = default;
    ~AsyncPool();

    void worker_loop();

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> queue_;
    std::vector<std::thread> workers_;
    bool running_ = false;
};

}  // namespace libadb::internal
