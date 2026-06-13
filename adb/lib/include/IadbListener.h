#pragma once
#include <string>
#include <cstdint>
#include "adb.h"

// // Состояния подключения, адаптированные из adb.h
// enum class ConnectionState {
//     Connecting,
//     Authorizing,
//     Unauthorized,
//     Offline,
//     Device,
//     Disconnected
// };

class IDeviceListener {
public:
    virtual ~IDeviceListener() = default;

    // Изменилось состояние подключения устройства
    virtual void onConnectionStateChanged(const std::string& serial, ConnectionState state) = 0;

    // Устройство запросило авторизацию (нужно подтвердить ключ на экране устройства или предоставить ключ)
    virtual void onAuthRequired(const std::string& serial) = 0;

    // Пришли данные от shell-команды или exec-сервиса
    virtual void onShellData(const std::string& serial, uint32_t session_id, const char* data, size_t len, bool is_stderr) = 0;

    // Сессия (команда) завершена
    virtual void onSessionClosed(const std::string& serial, uint32_t session_id, int exit_code) = 0;

    // Критическая ошибка (например, обрыв связи)
    virtual void onError(const std::string& serial, const std::string& error_msg) = 0;
};
