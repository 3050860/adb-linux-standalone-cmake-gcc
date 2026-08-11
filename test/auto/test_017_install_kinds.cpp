// test_017: установка — split/.apks/multi-package, разбор ошибок pm,
// ConflictPolicy (§10 спецификации, этап 9).
//
// Проверяем: InstallKind::Auto определяет вид по составу файлов; .apks
// распаковывается и ставится; ошибки pm раскладываются в Status + remote_code;
// ReinstallKeepData возвращает NotImplemented; ConflictPolicy::Reinstall без
// имени пакета не переустанавливает «что-нибудь»; user_id доходит до pm;
// install(vector) и install_async(vector) работают.
//
// Сборка:
//   g++ -std=c++20 test/auto/test_017_install_kinds.cpp -Iinclude -Ibuild/include
//       -Lbuild -ladb -Wl,-rpath,$PWD/build -pthread -o /tmp/test_017
//
// Запуск: /tmp/test_017 <ip> [apk] [apks]
//
// Бандл .apks для проверки готовится так (внутри должен быть base.apk):
//   mkdir -p /tmp/libadb-bundle && cp test/main.apk /tmp/libadb-bundle/base.apk
//   (cd /tmp/libadb-bundle && zip -q -0 /tmp/libadb-test-017.apks base.apk)
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "libadb/libadb.h"

namespace {

int failures = 0;

void check(bool condition, const std::string& name, const std::string& details = {}) {
    printf("%s %s%s%s\n", condition ? "[ OK ]" : "[FAIL]", name.c_str(),
           details.empty() ? "" : " -> ", details.c_str());
    if (!condition) ++failures;
}

bool exists(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

// Кладёт мусор в файл с нужным расширением: pm обязан отвергнуть его как
// INSTALL_PARSE_FAILED_*, а мы проверяем раскладку кода в Status.
bool make_broken_apk(const std::string& path) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    const char* junk = "this is not an apk at all";
    fwrite(junk, 1, strlen(junk), f);
    fclose(f);
    return true;
}

struct EventLog {
    mutable std::mutex mutex;
    std::vector<libadb::Event> events;

    void add(const libadb::Event& e) {
        std::lock_guard<std::mutex> lock(mutex);
        events.push_back(e);
    }

    size_t count(libadb::EventType type) const {
        std::lock_guard<std::mutex> lock(mutex);
        size_t n = 0;
        for (const auto& e : events) {
            if (e.type == type) ++n;
        }
        return n;
    }

    bool has_phase(libadb::Phase phase) const {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto& e : events) {
            if (e.type == libadb::EventType::OperationPhaseChanged && e.phase == phase) return true;
        }
        return false;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex);
        events.clear();
    }
};

}  // namespace

