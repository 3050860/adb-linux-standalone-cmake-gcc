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
#include "sync_progress.h"

#include "api/device_impl.h"
#include "api/events.h"

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

void fill_transfer_stats(Result& result, uint64_t bytes, uint64_t bytes_on_wire, ms duration) {
    result.transfer.bytes = bytes;
    result.transfer.bytes_on_wire = bytes_on_wire;
    result.transfer.duration = duration;
    if (bytes > 0 && duration.count() > 0) {
        result.transfer.mib_per_sec =
            (static_cast<double>(bytes) / (1024.0 * 1024.0)) / (duration.count() / 1000.0);
    }
}

// Счётчики одной передачи: заполняются из наблюдателя sync (§14 п.5).
// Живут в стеке операции, наблюдатель ссылается на них.
struct TransferCounters {
    uint64_t payload = 0;   // полезные байты (последнее значение прогресса по файлу)
    uint64_t on_wire = 0;   // байты, реально прошедшие через сокет
    uint64_t expected = 0;  // ожидаемый размер, если известен
};

// Собирает наблюдателя, который кормит события операции и колбэк вызывающего.
// serial и progress живут не меньше самого наблюдателя (стек операции).
SyncProgressObserver make_observer(internal::OperationContext& op, const std::string& serial,
                                  const ProgressFn& progress, TransferCounters& counters) {
    SyncProgressObserver observer;
    observer.on_progress = [&op, &serial, &progress, &counters](const std::string& /*path*/,
                                                               uint64_t done, uint64_t total) {
        // done приходит от sync накопительно по текущему файлу; total может быть
        // нулём (размер неизвестен) — тогда подставляем то, что знаем сами.
        counters.payload = done;
        const uint64_t reported_total = total != 0 ? total : counters.expected;
        op.progress(done, reported_total);
        if (progress) progress(serial, done, reported_total);
    };
    observer.on_wire_bytes = [&counters](uint64_t bytes) { counters.on_wire += bytes; };
    return observer;
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
    // Событие публикуем после освобождения мьютекса: обработчик подписчика
    // не должен исполняться под нашим локом (он вызывается из потока adb).
    EventType event_type = EventType::InternalError;
    Status event_status = Status::Ok;
    const char* event_message = nullptr;

    std::string current_serial;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!serial.empty()) serial_ = serial;
        current_serial = serial_;

        switch (state) {
            case kCsDevice:
                online_ = true;
                auth_required_ = false;
                break;
            case kCsUnauthorized:
                auth_required_ = true;
                event_type = EventType::DeviceUnauthorized;
                event_status = Status::Unauthorized;
                event_message = "device is unauthorized";
                break;
            case kCsNoPerm:
                failed_ = true;
                if (error_.empty()) error_ = "insufficient permissions to talk to the device";
                event_type = EventType::DeviceLost;
                event_status = Status::ConnectFailed;
                event_message = "insufficient permissions to talk to the device";
                break;
            case kCsOffline:
            case kCsDetached:
                // Offline приходит и в начале подключения (до ответа устройства),
                // поэтому «упало» считаем только если устройство уже было в сети.
                if (online_) {
                    online_ = false;
                    failed_ = true;
                    if (error_.empty()) error_ = "device went offline";
                    event_type = EventType::DeviceLost;
                    event_status = Status::DeviceLost;
                    event_message = "device went offline";
                }
                break;

            default:
                break;
        }
        cv_.notify_all();
    }

    if (event_message) {
        publish_device_event(event_type, current_serial, event_status, event_message);
    }
}

void FacadeListener::onAuthRequired(const std::string& /*serial*/) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auth_required_ = true;
    }
    cv_.notify_all();
    emit_log(LogLevel::Warn, serial(), "authorization required: accept the RSA key on the device");
    publish_device_event(EventType::DeviceAuthRequired, serial(), Status::AuthRequired,
                         "authorization required: accept the RSA key on the device");
}

