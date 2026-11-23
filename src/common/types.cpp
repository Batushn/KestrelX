#include "kestrel/common/types.hpp"

#include <Windows.h>
#include <algorithm>
#include <fstream>

namespace kestrel {

std::string hex32(uint32_t v) {
    return std::format("0x{:08X}", v);
}

std::string hex64(uint64_t v) {
    return std::format("0x{:016X}", v);
}

std::string to_lower_ascii(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

std::wstring utf8_to_wide(std::string_view s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len);
    return out;
}

std::string wide_to_utf8(std::wstring_view w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), len, nullptr, nullptr);
    return out;
}

Result<std::vector<uint8_t>> read_file_bytes(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        return fail("Failed to open file: " + path.string());
    }
    const auto size = f.tellg();
    if (size < 0) {
        return fail("Failed to determine size of file: " + path.string());
    }
    std::vector<uint8_t> buf(static_cast<size_t>(size));
    f.seekg(0, std::ios::beg);
    if (size > 0 && !f.read(reinterpret_cast<char*>(buf.data()), size)) {
        return fail("Failed to read file: " + path.string());
    }
    return buf;
}

std::string json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) out += std::format("\\u{:04x}", c);
                else out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

}  // namespace kestrel
