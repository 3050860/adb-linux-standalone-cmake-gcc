// libadb: реализация Device — операции на одном устройстве.
#include <sys/stat.h>

#include <atomic>
#include <chrono>
#include <thread>
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
#include "api/operation_impl.h"

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

// Дедлайны передачи (§6.1): общий и «нет прогресса дольше stall».
// Живёт в стеке операции; читается из потока передачи, обновляется оттуда же.
class TransferDeadline {
  public:
    TransferDeadline(TransferTimeout timeout, clock_type::time_point start)
        : timeout_(timeout), start_(start), last_progress_(start) {}

    // Вызывать при каждом продвижении: сбрасывает отсчёт stall.
    void touch() { last_progress_ = clock_type::now(); }

    // Какой таймаут сработал (Status::Ok — ни один).
    Status expired() const {
        const auto now = clock_type::now();
        if (timeout_.total > ms::zero() && now - start_ >= timeout_.total) {
            return Status::CommandTimeout;
        }
        if (timeout_.stall > ms::zero() && now - last_progress_ >= timeout_.stall) {
            return Status::StallTimeout;
        }
        return Status::Ok;
    }

    // Запомненная причина срыва: должна дожить до формирования Result, а сам
    // expired() к тому моменту уже неинформативен (мы прервали передачу).
    Status reason = Status::Ok;

  private:
    TransferTimeout timeout_;
    clock_type::time_point start_;
    clock_type::time_point last_progress_;
};

// Собирает наблюдателя, который кормит события операции и колбэк вызывающего.
// Все ссылки живут в стеке операции, то есть дольше самого наблюдателя.
// deadline может быть nullptr — тогда передача не ограничена по времени.
SyncProgressObserver make_observer(internal::OperationContext& op, const std::string& serial,
                                  const ProgressFn& progress, TransferCounters& counters,
                                  TransferDeadline* deadline) {
    SyncProgressObserver observer;
    observer.on_progress = [&op, &serial, &progress, &counters, deadline](
                                   const std::string& /*path*/, uint64_t done, uint64_t total) {
        // done приходит от sync накопительно по текущему файлу; total может быть
        // нулём (размер неизвестен) — тогда подставляем то, что знаем сами.
        counters.payload = done;
        const uint64_t reported_total = total != 0 ? total : counters.expected;
        // Прогресс есть — stall-таймер начинает отсчёт заново.
        if (deadline) deadline->touch();
        op.progress(done, reported_total);
        if (progress) progress(serial, done, reported_total);
    };
    observer.on_wire_bytes = [&counters](uint64_t bytes) { counters.on_wire += bytes; };
    observer.should_abort = [deadline, &op] {
        if (deadline && deadline->reason != Status::Ok) return true;  // уже решили прерваться
        // Отмена (Client::cancel / Operation::cancel / close_all) — §9.
        if (op.canceled()) {
            if (deadline) deadline->reason = op.cancel_status();
            return true;
        }
        if (!deadline) return false;
        const Status reason = deadline->expired();
        if (reason == Status::Ok) return false;
        deadline->reason = reason;
        return true;
    };
    return observer;
}

// Ошибку прерывания (таймаут или отмена) раскладываем в Result одинаково для
// push и pull: причина уже в deadline.reason, здесь только текст.
std::string abort_message(Status reason, const char* what) {
    switch (reason) {
        case Status::StallTimeout:
            return std::string(what) + " stalled: no progress within the stall timeout";
        case Status::CommandTimeout:
            return std::string(what) + " exceeded the total transfer timeout";
        case Status::ConnectionClosed:
            return std::string(what) + " aborted: connections were closed";
        case Status::Canceled:
            return std::string(what) + " canceled";
        default:
            return std::string(what) + " aborted";
    }
}

// Наблюдение за install (§6.2). Установка выполняется одним блокирующим вызовом
// install_multiple_app(), поэтому фазы отслеживаются по наблюдаемым признакам:
// пошли байты → Transfer; байты кончились → Commit (pm верифицирует и ставит).
// Дедлайны у фаз разные, а на фазе Commit прогресса нет вообще — там вместо
// угадывания времени работает health-check.
struct InstallMonitor {
    InstallTimeout timeout;
    clock_type::time_point started;

