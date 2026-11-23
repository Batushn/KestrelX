#include "kestrel/gpu/shader_cache.hpp"

#include <Windows.h>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>

namespace kestrel::gpu {

namespace {

constexpr uint8_t kCacheMagic[4] = {'K', 'X', 'S', 'C'};
constexpr uint32_t kCacheFormatVersion = 1;

struct CacheHeader {
    uint8_t magic[4];
    uint32_t format_version;
    uint32_t compiler_version;
    uint32_t dxil_size;
    uint64_t payload_hash;
};

uint64_t fnv1a_64_bytes(const uint8_t* data, size_t len, uint64_t seed = 14695981039346656037ull) {
    uint64_t h = seed;
    for (size_t i = 0; i < len; ++i) {
        h ^= data[i];
        h *= 1099511628211ull;
    }
    return h;
}

}  // namespace

DiskShaderCache::DiskShaderCache(std::filesystem::path root_dir)
    : root_dir_(root_dir.empty() ? default_cache_dir() : std::move(root_dir)) {}

std::filesystem::path DiskShaderCache::default_cache_dir() {
    if (const char* override_dir = std::getenv("KESTREL_SHADER_CACHE_DIR"); override_dir && override_dir[0] != '\0') {
        return std::filesystem::path(override_dir);
    }
    if (const char* local_app = std::getenv("LOCALAPPDATA"); local_app && local_app[0] != '\0') {
        return std::filesystem::path(local_app) / "KestrelX" / "cache";
    }
    std::error_code ec;
    auto tmp = std::filesystem::temp_directory_path(ec);
    return (ec ? std::filesystem::path(".") : tmp) / "KestrelX" / "cache";
}

uint64_t DiskShaderCache::make_key(ShaderStage stage, WaveMode wave, uint32_t group_x,
                                   const uint8_t* rdna2_bytes, size_t len) {
    uint64_t seed = 14695981039346656037ull;
    const uint32_t meta[4] = {
        kShaderCacheCompilerVersion,
        static_cast<uint32_t>(stage),
        static_cast<uint32_t>(wave),
        group_x,
    };
    seed = fnv1a_64_bytes(reinterpret_cast<const uint8_t*>(meta), sizeof(meta), seed);
    return fnv1a_64_bytes(rdna2_bytes, len, seed);
}

void DiskShaderCache::set_root_dir(std::filesystem::path root_dir) {
    root_dir_ = root_dir.empty() ? default_cache_dir() : std::move(root_dir);
}

std::filesystem::path DiskShaderCache::entry_path(uint64_t key) const {
    return root_dir_ / std::format("{:016x}.kxdxil", key);
}

bool DiskShaderCache::load(uint64_t key, std::vector<uint8_t>& out_dxil) {
    const auto path = entry_path(key);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        ++stats_.disk_misses;
        return false;
    }

    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        ++stats_.disk_misses;
        return false;
    }

    CacheHeader hdr{};
    if (!ifs.read(reinterpret_cast<char*>(&hdr), sizeof(hdr))) {
        ifs.close();
        std::filesystem::remove(path, ec);
        ++stats_.corrupt_evictions;
        ++stats_.disk_misses;
        return false;
    }

    if (std::memcmp(hdr.magic, kCacheMagic, 4) != 0 ||
        hdr.format_version != kCacheFormatVersion ||
        hdr.compiler_version != kShaderCacheCompilerVersion ||
        hdr.dxil_size < 32 || hdr.dxil_size > (64u << 20)) {
        ifs.close();
        std::filesystem::remove(path, ec);
        ++stats_.corrupt_evictions;
        ++stats_.disk_misses;
        return false;
    }

    std::vector<uint8_t> payload(hdr.dxil_size);
    if (!ifs.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(hdr.dxil_size))) {
        ifs.close();
        std::filesystem::remove(path, ec);
        ++stats_.corrupt_evictions;
        ++stats_.disk_misses;
        return false;
    }

    // Verify FNV-1a integrity checksum and DXBC/DXIL container signature
    if (fnv1a_64_bytes(payload.data(), payload.size()) != hdr.payload_hash ||
        std::memcmp(payload.data(), "DXBC", 4) != 0) {
        ifs.close();
        std::filesystem::remove(path, ec);
        ++stats_.corrupt_evictions;
        ++stats_.disk_misses;
        return false;
    }

    out_dxil = std::move(payload);
    ++stats_.disk_hits;
    return true;
}

bool DiskShaderCache::store(uint64_t key, const std::vector<uint8_t>& dxil) {
    if (dxil.size() < 32 || dxil.size() > (64u << 20)) return false;
    std::error_code ec;
    std::filesystem::create_directories(root_dir_, ec);
    if (ec) return false;

    const auto final_path = entry_path(key);
    const auto tmp_path = root_dir_ / std::format("{:016x}.{}.{}.tmp",
                                                  key, GetCurrentProcessId(), GetCurrentThreadId());

    CacheHeader hdr{};
    std::memcpy(hdr.magic, kCacheMagic, 4);
    hdr.format_version = kCacheFormatVersion;
    hdr.compiler_version = kShaderCacheCompilerVersion;
    hdr.dxil_size = static_cast<uint32_t>(dxil.size());
    hdr.payload_hash = fnv1a_64_bytes(dxil.data(), dxil.size());

    {
        std::ofstream ofs(tmp_path, std::ios::binary | std::ios::trunc);
        if (!ofs) return false;
        ofs.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
        ofs.write(reinterpret_cast<const char*>(dxil.data()), static_cast<std::streamsize>(dxil.size()));
        ofs.flush();
        if (!ofs.good()) {
            ofs.close();
            std::filesystem::remove(tmp_path, ec);
            return false;
        }
    }

    if (!MoveFileExW(tmp_path.c_str(), final_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(tmp_path, ec);
        return false;
    }

    ++stats_.disk_writes;
    return true;
}

}  // namespace kestrel::gpu
