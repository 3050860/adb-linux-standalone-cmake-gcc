#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <iomanip>
#include <getopt.h>
#include <android-base/strings.h>
#include <android-base/file.h>
#include "adb_utils.h"

#include "AdbManager.h"
#include "AdbDevice.h"
#include "AdbSession.h"
#include "AdbFileSync.h"
#include "AdbInstaller.h"
#include "IadbListener.h"

const char** __adb_argv;
const char** __adb_envp;

// ============================================================================
// Потокобезопасный Логгер
// ============================================================================
class Logger {
public:
    static std::mutex global_mutex_;

    Logger(const std::string& serial) : serial_(serial) {}

    template<typename... Args>
    void info(Args&&... args) {
        log("INFO", std::forward<Args>(args)...);
    }

    template<typename... Args>
    void error(Args&&... args) {
        log("ERROR", std::forward<Args>(args)...);
    }

    // Специальный метод для вывода сырых данных shell с префиксом
    void print_shell_line(const std::string& line) {
        std::lock_guard<std::mutex> lock(global_mutex_);
        print_prefix("SHELL");
        std::cout << line << "\n";
        std::cout.flush();
    }

private:
    std::string serial_;

    void print_prefix(const char* level) {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
        
        std::tm tm_buf;
        localtime_r(&time_t, &tm_buf);
        
        std::cout << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S") 
                  << "." << std::setfill('0') << std::setw(3) << ms.count()
                  << ":" << serial_ << ":" << level << ": ";
    }

    template<typename... Args>
    void log(const char* level, Args&&... args) {
        std::lock_guard<std::mutex> lock(global_mutex_);
        print_prefix(level);
        (std::cout << ... << args) << "\n";
        std::cout.flush();
    }
};
std::mutex Logger::global_mutex_;

// ============================================================================
// Слушатель событий для конкретного устройства
// ============================================================================
class DeviceListener : public IDeviceListener {
public:
    DeviceListener(Logger& log) : log_(log) {}

    void onConnectionStateChanged(const std::string& serial, ConnectionState state) override {
        if (state == ConnectionState::kCsDevice) {
            std::lock_guard<std::mutex> lock(mtx_);
            connected_ = true;
            log_.info("Device connected");
            cv_.notify_all();
        }
    }

    void onAuthRequired(const std::string& serial) override {
        log_.info("Auth required. Please accept RSA key on device screen.");
    }

    void onShellData(const std::string& serial, uint32_t session_id, const char* data, size_t len, bool is_stderr) override {
        std::lock_guard<std::mutex> lock(shell_mtx_);
        shell_buffer_.append(data, len);
        
        // Буферизуем вывод построчно, чтобы не ломать префиксы
        size_t pos = 0;
        while ((pos = shell_buffer_.find('\n')) != std::string::npos) {
            std::string line = shell_buffer_.substr(0, pos);
            if (!line.empty() && line.back() == '\r') line.pop_back(); // Убираем \r
            
            log_.print_shell_line(line);
            shell_buffer_.erase(0, pos + 1);
        }
    }

    void onSessionClosed(const std::string& serial, uint32_t session_id, int exit_code) override {
        // Достаем остаток буфера, если он есть
        std::lock_guard<std::mutex> lock(shell_mtx_);
        if (!shell_buffer_.empty()) {
            if (!shell_buffer_.empty() && shell_buffer_.back() == '\r') shell_buffer_.pop_back();
            log_.print_shell_line(shell_buffer_);
            shell_buffer_.clear();
        }
    }

    void onError(const std::string& serial, const std::string& error_msg) override {
        log_.error(error_msg);
        std::lock_guard<std::mutex> lock(mtx_);
        error_ = true;
        cv_.notify_all();
    }

    bool wait_for_device(int timeout_ms = 15000) {
        std::unique_lock<std::mutex> lock(mtx_);
        return cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), 
                            [this]{ return connected_ || error_; }) && connected_;
    }

