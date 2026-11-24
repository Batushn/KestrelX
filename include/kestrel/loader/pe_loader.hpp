#pragma once

#include "kestrel/common/types.hpp"
#include "kestrel/hle/registry.hpp"
#include "kestrel/loader/pe_image.hpp"

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace kestrel::loader {

struct MappedModule {
    std::string name;
    std::filesystem::path full_path;
    uint8_t* base = nullptr;
    uint32_t size_of_image = 0;
    uint32_t entry_rva = 0;
    bool is_dll = false;
    PRUNTIME_FUNCTION pdata = nullptr;
    PeImage image;
    uint32_t relocs_applied = 0;
    uint32_t hle_imports = 0;
    uint32_t guest_imports = 0;
    uint32_t host_imports = 0;
    uint32_t stub_imports = 0;
    uint32_t tls_callbacks_run = 0;

    // Static TLS (.tls) template metadata for x64 TEB->ThreadLocalStoragePointer (gs:[0x58])
    uint32_t tls_slot = 0xFFFFFFFFu;
    uint64_t tls_raw_start = 0;
    size_t tls_raw_size = 0;
    size_t tls_zero_fill = 0;
    size_t tls_align = 16;

    void* find_export_by_name(std::string_view sym) const;
    void* find_export_by_ordinal(uint16_t ord) const;
};

struct AgilitySdkConfig {
    uint32_t sdk_version = 0;
    std::string sdk_path;
    bool found = false;
};

struct GuestRunOptions {
    std::string command_line_args;
    uint32_t timeout_ms = INFINITE;
};

// Registers host-DLL overlays on kernel32.dll / ucrtbase.dll / kernelx.dll so unmodified
// MSVC CRT Win32/GDK/D3D12 executables receive accurate guest process identity
// (GetModuleHandle, GetModuleFileName, GetCommandLine), dynamic loader bridging
// (LoadLibrary/GetProcAddress/FreeLibrary), static TLS propagation across CreateThread,
// isolated ExitProcess/exit/atexit handling, and console VFS path translation.
void register_guest_process_overlays(hle::Registry& reg);

class PeLoader {
public:
    PeLoader(hle::Registry& hle, std::filesystem::path search_dir);
    ~PeLoader();
    PeLoader(const PeLoader&) = delete;
    PeLoader& operator=(const PeLoader&) = delete;

    Result<MappedModule*> load_image(const std::filesystem::path& path);
    Result<MappedModule*> find_or_load_guest_dll(std::string_view dll_name);
    Result<int> execute_entry(MappedModule& mod);
    Result<int> execute_entry(MappedModule& mod, const GuestRunOptions& options);
    Result<void*> resolve_export_by_name(MappedModule& mod, std::string_view sym);
    Result<void*> resolve_export_by_ordinal(MappedModule& mod, uint16_t ord);

    // Dynamic GDK & Agility SDK DLL redirection
    void add_dll_redirection(std::string_view from_dll, std::string_view to_dll);
    std::string get_redirected_dll(std::string_view dll_name) const;
    void add_search_directory(const std::filesystem::path& dir);
    AgilitySdkConfig query_agility_sdk(const MappedModule& mod) const;
    void apply_agility_sdk_redirection(const MappedModule& mod);

    const std::vector<std::unique_ptr<MappedModule>>& modules() const { return modules_; }
    MappedModule* main_module() const { return main_module_; }
    hle::Registry& hle_registry() { return hle_; }
    const hle::Registry& hle_registry() const { return hle_; }

    // Internal guest runtime helpers used by host overlays
    MappedModule* find_loaded_module_by_name(std::string_view name) const;
    MappedModule* find_loaded_module_by_handle(HMODULE hmod) const;
    MappedModule* find_loaded_module_containing(const void* addr) const;
    HMODULE get_or_create_hle_handle(std::string_view dll_lower);
    bool lookup_hle_handle(HMODULE hmod, std::string& out_dll_lower) const;
    const std::string& guest_cmdline_a() const { return cmdline_a_; }
    const std::wstring& guest_cmdline_w() const { return cmdline_w_; }
    const std::string& guest_args_only_a() const { return args_only_a_; }
    const std::wstring& guest_args_only_w() const { return args_only_w_; }
    void record_guest_exit(int code);
    void register_guest_atexit(void(__cdecl* fn)());
    void run_guest_atexit();
    void ensure_current_thread_tls();
    void invoke_tls_callbacks_for_thread(DWORD reason);

private:
    static constexpr int kMaxForwarderDepth = 8;
    static constexpr uint32_t kGuestTlsBaseSlot = 64u;
    static constexpr uint32_t kMaxTlsSlots = 128u;

    Result<void*> resolve_export_symbol(MappedModule& mod, const PeExportSymbol& exp, int depth);
    Result<void*> resolve_guest_export_by_name(MappedModule& mod, std::string_view sym, int depth);
    Result<void*> resolve_guest_export_by_ordinal(MappedModule& mod, uint16_t ord, int depth);
    Result<void*> resolve_forwarder(std::string_view forwarder, int depth);
    Result<void> apply_relocations(MappedModule& mod);
    Result<void> resolve_imports(MappedModule& mod);
    void setup_static_tls(MappedModule& mod);
    Result<void> protect_sections(MappedModule& mod);
    void register_exception_table(MappedModule& mod);
    void run_tls_callbacks(MappedModule& mod, DWORD reason = DLL_PROCESS_ATTACH);
    void rollback_module(const std::string& key, MappedModule* target);
    std::string format_address(uint64_t addr) const;

    hle::Registry& hle_;
    std::filesystem::path search_dir_;
    std::vector<std::filesystem::path> extra_search_dirs_;
    std::unordered_map<std::string, std::string> dll_redirections_;
    std::vector<std::unique_ptr<MappedModule>> modules_;
    std::unordered_map<std::string, MappedModule*> by_name_;
    MappedModule* main_module_ = nullptr;
    uint32_t next_tls_slot_ = kGuestTlsBaseSlot;

    mutable std::mutex runtime_mtx_;
    std::unordered_map<std::string, uintptr_t> hle_handles_by_name_;
    std::unordered_map<uintptr_t, std::string> hle_names_by_handle_;
    uintptr_t next_hle_handle_ = 0x4B58000000000100ull;

    std::string cmdline_a_;
    std::wstring cmdline_w_;
    std::string args_only_a_;
    std::wstring args_only_w_;
    std::atomic<bool> guest_exited_{false};
    std::atomic<int> guest_exit_code_{0};
    std::vector<void(__cdecl*)()> guest_atexit_;
};

}  // namespace kestrel::loader
