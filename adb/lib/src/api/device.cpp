// libadb: реализация Device — операции на одном устройстве.
#include <sys/stat.h>

#include <chrono>
#include <utility>
#include <vector>

#include <android-base/strings.h>

#include "AdbFileSync.h"
#include "AdbInstaller.h"
#include "AdbSession.h"
#include "adb_install_lib.h"
#include "adb_utils.h"

#include "api/device_impl.h"

namespace libadb {
namespace {

using clock_type = std::chrono::steady_clock;

ms elapsed_since(clock_type::time_point start) {
    return std::chrono::duration_cast<ms>(clock_type::now() - start);
}

// Размер локального файла; 0, если файла нет или это не обычный файл.
uint64_t file_size(const std::string& path) {
    struct stat st{};
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return 0;
    return static_cast<uint64_t>(st.st_size);
}

void fill_transfer_stats(Result& result, uint64_t bytes, ms duration) {
    result.transfer.bytes = bytes;
    // Внутренний SyncConnection пока не сообщает, сколько ушло в сеть после
    // сжатия, поэтому bytes_on_wire остаётся нулём (см. журнал, этап 3).
    result.transfer.duration = duration;
    if (bytes > 0 && duration.count() > 0) {
        result.transfer.mib_per_sec =
            (static_cast<double>(bytes) / (1024.0 * 1024.0)) / (duration.count() / 1000.0);
    }
}

// Оборачивает аргумент в одинарные кавычки для shell на устройстве.
std::string quote_arg(const std::string& value) {
    std::string quoted = "'";
    for (char c : value) {
        if (c == '\'') {
            quoted += "'\\''";  // выходим из кавычек, экранируем ', входим обратно
        } else {
            quoted += c;
        }
    }
    quoted += '\'';
    return quoted;
}

// Достаёт код отказа из вывода pm/cmd: "Failure [DELETE_FAILED_INTERNAL_ERROR]",
// "Failure [INSTALL_PARSE_FAILED_NOT_APK: ...]" и т.п.
std::string parse_failure_code(const std::string& output) {
    const size_t failure_pos = output.find("Failure");
    if (failure_pos == std::string::npos) return {};

    const size_t open = output.find('[', failure_pos);
    if (open == std::string::npos) return "FAILURE";

    const size_t code_start = open + 1;
    const size_t end = output.find_first_of(":] \r\n", code_start);
    if (end == std::string::npos || end == code_start) return "FAILURE";
    return output.substr(code_start, end - code_start);
}

// Готовит Result для случая «устройство уже закрыто/потеряно».
Result device_lost_result(Command command) {

    Result result;
    result.status = Status::DeviceLost;
    result.error = std::string("device is not connected (") + to_string(command) + ")";
    return result;
}

}  // namespace

// ---------------------------------------------------------------------------
// FacadeListener
// ---------------------------------------------------------------------------

namespace internal {

void FacadeListener::onConnectionStateChanged(const std::string& serial, ConnectionState state) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!serial.empty()) serial_ = serial;

    switch (state) {
        case kCsDevice:
            online_ = true;
            auth_required_ = false;
            break;
        case kCsUnauthorized:
            auth_required_ = true;
            break;
        case kCsNoPerm:
            failed_ = true;
            if (error_.empty()) error_ = "insufficient permissions to talk to the device";
            break;
        case kCsOffline:
        case kCsDetached:
            // Offline приходит и в начале подключения (до ответа устройства),
            // поэтому «упало» считаем только если устройство уже было в сети.
            if (online_) {
                online_ = false;
                failed_ = true;
                if (error_.empty()) error_ = "device went offline";
            }
            break;

        default:
            break;
    }
    cv_.notify_all();
}

void FacadeListener::onAuthRequired(const std::string& /*serial*/) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auth_required_ = true;
    }
    cv_.notify_all();
    emit_log(LogLevel::Warn, serial(), "authorization required: accept the RSA key on the device");
}

