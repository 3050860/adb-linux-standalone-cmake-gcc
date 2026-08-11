// libadb: шина событий — диспетчер-поток, подписки, маски, троттлинг (§8).
#include "api/events.h"

#include <algorithm>
#include <utility>

namespace libadb {

const char* to_string(EventType type) {
    switch (type) {
        case EventType::DeviceConnecting:      return "device-connecting";
        case EventType::DeviceConnected:       return "device-connected";
        case EventType::DeviceAuthRequired:    return "device-auth-required";
        case EventType::DeviceUnauthorized:    return "device-unauthorized";
        case EventType::DeviceDisconnected:    return "device-disconnected";
        case EventType::DeviceLost:            return "device-lost";
        case EventType::SlotWaiting:           return "slot-waiting";
        case EventType::SlotAcquired:          return "slot-acquired";
        case EventType::SlotTimeout:           return "slot-timeout";
        case EventType::OperationStarted:      return "operation-started";
        case EventType::OperationPhaseChanged: return "operation-phase-changed";
        case EventType::OperationProgress:     return "operation-progress";
        case EventType::OperationHeartbeat:    return "operation-heartbeat";
        case EventType::OperationRetry:        return "operation-retry";
        case EventType::OperationOutput:       return "operation-output";
        case EventType::OperationFinished:     return "operation-finished";
        case EventType::OperationTimeout:      return "operation-timeout";
        case EventType::OperationCanceled:     return "operation-canceled";
        case EventType::OperationFailed:       return "operation-failed";
        case EventType::OperationStats:        return "operation-stats";
        case EventType::ClientShutdown:        return "client-shutdown";
        case EventType::InternalError:         return "internal-error";
    }
    return "unknown";
}

namespace internal {
namespace {

// События, которыми можно пожертвовать при переполнении очереди: они
// информационные и повторяются, потеря отдельного экземпляра не ломает картину.
bool droppable(EventType type) {
    return type == EventType::OperationProgress || type == EventType::OperationHeartbeat;
}

}  // namespace

// ---------------------------------------------------------------------------
// EventBus
// ---------------------------------------------------------------------------

EventBus::EventBus() : epoch_(std::chrono::steady_clock::now()) {}

EventBus::~EventBus() {
    // Синглтон живёт до конца процесса; деструктор нужен лишь для полноты.
    stop();
}

EventBus& EventBus::instance() {
    // Как и Client: в куче и без удаления — порядок разрушения статических
    // объектов не определён, а события могут прилетать из потоков adb.
    static EventBus* instance = new EventBus();
    return *instance;
}

ms EventBus::now() const {
    return std::chrono::duration_cast<ms>(std::chrono::steady_clock::now() - epoch_);
}

OperationId EventBus::next_operation_id() {
    static std::atomic<OperationId> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed) + 1;
}

void EventBus::start() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (running_) return;
    running_ = true;
    dispatcher_ = std::thread(&EventBus::dispatch_loop, this);
}

void EventBus::stop() {
    std::thread worker;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!running_) return;
        running_ = false;
        worker = std::move(dispatcher_);
    }
    queue_cv_.notify_all();
    drain_cv_.notify_all();
    if (worker.joinable()) worker.join();
}

void EventBus::recompute_mask_locked() {
    EventMask mask = 0;
    for (const auto& s : subscribers_) {
        if (s.handler) mask |= s.mask;
    }
    combined_mask_.store(mask, std::memory_order_relaxed);
}

SubscriptionId EventBus::subscribe(EventFn handler, EventMask mask) {
    if (!handler) return 0;
    SubscriptionId id;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        id = next_id_++;
        subscribers_.push_back(Subscriber{id, mask, std::move(handler)});
        recompute_mask_locked();
    }
    // Диспетчер поднимаем лениво: без подписчиков поток не нужен вообще.
    start();
    return id;
}

void EventBus::unsubscribe(SubscriptionId id) {
    if (id == 0) return;
    // Обработчик может владеть ресурсами вызывающего — уничтожаем его вне лока,
    // чтобы деструктор не позвал наши методы под тем же мьютексом.
    EventFn removed;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        auto it = std::find_if(subscribers_.begin(), subscribers_.end(),
                               [id](const Subscriber& s) { return s.id == id; });
        if (it == subscribers_.end()) return;
        removed = std::move(it->handler);
        subscribers_.erase(it);
        if (primary_id_ == id) primary_id_ = 0;
        recompute_mask_locked();
    }
}

