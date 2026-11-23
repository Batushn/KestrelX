#include "kestrel/formats/sgd5.hpp"

#include <windows.h>
#include <compressapi.h>
#pragma comment(lib, "cabinet.lib")

#include <algorithm>
#include <cstring>

namespace kestrel::formats {

namespace {

constexpr size_t MAX_UNPACKED_SIZE = 64 * 1024 * 1024; // 64 MiB limit

uint32_t read_u32_le(const uint8_t* p) {
    uint32_t val;
    std::memcpy(&val, p, sizeof(val));
    return val;
}

void write_u32_le(uint8_t* p, uint32_t v) {
    std::memcpy(p, &v, sizeof(v));
}

}  // namespace

std::optional<std::vector<uint8_t>> Sgd5Record::unpack() const {
    if (unpacked_size == 0) return std::vector<uint8_t>{};
    if (unpacked_size > MAX_UNPACKED_SIZE) return std::nullopt;
    if (packed_payload.empty()) return std::nullopt;

    const uint8_t* in_ptr = packed_payload.data();
    const size_t in_size = packed_payload.size();

    // 1. Check for RFC 1951 uncompressed DEFLATE block (BFINAL=1, BTYPE=00)
    // Format: 0x01, LEN (u16), NLEN (u16), followed by LEN bytes
    if (in_size >= 5 && (in_ptr[0] & 0x07) == 0x01) {
        const uint16_t len = static_cast<uint16_t>(in_ptr[1] | (in_ptr[2] << 8));
        const uint16_t nlen = static_cast<uint16_t>(in_ptr[3] | (in_ptr[4] << 8));
        if ((len ^ nlen) == 0xFFFF && in_size >= 5 + len && len == unpacked_size) {
            std::vector<uint8_t> out(in_ptr + 5, in_ptr + 5 + len);
            return out;
        }
    }

    // 2. Check for MSZIP Cabinet compressed stream (starts with 0x43, 0x4B 'CK')
    std::vector<uint8_t> mszip_buf;
    if (in_size >= 2 && in_ptr[0] == 0x43 && in_ptr[1] == 0x4B) {
        // Already MSZIP prefixed
    } else {
        mszip_buf.resize(in_size + 2);
        mszip_buf[0] = 0x43;
        mszip_buf[1] = 0x4B;
        std::memcpy(mszip_buf.data() + 2, in_ptr, in_size);
        in_ptr = mszip_buf.data();
    }

    DECOMPRESSOR_HANDLE decomp = nullptr;
    if (CreateDecompressor(COMPRESS_ALGORITHM_MSZIP, nullptr, &decomp)) {
        std::vector<uint8_t> out(unpacked_size);
        SIZE_T actual = 0;
        const size_t effective_in_size = mszip_buf.empty() ? in_size : mszip_buf.size();
        if (Decompress(decomp, const_cast<uint8_t*>(in_ptr), effective_in_size, out.data(), out.size(), &actual)) {
            CloseDecompressor(decomp);
            if (actual == unpacked_size) return out;
        }
        CloseDecompressor(decomp);
    }

    return std::nullopt;
}

bool Sgd5Archive::is_sgd5(std::span<const uint8_t> data) {
    if (data.size() < 4) return false;
    for (size_t i = 0; i + 4 <= data.size(); ++i) {
        if (read_u32_le(data.data() + i) == SGD5_MAGIC) {
            return true;
        }
    }
    return false;
}

std::vector<Sgd5Record> Sgd5Archive::parse_records(std::span<const uint8_t> data) {
    std::vector<Sgd5Record> records;
    if (data.size() < 24) return records;

    const uint8_t* bytes = data.data();
    const size_t len = data.size();

    for (size_t at = 6; at + 12 <= len; ++at) {
        if (read_u32_le(bytes + at) != SGD5_RECORD_MARKER) {
            continue;
        }

        const uint32_t n_pre = read_u32_le(bytes + at - 4);
        const uint32_t n_post = read_u32_le(bytes + at + 4);
        const uint32_t unpacked_sz = read_u32_le(bytes + at + 8);

        if (n_pre != n_post || n_pre < 12 || unpacked_sz > MAX_UNPACKED_SIZE) {
            continue;
        }

        if (at + n_pre > len) {
            continue;
        }

        // Byte at -5 is kind
        const uint8_t kind_byte = bytes[at - 5];
        // Byte at -6 should be NUL ending the name string
        if (bytes[at - 6] != 0) {
            continue;
        }

        // Search backward for name length byte
        std::string record_name;
        const size_t nul_pos = at - 6;
        for (size_t p = (nul_pos > 128 ? nul_pos - 128 : 0); p < nul_pos; ++p) {
            const uint8_t str_len = bytes[p];
            if (str_len > 0 && (p + str_len == nul_pos)) {
                bool valid = true;
                for (size_t s = p + 1; s < nul_pos; ++s) {
                    if (bytes[s] < 0x20 || bytes[s] > 0x7E) {
                        valid = false;
                        break;
                    }
                }
                if (valid) {
                    record_name = std::string(reinterpret_cast<const char*>(bytes + p + 1), str_len - 1);
                    break;
                }
            }
        }

        Sgd5Record rec;
        rec.offset = at;
        rec.name = std::move(record_name);
        rec.kind = static_cast<Sgd5ShaderKind>(kind_byte);
        rec.packed_size = n_pre - 12;
        rec.unpacked_size = unpacked_sz;
        rec.packed_payload = std::span<const uint8_t>(bytes + at + 12, rec.packed_size);

        records.push_back(std::move(rec));
        at += n_pre - 1; // Advance past this record
    }

    return records;
}

std::vector<uint8_t> Sgd5Archive::synthesize_archive(
    const std::vector<std::tuple<std::string, uint8_t, std::vector<uint8_t>>>& items
) {
    std::vector<uint8_t> archive;
    // Magic: "SGD5"
    archive.resize(4);
    write_u32_le(archive.data(), SGD5_MAGIC);
    // Header padding
    archive.insert(archive.end(), {0x01, 0x00, 0x00, 0x00});

    for (const auto& [name, kind, raw_bytes] : items) {
        // Format as RFC 1951 raw uncompressed DEFLATE block
        std::vector<uint8_t> raw_deflate;
        if (raw_bytes.size() <= 65535) {
            const uint16_t len = static_cast<uint16_t>(raw_bytes.size());
            const uint16_t nlen = static_cast<uint16_t>(~len);
            raw_deflate.resize(5 + len);
            raw_deflate[0] = 0x01; // BFINAL=1, BTYPE=00 (uncompressed block)
            raw_deflate[1] = static_cast<uint8_t>(len & 0xFF);
            raw_deflate[2] = static_cast<uint8_t>((len >> 8) & 0xFF);
            raw_deflate[3] = static_cast<uint8_t>(nlen & 0xFF);
            raw_deflate[4] = static_cast<uint8_t>((nlen >> 8) & 0xFF);
            std::memcpy(raw_deflate.data() + 5, raw_bytes.data(), len);
        } else {
            raw_deflate = raw_bytes;
        }

        // Pre-record name preamble: [length_with_nul] [name] [0x00] [kind]
        const uint8_t name_len_with_nul = static_cast<uint8_t>(name.size() + 1);
        archive.push_back(name_len_with_nul);
        archive.insert(archive.end(), name.begin(), name.end());
        archive.push_back(0x00);
        archive.push_back(kind);

        // Record header:
        // u32 n = packed_len + 12
        // u32 marker = 0x000DDCE7
        // u32 n = packed_len + 12
        // u32 unpacked_size
        const uint32_t n = static_cast<uint32_t>(raw_deflate.size() + 12);
        const uint32_t marker = SGD5_RECORD_MARKER;
        const uint32_t unpacked_sz = static_cast<uint32_t>(raw_bytes.size());

        const size_t cur = archive.size();
        archive.resize(cur + 16);
        write_u32_le(archive.data() + cur, n);
        write_u32_le(archive.data() + cur + 4, marker);
        write_u32_le(archive.data() + cur + 8, n);
        write_u32_le(archive.data() + cur + 12, unpacked_sz);

        archive.insert(archive.end(), raw_deflate.begin(), raw_deflate.end());
    }

    return archive;
}

}  // namespace kestrel::formats
