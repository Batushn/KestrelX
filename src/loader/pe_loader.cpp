#include "kestrel/loader/pe_loader.hpp"

#include <cstring>

namespace kestrel::loader {

void* MappedModule::find_export_by_name(std::string_view sym) const {
    for (const auto& e : image.exports) {
        if (e.name == sym && e.forwarder.empty()) return base + e.rva;
    }
    return nullptr;
}

void* MappedModule::find_export_by_ordinal(uint16_t ord) const {
    for (const auto& e : image.exports) {
        if (e.ordinal == ord && e.forwarder.empty()) return base + e.rva;
    }
    return nullptr;
}

PeLoader::PeLoader(hle::Registry& hle, std::filesystem::path search_dir)
    : hle_(hle), search_dir_(std::move(search_dir)) {}

PeLoader::~PeLoader() {
    for (auto it = modules_.rbegin(); it != modules_.rend(); ++it) {
        auto& mod = **it;
        if (mod.pdata) { RtlDeleteFunctionTable(mod.pdata); mod.pdata = nullptr; }
        if (mod.base) { VirtualFree(mod.base, 0, MEM_RELEASE); mod.base = nullptr; }
    }
}

Result<MappedModule*> PeLoader::find_or_load_guest_dll(std::string_view dll_name) {
    const std::string key = to_lower_ascii(dll_name);
    if (auto it = by_name_.find(key); it != by_name_.end()) return it->second;
    const auto candidate = search_dir_ / dll_name;
    if (!std::filesystem::is_regular_file(candidate)) return nullptr;
    return load_image(candidate);
}

Result<MappedModule*> PeLoader::load_image(const std::filesystem::path& path) {
    const std::string key = to_lower_ascii(path.filename().string());
    if (auto it = by_name_.find(key); it != by_name_.end()) return it->second;

    auto parsed = parse_pe64_file(path);
    if (!parsed) return std::unexpected(parsed.error());

    auto mod = std::make_unique<MappedModule>();
    mod->name = path.filename().string();
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

    const size_t hdr_bytes = std::min<size_t>(mod->image.size_of_headers, mod->image.raw.size());
    std::memcpy(mod->base, mod->image.raw.data(), hdr_bytes);

    for (const auto& sec : mod->image.sections) {
        if (sec.raw_size == 0) continue;
        if (static_cast<size_t>(sec.raw_offset) + sec.raw_size > mod->image.raw.size() ||
            static_cast<size_t>(sec.virtual_address) + sec.raw_size > mod->size_of_image) {
            return fail("Section out of bounds in " + mod->name + ": " + sec.name);
        }
        std::memcpy(mod->base + sec.virtual_address, mod->image.raw.data() + sec.raw_offset, sec.raw_size);
    }

    MappedModule* raw_ptr = mod.get();
    by_name_[key] = raw_ptr;
    modules_.push_back(std::move(mod));

    if (auto r = apply_relocations(*raw_ptr); !r) return std::unexpected(r.error());
    if (auto r = resolve_imports(*raw_ptr); !r) return std::unexpected(r.error());
    if (auto r = protect_sections(*raw_ptr); !r) return std::unexpected(r.error());
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
    const int64_t delta = reinterpret_cast<int64_t>(mod.base) - static_cast<int64_t>(mod.image.image_base);
    if (delta == 0) return {};

    const auto dir = mod.image.data_directories[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (dir.rva == 0 || dir.size == 0) {
        return fail("Image requires relocation but has no base relocation table: " + mod.name);
    }

    uint32_t offset = 0;
    while (offset + sizeof(IMAGE_BASE_RELOCATION) <= dir.size) {
        auto* block = reinterpret_cast<const IMAGE_BASE_RELOCATION*>(mod.base + dir.rva + offset);
        if (block->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION) || offset + block->SizeOfBlock > dir.size) break;
        const uint32_t count = (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(uint16_t);
        const auto* entries = reinterpret_cast<const uint16_t*>(block + 1);
        for (uint32_t i = 0; i < count; ++i) {
            const uint16_t type = entries[i] >> 12;
            const uint16_t rel = entries[i] & 0x0FFF;
            const uint32_t target_rva = block->VirtualAddress + rel;
            if (type == IMAGE_REL_BASED_ABSOLUTE) continue;
            if (type == IMAGE_REL_BASED_DIR64) {
                if (target_rva + 8 > mod.size_of_image) return fail("DIR64 relocation out of bounds");
                uint64_t val = 0;
                std::memcpy(&val, mod.base + target_rva, 8);
                val = static_cast<uint64_t>(static_cast<int64_t>(val) + delta);
                std::memcpy(mod.base + target_rva, &val, 8);
                ++mod.relocs_applied;
            } else if (type == IMAGE_REL_BASED_HIGHLOW) {
                if (target_rva + 4 > mod.size_of_image) return fail("HIGHLOW relocation out of bounds");
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
        const std::string dll_lower = to_lower_ascii(imp_mod.dll_name);
        MappedModule* guest_dll = nullptr;
        HMODULE host_dll = nullptr;

        if (!hle_.has_module(dll_lower)) {
            auto g = find_or_load_guest_dll(imp_mod.dll_name);
            if (!g) return std::unexpected(g.error());
            guest_dll = *g;
            if (!guest_dll) host_dll = LoadLibraryA(imp_mod.dll_name.c_str());
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
                resolved = sym.by_ordinal
                               ? guest_dll->find_export_by_ordinal(sym.ordinal)
                               : guest_dll->find_export_by_name(sym.name);
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
            if (sym.iat_rva + 8 > mod.size_of_image) return fail("IAT entry out of bounds in " + mod.name);
            const uint64_t addr = reinterpret_cast<uint64_t>(resolved);
            std::memcpy(mod.base + sym.iat_rva, &addr, 8);
        }
    }
    return {};
}

Result<void> PeLoader::protect_sections(MappedModule& mod) {
    DWORD old = 0;
    VirtualProtect(mod.base, mod.image.size_of_headers, PAGE_READONLY, &old);
    for (const auto& sec : mod.image.sections) {
        const uint32_t span = std::max(sec.virtual_size, sec.raw_size);
        if (span == 0) continue;
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
    const auto dir = mod.image.data_directories[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (dir.rva == 0 || dir.size == 0) return;
    auto* table = reinterpret_cast<PRUNTIME_FUNCTION>(mod.base + dir.rva);
    const DWORD count = dir.size / sizeof(RUNTIME_FUNCTION);
    if (count > 0 && RtlAddFunctionTable(table, count, reinterpret_cast<DWORD64>(mod.base))) {
        mod.pdata = table;
    }
}

void PeLoader::run_tls_callbacks(MappedModule& mod) {
    const auto dir = mod.image.data_directories[IMAGE_DIRECTORY_ENTRY_TLS];
    if (dir.rva == 0 || dir.size < sizeof(IMAGE_TLS_DIRECTORY64)) return;
    const auto* tls = reinterpret_cast<const IMAGE_TLS_DIRECTORY64*>(mod.base + dir.rva);
    if (tls->AddressOfIndex != 0) *reinterpret_cast<uint32_t*>(tls->AddressOfIndex) = 0;
    if (tls->AddressOfCallBacks == 0) return;
    auto* cbs = reinterpret_cast<PIMAGE_TLS_CALLBACK*>(tls->AddressOfCallBacks);
    for (size_t i = 0; i < 64 && cbs[i] != nullptr; ++i) {
        cbs[i](mod.base, DLL_PROCESS_ATTACH, nullptr);
        ++mod.tls_callbacks_run;
    }
}

Result<int> PeLoader::execute_entry(MappedModule& mod) {
    if (mod.entry_rva == 0) return fail("Module has no entry point: " + mod.name);
    using EntryFn = int(__cdecl*)();
    auto fn = reinterpret_cast<EntryFn>(mod.base + mod.entry_rva);
    return fn();
}

}  // namespace kestrel::loader
