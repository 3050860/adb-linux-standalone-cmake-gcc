#ifndef __ADB_AUTH_H
#define __ADB_AUTH_H

#include "adb.h"

#include <deque>
#include <memory>
#include <string>
#include <vector>

#include <openssl/rsa.h>

/* AUTH packets first argument */
/* Request */
#define ADB_AUTH_TOKEN         1
/* Response */
#define ADB_AUTH_SIGNATURE     2
#define ADB_AUTH_RSAPUBLICKEY  3

void adb_auth_init();

// --- Настраиваемая инициализация авторизации (libadb, §5) -------------------
//
// Консольный adb всегда работает с ~/.android/adbkey и $ADB_VENDOR_KEYS.
// Библиотеке этого мало: ключи могут приходить текстом из чужой БД или vault,
// а стандартный набор бывает нужно отключить (сервис под своим пользователем).
struct AdbAuthConfig {
    // Приватные ключи из файлов; порядок = порядок попыток.
    std::vector<std::string> key_files;

    // Приватные ключи текстом (PEM). Ключ выводится из приватного.
    std::vector<std::string> private_keys_pem;

    // Использовать ~/.android/adbkey (с генерацией при отсутствии) и
    // $ADB_VENDOR_KEYS.
    bool use_default_key_store = true;

    // Если ни один ключ не загрузился — сгенерировать эфемерный (в памяти).
    bool generate_ephemeral_if_empty = true;

    // Куда записать сгенерированный эфемерный ключ (пусто = не записывать).
    std::string save_generated_key_to;
};

// Возвращает пустую строку при успехе, иначе описание ошибки (какой именно
// ключ не разобрался). Идемпотентна: повторный вызов до'загружает ключи.
std::string adb_auth_init_ex(const AdbAuthConfig& config);

// Была ли авторизация уже настроена через adb_auth_init_ex(). Нужно, чтобы
// AdbManager::start() не подмешал стандартный набор ключей поверх явно
// заданного — в том числе когда вызывающий сознательно оставил набор пустым.
bool adb_auth_is_configured();

// Сколько приватных ключей загружено (без sentinel'а).
size_t adb_auth_key_count();

// Отпечатки загруженных ключей (SHA-256 в hex) — для диагностики.
std::vector<std::string> adb_auth_key_fingerprints();

int adb_auth_keygen(const char* filename);
int adb_auth_pubkey(const char* filename);
std::string adb_auth_get_userkey();
bssl::UniquePtr<EVP_PKEY> adb_auth_get_user_privkey();
std::deque<std::shared_ptr<RSA>> adb_auth_get_private_keys();

void send_auth_response(const char* token, size_t token_size, atransport* t);

int adb_tls_set_certificate(SSL* ssl);
void adb_auth_tls_handshake(atransport* t);

#endif // __ADB_AUTH_H
