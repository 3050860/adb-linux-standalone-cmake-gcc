#pragma once
#include <memory>
#include <string>
#include <mutex>
#include <unordered_map>
#include "adb.h"
#include "transport.h"
#include "AdbSession.h"
#include "IadbListener.h"

class AdbDevice : public std::enable_shared_from_this<AdbDevice> {
public:
    AdbDevice(const std::string& address, IDeviceListener* listener);
    ~AdbDevice();

    // Инициирует TCP-подключение и регистрирует транспорт в ADB
    bool initiateConnection();

    // Создает новую сессию (например, shell-команду)
    std::shared_ptr<AdbSession> createSession(const std::string& service_string);

    // Принудительно закрывает соединение и все сессии
    void close();

    const std::string& getSerial() const { return serial_; }

    // Вызывается изнутри ADB (через кастомный fdevent) при смене состояния
    void notifyStateChanged(ConnectionState state);
    void notifyAuthRequired();
    void notifyError(const std::string& msg);

private:
    std::string serial_;
    IDeviceListener* listener_;
    std::shared_ptr<atransport> transport_;
    
    std::mutex sessions_mutex_;
    std::unordered_map<uint32_t, std::shared_ptr<AdbSession>> sessions_;
    uint32_t next_session_id_ = 1;

    friend class AdbSession;
    void registerSession(std::shared_ptr<AdbSession> session);
    void unregisterSession(uint32_t session_id);
};
