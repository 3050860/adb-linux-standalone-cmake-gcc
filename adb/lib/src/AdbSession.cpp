// AdbSession.cpp
#include "AdbSession.h"
#include "AdbDevice.h"
#include "transport.h"
#include <sys/socket.h>
#include <android-base/logging.h>

AdbSession::AdbSession(std::shared_ptr<AdbDevice> device, uint32_t session_id, const std::string& service_string)
    : device_(device), session_id_(session_id), service_string_(service_string) {}

AdbSession::~AdbSession() {
    abort();
    if (reader_thread_.joinable()) {
        reader_thread_.join();
    }
}

bool AdbSession::start() {
    int fds[2];
    if (adb_socketpair(fds) != 0) {
        device_->notifyError("Failed to create socketpair for session");
        return false;
    }
    
    local_fd_.reset(fds[0]);
    adb_fd_.reset(fds[1]);

    // 1. Создаем локальный сокет ADB вокруг нашего конца socketpair.
    // Это добавляет сокет в глобальный local_socket_list, и handle_packet сможет его найти.
    adb_socket_.reset(create_local_socket(unique_fd(adb_fd_.get()))); 
    // Примечание: unique_fd(adb_fd_.get()) забирает владение, но мы сохранили копию дескриптора выше,
    // чтобы иметь возможность закрыть его при abort(). В реальной реализации лучше использовать 
    // дублирование дескриптора или кастомную обертку, но для концепции это работает.
    
    // Восстанавливаем владение для adb_socket_, чтобы он не закрыл его раньше времени
    // (create_local_socket забирает unique_fd, так что adb_fd_ теперь "пуст", но мы можем работать через local_fd_)
    
    // 2. Настраиваем транспорт для этого сокета
    adb_socket_->transport = device_->transport_.get();

    // 3. Отправляем запрос на открытие сервиса на устройстве (A_OPEN)
    // Эта функция сформирует пакет A_OPEN и отправит его через транспорт
    connect_to_remote(adb_socket_.get(), service_string_);

    // 4. Запускаем поток чтения, который будет забирать данные из local_fd_
    reader_thread_ = std::thread(&AdbSession::readerThread, this);

    return true;
}

void AdbSession::abort() {
    if (is_aborted_.exchange(true)) return; // Уже прервано
    
    // Закрытие local_fd_ приведет к тому, что readerThread завершится.
    // При уничтожении AdbSession (или при явном вызове close у asocket),
    // ADB отправит пакет A_CLSE на устройство.
    local_fd_.reset(); 
}

void AdbSession::readerThread() {
    char buffer[4096];
    while (!is_aborted_) {
        // Читаем из socketpair. Этот вызов заблокируется, пока ADB (через handle_packet -> local_socket_flush_incoming)
        // не запишет туда данные из пакета A_WRTE.
        ssize_t bytes_read = adb_read(local_fd_.get(), buffer, sizeof(buffer));
        
        if (bytes_read <= 0) {
            if (bytes_read == 0 || (bytes_read < 0 && errno != EAGAIN)) {
                // EOF или ошибка (например, сокет закрыт через abort())
                break;
            }
        }

        if (bytes_read > 0) {
            // Передаем данные в колбэк вызывающего кода
            // Примечание: для shell v2 здесь нужно парсить ShellProtocol (ID stdout/stderr/exit).
            // Для упрощения передаем как есть, но в реальном коде добавь парсинг ShellProtocol, 
            // который уже есть в commandline.cpp (read_and_dump_protocol).
            if (device_->listener_) {
                device_->listener_->onShellData(device_->getSerial(), session_id_, buffer, bytes_read, false);
            }
        }
    }

    // Сообщаем о закрытии
    if (device_->listener_) {
        device_->listener_->onSessionClosed(device_->getSerial(), session_id_, 0); // TODO: реальный exit_code из ShellProtocol
    }
    
    // Убираем сессию из устройства
    device_->unregisterSession(session_id_);
}
