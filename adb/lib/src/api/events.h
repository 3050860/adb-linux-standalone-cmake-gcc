/*
 * libadb — внутренняя шина событий (§8). Не устанавливается, в ABI не входит.
 *
 * Модель: производители (потоки adb, рабочие потоки операций) складывают
 * события в очередь, единственный диспетчер-поток разбирает её и вызывает
 * подписчиков. Так медленный обработчик не тормозит протокол.
 */
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "api/internal.h"
#include "libadb/libadb.h"

namespace libadb::internal {

// Шина событий: одна на процесс (как и Client).
class LIBADB_INTERNAL EventBus {
  public:
    static EventBus& instance();

    // Запускает диспетчер-поток, если он ещё не запущен. Идемпотентно.
    void start();

    // Досылает всё, что в очереди, и останавливает диспетчер.
    void stop();

    SubscriptionId subscribe(EventFn handler, EventMask mask);
    void unsubscribe(SubscriptionId id);

    // Подписка «из Options»: повторный initialize() заменяет её, а не плодит
    // дубликаты. Пустой handler снимает подписку.
    void set_primary_subscription(EventFn handler, EventMask mask);

    void set_queue_limit(size_t limit);
    void set_progress_interval(ms interval);
    ms progress_interval() const;

    // Есть ли хоть один подписчик, которому интересен этот тип события.
    // Дешёвая проверка перед формированием Event (строки, статистика).
    bool wants(EventType type) const;

    // Кладёт событие в очередь. timestamp заполняется здесь, если он нулевой.
    void publish(Event event);

    // Ждёт, пока очередь опустеет (для тестов и shutdown).
    void drain();

    // Сколько событий выброшено по переполнению очереди (суммарно).
    uint64_t dropped() const;

    // Монотонное время от старта шины.
    ms now() const;

    // Следующий идентификатор операции. Ноль не выдаётся никогда.
    static OperationId next_operation_id();

  private:
    EventBus();
    ~EventBus();

    void dispatch_loop();
    void recompute_mask_locked();

    struct Subscriber {
        SubscriptionId id = 0;
        EventMask mask = 0;
        EventFn handler;
    };

    mutable std::mutex mutex_;
    std::condition_variable queue_cv_;   // «есть работа» для диспетчера
    std::condition_variable drain_cv_;   // «очередь опустела» для drain()
    std::deque<Event> queue_;
    std::vector<Subscriber> subscribers_;
    SubscriptionId next_id_ = 1;
    SubscriptionId primary_id_ = 0;

    // Объединённая маска всех подписчиков: атомарная, чтобы wants() не брал лок.
    std::atomic<EventMask> combined_mask_{0};

    size_t queue_limit_ = 10000;
    ms progress_interval_{200};
    uint64_t dropped_total_ = 0;
    uint64_t dropped_pending_ = 0;  // ещё не сообщённые через InternalError

    bool running_ = false;
    bool dispatching_ = false;
    std::thread dispatcher_;
    std::chrono::steady_clock::time_point epoch_;
};

// Троттлинг прогресса одной операции: помнит время последнего события.
// Живёт в объекте операции, поэтому синхронизация не нужна (прогресс идёт
// из одного потока — того, что выполняет операцию).
class LIBADB_INTERNAL ProgressThrottle {
  public:
    // true — событие пора отправлять. force=true для последнего события
    // (100 %), чтобы оно не потерялось из-за интервала.
    bool allow(bool force);

  private:
    std::chrono::steady_clock::time_point last_{};
    bool first_ = true;
};

// Контекст одной операции: id, устройство, команда, фаза. Используется
// device.cpp для событий; на этапе 8 сюда добавится флаг отмены.
class LIBADB_INTERNAL OperationContext {
  public:
    OperationContext(Command command, std::string serial);

    OperationId id() const { return id_; }
    Command command() const { return command_; }
    const std::string& serial() const { return serial_; }
    Phase phase() const { return phase_; }
    ms elapsed() const;

    // Отдаёт id вызывающему (StartedFn) и публикует OperationStarted.
    void start(const StartedFn& on_start);

    // Меняет фазу и публикует OperationPhaseChanged (если фаза изменилась).
    void set_phase(Phase phase);

    void progress(uint64_t done, uint64_t total, bool force = false);
    void heartbeat(std::string_view message);
    void output(std::string_view chunk, bool is_stderr);
    void retry(Status reason, std::string_view message);

    // Публикует финальное событие по статусу результата (Finished/Failed/
    // Timeout/Canceled) и, если есть статистика передачи, OperationStats.
    void finish(const Result& result);

  private:
    Event make(EventType type) const;

    OperationId id_;
    Command command_;
    std::string serial_;
    Phase phase_ = Phase::None;
    std::chrono::steady_clock::time_point started_;
    ProgressThrottle throttle_;
};

// Событие уровня устройства/клиента одной строкой.
LIBADB_INTERNAL void publish_device_event(EventType type, const std::string& serial, Status status,
                                         std::string_view message);

}  // namespace libadb::internal
