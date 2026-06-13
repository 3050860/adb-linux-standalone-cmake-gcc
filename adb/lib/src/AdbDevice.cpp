#include "AdbDevice.h"
#include <android-base/logging.h>
#include <android-base/strings.h>

AdbDevice::AdbDevice(const std::string& address, IDeviceListener* listener)
    : serial_(address), listener_(listener) {}

AdbDevice::~AdbDevice() {
    close();
}

bool AdbDevice::initiateConnection() {
    if (listener_) listener_->onConnectionStateChanged(serial_, ConnectionState::kCsConnecting);

    // Запускаем connect_device в отдельном потоке, так как она блокирующая
    // (внутри register_socket_transport есть WaitForConnection на 10 секунд).
    std::thread([this]() {
        std::string response;
        
        // Вызываем готовую серверную функцию ADB
        connect_device(serial_, &response);

        const std::string kSuccessPrefix = "connected to ";
        if (android::base::StartsWith(response, kSuccessPrefix)) {
            // connect_device канонизировала serial (например, "sr" -> "sr:5555").
            // Сохраняем именно этот serial, чтобы потом находить транспорт.
            std::string actual_serial = android::base::Trim(response.substr(kSuccessPrefix.length()));
            
            {
                std::lock_guard<std::mutex> lock(serial_mutex_);
                serial_ = actual_serial;
            }

            LOG(INFO) << "Successfully connected to " << actual_serial;
            if (listener_) listener_->onConnectionStateChanged(actual_serial, ConnectionState::kCsDevice);
        } else {
            LOG(ERROR) << "Failed to connect to " << serial_ << ": " << response;
            if (listener_) listener_->onError(serial_, "Connect failed: " + response);
        }
    }).detach();

    return true; // Возвращаем true, чтобы показать, что процесс запущен
}

std::shared_ptr<AdbSession> AdbDevice::createSession(const std::string& service_string) {
    atransport* t = getTransport();
    if (!t) {
        if (listener_) listener_->onError(getSerial(), "Device not connected or transport lost");
        return nullptr;
    }

    auto session = std::make_shared<AdbSession>(shared_from_this(), next_session_id_++, service_string);
    registerSession(session);
    return session;
}

void AdbDevice::registerSession(std::shared_ptr<AdbSession> session) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    sessions_[session->getId()] = session;
}

void AdbDevice::unregisterSession(uint32_t session_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    sessions_.erase(session_id);
}

void AdbDevice::close() {
    // 1. Прерываем все активные сессии
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        for (auto& pair : sessions_) {
            pair.second->abort();
        }
        sessions_.clear();
    }

    // 2. Корректно завершаем транспорт
    atransport* t = find_transport(serial_.c_str());
    if (t) {
        kick_transport(t, true); // true = reset, что закроет сокет и остановит потоки
    }
    
    if (listener_) {
        listener_->onConnectionStateChanged(getSerial(), ConnectionState::kCsDetached);
    }
}

// void AdbDevice::close() {
//     // Чтобы отключить устройство, мы можем использовать готовую функцию 
//     // kick_transport или просто удалить его из списка, но для TCP проще 
//     // вызвать host-запрос disconnect. 
//     // В рамках концепта просто очистим сессии.
//     std::lock_guard<std::mutex> lock(sessions_mutex_);
//     sessions_.clear();
    
//     if (listener_) listener_->onConnectionStateChanged(getSerial(), ConnectionState::kCsDetached);
// }

void AdbDevice::notifyError(const std::string& msg) {
    if (listener_) listener_->onError(getSerial(), msg);
}
