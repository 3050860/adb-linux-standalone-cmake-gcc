// libadb: разбор и форматирование адреса устройства.
#include <charconv>

#include "libadb/libadb.h"

namespace libadb {

DeviceAddress::DeviceAddress(std::string host, uint16_t port)
    : host(std::move(host)), port(port) {}

std::optional<DeviceAddress> DeviceAddress::parse(std::string_view text) {
    // Обрезаем пробелы по краям.
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' ||
                             text.back() == '\r' || text.back() == '\n')) {
        text.remove_suffix(1);
    }
    if (text.empty()) return std::nullopt;

    // IPv6 в квадратных скобках: [::1]:5555
    if (text.front() == '[') {
        const size_t close = text.find(']');
        if (close == std::string_view::npos || close == 1) return std::nullopt;
        const std::string_view host_part = text.substr(1, close - 1);
        std::string_view rest = text.substr(close + 1);
        if (rest.empty()) return DeviceAddress(std::string(host_part), 0);
        if (rest.front() != ':') return std::nullopt;
        rest.remove_prefix(1);
        uint32_t value = 0;
        const auto res = std::from_chars(rest.data(), rest.data() + rest.size(), value);
        if (res.ec != std::errc{} || res.ptr != rest.data() + rest.size()) return std::nullopt;
        if (value == 0 || value > 65535) return std::nullopt;
        return DeviceAddress(std::string(host_part), static_cast<uint16_t>(value));
    }

    const size_t colon = text.rfind(':');
    if (colon == std::string_view::npos) {
        return DeviceAddress(std::string(text), 0);
    }

    const std::string_view host_part = text.substr(0, colon);
    const std::string_view port_part = text.substr(colon + 1);
    if (host_part.empty() || port_part.empty()) return std::nullopt;

    uint32_t value = 0;
    const auto res =
        std::from_chars(port_part.data(), port_part.data() + port_part.size(), value);
    if (res.ec != std::errc{} || res.ptr != port_part.data() + port_part.size()) {
        return std::nullopt;
    }
    if (value == 0 || value > 65535) return std::nullopt;

    return DeviceAddress(std::string(host_part), static_cast<uint16_t>(value));
}

std::string DeviceAddress::to_string(uint16_t default_port) const {
    const uint16_t effective = port != 0 ? port : default_port;
    const bool needs_brackets =
        host.find(':') != std::string::npos && host.front() != '[';
    std::string out;
    out.reserve(host.size() + 8);
    if (needs_brackets) out.push_back('[');
    out.append(host);
    if (needs_brackets) out.push_back(']');
    out.push_back(':');
    out.append(std::to_string(effective));
    return out;
}

}  // namespace libadb
