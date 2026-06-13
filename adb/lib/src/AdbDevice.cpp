// AdbDevice.cpp
#include "AdbDevice.h"
#include "transport.h" // Для register_socket_transport
#include "socket.h"         // Для connect_to_remote
#include <sys/socket.h>
#include <android-base/logging.h>
#include <android-base/parsenetaddress.h>

AdbDevice::AdbDevice(const std::string& address, IDeviceListener* listener)
    : serial_(address), listener_(listener) {}

AdbDevice::~AdbDevice() {
    close();
}

bool AdbDevice::initiateConnection() {
    std::string host;
    int port = DEFAULT_ADB_LOCAL_TRANSPORT_PORT;
    std::string error;

    // Парсим адрес (например, "192.168.1.10:5555")
    if (!android::base::ParseNetAddress(serial_, &host, &port, nullptr, &error)) {
        if (listener_) listener_->onError(serial_, "Invalid address: " + error);
        return false;
    }

    // Создаем обычный TCP сокет
    unique_fd fd(network_connect(host.c_str(), port, SOCK_STREAM, 0, &error));
    if (fd < 0) {
        if (listener_) listener_->onError(serial_, "TCP connect failed: " + error);
        return false;
    }

    // Колбэк переподключения (для простоты отключаем, возвращаем Abort)
    auto reconnect_cb = [](atransport*) { return ReconnectResult::Abort; };

    // ВАЖНО: Эта функция из transport.cpp берет наш fd, оборачивает его в FdConnection,
    // создает atransport, регистрирует его в fdevent и автоматически отправляет A_CNXN!
    int reg_error = 0;
    if (!register_socket_transport(std::move(fd), serial_, port, 0, reconnect_cb, false, &reg_error)) {
        if (listener_) listener_->onError(serial_, "Failed to register transport");
        return false;
    }

    // Мы не можем легко получить указатель на созданный atransport из register_socket_transport,
    // поэтому мы найдем его по серийному номеру.
    // (В идеале нужно немного пропатчить register_socket_transport, чтобы он возвращал atransport*,
    // но find_transport работает надежно).
    
    // Даем fdevent пару миллисекунд на инициализацию
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    
    transport_.reset(find_transport(serial_.c_str()));
    if (!transport_) {
        if (listener_) listener_->onError(serial_, "Transport not found after registration");
        return false;
    }

    if (listener_) listener_->onConnectionStateChanged(serial_, ConnectionState::Connecting);
    return true;
}

std::shared_ptr<AdbSession> AdbDevice::createSession(const std::string& service_string) {
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
    if (transport_) {
        kick_transport(transport_.get(), true); // Закрывает сокет и останавливает потоки чтения/записи
        transport_.reset();
    }
    if (listener_) listener_->onConnectionStateChanged(serial_, ConnectionState::Disconnected);
}

void AdbDevice::notifyStateChanged(ConnectionState state) {
    if (listener_) listener_->onConnectionStateChanged(serial_, state);
    if (state == ConnectionState::Unauthorized && listener_) {
        listener_->onAuthRequired(serial_);
    }
}

void AdbDevice::notifyError(const std::string& msg) {
    if (listener_) listener_->onError(serial_, msg);
}
