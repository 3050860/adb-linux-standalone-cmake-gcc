#include "AdbFileSync.h"
#include "file_sync_lib_client.h"
#include <unistd.h>

namespace {
// Ставит наблюдателя на время передачи и снимает при выходе (в том числе по
// раннему return): наблюдатель принадлежит вызывающему и живёт короче потока.
class ScopedSyncObserver {
public:
    explicit ScopedSyncObserver(const SyncProgressObserver* observer)
        : previous_(adb_sync_set_observer(observer)) {}
    ~ScopedSyncObserver() { adb_sync_set_observer(previous_); }
private:
    const SyncProgressObserver* previous_;
};
}  // namespace

bool AdbFileSync::push(const std::vector<std::string>& local_paths, const std::string& remote_path, 
                       bool sync, CompressionType compression, bool quiet,
                       const SyncProgressObserver* observer, uint64_t* bytes_transferred) {
    if (local_paths.empty()) return false;
    ScopedSyncObserver scoped(observer);

    // 1. Создаем сессию sync
    auto session = device_->createSession("sync:");
    if (!session || !session->start(false)) return false;

    // 2. Получаем FeatureSet устройства
    auto features = device_->getFeatures();

    // 3. Дублируем FD, чтобы SyncConnection мог управлять своей копией, 
    // не закрывая сокет у AdbSession (который нужен до конца передачи)
    int raw_fd = dup(session->getFd());
    if (raw_fd < 0) return false;
    unique_fd sync_fd(raw_fd);

    // 4. Преобразуем std::string в vector<const char*>
    std::vector<const char*> c_paths;
    for (const auto& path : local_paths) {
        c_paths.push_back(path.c_str());
    }

    // 5. Вызываем оригинальную, проверенную логику Google
    return ::do_sync_push_fd(std::move(sync_fd), features, c_paths, remote_path.c_str(), sync, compression, false, quiet,
                             bytes_transferred);
}

bool AdbFileSync::pull(const std::vector<std::string>& remote_paths, const std::string& local_path, 
                       bool copy_attrs, CompressionType compression, bool quiet,
                       const SyncProgressObserver* observer, uint64_t* bytes_transferred) {
    if (remote_paths.empty()) return false;
    ScopedSyncObserver scoped(observer);

    auto session = device_->createSession("sync:");
    if (!session || !session->start(false)) return false;

    auto features = device_->getFeatures();
    int raw_fd = dup(session->getFd());
    if (raw_fd < 0) return false;
    unique_fd sync_fd(raw_fd);

    std::vector<const char*> c_paths;
    for (const auto& path : remote_paths) {
        c_paths.push_back(path.c_str());
    }

    return ::do_sync_pull_fd(std::move(sync_fd), features, c_paths, local_path.c_str(), copy_attrs, compression, nullptr, quiet,
                             bytes_transferred);
}