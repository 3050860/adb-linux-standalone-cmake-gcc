#pragma once
#include <memory>
#include <string>
#include <mutex>
#include <unordered_map>
#include "adb.h"
#include "transport.h" // Для find_transport, connect_device
#include "AdbSession.h"
#include "IadbListener.h"

class AdbDevice : public std::enable_shared_from_this<AdbDevice> {
public:
    AdbDevice(const std::string& address, IDeviceListener* listener);
    ~AdbDevice();

    bool initiateConnection();
    std::shared_ptr<AdbSession> createSession(const std::string& service_string);
    void close();

    std::string getSerial() const { 
        std::lock_guard<std::mutex> lock(serial_mutex_);
        return serial_; 
    }

    void notifyError(const std::string& msg);

    // Безопасно получаем актуальный транспорт из глобального списка ADB
    atransport* getTransport() {
        std::lock_guard<std::mutex> lock(serial_mutex_);
        return find_transport(serial_.c_str());
    }

private:
    mutable std::mutex serial_mutex_;
    std::string serial_; // Может измениться после connect_device (например, добавится порт)
    IDeviceListener* listener_;
    
    std::mutex sessions_mutex_;
    std::unordered_map<uint32_t, std::shared_ptr<AdbSession>> sessions_;
    uint32_t next_session_id_ = 1;

    friend class AdbSession;
    void registerSession(std::shared_ptr<AdbSession> session);
    void unregisterSession(uint32_t session_id);
};
