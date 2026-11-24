#include "kestrel/formats/gdk_container.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>

#pragma comment(lib, "bcrypt.lib")

namespace kestrel::formats {

namespace {

constexpr size_t kHeaderSize = 64;
constexpr uint64_t kMaxPayloadBytes = 512ull * 1024ull * 1024ull;

uint16_t read_u16(const uint8_t* p) {
    uint16_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

uint32_t read_u32(const uint8_t* p) {
    uint32_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

uint64_t read_u64(const uint8_t* p) {
    uint64_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

void write_u16(uint8_t* p, uint16_t v) { std::memcpy(p, &v, sizeof(v)); }
void write_u32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, sizeof(v)); }
void write_u64(uint8_t* p, uint64_t v) { std::memcpy(p, &v, sizeof(v)); }

// Increment big-endian 128-bit AES-CTR counter block by `block_delta`.
GdkIvNonce add_ctr_blocks(const GdkIvNonce& base_iv, uint64_t block_delta) {
    GdkIvNonce ctr = base_iv;
    uint64_t carry = block_delta;
    for (int i = 15; i >= 0 && carry > 0; --i) {
        const uint64_t sum = static_cast<uint64_t>(ctr[static_cast<size_t>(i)]) + (carry & 0xFFu);
        ctr[static_cast<size_t>(i)] = static_cast<uint8_t>(sum & 0xFFu);
        carry = (carry >> 8) + (sum >> 8);
    }
    return ctr;
}

}  // namespace

uint32_t compute_crc32(std::span<const uint8_t> data) {
    uint32_t crc = 0xFFFFFFFFu;
    for (uint8_t b : data) {
        crc ^= b;
        for (int i = 0; i < 8; ++i) {
            const uint32_t mask = static_cast<uint32_t>(-static_cast<int32_t>(crc & 1u));
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

std::string GdkKeyRing::key_id_hex(const GdkKeyId& id) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (uint8_t b : id) {
        out.push_back(kHex[(b >> 4) & 0xFu]);
        out.push_back(kHex[b & 0xFu]);
    }
    return out;
}

void GdkKeyRing::register_key(const GdkKeyId& key_id, const GdkAes128Key& key) {
    std::lock_guard lock(mtx_);
    keys_[key_id_hex(key_id)] = key;
}

std::optional<GdkAes128Key> GdkKeyRing::find_key(const GdkKeyId& key_id) const {
    std::lock_guard lock(mtx_);
    auto it = keys_.find(key_id_hex(key_id));
    if (it == keys_.end()) return std::nullopt;
    return it->second;
}

void GdkKeyRing::remove_key(const GdkKeyId& key_id) {
    std::lock_guard lock(mtx_);
    keys_.erase(key_id_hex(key_id));
}

void GdkKeyRing::clear() {
    std::lock_guard lock(mtx_);
    keys_.clear();
}

size_t GdkKeyRing::size() const {
    std::lock_guard lock(mtx_);
    return keys_.size();
}

Result<GdkAes128Key> GdkKeyRing::derive_key_from_token(std::string_view license_token, const GdkKeyId& key_id) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    auto cleanup = [&]() {
        if (hash) BCryptDestroyHash(hash);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    };

    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
        cleanup();
        return fail("BCryptOpenAlgorithmProvider(SHA256) failed");
    }

    DWORD obj_len = 0;
    DWORD cb_data = 0;
    if (!BCRYPT_SUCCESS(BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH,
                                          reinterpret_cast<PUCHAR>(&obj_len), sizeof(obj_len), &cb_data, 0))) {
        cleanup();
        return fail("BCryptGetProperty(BCRYPT_OBJECT_LENGTH) failed");
    }

    std::vector<uint8_t> hash_obj(obj_len);
    if (!BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, hash_obj.data(), obj_len, nullptr, 0, 0))) {
        cleanup();
        return fail("BCryptCreateHash failed");
    }

    if (!license_token.empty()) {
        if (!BCRYPT_SUCCESS(BCryptHashData(
                hash, reinterpret_cast<PUCHAR>(const_cast<char*>(license_token.data())),
                static_cast<ULONG>(license_token.size()), 0))) {
            cleanup();
            return fail("BCryptHashData(token) failed");
        }
    }
    if (!BCRYPT_SUCCESS(BCryptHashData(
            hash, reinterpret_cast<PUCHAR>(const_cast<uint8_t*>(key_id.data())),
            static_cast<ULONG>(key_id.size()), 0))) {
        cleanup();
        return fail("BCryptHashData(key_id) failed");
    }

    std::array<uint8_t, 32> digest{};
    if (!BCRYPT_SUCCESS(BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0))) {
        cleanup();
        return fail("BCryptFinishHash failed");
    }
    cleanup();

    GdkAes128Key derived{};
    std::memcpy(derived.data(), digest.data(), derived.size());
    return derived;
}

