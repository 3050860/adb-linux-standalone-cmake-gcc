// libadb: реализация Client — инициализация, подключение, групповые операции.
#include <mutex>
#include <shared_mutex>
#include <utility>
#include <vector>

#include "AdbManager.h"
#include "api/device_impl.h"

namespace libadb {
namespace {

// Приводит адрес к виду "host:port", подставляя порт по умолчанию.
// Пустая строка означает «разобрать не удалось».
std::string normalize_address(const std::string& text, uint16_t default_port) {
    auto parsed = DeviceAddress::parse(text);
    if (!parsed) return {};
    return parsed->to_string(default_port);
}

Result make_error_result(Status status, const std::string& message) {
    Result result;
    result.status = status;
    result.error = message;
    result.phase = Phase::Connecting;
    return result;
}

}  // namespace

// ---------------------------------------------------------------------------
// Client::Impl
// ---------------------------------------------------------------------------

struct Client::Impl {
    mutable std::shared_mutex mutex;
    Options options;
    bool initialized = false;

    // Живые подключения, выданные connect(): нужны для close_all().
    // weak_ptr — владельцем остаётся вызывающий; мы не продлеваем жизнь Device.
    std::mutex devices_mutex;
    std::vector<std::weak_ptr<Device>> devices;

    void remember(const DevicePtr& device) {
        std::lock_guard<std::mutex> lock(devices_mutex);
        // Попутно выкидываем истёкшие ссылки, чтобы список не рос бесконечно.
        for (auto it = devices.begin(); it != devices.end();) {
            it = it->expired() ? devices.erase(it) : ++it;
        }
        devices.push_back(device);
    }

    void close_all() {
        std::vector<DevicePtr> alive;
        {
            std::lock_guard<std::mutex> lock(devices_mutex);
            for (auto& weak : devices) {
                if (auto device = weak.lock()) alive.push_back(std::move(device));
            }
            devices.clear();
        }
        // close() вне мьютекса: он ждёт освобождения транспорта.
        for (auto& device : alive) device->close();
    }

