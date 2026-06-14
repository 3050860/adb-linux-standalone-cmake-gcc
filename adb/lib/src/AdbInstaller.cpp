#include "AdbInstaller.h"
#include "adb_install.h"       // Оригинальные функции Google
#include "adb_client.h"        // Для adb_set_transport / adb_get_transport
#include "adb_utils.h"         // Для mkdirs
#include <android-base/strings.h>
#include <android-base/file.h>
#include <sys/stat.h>
#include <dirent.h>
#include <chrono>
#include <iostream>

extern "C" void adb_set_current_device(class AdbDevice* device);
std::vector<std::string> AdbInstaller::expandApks(const std::vector<std::string>& inputs) {
    std::vector<std::string> result;
    for (const auto& path : inputs) {
        if (android::base::EndsWithIgnoreCase(path, ".apks")) {
            // ADB не умеет ставить .apks напрямую. Распакуем во временную папку через системный unzip
            std::string temp_dir = "/tmp/adirect_apks_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
            mkdirs(temp_dir);
            
            std::string cmd = "unzip -q '" + path + "' -d '" + temp_dir + "'";
            if (system(cmd.c_str()) != 0) {
                std::cerr << "Failed to extract .apks file: " << path << "\n";
                return {};
            }
            
            // Собираем все .apk из распакованной папки
            DIR* dir = opendir(temp_dir.c_str());
            if (dir) {
                struct dirent* entry;
                while ((entry = readdir(dir)) != nullptr) {
                    std::string name = entry->d_name;
                    if (android::base::EndsWithIgnoreCase(name, ".apk")) {
                        result.push_back(temp_dir + "/" + name);
                    }
                }
                closedir(dir);
            }
        } else {
            result.push_back(path);
        }
    }
    return result;
}

bool AdbInstaller::install(const std::vector<std::string>& paths, const std::vector<std::string>& flags) {
    auto apk_paths = expandApks(paths);
    if (apk_paths.empty()) return false;

    // Формируем argv для оригинальной функции Google
    std::vector<const char*> argv;
    argv.push_back("install-multiple"); // Имя команды (Google-код ожидает его в argv[0])
    for (const auto& flag : flags) {
        argv.push_back(flag.c_str());
    }
    for (const auto& apk : apk_paths) {
        argv.push_back(apk.c_str());
    }

    // // 1. СОХРАНЯЕМ текущее глобальное состояние транспорта
    // TransportType old_type;
    // const char* old_serial;
    // TransportId old_id;
    // adb_get_transport(&old_type, &old_serial, &old_id);

    // // 2. ПЕРЕКЛЮЧАЕМ транспорт на наше конкретное устройство
    // adb_set_transport(kTransportLocal, device_->getSerial().c_str(), 0);

    adb_set_current_device(device_.get());

    // 3. ВЫЗЫВАЕМ ОРИГИНАЛЬНУЮ ФУНКЦИЮ GOOGLE
    // Она САМА проверит фичи (best_install_mode), сделает fallback на legacy (push),
    // обработает split-apk и все переданные флаги (-r, -t, -d, -g и т.д.)
    int result = install_multiple_app(argv.size(), argv.data());

    // 3. ВЫКЛЮЧАЕМ ПЕРЕХВАТ И ОЧИЩАЕМ СЕССИИ
    adb_set_current_device(nullptr);
    device_->clearActiveSessions();

    // // 4. ВОССТАНАВЛИВАЕМ глобальное состояние транспорта (чтобы не сломать другие потоки)
    // adb_set_transport(old_type, old_serial, old_id);

    // 5. Очистка временных папок от распакованных .apks
    for (const auto& path : apk_paths) {
        size_t pos = path.find("/tmp/adirect_apks_");
        if (pos != std::string::npos) {
            std::string temp_dir = path.substr(0, path.find_last_of('/'));
            std::string rm_cmd = "rm -rf '" + temp_dir + "'";
            system(rm_cmd.c_str());
            break; // Достаточно очистить один раз, все файлы были в одной папке
        }
    }

    return result == 0;
}

bool AdbInstaller::uninstall(const std::vector<std::string>& args) {
    if (args.empty()) return false;

    std::vector<const char*> argv;
    argv.push_back("uninstall");
    for (const auto& arg : args) {
        argv.push_back(arg.c_str());
    }

    adb_set_current_device(device_.get());
    int result = uninstall_app(argv.size(), argv.data());
    adb_set_current_device(nullptr);
    device_->clearActiveSessions();

    return result == 0;
}
