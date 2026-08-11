#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

// Наблюдатель за передачей файлов (§14 п.5 спецификации libadb).
//
// Код sync из adb (file_sync_client.cpp) писал прогресс только в stdout через
// LinePrinter. Библиотеке нужен машинно-читаемый поток: сколько полезных байт
// уже передано, сколько ушло/пришло по проводу после сжатия, и возможность
// прервать передачу.
//
// Наблюдатель ставится на текущий поток (thread_local), как и остальные
// перехваты в этой сборке (`adb_set_current_device`,
// `adb_install_set_status_sink`): sync целиком выполняется в потоке, который
// позвал do_sync_push_fd()/do_sync_pull_fd().
struct SyncProgressObserver {
    // Полезные байты текущего файла. total == 0 — размер неизвестен.
    std::function<void(const std::string& path, uint64_t done, uint64_t total)> on_progress;

    // Байты, реально прошедшие через сокет (после сжатия): при push — записанные,
    // при pull — прочитанные. Вызывается часто и мелкими порциями.
    std::function<void(uint64_t bytes)> on_wire_bytes;

    // true — передачу нужно прервать (отмена или таймаут). Проверяется между
    // блоками данных, поэтому реакция — в пределах одного блока (64 КиБ).
    std::function<bool()> should_abort;
};

// Ставит/снимает наблюдателя для текущего потока. nullptr — снять.
// Возвращает предыдущего: наблюдатели вкладываются (install → push внутри).
const SyncProgressObserver* adb_sync_set_observer(const SyncProgressObserver* observer);
const SyncProgressObserver* adb_sync_observer();
