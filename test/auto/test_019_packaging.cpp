// test_019: упаковка — установка, pkg-config, CMake export, version script
// (§13, этап 11).
//
// Это тест *потребителя*: он проверяет только то, что попало в staging-каталог
// после `cmake --install`, а не дерево сборки. Проверяем: заголовки и .so на
// месте, симлинки версий правильные, SONAME корректный, pkg-config отдаёт
// рабочие флаги, CMake-экспорт содержит libadb::adb, наружу видны только
// публичные символы.
//
// Staging-каталог готовит скрипт test/auto/run_test_019.sh; он же собирает
// и запускает потребителей обоими способами (pkg-config и find_package).
//
// Запуск: /tmp/test_019 <staging-prefix>   (например /tmp/stage11/usr)
#include <cstdio>
#include <cstdlib>
#include <string>
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

// Выполняет команду и возвращает её вывод.
std::string run(const std::string& cmd) {
    std::string out;
    FILE* pipe = popen((cmd + " 2>&1").c_str(), "r");
    if (!pipe) return out;
    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe)) out += buffer;
    pclose(pipe);
    return out;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string prefix = argc > 1 ? argv[1] : "/tmp/stage11/usr";
    const std::string libdir = prefix + "/lib/x86_64-linux-gnu";

    printf("staging prefix: %s\n", prefix.c_str());

    // --- 1. Заголовки ---------------------------------------------------------
    check(exists(prefix + "/include/libadb/libadb.h"), "установлен libadb.h");
    check(exists(prefix + "/include/libadb/version.h"),
          "установлен сгенерированный version.h");
    check(exists(prefix + "/include/libadb/spdlog_sink.hpp"), "установлен spdlog_sink.hpp");
    // Шаблон version.h.in — деталь сборки, в пакете ему не место.
    check(!exists(prefix + "/include/libadb/version.h.in"),
          "version.h.in НЕ установлен (это шаблон сборки)");

    // --- 2. Библиотека и симлинки версий -------------------------------------
    check(exists(libdir + "/libadb.so.1.0.0"), "установлен libadb.so.1.0.0");
    check(exists(libdir + "/libadb.so.1"), "установлен SONAME-симлинк libadb.so.1");
    check(exists(libdir + "/libadb.so"), "установлен линковочный симлинк libadb.so");

    const std::string soname = run("readelf -d " + libdir + "/libadb.so.1.0.0 | grep SONAME");
    check(contains(soname, "libadb.so.1"), "SONAME внутри файла — libadb.so.1", soname);

    // --- 3. pkg-config --------------------------------------------------------
    const std::string pc_env = "PKG_CONFIG_PATH=" + libdir + "/pkgconfig ";
    check(exists(libdir + "/pkgconfig/libadb.pc"), "установлен libadb.pc");

    const std::string version = run(pc_env + "pkg-config --modversion libadb");
    check(contains(version, "1.0.0"), "pkg-config --modversion", version);

    const std::string libs = run(pc_env + "pkg-config --libs libadb");
    check(contains(libs, "-ladb"), "pkg-config --libs содержит -ladb", libs);
    // Внутренние зависимости наружу не торчат.
    check(!contains(libs, "-lssl") && !contains(libs, "-lcrypto"),
          "pkg-config --libs не тянет BoringSSL", libs);

    // --cflags для префикса /usr намеренно ПУСТОЙ: pkg-config опускает
    // -I/usr/include, потому что это и так путь поиска по умолчанию. Чтобы
    // увидеть флаг для staging-каталога, нужен PKG_CONFIG_SYSROOT_DIR — заодно
    // это проверяет, что includedir в .pc собран из ${prefix}, а не зашит.
    const std::string sysroot = prefix.substr(0, prefix.size() - 4);  // отрезаем "/usr"
    const std::string cflags =
            run("PKG_CONFIG_SYSROOT_DIR=" + sysroot + " " + pc_env + "pkg-config --cflags libadb");
    check(contains(cflags, "include"), "pkg-config --cflags указывает на include (с sysroot)",
          cflags);
    check(contains(cflags, sysroot), "includedir в .pc относителен prefix", cflags);

    // --- 4. CMake export ------------------------------------------------------
    check(exists(libdir + "/cmake/libadb/libadbConfig.cmake"), "установлен libadbConfig.cmake");
    check(exists(libdir + "/cmake/libadb/libadbConfigVersion.cmake"),
          "установлен libadbConfigVersion.cmake");
    check(exists(libdir + "/cmake/libadb/libadbTargets.cmake"),
          "установлен libadbTargets.cmake");

    const std::string targets = run("cat " + libdir + "/cmake/libadb/libadbTargets.cmake");
    check(contains(targets, "libadb::adb"), "экспортирован таргет libadb::adb");
    // В экспорте не должно быть путей из дерева сборки: иначе пакет непереносим.
    check(!contains(targets, "/build/include"),
          "в экспорте нет путей из дерева сборки (BUILD_INTERFACE не утёк)");

    // --- 5. Публичные символы .so --------------------------------------------
    const std::string exported =
            run("nm -D --defined-only " + libdir + "/libadb.so.1.0.0 | grep -c ' T '");
    printf("[INFO] экспортировано символов: %s", exported.c_str());

    const std::string leaked = run("nm -D --defined-only " + libdir +
                                   "/libadb.so.1.0.0 | grep -icE 'SSL_|EVP_|BIO_new|ZipArchive'");
    check(contains(leaked, "0"), "символы BoringSSL/ziparchive наружу не видны", leaked);

    const std::string internals = run("nm -D --defined-only " + libdir +
                                      "/libadb.so.1.0.0 | grep -icE 'libadb8internal'");
    check(contains(internals, "0"), "внутренние символы libadb::internal скрыты", internals);

    // --- 6. Сама библиотека работоспособна -----------------------------------
    check(std::string(libadb::version()) == "1.0.0", "libadb::version()", libadb::version());
    check((libadb::version_number() >> 16) == LIBADB_VERSION_MAJOR,
          "MAJOR из version_number() совпадает с заголовком",
          std::to_string(libadb::version_number() >> 16));

    printf("\n%s (%d failure(s))\n", failures == 0 ? "ALL PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
