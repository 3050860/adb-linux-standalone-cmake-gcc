#include "AdbDevice.h"
#include <android-base/logging.h>
#include <android-base/strings.h>

AdbDevice::AdbDevice(const std::string& address, IDeviceListener* listener)
    : serial_(address), listener_(listener) {}

AdbDevice::~AdbDevice() {
    close();
}

FeatureSet const AdbDevice::getFeatures() {
    atransport* t = getTransport();
    if (t) {
        return t->features();
    }
    return FeatureSet();
}

bool AdbDevice::hasFeature(const std::string& feature) {
    return CanUseFeature(getFeatures(), feature);
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

std::shared_ptr<AdbSession> AdbDevice::createSession(const std::string& service_string, bool use_shell2) {
    // 1. Прерываем все активные сессии
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        for (auto& pair : sessions_) {
            pair.second->abort();
        }
        sessions_.clear();
    }
    atransport* t = getTransport();
    if (!t) {
        if (listener_) listener_->onError(getSerial(), "Device not connected or transport lost");
        return nullptr;
    }

    auto session = std::make_shared<AdbSession>(shared_from_this(), next_session_id_++, service_string, use_shell2);
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

void AdbDevice::notifyError(const std::string& msg) {
    if (listener_) listener_->onError(getSerial(), msg);
}

/* ========================================================================================================= */

std::shared_ptr<AdbSession> AdbDevice::createShellSession(const std::string& command, bool force_raw) {
    std::string service_string = "shell:" + command;

    // Если устройство поддерживает shell_v2, и мы не форсируем старый режим
    if (!force_raw && hasFeature(kFeatureShell2)) {
        // Формируем строку в стиле оригинального ADB: shell,v2,TERM=...,raw:command
        const char* term = getenv("TERM");
        std::string term_arg = term ? std::string("TERM=") + term : "";
        
        service_string = "shell,v2";
        if (!term_arg.empty()) {
            service_string += "," + term_arg;
        }
        service_string += ",raw:" + command;
        
        LOG(INFO) << "Upgrading shell command to v2 protocol: " << service_string;
        return createSession(service_string, true);
    } else {
        LOG(INFO) << "Using legacy shell protocol for: " << service_string;
        return createSession(service_string, false);
    }
}