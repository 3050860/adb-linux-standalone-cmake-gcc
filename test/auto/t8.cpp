// Проверка: реально ли uninstall удаляет пакет (send_shell_command — заглушка?).
#include <cstdio>
#include "libadb/libadb.h"

int main() {
    auto& client = libadb::Client::instance();
    libadb::Options options;
    options.log.level = libadb::LogLevel::Warn;  // тише

    client.initialize(options);
    auto device = client.connect("192.168.177.248");
    if (!device) {
        printf("connect failed\n");
        return 1;
    }
    printf("packages before:\n%s", device->shell("pm list packages -3").output.c_str());

    auto r = device->uninstall("com.example.myapplication");
    printf("uninstall: status=%s ms=%ld output='%s'\n", libadb::to_string(r.status),
           (long)r.duration.count(), r.output.c_str());

    printf("packages after:\n%s", device->shell("pm list packages -3").output.c_str());
    client.shutdown();
    return 0;
}
