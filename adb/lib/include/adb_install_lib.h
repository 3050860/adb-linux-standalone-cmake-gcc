#pragma once

#include <string>

// Перехват статусных строк pm ("Success", "Failure [...]").
// Пока приёмник установлен (в текущем потоке), эти строки складываются в него
// вместо stdout/stderr процесса: библиотека не должна писать в консоль
// приложения. nullptr возвращает прежнее поведение (нужно adirect).
void adb_install_set_status_sink(std::string* sink);

int install_app(int argc, const char** argv);

int install_multiple_app(int argc, const char** argv);
int install_multi_package(int argc, const char** argv);
int uninstall_app(int argc, const char** argv);

int delete_device_file(const std::string& filename);
int delete_host_file(const std::string& filename);