    // Обновляется наблюдателем передачи.
    std::atomic<uint64_t> bytes{0};
    std::atomic<int64_t> last_progress_ms{0};   // от started
    std::atomic<bool> transfer_started{false};

    // Итог наблюдения: пишется только потоком-наблюдателем и читается после
    // его завершения, поэтому синхронизация не нужна.
    std::atomic<bool> abort{false};
    Status reason = Status::Ok;
    std::string message;
    Phase phase = Phase::CreateSession;  // на какой фазе прервались

    int64_t now_ms() const {
        return std::chrono::duration_cast<ms>(clock_type::now() - started).count();
    }
};

// Одна проверка «устройство ещё живо». Дешёвый режим Transport ничего не
// запускает на устройстве; Shell строже, но создаёт процесс на каждую проверку.
bool health_check_ok(Device::Impl& impl, HealthCheckMode mode) {
    switch (mode) {
        case HealthCheckMode::None:
            return true;
        case HealthCheckMode::Transport:
            return impl.device && impl.device->getTransport() != nullptr &&
                   impl.listener->online();
        case HealthCheckMode::Shell: {
            if (!impl.device || impl.device->getTransport() == nullptr) return false;
            ShellOptions probe;
            probe.capture_output = false;
            // Своя короткая команда со своим таймаутом: она не должна ждать
            // столько же, сколько сам commit.
            Result result = impl.run_shell("true", probe, nullptr, ms{10000});
            return result.ok();
        }
    }
    return true;
}