private:
    Logger& log_;
    std::mutex mtx_;
    std::condition_variable cv_;
    bool connected_ = false;
    bool error_ = false;

    std::mutex shell_mtx_;
    std::string shell_buffer_;
};

// ============================================================================
// Утилиты
// ============================================================================
std::vector<std::string> read_devices_file(const std::string& path) {
    std::vector<std::string> devices;
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "Failed to open devices file: " << path << "\n";
        return devices;
    }

    std::string line;
    while (std::getline(file, line)) {
        // Trim
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);
        
        if (line.empty() || line[0] == '#') continue;
        
        // Добавляем порт по умолчанию, если его нет
        if (line.find(':') == std::string::npos) {
            line += ":5555";
        }
        devices.push_back(line);
    }
    return devices;
}

void print_help() {
    std::cout << "Usage: adirect -f <devices.txt> <command> [args...]\n"
              << "Commands:\n"
              << "  shell <command...>       Execute shell command on all devices\n"
              << "  push <local> <remote>    Push file to all devices\n"
              << "  pull <remote> <local>    Pull file from all devices\n"
              << "Options:\n"
              << "  -f, --file <path>        Path to file with device IPs (one per line)\n"
              << "  -h, --help               Show this help\n";
}

// ============================================================================
// Обработчики команд
// ============================================================================
void run_shell(std::shared_ptr<AdbDevice> device, const std::vector<std::string>& args, Logger& log) {
    std::string cmd = android::base::Join(args, ' ');
    log.info("Executing shell: ", cmd);
    
    auto session = device->createShellSession(cmd);
    if (!session || !session->start()) {
        log.error("Failed to start shell session");
        return;
    }
    
    int exit_code = session->wait();
    log.info("Shell finished with exit code: ", exit_code);
}

void run_push(std::shared_ptr<AdbDevice> device, const std::vector<std::string>& args, Logger& log) {
    if (args.size() != 2) {
        log.error("push requires <local> <remote> arguments");
        return;
    }
    std::string local = args[0];
    std::string remote = args[1];
    
    log.info("Pushing ", local, " to ", remote);
    AdbFileSync sync(device);
    // quiet = true, чтобы SyncConnection не ломал многопоточный вывод своим прогресс-баром
    if (sync.push({local}, remote, false, CompressionType::Any, true)) { 
        log.info("Push successful");
    } else {
        log.error("Push failed");
    }
}

