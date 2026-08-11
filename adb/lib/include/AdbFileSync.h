#pragma once
#include <string>
#include <vector>
#include <memory>
#include "AdbDevice.h"
#include "file_sync_protocol.h"
#include "sync_progress.h"

class AdbFileSync {
public:
    explicit AdbFileSync(std::shared_ptr<AdbDevice> device) : device_(device) {}

    // observer — необязательный наблюдатель за передачей (прогресс, байты в
    // сети, отмена); ставится только на время вызова.
    // bytes_transferred — сколько полезных байт реально прошло (может быть nullptr).
    bool push(const std::vector<std::string>& local_paths, const std::string& remote_path, 
              bool sync = false, CompressionType compression = CompressionType::Any, bool quiet = false,
              const SyncProgressObserver* observer = nullptr, uint64_t* bytes_transferred = nullptr);
              
    bool pull(const std::vector<std::string>& remote_paths, const std::string& local_path, 
              bool copy_attrs = false, CompressionType compression = CompressionType::None, bool quiet = false,
              const SyncProgressObserver* observer = nullptr, uint64_t* bytes_transferred = nullptr);

private:
    std::shared_ptr<AdbDevice> device_;
};
