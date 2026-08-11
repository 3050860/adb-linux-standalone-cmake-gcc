#include "AdbInstaller.h"
#include "adb_install.h"       // Оригинальные функции Google
#include "adb_client.h"        // Для adb_set_transport / adb_get_transport
#include "adb_utils.h"         // Для mkdirs
#include <android-base/strings.h>
#include <android-base/file.h>
#include <sys/stat.h>
#include <dirent.h>
#include <algorithm>
#include <chrono>
#include <iostream>

extern "C" void adb_set_current_device(class AdbDevice* device);

// Префикс временных каталогов распаковки .apks. По нему же ищем, что чистить.
static const char* const kApksTempPrefix = "/tmp/libadb_apks_";

std::vector<std::string> AdbInstaller::expandApks(const std::vector<std::string>& inputs) {
    std::vector<std::string> result;
    for (const auto& path : inputs) {
        // .apks (bundletool) и обычный .zip с набором apk внутри: adb не умеет
        // ставить их напрямую, поэтому распаковываем.
        const bool is_bundle = android::base::EndsWithIgnoreCase(path, ".apks") ||
                               android::base::EndsWithIgnoreCase(path, ".zip");
        if (!is_bundle) {
            result.push_back(path);
            continue;
        }

        std::string temp_dir = kApksTempPrefix +
                               std::to_string(std::chrono::system_clock::now()
                                                      .time_since_epoch()
                                                      .count());
        mkdirs(temp_dir);

        // Вывод unzip гасим: библиотека не должна писать в консоль приложения.
        std::string cmd = "unzip -q -o '" + path + "' -d '" + temp_dir + "' >/dev/null 2>&1";
        if (system(cmd.c_str()) != 0) {
            // Каталог мог остаться полупустым — убираем за собой.
            system(("rm -rf '" + temp_dir + "'").c_str());
            return {};
        }

        // Собираем все .apk из распакованной папки. base.apk должен идти первым:
        // pm ожидает базовый пакет раньше своих split-частей.
        std::vector<std::string> found;
        std::string base;
        DIR* dir = opendir(temp_dir.c_str());
        if (dir) {
            struct dirent* entry;
            while ((entry = readdir(dir)) != nullptr) {
                std::string name = entry->d_name;
                if (!android::base::EndsWithIgnoreCase(name, ".apk")) continue;
                if (name == "base.apk" || android::base::StartsWithIgnoreCase(name, "base-master")) {
                    base = temp_dir + "/" + name;
                } else {
                    found.push_back(temp_dir + "/" + name);
                }
            }
            closedir(dir);
        }
        std::sort(found.begin(), found.end());
        if (!base.empty()) result.push_back(base);
        result.insert(result.end(), found.begin(), found.end());

        if (base.empty() && found.empty()) {
            system(("rm -rf '" + temp_dir + "'").c_str());
            return {};
        }
    }
    return result;
}

void AdbInstaller::cleanupExpanded(const std::vector<std::string>& paths) {
    // Все файлы одного бандла лежат в одном каталоге, но бандлов может быть
    // несколько — собираем уникальные каталоги.
    std::vector<std::string> dirs;
    for (const auto& path : paths) {
        if (path.find(kApksTempPrefix) != 0) continue;
        const size_t slash = path.find_last_of('/');
        if (slash == std::string::npos) continue;
        std::string dir = path.substr(0, slash);
        if (std::find(dirs.begin(), dirs.end(), dir) == dirs.end()) dirs.push_back(dir);
    }
    for (const auto& dir : dirs) {
        system(("rm -rf '" + dir + "'").c_str());
    }
}

bool AdbInstaller::install(const std::vector<std::string>& paths,
                           const std::vector<std::string>& flags, bool multi_package) {
    auto apk_paths = expandApks(paths);
    if (apk_paths.empty()) return false;

    // Формируем argv для оригинальной функции Google. argv[0] — имя команды:
    // Google-код ожидает его на месте и пропускает при разборе.
    std::vector<const char*> argv;
    argv.push_back(multi_package ? "install-multi-package" : "install-multiple");
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
    // Для независимых пакетов — своя функция: она создаёт родительскую сессию
    // `install-create --multi-package` и вкладывает в неё по сессии на пакет.
    int result = multi_package ? install_multi_package(argv.size(), argv.data())
                               : install_multiple_app(argv.size(), argv.data());

    // 3. ВЫКЛЮЧАЕМ ПЕРЕХВАТ И ОЧИЩАЕМ СЕССИИ
    adb_set_current_device(nullptr);
    device_->clearActiveSessions();

    // // 4. ВОССТАНАВЛИВАЕМ глобальное состояние транспорта (чтобы не сломать другие потоки)
    // adb_set_transport(old_type, old_serial, old_id);

    // 5. Очистка временных папок от распакованных .apks
    cleanupExpanded(apk_paths);

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
