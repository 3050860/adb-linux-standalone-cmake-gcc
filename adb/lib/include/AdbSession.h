#pragma once
#include <memory>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include "adb.h"
 // Для create_local_socket, connect_to_remote
#include "socket.h"

class AdbDevice;

class AdbSession : public std::enable_shared_from_this<AdbSession> {
public:
    AdbSession(std::shared_ptr<AdbDevice> device, uint32_t session_id, const std::string& service_string);
    ~AdbSession();

    uint32_t getId() const { return session_id_; }

    // Запускает выполнение команды. Возвращает true, если команда успешно отправлена.
    bool start();

    // Немедленно прерывает сессию (закрывает сокет, что вызывает разрыв цикла чтения)
    void abort();

private:
    void readerThread();

    std::shared_ptr<AdbDevice> device_;
    uint32_t session_id_;
    std::string service_string_;
    
    unique_fd local_fd_;      // Наш конец socketpair для чтения
    unique_fd adb_fd_;        // Конец socketpair, переданный в ADB
    std::shared_ptr<asocket> adb_socket_; // Обертка ADB над adb_fd_
    
    std::thread reader_thread_;
    std::atomic<bool> is_aborted_{false};
};