void FacadeListener::onShellData(const std::string& /*serial*/, uint32_t /*session_id*/,
                                 const char* data, size_t len, bool is_stderr) {
    std::lock_guard<std::mutex> lock(output_mutex_);
    if (output_.buffer) output_.buffer->append(data, len);
    if (output_.callback && *output_.callback) {
        (*output_.callback)(serial_, std::string_view(data, len), is_stderr);
    }
}

void FacadeListener::onSessionClosed(const std::string& /*serial*/, uint32_t /*session_id*/,
                                     int /*exit_code*/) {
    // Код возврата берётся из AdbSession::wait(); здесь ничего не требуется.
}

void FacadeListener::onError(const std::string& /*serial*/, const std::string& error_msg) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        failed_ = true;
        error_ = error_msg;
    }
    cv_.notify_all();
    emit_log(LogLevel::Error, serial(), error_msg);
}

Status FacadeListener::wait_until_online(ms timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    const bool signaled = cv_.wait_for(lock, timeout, [this] {
        return online_ || failed_ || auth_required_;
    });

    if (online_) return Status::Ok;
    if (auth_required_) return Status::AuthRequired;
    if (failed_) return Status::ConnectFailed;
    return signaled ? Status::ConnectFailed : Status::ConnectTimeout;
}

void FacadeListener::set_output_target(const OutputTarget& target) {
    std::lock_guard<std::mutex> lock(output_mutex_);
    output_ = target;
}

void FacadeListener::clear_output_target() {
    std::lock_guard<std::mutex> lock(output_mutex_);
    output_ = OutputTarget{};
}

std::string FacadeListener::take_error() {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::move(error_);
}

bool FacadeListener::online() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return online_;
}

void FacadeListener::set_serial(std::string serial) {
    std::lock_guard<std::mutex> lock(mutex_);
    serial_ = std::move(serial);
}

std::string FacadeListener::serial() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return serial_;
}

CompressionType to_compression_type(Compression compression) {
    switch (compression) {
        case Compression::None:   return CompressionType::None;
        case Compression::Any:    return CompressionType::Any;
        case Compression::Brotli: return CompressionType::Brotli;
        case Compression::Lz4:    return CompressionType::LZ4;
        case Compression::Zstd:   return CompressionType::Zstd;
    }
    return CompressionType::Any;
}

DevicePtr DeviceFactory::create(std::unique_ptr<Device::Impl> impl) {
    // std::make_shared здесь нельзя: конструктор Device приватный, а фабрика —
    // friend, поэтому создаём объект вручную.
    return DevicePtr(new Device(std::move(impl)));
}


}  // namespace internal

// ---------------------------------------------------------------------------
// Device::Impl
// ---------------------------------------------------------------------------

void Device::Impl::close() {
    if (closed) return;
    closed = true;

    // Порядок важен: сначала отпускаем свой shared_ptr на AdbDevice, потом
    // просим менеджер убрать транспорт — иначе устройство осталось бы живым
    // и следующий connect получил бы старое соединение.
    device.reset();
    if (!serial.empty()) {
        AdbManager::instance().disconnectDevice(serial);
    }
    internal::emit_log(LogLevel::Debug, serial, "disconnected");
}

Device::Impl::~Impl() {
    close();
}

// ---------------------------------------------------------------------------
// Device
// ---------------------------------------------------------------------------

