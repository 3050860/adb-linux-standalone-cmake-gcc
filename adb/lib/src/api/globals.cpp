// libadb: определения глобалов, которые в консольных клиентах задаёт main().
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
