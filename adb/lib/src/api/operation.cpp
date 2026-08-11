// libadb: асинхронные операции — пул потоков, Operation, BatchOperation (§9).
#include "api/operation_impl.h"

#include <algorithm>
#include <utility>

namespace libadb {
namespace internal {

// ---------------------------------------------------------------------------
// AsyncPool
// ---------------------------------------------------------------------------

AsyncPool& AsyncPool::instance() {
    // Как и остальные синглтоны: в куче и без удаления — потоки живут до конца
    // процесса, порядок разрушения статических объектов не определён.
    static AsyncPool* instance = new AsyncPool();
    return *instance;
}

AsyncPool::~AsyncPool() {
    stop();
}

void AsyncPool::configure(size_t threads) {
    if (threads == 0) threads = 1;  // 0 воркеров означало бы «никогда не выполнять»
    std::unique_lock<std::mutex> lock(mutex_);
    running_ = true;
    // Только доращиваем: уменьшать пул на ходу нельзя — придётся либо ждать
    // задачи, либо их прерывать, а простаивающий поток почти бесплатен.
    while (workers_.size() < threads) {
        workers_.emplace_back(&AsyncPool::worker_loop, this);
    }
}

size_t AsyncPool::threads() const {
    std::unique_lock<std::mutex> lock(mutex_);
    return workers_.size();
}

void AsyncPool::submit(std::function<void()> task) {
    if (!task) return;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        // Если пул ещё не настраивали (никто не звал initialize с async), поднимаем
        // его лениво с дефолтным размером.
        if (!running_ || workers_.empty()) {
            lock.unlock();
            configure(4);
            lock.lock();
        }
        queue_.push_back(std::move(task));
    }
    cv_.notify_one();
}

void AsyncPool::stop() {
    std::vector<std::thread> workers;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!running_ && workers_.empty()) return;
        running_ = false;
        queue_.clear();
        workers = std::move(workers_);
        workers_.clear();
    }
    cv_.notify_all();
    for (auto& worker : workers) {
        if (worker.joinable()) worker.join();
    }
}

void AsyncPool::worker_loop() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return !queue_.empty() || !running_; });
            if (queue_.empty()) {
                if (!running_) return;
                continue;
            }
            task = std::move(queue_.front());
            queue_.pop_front();
        }
        // Исключение из задачи не должно уносить воркер: результат операции
        // уже выставлен её собственным кодом.
        try {
            task();
        } catch (...) {
        }
    }
}

// ---------------------------------------------------------------------------
// Фабрики
// ---------------------------------------------------------------------------

OperationPtr OperationFactory::create(std::unique_ptr<Operation::Impl> impl) {
    // make_shared нельзя: конструктор приватный, friend — эта фабрика.
    return OperationPtr(new Operation(std::move(impl)));
}

BatchOperationPtr OperationFactory::create_batch(std::unique_ptr<BatchOperation::Impl> impl) {
    return BatchOperationPtr(new BatchOperation(std::move(impl)));
}

OperationPtr OperationFactory::completed(Command command, const std::string& serial,
                                         Result result) {
    auto impl = std::make_unique<Operation::Impl>();
    impl->id = 0;  // работы не было, значит и идентификатора операции нет
    impl->command = command;
    impl->serial = serial;
    impl->finished = true;
    impl->phase = result.phase;
    impl->result = std::move(result);
    return create(std::move(impl));
}

}  // namespace internal

// ---------------------------------------------------------------------------
// Operation
// ---------------------------------------------------------------------------

Operation::Operation(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Operation::~Operation() = default;

OperationId Operation::id() const {
    // Идентификатор рождается внутри OperationContext (то есть уже в потоке
    // пула) и публикуется через общий флаг отмены; до старта работы вернётся 0.
    if (impl_->id != 0) return impl_->id;
    return impl_->flag->operation_id.load(std::memory_order_relaxed);
}

Command Operation::command() const {
    return impl_->command;
}

const std::string& Operation::serial() const {
    return impl_->serial;
}

bool Operation::done() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->finished;
}

Phase Operation::phase() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->phase;
}

bool Operation::wait(ms timeout) const {
    std::unique_lock<std::mutex> lock(impl_->mutex);
    if (timeout <= ms::zero()) {
        impl_->cv.wait(lock, [this] { return impl_->finished; });
        return true;
    }
    return impl_->cv.wait_for(lock, timeout, [this] { return impl_->finished; });
}

Result Operation::result() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->result;
}

void Operation::cancel() {
    // Флаг общий с OperationContext операции, поэтому отмена работает и до
    // того, как воркер взял задачу из очереди.
    impl_->flag->canceled.store(true, std::memory_order_relaxed);
}

bool Operation::canceled() const {
    return impl_->flag->triggered();
}

// ---------------------------------------------------------------------------
// BatchOperation
// ---------------------------------------------------------------------------

BatchOperation::BatchOperation(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
BatchOperation::~BatchOperation() = default;

bool BatchOperation::done() const {
    for (const auto& op : impl_->operations) {
        if (!op->done()) return false;
    }
    return true;
}

bool BatchOperation::wait(ms timeout) const {
    if (timeout <= ms::zero()) {
        for (const auto& op : impl_->operations) op->wait();
        return true;
    }
    // Общий дедлайн на весь батч: каждой следующей операции остаётся то, что
    // не съели предыдущие.
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (const auto& op : impl_->operations) {
        const auto left = std::chrono::duration_cast<ms>(deadline - std::chrono::steady_clock::now());
        if (left <= ms::zero()) return false;
        if (!op->wait(left)) return false;
    }
    return true;
}

size_t BatchOperation::total() const {
    return impl_->operations.size();
}

size_t BatchOperation::finished() const {
    size_t count = 0;
    for (const auto& op : impl_->operations) {
        if (op->done()) ++count;
    }
    return count;
}

std::map<std::string, Result> BatchOperation::results() const {
    std::map<std::string, Result> results;
    for (size_t i = 0; i < impl_->operations.size(); ++i) {
        if (!impl_->operations[i]->done()) continue;
        results.emplace(impl_->addresses[i], impl_->operations[i]->result());
    }
    return results;
}

std::vector<OperationPtr> BatchOperation::operations() const {
    return impl_->operations;
}

void BatchOperation::cancel() {
    for (const auto& op : impl_->operations) op->cancel();
}

}  // namespace libadb