void EventBus::set_primary_subscription(EventFn handler, EventMask mask) {
    SubscriptionId old = 0;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        old = primary_id_;
    }
    if (old != 0) unsubscribe(old);

    if (!handler) return;
    const SubscriptionId id = subscribe(std::move(handler), mask);
    std::unique_lock<std::mutex> lock(mutex_);
    primary_id_ = id;
}

void EventBus::set_queue_limit(size_t limit) {
    std::unique_lock<std::mutex> lock(mutex_);
    queue_limit_ = limit;
}

void EventBus::set_progress_interval(ms interval) {
    std::unique_lock<std::mutex> lock(mutex_);
    progress_interval_ = interval;
}

ms EventBus::progress_interval() const {
    std::unique_lock<std::mutex> lock(mutex_);
    return progress_interval_;
}

bool EventBus::wants(EventType type) const {
    return (combined_mask_.load(std::memory_order_relaxed) & event_bit(type)) != 0;
}

uint64_t EventBus::dropped() const {
    std::unique_lock<std::mutex> lock(mutex_);
    return dropped_total_;
}

void EventBus::publish(Event event) {
    // Никому не интересно — не тратим ни лока, ни памяти.
    if (!wants(event.type)) return;
    if (event.timestamp == ms::zero()) event.timestamp = now();

    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (queue_limit_ > 0 && queue_.size() >= queue_limit_) {
            // Сначала пробуем выкинуть самое старое «расходное» событие.
            auto victim = std::find_if(queue_.begin(), queue_.end(),
                                       [](const Event& e) { return droppable(e.type); });
            if (victim != queue_.end()) {
                queue_.erase(victim);
            } else if (droppable(event.type)) {
                // Очередь целиком из критичных событий, а пришло расходное —
                // проще выбросить именно его.
                ++dropped_total_;
                ++dropped_pending_;
                return;
            } else {
                // Критичные события не теряем «молча», но и расти бесконечно
                // не даём: жертвуем самым старым.
                queue_.pop_front();
            }
            ++dropped_total_;
            ++dropped_pending_;
        }
        queue_.push_back(std::move(event));
    }
    queue_cv_.notify_one();
}

void EventBus::drain() {
    std::unique_lock<std::mutex> lock(mutex_);
    drain_cv_.wait(lock, [this] { return (queue_.empty() && !dispatching_) || !running_; });
}

void EventBus::dispatch_loop() {
    std::vector<Subscriber> snapshot;
    for (;;) {
        Event event;
        uint64_t dropped_report = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            queue_cv_.wait(lock, [this] { return !queue_.empty() || !running_; });
            if (queue_.empty()) {
                // Останов: очередь досылаем до конца, поэтому выходим только
                // когда в ней ничего не осталось.
                drain_cv_.notify_all();
                if (!running_) return;
                continue;
            }
            event = std::move(queue_.front());
            queue_.pop_front();
            dispatching_ = true;

            // О потерях сообщаем отдельным событием, но только когда очередь
            // разгрузилась: иначе сами же её и переполним.
            if (dropped_pending_ > 0 && queue_.empty()) {
                dropped_report = dropped_pending_;
                dropped_pending_ = 0;
            }

            // Копия списка подписчиков: обработчик вправе вызвать
            // subscribe()/unsubscribe() прямо из колбэка.
            snapshot.clear();
            for (const auto& s : subscribers_) {
                if (s.handler && (s.mask & event_bit(event.type))) snapshot.push_back(s);
            }
        }

        for (const auto& s : snapshot) {
            // Исключение из обработчика не должно валить диспетчер: превращаем
            // его в InternalError (доставится следующим кругом).
            try {
                s.handler(event);
            } catch (const std::exception& e) {
                Event error;
                error.type = EventType::InternalError;
                error.status = Status::Internal;
                error.serial = event.serial;
                error.op = event.op;
                error.message = std::string("event handler threw: ") + e.what();
                publish(std::move(error));
            } catch (...) {
                Event error;
                error.type = EventType::InternalError;
                error.status = Status::Internal;
                error.serial = event.serial;
                error.op = event.op;
                error.message = "event handler threw an unknown exception";
                publish(std::move(error));
            }
        }

        if (dropped_report > 0) {
            Event error;
            error.type = EventType::InternalError;
            error.status = Status::Internal;
            error.bytes_done = dropped_report;
            error.message =
                "event queue overflow: " + std::to_string(dropped_report) + " event(s) dropped";
            publish(std::move(error));
        }

        {
            std::unique_lock<std::mutex> lock(mutex_);
            dispatching_ = false;
            if (queue_.empty()) drain_cv_.notify_all();
        }
    }
}

