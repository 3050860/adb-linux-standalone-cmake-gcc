#include <string>
#include "adb.h"

class IDeviceListener {
public:
    virtual ~IDeviceListener() = default;

    // Состояние подключения изменилось (device, offline, unauthorized, connecting)
    virtual void onConnectionStateChanged(ConnectionState state) = 0;

    // Устройство запросило авторизацию (нужно показать диалог или подтвердить ключ)
    virtual void onAuthRequired(const std::string& device_name) = 0;

    // Для shell-команд: пришли данные (stdout/stderr). 
    // Позволяет стримить вывод, а не ждать окончания.
    virtual void onShellData(const std::string& data, bool is_stderr) = 0;

    // Для push/pull: прогресс передачи
    virtual void onFileTransferProgress(uint64_t bytes_transferred, uint64_t total_bytes) = 0;

    // Критическая ошибка, после которой сессия или устройство закрываются
    virtual void onError(const std::string& error_message) = 0;
};
