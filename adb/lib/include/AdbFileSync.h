#pragma once
#include <string>
#include <vector>
#include <memory>
#include "AdbDevice.h"
#include "file_sync_protocol.h"

class AdbFileSync {
public:
    explicit AdbFileSync(std::shared_ptr<AdbDevice> device) : device_(device) {}

    bool push(const std::vector<std::string>& local_paths, const std::string& remote_path, 
              bool sync = false, CompressionType compression = CompressionType::Any, bool quiet = false);
              
    bool pull(const std::vector<std::string>& remote_paths, const std::string& local_path, 
              bool copy_attrs = false, CompressionType compression = CompressionType::None, bool quiet = false);

private:
    std::shared_ptr<AdbDevice> device_;
};
