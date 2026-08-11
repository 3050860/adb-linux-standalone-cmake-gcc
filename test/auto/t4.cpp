// Проверка libadb: Client/Device, одиночные и групповые операции.
#include <cstdio>
#include <string>
#include <vector>

#include "libadb/libadb.h"

int main(int argc, char** argv) {
    std::vector<std::string> addrs;
    for (int i = 1; i < argc; ++i) addrs.push_back(argv[i]);
    if (addrs.empty()) addrs = {"192.168.177.248", "192.168.177.249"};

    libadb::Options options;
    options.timeouts.connect = libadb::ms{8000};
    options.log_sink = [](libadb::LogLevel level, const std::string& serial,
                          std::string_view msg) {
        printf("  [%s] %s%s%.*s\n", libadb::to_string(level), serial.c_str(),
               serial.empty() ? "" : ": ", (int)msg.size(), msg.data());
    };
    auto& client = libadb::Client::instance();
    printf("initialize: %s, initialized=%d\n", libadb::to_string(client.initialize(options)),
           (int)client.initialized());

    // 1) плохой адрес
    libadb::Status status = libadb::Status::Ok;
    auto bad = client.connect("not:a:port", &status);
    printf("bad address -> device=%p status=%s\n", (void*)bad.get(), libadb::to_string(status));

    // 2) одиночное устройство
    auto device = client.connect(addrs[0], &status);
    printf("connect %s -> status=%s\n", addrs[0].c_str(), libadb::to_string(status));
    if (device) {
        printf("  serial=%s online=%d\n", device->serial().c_str(), (int)device->is_online());

        libadb::Result r = device->shell("echo hello-from-libadb");
        printf("  shell: status=%s exit=%d ms=%ld out='%s'\n", libadb::to_string(r.status),
               r.exit_code, (long)r.duration.count(), r.output.c_str());

        r = device->shell("exit 7");
        printf("  shell exit-code: status=%s exit=%d\n", libadb::to_string(r.status), r.exit_code);

        auto model = device->get_prop("ro.product.model");
        printf("  get_prop(ro.product.model)=%s\n", model ? model->c_str() : "<none>");

        // push/pull одного и того же файла
        FILE* f = fopen("/tmp/libadb-payload.bin", "wb");
        std::string data(256 * 1024, 'x');
        fwrite(data.data(), 1, data.size(), f);
        fclose(f);

        r = device->push("/tmp/libadb-payload.bin", "/data/local/tmp/libadb-payload.bin");
        printf("  push: status=%s ms=%ld bytes=%lu %.2f MiB/s err='%s'\n",
               libadb::to_string(r.status), (long)r.duration.count(),
               (unsigned long)r.transfer.bytes, r.transfer.mib_per_sec, r.error.c_str());

        r = device->pull("/data/local/tmp/libadb-payload.bin", "/tmp/libadb-pulled.bin");
        printf("  pull: status=%s ms=%ld bytes=%lu err='%s'\n", libadb::to_string(r.status),
               (long)r.duration.count(), (unsigned long)r.transfer.bytes, r.error.c_str());

        device->close();
        printf("  after close: online=%d, shell status=%s\n", (int)device->is_online(),
               libadb::to_string(device->shell("echo x").status));
    }

    // 3) групповая операция
    printf("shell_all on %zu device(s):\n", addrs.size());
    auto results = client.shell_all(addrs, "getprop ro.product.model");
    for (const auto& [address, result] : results) {
        std::string out = result.output;
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
        printf("  %s -> status=%s exit=%d ms=%ld out='%s' err='%s'\n", address.c_str(),
               libadb::to_string(result.status), result.exit_code, (long)result.duration.count(),
               out.c_str(), result.error.c_str());
    }

    client.shutdown();
    printf("shutdown done, initialized=%d\n", (int)client.initialized());
    return 0;
}