Result<std::vector<uint8_t>> aes128_ctr_crypt(
    const GdkAes128Key& key,
    const GdkIvNonce& iv_nonce,
    uint64_t stream_byte_offset,
    std::span<const uint8_t> input) {
    if (input.empty()) return std::vector<uint8_t>{};

    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE key_handle = nullptr;
    auto cleanup = [&]() {
        if (key_handle) BCryptDestroyKey(key_handle);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    };

    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0))) {
        cleanup();
        return fail("BCryptOpenAlgorithmProvider(AES) failed");
    }

    if (!BCRYPT_SUCCESS(BCryptSetProperty(
            alg, BCRYPT_CHAINING_MODE,
            reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_ECB)),
            static_cast<ULONG>(sizeof(BCRYPT_CHAIN_MODE_ECB)), 0))) {
        cleanup();
        return fail("BCryptSetProperty(ECB) failed");
    }

    DWORD key_obj_len = 0;
    DWORD cb_data = 0;
    if (!BCRYPT_SUCCESS(BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH,
                                          reinterpret_cast<PUCHAR>(&key_obj_len), sizeof(key_obj_len), &cb_data, 0))) {
        cleanup();
        return fail("BCryptGetProperty(key_obj_len) failed");
    }

    std::vector<uint8_t> key_obj(key_obj_len);
    if (!BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(
            alg, &key_handle, key_obj.data(), key_obj_len,
            const_cast<PUCHAR>(key.data()), static_cast<ULONG>(key.size()), 0))) {
        cleanup();
        return fail("BCryptGenerateSymmetricKey failed");
    }

    const uint64_t first_block = stream_byte_offset / 16ull;
    const size_t first_block_skip = static_cast<size_t>(stream_byte_offset % 16ull);
    const size_t total_blocks = (first_block_skip + input.size() + 15ull) / 16ull;

    std::vector<uint8_t> ctr_blocks(total_blocks * 16ull, 0);
    for (size_t b = 0; b < total_blocks; ++b) {
        const GdkIvNonce ctr = add_ctr_blocks(iv_nonce, first_block + b);
        std::memcpy(ctr_blocks.data() + b * 16ull, ctr.data(), 16);
    }

    std::vector<uint8_t> keystream(ctr_blocks.size(), 0);
    ULONG out_len = 0;
    if (!BCRYPT_SUCCESS(BCryptEncrypt(
            key_handle,
            ctr_blocks.data(), static_cast<ULONG>(ctr_blocks.size()),
            nullptr, nullptr, 0,
            keystream.data(), static_cast<ULONG>(keystream.size()),
            &out_len, 0))) {
        cleanup();
        return fail("BCryptEncrypt(AES-ECB keystream) failed");
    }
    cleanup();

    std::vector<uint8_t> out(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        out[i] = input[i] ^ keystream[first_block_skip + i];
    }
    return out;
}

bool GdkContentContainer::is_gdk_container(std::span<const uint8_t> data) {
    return data.size() >= kHeaderSize && read_u32(data.data()) == KXVC_MAGIC;
}