void FacadeListener::onShellData(const std::string& /*serial*/, uint32_t /*session_id*/,
                                 const char* data, size_t len, bool is_stderr) {
    std::lock_guard<std::mutex> lock(output_mutex_);
    if (output_.buffer) output_.buffer->append(data, len);
    if (output_.callback && *output_.callback) {
        (*output_.callback)(serial_, std::string_view(data, len), is_stderr);
    }
    // Событие OperationOutput: сам Event уходит в очередь, поэтому подписчик
    // не тормозит reader-поток сессии.
    if (output_.op) output_.op->output(std::string_view(data, len), is_stderr);
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
    publish_device_event(EventType::InternalError, serial(), Status::Internal, error_msg);
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
    if (!closed) {
        closed = true;

        // Порядок важен: сначала отпускаем свой shared_ptr на AdbDevice, потом
        // просим менеджер убрать транспорт — иначе устройство осталось бы живым
        // и следующий connect получил бы старое соединение.
        device.reset();
        if (!serial.empty()) {
            AdbManager::instance().disconnectDevice(serial);
        }
        internal::emit_log(LogLevel::Debug, serial, "disconnected");
        internal::publish_device_event(EventType::DeviceDisconnected, serial, Status::Ok,
                                       "disconnected");
    }

    // Слот отдаём последним и вне проверки closed: при неудачном подключении
    // флаг выставляют вручную, а слот к тому моменту уже занят — иначе он
    // остался бы занятым навсегда. Перемещаем, чтобы вызвать ровно один раз.
    if (release_slot) {
        auto release = std::move(release_slot);
        release_slot = nullptr;
        release();
    }
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
    // Контекст создаём до любых проверок: вызывающий получает OperationId даже
    // у операции, которая тут же и провалилась.
    internal::OperationContext op(Command::Push, impl_->serial);
    op.start(options.on_start);

    if (!is_online()) {
        Result result = device_lost_result(Command::Push);
        op.finish(result);
        return result;
    }

    const uint64_t size = file_size(local);
    if (size == 0 && !android::base::EndsWith(local, "/")) {
        // Пустой или отсутствующий файл: отличаем «нет файла» от «файл на 0 байт».
        struct stat st{};
        if (stat(local.c_str(), &st) != 0) {
            Result result;
            result.status = Status::LocalFileError;
            result.error = "cannot read local file: " + local;
            result.duration = elapsed_since(started);
            op.finish(result);
            return result;
        }
    }

    internal::emit_log(LogLevel::Info, impl_->serial, "push " + local + " -> " + remote);

    AdbFileSync sync(impl_->device);
    op.set_phase(Phase::Transfer);

    TransferCounters counters;
    counters.expected = size;
    const SyncProgressObserver observer =
        make_observer(op, impl_->serial, options.on_progress, counters);

    const auto transfer_started = clock_type::now();
    // quiet = true: прогресс-бар SyncConnection пишет в stdout и ломал бы вывод
    // приложения, у которого много устройств одновременно. Прогресс идёт
    // наблюдателем — в события и в options.on_progress.
    uint64_t transferred = 0;
    const bool ok = sync.push({local}, remote, options.sync_only_newer,
                              internal::to_compression_type(options.compression), true, &observer,
                              &transferred);
    const ms transfer_duration = elapsed_since(transfer_started);

    Result result;
    result.phase = Phase::Transfer;
    result.duration = elapsed_since(started);
    if (ok) {
        // transferred от SyncConnection точнее размера файла: при sync=true часть
        // файлов пропускается, а у каталогов размер вообще не при чём.
        const uint64_t bytes = transferred > 0 ? transferred : size;
        fill_transfer_stats(result, bytes, counters.on_wire, transfer_duration);
        // Завершающее событие прогресса (force): при быстрой передаче троттлинг
        // мог съесть последнее «100 %».
        op.progress(bytes, bytes, true);
        if (options.on_progress) options.on_progress(impl_->serial, bytes, bytes);
    } else {
        result.status = Status::IoError;
        result.error = impl_->listener->take_error();
        if (result.error.empty()) result.error = "push failed";
    }
    op.finish(result);
    return result;
}

