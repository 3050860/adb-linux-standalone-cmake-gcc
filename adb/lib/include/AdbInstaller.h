#pragma once
#include <string>
#include <vector>
#include <memory>
#include "AdbDevice.h"

class AdbInstaller {
public:
    explicit AdbInstaller(std::shared_ptr<AdbDevice> device) : device_(device) {}

    // Устанавливает APK или APKS (с автораспаковкой)
    bool install(const std::vector<std::string>& paths, const std::vector<std::string>& flags);
    
    // Удаляет пакет
    bool uninstall(const std::vector<std::string>& args);

private:
    std::shared_ptr<AdbDevice> device_;
    
    // Если передан .apks, распаковывает его и возвращает список внутренних .apk
    std::vector<std::string> expandApks(const std::vector<std::string>& inputs);
};
