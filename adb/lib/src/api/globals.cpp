// libadb: определения глобалов и разовой настройки процесса, которые в
// консольных клиентах делает main().
#include <csignal>

#include "api/internal.h"
//
// adb_client.cpp использует __adb_argv/__adb_envp для перезапуска себя как
// adb-сервера (execve). В библиотеке этот путь не используется: сервер не
// поднимается, работа идёт напрямую с устройствами. Значения остаются nullptr,
// но символы должны существовать, иначе .so не слинкуется.
namespace {
const char* kNoArgs[] = {"libadb", nullptr};
}  // namespace

const char** __adb_argv = kNoArgs;
const char** __adb_envp = nullptr;

// Сильное определение слабого символа из adb_trace.cpp: в библиотеке внутренний
// файловый лог выключен по умолчанию, поэтому /tmp/adb.log не открывается и не
// создаётся, пока приложение само не позовёт libadb::set_log_options().
// Решение через weak-символ, а не через статический инициализатор: не зависит от
// порядка инициализации глобалов, то есть работает даже для самой первой записи.
bool adb_log_default_enabled() {
    return false;
}

namespace libadb::internal {

// Консольный adb делает signal(SIGPIPE, SIG_IGN) в main(): запись в оборванный
// сокет должна давать EPIPE, а не убивать процесс. Библиотеке это нужно не
// меньше (например, соединение закрыли через close_all() во время передачи),
// но чужой обработчик перебивать нельзя — поэтому ставим SIG_IGN только если
// приложение ничего не настраивало.
void ensure_sigpipe_ignored() {
    struct sigaction current{};
    if (sigaction(SIGPIPE, nullptr, &current) != 0) return;
    if (current.sa_handler != SIG_DFL) return;  // приложение уже решило само

    struct sigaction ignore{};
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    sigaction(SIGPIPE, &ignore, nullptr);
}

}  // namespace libadb::internal