// Следит за фазами install и их таймаутами, пока рабочий поток занят
// install_multiple_app(). Возвращается, когда установка закончилась сама или
// когда мы решили её прервать (monitor.abort — наблюдатель прочитает его между
// блоками данных).
void monitor_install(Device::Impl& impl, internal::OperationContext& op, InstallMonitor& monitor,
                     const std::atomic<bool>& done) {
    const InstallTimeout& timeout = monitor.timeout;
    int health_failures = 0;
    int64_t commit_started_ms = -1;
    int64_t last_health_ms = 0;

    const auto give_up = [&monitor](Status reason, std::string message) {
        monitor.reason = reason;
        monitor.message = std::move(message);
        monitor.abort.store(true, std::memory_order_relaxed);
    };

    while (!done.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(ms{100});
        if (done.load(std::memory_order_acquire)) return;

        // Отмена (§9). На фазе Commit это уже не отменит установку на самом
        // устройстве — pm получил команду; мы лишь перестаём её ждать, и это
        // отражается в Result::error.
        if (op.canceled()) {
            give_up(op.cancel_status(),
                    monitor.phase == Phase::Commit
                            ? "install canceled while committing: the device keeps installing"
                            : "install canceled");
            return;
        }

        const int64_t now = monitor.now_ms();
        const bool transferring = monitor.transfer_started.load(std::memory_order_relaxed);
        const int64_t last_progress = monitor.last_progress_ms.load(std::memory_order_relaxed);

        if (!transferring) {
            // Байты ещё не пошли — идёт pm install-create.
            if (timeout.create_session > ms::zero() && now >= timeout.create_session.count()) {
                give_up(Status::CommandTimeout, "pm install-create did not respond in time");
                return;
            }
            continue;
        }

        // Тишина дольше секунды после заливки означает, что pm уже коммитит:
        // отдельного сигнала «install-write закончился» у нас нет.
        const bool committing = monitor.phase == Phase::Commit || now - last_progress >= 1000;
        if (!committing) {
            if (monitor.phase != Phase::Transfer) {
                monitor.phase = Phase::Transfer;
                op.set_phase(Phase::Transfer);
            }
            const TransferTimeout& tt = timeout.transfer;
            if (tt.total > ms::zero() && now >= tt.total.count()) {
                give_up(Status::CommandTimeout, "install transfer exceeded the total timeout");
                return;
            }
            if (tt.stall > ms::zero() && now - last_progress >= tt.stall.count()) {
                give_up(Status::StallTimeout, "install transfer stalled");
                return;
            }
            continue;
        }

        // Фаза Commit: прогресса нет принципиально, поэтому общий дедлайн плюс
        // health-check — отличаем «долго ставится» от «устройство умерло».
        if (monitor.phase != Phase::Commit) {
            monitor.phase = Phase::Commit;
            commit_started_ms = now;
            last_health_ms = now;
            op.set_phase(Phase::Commit);
        }
        if (timeout.commit > ms::zero() && now - commit_started_ms >= timeout.commit.count()) {
            give_up(Status::CommandTimeout, "pm install-commit did not finish in time");
            return;
        }

        const ms interval = timeout.commit_healthcheck_interval;
        if (timeout.commit_healthcheck == HealthCheckMode::None || interval <= ms::zero()) continue;
        if (now - last_health_ms < interval.count()) continue;

        last_health_ms = now;
        if (health_check_ok(impl, timeout.commit_healthcheck)) {
            health_failures = 0;
            op.heartbeat("device is alive, still committing");
        } else if (++health_failures >= timeout.commit_healthcheck_failures) {
            give_up(Status::DeviceLost, "device stopped responding while committing");
            return;
        }
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

// Раскладывает код отказа pm в Status (§10). Код уже вынут из "Failure [CODE]".
Status status_from_install_code(const std::string& code) {
    if (code.empty()) return Status::RemoteError;

    // INSTALL_PARSE_FAILED_* — целое семейство (NOT_APK, BAD_MANIFEST,
    // NO_CERTIFICATES, ...), все означают «apk не годится».
    if (code.rfind("INSTALL_PARSE_FAILED_", 0) == 0) return Status::InvalidApk;

    if (code == "INSTALL_FAILED_UPDATE_INCOMPATIBLE") return Status::SignatureMismatch;
    if (code == "INSTALL_FAILED_VERSION_DOWNGRADE") return Status::VersionDowngrade;
    if (code == "INSTALL_FAILED_INSUFFICIENT_STORAGE") return Status::InsufficientStorage;
    if (code == "INSTALL_FAILED_INVALID_APK") return Status::InvalidApk;
    if (code == "INSTALL_FAILED_MISSING_SPLIT") return Status::MissingSplit;

    // Всё остальное (INSTALL_FAILED_TEST_ONLY, INSTALL_FAILED_USER_RESTRICTED,
    // DELETE_FAILED_*, ...) — RemoteError с сохранением кода в remote_code.
    return Status::RemoteError;
}

bool ends_with_ignore_case(const std::string& text, const char* suffix) {
    return android::base::EndsWithIgnoreCase(text, suffix);
}

// Определяет InstallKind для Auto по составу списка файлов (§10).
InstallKind resolve_install_kind(InstallKind requested, const std::vector<std::string>& paths) {
    if (requested != InstallKind::Auto) return requested;
    if (paths.size() == 1) {
        // .apks/.zip — бандл bundletool: распакуем и поставим как SplitSet.
        if (ends_with_ignore_case(paths[0], ".apks") || ends_with_ignore_case(paths[0], ".zip")) {
            return InstallKind::Bundle;
        }
        return InstallKind::Single;
    }
    // Несколько файлов по умолчанию считаем частями ОДНОГО пакета: это самый
    // частый случай (base + split_config.*). Независимые пакеты вызывающий
    // должен запросить явно — MultiPackage меняет семантику на атомарную
    // сессию, и угадывать это по именам файлов было бы гаданием.
    return InstallKind::SplitSet;
}

// Пытается вытащить имя пакета из текста ошибки pm. Прошивки пишут разное,
// но чаще всего имя присутствует в сообщении:
//   "Failure [INSTALL_FAILED_UPDATE_INCOMPATIBLE: Package com.example
//    signatures do not match previously installed version; ignoring!]"
std::string package_name_from_output(const std::string& output) {
    static const char* const kMarkers[] = {"Package ", "package "};
    for (const char* marker : kMarkers) {
        size_t pos = output.find(marker);
        while (pos != std::string::npos) {
            const size_t start = pos + strlen(marker);
            const size_t end = output.find_first_of(" \t\r\n:;,]", start);
            std::string candidate = output.substr(start, end == std::string::npos ? end
                                                                                 : end - start);
            // Имя пакета обязано содержать точку и не быть похожим на путь.
            if (candidate.find('.') != std::string::npos &&
                candidate.find('/') == std::string::npos && candidate.size() > 2) {
                return candidate;
            }
            pos = output.find(marker, start);
        }
    }
    return {};
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

        // Операции этого устройства должны узнать причину: соединение закрыли,
        // это не IoError и не их собственная отмена (§9).
        internal::OperationRegistry::instance().close(serial);

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
    const auto transfer_started = clock_type::now();
    TransferDeadline deadline(options.timeout.value_or(internal::current_timeouts().push),
                              transfer_started);
    const SyncProgressObserver observer =
        make_observer(op, impl_->serial, options.on_progress, counters, &deadline);

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
    } else if (deadline.reason != Status::Ok || op.canceled()) {
        // Передачу прервали мы сами (таймаут или отмена) — не выдаём это за
        // IoError. Проверяем и op.canceled(): при close_all() запись в сокет
        // падает раньше, чем наблюдатель успевает заметить флаг, и причина
        // ConnectionClosed иначе подменилась бы ошибкой ввода-вывода.
        const Status reason = deadline.reason != Status::Ok ? deadline.reason : op.cancel_status();
        fill_transfer_stats(result, counters.payload, counters.on_wire, transfer_duration);
        result.status = reason;
        result.error = abort_message(reason, "push");
        impl_->listener->take_error();  // сообщение sync об обрыве нам не нужно
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
    const auto transfer_started = clock_type::now();
    TransferDeadline deadline(options.timeout.value_or(internal::current_timeouts().pull),
                              transfer_started);
    const SyncProgressObserver observer =
        make_observer(op, impl_->serial, options.on_progress, counters, &deadline);

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
    } else if (deadline.reason != Status::Ok || op.canceled()) {
        // См. комментарий в push: op.canceled() нужен для close_all().
        const Status reason = deadline.reason != Status::Ok ? deadline.reason : op.cancel_status();
        fill_transfer_stats(result, counters.payload, counters.on_wire, transfer_duration);
        result.status = reason;
        result.error = abort_message(reason, "pull");
        impl_->listener->take_error();
    } else {
        result.status = Status::IoError;
        result.error = impl_->listener->take_error();
        if (result.error.empty()) result.error = "pull failed";
    }
    op.finish(result);
    return result;
}

Result Device::Impl::run_shell(const std::string& command, const ShellOptions& options,
                               internal::OperationContext* op, ms timeout) {
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

    // Ждём порциями: waitFor() сам прерывает сессию по истечении своего
    // таймаута (§14 п.4), а между порциями мы успеваем заметить отмену.
    // Без нарезки cancel() для shell работал бы только после таймаута.
    constexpr ms kSlice{100};
    int exit_code = 0;
    bool finished = false;
    bool canceled = false;
    const auto deadline = started + timeout;
    for (;;) {
        if (op && op->canceled()) {
            canceled = true;
            break;
        }
        ms slice = kSlice;
        if (timeout > ms::zero()) {
            const auto left = std::chrono::duration_cast<ms>(deadline - clock_type::now());
            if (left <= ms::zero()) break;  // истёк общий таймаут команды
            slice = std::min(kSlice, left);
        }
        if (session->waitFor(static_cast<unsigned>(slice.count()), &exit_code)) {
            finished = true;
            break;
        }
    }

    // Прерывать сессию — забота вызывающего: waitFor() этого не делает.
    // Без abort() reader-поток остался бы висеть на adb_read() до конца жизни
    // процесса, а деструктор AdbSession ждал бы его join().
    // abort() синхронно закрывает и наш конец socketpair, и asocket внутри
    // fdevent-потока (см. AdbSession::abort), поэтому следующая команда на этом
    // устройстве создаётся уже на чистом состоянии.
    if (!finished) session->abort();
    listener->clear_output_target();

    if (canceled) {
        result.status = op->cancel_status();
        result.phase = Phase::Transfer;
        result.exit_code = -1;
        result.error = result.status == Status::ConnectionClosed
                               ? "command aborted: connections were closed"
                               : "command canceled";
        result.duration = elapsed_since(started);
        return result;
    }

    if (!finished) {
        result.status = Status::CommandTimeout;
        result.phase = Phase::Transfer;
        result.exit_code = -1;
        result.error = "command timed out after " + std::to_string(timeout.count()) + " ms";
        result.duration = elapsed_since(started);
        return result;
    }

    result.exit_code = exit_code;
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
    const ms timeout = options.timeout.value_or(internal::current_timeouts().shell);
    Result result = impl_->run_shell(command, options, &op, timeout);
    op.set_phase(result.phase);
    op.finish(result);
    return result;
}

Result Device::Impl::run_install_attempt(const std::vector<std::string>& paths,
                                         const std::vector<std::string>& flags,
                                         bool multi_package, uint64_t total_size,
                                         const InstallOptions& options,
                                         const InstallTimeout& timeout,
                                         internal::OperationContext& op) {
    const auto started = clock_type::now();
    const uint64_t apk_size = total_size;

    Result result;
    // Вывод pm приходит теми же событиями, что и shell: собираем его, чтобы
    // вытащить причину отказа (INSTALL_FAILED_*).
    internal::OutputTarget target;
    target.buffer = &result.output;
    if (options.on_output) target.callback = &options.on_output;
    target.op = &op;
    listener->set_output_target(target);

    op.set_phase(Phase::CreateSession);

    InstallMonitor monitor;
    monitor.timeout = timeout;
    monitor.started = clock_type::now();

    TransferCounters counters;
    counters.expected = apk_size;

    // Наблюдатель заливки: тот же, что у sync (install льёт apk через
    // copy_to_file()). Дополнительно кормит монитор фаз.
    SyncProgressObserver observer;
    observer.on_progress = [&](const std::string&, uint64_t done, uint64_t /*total*/) {
        counters.payload = done;
        monitor.bytes.store(done, std::memory_order_relaxed);
        monitor.last_progress_ms.store(monitor.now_ms(), std::memory_order_relaxed);
        monitor.transfer_started.store(true, std::memory_order_relaxed);
        op.progress(done, apk_size);
        if (options.on_progress) options.on_progress(serial, done, apk_size);
    };
    observer.on_wire_bytes = [&counters](uint64_t bytes) { counters.on_wire += bytes; };
    observer.should_abort = [&monitor] { return monitor.abort.load(std::memory_order_relaxed); };

    const auto transfer_started = clock_type::now();

    // Статусные строки pm ("Success"/"Failure [...]") приходят не через shell-канал,
    // а из кода установки, который печатал их в stdout процесса. Перенаправляем их
    // в result.output: библиотека не должна писать в консоль приложения.
    // Оба перехвата thread_local, поэтому ставим их внутри рабочего потока.
    std::atomic<bool> install_ok{false};
    std::atomic<bool> install_done{false};
    std::thread worker([&] {
        adb_install_set_status_sink(&result.output);
        const SyncProgressObserver* previous = adb_sync_set_observer(&observer);
        AdbInstaller installer(device);
        install_ok.store(installer.install(paths, flags, multi_package),
                         std::memory_order_release);
        adb_sync_set_observer(previous);
        adb_install_set_status_sink(nullptr);
        install_done.store(true, std::memory_order_release);
    });

    monitor_install(*this, op, monitor, install_done);

    // Даже решив прерваться, дожидаемся рабочий поток: он держит ссылки на
    // result.output, наблюдателя и AdbDevice.
    worker.join();
    const bool ok = install_ok.load(std::memory_order_acquire) && monitor.reason == Status::Ok;
    const ms transfer_duration = elapsed_since(transfer_started);

    listener->clear_output_target();
    device->clearActiveSessions();

    result.duration = elapsed_since(started);
    // Байты берём от наблюдателя: install-write мог залить не весь файл (ошибка),
    // а на legacy-пути apk уходит push'ем — счётчик всё равно один.
    const uint64_t bytes = counters.payload > 0 ? counters.payload : apk_size;
    fill_transfer_stats(result, bytes, counters.on_wire, transfer_duration);
    if (ok) {
        result.phase = Phase::Finalize;
        op.set_phase(Phase::Finalize);
        op.progress(bytes, bytes, true);
        if (options.on_progress) options.on_progress(serial, bytes, bytes);
        return result;
    }

    // Установку прервали мы сами (таймаут фазы, отмена или потеря устройства) —
    // статус берём от монитора, а не выдаём за отказ pm.
    if (monitor.reason != Status::Ok) {
        result.phase = monitor.phase;
        result.status = monitor.reason;
        result.exit_code = -1;
        result.error = monitor.message;
        return result;
    }

    result.phase = Phase::Commit;
    result.exit_code = 1;
    // Код отказа берём из "Failure [CODE...]" — так ловятся и INSTALL_FAILED_*,
    // и INSTALL_PARSE_FAILED_*, которые pm печатает в том же формате.
    result.remote_code = parse_failure_code(result.output);
    // Раскладываем код в Status (§10): SignatureMismatch, VersionDowngrade,
    // InsufficientStorage, InvalidApk, MissingSplit, иначе RemoteError.
    result.status = status_from_install_code(result.remote_code);
    result.error = result.remote_code.empty() ? "install failed" : result.remote_code;
    op.set_phase(Phase::Commit);
    return result;
}

Result Device::install(const std::string& apk_path, const InstallOptions& options) {
    return install(std::vector<std::string>{apk_path}, options);
}

Result Device::install(const std::vector<std::string>& paths, const InstallOptions& options) {
    const auto started = clock_type::now();
    internal::OperationContext op(Command::Install, impl_->serial);
    op.start(options.on_start);

    const auto fail = [&](Status status, Phase phase, std::string message) {
        Result result;
        result.status = status;
        result.phase = phase;
        result.error = std::move(message);
        result.duration = elapsed_since(started);
        op.finish(result);
        return result;
    };

    if (paths.empty()) return fail(Status::InvalidArgument, Phase::Prepare, "no files to install");

    // ReinstallKeepData заявлен в API, но обещать сохранение данных мы не будем:
    // `pm uninstall -k` с чужой подписью не спасает и на многих прошивках
    // объявлен нерабочим (§10).
    if (options.on_conflict == ConflictPolicy::ReinstallKeepData) {
        return fail(Status::NotImplemented, Phase::Prepare,
                    "ConflictPolicy::ReinstallKeepData is reserved and not implemented");
    }

    if (!is_online()) {
        Result result = device_lost_result(Command::Install);
        op.finish(result);
        return result;
    }

    // --- фаза Prepare: проверка файлов и распаковка бандла --------------------
    op.set_phase(Phase::Prepare);
    const InstallTimeout timeout = options.timeout.value_or(internal::current_timeouts().install);
    const InstallKind kind = resolve_install_kind(options.kind, paths);

    for (const auto& path : paths) {
        if (file_size(path) == 0) {
            return fail(Status::LocalFileError, Phase::Prepare, "file not found or empty: " + path);
        }
    }
    if (kind == InstallKind::Single && paths.size() > 1) {
        return fail(Status::InvalidArgument, Phase::Prepare,
                    "InstallKind::Single expects exactly one file");
    }

    // Бандл распаковываем сами, до запуска pm: так фаза Prepare честно
    // укладывается в свой таймаут, а состав пакета известен заранее (нужен для
    // общего размера, то есть для прогресса).
    std::vector<std::string> files = paths;
    bool expanded = false;
    if (kind == InstallKind::Bundle) {
        const auto prepare_started = clock_type::now();
        files = AdbInstaller::expandApks(paths);
        if (files.empty()) {
            return fail(Status::InvalidApk, Phase::Prepare,
                        "cannot expand bundle (.apks/.zip): no apk inside or unzip failed");
        }
        expanded = true;
        if (timeout.prepare > ms::zero() && elapsed_since(prepare_started) > timeout.prepare) {
            AdbInstaller::cleanupExpanded(files);
            return fail(Status::CommandTimeout, Phase::Prepare,
                        "unpacking the bundle took longer than InstallTimeout::prepare");
        }
    }

    uint64_t total_size = 0;
    for (const auto& file : files) total_size += file_size(file);

    // --- флаги pm ------------------------------------------------------------
    std::vector<std::string> flags;
    if (options.reinstall) flags.push_back("-r");
    if (options.allow_downgrade) flags.push_back("-d");
    if (options.grant_permissions) flags.push_back("-g");
    if (options.user_id >= 0) {
        flags.push_back("--user");
        flags.push_back(std::to_string(options.user_id));
    }
    flags.insert(flags.end(), options.extra_args.begin(), options.extra_args.end());

    const bool multi_package = kind == InstallKind::MultiPackage;

    internal::emit_log(LogLevel::Info, impl_->serial,
                       std::string("install ") + to_string(kind) + ": " + files.front() +
                               (files.size() > 1
                                        ? " (+" + std::to_string(files.size() - 1) + " more)"
                                        : ""));

    Result result = impl_->run_install_attempt(files, flags, multi_package, total_size, options,
                                               timeout, op);

    // --- повтор при downgrade ------------------------------------------------
    if (result.status == Status::VersionDowngrade && options.allow_downgrade_retry &&
        !options.allow_downgrade && !op.canceled()) {
        internal::emit_log(LogLevel::Warn, impl_->serial, "version downgrade: retrying with -d");
        op.retry(Status::VersionDowngrade, "retrying install with -d");

        std::vector<std::string> retry_flags = flags;
        retry_flags.insert(retry_flags.begin(), "-d");
        result = impl_->run_install_attempt(files, retry_flags, multi_package, total_size, options,
                                           timeout, op);
        result.retries += 1;
    }

    // --- повтор при конфликте подписи (§10) ----------------------------------
    // Никакой флаг pm тут не поможет: единственный путь без root — удалить пакет
    // и поставить заново, данные при этом теряются. Делаем это только по явной
    // просьбе (ConflictPolicy::Reinstall) и только зная имя пакета.
    if (result.status == Status::SignatureMismatch &&
        options.on_conflict == ConflictPolicy::Reinstall && !op.canceled()) {
        std::string package = options.package_name;
        // Explicit — только то, что передали. Auto/Both умеют ещё вытащить имя
        // из текста ошибки pm; разбор AndroidManifest.xml — этап 10.
        if (options.package_name_source != PackageNameSource::Explicit &&
            (package.empty() || options.package_name_source == PackageNameSource::Auto)) {
            const std::string detected = package_name_from_output(result.output);
            if (!detected.empty()) package = detected;
        }

        if (package.empty()) {
            result.error = "signature mismatch: package name is unknown, set "
                           "InstallOptions::package_name";
            if (options.package_name_source == PackageNameSource::Explicit) {
                result.status = Status::InvalidArgument;
            }
        } else {
            internal::emit_log(LogLevel::Warn, impl_->serial,
                               "signature mismatch: reinstalling " + package +
                                       " (application data will be lost)");
            op.retry(Status::SignatureMismatch,
                     "uninstalling " + package + " before reinstall (data will be lost)");

            UninstallOptions uninstall_options;
            const Result removed = uninstall(package, uninstall_options);
            if (!removed.ok()) {
                result.error =
                        "signature mismatch: cannot uninstall " + package + ": " + removed.error;
            } else {
                result = impl_->run_install_attempt(files, flags, multi_package, total_size,
                                                    options, timeout, op);
                result.retries += 1;
            }
        }
    }

    // Временные каталоги распакованного бандла убираем сами: попытки могли
    // повторяться, а expandApks() распаковывает ровно один раз.
    if (expanded) AdbInstaller::cleanupExpanded(files);

    result.duration = elapsed_since(started);
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
    std::string args = options.keep_data ? " -k" : "";
    // --user N: сахар над тем же флагом pm (§9). Число подставляем сами, поэтому
    // экранировать нечего.
    if (options.user_id >= 0) args += " --user " + std::to_string(options.user_id);

    const std::string quoted = quote_arg(package);
    const std::string command = "if command -v cmd >/dev/null 2>&1; then cmd package uninstall" +
                                args + " " + quoted + "; else pm uninstall" + args + " " + quoted +
                                "; fi 2>&1";


    internal::emit_log(LogLevel::Info, impl_->serial, "uninstall " + package);

    ShellOptions shell_options;
    shell_options.capture_output = true;
    // Через run_shell(), а не через shell(): вторая операция с собственным
    // OperationId здесь была бы лишней — событие уже одно, uninstall.
    op.set_phase(Phase::Transfer);
    Result result = impl_->run_shell(command, shell_options, &op,
                                     options.timeout.value_or(internal::current_timeouts().uninstall));
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


// ---------------------------------------------------------------------------
// Асинхронный режим (§9)
// ---------------------------------------------------------------------------

namespace {

// Общий каркас *_async: одна операция на устройство, работа уходит в пул.
// task вызывается в потоке пула и возвращает Result.
OperationPtr start_async(const DevicePtr& device, Device::Impl& impl, Command command,
                         std::function<Result()> task) {
    // Занятость проверяем и захватываем одним обменом: два *_async из разных
    // потоков не должны оба «успеть».
    bool expected = false;
    if (!impl.async_busy.compare_exchange_strong(expected, true)) {
        Result busy;
        busy.status = Status::DeviceBusy;
        busy.error = "device is busy with another asynchronous operation";
        return internal::OperationFactory::completed(command, impl.serial, std::move(busy));
    }

    auto op_impl = std::make_unique<Operation::Impl>();
    op_impl->command = command;
    op_impl->serial = impl.serial;
    // Держим устройство: вызывающий вправе отпустить свой DevicePtr сразу.
    op_impl->device = device;

    Operation::Impl* raw = op_impl.get();
    OperationPtr operation = internal::OperationFactory::create(std::move(op_impl));

    // Копия shared_ptr на саму операцию: она должна дожить до конца задачи,
    // даже если вызывающий выбросил свой хэндл.
    internal::AsyncPool::instance().submit([raw, operation, &impl, task = std::move(task)] {
        // Флаг отмены передаём через поток: публичные Device::push/shell/...
        // не принимают его параметром (не хочется тащить это в ABI).
        Result result;
        {
            internal::PendingCancelFlag pending(raw->flag);
            result = task();
        }
        impl.async_busy.store(false);
        raw->complete(std::move(result));
    });
    return operation;
}

}  // namespace

OperationPtr Device::push_async(const std::string& local, const std::string& remote,
                                const PushOptions& options) {
    auto self = shared_from_this();
    return start_async(self, *impl_, Command::Push,
                       [self, local, remote, options] { return self->push(local, remote, options); });
}

OperationPtr Device::pull_async(const std::string& remote, const std::string& local,
                                const PullOptions& options) {
    auto self = shared_from_this();
    return start_async(self, *impl_, Command::Pull,
                       [self, remote, local, options] { return self->pull(remote, local, options); });
}

OperationPtr Device::shell_async(const std::string& command, const ShellOptions& options) {
    auto self = shared_from_this();
    return start_async(self, *impl_, Command::Shell,
                       [self, command, options] { return self->shell(command, options); });
}

OperationPtr Device::install_async(const std::string& apk_path, const InstallOptions& options) {
    return install_async(std::vector<std::string>{apk_path}, options);
}

OperationPtr Device::install_async(const std::vector<std::string>& paths,
                                   const InstallOptions& options) {
    auto self = shared_from_this();
    return start_async(self, *impl_, Command::Install,
                       [self, paths, options] { return self->install(paths, options); });
}

OperationPtr Device::uninstall_async(const std::string& package, const UninstallOptions& options) {
    auto self = shared_from_this();
    return start_async(self, *impl_, Command::Uninstall,
                       [self, package, options] { return self->uninstall(package, options); });
}

bool Device::busy() const {
    return impl_->async_busy.load();
}

size_t Device::cancel_current() {
    return internal::OperationRegistry::instance().cancel_serial(impl_->serial);
}

std::optional<std::string> Device::get_prop(const std::string& name) {
    ShellOptions options;
    options.capture_output = true;
    // Служебный вызов: отдельной операции и событий не заводим, иначе каждый
    // getprop засорял бы поток событий приложения.
    if (!is_online()) return std::nullopt;
    Result result = impl_->run_shell("getprop " + name, options, nullptr,
                                     internal::current_timeouts().shell);
    if (!result.ok()) return std::nullopt;

    std::string value = android::base::Trim(result.output);
    if (value.empty()) return std::nullopt;
    return value;
}

}  // namespace libadb
