// libadb: текстовые представления статусов, команд и фаз.
#include "libadb/libadb.h"

namespace libadb {

const char* to_string(Status s) {
    switch (s) {
        case Status::Ok:                  return "Ok";
        case Status::InvalidArgument:     return "InvalidArgument";
        case Status::NotInitialized:      return "NotInitialized";
        case Status::NotImplemented:      return "NotImplemented";
        case Status::Unsupported:         return "Unsupported";
        case Status::Internal:            return "Internal";
        case Status::ConnectFailed:       return "ConnectFailed";
        case Status::ConnectTimeout:      return "ConnectTimeout";
        case Status::AuthRequired:        return "AuthRequired";
        case Status::Unauthorized:        return "Unauthorized";
        case Status::Offline:             return "Offline";
        case Status::DeviceLost:          return "DeviceLost";
        case Status::ConnectionClosed:    return "ConnectionClosed";
        case Status::SlotBusy:            return "SlotBusy";
        case Status::SlotTimeout:         return "SlotTimeout";
        case Status::DeviceBusy:          return "DeviceBusy";
        case Status::CommandTimeout:      return "CommandTimeout";
        case Status::StallTimeout:        return "StallTimeout";
        case Status::Canceled:            return "Canceled";
        case Status::IoError:             return "IoError";
        case Status::LocalFileError:      return "LocalFileError";
        case Status::RemoteError:         return "RemoteError";
        case Status::SignatureMismatch:   return "SignatureMismatch";
        case Status::VersionDowngrade:    return "VersionDowngrade";
        case Status::InsufficientStorage: return "InsufficientStorage";
        case Status::InvalidApk:          return "InvalidApk";
        case Status::MissingSplit:        return "MissingSplit";
    }
    return "Unknown";
}

const char* to_string(Command c) {
    switch (c) {
        case Command::Connect:      return "connect";
        case Command::Shell:        return "shell";
        case Command::Push:         return "push";
        case Command::Pull:         return "pull";
        case Command::Install:      return "install";
        case Command::Uninstall:    return "uninstall";
        case Command::ShellSession: return "shell-session";
    }
    return "unknown";
}

const char* to_string(Phase p) {
    switch (p) {
        case Phase::None:          return "none";
        case Phase::Connecting:    return "connecting";
        case Phase::Prepare:       return "prepare";
        case Phase::CreateSession: return "create-session";
        case Phase::Transfer:      return "transfer";
        case Phase::Commit:        return "commit";
        case Phase::Finalize:      return "finalize";
        case Phase::Abandon:       return "abandon";
    }
    return "unknown";
}

}  // namespace libadb
