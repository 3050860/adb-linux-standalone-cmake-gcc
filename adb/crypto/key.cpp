#include "adb/crypto/key.h"

#include <android-base/logging.h>
#include <openssl/bn.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

namespace adb {
namespace crypto {

// static
std::string Key::ToPEMString(EVP_PKEY* pkey) {
    bssl::UniquePtr<BIO> bio(BIO_new(BIO_s_mem()));
    int rc = PEM_write_bio_PKCS8PrivateKey(bio.get(), pkey, nullptr, nullptr, 0, nullptr, nullptr);
    if (rc != 1) {
        LOG(ERROR) << "PEM_write_bio_PKCS8PrivateKey failed";
        return "";
    }

    BUF_MEM* mem = nullptr;
    BIO_get_mem_ptr(bio.get(), &mem);
    if (!mem || !mem->data || !mem->length) {
        LOG(ERROR) << "BIO_get_mem_ptr failed";
        return "";
    }

    return std::string(mem->data, mem->length);
}

}  // namespace crypto
}  // namespace adb