// ---------------------------------------------------------------------------
// ProgressThrottle
// ---------------------------------------------------------------------------

bool ProgressThrottle::allow(bool force) {
    const auto interval = EventBus::instance().progress_interval();
    const auto now = std::chrono::steady_clock::now();
    // Первое и последнее события прогресса пропускаем всегда: по ним видно
    // начало передачи и её завершение (100 %).
    if (force || first_ || interval <= ms::zero() || now - last_ >= interval) {
        first_ = false;
        last_ = now;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// OperationRegistry
// ---------------------------------------------------------------------------

OperationRegistry& OperationRegistry::instance() {
    // Как и остальные синглтоны библиотеки: в куче и без удаления.
    static OperationRegistry* instance = new OperationRegistry();
    return *instance;
}

void OperationRegistry::add(OperationId id, const std::string& serial,
                            const CancelFlagPtr& flag) {
    std::lock_guard<std::mutex> lock(mutex_);
    operations_[id] = Entry{serial, flag};
}

void OperationRegistry::remove(OperationId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    operations_.erase(id);
}

bool OperationRegistry::cancel(OperationId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = operations_.find(id);
    if (it == operations_.end()) return false;
    it->second.flag->canceled.store(true, std::memory_order_relaxed);
    return true;
}

size_t OperationRegistry::cancel_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, entry] : operations_) {
        entry.flag->canceled.store(true, std::memory_order_relaxed);
    }
    return operations_.size();
}

size_t OperationRegistry::cancel_serial(const std::string& serial) {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t affected = 0;
    for (auto& [id, entry] : operations_) {
        if (entry.serial != serial) continue;
        entry.flag->canceled.store(true, std::memory_order_relaxed);
        ++affected;
    }
    return affected;
}

size_t OperationRegistry::close(const std::string& serial) {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t affected = 0;
    for (auto& [id, entry] : operations_) {
        if (!serial.empty() && entry.serial != serial) continue;
        entry.flag->connection_closed.store(true, std::memory_order_relaxed);
        ++affected;
    }
    return affected;
}

size_t OperationRegistry::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return operations_.size();
}

// ---------------------------------------------------------------------------
// PendingCancelFlag
// ---------------------------------------------------------------------------

namespace {
thread_local CancelFlagPtr g_pending_cancel_flag;
}  // namespace

PendingCancelFlag::PendingCancelFlag(CancelFlagPtr flag)
    : previous_(std::move(g_pending_cancel_flag)) {
    g_pending_cancel_flag = std::move(flag);
}

PendingCancelFlag::~PendingCancelFlag() {
    g_pending_cancel_flag = std::move(previous_);
}

CancelFlagPtr PendingCancelFlag::take() {
    CancelFlagPtr flag = std::move(g_pending_cancel_flag);
    g_pending_cancel_flag = nullptr;
    return flag;
}

// ---------------------------------------------------------------------------
// OperationContext
// ---------------------------------------------------------------------------

OperationContext::OperationContext(Command command, std::string serial, CancelFlagPtr flag)
    : id_(EventBus::next_operation_id()),
      command_(command),
      serial_(std::move(serial)),
      started_(std::chrono::steady_clock::now()),
      flag_(flag ? std::move(flag) : PendingCancelFlag::take()) {
    // Флага не было ни в аргументе, ни приготовленного на потоке — создаём свой
    // (обычный синхронный вызов из кода приложения).
    if (!flag_) flag_ = std::make_shared<CancelFlag>();
    // Публикуем id в флаге: только так внешний хэндл Operation узнаёт его.
    flag_->operation_id.store(id_, std::memory_order_relaxed);
    // Регистрируемся сразу в конструкторе: cancel(id) должен работать с того
    // момента, как вызывающий получил id через on_start.
    OperationRegistry::instance().add(id_, serial_, flag_);
}

OperationContext::~OperationContext() {
    OperationRegistry::instance().remove(id_);
}

