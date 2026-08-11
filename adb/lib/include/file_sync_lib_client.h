#pragma once

#include <string>
#include <vector>

#include "file_sync_protocol.h"
#include "transport.h"

bool do_sync_ls(const char* path);
bool do_sync_push(const std::vector<const char*>& srcs, const char* dst, bool sync,
                  CompressionType compression, bool dry_run, bool quiet);
bool do_sync_pull(const std::vector<const char*>& srcs, const char* dst, bool copy_attrs,
                  CompressionType compression, const char* name = nullptr, bool quiet = false);

// bool do_sync_sync(const std::string& lpath, const std::string& rpath, bool list_only,
//                   CompressionType compression, bool dry_run, bool quiet);

// bytes_transferred (если задан) — сколько полезных байт реально прошло за эту
// операцию: SyncConnection знает это точнее, чем вызывающий по размерам файлов
// (пропущенные файлы при sync=true, каталоги, символические ссылки).
bool do_sync_push_fd(unique_fd fd, const FeatureSet& features, const std::vector<const char*>& srcs, const char* dst, bool sync,
                     CompressionType compression, bool dry_run, bool quiet,
                     uint64_t* bytes_transferred = nullptr);
bool do_sync_pull_fd(unique_fd fd, const FeatureSet& features, const std::vector<const char*>& srcs, const char* dst, bool copy_attrs,
                     CompressionType compression, const char* name, bool quiet,
                     uint64_t* bytes_transferred = nullptr);