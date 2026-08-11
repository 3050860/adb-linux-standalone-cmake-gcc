// examples/cpp/events.cpp — подписка на события: прогресс, таймауты, ошибки.
// Запуск: ./ex_events 192.168.1.10 /tmp/big.bin /data/local/tmp/big.bin
#include <cstdio>
#include <cstring>
#include "libadb/libadb.h"

int main(int argc, char** argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <host:port> <local> <remote>\n", argv[0]);
        return 1;
    }

    auto& client = libadb::Client::instance();

    libadb::Options opts;
    // Прогресс не чаще раза в 200 мс.
    opts.progress_interval = libadb::ms{200};
    // Stall-таймаут передачи — 15 секунд.
    opts.timeouts.push.stall = libadb::ms{15000};

    opts.on_event = [](const libadb::Event& e) {
        switch (e.type) {
            case libadb::EventType::DeviceConnected:
                printf("[event] connected to %s\n", e.serial.c_str());
                break;
            case libadb::EventType::OperationProgress:
                if (e.bytes_total > 0)
                    printf("\r[event] progress %3llu%%",
                           (unsigned long long)(e.bytes_done * 100 / e.bytes_total));
                break;
            case libadb::EventType::OperationFinished:
                printf("\n[event] finished: %.2f MiB/s\n", e.stats.mib_per_sec);
                break;
            case libadb::EventType::OperationFailed:
                printf("\n[event] FAILED: %s\n", e.message.c_str());
                break;
            case libadb::EventType::OperationTimeout:
                printf("\n[event] TIMEOUT\n");
                break;
            default:
                break;
        }
    };
    client.initialize(opts);

    libadb::Status status;
    auto device = client.connect(argv[1], &status);
    if (!device) {
        fprintf(stderr, "connect: %s\n", libadb::to_string(status));
        return 1;
    }

    // on_start: получаем OperationId до начала, чтобы можно было отменить.
    libadb::OperationId op_id = 0;
    libadb::PushOptions popts;
    popts.on_start = [&op_id](libadb::OperationId id, const std::string&) {
        op_id = id;
        printf("[info] operation id=%llu\n", (unsigned long long)id);
    };

    auto result = device->push(argv[2], argv[3], popts);
    if (!result.ok()) {
        fprintf(stderr, "push failed: %s\n", result.error.c_str());
        return 1;
    }
    printf("done: %llu bytes, %.1f MiB/s\n",
           (unsigned long long)result.transfer.bytes,
           result.transfer.mib_per_sec);
    return 0;
}
