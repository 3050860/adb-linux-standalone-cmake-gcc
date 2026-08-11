// test_018: авторизация — ключи из файлов и из текста PEM (§5, этап 10).
//
// Проверяем: ключ из файла загружается; ключ текстом PEM загружается и даёт тот
// же отпечаток, что и он же из файла; битый PEM даёт InvalidArgument из
// initialize() (а не падение); use_default_key_store=false не подмешивает
// ~/.android/adbkey; эфемерный ключ генерируется и пишется на диск; подключение
// к живому устройству работает с ключом, заданным текстом.
//
// Сборка:
//   g++ -std=c++20 test/auto/test_018_auth.cpp -Iinclude -Ibuild/include
//       -Lbuild -ladb -Wl,-rpath,$PWD/build -pthread -o /tmp/test_018
//
// Запуск: /tmp/test_018 <ip>
//
// ВАЖНО: каждый подтест меняет глобальное состояние ключей процесса, а ключи
// внутри adb загружаются в один общий набор и не выгружаются. Поэтому подтесты,
// требующие «чистого» набора, запускаются отдельными процессами — см. режим
// `--mode` ниже.
#include <chrono>
#include <cstdio>
#include <cstdlib>
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

std::string read_file(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return {};
    std::string data;
    char buffer[4096];
    size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), f)) > 0) data.append(buffer, n);
    fclose(f);
    return data;
}

bool file_exists(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

std::string join(const std::vector<std::string>& items) {
    std::string out;
    for (const auto& item : items) {
        if (!out.empty()) out += ",";
        out += item;
    }
    return out;
}

}  // namespace

// --- Режим "ephemeral": пустой набор ключей → генерация эфемерного -----------
static int mode_ephemeral() {
    const std::string saved = "/tmp/libadb-test-018-generated";
    remove(saved.c_str());
    remove((saved + ".pub").c_str());

    libadb::Options options;
    options.auth.use_default_key_store = false;  // никаких ~/.android/adbkey
    options.auth.generate_ephemeral_if_empty = true;
    options.auth.save_generated_key_to = saved;

    const libadb::Status status = libadb::Client::instance().initialize(options);
    check(status == libadb::Status::Ok, "initialize с эфемерным ключом",
          libadb::to_string(status));

    const auto fingerprints = libadb::auth_key_fingerprints();
    check(fingerprints.size() == 1, "загружен ровно один (эфемерный) ключ",
          std::to_string(fingerprints.size()) + ": " + join(fingerprints));
    check(file_exists(saved), "приватный ключ записан в save_generated_key_to");
    check(file_exists(saved + ".pub"), "публичный ключ записан рядом (.pub)");

    const std::string pem = read_file(saved);
    check(pem.find("PRIVATE KEY") != std::string::npos, "в файле действительно PEM",
          pem.substr(0, 30));

    libadb::Client::instance().shutdown();
    printf("%s\n", failures == 0 ? "MODE_EPHEMERAL_OK" : "MODE_EPHEMERAL_FAILED");
    return failures == 0 ? 0 : 1;
}

// --- Режим "badpem": битый PEM → InvalidArgument -----------------------------
static int mode_badpem() {
    libadb::Options options;
    options.auth.use_default_key_store = false;
    options.auth.generate_ephemeral_if_empty = false;
    options.auth.private_keys_pem = {"-----BEGIN RSA PRIVATE KEY-----\nnot base64 at all\n"
                                     "-----END RSA PRIVATE KEY-----\n"};

    const libadb::Status status = libadb::Client::instance().initialize(options);
    check(status == libadb::Status::InvalidArgument, "битый PEM → InvalidArgument",
          libadb::to_string(status));
    check(libadb::auth_key_fingerprints().empty(), "битый ключ не попал в набор");

    printf("%s\n", failures == 0 ? "MODE_BADPEM_OK" : "MODE_BADPEM_FAILED");
    return failures == 0 ? 0 : 1;
}

// --- Режим "nokeys": ключей нет и генерировать нельзя ------------------------
static int mode_nokeys() {
    libadb::Options options;
    options.auth.use_default_key_store = false;
    options.auth.generate_ephemeral_if_empty = false;

    const libadb::Status status = libadb::Client::instance().initialize(options);
    // Это не ошибка конфигурации: initialize проходит, просто авторизоваться
    // будет нечем — устройство ответит AuthRequired.
    check(status == libadb::Status::Ok, "initialize без ключей проходит",
          libadb::to_string(status));
    check(libadb::auth_key_fingerprints().empty(), "набор ключей пуст",
          join(libadb::auth_key_fingerprints()));

    printf("%s\n", failures == 0 ? "MODE_NOKEYS_OK" : "MODE_NOKEYS_FAILED");
    return failures == 0 ? 0 : 1;
}

// --- Режим "missingfile": несуществующий файл ключа --------------------------
static int mode_missingfile() {
    libadb::Options options;
    options.auth.use_default_key_store = false;
    options.auth.generate_ephemeral_if_empty = false;
    options.auth.key_files = {"/tmp/libadb-no-such-key-018"};

    const libadb::Status status = libadb::Client::instance().initialize(options);
    check(status == libadb::Status::InvalidArgument,
          "несуществующий key_file → InvalidArgument", libadb::to_string(status));

    printf("%s\n", failures == 0 ? "MODE_MISSINGFILE_OK" : "MODE_MISSINGFILE_FAILED");
    return failures == 0 ? 0 : 1;
}

