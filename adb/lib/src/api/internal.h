/*
 * libadb — внутренние помощники фасада. Не устанавливается, в ABI не входит.
 */
#pragma once

#include <string>
#include <string_view>

#include "libadb/libadb.h"

// hidden: имена libadb::internal::* подпадают под шаблон _ZN6libadb* в
// libadb.map, поэтому видимость гасим явно — иначе внутренние помощники
// оказались бы в динамической таблице .so.
#define LIBADB_INTERNAL __attribute__((visibility("hidden")))

namespace libadb::internal {

// Инициализирует libbase-логирование (InitLogging + AdbLogger) ровно один раз.
// Безопасно вызывать из любого места фасада перед работой с внутренним кодом.
LIBADB_INTERNAL void ensure_logging_initialized();

// Отправляет сообщение в LogSink приложения, если он задан.
LIBADB_INTERNAL void emit_log(LogLevel level, const std::string& serial,
                              std::string_view message);

// Есть ли смысл форматировать сообщение (sink задан и уровень проходит фильтр).
LIBADB_INTERNAL bool log_sink_wants(LogLevel level);

// Текущие таймауты клиента (§6). Операции читают их на старте, поэтому
// Client::set_timeouts() влияет на всё, что начнётся после вызова.
LIBADB_INTERNAL Timeouts current_timeouts();

// Ставит SIG_IGN на SIGPIPE, если приложение не настроило обработчик само:
// запись в оборванный сокет должна давать EPIPE, а не убивать процесс.
LIBADB_INTERNAL void ensure_sigpipe_ignored();


}  // namespace libadb::internal