Result Device::pull(const std::string& remote, const std::string& local,
                    const PullOptions& options) {
    const auto started = clock_type::now();
    internal::OperationContext op(Command::Pull, impl_->serial);
    op.start(options.on_start);

    if (!is_online()) {
        Result result = device_lost_result(Command::Pull);
        op.finish(result);
        return result;
    }

    internal::emit_log(LogLevel::Info, impl_->serial, "pull " + remote + " -> " + local);

    AdbFileSync sync(impl_->device);
    op.set_phase(Phase::Transfer);

    // Размер файла на устройстве заранее не известен: sync сообщит его сам в
    // первом же событии прогресса (total).
    TransferCounters counters;
    const SyncProgressObserver observer =
        make_observer(op, impl_->serial, options.on_progress, counters);

    const auto transfer_started = clock_type::now();
    uint64_t transferred = 0;
    const bool ok = sync.pull({remote}, local, false,
                              internal::to_compression_type(options.compression), true, &observer,
                              &transferred);
    const ms transfer_duration = elapsed_since(transfer_started);

    Result result;
    result.phase = Phase::Transfer;
    result.duration = elapsed_since(started);
    if (ok) {
        // Если sync ничего не сообщил (например, скачали каталог), падаем на
        // размер локальной копии.
        const uint64_t bytes = transferred > 0 ? transferred : file_size(local);
        fill_transfer_stats(result, bytes, counters.on_wire, transfer_duration);
        op.progress(bytes, bytes, true);
        if (options.on_progress) options.on_progress(impl_->serial, bytes, bytes);
    } else {
        result.status = Status::IoError;
        result.error = impl_->listener->take_error();
        if (result.error.empty()) result.error = "pull failed";
    }
    op.finish(result);
    return result;
}

Result Device::Impl::run_shell(const std::string& command, const ShellOptions& options,
                               internal::OperationContext* op) {
    const auto started = clock_type::now();

    Result result;
    internal::OutputTarget target;
    if (options.capture_output) target.buffer = &result.output;
    if (options.on_output) target.callback = &options.on_output;
    target.op = op;
    listener->set_output_target(target);

    auto session = device->createShellSession(command);
    if (!session || !session->start()) {
        listener->clear_output_target();
        result.status = Status::Internal;
        result.error = "failed to start shell session";
        result.duration = elapsed_since(started);
        return result;
    }

    result.exit_code = session->wait();
    listener->clear_output_target();

    result.phase = Phase::Finalize;
    result.duration = elapsed_since(started);
    // Ненулевой код возврата команды — не ошибка транспорта: status остаётся Ok,
    // а вызывающий смотрит exit_code (как у обычного adb shell).
    return result;
}

Result Device::shell(const std::string& command, const ShellOptions& options) {
    internal::OperationContext op(Command::Shell, impl_->serial);
    op.start(options.on_start);

    if (!is_online()) {
        Result result = device_lost_result(Command::Shell);
        op.finish(result);
        return result;
    }

    op.set_phase(Phase::Transfer);
    Result result = impl_->run_shell(command, options, &op);
    op.set_phase(result.phase);
    op.finish(result);
    return result;
}