int main(int argc, char** argv) {
    const std::string address = argc > 1 ? argv[1] : "192.168.177.249";
    const std::string apk = argc > 2 ? argv[2] : "test/main.apk";
    const std::string apks = argc > 3 ? argv[3] : "/tmp/libadb-test-017.apks";

    EventLog events;

    auto& client = libadb::Client::instance();
    libadb::Options options;
    options.progress_interval = libadb::ms{0};
    options.on_event = [&events](const libadb::Event& e) { events.add(e); };
    client.initialize(options);

    libadb::Status status = libadb::Status::Ok;
    auto device = client.connect(address, &status);
    check(device != nullptr, std::string("connect ") + address, libadb::to_string(status));
    if (!device) {
        printf("\nCannot continue without a live device.\n");
        return 1;
    }

    // --- 1. Валидация аргументов без обращения к устройству -------------------
    {
        libadb::Result empty = device->install(std::vector<std::string>{});
        check(empty.status == libadb::Status::InvalidArgument, "пустой список → InvalidArgument",
              libadb::to_string(empty.status));
        check(empty.phase == libadb::Phase::Prepare, "фаза Prepare", libadb::to_string(empty.phase));
    }
    {
        libadb::Result missing = device->install("/tmp/libadb-no-such-file-017.apk");
        check(missing.status == libadb::Status::LocalFileError,
              "отсутствующий файл → LocalFileError", libadb::to_string(missing.status));
    }
    {
        // Single с двумя файлами — противоречие в запросе.
        libadb::InstallOptions single;
        single.kind = libadb::InstallKind::Single;
        libadb::Result bad = device->install(std::vector<std::string>{apk, apk}, single);
        check(bad.status == libadb::Status::InvalidArgument,
              "Single с двумя файлами → InvalidArgument", libadb::to_string(bad.status));
    }

    // --- 2. ReinstallKeepData зарезервирован ---------------------------------
    {
        libadb::InstallOptions reserved;
        reserved.on_conflict = libadb::ConflictPolicy::ReinstallKeepData;
        libadb::Result result = device->install(apk, reserved);
        check(result.status == libadb::Status::NotImplemented,
              "ReinstallKeepData → NotImplemented", libadb::to_string(result.status));
        // Устройство при этом трогать не должны были.
        check(result.transfer.bytes == 0, "ничего не передавалось",
              std::to_string(result.transfer.bytes));
    }

    // --- 3. to_string для новых перечислений --------------------------------
    check(std::string(libadb::to_string(libadb::InstallKind::Bundle)) == "bundle",
          "to_string(InstallKind::Bundle)", libadb::to_string(libadb::InstallKind::Bundle));
    check(std::string(libadb::to_string(libadb::InstallKind::MultiPackage)) == "multi-package",
          "to_string(InstallKind::MultiPackage)");
    check(std::string(libadb::to_string(libadb::ConflictPolicy::Reinstall)) == "reinstall",
          "to_string(ConflictPolicy::Reinstall)");
    check(std::string(libadb::to_string(libadb::PackageNameSource::Both)) == "both",
          "to_string(PackageNameSource::Both)");

    // --- 4. Разбор ошибок pm: битый apk → InvalidApk ------------------------
    {
        const std::string broken = "/tmp/libadb-test-017-broken.apk";
        check(make_broken_apk(broken), "подготовлен битый apk");
        events.clear();
        libadb::Result result = device->install(broken);
        check(!result.ok(), "битый apk не установился", libadb::to_string(result.status));
        printf("[INFO] broken apk -> %s remote_code='%s'\n", libadb::to_string(result.status),
               result.remote_code.c_str());
        // pm обязан отдать код семейства INSTALL_PARSE_FAILED_* / INVALID_APK.
        check(result.status == libadb::Status::InvalidApk,
              "код pm разложен в Status::InvalidApk", libadb::to_string(result.status));
        check(!result.remote_code.empty(), "remote_code сохранён", result.remote_code);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        check(events.count(libadb::EventType::OperationFailed) == 1,
              "пришло событие OperationFailed");
    }

    // --- 5. ConflictPolicy::Reinstall без имени пакета -----------------------
    // Explicit + пустой package_name: библиотека не должна удалять «что-нибудь».
    {
        libadb::InstallOptions conflict;
        conflict.on_conflict = libadb::ConflictPolicy::Reinstall;  // package_name пуст
        libadb::Result result = device->install("/tmp/libadb-test-017-broken.apk", conflict);
        // Конфликта подписи тут нет (apk битый), поэтому просто убеждаемся, что
        // политика ничего не сломала и статус остался «битый apk».
        check(result.status == libadb::Status::InvalidApk,
              "Reinstall без конфликта не меняет статус", libadb::to_string(result.status));
    }

    // --- 6. Одиночная установка: Auto -> Single ------------------------------
    if (!exists(apk)) {
        printf("[SKIP] %s не найден — установочные проверки пропущены\n", apk.c_str());
    } else {
        events.clear();
        libadb::InstallOptions single;
        single.user_id = 0;  // --user 0: должно дойти до pm и не сломать установку
        libadb::Result result = device->install(apk, single);
        check(result.ok(), "install одного apk (Auto -> Single, --user 0)",
              std::string(libadb::to_string(result.status)) + " " + result.error);
        if (result.ok()) {
            check(result.transfer.bytes > 0, "передан apk", std::to_string(result.transfer.bytes));
            check(result.output.find("Success") != std::string::npos,
                  "статус pm попал в output");
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            check(events.has_phase(libadb::Phase::Prepare), "была фаза Prepare");
            check(events.has_phase(libadb::Phase::Transfer), "была фаза Transfer");
            check(events.count(libadb::EventType::OperationFinished) == 1,
                  "пришло OperationFinished");
        }
    }

    // --- 7. Bundle: .apks распаковывается и ставится ------------------------
    if (!exists(apks)) {
        printf("[SKIP] %s не найден — проверка .apks пропущена\n", apks.c_str());
    } else {
        events.clear();
        libadb::Result result = device->install(apks);
        check(result.ok(), "install .apks (Auto -> Bundle)",
              std::string(libadb::to_string(result.status)) + " " + result.error);
        if (result.ok()) {
            check(result.transfer.bytes > 0, "содержимое бандла передано",
                  std::to_string(result.transfer.bytes));
        }
        // Временные каталоги распаковки не должны оставаться в /tmp.
        const int leftovers = system("ls -d /tmp/libadb_apks_* >/dev/null 2>&1");
        check(leftovers != 0, "временные каталоги распаковки удалены");
    }

    // --- 8. SplitSet: тот же apk дважды как «части одного пакета» -----------
    // Настоящих split-частей под рукой нет, поэтому проверяем, что путь
    // SplitSet доходит до pm и его отказ раскладывается в понятный код, а не
    // роняет библиотеку.
    if (exists(apk)) {
        events.clear();
        libadb::InstallOptions split;
        split.kind = libadb::InstallKind::SplitSet;
        libadb::Result result = device->install(std::vector<std::string>{apk, apk}, split);
        printf("[INFO] SplitSet(apk,apk) -> %s remote_code='%s' error='%s'\n",
               libadb::to_string(result.status), result.remote_code.c_str(), result.error.c_str());
        check(result.ok() || !result.remote_code.empty() || !result.error.empty(),
              "SplitSet отдал внятный результат", libadb::to_string(result.status));
        check(device->is_online(), "устройство живо после SplitSet");
    }

    // --- 9. MultiPackage ----------------------------------------------------
    if (exists(apk)) {
        events.clear();
        libadb::InstallOptions multi;
        multi.kind = libadb::InstallKind::MultiPackage;
        libadb::Result result = device->install(std::vector<std::string>{apk}, multi);
        printf("[INFO] MultiPackage(apk) -> %s remote_code='%s' error='%s'\n",
               libadb::to_string(result.status), result.remote_code.c_str(), result.error.c_str());
        check(result.ok() || !result.error.empty(), "MultiPackage отдал внятный результат",
              libadb::to_string(result.status));
        check(device->is_online(), "устройство живо после MultiPackage");
    }

    // --- 10. ConflictPolicy::Reinstall с известным именем пакета -------------
    // Настоящий конфликт подписи воспроизвести нечем (нужен apk, подписанный
    // другим ключом), поэтому проверяем то, что можно проверить достоверно:
    // политика с явным именем пакета не мешает обычной установке и оставляет
    // пакет на устройстве.
    if (exists(apk)) {
        libadb::InstallOptions conflict;
        conflict.on_conflict = libadb::ConflictPolicy::Reinstall;
        conflict.package_name = "cs.netarium";  // из test/main.apk
        conflict.package_name_source = libadb::PackageNameSource::Both;
        libadb::Result result = device->install(apk, conflict);
        check(result.ok(), "install с ConflictPolicy::Reinstall (без конфликта)",
              std::string(libadb::to_string(result.status)) + " " + result.error);
        check(result.retries == 0, "повторов не потребовалось",
              std::to_string(result.retries));

        libadb::ShellOptions shell_options;
        shell_options.capture_output = true;
        libadb::Result listed = device->shell("pm list packages " + conflict.package_name,
                                              shell_options);
        check(listed.output.find(conflict.package_name) != std::string::npos,
              "пакет на месте после установки", listed.output);
    }

    // --- 11. install_async(vector) ------------------------------------------
    if (exists(apk)) {
        auto op = device->install_async(std::vector<std::string>{apk}, libadb::InstallOptions{});
        check(op != nullptr, "install_async(vector) вернул операцию");
        check(op->wait(libadb::ms{600000}), "install_async дождался");
        check(op->command() == libadb::Command::Install, "команда операции Install");
        check(op->result().ok(), "install_async успешен",
              std::string(libadb::to_string(op->result().status)) + " " + op->result().error);
    }

    device->close();
    client.shutdown();

    printf("\n%s (%d failure(s))\n", failures == 0 ? "ALL PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
