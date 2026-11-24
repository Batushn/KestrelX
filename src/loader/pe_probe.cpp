#include "kestrel/loader/pe_probe.hpp"

#include "kestrel/loader/game_config.hpp"
#include "kestrel/loader/pe_image.hpp"

#include <Windows.h>
#include <algorithm>
#include <cstring>
#include <format>
#include <sstream>
#include <unordered_map>

namespace fs = std::filesystem;

namespace kestrel::loader {

namespace {

std::string json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    out += std::format("\\u{:04x}", static_cast<unsigned>(c));
                } else {
                    out.push_back(ch);
                }
                break;
        }
    }
    return out;
}

std::optional<PeExportSymbol> find_export_in_image(
    const PeImage& img,
    bool by_ordinal,
    uint16_t ordinal,
    std::string_view name) {
    for (const auto& e : img.exports) {
        if (by_ordinal) {
            if (e.ordinal == ordinal) return e;
        } else {
            if (e.name == name) return e;
        }
    }
    return std::nullopt;
}

bool check_host_symbol(const std::string& dll_name, bool by_ordinal, uint16_t ordinal, std::string_view name) {
    HMODULE h = GetModuleHandleA(dll_name.c_str());
    bool loaded_temp = false;
    if (!h) {
        h = LoadLibraryExA(dll_name.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
        loaded_temp = (h != nullptr);
    }
    if (!h) return false;
    FARPROC proc = by_ordinal
                       ? GetProcAddress(h, MAKEINTRESOURCEA(ordinal))
                       : GetProcAddress(h, std::string(name).c_str());
    if (loaded_temp) FreeLibrary(h);
    return proc != nullptr;
}

const PeImage* load_guest_cached(
    const std::string& dll_name,
    const std::vector<fs::path>& search_dirs,
    std::unordered_map<std::string, PeImage>& cache) {
    const std::string key = to_lower_ascii(dll_name);
    if (auto it = cache.find(key); it != cache.end()) {
        return &it->second;
    }
    for (const auto& dir : search_dirs) {
        const fs::path cand = dir / dll_name;
        if (fs::is_regular_file(cand)) {
            auto parsed = parse_pe64_file(cand);
            if (parsed) {
                auto [ins, _] = cache.emplace(key, std::move(*parsed));
                return &ins->second;
            }
        }
    }
    return nullptr;
}

bool resolve_guest_chain(
    const PeImage& img,
    bool by_ordinal,
    uint16_t ordinal,
    std::string_view name,
    const hle::Registry& hle,
    const std::vector<fs::path>& search_dirs,
    std::unordered_map<std::string, PeImage>& cache,
    int depth) {
    if (depth >= 8) return false;
    auto exp = find_export_in_image(img, by_ordinal, ordinal, name);
    if (!exp) return false;
    if (exp->forwarder.empty()) return exp->rva < img.size_of_image;

    const size_t dot = exp->forwarder.rfind('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= exp->forwarder.size()) return false;
    const std::string dll_part = exp->forwarder.substr(0, dot);
    const std::string sym_part = exp->forwarder.substr(dot + 1);
    const std::string target_dll = to_lower_ascii(dll_part).ends_with(".dll") ? dll_part : (dll_part + ".dll");
    const std::string target_lower = to_lower_ascii(target_dll);

    bool fwd_ord = false;
    uint16_t ord_val = 0;
    if (sym_part[0] == '#' && sym_part.size() > 1) {
        uint32_t v = 0;
        bool ok = true;
        for (size_t i = 1; i < sym_part.size(); ++i) {
            if (sym_part[i] < '0' || sym_part[i] > '9') {
                ok = false;
                break;
            }
            v = v * 10u + static_cast<uint32_t>(sym_part[i] - '0');
        }
        if (ok && v <= 0xFFFFu) {
            fwd_ord = true;
            ord_val = static_cast<uint16_t>(v);
        }
    }

    if (hle.has_module(target_lower)) {
        void* fn = fwd_ord ? hle.resolve_ordinal(target_lower, ord_val)
                           : hle.resolve_name(target_lower, sym_part);
        if (fn) return true;
        if (target_lower == "kernelx.dll") {
            return check_host_symbol("kernel32.dll", fwd_ord, ord_val, sym_part);
        }
        return false;
    }

    if (const PeImage* g = load_guest_cached(target_dll, search_dirs, cache)) {
        return resolve_guest_chain(*g, fwd_ord, ord_val, sym_part, hle, search_dirs, cache, depth + 1);
    }

    return check_host_symbol(target_dll, fwd_ord, ord_val, sym_part);
}

}  // namespace

Result<PeProbeReport> run_pe_probe(
    const fs::path& target,
    const hle::Registry& hle) {
    fs::path exe_path = target;
    fs::path game_root = fs::is_directory(target) ? target : target.parent_path();

    if (fs::is_directory(target)) {
        const fs::path cfg_path = target / "MicrosoftGame.config";
        if (fs::exists(cfg_path)) {
            auto cfg = load_game_config(cfg_path);
            if (cfg && !cfg->executables.empty()) {
                exe_path = target / cfg->executables.front().name;
            }
        }
    }

    if (!fs::is_regular_file(exe_path)) {
        return fail("PE probe target executable not found: " + exe_path.string());
    }

    auto pe = parse_pe64_file(exe_path);
    if (!pe) return std::unexpected(pe.error());

    PeProbeReport report{};
    report.target_path = exe_path.string();
    report.image_name = pe->filename;
    report.image_base = pe->image_base;
    report.size_of_image = pe->size_of_image;
    report.entry_point_rva = pe->entry_point_rva;
    report.subsystem = pe->subsystem;

    if (pe->data_directories.size() > IMAGE_DIRECTORY_ENTRY_BASERELOC) {
        const auto d = pe->data_directories[IMAGE_DIRECTORY_ENTRY_BASERELOC];
        report.has_relocations = (d.rva != 0 && d.size != 0);
    }
    if (pe->data_directories.size() > IMAGE_DIRECTORY_ENTRY_EXCEPTION) {
        const auto d = pe->data_directories[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        report.has_pdata_seh = (d.rva != 0 && d.size != 0);
    }
    if (pe->data_directories.size() > IMAGE_DIRECTORY_ENTRY_TLS) {
        const auto d = pe->data_directories[IMAGE_DIRECTORY_ENTRY_TLS];
        report.has_tls = (d.rva != 0 && d.size != 0);
    }

    std::vector<fs::path> search_dirs{game_root};
    if (auto ver_exp = find_export_in_image(*pe, false, 0, "D3D12SDKVersion"); ver_exp) {
        if (auto off = pe->rva_to_file_offset(ver_exp->rva); off && *off + 4 <= pe->raw.size()) {
            uint32_t ver = 0;
            std::memcpy(&ver, pe->raw.data() + *off, 4);
            report.agility_sdk_detected = true;
            report.agility_sdk_version = ver;
            if (auto path_exp = find_export_in_image(*pe, false, 0, "D3D12SDKPath"); path_exp) {
                if (auto poff = pe->rva_to_file_offset(path_exp->rva); poff && *poff + 8 <= pe->raw.size()) {
                    uint64_t va = 0;
                    std::memcpy(&va, pe->raw.data() + *poff, 8);
                    if (va >= pe->image_base && va < pe->image_base + pe->size_of_image) {
                        if (auto s = pe->read_cstring_at_rva(static_cast<uint32_t>(va - pe->image_base), 260)) {
                            report.agility_sdk_path = *s;
                            if (!s->empty()) search_dirs.push_back(game_root / *s);
                        }
                    }
                }
            }
        }
    }

    std::unordered_map<std::string, PeImage> guest_cache;
    for (const auto& imp : pe->imports) {
        PeModuleProbe mp{};
        mp.dll_name = imp.dll_name;
        const std::string lower_raw = to_lower_ascii(imp.dll_name);
        if (report.agility_sdk_detected && (lower_raw == "d3d12.dll" || lower_raw == "d3d12core.dll")) {
            mp.redirected_name = "d3d12_xs.dll";
        } else {
            mp.redirected_name = imp.dll_name;
        }
        mp.is_delay_load = imp.is_delay_load;

        const std::string redir_lower = to_lower_ascii(mp.redirected_name);
        mp.hle_registered = hle.has_module(redir_lower);

        const PeImage* guest_img = nullptr;
        if (!mp.hle_registered) {
            guest_img = load_guest_cached(mp.redirected_name, search_dirs, guest_cache);
            mp.guest_dll_found = (guest_img != nullptr);
        }

        HMODULE host_probe = nullptr;
        if (!mp.hle_registered && !guest_img) {
            host_probe = LoadLibraryExA(mp.redirected_name.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
            mp.host_dll_found = (host_probe != nullptr);
        } else if (redir_lower == "kernelx.dll") {
            mp.host_dll_found = true;
        }

        for (const auto& sym : imp.symbols) {
            ++mp.total_symbols;
            ++report.total_imports;
            const std::string sym_label = sym.by_ordinal ? ("#" + std::to_string(sym.ordinal)) : sym.name;

            bool resolved = false;
            if (mp.hle_registered) {
                void* p = sym.by_ordinal
                              ? hle.resolve_ordinal(redir_lower, sym.ordinal)
                              : hle.resolve_name(redir_lower, sym.name);
                if (p) {
                    ++mp.hle_resolved;
                    ++report.hle_resolved;
                    resolved = true;
                }
            }

            if (!resolved && guest_img) {
                if (resolve_guest_chain(*guest_img, sym.by_ordinal, sym.ordinal, sym.name,
                                        hle, search_dirs, guest_cache, 0)) {
                    ++mp.guest_resolved;
                    ++report.guest_resolved;
                    resolved = true;
                }
            }

            if (!resolved && mp.hle_registered && redir_lower == "kernelx.dll") {
                if (check_host_symbol("kernel32.dll", sym.by_ordinal, sym.ordinal, sym.name)) {
                    ++mp.host_resolved;
                    ++report.host_resolved;
                    resolved = true;
                }
            } else if (!resolved && host_probe) {
                FARPROC p = sym.by_ordinal
                                ? GetProcAddress(host_probe, MAKEINTRESOURCEA(sym.ordinal))
                                : GetProcAddress(host_probe, sym.name.c_str());
                if (p) {
                    ++mp.host_resolved;
                    ++report.host_resolved;
                    resolved = true;
                }
            }

            if (!resolved) {
                ++mp.missing_symbols;
                ++report.missing_imports;
                if (mp.missing_symbol_names.size() < 32) {
                    mp.missing_symbol_names.push_back(sym_label);
                }
            }
        }

        if (host_probe) FreeLibrary(host_probe);
        ++report.total_modules;
        report.modules.push_back(std::move(mp));
    }

    return report;
}

std::string format_pe_probe_text(const PeProbeReport& r) {
    const uint32_t resolved = r.hle_resolved + r.guest_resolved + r.host_resolved;
    const double pct = r.total_imports == 0
                           ? 100.0
                           : (100.0 * static_cast<double>(resolved) / static_cast<double>(r.total_imports));
    std::ostringstream oss;
    oss << std::format(
        "KestrelX PE32+ Compatibility Probe\n"
        "  Target        : {} ({})\n"
        "  Layout        : ImageBase={}, ImageSize={}, EntryRVA={}\n"
        "  Hardening     : Relocs={}, .pdata SEH={}, TLS={}, AgilitySDK={}{}\n"
        "  Import Audit  : {} / {} resolved ({:.1f}% | HLE={}, Guest={}, Host={}, Missing/Stub={})\n"
        "  Modules ({}):\n",
        r.image_name, r.target_path,
        hex64(r.image_base), hex32(r.size_of_image), hex32(r.entry_point_rva),
        r.has_relocations ? "yes" : "no",
        r.has_pdata_seh ? "yes" : "no",
        r.has_tls ? "yes" : "no",
        r.agility_sdk_detected ? std::format("v{}", r.agility_sdk_version) : "no",
        r.agility_sdk_path.empty() ? "" : (" [" + r.agility_sdk_path + "]"),
        resolved, r.total_imports, pct,
        r.hle_resolved, r.guest_resolved, r.host_resolved, r.missing_imports,
        r.total_modules);

    for (const auto& m : r.modules) {
        const char* kind = m.hle_registered ? "HLE"
                         : m.guest_dll_found ? "GuestDLL"
                         : m.host_dll_found ? "HostDLL"
                         : "Missing";
        oss << std::format("    - {:<24} [{:<8}{}] total={}, hle={}, guest={}, host={}, missing={}\n",
                           m.dll_name, kind, m.is_delay_load ? ",delay" : "",
                           m.total_symbols, m.hle_resolved, m.guest_resolved, m.host_resolved, m.missing_symbols);
        for (const auto& sym : m.missing_symbol_names) {
            oss << std::format("        * missing: {}\n", sym);
        }
    }
    return oss.str();
}

std::string format_pe_probe_json(const PeProbeReport& r) {
    std::ostringstream oss;
    oss << "{\n"
        << std::format("  \"target_path\": \"{}\",\n", json_escape(r.target_path))
        << std::format("  \"image_name\": \"{}\",\n", json_escape(r.image_name))
        << std::format("  \"image_base\": \"{}\",\n", hex64(r.image_base))
        << std::format("  \"size_of_image\": {},\n", r.size_of_image)
        << std::format("  \"entry_point_rva\": {},\n", r.entry_point_rva)
        << std::format("  \"has_relocations\": {},\n", r.has_relocations ? "true" : "false")
        << std::format("  \"has_pdata_seh\": {},\n", r.has_pdata_seh ? "true" : "false")
        << std::format("  \"has_tls\": {},\n", r.has_tls ? "true" : "false")
        << std::format("  \"agility_sdk_detected\": {},\n", r.agility_sdk_detected ? "true" : "false")
        << std::format("  \"agility_sdk_version\": {},\n", r.agility_sdk_version)
        << std::format("  \"agility_sdk_path\": \"{}\",\n", json_escape(r.agility_sdk_path))
        << std::format("  \"total_modules\": {},\n", r.total_modules)
        << std::format("  \"total_imports\": {},\n", r.total_imports)
        << std::format("  \"hle_resolved\": {},\n", r.hle_resolved)
        << std::format("  \"guest_resolved\": {},\n", r.guest_resolved)
        << std::format("  \"host_resolved\": {},\n", r.host_resolved)
        << std::format("  \"missing_imports\": {},\n", r.missing_imports)
        << "  \"modules\": [\n";
    for (size_t i = 0; i < r.modules.size(); ++i) {
        const auto& m = r.modules[i];
        oss << std::format(
            "    {{\"dll_name\":\"{}\",\"redirected_name\":\"{}\",\"is_delay_load\":{},\"hle_registered\":{},\"guest_dll_found\":{},\"host_dll_found\":{},\"total_symbols\":{},\"hle_resolved\":{},\"guest_resolved\":{},\"host_resolved\":{},\"missing_symbols\":{},\"missing_symbol_names\":[",
            json_escape(m.dll_name), json_escape(m.redirected_name),
            m.is_delay_load ? "true" : "false",
            m.hle_registered ? "true" : "false",
            m.guest_dll_found ? "true" : "false",
            m.host_dll_found ? "true" : "false",
            m.total_symbols, m.hle_resolved, m.guest_resolved, m.host_resolved, m.missing_symbols);
        for (size_t j = 0; j < m.missing_symbol_names.size(); ++j) {
            if (j > 0) oss << ",";
            oss << std::format("\"{}\"", json_escape(m.missing_symbol_names[j]));
        }
        oss << "]}";
        if (i + 1 < r.modules.size()) oss << ",";
        oss << "\n";
    }
    oss << "  ]\n}\n";
    return oss.str();
}

}  // namespace kestrel::loader
