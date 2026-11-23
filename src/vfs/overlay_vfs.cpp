#include "kestrel/vfs/overlay_vfs.hpp"

#include <algorithm>

namespace kestrel::vfs {

OverlayVfs::OverlayVfs(std::filesystem::path game_root, std::filesystem::path data_root)
    : game_root_(std::filesystem::weakly_canonical(std::move(game_root))),
      overlay_root_(std::filesystem::weakly_canonical(data_root / "overlay")),
      save_root_(std::filesystem::weakly_canonical(data_root / "save")),
      temp_root_(std::filesystem::weakly_canonical(data_root / "temp")) {
    std::filesystem::create_directories(overlay_root_);
    std::filesystem::create_directories(save_root_);
    std::filesystem::create_directories(temp_root_);
}

Result<OverlayVfs::ParsedPath> OverlayVfs::parse_console_path(std::string_view console_path) const {
    if (console_path.empty()) return fail("Empty console path");

    // Strip Win32 extended-length prefix if present
    if (console_path.starts_with("\\\\?\\")) console_path.remove_prefix(4);

    MountKind mount = MountKind::GameReadOnly;
    std::string_view rest = console_path;

    auto starts_ci = [](std::string_view s, std::string_view pfx) {
        if (s.size() < pfx.size()) return false;
        return to_lower_ascii(s.substr(0, pfx.size())) == to_lower_ascii(pfx);
    };

    if (starts_ci(rest, "game:")) {
        mount = MountKind::GameReadOnly;
        rest.remove_prefix(5);
    } else if (starts_ci(rest, "g:") || starts_ci(rest, "d:")) {
        mount = MountKind::GameReadOnly;
        rest.remove_prefix(2);
    } else if (starts_ci(rest, "s:")) {
        mount = MountKind::PersistentSave;
        rest.remove_prefix(2);
    } else if (starts_ci(rest, "t:")) {
        mount = MountKind::TempScratch;
        rest.remove_prefix(2);
    } else if (rest.find(':') != std::string_view::npos) {
        return fail("Unsupported console drive in path: " + std::string(console_path));
    }

    while (!rest.empty() && (rest.front() == '\\' || rest.front() == '/')) {
        rest.remove_prefix(1);
    }

    // Normalize relative path components and reject traversal outside root
    std::vector<std::string> parts;
    size_t pos = 0;
    while (pos < rest.size()) {
        size_t next = rest.find_first_of("\\/", pos);
        std::string_view seg = (next == std::string_view::npos) ? rest.substr(pos) : rest.substr(pos, next - pos);
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
    std::string key = std::to_string(static_cast<int>(mount)) + ":";
    for (const auto& part : parts) {
        rel /= part;
        key += "/";
        key += to_lower_ascii(part);
    }
    return ParsedPath{mount, std::move(rel), std::move(key)};
}

Result<std::filesystem::path> OverlayVfs::resolve(std::string_view console_path, bool for_write) const {
    auto parsed = parse_console_path(console_path);
    if (!parsed) return std::unexpected(parsed.error());

    if (parsed->mount == MountKind::PersistentSave) {
        auto p = save_root_ / parsed->rel;
        if (for_write && p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
        return p;
    }
    if (parsed->mount == MountKind::TempScratch) {
        auto p = temp_root_ / parsed->rel;
        if (for_write && p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
        return p;
    }

    // GameReadOnly: writes ALWAYS land in overlay_root_
    const auto ov = overlay_root_ / parsed->rel;
    if (for_write) {
        std::lock_guard lock(mutex_);
        // Writing recreates a previously whited-out file (caller handles creation)
        const_cast<std::unordered_set<std::string>&>(whiteouts_).erase(parsed->canonical_key);
        if (ov.has_parent_path()) std::filesystem::create_directories(ov.parent_path());
        return ov;
    }

    {
        std::lock_guard lock(mutex_);
        if (whiteouts_.contains(parsed->canonical_key)) {
            return fail("File was deleted in overlay: " + std::string(console_path));
        }
    }
    if (std::filesystem::exists(ov)) return ov;
    return game_root_ / parsed->rel;
}

Result<void> OverlayVfs::remove_file(std::string_view console_path) {
    auto parsed = parse_console_path(console_path);
    if (!parsed) return std::unexpected(parsed.error());

    if (parsed->mount == MountKind::PersistentSave) {
        std::error_code ec;
        std::filesystem::remove(save_root_ / parsed->rel, ec);
        return {};
    }
    if (parsed->mount == MountKind::TempScratch) {
        std::error_code ec;
        std::filesystem::remove(temp_root_ / parsed->rel, ec);
        return {};
    }

    std::error_code ec;
    std::filesystem::remove(overlay_root_ / parsed->rel, ec);
    if (std::filesystem::exists(game_root_ / parsed->rel)) {
        std::lock_guard lock(mutex_);
        whiteouts_.insert(parsed->canonical_key);
    }
    return {};
}

bool OverlayVfs::exists(std::string_view console_path) const {
    auto p = resolve(console_path, false);
    if (!p) return false;
    return std::filesystem::exists(*p);
}

}  // namespace kestrel::vfs
