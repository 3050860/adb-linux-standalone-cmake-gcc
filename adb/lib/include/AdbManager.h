#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "adb.h"
#include "fdevent/fdevent.h"
#include "IadbListener.h"


class AdbDevice;

class AdbManager {
public:
    static AdbManager& instance();

    // Запускает фоновый поток обработки событий (fdevent_loop). 
    // Должен быть вызван один раз при инициализации библиотеки.
    void start();
    
    // Останавливает менеджер и ждет завершения потока.
    void stop();

    // Максимальное количество устройств, обрабатываемых одновременно.
    // 0 (по умолчанию) — без ограничения (поток на каждое устройство).
    // Настройка глобальная и действует для любых команд, выполняемых
    // через runOnDevices() (push/pull/shell/install/...).
    void setMaxThreads(size_t max_threads);
    size_t maxThreads() const;

    // Выполняет task для каждого адреса из addresses, но не более maxThreads()
    // одновременно. Подключение к очередному устройству происходит внутри
    // задачи, т.е. к следующим устройствам подключаемся только после того,
    // как освободился рабочий поток. Блокируется до завершения всех задач.
    void runOnDevices(const std::vector<std::string>& addresses,
                      const std::function<void(const std::string& address)>& task);


    // Подключается к устройству по TCP (например, "192.168.1.10:5555")
    // Возвращает shared_ptr на устройство или nullptr при ошибке.
    std::shared_ptr<AdbDevice> connectDevice(const std::string& address, IDeviceListener* listener);

    // Отключает устройство и очищает ресурсы.
    void disconnectDevice(const std::string& address);

private:
    AdbManager() = default;
    ~AdbManager();

    void eventLoopThread();

    std::thread event_thread_;
    std::mutex devices_mutex_;
    std::unordered_map<std::string, std::shared_ptr<AdbDevice>> devices_;
    bool is_running_ = false;
    std::atomic<size_t> max_threads_{0};

};
