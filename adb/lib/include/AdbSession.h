#pragma once
#include <memory>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <future>
#include "adb.h"
#include "socket.h" // Для asocket, create_local_socket, connect_to_remote

class AdbDevice;

class AdbSession : public std::enable_shared_from_this<AdbSession> {
public:
    AdbSession(std::shared_ptr<AdbDevice> device, uint32_t session_id, const std::string& service_string,
                bool use_shell_v2);
    ~AdbSession();

    uint32_t getId() const { return session_id_; }
    bool start(bool start_reader_thread = true);
    void abort();
    bool write2(const void* data, size_t length);
    int wait();
    int getFd() const { return local_fd_.get(); }
private:
    void readerThread();

    std::shared_ptr<AdbDevice> device_;
    uint32_t session_id_;
    std::string service_string_;
    
    unique_fd local_fd_;      // Наш конец socketpair для чтения
    unique_fd adb_fd_;        // Конец socketpair, переданный в ADB
    
    // ИСПРАВЛЕНИЕ: asocket удаляет себя сам (через local_socket_destroy).
    // Мы не должны использовать shared_ptr, иначе будет double-free.
    asocket* adb_socket_ = nullptr; 
    
    std::thread reader_thread_;
    std::atomic<bool> is_aborted_{false};
    bool use_shell_v2_;
    std::promise<int> exit_code_promise_;
    std::future<int> exit_code_future_;
};