#pragma once

#include "kestrel/gpu/rdna2_recomp.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <vector>

namespace kestrel::gpu {

// Bump whenever IR lifting or HLSL emission semantics change to auto-invalidate stale disk entries.
constexpr uint32_t kShaderCacheCompilerVersion = 0x00040001u;

struct ShaderCacheConfig {
    size_t max_entries = 4096;                  // 0 = unlimited
    uint64_t max_bytes = 256ull * 1024 * 1024;  // 256 MiB; 0 = unlimited
    bool enable_lru = true;
};

struct ShaderCacheStats {
    uint64_t disk_hits = 0;
    uint64_t disk_misses = 0;
    uint64_t disk_writes = 0;
    uint64_t corrupt_evictions = 0;
    uint64_t evictions = 0;
};

// Persistent on-disk DXIL cache with atomic writes, LRU quota eviction, and FNV-1a payload integrity checks.
// Default root: KESTREL_SHADER_CACHE_DIR -> %LOCALAPPDATA%/KestrelX/cache -> temp/KestrelX/cache.
class DiskShaderCache {
public:
    explicit DiskShaderCache(std::filesystem::path root_dir = {});
    DiskShaderCache(std::filesystem::path root_dir, ShaderCacheConfig config);

    static std::filesystem::path default_cache_dir();
    static uint64_t make_key(ShaderStage stage, WaveMode wave, uint32_t group_x,
                             const uint8_t* rdna2_bytes, size_t len,
                             uint32_t group_y = 1, uint32_t group_z = 1,
                             uint32_t mrt_mask = 0);

    void set_root_dir(std::filesystem::path root_dir);
    std::filesystem::path root_dir() const;

    void set_config(const ShaderCacheConfig& config);
    ShaderCacheConfig config() const;

    bool load(uint64_t key, std::vector<uint8_t>& out_dxil);
    bool store(uint64_t key, const std::vector<uint8_t>& dxil);

    size_t prune();
    size_t clear();

    ShaderCacheStats stats() const;
    void reset_stats();

private:
    std::filesystem::path entry_path(uint64_t key) const;
    size_t prune_locked();

    mutable std::mutex mtx_;
    std::filesystem::path root_dir_;
    ShaderCacheConfig config_{};
    ShaderCacheStats stats_{};
};

}  // namespace kestrel::gpu
