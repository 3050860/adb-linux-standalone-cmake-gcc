/*
 * libadb — внутреннее устройство фасада Device.
 *
 * Здесь живёт мост между публичным API и внутренними классами adb
 * (AdbManager/AdbDevice/AdbSession). Заголовок не устанавливается.
 */
#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>


#include "AdbDevice.h"
#include "AdbManager.h"
#include "IadbListener.h"
#include "api/internal.h"
#include "file_sync_protocol.h"
#include "libadb/libadb.h"

namespace libadb::internal {

class OperationContext;

// Куда складывать вывод текущей операции. Устанавливается на время shell/install
// и снимается по завершении: listener живёт дольше отдельной операции.
struct OutputTarget {
    std::string* buffer = nullptr;  // nullptr, если capture_output == false
    const OutputFn* callback = nullptr;
    // Операция, от имени которой публикуются события OperationOutput (§8).
    // nullptr — вывод в события не транслируется.
    OperationContext* op = nullptr;
};

// Слушатель событий одного устройства. Все методы вызываются из потоков adb
// (fdevent loop и reader-поток сессии), поэтому состояние под мьютексом.
class LIBADB_INTERNAL FacadeListener : public IDeviceListener {
  public:
    explicit FacadeListener(std::string serial) : serial_(std::move(serial)) {}

    // --- IDeviceListener ---
    void onConnectionStateChanged(const std::string& serial, ConnectionState state) override;
    void onAuthRequired(const std::string& serial) override;
    void onShellData(const std::string& serial, uint32_t session_id, const char* data, size_t len,
                     bool is_stderr) override;
    void onSessionClosed(const std::string& serial, uint32_t session_id, int exit_code) override;
    void onError(const std::string& serial, const std::string& error_msg) override;

    // Ждёт перехода в состояние device. Возвращает Status::Ok либо причину отказа.
    Status wait_until_online(ms timeout);

    // Управление приёмником вывода текущей операции.
    void set_output_target(const OutputTarget& target);
    void clear_output_target();

    // Последняя ошибка от устройства (для заполнения Result::error).
    std::string take_error();

    bool online() const;

    // Серийник может уточниться после подключения (добавится порт).
    void set_serial(std::string serial);
    std::string serial() const;

  private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::string serial_;

    bool online_ = false;
    bool auth_required_ = false;
    bool failed_ = false;
    std::string error_;

    std::mutex output_mutex_;
    OutputTarget output_;
};

}  // namespace libadb::internal

namespace libadb {

// Определение PIMPL-части Device: нужно и device.cpp, и client.cpp (тот создаёт объекты).
// LIBADB_INTERNAL: имена Device::Impl подпадают под шаблон _ZN6libadb* в
// libadb.map, поэтому видимость гасим явно — методы PIMPL наружу не нужны.
struct LIBADB_INTERNAL Device::Impl {
    std::string address;  // как его передал вызывающий, для сообщений и ключей результата
    std::string serial;   // "ip:port" — то, чем оперирует внутренний adb
    std::shared_ptr<AdbDevice> device;
    std::unique_ptr<internal::FacadeListener> listener;
    ms connect_timeout{15000};
    bool closed = false;

    // Возврат слота подключения в пул клиента (§7). Задаётся в Client::connect
    // сразу после захвата слота; вызывается ровно один раз при закрытии.
    std::function<void()> release_slot;

    ~Impl();

    // Закрывает подключение и отпускает транспорт (idempotent).
    void close();

    // Выполняет shell-команду. op != nullptr — события публикуются от имени
    // уже начатой операции (например, uninstall, который работает через shell
    // и не должен создавать вторую операцию).
    // timeout: 0 — ждать бесконечно; по истечении сессия прерывается и
    // возвращается Status::CommandTimeout.
    Result run_shell(const std::string& command, const ShellOptions& options,
                     internal::OperationContext* op, ms timeout);

    // Одна попытка установки (§10): запускает pm в рабочем потоке и следит за
    // фазами/таймаутами из вызывающего. Повторы (ConflictPolicy::Reinstall,
    // allow_downgrade_retry) организует Device::install поверх этого метода.
    // paths уже развёрнуты (bundle распакован), flags — готовые аргументы pm.
    Result run_install_attempt(const std::vector<std::string>& paths,
                               const std::vector<std::string>& flags, bool multi_package,
                               uint64_t total_size, const InstallOptions& options,
                               const InstallTimeout& timeout, internal::OperationContext& op);

    // Занято ли устройство асинхронной операцией. Синхронные вызовы этот флаг
    // не выставляют: их последовательность — забота вызывающего.
    std::atomic<bool> async_busy{false};
};

namespace internal {

// Создаёт Device: конструктор приватный, а фабрика объявлена friend'ом в libadb.h.
struct LIBADB_INTERNAL DeviceFactory {
    static DevicePtr create(std::unique_ptr<Device::Impl> impl);
};


// Публичное перечисление → внутреннее.
LIBADB_INTERNAL CompressionType to_compression_type(Compression compression);

}  // namespace internal
}  // namespace libadb
