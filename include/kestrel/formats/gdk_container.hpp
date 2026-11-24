#pragma once

#include "kestrel/common/types.hpp"
#include "kestrel/vfs/overlay_vfs.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kestrel::formats {

// Clean-room GDK / XVC-inspired Content Container ("KXVC")
constexpr uint32_t KXVC_MAGIC = 0x4356584Bu;  // 'K','X','V','C'
constexpr uint16_t KXVC_VERSION = 1;

enum class GdkCipherMode : uint16_t {
    None = 0,       // Transparent unencrypted container pass-through
    Aes128Ctr = 1,  // Hardware/CNG AES-128-CTR seekable stream decryption
};

using GdkKeyId = std::array<uint8_t, 16>;
using GdkAes128Key = std::array<uint8_t, 16>;
using GdkIvNonce = std::array<uint8_t, 16>;

struct GdkContainerSegment {
    std::string name;
    uint64_t logical_offset = 0;
    uint64_t logical_size = 0;
    uint64_t storage_offset = 0;
    uint64_t storage_size = 0;
    uint32_t crc32 = 0;
};

struct GdkContainerHeader {
    uint32_t magic = KXVC_MAGIC;
    uint16_t version = KXVC_VERSION;
    GdkCipherMode cipher = GdkCipherMode::None;
    uint32_t title_id = 0;
    uint32_t segment_count = 0;
    GdkKeyId key_id{};
    GdkIvNonce iv_nonce{};
    uint64_t payload_base_offset = 0;
    uint64_t payload_total_bytes = 0;
};

// Thread-safe content key registry mapping 16-byte KeyIDs -> 128-bit AES keys.
class GdkKeyRing {
public:
    void register_key(const GdkKeyId& key_id, const GdkAes128Key& key);
    std::optional<GdkAes128Key> find_key(const GdkKeyId& key_id) const;
    void remove_key(const GdkKeyId& key_id);
    void clear();
    size_t size() const;

    // Derive a deterministic 128-bit key from a license token and key ID via SHA-256 (Win32 CNG).
    static Result<GdkAes128Key> derive_key_from_token(std::string_view license_token, const GdkKeyId& key_id);

private:
    static std::string key_id_hex(const GdkKeyId& id);
    mutable std::mutex mtx_;
    std::unordered_map<std::string, GdkAes128Key> keys_;
};

// Low-level Win32 CNG AES-128-CTR stream cipher (seekable at arbitrary byte offsets).
Result<std::vector<uint8_t>> aes128_ctr_crypt(
    const GdkAes128Key& key,
    const GdkIvNonce& iv_nonce,
    uint64_t stream_byte_offset,
    std::span<const uint8_t> input);

uint32_t compute_crc32(std::span<const uint8_t> data);

class GdkContentContainer {
public:
    static bool is_gdk_container(std::span<const uint8_t> data);

    static Result<GdkContentContainer> parse(std::span<const uint8_t> blob);
    static Result<GdkContentContainer> parse_file(const std::filesystem::path& path);

    // Synthesize an encrypted (Aes128Ctr) or unencrypted (None) clean-room KXVC container blob.
    static Result<std::vector<uint8_t>> synthesize_container(
        uint32_t title_id,
        GdkCipherMode cipher,
        const GdkKeyId& key_id,
        const GdkIvNonce& iv_nonce,
        const std::optional<GdkAes128Key>& key,
        const std::vector<std::pair<std::string, std::vector<uint8_t>>>& segments);

    // Read & transparently decrypt/pass-through a named segment, verifying CRC32.
    Result<std::vector<uint8_t>> read_segment(std::string_view name, const GdkKeyRing& keyring) const;

    // Seekable read & decrypt at arbitrary logical offset across the container payload stream.
    Result<std::vector<uint8_t>> read_range(uint64_t logical_offset, size_t size, const GdkKeyRing& keyring) const;

    // Mount all segments into an OverlayVfs writable overlay under `mount_prefix` (e.g. "GAME:\\").
    Result<size_t> mount_to_vfs(vfs::OverlayVfs& vfs, const GdkKeyRing& keyring,
                                std::string_view mount_prefix = "GAME:\\") const;

    const GdkContainerHeader& header() const { return header_; }
    const std::vector<GdkContainerSegment>& segments() const { return segments_; }

private:
    GdkContainerHeader header_{};
    std::vector<GdkContainerSegment> segments_;
    std::vector<uint8_t> raw_payload_;
};

}  // namespace kestrel::formats
