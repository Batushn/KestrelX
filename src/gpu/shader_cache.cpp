#include "kestrel/gpu/shader_cache.hpp"

#include <Windows.h>
#include <algorithm>
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

DiskShaderCache::DiskShaderCache(std::filesystem::path root_dir, ShaderCacheConfig config)
    : root_dir_(root_dir.empty() ? default_cache_dir() : std::move(root_dir)), config_(config) {}

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
                                   const uint8_t* rdna2_bytes, size_t len,
                                   uint32_t group_y, uint32_t group_z,
                                   uint32_t mrt_mask) {
    uint64_t seed = 14695981039346656037ull;
    const uint32_t meta[7] = {
        kShaderCacheCompilerVersion,
        static_cast<uint32_t>(stage),
        static_cast<uint32_t>(wave),
        group_x,
        group_y,
        group_z,
        mrt_mask,
    };
    seed = fnv1a_64_bytes(reinterpret_cast<const uint8_t*>(meta), sizeof(meta), seed);
    return fnv1a_64_bytes(rdna2_bytes, len, seed);
}

void DiskShaderCache::set_root_dir(std::filesystem::path root_dir) {
    std::lock_guard<std::mutex> lock(mtx_);
    root_dir_ = root_dir.empty() ? default_cache_dir() : std::move(root_dir);
}

std::filesystem::path DiskShaderCache::root_dir() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return root_dir_;
}

void DiskShaderCache::set_config(const ShaderCacheConfig& config) {
    std::lock_guard<std::mutex> lock(mtx_);
    config_ = config;
}

ShaderCacheConfig DiskShaderCache::config() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return config_;
}

std::filesystem::path DiskShaderCache::entry_path(uint64_t key) const {
    return root_dir_ / std::format("{:016x}.kxdxil", key);
}

bool DiskShaderCache::load(uint64_t key, std::vector<uint8_t>& out_dxil) {
    std::lock_guard<std::mutex> lock(mtx_);
    const auto path = entry_path(key);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
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

    ifs.close();

    if (config_.enable_lru) {
        std::error_code touch_ec;
        std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now(), touch_ec);
    }

    out_dxil = std::move(payload);
    ++stats_.disk_hits;
    return true;
}

bool DiskShaderCache::store(uint64_t key, const std::vector<uint8_t>& dxil) {
    if (dxil.size() < 32 || dxil.size() > (64u << 20)) return false;

    std::lock_guard<std::mutex> lock(mtx_);
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
    (void)prune_locked();
    return true;
}

size_t DiskShaderCache::prune() {
    std::lock_guard<std::mutex> lock(mtx_);
    return prune_locked();
}

size_t DiskShaderCache::clear() {
    std::lock_guard<std::mutex> lock(mtx_);
    std::error_code ec;
    if (!std::filesystem::exists(root_dir_, ec) || ec) return 0;

    size_t removed = 0;
    for (std::filesystem::directory_iterator it(root_dir_, ec), end; it != end && !ec; it.increment(ec)) {
        std::error_code entry_ec;
        if (!it->is_regular_file(entry_ec) || entry_ec) continue;
        const auto ext = it->path().extension();
        if (ext == ".kxdxil" || ext == ".tmp") {
            if (std::filesystem::remove(it->path(), entry_ec) && !entry_ec) {
                if (ext == ".kxdxil") ++removed;
            }
        }
    }
    return removed;
}

ShaderCacheStats DiskShaderCache::stats() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return stats_;
}

void DiskShaderCache::reset_stats() {
    std::lock_guard<std::mutex> lock(mtx_);
    stats_ = {};
}

size_t DiskShaderCache::prune_locked() {
    if (config_.max_entries == 0 && config_.max_bytes == 0) return 0;

    std::error_code ec;
    if (!std::filesystem::exists(root_dir_, ec) || ec) return 0;

    struct Entry {
        std::filesystem::path path;
        uint64_t size = 0;
        std::filesystem::file_time_type mtime{};
    };

    std::vector<Entry> entries;
    uint64_t total_bytes = 0;

    for (std::filesystem::directory_iterator it(root_dir_, ec), end; it != end && !ec; it.increment(ec)) {
        std::error_code entry_ec;
        if (!it->is_regular_file(entry_ec) || entry_ec) continue;
        if (it->path().extension() != ".kxdxil") continue;

        const uint64_t sz = it->file_size(entry_ec);
        if (entry_ec) continue;
        const auto mt = it->last_write_time(entry_ec);
        if (entry_ec) continue;

        entries.push_back({it->path(), sz, mt});
        total_bytes += sz;
    }

    size_t total_entries = entries.size();
    const bool over_entries = (config_.max_entries > 0) && (total_entries > config_.max_entries);
    const bool over_bytes = (config_.max_bytes > 0) && (total_bytes > config_.max_bytes);
    if (!over_entries && !over_bytes) return 0;

    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.mtime != b.mtime) return a.mtime < b.mtime;
        return a.path < b.path;
    });

    size_t evicted = 0;
    for (const auto& e : entries) {
        const bool still_over_entries = (config_.max_entries > 0) && (total_entries > config_.max_entries);
        const bool still_over_bytes = (config_.max_bytes > 0) && (total_bytes > config_.max_bytes);
        if (!still_over_entries && !still_over_bytes) break;

        std::error_code rm_ec;
        if (std::filesystem::remove(e.path, rm_ec) && !rm_ec) {
            --total_entries;
            total_bytes -= e.size;
            ++stats_.evictions;
            ++evicted;
        }
    }
    return evicted;
}

}  // namespace kestrel::gpu
