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
    const std::string serial = getSerial();
    withListener([&](IDeviceListener& l) { l.onConnectionStateChanged(serial, ConnectionState::kCsConnecting); });

    // Запускаем connect_device в отдельном потоке, так как она блокирующая
    // (внутри register_socket_transport есть WaitForConnection на 10 секунд, а TCP connect
    // к молчащему адресу ждёт таймаута ядра). Поток может пережить устройство: вызывающий
    // не дождался (connect_timeout) и закрыл его — поэтому только weak_ptr, не this.
    std::weak_ptr<AdbDevice> weak = weak_from_this();
    std::thread([weak, serial]() {
        std::string response;

        // Вызываем готовую серверную функцию ADB
        connect_device(serial, &response);

        std::string actual_serial;
        for (const std::string prefix : {"connected to ", "already connected to "}) {
            if (android::base::StartsWith(response, prefix)) {
                actual_serial = android::base::Trim(response.substr(prefix.length()));
                break;
            }
        }

        std::shared_ptr<AdbDevice> self = weak.lock();
        if (!self || self->closed_) {
            // Подключение уже никому не нужно: не оставляем висеть транспорт, иначе следующий
            // connect к этому адресу получит "already connected" к чужому соединению
            if (!actual_serial.empty()) {
                atransport* t = find_transport(actual_serial.c_str());
                if (t) kick_transport(t, true);
            }
            LOG(INFO) << "Connection to " << serial << " finished after the device was closed: " << response;
            return;
        }

        if (!actual_serial.empty()) {
            // connect_device канонизировала serial (например, "sr" -> "sr:5555").
            // Сохраняем именно этот serial, чтобы потом находить транспорт.
            {
                std::lock_guard<std::mutex> lock(self->serial_mutex_);
                self->serial_ = actual_serial;
            }

            LOG(INFO) << "Successfully connected to " << actual_serial;
            self->withListener([&](IDeviceListener& l) { l.onConnectionStateChanged(actual_serial, ConnectionState::kCsDevice); });
        } else {
            LOG(ERROR) << "Failed to connect to " << serial << ": " << response;
            self->withListener([&](IDeviceListener& l) { l.onError(serial, "Connect failed: " + response); });
        }
    }).detach();

    return true; // Возвращаем true, чтобы показать, что процесс запущен
}

std::shared_ptr<AdbSession> AdbDevice::createSession(const std::string& service_string, bool use_shell2) {
    // Остальные сессии устройства не трогаем: каждая — свой поток протокола
    // (свой A_OPEN/local-id) в общем транспорте.
    atransport* t = getTransport();
    if (!t) {
        notifyError("Device not connected or transport lost");
        return nullptr;
    }

    auto session = std::make_shared<AdbSession>(shared_from_this(), next_session_id_++, service_string, use_shell2);
    registerSession(session);
    return session;
}

void AdbDevice::registerSession(std::shared_ptr<AdbSession> session) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    // Заодно выбрасываем завершённые: сессии не снимаются с учёта сами
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (it->second.expired()) it = sessions_.erase(it); else ++it;
    }
    sessions_[session->getId()] = session;
}

void AdbDevice::unregisterSession(uint32_t session_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    sessions_.erase(session_id);
}

void AdbDevice::close() {
    if (closed_.exchange(true)) return;
    // 1. Прерываем все живые сессии. abort() ждёт fdevent-цикл, поэтому вне мьютекса;
    // сессии connectServiceSync() освобождаем тоже вне его (деструктор зовёт abort()).
    std::vector<std::shared_ptr<AdbSession>> alive;
    std::unordered_multimap<std::thread::id, std::shared_ptr<AdbSession>> active;
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        for (auto& pair : sessions_) {
            if (auto s = pair.second.lock()) alive.push_back(std::move(s));
        }
        sessions_.clear();
        active.swap(active_sessions_);
    }
    for (auto& s : alive) s->abort();

    // 2. Корректно завершаем транспорт
    atransport* t = getTransport();
    if (t) {
        kick_transport(t, true); // true = reset, что закроет сокет и остановит потоки
    }

    const std::string serial = getSerial();
    withListener([&](IDeviceListener& l) { l.onConnectionStateChanged(serial, ConnectionState::kCsDetached); });
}

void AdbDevice::notifyError(const std::string& msg) {
    const std::string serial = getSerial();
    withListener([&](IDeviceListener& l) { l.onError(serial, msg); });
}

void AdbDevice::detachListener(IDeviceListener* listener) {
    std::lock_guard<std::mutex> lock(listener_mutex_);
    if (listener_ == listener) listener_ = nullptr;
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

int AdbDevice::connectServiceSync(const std::string& service) {
    auto session = createSession(service);
    if (!session || !session->start(false)) {
        return -1;
    }
    
    // Сохраняем сессию, чтобы ADB не удалил asocket раньше времени
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        active_sessions_.emplace(std::this_thread::get_id(), session);
    }
    
    // ОТДАЕМ ВЛАДЕНИЕ дескриптором вызывающему коду.
    return session->releaseFd(); 
}

void AdbDevice::clearActiveSessions() {
    std::vector<std::shared_ptr<AdbSession>> released;
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        auto range = active_sessions_.equal_range(std::this_thread::get_id());
        for (auto it = range.first; it != range.second; ++it) released.push_back(std::move(it->second));
        active_sessions_.erase(range.first, range.second);
    }
    // Деструкторы сессий — вне мьютекса
}
