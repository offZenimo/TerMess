#pragma once

#include <charconv>
#include <cstddef>
#include <string>
#include <string_view>

namespace protocol {

inline constexpr int defaultPort = 5555;
inline constexpr std::size_t maxLineLength = 1024;
inline constexpr std::size_t maxNameLength = 24;

inline int parsePort(std::string_view text) {
    int port = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), port);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
        port < 1 || port > 65535) {
        return 0;
    }
    return port;
}

inline bool isValidName(std::string_view name) {
    if (name.empty() || name.size() > maxNameLength) {
        return false;
    }
    for (char character : name) {
        if (!((character >= 'a' && character <= 'z') ||
              (character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9') || character == '_')) {
            return false;
        }
    }
    return true;
}

inline void trimCarriageReturn(std::string& line) {
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
}

}  // namespace protocol
