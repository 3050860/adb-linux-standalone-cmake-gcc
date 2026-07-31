// libadb: версия библиотеки.
#include "libadb/libadb.h"

namespace libadb {

const char* version() {
    return LIBADB_VERSION_STRING;
}

uint32_t version_number() {
    return static_cast<uint32_t>(LIBADB_VERSION_NUMBER);
}

}  // namespace libadb