// Прогоняет себя же в отдельном процессе: набор ключей внутри adb глобален и не
// выгружается, поэтому «чистое» состояние достигается только новым процессом.
static void run_submode(const char* self, const char* mode, const std::string& marker) {
    const std::string cmd = std::string(self) + " --mode=" + mode + " > /tmp/libadb-test-018-" +
                            mode + ".log 2>&1";
    const int rc = system(cmd.c_str());
    const std::string output = read_file(std::string("/tmp/libadb-test-018-") + mode + ".log");
    const bool ok = rc == 0 && output.find(marker) != std::string::npos;
    check(ok, std::string("подтест ") + mode, ok ? "" : output);
    if (!ok) return;
    // Пробрасываем строки подтеста в общий вывод, чтобы было видно все проверки.
    printf("%s", output.c_str());
}

int main(int argc, char** argv) {
    // Режимы для подпроцессов.
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--mode=ephemeral") return mode_ephemeral();
        if (arg == "--mode=badpem") return mode_badpem();
        if (arg == "--mode=nokeys") return mode_nokeys();
        if (arg == "--mode=missingfile") return mode_missingfile();
    }

    const std::string address = argc > 1 ? argv[1] : "192.168.177.249";

    // --- 1. Подтесты в отдельных процессах (нужен чистый набор ключей) --------
    printf("--- subtests in separate processes ---\n");
    run_submode(argv[0], "ephemeral", "MODE_EPHEMERAL_OK");
    run_submode(argv[0], "badpem", "MODE_BADPEM_OK");
    run_submode(argv[0], "nokeys", "MODE_NOKEYS_OK");
    run_submode(argv[0], "missingfile", "MODE_MISSINGFILE_OK");

    // --- 2. Ключ текстом PEM: тот же отпечаток, что и у файла ----------------
    printf("--- main process ---\n");
    // Берём ключ, сгенерированный подтестом ephemeral: он заведомо валиден.
    const std::string key_path = "/tmp/libadb-test-018-generated";
    const std::string pem = read_file(key_path);
    check(!pem.empty(), "ключ от подтеста ephemeral доступен", key_path);
    if (pem.empty()) {
        printf("\nFAILED (%d failure(s))\n", failures);
        return 1;
    }

    auto& client = libadb::Client::instance();

    libadb::Options options;
    options.auth.use_default_key_store = false;
    options.auth.generate_ephemeral_if_empty = false;
    options.auth.private_keys_pem = {pem};
    options.timeouts.connect = libadb::ms{20000};

    libadb::Status status = client.initialize(options);
    check(status == libadb::Status::Ok, "initialize с ключом из PEM",
          libadb::to_string(status));

    const auto pem_fingerprints = libadb::auth_key_fingerprints();
    check(pem_fingerprints.size() == 1, "ключ из PEM загружен ровно один",
          std::to_string(pem_fingerprints.size()));

    // --- 3. Тот же ключ, но из файла — отпечаток должен совпасть -------------
    libadb::Options with_file = options;
    with_file.auth.private_keys_pem.clear();
    with_file.auth.key_files = {key_path};
    status = client.initialize(with_file);
    check(status == libadb::Status::Ok, "initialize с тем же ключом из файла",
          libadb::to_string(status));

    const auto all_fingerprints = libadb::auth_key_fingerprints();
    check(all_fingerprints.size() == 1,
          "тот же ключ из файла не удвоил набор (дедупликация по отпечатку)",
          std::to_string(all_fingerprints.size()) + ": " + join(all_fingerprints));
    check(all_fingerprints == pem_fingerprints, "отпечаток из PEM и из файла совпал",
          join(all_fingerprints));

    // --- 4. Живое подключение с ключом, заданным текстом --------------------
    // Устройство уже авторизовало этот хост ранее своим ключом, поэтому наш
    // эфемерный ключ оно, скорее всего, не примет. Проверяем не «пустят», а то,
    // что путь авторизации работает: либо подключились, либо честно получили
    // AuthRequired/ConnectTimeout, а не упали.
    libadb::Status connect_status = libadb::Status::Ok;
    auto device = client.connect(address, &connect_status);
    printf("[INFO] connect с эфемерным ключом -> %s\n", libadb::to_string(connect_status));
    const bool sane = device != nullptr || connect_status == libadb::Status::AuthRequired ||
                      connect_status == libadb::Status::ConnectTimeout ||
                      connect_status == libadb::Status::ConnectFailed;
    check(sane, "connect отработал предсказуемо", libadb::to_string(connect_status));
    if (device) {
        libadb::Result result = device->shell("echo auth-ok", libadb::ShellOptions{});
        check(result.ok() && result.output.find("auth-ok") != std::string::npos,
              "команда выполнена по ключу из PEM", libadb::to_string(result.status));
        device->close();
    }

    client.shutdown();

    printf("\n%s (%d failure(s))\n", failures == 0 ? "ALL PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
