// examples/cpp/install.cpp — установить APK с прогрессом и обработкой конфликта.
// Запуск: ./ex_install 192.168.1.10 /tmp/app.apk
#include <cstdio>
#include "libadb/libadb.h"

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <host:port> <apk>\n", argv[0]);
        return 1;
    }

    auto& client = libadb::Client::instance();
    client.initialize();

    libadb::Status status;
    auto device = client.connect(argv[1], &status);
    if (!device) {
        fprintf(stderr, "connect: %s\n", libadb::to_string(status));
        return 1;
    }

    libadb::InstallOptions opts;
    opts.reinstall          = true;
    opts.grant_permissions  = true;
    // При конфликте подписи — переустановить (данные теряются).
    opts.on_conflict        = libadb::ConflictPolicy::Reinstall;
    opts.package_name_source = libadb::PackageNameSource::Auto;
    opts.on_progress = [](const std::string&, uint64_t done, uint64_t total) {
        if (total > 0)
            printf("\r  transfer %3u%%",
                   static_cast<unsigned>(done * 100 / total));
    };

    auto result = device->install(argv[2], opts);
    printf("\n");
    if (!result.ok()) {
        fprintf(stderr, "install failed [%s]: %s\n",
                result.remote_code.c_str(), result.error.c_str());
        return 1;
    }
    printf("installed %.1f MB in %.1f s\n",
           result.transfer.bytes / 1048576.0,
           result.transfer.duration.count() / 1000.0);
    if (result.retries)
        printf("(required %d retry)\n", result.retries);
    return 0;
}