Result<std::vector<uint8_t>> GdkContentContainer::synthesize_container(
    uint32_t title_id,
    GdkCipherMode cipher,
    const GdkKeyId& key_id,
    const GdkIvNonce& iv_nonce,
    const std::optional<GdkAes128Key>& key,
    const std::vector<std::pair<std::string, std::vector<uint8_t>>>& segments) {
    if (cipher == GdkCipherMode::Aes128Ctr && !key.has_value()) {
        return fail("synthesize_container: Aes128Ctr requires a 128-bit key");
    }

    // Build segment directory + contiguous logical payload
    std::vector<uint8_t> dir_bytes;
    std::vector<uint8_t> plain_payload;

    for (const auto& [name, data] : segments) {
        if (name.empty() || name.size() > 4096) {
            return fail("synthesize_container: invalid segment name length");
        }
        const uint64_t off = plain_payload.size();
        const uint64_t sz = data.size();
        const uint32_t crc = compute_crc32(data);
        plain_payload.insert(plain_payload.end(), data.begin(), data.end());

        // Entry layout: u16 name_len + u64 logical_off + u64 logical_sz + u64 storage_off + u64 storage_sz + u32 crc + name
        const size_t pos = dir_bytes.size();
        dir_bytes.resize(pos + 38 + name.size());
        uint8_t* ep = dir_bytes.data() + pos;
        write_u16(ep + 0, static_cast<uint16_t>(name.size()));
        write_u64(ep + 2, off);
        write_u64(ep + 10, sz);
        write_u64(ep + 18, off);
        write_u64(ep + 26, sz);
        write_u32(ep + 34, crc);
        std::memcpy(ep + 38, name.data(), name.size());
    }

    std::vector<uint8_t> stored_payload;
    if (cipher == GdkCipherMode::None) {
        stored_payload = std::move(plain_payload);
    } else if (cipher == GdkCipherMode::Aes128Ctr) {
        auto enc = aes128_ctr_crypt(*key, iv_nonce, 0, plain_payload);
        if (!enc) return fail(enc.error().message);
        stored_payload = std::move(*enc);
    } else {
        return fail("synthesize_container: unsupported cipher mode");
    }

    const uint64_t payload_base = kHeaderSize + dir_bytes.size();
    std::vector<uint8_t> out(payload_base + stored_payload.size(), 0);
    uint8_t* h = out.data();
    write_u32(h + 0, KXVC_MAGIC);
    write_u16(h + 4, KXVC_VERSION);
    write_u16(h + 6, static_cast<uint16_t>(cipher));
    write_u32(h + 8, title_id);
    write_u32(h + 12, static_cast<uint32_t>(segments.size()));
    std::memcpy(h + 16, key_id.data(), 16);
    std::memcpy(h + 32, iv_nonce.data(), 16);
    write_u64(h + 48, payload_base);
    write_u64(h + 56, stored_payload.size());

    if (!dir_bytes.empty()) {
        std::memcpy(out.data() + kHeaderSize, dir_bytes.data(), dir_bytes.size());
    }
    if (!stored_payload.empty()) {
        std::memcpy(out.data() + payload_base, stored_payload.data(), stored_payload.size());
    }
    return out;
}

Result<GdkContentContainer> GdkContentContainer::parse(std::span<const uint8_t> blob) {
    if (blob.size() < kHeaderSize) {
        return fail("KXVC container truncated (< 64B header)");
    }
    const uint8_t* h = blob.data();
    if (read_u32(h + 0) != KXVC_MAGIC) {
        return fail("Invalid KXVC container magic");
    }

    GdkContentContainer c{};
    c.header_.magic = read_u32(h + 0);
    c.header_.version = read_u16(h + 4);
    if (c.header_.version != KXVC_VERSION) {
        return fail(std::format("Unsupported KXVC version: {}", c.header_.version));
    }
    const uint16_t raw_cipher = read_u16(h + 6);
    if (raw_cipher > static_cast<uint16_t>(GdkCipherMode::Aes128Ctr)) {
        return fail(std::format("Unsupported KXVC cipher mode: {}", raw_cipher));
    }
    c.header_.cipher = static_cast<GdkCipherMode>(raw_cipher);
    c.header_.title_id = read_u32(h + 8);
    c.header_.segment_count = read_u32(h + 12);
    std::memcpy(c.header_.key_id.data(), h + 16, 16);
    std::memcpy(c.header_.iv_nonce.data(), h + 32, 16);
    c.header_.payload_base_offset = read_u64(h + 48);
    c.header_.payload_total_bytes = read_u64(h + 56);

    if (c.header_.payload_base_offset < kHeaderSize ||
        c.header_.payload_base_offset > blob.size() ||
        c.header_.payload_total_bytes > kMaxPayloadBytes ||
        c.header_.payload_base_offset + c.header_.payload_total_bytes > blob.size()) {
        return fail("KXVC container payload offsets out of bounds");
    }

    size_t cursor = kHeaderSize;
    c.segments_.reserve(c.header_.segment_count);
    for (uint32_t i = 0; i < c.header_.segment_count; ++i) {
        if (cursor + 38 > c.header_.payload_base_offset) {
            return fail("KXVC segment directory truncated");
        }
        const uint8_t* ep = blob.data() + cursor;
        const uint16_t name_len = read_u16(ep + 0);
        if (cursor + 38 + name_len > c.header_.payload_base_offset) {
            return fail("KXVC segment name out of directory bounds");
        }
        GdkContainerSegment seg{};
        seg.logical_offset = read_u64(ep + 2);
        seg.logical_size = read_u64(ep + 10);
        seg.storage_offset = read_u64(ep + 18);
        seg.storage_size = read_u64(ep + 26);
        seg.crc32 = read_u32(ep + 34);
        seg.name.assign(reinterpret_cast<const char*>(ep + 38), name_len);
        cursor += 38 + name_len;

        if (seg.storage_offset > c.header_.payload_total_bytes ||
            seg.storage_size > c.header_.payload_total_bytes - seg.storage_offset) {
            return fail(std::format("KXVC segment '{}' exceeds payload bounds", seg.name));
        }
        c.segments_.push_back(std::move(seg));
    }

    c.raw_payload_.assign(
        blob.data() + c.header_.payload_base_offset,
        blob.data() + c.header_.payload_base_offset + c.header_.payload_total_bytes);
    return c;
}

