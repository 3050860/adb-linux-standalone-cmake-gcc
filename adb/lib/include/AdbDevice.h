#pragma once
#include <memory>
#include <string>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <unordered_map>
#include "adb.h"
#include "transport.h"
#include "AdbSession.h"
#include "IadbListener.h"

class AdbDevice : public std::enable_shared_from_this<AdbDevice> {
public:
    AdbDevice(const std::string& address, IDeviceListener* listener);
    ~AdbDevice();

    bool initiateConnection();
    std::shared_ptr<AdbSession> createSession(const std::string& service_string, bool use_shell2=false);
    void close();

    std::string getSerial() const { 
        std::lock_guard<std::mutex> lock(serial_mutex_);
        return serial_; 
    }

    FeatureSet const getFeatures();
    bool hasFeature(const std::string& feature);
    std::shared_ptr<AdbSession> createShellSession(const std::string& command, bool force_raw=false);

    void notifyError(const std::string& msg);

    // Отцепить слушателя (если это он): после возврата его методы больше не вызываются,
    // и текущий вызов уже завершён — слушателя можно уничтожать.
    void detachListener(IDeviceListener* listener);

    // Вызвать слушателя под мьютексом; без слушателя — ничего.
    template <typename F>
    void withListener(F&& f) {
        std::lock_guard<std::mutex> lock(listener_mutex_);
        if (listener_) f(*listener_);
    }

    // Безопасно получаем актуальный транспорт из глобального списка ADB
    atransport* getTransport() {
        std::lock_guard<std::mutex> lock(serial_mutex_);
        return find_transport(serial_.c_str());
    }

    // Синхронно создает сессию, запускает её и возвращает FD.
    // Сессия сохраняется внутри устройства (за вызвавшим потоком), чтобы не быть уничтоженной.
    int connectServiceSync(const std::string& service);
    // Отпускает сессии connectServiceSync() текущего потока (вызывать после завершения
    // install/uninstall). Сессии других потоков — других операций на том же устройстве — не трогает.
    void clearActiveSessions();

private:
    mutable std::mutex serial_mutex_;
    std::string serial_; // Может измениться после connect_device (например, добавится порт)
    std::mutex listener_mutex_;
    IDeviceListener* listener_;
    std::atomic_bool closed_{false};
    
    // Сессии устройства независимы: по одному транспорту одновременно идут потоки
    // разных операций (мультиплексирование протокола ADB). Здесь слабые ссылки —
    // только чтобы прервать живые сессии в close(); владеет сессией операция.
    std::mutex sessions_mutex_;
    std::unordered_map<uint32_t, std::weak_ptr<AdbSession>> sessions_;
    std::unordered_multimap<std::thread::id, std::shared_ptr<AdbSession>> active_sessions_;
    std::atomic<uint32_t> next_session_id_{1};

    friend class AdbSession;
    void registerSession(std::shared_ptr<AdbSession> session);
    void unregisterSession(uint32_t session_id);
};
