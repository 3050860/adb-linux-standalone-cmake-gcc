#define TRACE_TAG ADB

#include <iostream>
#include <string>
#include <mutex>
#include <condition_variable>
#include <chrono>

#include "AdbManager.h"
#include "AdbDevice.h"
#include "AdbSession.h"
#include "IadbListener.h"


const char** __adb_argv;
const char** __adb_envp;

class MyListener : public IDeviceListener {
public:
    std::mutex mtx;
    std::condition_variable cv;
    bool connected = false;
    bool session_done = false;
    bool error_occurred = false;

    void onConnectionStateChanged(const std::string& serial, ConnectionState state) override {
        std::cout << "[STATE] " << serial << " -> state " << static_cast<int>(state) << std::endl;
        if (state == ConnectionState::kCsDevice) {
            std::lock_guard<std::mutex> lock(mtx);
            connected = true;
            cv.notify_all();
        }
    }

    void onAuthRequired(const std::string& serial) override {
        std::cout << "[AUTH] Please accept the RSA key on the device screen: " << serial << std::endl;
        // Устройство запросило подтверждение ключа. Пользователь должен взять телефон и нажать "ОК".
    }

    void onShellData(const std::string& serial, uint32_t session_id, const char* data, size_t len, bool is_stderr) override {
        // Выводим сырые данные. Поскольку мы используем "shell:" без ",v2", это будет просто текст.
        fwrite(data, 1, len, stdout);
        fflush(stdout);
    }

    void onSessionClosed(const std::string& serial, uint32_t session_id, int exit_code) override {
        std::cout << "\n[SESSION] Closed with exit code " << exit_code << std::endl;
        std::lock_guard<std::mutex> lock(mtx);
        session_done = true;
        cv.notify_all();
    }

    void onError(const std::string& serial, const std::string& error_msg) override {
        std::cerr << "[ERROR] " << serial << ": " << error_msg << std::endl;
        std::lock_guard<std::mutex> lock(mtx);
        error_occurred = true;
        connected = true;   // Разбудим wait_for_connection, чтобы он проверил флаг ошибки
        session_done = true; // Разбудим wait_for_session
        cv.notify_all();
    }
    
    // Ждем подключения с таймаутом
    bool wait_for_connection(int timeout_ms = 15000) {
        std::unique_lock<std::mutex> lock(mtx);
        bool result = cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this]{ return connected || error_occurred; });
        return result && connected && !error_occurred;
    }

    // Ждем завершения сессии с таймаутом
    bool wait_for_session(int timeout_ms = 30000) {
        std::unique_lock<std::mutex> lock(mtx);
        bool result = cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this]{ return session_done || error_occurred; });
        return result && session_done && !error_occurred;
    }
};

int main(int argc, char* argv[], char* envp[]) {
    __adb_argv = const_cast<const char**>(argv);
    __adb_envp = const_cast<const char**>(envp);

    adb_trace_init(argv);

    // 1. Запускаем менеджер событий (fdevent_loop в фоне)
    // Внутри AdbManager::start() автоматически вызывается adb_auth_init(), 
    // который загружает RSA-ключи из ~/.android/adbkey.
    auto& manager = AdbManager::instance();
    manager.start();

    // 2. Создаем слушатель
    static MyListener listener;

    // 3. Подключаемся к устройству
    std::string address = "192.168.177.248:5555";
    std::cout << "Connecting to " << address << "..." << std::endl;
    
    auto device = manager.connectDevice(address, &listener);
    if (!device) {
        std::cerr << "Failed to initiate connection." << std::endl;
        manager.stop();
        return 1;
    }

    // 4. Ждем, пока устройство не перейдет в состояние Device
    // (Это может занять несколько секунд, так как идет TCP-подключение и авторизация)
    std::cout << "Waiting for connection (up to 15 seconds)..." << std::endl;
    if (!listener.wait_for_connection()) {
        std::cerr << "Connection failed, timeout, or error occurred." << std::endl;
        manager.stop();
        return 1;
    }
    std::cout << "Device connected successfully!" << std::endl;

    // 5. Создаем сессию для выполнения команды
    // ВАЖНО: Используем "shell:" без ",v2", чтобы устройство слало просто текст, 
    // а не бинарный ShellProtocol. Иначе в консоли будет мусор.
    std::string command = "shell:df -h /sdcard";
    std::cout << "\nExecuting command: " << command << "\n" << std::endl;
    
    auto session = device->createSession(command);
    if (!session) {
        std::cerr << "Failed to create session." << std::endl;
        manager.stop();
        return 1;
    }

    // 6. Запускаем сессию (отправляет пакет A_OPEN на устройство)
    if (!session->start()) {
        std::cerr << "Failed to start session." << std::endl;
        manager.stop();
        return 1;
    }

    // 7. Ждем завершения сессии (устройство само закроет соединение, когда команда выполнится)
    if (!listener.wait_for_session()) {
        std::cerr << "Session failed or timeout." << std::endl;
    }

    // 8. Корректно завершаем работу (останавливаем фоновый поток fdevent_loop)
    manager.stop();
    std::cout << "\nDone." << std::endl;

    return 0;
}