ms OperationContext::elapsed() const {
    return std::chrono::duration_cast<ms>(std::chrono::steady_clock::now() - started_);
}

Event OperationContext::make(EventType type) const {
    Event event;
    event.type = type;
    event.serial = serial_;
    event.op = id_;
    event.command = command_;
    event.phase = phase_;
    event.elapsed = elapsed();
    return event;
}

void OperationContext::start(const StartedFn& on_start) {
    // Сначала отдаём id вызывающему: он должен успеть его запомнить до того,
    // как операция реально начнётся (иначе отменять будет нечего).
    if (on_start) on_start(id_, serial_);
    EventBus::instance().publish(make(EventType::OperationStarted));
}

void OperationContext::set_phase(Phase phase) {
    if (phase == phase_) return;
    phase_ = phase;
    EventBus::instance().publish(make(EventType::OperationPhaseChanged));
}

void OperationContext::progress(uint64_t done, uint64_t total, bool force) {
    auto& bus = EventBus::instance();
    if (!bus.wants(EventType::OperationProgress)) return;
    if (!throttle_.allow(force)) return;

    Event event = make(EventType::OperationProgress);
    event.bytes_done = done;
    event.bytes_total = total;
    bus.publish(std::move(event));
}

void OperationContext::heartbeat(std::string_view message) {
    auto& bus = EventBus::instance();
    if (!bus.wants(EventType::OperationHeartbeat)) return;

    Event event = make(EventType::OperationHeartbeat);
    event.message = std::string(message);
    bus.publish(std::move(event));
}

void OperationContext::output(std::string_view chunk, bool is_stderr) {
    auto& bus = EventBus::instance();
    if (!bus.wants(EventType::OperationOutput)) return;

    Event event = make(EventType::OperationOutput);
    event.message = std::string(chunk);
    // Признак потока держим в bytes_done: отдельного поля в Event нет, а
    // расширять структуру ради него — ломать ABI.
    event.bytes_done = is_stderr ? 1 : 0;
    bus.publish(std::move(event));
}

void OperationContext::retry(Status reason, std::string_view message) {
    auto& bus = EventBus::instance();
    if (!bus.wants(EventType::OperationRetry)) return;

    Event event = make(EventType::OperationRetry);
    event.status = reason;
    event.message = std::string(message);
    bus.publish(std::move(event));
}

void OperationContext::finish(const Result& result) {
    auto& bus = EventBus::instance();

    // Тип финального события выбираем по статусу: подписчику не нужно разбирать
    // Status, чтобы понять, чем всё кончилось.
    EventType type;
    switch (result.status) {
        case Status::Ok:
            type = EventType::OperationFinished;
            break;
        case Status::Canceled:
        case Status::ConnectionClosed:
            type = EventType::OperationCanceled;
            break;
        case Status::CommandTimeout:
        case Status::StallTimeout:
        case Status::ConnectTimeout:
        case Status::SlotTimeout:
            type = EventType::OperationTimeout;
            break;
        default:
            type = EventType::OperationFailed;
            break;
    }

    if (bus.wants(type)) {
        Event event = make(type);
        event.phase = result.phase;
        event.status = result.status;
        event.remote_code = result.remote_code;
        event.message = result.error;
        event.bytes_done = result.transfer.bytes;
        event.bytes_total = result.transfer.bytes;
        event.stats = result.transfer;
        event.elapsed = result.duration;
        bus.publish(std::move(event));
    }

    // OperationStats — отдельным событием и только если было что передавать.
    if (result.transfer.bytes > 0 && bus.wants(EventType::OperationStats)) {
        Event event = make(EventType::OperationStats);
        event.phase = result.phase;
        event.status = result.status;
        event.bytes_done = result.transfer.bytes;
        event.bytes_total = result.transfer.bytes;
        event.stats = result.transfer;
        event.elapsed = result.duration;
        bus.publish(std::move(event));
    }
}

// ---------------------------------------------------------------------------

void publish_device_event(EventType type, const std::string& serial, Status status,
                          std::string_view message) {
    auto& bus = EventBus::instance();
    if (!bus.wants(type)) return;

    Event event;
    event.type = type;
    event.serial = serial;
    event.status = status;
    event.command = Command::Connect;
    event.message = std::string(message);
    bus.publish(std::move(event));
}

}  // namespace internal
}  // namespace libadb
