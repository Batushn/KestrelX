#include "kestrel/loader/pe_loader.hpp"

#include <algorithm>
#include <cstring>

namespace kestrel::loader {

namespace {

bool span_in_bounds(uint64_t offset, uint64_t size, uint64_t limit) {
    return offset <= limit && size <= limit - offset;
}

void release_mapped_module(MappedModule& mod) {
    if (mod.pdata) {
        RtlDeleteFunctionTable(mod.pdata);
        mod.pdata = nullptr;
    }
    if (mod.base) {
        VirtualFree(mod.base, 0, MEM_RELEASE);
        mod.base = nullptr;
    }
}

}  // namespace

void* MappedModule::find_export_by_name(std::string_view sym) const {
    for (const auto& e : image.exports) {
        if (e.name == sym && e.forwarder.empty() && e.rva < size_of_image) return base + e.rva;
    }
    return nullptr;
}

void* MappedModule::find_export_by_ordinal(uint16_t ord) const {
    for (const auto& e : image.exports) {
        if (e.ordinal == ord && e.forwarder.empty() && e.rva < size_of_image) return base + e.rva;
    }
    return nullptr;
}

PeLoader::PeLoader(hle::Registry& hle, std::filesystem::path search_dir)
    : hle_(hle), search_dir_(std::move(search_dir)) {}

PeLoader::~PeLoader() {
    for (auto it = modules_.rbegin(); it != modules_.rend(); ++it) {
        release_mapped_module(**it);
    }
}

void PeLoader::rollback_module(const std::string& key, MappedModule* target) {
    if (auto it = by_name_.find(key); it != by_name_.end() && it->second == target) {
        by_name_.erase(it);
    }
    for (auto it = modules_.begin(); it != modules_.end(); ++it) {
        if (it->get() == target) {
            release_mapped_module(**it);
            modules_.erase(it);
            return;
        }
    }
}

Result<MappedModule*> PeLoader::find_or_load_guest_dll(std::string_view dll_name) {
    const std::string redirected = get_redirected_dll(dll_name);
    const std::string key = to_lower_ascii(redirected);
    if (auto it = by_name_.find(key); it != by_name_.end()) return it->second;
    const auto candidate = search_dir_ / redirected;
    if (std::filesystem::is_regular_file(candidate)) return load_image(candidate);
    for (const auto& dir : extra_search_dirs_) {
        const auto cand = dir / redirected;
        if (std::filesystem::is_regular_file(cand)) return load_image(cand);
    }
    return nullptr;
}

Result<void*> PeLoader::resolve_export_by_name(MappedModule& mod, std::string_view sym) {
    return resolve_guest_export_by_name(mod, sym, 0);
}

Result<void*> PeLoader::resolve_export_by_ordinal(MappedModule& mod, uint16_t ord) {
    return resolve_guest_export_by_ordinal(mod, ord, 0);
}

Result<void*> PeLoader::resolve_export_symbol(MappedModule& mod, const PeExportSymbol& exp, int depth) {
    if (!exp.forwarder.empty()) {
        return resolve_forwarder(exp.forwarder, depth);
    }
    if (exp.rva >= mod.size_of_image) {
        return fail("Export RVA out of bounds in " + mod.name + ": " + hex32(exp.rva));
    }
    return mod.base + exp.rva;
}

Result<void*> PeLoader::resolve_guest_export_by_name(MappedModule& mod, std::string_view sym, int depth) {
    for (const auto& e : mod.image.exports) {
        if (e.name == sym) {
            return resolve_export_symbol(mod, e, depth);
        }
    }
    return nullptr;
}

Result<void*> PeLoader::resolve_guest_export_by_ordinal(MappedModule& mod, uint16_t ord, int depth) {
    for (const auto& e : mod.image.exports) {
        if (e.ordinal == ord) {
            return resolve_export_symbol(mod, e, depth);
        }
    }
    return nullptr;
}

Result<void*> PeLoader::resolve_forwarder(std::string_view forwarder, int depth) {
    if (depth >= kMaxForwarderDepth) {
        return fail("Export forwarder recursion limit exceeded: " + std::string(forwarder));
    }

    const size_t dot = forwarder.rfind('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 >= forwarder.size()) {
        return nullptr;
    }

    const std::string_view dll_part = forwarder.substr(0, dot);
    const std::string_view sym_part = forwarder.substr(dot + 1);

    bool by_ordinal = false;
    uint16_t ordinal = 0;
    if (sym_part[0] == '#') {
        if (sym_part.size() == 1) return nullptr;
        uint32_t parsed_ord = 0;
        for (size_t i = 1; i < sym_part.size(); ++i) {
            const char c = sym_part[i];
            if (c < '0' || c > '9') return nullptr;
            parsed_ord = parsed_ord * 10u + static_cast<uint32_t>(c - '0');
            if (parsed_ord > 0xFFFFu) return nullptr;
        }
        by_ordinal = true;
        ordinal = static_cast<uint16_t>(parsed_ord);
    }

    const std::string sym_str(sym_part);
    const std::string dll_lower_raw = to_lower_ascii(dll_part);
    const std::string raw_target = dll_lower_raw.ends_with(".dll")
                                       ? std::string(dll_part)
                                       : (std::string(dll_part) + ".dll");
    const std::string target_dll = get_redirected_dll(raw_target);
    const std::string target_lower = to_lower_ascii(target_dll);

    // 1. Check HLE registry first
    if (hle_.has_module(target_lower)) {
        void* p = by_ordinal ? hle_.resolve_ordinal(target_lower, ordinal)
                             : hle_.resolve_name(target_lower, sym_str);
        if (p) return p;
    } else if (target_lower != dll_lower_raw && hle_.has_module(dll_lower_raw)) {
        void* p = by_ordinal ? hle_.resolve_ordinal(dll_lower_raw, ordinal)
                             : hle_.resolve_name(dll_lower_raw, sym_str);
        if (p) return p;
    }

    // 2. Check loaded or on-disk guest DLLs
    if (!hle_.has_module(target_lower)) {
        auto guest = find_or_load_guest_dll(target_dll);
        if (!guest) return std::unexpected(guest.error());
        if (*guest) {
            auto resolved = by_ordinal ? resolve_guest_export_by_ordinal(**guest, ordinal, depth + 1)
                                       : resolve_guest_export_by_name(**guest, sym_str, depth + 1);
            if (!resolved) return std::unexpected(resolved.error());
            if (*resolved) return *resolved;
        }
    }

    // 3. Fall back to host DLLs (LoadLibraryA / GetProcAddress)
    HMODULE host_dll = nullptr;
    if (hle_.has_module(target_lower)) {
        if (target_lower == "kernelx.dll") {
            host_dll = GetModuleHandleA("kernel32.dll");
        }
    } else {
        host_dll = GetModuleHandleA(target_dll.c_str());
        if (!host_dll) host_dll = LoadLibraryA(target_dll.c_str());
    }
    if (host_dll) {
        FARPROC p = by_ordinal ? GetProcAddress(host_dll, MAKEINTRESOURCEA(ordinal))
                               : GetProcAddress(host_dll, sym_str.c_str());
        if (p) return reinterpret_cast<void*>(p);
    }

    return nullptr;
}

Result<MappedModule*> PeLoader::load_image(const std::filesystem::path& path) {
    const std::string key = to_lower_ascii(path.filename().string());
    if (auto it = by_name_.find(key); it != by_name_.end()) return it->second;

    auto parsed = parse_pe64_file(path);
    if (!parsed) return std::unexpected(parsed.error());

    const std::string mod_name = path.filename().string();
    if (parsed->size_of_image == 0) {
        return fail("Invalid PE image with SizeOfImage == 0: " + mod_name);
    }
    if (parsed->size_of_headers > parsed->size_of_image) {
        return fail("SizeOfHeaders exceeds SizeOfImage in " + mod_name);
    }
    if (parsed->entry_point_rva != 0 && parsed->entry_point_rva >= parsed->size_of_image) {
        return fail("Entry point RVA out of bounds in " + mod_name);
    }

    for (const auto& sec : parsed->sections) {
        if (!span_in_bounds(sec.virtual_address, sec.virtual_size, parsed->size_of_image) ||
            !span_in_bounds(sec.virtual_address, sec.raw_size, parsed->size_of_image)) {
            return fail("Section virtual span out of bounds in " + mod_name + ": " + sec.name);
        }
        if (sec.raw_size > 0 && !span_in_bounds(sec.raw_offset, sec.raw_size, parsed->raw.size())) {
            return fail("Section raw span out of bounds in " + mod_name + ": " + sec.name);
        }
    }

    auto mod = std::make_unique<MappedModule>();
    mod->name = mod_name;
    mod->size_of_image = parsed->size_of_image;
    mod->entry_rva = parsed->entry_point_rva;
    mod->is_dll = parsed->is_dll;
    mod->image = std::move(*parsed);

    void* mem = VirtualAlloc(
        reinterpret_cast<void*>(mod->image.image_base),
        mod->size_of_image, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem) mem = VirtualAlloc(nullptr, mod->size_of_image, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem) return fail("VirtualAlloc failed for " + mod->name);
    mod->base = static_cast<uint8_t*>(mem);

    const size_t hdr_bytes = std::min<size_t>({mod->image.size_of_headers, mod->image.raw.size(), mod->size_of_image});
    std::memcpy(mod->base, mod->image.raw.data(), hdr_bytes);

    for (const auto& sec : mod->image.sections) {
        if (sec.raw_size == 0) continue;
        std::memcpy(mod->base + sec.virtual_address, mod->image.raw.data() + sec.raw_offset, sec.raw_size);
    }

    MappedModule* raw_ptr = mod.get();
    by_name_[key] = raw_ptr;
    modules_.push_back(std::move(mod));

    if (auto r = apply_relocations(*raw_ptr); !r) {
        rollback_module(key, raw_ptr);
        return std::unexpected(r.error());
    }
    if (!raw_ptr->is_dll) {
        apply_agility_sdk_redirection(*raw_ptr);
    }
    if (auto r = resolve_imports(*raw_ptr); !r) {
        rollback_module(key, raw_ptr);
        return std::unexpected(r.error());
    }
    if (auto r = protect_sections(*raw_ptr); !r) {
        rollback_module(key, raw_ptr);
        return std::unexpected(r.error());
    }
    register_exception_table(*raw_ptr);
    run_tls_callbacks(*raw_ptr);

    if (raw_ptr->is_dll && raw_ptr->entry_rva != 0) {
        using DllMainFn = BOOL(WINAPI*)(HINSTANCE, DWORD, LPVOID);
        auto fn = reinterpret_cast<DllMainFn>(raw_ptr->base + raw_ptr->entry_rva);
        fn(reinterpret_cast<HINSTANCE>(raw_ptr->base), DLL_PROCESS_ATTACH, nullptr);
    }
    return raw_ptr;
}

Result<void> PeLoader::apply_relocations(MappedModule& mod) {
    if (mod.size_of_image == 0 || mod.image.data_directories.size() <= IMAGE_DIRECTORY_ENTRY_BASERELOC) {
        return fail("Invalid image metadata during relocation: " + mod.name);
    }

    const auto dir = mod.image.data_directories[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if ((dir.rva != 0 || dir.size != 0) && !span_in_bounds(dir.rva, dir.size, mod.size_of_image)) {
        return fail("Base relocation directory out of bounds in " + mod.name);
    }

    const int64_t delta = reinterpret_cast<int64_t>(mod.base) - static_cast<int64_t>(mod.image.image_base);
    if (delta == 0) return {};

    if (dir.rva == 0 || dir.size == 0) {
        return fail("Image requires relocation but has no base relocation table: " + mod.name);
    }

    uint32_t offset = 0;
    while (dir.size - offset >= sizeof(IMAGE_BASE_RELOCATION)) {
        auto* block = reinterpret_cast<const IMAGE_BASE_RELOCATION*>(mod.base + dir.rva + offset);
        if (block->SizeOfBlock == 0) break;
        if (block->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION) || block->SizeOfBlock > dir.size - offset) {
            return fail("Malformed base relocation block in " + mod.name);
        }
        const uint32_t count = (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(uint16_t);
        const auto* entries = reinterpret_cast<const uint16_t*>(block + 1);
        for (uint32_t i = 0; i < count; ++i) {
            const uint16_t type = entries[i] >> 12;
            const uint16_t rel = entries[i] & 0x0FFFu;
            const uint64_t target_rva = static_cast<uint64_t>(block->VirtualAddress) + rel;
            if (type == IMAGE_REL_BASED_ABSOLUTE) continue;
            if (type == IMAGE_REL_BASED_DIR64) {
                if (!span_in_bounds(target_rva, 8, mod.size_of_image)) {
                    return fail("DIR64 relocation out of bounds in " + mod.name);
                }
                uint64_t val = 0;
                std::memcpy(&val, mod.base + target_rva, 8);
                val = static_cast<uint64_t>(static_cast<int64_t>(val) + delta);
                std::memcpy(mod.base + target_rva, &val, 8);
                ++mod.relocs_applied;
            } else if (type == IMAGE_REL_BASED_HIGHLOW) {
                if (!span_in_bounds(target_rva, 4, mod.size_of_image)) {
                    return fail("HIGHLOW relocation out of bounds in " + mod.name);
                }
                uint32_t val = 0;
                std::memcpy(&val, mod.base + target_rva, 4);
                val = static_cast<uint32_t>(val + static_cast<int32_t>(delta));
                std::memcpy(mod.base + target_rva, &val, 4);
                ++mod.relocs_applied;
            }
        }
        offset += block->SizeOfBlock;
    }
    return {};
}

Result<void> PeLoader::resolve_imports(MappedModule& mod) {
    for (const auto& imp_mod : mod.image.imports) {
        const std::string redirected_name = get_redirected_dll(imp_mod.dll_name);
        const std::string dll_lower = to_lower_ascii(redirected_name);
        MappedModule* guest_dll = nullptr;
        HMODULE host_dll = nullptr;

        if (!hle_.has_module(dll_lower)) {
            auto g = find_or_load_guest_dll(redirected_name);
            if (!g) return std::unexpected(g.error());
            guest_dll = *g;
            if (!guest_dll) host_dll = LoadLibraryA(redirected_name.c_str());
        }

        for (const auto& sym : imp_mod.symbols) {
            void* resolved = nullptr;
            if (hle_.has_module(dll_lower)) {
                resolved = sym.by_ordinal
                               ? hle_.resolve_ordinal(dll_lower, sym.ordinal)
                               : hle_.resolve_name(dll_lower, sym.name);
                if (resolved) ++mod.hle_imports;
            }
            if (!resolved && guest_dll) {
                auto gres = sym.by_ordinal
                                ? resolve_guest_export_by_ordinal(*guest_dll, sym.ordinal, 0)
                                : resolve_guest_export_by_name(*guest_dll, sym.name, 0);
                if (!gres) return std::unexpected(gres.error());
                resolved = *gres;
                if (resolved) ++mod.guest_imports;
            }
            if (!resolved && !host_dll && hle_.has_module(dll_lower) && dll_lower == "kernelx.dll") {
                host_dll = GetModuleHandleA("kernel32.dll");
            }
            if (!resolved && host_dll) {
                FARPROC p = sym.by_ordinal
                                ? GetProcAddress(host_dll, MAKEINTRESOURCEA(sym.ordinal))
                                : GetProcAddress(host_dll, sym.name.c_str());
                if (p) { resolved = reinterpret_cast<void*>(p); ++mod.host_imports; }
            }
            if (!resolved) {
                const std::string label = sym.by_ordinal ? ("#" + std::to_string(sym.ordinal)) : sym.name;
                resolved = hle_.get_or_create_stub(imp_mod.dll_name, label);
                ++mod.stub_imports;
            }
            if (!span_in_bounds(sym.iat_rva, 8, mod.size_of_image)) {
                return fail("IAT entry out of bounds in " + mod.name);
            }
            const uint64_t addr = reinterpret_cast<uint64_t>(resolved);
            std::memcpy(mod.base + sym.iat_rva, &addr, 8);
        }
    }
    return {};
}

Result<void> PeLoader::protect_sections(MappedModule& mod) {
    DWORD old = 0;
    if (mod.image.size_of_headers > 0 && mod.image.size_of_headers <= mod.size_of_image) {
        VirtualProtect(mod.base, mod.image.size_of_headers, PAGE_READONLY, &old);
    }
    for (const auto& sec : mod.image.sections) {
        const uint32_t span = std::max(sec.virtual_size, sec.raw_size);
        if (span == 0) continue;
        if (!span_in_bounds(sec.virtual_address, span, mod.size_of_image)) {
            return fail("Section protection span out of bounds in " + mod.name + ": " + sec.name);
        }
        const bool x = (sec.characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
        const bool r = (sec.characteristics & IMAGE_SCN_MEM_READ) != 0;
        const bool w = (sec.characteristics & IMAGE_SCN_MEM_WRITE) != 0;
        DWORD prot = PAGE_NOACCESS;
        if (x && w) prot = PAGE_EXECUTE_READWRITE;
        else if (x && r) prot = PAGE_EXECUTE_READ;
        else if (x) prot = PAGE_EXECUTE;
        else if (w) prot = PAGE_READWRITE;
        else if (r) prot = PAGE_READONLY;
        VirtualProtect(mod.base + sec.virtual_address, span, prot, &old);
    }
    FlushInstructionCache(GetCurrentProcess(), mod.base, mod.size_of_image);
    return {};
}

void PeLoader::register_exception_table(MappedModule& mod) {
    if (mod.size_of_image == 0 || mod.image.data_directories.size() <= IMAGE_DIRECTORY_ENTRY_EXCEPTION) return;
    const auto dir = mod.image.data_directories[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (dir.rva == 0 || dir.size == 0) return;
    if (!span_in_bounds(dir.rva, dir.size, mod.size_of_image)) return;
    if ((dir.rva % alignof(RUNTIME_FUNCTION)) != 0) return;
    auto* table = reinterpret_cast<PRUNTIME_FUNCTION>(mod.base + dir.rva);
    const DWORD count = dir.size / sizeof(RUNTIME_FUNCTION);
    if (count > 0 && RtlAddFunctionTable(table, count, reinterpret_cast<DWORD64>(mod.base))) {
        mod.pdata = table;
    }
}

void PeLoader::run_tls_callbacks(MappedModule& mod) {
    if (mod.size_of_image == 0 || mod.image.data_directories.size() <= IMAGE_DIRECTORY_ENTRY_TLS) return;
    const auto dir = mod.image.data_directories[IMAGE_DIRECTORY_ENTRY_TLS];
    if (dir.rva == 0 || dir.size < sizeof(IMAGE_TLS_DIRECTORY64)) return;
    if (!span_in_bounds(dir.rva, dir.size, mod.size_of_image)) return;
    const auto* tls = reinterpret_cast<const IMAGE_TLS_DIRECTORY64*>(mod.base + dir.rva);
    const uint64_t base_va = reinterpret_cast<uint64_t>(mod.base);
    if (tls->AddressOfIndex != 0) {
        if (tls->AddressOfIndex < base_va ||
            !span_in_bounds(tls->AddressOfIndex - base_va, sizeof(uint32_t), mod.size_of_image)) {
            return;
        }
        *reinterpret_cast<uint32_t*>(tls->AddressOfIndex) = 0;
    }
    if (tls->AddressOfCallBacks == 0) return;
    if (tls->AddressOfCallBacks < base_va) return;
    const uint64_t cbs_rva = tls->AddressOfCallBacks - base_va;
    auto* cbs = reinterpret_cast<PIMAGE_TLS_CALLBACK*>(tls->AddressOfCallBacks);
    for (size_t i = 0; i < 64; ++i) {
        if (!span_in_bounds(cbs_rva + i * sizeof(PIMAGE_TLS_CALLBACK), sizeof(PIMAGE_TLS_CALLBACK), mod.size_of_image)) {
            break;
        }
        if (cbs[i] == nullptr) break;
        const uint64_t cb_va = reinterpret_cast<uint64_t>(cbs[i]);
        if (cb_va < base_va || cb_va - base_va >= mod.size_of_image) break;
        cbs[i](mod.base, DLL_PROCESS_ATTACH, nullptr);
        ++mod.tls_callbacks_run;
    }
}

void PeLoader::add_dll_redirection(std::string_view from_dll, std::string_view to_dll) {
    dll_redirections_[to_lower_ascii(from_dll)] = std::string(to_dll);
}

std::string PeLoader::get_redirected_dll(std::string_view dll_name) const {
    const std::string lower = to_lower_ascii(dll_name);
    if (auto it = dll_redirections_.find(lower); it != dll_redirections_.end()) {
        return it->second;
    }
    return std::string(dll_name);
}

void PeLoader::add_search_directory(const std::filesystem::path& dir) {
    extra_search_dirs_.push_back(dir);
}

AgilitySdkConfig PeLoader::query_agility_sdk(const MappedModule& mod) const {
    AgilitySdkConfig cfg;
    void* ver_ptr = mod.find_export_by_name("D3D12SDKVersion");
    if (!ver_ptr) return cfg;

    cfg.sdk_version = *reinterpret_cast<const uint32_t*>(ver_ptr);
    cfg.found = true;

    void* path_ptr = mod.find_export_by_name("D3D12SDKPath");
    if (path_ptr) {
        uint64_t str_va = *reinterpret_cast<const uint64_t*>(path_ptr);
        const uint64_t base_va = reinterpret_cast<uint64_t>(mod.base);
        const char* s = nullptr;
        size_t max_len = 260;

        if (str_va >= base_va && str_va < base_va + mod.size_of_image) {
            s = reinterpret_cast<const char*>(str_va);
            max_len = static_cast<size_t>((base_va + mod.size_of_image) - str_va);
            if (max_len > 260) max_len = 260;
        } else if (mod.image.image_base != 0 &&
                   str_va >= mod.image.image_base &&
                   str_va < mod.image.image_base + mod.size_of_image) {
            uint64_t rva = str_va - mod.image.image_base;
            s = reinterpret_cast<const char*>(mod.base + rva);
            max_len = static_cast<size_t>(mod.size_of_image - rva);
            if (max_len > 260) max_len = 260;
        } else {
            s = reinterpret_cast<const char*>(path_ptr);
        }

        if (s) {
            size_t len = 0;
            while (len < max_len && s[len] != '\0') ++len;
            cfg.sdk_path = std::string(s, len);
        }
    }
    return cfg;
}

void PeLoader::apply_agility_sdk_redirection(const MappedModule& mod) {
    auto cfg = query_agility_sdk(mod);
    if (!cfg.found) return;

    if (!cfg.sdk_path.empty()) {
        auto extra_dir = search_dir_ / cfg.sdk_path;
        add_search_directory(extra_dir);
    }
    add_dll_redirection("d3d12.dll", "d3d12_xs.dll");
    add_dll_redirection("d3d12core.dll", "d3d12_xs.dll");
}

Result<int> PeLoader::execute_entry(MappedModule& mod) {
    if (mod.entry_rva == 0 || mod.entry_rva >= mod.size_of_image) {
        return fail("Module has no valid entry point: " + mod.name);
    }
    using EntryFn = int(__cdecl*)();
    auto fn = reinterpret_cast<EntryFn>(mod.base + mod.entry_rva);
    return fn();
}

}  // namespace kestrel::loader
