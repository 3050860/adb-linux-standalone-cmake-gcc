#pragma once
#include <memory>
#include <string>
#include <mutex>
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

    // Безопасно получаем актуальный транспорт из глобального списка ADB
    atransport* getTransport() {
        std::lock_guard<std::mutex> lock(serial_mutex_);
        return find_transport(serial_.c_str());
    }

    // Синхронно создает сессию, запускает её и возвращает FD.
    // Сессия сохраняется внутри устройства, чтобы не быть уничтоженной.
    int connectServiceSync(const std::string& service);
    // Очищает все активные сессии (вызывать после завершения install/uninstall)
    void clearActiveSessions();

private:
    mutable std::mutex serial_mutex_;
    std::string serial_; // Может измениться после connect_device (например, добавится порт)
    IDeviceListener* listener_;
    
    std::mutex sessions_mutex_;
    std::unordered_map<uint32_t, std::shared_ptr<AdbSession>> sessions_;
    std::vector<std::shared_ptr<AdbSession>> active_sessions_;
    uint32_t next_session_id_ = 1;

    friend class AdbSession;
    void registerSession(std::shared_ptr<AdbSession> session);
    void unregisterSession(uint32_t session_id);
};
