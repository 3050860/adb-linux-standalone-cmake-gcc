/*
 * libadb — внутреннее устройство Operation/BatchOperation (§9).
 * Не устанавливается, в ABI не входит.
 */
#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "api/async_pool.h"
#include "api/events.h"
#include "libadb/libadb.h"

namespace libadb {

// LIBADB_INTERNAL: имена подпадают под шаблон _ZN6libadb* в libadb.map,
// поэтому видимость гасим явно.
struct LIBADB_INTERNAL Operation::Impl {
    OperationId id = 0;
    Command command = Command::Shell;
    std::string serial;

    // Флаг отмены создаётся здесь и передаётся в OperationContext операции:
    // cancel() должен работать и до того, как воркер реально начал работу.
    internal::CancelFlagPtr flag = std::make_shared<internal::CancelFlag>();

    // Устройство держим shared_ptr: вызывающий вправе отпустить свой DevicePtr
    // сразу после *_async.
    DevicePtr device;

    mutable std::mutex mutex;
    mutable std::condition_variable cv;
    bool finished = false;
    Phase phase = Phase::None;
    Result result;

    void complete(Result value) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            result = std::move(value);
            phase = result.phase;
            finished = true;
        }
        cv.notify_all();
    }
};

struct LIBADB_INTERNAL BatchOperation::Impl {
    std::vector<OperationPtr> operations;
    std::vector<std::string> addresses;  // параллельно operations: ключи результатов
};

namespace internal {

// Создаёт Operation/BatchOperation: конструкторы приватные, а фабрика объявлена
// friend'ом в libadb.h.
struct LIBADB_INTERNAL OperationFactory {
    static OperationPtr create(std::unique_ptr<Operation::Impl> impl);
    static BatchOperationPtr create_batch(std::unique_ptr<BatchOperation::Impl> impl);

    // Операция, завершённая ещё до запуска (например, DeviceBusy).
    static OperationPtr completed(Command command, const std::string& serial, Result result);
};

}  // namespace internal
}  // namespace libadb