void run_pull(std::shared_ptr<AdbDevice> device, const std::vector<std::string>& args, Logger& log) {
    if (args.size() != 2) {
        log.error("pull requires <remote_file> <local_dir> arguments");
        return;
    }
    
    std::string remote = args[0];
    std::string local_dir = args[1];

    // 1. Проверяем, что второй аргумент является существующей директорией
    struct stat st;
    if (stat(local_dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        log.error("Second argument must be an existing directory: ", local_dir);
        return;
    }

    // 2. Извлекаем IP-часть из serial (например, "192.168.177.248:5555" -> "192.168.177.248")
    std::string serial = device->getSerial();
    std::string ip_part = serial;
    size_t colon_pos = serial.find(':');
    if (colon_pos != std::string::npos) {
        ip_part = serial.substr(0, colon_pos);
    }

    // 3. Формируем компоненты нового пути
    std::string remote_filename = android::base::Basename(remote);
    std::string target_subdir = local_dir + "/" + ip_part;
    std::string target_filepath = target_subdir + "/" + remote_filename;

    // 4. Создаем поддиректорию <local_dir>/<ip_part>, если её нет (аналог mkdir -p)
    // Используем нативную функцию ADB mkdirs вместо CreateDirectories
    if (!mkdirs(target_subdir)) {
        log.error("Failed to create target directory: ", target_subdir);
        return;
    }

    // 5. Выполняем pull, передавая сформированный полный путь к файлу
    log.info("Pulling ", remote, " to ", target_filepath);
    AdbFileSync sync(device);
    
    // Передаем target_filepath как dst. do_sync_pull корректно обработает это 
    // как путь к конкретному файлу, так как srcs.size() == 1 и путь не заканчивается на '/'
    if (sync.pull({remote}, target_filepath, false, CompressionType::None, true)) {
        log.info("Pull successful");
    } else {
        log.error("Pull failed");
    }
}

void run_install(std::shared_ptr<AdbDevice> device, const std::vector<std::string>& args, Logger& log) {
    std::vector<std::string> flags;
    std::vector<std::string> paths;
    
    for (const auto& arg : args) {
        if (arg.starts_with("-") || arg.starts_with("--")) {
            flags.push_back(arg);
        } else {
            paths.push_back(arg);
        }
    }

    if (paths.empty()) {
        log.error("install requires at least one .apk or .apks path");
        return;
    }

    log.info("Installing ", paths.size(), " package(s) with flags: ", android::base::Join(flags, " "));
    AdbInstaller installer(device);
    if (installer.install(paths, flags)) {
        log.info("Installation successful");
    } else {
        log.error("Installation failed");
    }
}

void run_uninstall(std::shared_ptr<AdbDevice> device, const std::vector<std::string>& args, Logger& log) {
    if (args.empty()) {
        log.error("uninstall requires a package name");
        return;
    }
    log.info("Uninstalling ", android::base::Join(args, " "));
    AdbInstaller installer(device);
    if (installer.uninstall(args)) {
        log.info("Uninstall successful");
    } else {
        log.error("Uninstall failed");
    }
}

// ============================================================================
// Main
// ============================================================================
int main(int argc, char* argv[]) {
    __adb_argv = const_cast<const char**>(argv);
    __adb_envp = const_cast<const char**>(environ); // Убедись, что environ доступен
    adb_trace_init(argv);

    std::string devices_file = "devices.txt";
    
    static struct option long_options[] = {
        {"file", required_argument, 0, 'f'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };
    
    int opt;
    while ((opt = getopt_long(argc, argv, "f:h", long_options, nullptr)) != -1) {
        switch (opt) {
            case 'f': 
                devices_file = optarg; // Если ключ -f передан, перезаписываем дефолтное значение
                break;
            case 'h': 
                print_help(); 
                return 0;
            default: 
                print_help(); 
                return 1;
        }
    }

    if (devices_file.empty()) {
        std::cerr << "Error: devices file (-f) is required\n";
        print_help();
        return 1;
    }
    
    if (optind >= argc) {
        std::cerr << "Error: command is required (shell, push, pull)\n";
        print_help();
        return 1;
    }
    
    std::string command = argv[optind];
    std::vector<std::string> cmd_args(argv + optind + 1, argv + argc);
    
    auto devices = read_devices_file(devices_file);
    if (devices.empty()) {
        std::cerr << "Error: no devices found in " << devices_file << "\n";
        return 1;
    }
    
    auto& manager = AdbManager::instance();
    manager.start();
    
    std::vector<std::thread> threads;
    for (const auto& addr : devices) {
        threads.emplace_back([&, addr]() {
            Logger log(addr);
            log.info("Connecting...");
            
            DeviceListener listener(log);
            auto device = manager.connectDevice(addr, &listener);
            if (!device) {
                log.error("Failed to initiate connection");
                return;
            }
            
            if (!listener.wait_for_device()) {
                log.error("Connection timeout or failed");
                return;
            }
            
            log.info("Connected successfully");
            
            if (command == "shell") {
                run_shell(device, cmd_args, log);
            } else if (command == "push") {
                run_push(device, cmd_args, log);
            } else if (command == "pull") {
                run_pull(device, cmd_args, log);
            } else if (command == "install") {
                run_install(device, cmd_args, log);
            } else if (command == "uninstall") {
                run_uninstall(device, cmd_args, log);
            } else {
                log.error("Unknown command: ", command);
            }
            
            manager.disconnectDevice(addr);
            log.info("Disconnected");
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    manager.stop();
    return 0;
}
