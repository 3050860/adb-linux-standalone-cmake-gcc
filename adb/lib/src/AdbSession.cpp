#include "AdbSession.h"
#include "AdbDevice.h"
#include "transport.h"
#include "socket.h"
#include "sysdeps.h"
#include "adb_io.h"
#include <sys/socket.h>
#include <android-base/logging.h>
#include <future>
#include "shell_protocol.h"

AdbSession::AdbSession(std::shared_ptr<AdbDevice> device, uint32_t session_id, const std::string& service_string,
                       bool use_shell_v2)
    : device_(device), session_id_(session_id), service_string_(service_string), use_shell_v2_(use_shell_v2),
      exit_code_future_(exit_code_promise_.get_future()) 
      {}

AdbSession::~AdbSession() {
    abort();
    if (reader_thread_.joinable()) {
        reader_thread_.join();
    }
}

int AdbSession::wait() {
    return exit_code_future_.get();
}

bool AdbSession::waitFor(unsigned timeout_ms, int* exit_code) {
    if (timeout_ms == 0) {
        const int code = wait();
        if (exit_code) *exit_code = code;
        return true;
    }

    // Ждём именно future, а не сам поток: reader-поток закрывается сам, а нам
    // важно, чтобы код возврата был уже выставлен.
    if (exit_code_future_.wait_for(std::chrono::milliseconds(timeout_ms)) !=
        std::future_status::ready) {
        // Таймаут: рвём сессию, чтобы reader-поток вышел и не остался висеть
        // на adb_read() до конца жизни процесса.
        abort();
        return false;
    }

    const int code = exit_code_future_.get();
    if (exit_code) *exit_code = code;
    return true;
}

bool AdbSession::start(bool start_reader_thread) {
    atransport* t = device_->getTransport();
    if (!t) {
        device_->notifyError("Transport lost before session start");
        return false;
    }

    int fds[2];
    if (adb_socketpair(fds) != 0) {
        device_->notifyError("Failed to create socketpair for session");
        return false;
    }
    
    local_fd_.reset(fds[0]);
    adb_fd_.reset(fds[1]);

    // Мы НЕ МОЖЕМ вызывать create_local_socket из текущего (main) потока.
    // Мы должны попросить сделать это fdevent_loop thread.
    std::promise<bool> setup_promise;
    std::future<bool> setup_future = setup_promise.get_future();

    fdevent_run_on_looper([this, t, &setup_promise]() {
        bool success = false;
        
        // Мы передаем владение дескриптором в create_local_socket.
        // Если она вернет nullptr, unique_fd автоматически закроет дескриптор, предотвращая утечку.
        unique_fd fd_for_adb(adb_fd_.release());
        asocket* s = create_local_socket(std::move(fd_for_adb));
        
        if (s) {
            s->transport = t;
            adb_socket_ = s; // Сохраняем сырой указатель (ADB сам удалит этот объект при закрытии)
            
            // Отправляем запрос на открытие сервиса (A_OPEN)
            connect_to_remote(adb_socket_, service_string_);
            success = true;
        } else {
            // Если ADB не смог создать сокет, закрываем нашу сторону, чтобы readerThread не завис
            local_fd_.reset();
        }
        
        // Разблокируем главный поток и передаем результат
        setup_promise.set_value(success);
    });

    // Ждем, пока фоновый поток завершит настройку сокета
    bool result = setup_future.get();
    
    if (result) {
        if (start_reader_thread)
            // Запускаем поток чтения ТОЛЬКО после успешной регистрации сокета в ADB
            reader_thread_ = std::thread(&AdbSession::readerThread, this);
    }

    return result;
}

void AdbSession::abort() {
    if (is_aborted_.exchange(true)) return;
    
    // Закрытие local_fd_ приведет к тому, что readerThread завершится.
    // ADB заметит это (через local_socket_event_func), сам отправит A_CLSE 
    // и корректно уничтожит asocket.
    local_fd_.reset(); 
}
bool AdbSession::write2(const void* data, size_t length) {
    return adb_write(local_fd_.get(), data, length) == static_cast<ssize_t>(length);
}
void AdbSession::readerThread() {
    int exit_code = 255;

    if (use_shell_v2_) {
        // ИСПОЛЬЗУЕМ ГОТОВЫЙ КЛАСС ИЗ ИСХОДНИКОВ ADB
        ShellProtocol protocol(local_fd_.get());
        while (protocol.Read()) {
            if (protocol.id() == ShellProtocol::kIdStdout) {
                if (device_->listener_) {
                    device_->listener_->onShellData(device_->getSerial(), session_id_, 
                                                    protocol.data(), protocol.data_length(), false);
                }
            } else if (protocol.id() == ShellProtocol::kIdStderr) {
                if (device_->listener_) {
                    device_->listener_->onShellData(device_->getSerial(), session_id_, 
                                                    protocol.data(), protocol.data_length(), true);
                }
            } else if (protocol.id() == ShellProtocol::kIdExit) {
                exit_code = static_cast<uint8_t>(protocol.data()[0]);
            }
        }
    } else {
        // Legacy режим (простой текст)
        char buffer[4096];
        while (!is_aborted_) {
            ssize_t bytes_read = adb_read(local_fd_.get(), buffer, sizeof(buffer));
            if (bytes_read <= 0) break;
            if (device_->listener_) {
                device_->listener_->onShellData(device_->getSerial(), session_id_, buffer, bytes_read, false);
            }
        }
    }

    if (device_->listener_) {
        device_->listener_->onSessionClosed(device_->getSerial(), session_id_, exit_code);
    }
    try {
        exit_code_promise_.set_value(exit_code);
    } catch (...) {
        // Игнорируем, если promise уже установлен
    }
    // device_->unregisterSession(session_id_);
}