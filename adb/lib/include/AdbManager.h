#pragma once
#include <memory>
#include <string>
#include <thread>
#include <mutex>
#include <unordered_map>
#include "adb.h"
#include "fdevent.h"
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
};