    Options snapshot() const {
        std::shared_lock<std::shared_mutex> lock(mutex);
        return options;
    }
};

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------

Client::Client() : impl_(std::make_unique<Impl>()) {}
Client::~Client() = default;

Client& Client::instance() {
    // Синглтон в куче и без удаления: event loop adb и транспорты живут до
    // конца процесса, а порядок разрушения статических объектов не определён.
    static Client* instance = new Client();
    return *instance;
}

Status Client::initialize(const Options& options) {
    internal::ensure_logging_initialized();

    {
        std::unique_lock<std::shared_mutex> lock(impl_->mutex);
        impl_->options = options;
    }

    // Логи применяем до старта event loop, чтобы поймать сообщения подключения.
    libadb::set_log_options(options.log);
    libadb::set_log_sink(options.log_sink);

    auto& manager = AdbManager::instance();
    manager.setMaxThreads(options.max_parallel);

    bool need_start = false;
    {
        std::unique_lock<std::shared_mutex> lock(impl_->mutex);
        need_start = !impl_->initialized;
        impl_->initialized = true;
    }
    if (need_start) {
        // Повторный start() менеджера безопасен, но лишний вызов бессмысленен.
        manager.start();
        internal::emit_log(LogLevel::Debug, "", std::string("libadb ") + version() + " initialized");
    }
    return Status::Ok;
}

bool Client::initialized() const {
    std::shared_lock<std::shared_mutex> lock(impl_->mutex);
    return impl_->initialized;
}

const Options& Client::options() const {
    // Ссылка на поле под защитой: менять options можно только через initialize(),
    // а он вызывается до начала работы, поэтому гонки здесь нет.
    std::shared_lock<std::shared_mutex> lock(impl_->mutex);
    return impl_->options;
}

DevicePtr Client::connect(const DeviceAddress& address, Status* status) {
    return connect(address.host.empty() ? std::string() : address.to_string(options().default_port),
                   status);
}

DevicePtr Client::connect(const std::string& address, Status* status) {
    const auto set_status = [status](Status value) {
        if (status) *status = value;
    };

    if (!initialized()) {
        set_status(Status::NotInitialized);
        return nullptr;
    }

    const Options options = impl_->snapshot();
    const std::string serial = normalize_address(address, options.default_port);
    if (serial.empty()) {
        set_status(Status::InvalidArgument);
        internal::emit_log(LogLevel::Error, address, "cannot parse device address");
        return nullptr;
    }

    auto impl = std::make_unique<Device::Impl>();
    impl->address = address;
    impl->serial = serial;
    impl->connect_timeout = options.connect_timeout;
    impl->listener = std::make_unique<internal::FacadeListener>(serial);

    internal::emit_log(LogLevel::Debug, serial, "connecting");
    impl->device = AdbManager::instance().connectDevice(serial, impl->listener.get());
    if (!impl->device) {
        // Транспорт не создан — освобождать через disconnectDevice() нечего.
        impl->closed = true;
        set_status(Status::ConnectFailed);
        internal::emit_log(LogLevel::Error, serial, "failed to initiate connection");
        return nullptr;
    }

    const Status wait_status = impl->listener->wait_until_online(options.connect_timeout);
    if (wait_status != Status::Ok) {
        set_status(wait_status);
        internal::emit_log(LogLevel::Error, serial,
                           std::string("connect failed: ") + to_string(wait_status));
        impl->close();  // отпускаем транспорт, иначе он останется висеть в adb
        return nullptr;
    }

    // Серийник мог уточниться при рукопожатии.
    impl->serial = impl->listener->serial();
    internal::emit_log(LogLevel::Info, impl->serial, "connected");

    DevicePtr device = internal::DeviceFactory::create(std::move(impl));
    impl_->remember(device);
    set_status(Status::Ok);
    return device;
}

void Client::for_each(const std::vector<std::string>& addresses,
                      const std::function<void(const DevicePtr& device,
                                               const std::string& address, Status status)>& task) {
    if (!initialized()) {
        for (const auto& address : addresses) task(nullptr, address, Status::NotInitialized);
        return;
    }
    if (!task) return;

    // Ограничение параллелизма живёт в AdbManager: подключение к очередному
    // устройству происходит внутри задачи, т.е. только когда освободился воркер.
    AdbManager::instance().runOnDevices(addresses, [&](const std::string& address) {
        Status status = Status::Ok;
        DevicePtr device = connect(address, &status);
        task(device, address, status);
        // Закрываем до возврата из задачи: иначе воркер возьмёт следующее
        // устройство, а транспорт этого останется занятым и лимит потеряет смысл.
        if (device) device->close();
    });
}

// Общий каркас для *_all: каждая операция отличается только вызовом на Device.
namespace {

std::map<std::string, Result> run_all(
    Client& client, const std::vector<std::string>& addresses,
    const std::function<Result(Device&)>& operation) {
    std::map<std::string, Result> results;
    std::mutex results_mutex;

    client.for_each(addresses, [&](const DevicePtr& device, const std::string& address,
                                   Status status) {
        Result result;
        if (!device) {
            result = make_error_result(status, std::string("connect failed: ") + to_string(status));
        } else {
            result = operation(*device);
        }
        std::lock_guard<std::mutex> lock(results_mutex);
        results.emplace(address, std::move(result));
    });
    return results;
}

}  // namespace

std::map<std::string, Result> Client::push_all(const std::vector<std::string>& addresses,
                                               const std::string& local, const std::string& remote,
                                               const PushOptions& options) {
    return run_all(*this, addresses,
                   [&](Device& device) { return device.push(local, remote, options); });
}

std::map<std::string, Result> Client::pull_all(const std::vector<std::string>& addresses,
                                               const std::string& remote,
                                               const std::string& local_dir,
                                               const PullOptions& options) {
    return run_all(*this, addresses, [&](Device& device) {
        // Файлы с разных устройств не должны затирать друг друга, поэтому
        // раскладываем их по подкаталогам <local_dir>/<serial>/ — так же,
        // как это делает adirect.
        std::string target = local_dir;
        if (!target.empty() && target.back() != '/') target += '/';
        target += device.serial();
        target += '/';
        return device.pull(remote, target, options);
    });
}

std::map<std::string, Result> Client::shell_all(const std::vector<std::string>& addresses,
                                                const std::string& command,
                                                const ShellOptions& options) {
    return run_all(*this, addresses,
                   [&](Device& device) { return device.shell(command, options); });
}

std::map<std::string, Result> Client::install_all(const std::vector<std::string>& addresses,
                                                  const std::string& apk_path,
                                                  const InstallOptions& options) {
    return run_all(*this, addresses,
                   [&](Device& device) { return device.install(apk_path, options); });
}

std::map<std::string, Result> Client::uninstall_all(const std::vector<std::string>& addresses,
                                                    const std::string& package,
                                                    const UninstallOptions& options) {
    return run_all(*this, addresses,
                   [&](Device& device) { return device.uninstall(package, options); });
}

void Client::set_log_options(const LogOptions& options) {
    {
        std::unique_lock<std::shared_mutex> lock(impl_->mutex);
        impl_->options.log = options;
    }
    libadb::set_log_options(options);
}

void Client::set_log_level(LogLevel level) {
    {
        std::unique_lock<std::shared_mutex> lock(impl_->mutex);
        impl_->options.log.level = level;
    }
    libadb::set_log_level(level);
}

void Client::set_log_sink(LogSink sink) {
    {
        std::unique_lock<std::shared_mutex> lock(impl_->mutex);
        impl_->options.log_sink = sink;
    }
    libadb::set_log_sink(std::move(sink));
}

void Client::close_all() {
    impl_->close_all();
}

void Client::shutdown() {
    impl_->close_all();
    {
        std::unique_lock<std::shared_mutex> lock(impl_->mutex);
        if (!impl_->initialized) return;
        impl_->initialized = false;
    }
    internal::emit_log(LogLevel::Debug, "", "shutting down");
    AdbManager::instance().stop();
    libadb::flush_log();
}

}  // namespace libadb
