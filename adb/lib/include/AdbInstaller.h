#pragma once
#include <string>
#include <vector>
#include <memory>
#include "AdbDevice.h"

class AdbInstaller {
public:
    explicit AdbInstaller(std::shared_ptr<AdbDevice> device) : device_(device) {}

    // Устанавливает APK или APKS (с автораспаковкой).
    // multi_package = true — независимые пакеты одной атомарной сессией
    // (pm install-create --multi-package), иначе части одного пакета.
    bool install(const std::vector<std::string>& paths, const std::vector<std::string>& flags,
                 bool multi_package = false);

    // Удаляет пакет
    bool uninstall(const std::vector<std::string>& args);

    // Если передан .apks, распаковывает его во временный каталог и возвращает
    // список внутренних .apk. Публичный: фасаду libadb нужно знать состав
    // бандла до установки (фаза Prepare со своим таймаутом, §6.2).
    // Пустой результат — распаковать не удалось.
    static std::vector<std::string> expandApks(const std::vector<std::string>& inputs);

    // Удаляет временные каталоги, созданные expandApks().
    static void cleanupExpanded(const std::vector<std::string>& paths);

private:
    std::shared_ptr<AdbDevice> device_;
};
