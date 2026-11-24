#include "kestrel/vfs/overlay_vfs.hpp"

#include <algorithm>
#include <system_error>

namespace kestrel::vfs {

namespace {

std::filesystem::path safe_weakly_canonical(const std::filesystem::path& p) {
    std::error_code ec;
    auto res = std::filesystem::weakly_canonical(p, ec);
    if (ec) {
        return p.lexically_normal();
    }
    return res;
}

std::string join_lower_parts(const std::vector<std::string>& parts, size_t count) {
    std::string out;
    for (size_t i = 0; i < count; ++i) {
        if (i > 0) out += '/';
        out += to_lower_ascii(parts[i]);
    }
    return out;
}

std::string_view parent_lower_rel(std::string_view lower_rel) {
    const size_t pos = lower_rel.rfind('/');
    if (pos == std::string_view::npos) return {};
    return lower_rel.substr(0, pos);
}

bool starts_with_ci(std::string_view s, std::string_view prefix) {
    if (s.size() < prefix.size()) return false;
    return to_lower_ascii(s.substr(0, prefix.size())) == to_lower_ascii(prefix);
}

}  // namespace

OverlayVfs::OverlayVfs(std::filesystem::path game_root, std::filesystem::path data_root)
    : game_root_(safe_weakly_canonical(game_root)),
      overlay_root_(safe_weakly_canonical(data_root / "overlay")),
      save_root_(safe_weakly_canonical(data_root / "save")),
      temp_root_(safe_weakly_canonical(data_root / "temp")) {
    std::error_code ec;
    std::filesystem::create_directories(overlay_root_, ec);
    std::filesystem::create_directories(save_root_, ec);
    std::filesystem::create_directories(temp_root_, ec);

    game_cache_.root = game_root_;
    overlay_cache_.root = overlay_root_;
    save_cache_.root = save_root_;
    temp_cache_.root = temp_root_;
}

OverlayVfs::CachedLookup OverlayVfs::LayerCache::lookup(
    const std::vector<std::string>& parts,
    std::string_view lower_rel,
    CacheStats& stats) {
    const std::string key(lower_rel);
    if (auto it = path_cache.find(key); it != path_cache.end()) {
        ++stats.cache_hits;
        return it->second;
    }

    ++stats.cache_misses;

    if (parts.empty()) {
        std::error_code ec;
        const bool is_dir = std::filesystem::is_directory(root, ec) && !ec;
        CachedLookup res{is_dir, is_dir, is_dir ? root : std::filesystem::path{}};
        path_cache.emplace(key, res);
        return res;
    }

    std::filesystem::path cur_host = root;
    std::string cur_dir_key;
    for (size_t i = 0; i < parts.size(); ++i) {
        auto dir_it = dirs.find(cur_dir_key);
        if (dir_it == dirs.end()) {
            DirIndex idx;
            std::error_code ec;
            const bool is_dir = std::filesystem::is_directory(cur_host, ec) && !ec;
            if (is_dir) {
                ++stats.dir_scans;
                const auto opts = std::filesystem::directory_options::skip_permission_denied;
                for (auto it = std::filesystem::directory_iterator(cur_host, opts, ec);
                     !ec && it != std::filesystem::directory_iterator();
                     it.increment(ec)) {
                    std::error_code item_ec;
                    const std::string fname = it->path().filename().string();
                    const bool entry_is_dir = it->is_directory(item_ec) && !item_ec;
                    idx.children.insert_or_assign(
                        to_lower_ascii(fname),
                        CachedChild{fname, entry_is_dir});
                }
            }
            dir_it = dirs.emplace(cur_dir_key, std::move(idx)).first;
        }

        const std::string lower_part = to_lower_ascii(parts[i]);
        auto child_it = dir_it->second.children.find(lower_part);
        if (child_it == dir_it->second.children.end()) {
            CachedLookup miss{};
            path_cache.emplace(key, miss);
            return miss;
        }

        cur_host /= child_it->second.actual_name;
        if (i + 1 < parts.size()) {
            if (!child_it->second.is_directory) {
                CachedLookup miss{};
                path_cache.emplace(key, miss);
                return miss;
            }
            if (!cur_dir_key.empty()) cur_dir_key += '/';
            cur_dir_key += lower_part;
        } else {
            CachedLookup hit{true, child_it->second.is_directory, std::move(cur_host)};
            path_cache.emplace(key, hit);
            return hit;
        }
    }

    return {};
}

const OverlayVfs::DirIndex* OverlayVfs::LayerCache::get_dir(
    const std::vector<std::string>& parts,
    std::string_view lower_rel,
    CacheStats& stats) {
    const CachedLookup dir_lookup = lookup(parts, lower_rel, stats);
    if (!dir_lookup.exists || !dir_lookup.is_directory) {
        return nullptr;
    }

    const std::string dir_key(lower_rel);
    if (auto it = dirs.find(dir_key); it != dirs.end()) {
        return &it->second;
    }

    DirIndex idx;
    std::error_code ec;
    ++stats.dir_scans;
    const auto opts = std::filesystem::directory_options::skip_permission_denied;
    for (auto it = std::filesystem::directory_iterator(dir_lookup.host_path, opts, ec);
         !ec && it != std::filesystem::directory_iterator();
         it.increment(ec)) {
        std::error_code item_ec;
        const std::string fname = it->path().filename().string();
        const bool entry_is_dir = it->is_directory(item_ec) && !item_ec;
        idx.children.insert_or_assign(
            to_lower_ascii(fname),
            CachedChild{fname, entry_is_dir});
    }
    return &dirs.emplace(dir_key, std::move(idx)).first->second;
}

void OverlayVfs::LayerCache::invalidate(std::string_view lower_rel) {
    const std::string target(lower_rel);
    path_cache.erase(target);

    const std::string sub_prefix = target.empty() ? std::string{} : (target + "/");
    for (auto it = path_cache.begin(); it != path_cache.end();) {
        if (target.empty() || it->first == target || it->first.starts_with(sub_prefix)) {
            it = path_cache.erase(it);
        } else {
            ++it;
        }
    }

    for (auto it = dirs.begin(); it != dirs.end();) {
        if (target.empty() || it->first == target || it->first.starts_with(sub_prefix)) {
            it = dirs.erase(it);
        } else {
            ++it;
        }
    }

    std::string_view cur = parent_lower_rel(lower_rel);
    while (true) {
        dirs.erase(std::string(cur));
        if (cur.empty()) break;
        cur = parent_lower_rel(cur);
    }
}

void OverlayVfs::LayerCache::clear() {
    dirs.clear();
    path_cache.clear();
}

Result<OverlayVfs::ParsedPath> OverlayVfs::parse_console_path(std::string_view console_path) const {
    if (console_path.empty()) return fail("Empty console path");
    if (console_path.find('\0') != std::string_view::npos) {
        return fail("Console path contains NUL byte");
    }

    // Strip Win32 extended-length or NT object namespace prefix if present
    if (console_path.starts_with("\\\\?\\") || console_path.starts_with("\\??\\")) {
        console_path.remove_prefix(4);
    }
    if (console_path.empty()) return fail("Empty console path after prefix");

    MountKind mount = MountKind::GameReadOnly;
    std::string_view rest = console_path;

    if (starts_with_ci(rest, "game:")) {
        mount = MountKind::GameReadOnly;
        rest.remove_prefix(5);
    } else if (starts_with_ci(rest, "g:") || starts_with_ci(rest, "d:") || starts_with_ci(rest, "c:")) {
        mount = MountKind::GameReadOnly;
        rest.remove_prefix(2);
    } else if (starts_with_ci(rest, "s:")) {
        mount = MountKind::PersistentSave;
        rest.remove_prefix(2);
    } else if (starts_with_ci(rest, "t:")) {
        mount = MountKind::TempScratch;
        rest.remove_prefix(2);
    } else if (rest.find(':') != std::string_view::npos) {
        return fail("Unsupported console drive in path: " + std::string(console_path));
    }

    // Reject colons (e.g. NTFS ADS) in the remainder of the path
    if (rest.find(':') != std::string_view::npos) {
        return fail("Invalid colon in console path: " + std::string(console_path));
    }

    while (!rest.empty() && (rest.front() == '\\' || rest.front() == '/')) {
        rest.remove_prefix(1);
    }

    // Normalize relative path components and reject traversal outside root
    std::vector<std::string> parts;
    size_t pos = 0;
    while (pos < rest.size()) {
        const size_t next = rest.find_first_of("\\/", pos);
        const std::string_view seg =
            (next == std::string_view::npos) ? rest.substr(pos) : rest.substr(pos, next - pos);
        pos = (next == std::string_view::npos) ? rest.size() : next + 1;
        if (seg.empty() || seg == ".") continue;
        if (seg == "..") {
            if (parts.empty()) {
                return fail("Console path escapes volume root: " + std::string(console_path));
            }
            parts.pop_back();
            continue;
        }
        parts.emplace_back(seg);
    }

    std::filesystem::path rel;
    std::string lower_rel = join_lower_parts(parts, parts.size());
    std::string key = std::to_string(static_cast<int>(mount)) + ":";
    for (const auto& part : parts) {
        rel /= part;
        key += "/";
        key += to_lower_ascii(part);
    }
    return ParsedPath{mount, std::move(parts), std::move(rel), std::move(lower_rel), std::move(key)};
}

Result<std::filesystem::path> OverlayVfs::resolve(std::string_view console_path, bool for_write) const {
    auto parsed = parse_console_path(console_path);
    if (!parsed) return std::unexpected(parsed.error());

    std::lock_guard lock(mutex_);

    if (parsed->mount == MountKind::PersistentSave) {
        const auto save_root = current_save_root_locked();
        if (for_write) {
            save_cache_.invalidate(parsed->lower_rel);
            const auto p = save_root / parsed->rel;
            if (p.has_parent_path()) {
                std::error_code ec;
                std::filesystem::create_directories(p.parent_path(), ec);
                if (ec) return fail("Failed to create save directory: " + ec.message());
            }
            return p;
        }
        const auto hit = save_cache_.lookup(parsed->parts, parsed->lower_rel, stats_);
        return hit.exists ? hit.host_path : (save_root / parsed->rel);
    }

    if (parsed->mount == MountKind::TempScratch) {
        if (for_write) {
            temp_cache_.invalidate(parsed->lower_rel);
            const auto p = temp_root_ / parsed->rel;
            if (p.has_parent_path()) {
                std::error_code ec;
                std::filesystem::create_directories(p.parent_path(), ec);
                if (ec) return fail("Failed to create temp directory: " + ec.message());
            }
            return p;
        }
        const auto hit = temp_cache_.lookup(parsed->parts, parsed->lower_rel, stats_);
        return hit.exists ? hit.host_path : (temp_root_ / parsed->rel);
    }

    // GameReadOnly: writes ALWAYS land in overlay_root_
    if (for_write) {
        const_cast<std::unordered_set<std::string>&>(whiteouts_).erase(parsed->canonical_key);
        overlay_cache_.invalidate(parsed->lower_rel);
        const auto ov = overlay_root_ / parsed->rel;
        if (ov.has_parent_path()) {
            std::error_code ec;
            std::filesystem::create_directories(ov.parent_path(), ec);
            if (ec) return fail("Failed to create overlay directory: " + ec.message());
        }
        return ov;
    }

    if (whiteouts_.contains(parsed->canonical_key)) {
        return fail("File was deleted in overlay: " + std::string(console_path));
    }

    const auto ov_hit = overlay_cache_.lookup(parsed->parts, parsed->lower_rel, stats_);
    if (ov_hit.exists) return ov_hit.host_path;

    const auto game_hit = game_cache_.lookup(parsed->parts, parsed->lower_rel, stats_);
    if (game_hit.exists) return game_hit.host_path;

    return game_root_ / parsed->rel;
}

Result<void> OverlayVfs::remove_file(std::string_view console_path) {
    auto parsed = parse_console_path(console_path);
    if (!parsed) return std::unexpected(parsed.error());

    std::lock_guard lock(mutex_);
    std::error_code ec;

    if (parsed->mount == MountKind::PersistentSave) {
        const auto save_root = current_save_root_locked();
        const auto hit = save_cache_.lookup(parsed->parts, parsed->lower_rel, stats_);
        const auto target = hit.exists ? hit.host_path : (save_root / parsed->rel);
        std::filesystem::remove(target, ec);
        save_cache_.invalidate(parsed->lower_rel);
        return {};
    }

    if (parsed->mount == MountKind::TempScratch) {
        const auto hit = temp_cache_.lookup(parsed->parts, parsed->lower_rel, stats_);
        const auto target = hit.exists ? hit.host_path : (temp_root_ / parsed->rel);
        std::filesystem::remove(target, ec);
        temp_cache_.invalidate(parsed->lower_rel);
        return {};
    }

    const auto ov_hit = overlay_cache_.lookup(parsed->parts, parsed->lower_rel, stats_);
    const auto ov_target = ov_hit.exists ? ov_hit.host_path : (overlay_root_ / parsed->rel);
    std::filesystem::remove(ov_target, ec);
    overlay_cache_.invalidate(parsed->lower_rel);

    const auto game_hit = game_cache_.lookup(parsed->parts, parsed->lower_rel, stats_);
    if (game_hit.exists) {
        whiteouts_.insert(parsed->canonical_key);
    }
    return {};
}

bool OverlayVfs::exists(std::string_view console_path) const {
    auto parsed = parse_console_path(console_path);
    if (!parsed) return false;

    std::lock_guard lock(mutex_);
    if (parsed->mount == MountKind::PersistentSave) {
        return save_cache_.lookup(parsed->parts, parsed->lower_rel, stats_).exists;
    }
    if (parsed->mount == MountKind::TempScratch) {
        return temp_cache_.lookup(parsed->parts, parsed->lower_rel, stats_).exists;
    }
    if (whiteouts_.contains(parsed->canonical_key)) {
        return false;
    }
    if (overlay_cache_.lookup(parsed->parts, parsed->lower_rel, stats_).exists) {
        return true;
    }
    return game_cache_.lookup(parsed->parts, parsed->lower_rel, stats_).exists;
}

Result<std::vector<DirEntry>> OverlayVfs::list_directory(std::string_view virtual_dir) const {
    auto parsed = parse_console_path(virtual_dir);
    if (!parsed) return std::unexpected(parsed.error());

    std::lock_guard lock(mutex_);

    auto collect_single = [&](LayerCache& cache) {
        std::vector<DirEntry> entries;
        const DirIndex* idx = cache.get_dir(parsed->parts, parsed->lower_rel, stats_);
        if (!idx) return entries;
        entries.reserve(idx->children.size());
        for (const auto& [lower_name, child] : idx->children) {
            (void)lower_name;
            entries.push_back(DirEntry{child.actual_name, child.is_directory});
        }
        std::sort(entries.begin(), entries.end(), [](const DirEntry& a, const DirEntry& b) {
            return to_lower_ascii(a.name) < to_lower_ascii(b.name);
        });
        return entries;
    };

    if (parsed->mount == MountKind::PersistentSave) {
        return collect_single(save_cache_);
    }
    if (parsed->mount == MountKind::TempScratch) {
        return collect_single(temp_cache_);
    }

    if (whiteouts_.contains(parsed->canonical_key)) {
        return std::vector<DirEntry>{};
    }

    std::unordered_map<std::string, DirEntry> merged;
    auto merge_layer = [&](LayerCache& cache) {
        const DirIndex* idx = cache.get_dir(parsed->parts, parsed->lower_rel, stats_);
        if (!idx) return;
        for (const auto& [lower_name, child] : idx->children) {
            const std::string child_key = parsed->canonical_key + "/" + lower_name;
            if (whiteouts_.contains(child_key)) continue;
            // Upper layer is merged first; try_emplace keeps upper on case-insensitive collision.
            merged.try_emplace(lower_name, DirEntry{child.actual_name, child.is_directory});
        }
    };

    merge_layer(overlay_cache_);
    merge_layer(game_cache_);

    std::vector<DirEntry> entries;
    entries.reserve(merged.size());
    for (auto& [lower_name, entry] : merged) {
        (void)lower_name;
        entries.push_back(std::move(entry));
    }
    std::sort(entries.begin(), entries.end(), [](const DirEntry& a, const DirEntry& b) {
        return to_lower_ascii(a.name) < to_lower_ascii(b.name);
    });
    return entries;
}

void OverlayVfs::invalidate_caches() const {
    std::lock_guard lock(mutex_);
    game_cache_.clear();
    overlay_cache_.clear();
    save_cache_.clear();
    save_cache_.root = current_save_root_locked();
    temp_cache_.clear();
}

CacheStats OverlayVfs::cache_stats() const {
    std::lock_guard lock(mutex_);
    return stats_;
}

std::filesystem::path OverlayVfs::current_save_root_locked() const {
    if (active_user_xuid_ == 0) {
        return save_root_;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "user_%016llx", static_cast<unsigned long long>(active_user_xuid_));
    return save_root_ / buf;
}

void OverlayVfs::set_active_user_xuid(uint64_t xuid) {
    std::lock_guard lock(mutex_);
    if (active_user_xuid_ == xuid) return;
    active_user_xuid_ = xuid;
    save_cache_.clear();
    save_cache_.root = current_save_root_locked();
    std::error_code ec;
    std::filesystem::create_directories(save_cache_.root, ec);
}

uint64_t OverlayVfs::active_user_xuid() const {
    std::lock_guard lock(mutex_);
    return active_user_xuid_;
}

std::filesystem::path OverlayVfs::active_save_root() const {
    std::lock_guard lock(mutex_);
    return current_save_root_locked();
}

}  // namespace kestrel::vfs