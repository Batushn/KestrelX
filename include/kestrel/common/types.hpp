#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace kestrel {

struct Error {
    std::string message;
};

template <typename T>
using Result = std::expected<T, Error>;

inline std::unexpected<Error> fail(std::string msg) {
    return std::unexpected<Error>(Error{std::move(msg)});
}

std::string hex32(uint32_t v);
std::string hex64(uint64_t v);
std::string to_lower_ascii(std::string_view s);
std::wstring utf8_to_wide(std::string_view s);
std::string wide_to_utf8(std::wstring_view w);
Result<std::vector<uint8_t>> read_file_bytes(const std::filesystem::path& path);
std::string json_escape(std::string_view s);

}  // namespace kestrel
