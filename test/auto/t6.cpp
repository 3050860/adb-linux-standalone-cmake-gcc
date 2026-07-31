// Проверка путей ошибок install/uninstall и потокового вывода shell.
#include <cstdio>
#include <string>

#include "libadb/libadb.h"

int main() {
    auto& client = libadb::Client::instance();
    libadb::Options options;
    options.connect_timeout = libadb::ms{8000};
    client.initialize(options);

    auto device = client.connect("192.168.177.248");
    if (!device) {
        printf("connect failed\n");
        return 1;
    }

    // 1) install: локального файла нет
    auto r = device->install("/tmp/definitely-missing.apk");
    printf("install(missing): status=%s error='%s'\n", libadb::to_string(r.status),
           r.error.c_str());

    // 2) install: файл есть, но это не apk — ждём отказа от pm
    FILE* f = fopen("/tmp/not-an.apk", "wb");
    fputs("this is not an apk", f);
    fclose(f);
    r = device->install("/tmp/not-an.apk");
    printf("install(bad apk): status=%s remote_code='%s' error='%s'\n",
           libadb::to_string(r.status), r.remote_code.c_str(), r.error.c_str());

    // 3) uninstall несуществующего пакета
    r = device->uninstall("com.libadb.definitely.missing");
    printf("uninstall(missing): status=%s exit=%d error='%s'\n", libadb::to_string(r.status),
           r.exit_code, r.error.c_str());

    // 4) потоковый вывод shell без буфера
    libadb::ShellOptions shell_options;
    shell_options.capture_output = false;
    int chunks = 0;
    shell_options.on_output = [&chunks](const std::string& serial, std::string_view chunk,
                                        bool is_stderr) {
        ++chunks;
        printf("  chunk from %s (stderr=%d): %.*s", serial.c_str(), (int)is_stderr,
               (int)chunk.size(), chunk.data());
    };
    r = device->shell("echo line1; echo line2", shell_options);
    printf("stream shell: status=%s chunks=%d captured_len=%zu\n", libadb::to_string(r.status),
           chunks, r.output.size());

    client.shutdown();
    return 0;
}