Result<GdkContentContainer> GdkContentContainer::parse_file(const std::filesystem::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        return fail(std::format("Unable to open KXVC container: {}", path.string()));
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(ifs)), {});
    return parse(bytes);
}

Result<std::vector<uint8_t>> GdkContentContainer::read_range(
    uint64_t logical_offset, size_t size, const GdkKeyRing& keyring) const {
    if (logical_offset > raw_payload_.size() || size > raw_payload_.size() - logical_offset) {
        return fail("KXVC read_range out of payload bounds");
    }
    std::span<const uint8_t> slice(raw_payload_.data() + logical_offset, size);
    if (header_.cipher == GdkCipherMode::None) {
        return std::vector<uint8_t>(slice.begin(), slice.end());
    }
    if (header_.cipher == GdkCipherMode::Aes128Ctr) {
        auto key = keyring.find_key(header_.key_id);
        if (!key.has_value()) {
            return fail("KXVC decryption failed: missing content key in GdkKeyRing (acquire license first)");
        }
        return aes128_ctr_crypt(*key, header_.iv_nonce, logical_offset, slice);
    }
    return fail("KXVC unsupported cipher mode");
}

Result<std::vector<uint8_t>> GdkContentContainer::read_segment(
    std::string_view name, const GdkKeyRing& keyring) const {
    for (const auto& seg : segments_) {
        if (seg.name == name) {
            auto data = read_range(seg.storage_offset, static_cast<size_t>(seg.storage_size), keyring);
            if (!data) return fail(data.error().message);
            const uint32_t actual_crc = compute_crc32(*data);
            if (actual_crc != seg.crc32) {
                return fail(std::format(
                    "KXVC segment '{}' CRC32 mismatch (expected {:#x}, got {:#x} - wrong key or corrupt data)",
                    seg.name, seg.crc32, actual_crc));
            }
            return data;
        }
    }
    return fail(std::format("KXVC segment not found: {}", name));
}

Result<size_t> GdkContentContainer::mount_to_vfs(
    vfs::OverlayVfs& vfs, const GdkKeyRing& keyring, std::string_view mount_prefix) const {
    size_t mounted = 0;
    for (const auto& seg : segments_) {
        auto plain = read_segment(seg.name, keyring);
        if (!plain) return fail(plain.error().message);

        std::string vpath(mount_prefix);
        if (!vpath.empty() && vpath.back() != '\\' && vpath.back() != '/') {
            vpath.push_back('\\');
        }
        vpath.append(seg.name);

        auto host_path = vfs.resolve(vpath, /*for_write=*/true);
        if (!host_path) return fail(host_path.error().message);

        if (host_path->has_parent_path()) {
            std::error_code ec;
            std::filesystem::create_directories(host_path->parent_path(), ec);
        }
        std::ofstream ofs(*host_path, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            return fail(std::format("mount_to_vfs failed writing {}", host_path->string()));
        }
        if (!plain->empty()) {
            ofs.write(reinterpret_cast<const char*>(plain->data()), static_cast<std::streamsize>(plain->size()));
        }
        ++mounted;
    }
    return mounted;
}

}  // namespace kestrel::formats