Result Device::install(const std::string& apk_path, const InstallOptions& options) {
    const auto started = clock_type::now();
    internal::OperationContext op(Command::Install, impl_->serial);
    op.start(options.on_start);

    if (!is_online()) {
        Result result = device_lost_result(Command::Install);
        op.finish(result);
        return result;
    }

    op.set_phase(Phase::Prepare);
    if (file_size(apk_path) == 0) {
        Result result;
        result.status = Status::LocalFileError;
        result.phase = Phase::Prepare;
        result.error = "apk not found or empty: " + apk_path;
        result.duration = elapsed_since(started);
        op.finish(result);
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
    if (options.on_output) target.callback = &options.on_output;
    target.op = &op;
    impl_->listener->set_output_target(target);

    internal::emit_log(LogLevel::Info, impl_->serial, "install " + apk_path);
    // Разделение фаз install по таймаутам — этап 7; сейчас событиями отмечаем
    // только переход к заливке и коммиту, чтобы подписчик видел прогресс работ.
    op.set_phase(Phase::Transfer);
    // Статусные строки pm ("Success"/"Failure [...]") приходят не через shell-канал,
    // а из кода установки, который печатал их в stdout процесса. Перенаправляем их
    // в result.output: библиотека не должна писать в консоль приложения.
    adb_install_set_status_sink(&result.output);

    // Прогресс заливки apk: install льёт файл через copy_to_file(), там стоит
    // тот же хук наблюдателя, что и в sync.
    const uint64_t apk_size = file_size(apk_path);
    TransferCounters counters;
    counters.expected = apk_size;
    const SyncProgressObserver observer =
        make_observer(op, impl_->serial, options.on_progress, counters);
    const SyncProgressObserver* previous_observer = adb_sync_set_observer(&observer);

    const auto transfer_started = clock_type::now();
    AdbInstaller installer(impl_->device);
    const bool ok = installer.install({apk_path}, flags);
    const ms transfer_duration = elapsed_since(transfer_started);

    adb_sync_set_observer(previous_observer);
    adb_install_set_status_sink(nullptr);
    impl_->listener->clear_output_target();

    impl_->device->clearActiveSessions();

    result.duration = elapsed_since(started);
    // Байты берём от наблюдателя: install-write мог залить не весь файл (ошибка),
    // а на legacy-пути apk уходит push'ем — счётчик всё равно один.
    const uint64_t bytes = counters.payload > 0 ? counters.payload : apk_size;
    fill_transfer_stats(result, bytes, counters.on_wire, transfer_duration);
    if (ok) {
        result.phase = Phase::Finalize;
        op.set_phase(Phase::Finalize);
        op.progress(bytes, bytes, true);
        if (options.on_progress) options.on_progress(impl_->serial, bytes, bytes);
        op.finish(result);
        return result;
    }

    result.phase = Phase::Commit;
    result.exit_code = 1;
    // Код отказа берём из "Failure [CODE...]" — так ловятся и INSTALL_FAILED_*,
    // и INSTALL_PARSE_FAILED_*, которые pm печатает в том же формате.
    result.remote_code = parse_failure_code(result.output);
    result.status = Status::RemoteError;
    result.error = result.remote_code.empty() ? "install failed" : result.remote_code;
    op.set_phase(Phase::Commit);
    op.finish(result);
    return result;

}

Result Device::uninstall(const std::string& package, const UninstallOptions& options) {
    const auto started = clock_type::now();
    internal::OperationContext op(Command::Uninstall, impl_->serial);
    op.start(options.on_start);

    if (!is_online()) {
        Result result = device_lost_result(Command::Uninstall);
        op.finish(result);
        return result;
    }

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
    // Через run_shell(), а не через shell(): вторая операция с собственным
    // OperationId здесь была бы лишней — событие уже одно, uninstall.
    op.set_phase(Phase::Transfer);
    Result result = impl_->run_shell(command, shell_options, &op);
    result.duration = elapsed_since(started);
    result.phase = Phase::Finalize;
    op.set_phase(Phase::Finalize);
    if (!result.ok()) {
        // Ошибка самого канала (устройство отвалилось и т.п.) — отдаём как есть.
        if (result.error.empty()) result.error = "uninstall failed";
        op.finish(result);
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
    op.finish(result);
    return result;
}


std::optional<std::string> Device::get_prop(const std::string& name) {
    ShellOptions options;
    options.capture_output = true;
    // Служебный вызов: отдельной операции и событий не заводим, иначе каждый
    // getprop засорял бы поток событий приложения.
    if (!is_online()) return std::nullopt;
    Result result = impl_->run_shell("getprop " + name, options, nullptr);
    if (!result.ok()) return std::nullopt;

    std::string value = android::base::Trim(result.output);
    if (value.empty()) return std::nullopt;
    return value;
}

}  // namespace libadb