Device::Device(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Device::~Device() = default;

const std::string& Device::serial() const {
    return impl_->serial;
}

bool Device::is_online() const {
    if (impl_->closed || !impl_->device) return false;
    return impl_->listener->online() && impl_->device->getTransport() != nullptr;
}

void Device::close() {
    impl_->close();
}

Result Device::push(const std::string& local, const std::string& remote,
                    const PushOptions& options) {
    const auto started = clock_type::now();
    if (!is_online()) return device_lost_result(Command::Push);

    const uint64_t size = file_size(local);
    if (size == 0 && !android::base::EndsWith(local, "/")) {
        // Пустой или отсутствующий файл: отличаем «нет файла» от «файл на 0 байт».
        struct stat st{};
        if (stat(local.c_str(), &st) != 0) {
            Result result;
            result.status = Status::LocalFileError;
            result.error = "cannot read local file: " + local;
            result.duration = elapsed_since(started);
            return result;
        }
    }

    internal::emit_log(LogLevel::Info, impl_->serial, "push " + local + " -> " + remote);

    AdbFileSync sync(impl_->device);
    const auto transfer_started = clock_type::now();
    // quiet = true: прогресс-бар SyncConnection пишет в stdout и ломал бы вывод
    // приложения, у которого много устройств одновременно.
    const bool ok = sync.push({local}, remote, options.sync_only_newer,
                              internal::to_compression_type(options.compression), true);
    const ms transfer_duration = elapsed_since(transfer_started);

    Result result;
    result.phase = Phase::Transfer;
    result.duration = elapsed_since(started);
    if (ok) {
        fill_transfer_stats(result, size, transfer_duration);
    } else {
        result.status = Status::IoError;
        result.error = impl_->listener->take_error();
        if (result.error.empty()) result.error = "push failed";
    }
    return result;
}

Result Device::pull(const std::string& remote, const std::string& local,
                    const PullOptions& options) {
    const auto started = clock_type::now();
    if (!is_online()) return device_lost_result(Command::Pull);

    internal::emit_log(LogLevel::Info, impl_->serial, "pull " + remote + " -> " + local);

    AdbFileSync sync(impl_->device);
    const auto transfer_started = clock_type::now();
    const bool ok = sync.pull({remote}, local, false,
                              internal::to_compression_type(options.compression), true);
    const ms transfer_duration = elapsed_since(transfer_started);

    Result result;
    result.phase = Phase::Transfer;
    result.duration = elapsed_since(started);
    if (ok) {
        // Размер известен только после скачивания — берём с локальной копии.
        fill_transfer_stats(result, file_size(local), transfer_duration);
    } else {
        result.status = Status::IoError;
        result.error = impl_->listener->take_error();
        if (result.error.empty()) result.error = "pull failed";
    }
    return result;
}

Result Device::shell(const std::string& command, const ShellOptions& options) {
    const auto started = clock_type::now();
    if (!is_online()) return device_lost_result(Command::Shell);

    Result result;
    internal::OutputTarget target;
    if (options.capture_output) target.buffer = &result.output;
    if (options.on_output) target.callback = &options.on_output;
    impl_->listener->set_output_target(target);

    auto session = impl_->device->createShellSession(command);
    if (!session || !session->start()) {
        impl_->listener->clear_output_target();
        result.status = Status::Internal;
        result.error = "failed to start shell session";
        result.duration = elapsed_since(started);
        return result;
    }

    result.exit_code = session->wait();
    impl_->listener->clear_output_target();

    result.phase = Phase::Finalize;
    result.duration = elapsed_since(started);
    // Ненулевой код возврата команды — не ошибка транспорта: status остаётся Ok,
    // а вызывающий смотрит exit_code (как у обычного adb shell).
    return result;
}

Result Device::install(const std::string& apk_path, const InstallOptions& options) {
    const auto started = clock_type::now();
    if (!is_online()) return device_lost_result(Command::Install);

    if (file_size(apk_path) == 0) {
        Result result;
        result.status = Status::LocalFileError;
        result.error = "apk not found or empty: " + apk_path;
        result.duration = elapsed_since(started);
        return result;
    }

    std::vector<std::string> flags;
    if (options.reinstall) flags.push_back("-r");
    if (options.allow_downgrade) flags.push_back("-d");
    if (options.grant_permissions) flags.push_back("-g");
    flags.insert(flags.end(), options.extra_args.begin(), options.extra_args.end());

    Result result;
    // Вывод pm приходит теми же событиями, что и shell: собираем его, чтобы
    // вытащить причину отказа (INSTALL_FAILED_*).
    internal::OutputTarget target;
    target.buffer = &result.output;
    impl_->listener->set_output_target(target);

    internal::emit_log(LogLevel::Info, impl_->serial, "install " + apk_path);
    // Статусные строки pm ("Success"/"Failure [...]") приходят не через shell-канал,
    // а из кода установки, который печатал их в stdout процесса. Перенаправляем их
    // в result.output: библиотека не должна писать в консоль приложения.
    adb_install_set_status_sink(&result.output);
    AdbInstaller installer(impl_->device);
    const bool ok = installer.install({apk_path}, flags);
    adb_install_set_status_sink(nullptr);
    impl_->listener->clear_output_target();

    impl_->device->clearActiveSessions();

    result.duration = elapsed_since(started);
    fill_transfer_stats(result, file_size(apk_path), result.duration);
    if (ok) {
        result.phase = Phase::Finalize;
        return result;
    }

    result.phase = Phase::Commit;
    result.exit_code = 1;
    // Код отказа берём из "Failure [CODE...]" — так ловятся и INSTALL_FAILED_*,
    // и INSTALL_PARSE_FAILED_*, которые pm печатает в том же формате.
    result.remote_code = parse_failure_code(result.output);
    result.status = Status::RemoteError;
    result.error = result.remote_code.empty() ? "install failed" : result.remote_code;
    return result;

}

Result Device::uninstall(const std::string& package, const UninstallOptions& options) {
    const auto started = clock_type::now();
    if (!is_online()) return device_lost_result(Command::Uninstall);

    // Удаление выполняем через обычный shell-канал: путь uninstall_app() из
    // кода adb опирается на send_shell_command(), которая в этой сборке
    // является заглушкой и молча возвращает успех.
    // На старых прошивках 'cmd' отсутствует — выбираем инструмент до запуска,
    // а не по коду возврата: 'cmd package uninstall' умеет печатать Success и
    // при этом возвращать ненулевой код, из-за чего fallback стирал результат.
    const std::string keep = options.keep_data ? " -k" : "";
    const std::string quoted = quote_arg(package);
    const std::string command = "if command -v cmd >/dev/null 2>&1; then cmd package uninstall" +
                                keep + " " + quoted + "; else pm uninstall" + keep + " " + quoted +
                                "; fi 2>&1";


    internal::emit_log(LogLevel::Info, impl_->serial, "uninstall " + package);

    ShellOptions shell_options;
    shell_options.capture_output = true;
    Result result = shell(command, shell_options);
    result.duration = elapsed_since(started);
    result.phase = Phase::Finalize;
    if (!result.ok()) {
        // Ошибка самого канала (устройство отвалилось и т.п.) — отдаём как есть.
        if (result.error.empty()) result.error = "uninstall failed";
        return result;
    }

    // pm/cmd почти всегда завершаются с кодом 0, поэтому смотрим на текст.
    // "Success" — решающий признак: некоторые прошивки печатают его вместе с
    // предупреждениями и возвращают ненулевой код.
    const bool success = result.output.find("Success") != std::string::npos;
    if (!success) result.remote_code = parse_failure_code(result.output);
    if (!success) {

        result.status = Status::RemoteError;
        result.exit_code = result.exit_code == 0 ? 1 : result.exit_code;
        result.error = result.remote_code.empty()
                           ? "uninstall failed: " + android::base::Trim(result.output)
                           : result.remote_code;
    }
    return result;
}


std::optional<std::string> Device::get_prop(const std::string& name) {
    ShellOptions options;
    options.capture_output = true;
    Result result = shell("getprop " + name, options);
    if (!result.ok()) return std::nullopt;

    std::string value = android::base::Trim(result.output);
    if (value.empty()) return std::nullopt;
    return value;
}

}  // namespace libadb
