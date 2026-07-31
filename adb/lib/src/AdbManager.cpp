// AdbManager.cpp
#include "AdbManager.h"
#include "AdbDevice.h"
#include "adb_auth.h"
#include "transport.h"
#include <android-base/logging.h>

AdbManager& AdbManager::instance() {
    static AdbManager instance;
    return instance;
}

void AdbManager::start() {
    if (is_running_) return;
    
    // Инициализируем аутентификацию (загружает ~/.android/adbkey)
    adb_auth_init();
    
    is_running_ = true;
    event_thread_ = std::thread(&AdbManager::eventLoopThread, this);
}
void AdbManager::stop() {
    if (!is_running_) return;

    // 1. Явно отключаем все устройства, пока fdevent_loop еще работает,
    // а объекты-слушатели (например, переменная listener в main) еще валидны.
    {
        std::lock_guard<std::mutex> lock(devices_mutex_);
        for (auto& pair : devices_) {
            pair.second->close(); // Это корректно вызовет onConnectionStateChanged(Disconnected)
        }
        devices_.clear(); // Очищаем карту, чтобы ~AdbManager() не пытался делать это позже
    }

    // 2. Останавливаем цикл событий
    is_running_ = false;
    fdevent_terminate_loop();
    if (event_thread_.joinable()) {
        event_thread_.join();
    }
}

void AdbManager::setMaxThreads(size_t max_threads) {
    max_threads_.store(max_threads);
}

size_t AdbManager::maxThreads() const {
    return max_threads_.load();
}

void AdbManager::runOnDevices(const std::vector<std::string>& addresses,
                              const std::function<void(const std::string&)>& task) {
    if (addresses.empty() || !task) return;

    size_t workers_count = max_threads_.load();
    if (workers_count == 0 || workers_count > addresses.size()) {
        workers_count = addresses.size();
    }

    LOG(INFO) << "AdbManager: processing " << addresses.size() << " device(s) using "
              << workers_count << " worker thread(s)";

    // Общий индекс очереди: воркер берёт следующий адрес только когда освободился,
    // т.е. одновременно обрабатывается не более workers_count устройств.
    std::atomic<size_t> next_index{0};

    std::vector<std::thread> workers;
    workers.reserve(workers_count);
    for (size_t i = 0; i < workers_count; ++i) {
        workers.emplace_back([&addresses, &task, &next_index]() {
            for (;;) {
                const size_t index = next_index.fetch_add(1);
                if (index >= addresses.size()) return;

                const std::string& address = addresses[index];
                try {
                    task(address);
                } catch (const std::exception& e) {
                    // Один упавший девайс не должен утаскивать за собой воркер.
                    LOG(ERROR) << "AdbManager: task for " << address << " threw: " << e.what();
                } catch (...) {
                    LOG(ERROR) << "AdbManager: task for " << address << " threw unknown exception";
                }
            }
        });
    }

    for (auto& worker : workers) {
        worker.join();
    }
}

void AdbManager::eventLoopThread() {

    LOG(INFO) << "AdbManager: fdevent_loop started";
    fdevent_loop(); // Блокирующий вызов, работает пока не вызван fdevent_terminate_loop
    LOG(INFO) << "AdbManager: fdevent_loop stopped";
}

std::shared_ptr<AdbDevice> AdbManager::connectDevice(const std::string& address, IDeviceListener* listener) {
    std::lock_guard<std::mutex> lock(devices_mutex_);
    
    if (devices_.find(address) != devices_.end()) {
        return devices_[address]; // Уже подключено
    }

    auto device = std::make_shared<AdbDevice>(address, listener);
    if (device->initiateConnection()) {
        devices_[address] = device;
        return device;
    }
    
    return nullptr;
}

void AdbManager::disconnectDevice(const std::string& address) {
    std::lock_guard<std::mutex> lock(devices_mutex_);
    auto it = devices_.find(address);
    if (it != devices_.end()) {
        it->second->close();
        devices_.erase(it);
    }
}

AdbManager::~AdbManager() {
    stop();
}
