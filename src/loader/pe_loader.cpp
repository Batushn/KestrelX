#include "kestrel/loader/pe_loader.hpp"

#include "kestrel/hle/kernelx.hpp"

#include <intrin.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace kestrel::loader {

namespace {

bool span_in_bounds(uint64_t offset, uint64_t length, uint64_t limit) {
    return offset <= limit && length <= (limit - offset);
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

std::string wide_to_utf8(LPCWSTR ws) {
    if (!ws || !*ws) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws, -1, out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring utf8_to_wide(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string basename_lower(std::string_view path_or_name) {
    size_t pos = path_or_name.find_last_of("\\/");
    std::string_view base = (pos == std::string_view::npos) ? path_or_name : path_or_name.substr(pos + 1);
    return to_lower_ascii(base);
}

// Native ntdll-managed static TLS slot provider (kestrel_tls_slot.dll).
// Never mutates TEB->ThreadLocalStoragePointer (gs:[0x58]) or touches ntdll's internal heap!
struct NativeTlsSlotProvider {
    HMODULE hmod = nullptr;
    uint32_t slot_index = 0xFFFFFFFFu;
    uint32_t capacity = 0;
};

NativeTlsSlotProvider& get_native_tls_provider() {
    static NativeTlsSlotProvider provider = []() {
        NativeTlsSlotProvider p{};
        wchar_t exe_path[MAX_PATH]{};
        HMODULE h = nullptr;
        if (GetModuleFileNameW(nullptr, exe_path, MAX_PATH) > 0) {
            std::filesystem::path dll_path = std::filesystem::path(exe_path).parent_path() / L"kestrel_tls_slot.dll";
            if (std::filesystem::is_regular_file(dll_path)) {
                h = LoadLibraryW(dll_path.c_str());
            }
        }
        if (!h) {
            h = LoadLibraryW(L"kestrel_tls_slot.dll");
        }
        if (h) {
            using GetIndexFn = ULONG(__cdecl*)();
            auto get_idx = reinterpret_cast<GetIndexFn>(GetProcAddress(h, "kestrel_tls_get_index"));
            auto get_cap = reinterpret_cast<GetIndexFn>(GetProcAddress(h, "kestrel_tls_get_capacity"));
            if (get_idx) {
                p.hmod = h;
                p.slot_index = static_cast<uint32_t>(get_idx());
                p.capacity = get_cap ? static_cast<uint32_t>(get_cap()) : 262144u;
            }
        }
        return p;
    }();
    return provider;
}

DWORD get_loader_win32_tls_index() {
    static const DWORD idx = TlsAlloc();
    return idx;
}

void cleanup_current_thread_guest_tls_force() {
    const DWORD idx = get_loader_win32_tls_index();
    if (idx != TLS_OUT_OF_INDEXES) {
        TlsSetValue(idx, nullptr);
    }
}

class ScopedGuestTls {
public:
    explicit ScopedGuestTls(PeLoader& loader) : loader_(loader) {
        loader_.ensure_current_thread_tls();
    }
    ~ScopedGuestTls() = default;
    ScopedGuestTls(const ScopedGuestTls&) = delete;
    ScopedGuestTls& operator=(const ScopedGuestTls&) = delete;

private:
    PeLoader& loader_;
};

// Active PeLoader for guest process overlays (supports nested scopes in unit tests)
std::atomic<PeLoader*> g_active_loader{nullptr};

class ScopedActiveLoader {
public:
    explicit ScopedActiveLoader(PeLoader* loader)
        : prev_(g_active_loader.exchange(loader, std::memory_order_acq_rel)) {}
    ~ScopedActiveLoader() {
        g_active_loader.store(prev_, std::memory_order_release);
    }
    ScopedActiveLoader(const ScopedActiveLoader&) = delete;
    ScopedActiveLoader& operator=(const ScopedActiveLoader&) = delete;

private:
    PeLoader* prev_ = nullptr;
};

// SEH crash capture (plain POD struct, no C++ unwindable objects in __try frame)
struct SehCrashInfo {
    bool crashed = false;
    DWORD code = 0;
    uint64_t rip = 0;
    uint64_t access_type = 0;
    uint64_t access_addr = 0;
    uint64_t rax = 0;
    uint64_t rbx = 0;
    uint64_t rcx = 0;
    uint64_t rdx = 0;
    uint64_t rsi = 0;
    uint64_t rdi = 0;
    uint64_t rbp = 0;
    uint64_t rsp = 0;
    uint8_t instr_bytes[16]{};
    size_t instr_len = 0;
};

DWORD capture_seh_filter(EXCEPTION_POINTERS* ep, SehCrashInfo* out) {
    if (!ep || !ep->ExceptionRecord || !out) return EXCEPTION_EXECUTE_HANDLER;
    const auto* er = ep->ExceptionRecord;
    out->crashed = true;
    out->code = er->ExceptionCode;
    out->rip = reinterpret_cast<uint64_t>(er->ExceptionAddress);
    if ((er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || er->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
        er->NumberParameters >= 2) {
        out->access_type = er->ExceptionInformation[0];
        out->access_addr = er->ExceptionInformation[1];
    }
    if (ep->ContextRecord) {
        const auto* ctx = ep->ContextRecord;
        out->rip = ctx->Rip;
        out->rax = ctx->Rax;
        out->rbx = ctx->Rbx;
        out->rcx = ctx->Rcx;
        out->rdx = ctx->Rdx;
        out->rsi = ctx->Rsi;
        out->rdi = ctx->Rdi;
        out->rbp = ctx->Rbp;
        out->rsp = ctx->Rsp;
    }
    MEMORY_BASIC_INFORMATION mbi{};
    const auto* code_ptr = reinterpret_cast<const uint8_t*>(out->rip);
    if (code_ptr && VirtualQuery(code_ptr, &mbi, sizeof(mbi)) != 0 && mbi.State == MEM_COMMIT &&
        (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_READONLY | PAGE_READWRITE))) {
        const uintptr_t region_end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (region_end > out->rip) {
            const size_t avail = std::min<size_t>(16u, static_cast<size_t>(region_end - out->rip));
            std::memcpy(out->instr_bytes, code_ptr, avail);
            out->instr_len = avail;
        }
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

using GuestEntryFn = int(__cdecl*)();

void invoke_entry_seh(GuestEntryFn fn, int* out_rc, SehCrashInfo* out_crash) {
    __try {
        *out_rc = fn();
    } __except (capture_seh_filter(GetExceptionInformation(), out_crash)) {
        *out_rc = -1;
    }
}

const char* exception_code_name(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION: return "ACCESS_VIOLATION";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_PRIV_INSTRUCTION: return "PRIV_INSTRUCTION";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_STACK_OVERFLOW: return "STACK_OVERFLOW";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
        case EXCEPTION_BREAKPOINT: return "BREAKPOINT";
        default: return "SEH_EXCEPTION";
    }
}

struct GuestThreadLaunch {
    PeLoader* loader = nullptr;
    GuestEntryFn entry = nullptr;
    int exit_rc = 0;
    SehCrashInfo crash{};
};

DWORD WINAPI guest_main_thread_proc(LPVOID param) {
    auto* launch = static_cast<GuestThreadLaunch*>(param);
    if (!launch || !launch->loader || !launch->entry) return 1u;

    ScopedGuestTls tls_guard(*launch->loader);
    launch->loader->invoke_tls_callbacks_for_thread(DLL_THREAD_ATTACH);
    invoke_entry_seh(launch->entry, &launch->exit_rc, &launch->crash);
    if (!launch->crash.crashed) {
        launch->loader->run_guest_atexit();
    }
    return static_cast<DWORD>(launch->exit_rc);
}

struct GuestChildThreadLaunch {
    PeLoader* loader = nullptr;
    LPTHREAD_START_ROUTINE start = nullptr;
    LPVOID param = nullptr;
};

DWORD WINAPI guest_child_thread_proc(LPVOID raw) {
    std::unique_ptr<GuestChildThreadLaunch> launch(static_cast<GuestChildThreadLaunch*>(raw));
    if (!launch || !launch->start) return 0u;
    if (launch->loader) {
        ScopedGuestTls tls_guard(*launch->loader);
        launch->loader->invoke_tls_callbacks_for_thread(DLL_THREAD_ATTACH);
        const DWORD rc = launch->start(launch->param);
        launch->loader->invoke_tls_callbacks_for_thread(DLL_THREAD_DETACH);
        return rc;
    }
    return launch->start(launch->param);
}

// ---- Guest Process Identity & Dynamic Loader Overlays ----

HMODULE WINAPI kx_GetModuleHandleA(LPCSTR name) {
    PeLoader* loader = g_active_loader.load(std::memory_order_acquire);
    if (loader) {
        if (!name) {
            if (auto* main_mod = loader->main_module()) {
                return reinterpret_cast<HMODULE>(main_mod->base);
            }
        } else {
            if (auto* mod = loader->find_loaded_module_by_name(name)) {
                return reinterpret_cast<HMODULE>(mod->base);
            }
            const std::string redir = to_lower_ascii(loader->get_redirected_dll(basename_lower(name)));
            if (loader->hle_registry().has_module(redir)) {
                return loader->get_or_create_hle_handle(redir);
            }
        }
    }
    return GetModuleHandleA(name);
}

HMODULE WINAPI kx_GetModuleHandleW(LPCWSTR name) {
    PeLoader* loader = g_active_loader.load(std::memory_order_acquire);
    if (loader) {
        if (!name) {
            if (auto* main_mod = loader->main_module()) {
                return reinterpret_cast<HMODULE>(main_mod->base);
            }
        } else {
            const std::string utf8 = wide_to_utf8(name);
            if (auto* mod = loader->find_loaded_module_by_name(utf8)) {
                return reinterpret_cast<HMODULE>(mod->base);
            }
            const std::string redir = to_lower_ascii(loader->get_redirected_dll(basename_lower(utf8)));
            if (loader->hle_registry().has_module(redir)) {
                return loader->get_or_create_hle_handle(redir);
            }
        }
    }
    return GetModuleHandleW(name);
}

BOOL WINAPI kx_GetModuleHandleExA(DWORD flags, LPCSTR name, HMODULE* out) {
    if (!out) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    PeLoader* loader = g_active_loader.load(std::memory_order_acquire);
    if (loader) {
        if (flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) {
            if (auto* mod = loader->find_loaded_module_containing(name)) {
                *out = reinterpret_cast<HMODULE>(mod->base);
                return TRUE;
            }
        } else if (!name) {
            if (auto* main_mod = loader->main_module()) {
                *out = reinterpret_cast<HMODULE>(main_mod->base);
                return TRUE;
            }
        } else if (auto* mod = loader->find_loaded_module_by_name(name)) {
            *out = reinterpret_cast<HMODULE>(mod->base);
            return TRUE;
        }
    }
    return GetModuleHandleExA(flags, name, out);
}

BOOL WINAPI kx_GetModuleHandleExW(DWORD flags, LPCWSTR name, HMODULE* out) {
    if (!out) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    PeLoader* loader = g_active_loader.load(std::memory_order_acquire);
    if (loader) {
        if (flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) {
            if (auto* mod = loader->find_loaded_module_containing(name)) {
                *out = reinterpret_cast<HMODULE>(mod->base);
                return TRUE;
            }
        } else if (!name) {
            if (auto* main_mod = loader->main_module()) {
                *out = reinterpret_cast<HMODULE>(main_mod->base);
                return TRUE;
            }
        } else {
            const std::string utf8 = wide_to_utf8(name);
            if (auto* mod = loader->find_loaded_module_by_name(utf8)) {
                *out = reinterpret_cast<HMODULE>(mod->base);
                return TRUE;
            }
        }
    }
    return GetModuleHandleExW(flags, name, out);
}

DWORD WINAPI kx_GetModuleFileNameA(HMODULE hmod, LPSTR buf, DWORD size) {
    PeLoader* loader = g_active_loader.load(std::memory_order_acquire);
    if (loader && buf && size > 0) {
        MappedModule* target = hmod ? loader->find_loaded_module_by_handle(hmod) : loader->main_module();
        if (target && !target->full_path.empty()) {
            const std::string p = target->full_path.string();
            const DWORD copy_len = static_cast<DWORD>(std::min<size_t>(p.size(), size - 1));
            std::memcpy(buf, p.data(), copy_len);
            buf[copy_len] = '\0';
            if (p.size() >= size) SetLastError(ERROR_INSUFFICIENT_BUFFER);
            else SetLastError(ERROR_SUCCESS);
            return copy_len;
        }
    }
    return GetModuleFileNameA(hmod, buf, size);
}

DWORD WINAPI kx_GetModuleFileNameW(HMODULE hmod, LPWSTR buf, DWORD size) {
    PeLoader* loader = g_active_loader.load(std::memory_order_acquire);
    if (loader && buf && size > 0) {
        MappedModule* target = hmod ? loader->find_loaded_module_by_handle(hmod) : loader->main_module();
        if (target && !target->full_path.empty()) {
            const std::wstring p = target->full_path.wstring();
            const DWORD copy_len = static_cast<DWORD>(std::min<size_t>(p.size(), size - 1));
            std::memcpy(buf, p.data(), copy_len * sizeof(wchar_t));
            buf[copy_len] = L'\0';
            if (p.size() >= size) SetLastError(ERROR_INSUFFICIENT_BUFFER);
            else SetLastError(ERROR_SUCCESS);
            return copy_len;
        }
    }
    return GetModuleFileNameW(hmod, buf, size);
}

LPSTR WINAPI kx_GetCommandLineA() {
    PeLoader* loader = g_active_loader.load(std::memory_order_acquire);
    if (loader && !loader->guest_cmdline_a().empty()) {
        return const_cast<LPSTR>(loader->guest_cmdline_a().c_str());
    }
    return GetCommandLineA();
}

LPWSTR WINAPI kx_GetCommandLineW() {
    PeLoader* loader = g_active_loader.load(std::memory_order_acquire);
    if (loader && !loader->guest_cmdline_w().empty()) {
        return const_cast<LPWSTR>(loader->guest_cmdline_w().c_str());
    }
    return GetCommandLineW();
}

char* __cdecl kx_get_narrow_winmain_command_line() {
    PeLoader* loader = g_active_loader.load(std::memory_order_acquire);
    if (loader) {
        return const_cast<char*>(loader->guest_args_only_a().c_str());
    }
    static char empty[] = "";
    return empty;
}

wchar_t* __cdecl kx_get_wide_winmain_command_line() {
    PeLoader* loader = g_active_loader.load(std::memory_order_acquire);
    if (loader) {
        return const_cast<wchar_t*>(loader->guest_args_only_w().c_str());
    }
    static wchar_t empty[] = L"";
    return empty;
}

bool kestrel_trace_hle_enabled() {
    static const bool enabled = []() {
        char buf[8]{};
        return GetEnvironmentVariableA("KESTREL_TRACE_HLE", buf, sizeof(buf)) > 0 && buf[0] != '0';
    }();
    return enabled;
}

HMODULE load_library_common(std::string_view utf8_name, DWORD flags, bool wide_fallback, LPCWSTR orig_w, LPCSTR orig_a) {
    if (kestrel_trace_hle_enabled()) {
        std::fprintf(stderr, "[KestrelHLE] LoadLibrary(%s, flags=0x%lX)\n", std::string(utf8_name).c_str(), flags);
        std::fflush(stderr);
    }
    PeLoader* loader = g_active_loader.load(std::memory_order_acquire);
    if (loader && !utf8_name.empty()) {
        const std::string base = basename_lower(utf8_name);
        const std::string redir = loader->get_redirected_dll(base);
        const std::string redir_lower = to_lower_ascii(redir);

        if (loader->hle_registry().has_module(redir_lower)) {
            return loader->get_or_create_hle_handle(redir_lower);
        }
        auto g = loader->find_or_load_guest_dll(redir);
        if (g && *g) {
            loader->ensure_current_thread_tls();
            return reinterpret_cast<HMODULE>((*g)->base);
        }
    }
    if (wide_fallback) {
        return flags ? LoadLibraryExW(orig_w, nullptr, flags) : LoadLibraryW(orig_w);
    }
    return flags ? LoadLibraryExA(orig_a, nullptr, flags) : LoadLibraryA(orig_a);
}

HMODULE WINAPI kx_LoadLibraryA(LPCSTR name) {
    return load_library_common(name ? std::string_view(name) : std::string_view{}, 0, false, nullptr, name);
}

HMODULE WINAPI kx_LoadLibraryW(LPCWSTR name) {
    const std::string utf8 = wide_to_utf8(name);
    return load_library_common(utf8, 0, true, name, nullptr);
}

HMODULE WINAPI kx_LoadLibraryExA(LPCSTR name, HANDLE, DWORD flags) {
    return load_library_common(name ? std::string_view(name) : std::string_view{}, flags, false, nullptr, name);
}

HMODULE WINAPI kx_LoadLibraryExW(LPCWSTR name, HANDLE, DWORD flags) {
    const std::string utf8 = wide_to_utf8(name);
    return load_library_common(utf8, flags, true, name, nullptr);
}

BOOL WINAPI kx_FreeLibrary(HMODULE hmod) {
    PeLoader* loader = g_active_loader.load(std::memory_order_acquire);
    if (loader && hmod) {
        std::string hle_dll;
        if (loader->lookup_hle_handle(hmod, hle_dll)) return TRUE;
        if (loader->find_loaded_module_by_handle(hmod)) return TRUE;
    }
    return FreeLibrary(hmod);
}

FARPROC WINAPI kx_GetProcAddress(HMODULE hmod, LPCSTR proc) {
    if (!hmod || !proc) return nullptr;
    const bool by_ord = IS_INTRESOURCE(proc);
    const uint16_t ord = by_ord ? static_cast<uint16_t>(reinterpret_cast<uintptr_t>(proc) & 0xFFFFu) : 0u;

    PeLoader* loader = g_active_loader.load(std::memory_order_acquire);
    if (loader) {
        // 1. HLE pseudo-handle (e.g., LoadLibraryW(L"xgameruntime.dll") or L"d3d12.dll")
        std::string hle_dll;
        if (loader->lookup_hle_handle(hmod, hle_dll)) {
            if (kestrel_trace_hle_enabled()) {
                if (by_ord) std::fprintf(stderr, "[KestrelHLE] GetProcAddress(%s, #%u)\n", hle_dll.c_str(), ord);
                else std::fprintf(stderr, "[KestrelHLE] GetProcAddress(%s, %s)\n", hle_dll.c_str(), proc);
                std::fflush(stderr);
            }
            void* p = by_ord ? loader->hle_registry().resolve_ordinal(hle_dll, ord)
                             : loader->hle_registry().resolve_name(hle_dll, proc);
            if (p) return reinterpret_cast<FARPROC>(p);
            if (loader->hle_registry().allows_host_fallback(hle_dll)) {
                const char* fallback_name = (hle_dll == "kernelx.dll") ? "kernel32.dll" : hle_dll.c_str();
                HMODULE host = GetModuleHandleA(fallback_name);
                if (!host) host = LoadLibraryA(fallback_name);
                if (host) return GetProcAddress(host, proc);
            }
            SetLastError(ERROR_PROC_NOT_FOUND);
            return nullptr;
        }

        // 2. Guest-loaded PE module (companion DLL or main EXE)
        if (auto* mod = loader->find_loaded_module_by_handle(hmod)) {
            auto r = by_ord ? loader->resolve_export_by_ordinal(*mod, ord)
                            : loader->resolve_export_by_name(*mod, proc);
            if (r && *r) return reinterpret_cast<FARPROC>(*r);
            SetLastError(ERROR_PROC_NOT_FOUND);
            return nullptr;
        }

        // 3. Host DLL that has HLE overlays (e.g., GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "ExitProcess"))
        if (!by_ord) {
            char mod_path[MAX_PATH]{};
            if (GetModuleFileNameA(hmod, mod_path, MAX_PATH) > 0) {
                const std::string base = basename_lower(mod_path);
                if (void* ov = loader->hle_registry().resolve_host_overlay(base, proc)) {
                    return reinterpret_cast<FARPROC>(ov);
                }
                if (loader->hle_registry().has_module(base)) {
                    if (void* hle_fn = loader->hle_registry().resolve_name(base, proc)) {
                        return reinterpret_cast<FARPROC>(hle_fn);
                    }
                }
            }
        }
    }
    return GetProcAddress(hmod, proc);
}

void WINAPI kx_ExitProcess(UINT code) {
    PeLoader* loader = g_active_loader.load(std::memory_order_acquire);
    if (loader) {
        loader->record_guest_exit(static_cast<int>(code));
        cleanup_current_thread_guest_tls_force();
        ExitThread(code);
    }
    ExitProcess(code);
}

BOOL WINAPI kx_TerminateProcess(HANDLE proc, UINT code) {
    if (proc == GetCurrentProcess() || GetProcessId(proc) == GetCurrentProcessId()) {
        kx_ExitProcess(code);
    }
    return TerminateProcess(proc, code);
}

void __cdecl kx_crt_exit(int code) {
    PeLoader* loader = g_active_loader.load(std::memory_order_acquire);
    if (loader) {
        loader->run_guest_atexit();
        loader->record_guest_exit(code);
        cleanup_current_thread_guest_tls_force();
        ExitThread(static_cast<DWORD>(code));
    }
    std::exit(code);
}

void __cdecl kx_crt_quick_exit(int code) {
    PeLoader* loader = g_active_loader.load(std::memory_order_acquire);
    if (loader) {
        loader->record_guest_exit(code);
        cleanup_current_thread_guest_tls_force();
        ExitThread(static_cast<DWORD>(code));
    }
    std::_Exit(code);
}

int __cdecl kx_crt_atexit(void(__cdecl* fn)()) {
    PeLoader* loader = g_active_loader.load(std::memory_order_acquire);
    if (loader && fn) {
        loader->register_guest_atexit(fn);
        return 0;
    }
    return 0;
}

LPTOP_LEVEL_EXCEPTION_FILTER WINAPI kx_SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER) {
    return nullptr;
}

HANDLE WINAPI kx_CreateThread(
    LPSECURITY_ATTRIBUTES sec, SIZE_T stack_size, LPTHREAD_START_ROUTINE start,
    LPVOID param, DWORD flags, LPDWORD tid) {
    PeLoader* loader = g_active_loader.load(std::memory_order_acquire);
    if (loader && start) {
        auto* launch = new GuestChildThreadLaunch{loader, start, param};
        HANDLE h = CreateThread(sec, stack_size, &guest_child_thread_proc, launch, flags, tid);
        if (!h) delete launch;
        return h;
    }
    return CreateThread(sec, stack_size, start, param, flags, tid);
}

bool is_console_vfs_drive_path(std::string_view p) {
    if (p.size() < 3) return false;
    const char d = static_cast<char>(std::tolower(static_cast<unsigned char>(p[0])));
    return (d == 'g' || d == 't' || d == 's') && p[1] == ':' && (p[2] == '\\' || p[2] == '/');
}

HANDLE WINAPI kx_host_CreateFileA(
    LPCSTR filename, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sec,
    DWORD disp, DWORD flags, HANDLE tmpl) {
    if (filename && is_console_vfs_drive_path(filename)) {
        PeLoader* loader = g_active_loader.load(std::memory_order_acquire);
        if (loader) {
            if (auto* fn = reinterpret_cast<decltype(&CreateFileA)>(
                    loader->hle_registry().resolve_name("kernelx.dll", "CreateFileA"))) {
                return fn(filename, access, share, sec, disp, flags, tmpl);
            }
        }
    }
    return CreateFileA(filename, access, share, sec, disp, flags, tmpl);
}

HANDLE WINAPI kx_host_CreateFileW(
    LPCWSTR filename, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sec,
    DWORD disp, DWORD flags, HANDLE tmpl) {
    if (filename) {
        const std::string utf8 = wide_to_utf8(filename);
        if (is_console_vfs_drive_path(utf8)) {
            PeLoader* loader = g_active_loader.load(std::memory_order_acquire);
            if (loader) {
                if (auto* fn = reinterpret_cast<decltype(&CreateFileW)>(
                        loader->hle_registry().resolve_name("kernelx.dll", "CreateFileW"))) {
                    return fn(filename, access, share, sec, disp, flags, tmpl);
                }
            }
        }
    }
    return CreateFileW(filename, access, share, sec, disp, flags, tmpl);
}

HANDLE WINAPI kx_host_CreateFile2(
    LPCWSTR filename, DWORD access, DWORD share, DWORD disp, LPCREATEFILE2_EXTENDED_PARAMETERS ext) {
    if (filename) {
        const std::string utf8 = wide_to_utf8(filename);
        if (is_console_vfs_drive_path(utf8)) {
            PeLoader* loader = g_active_loader.load(std::memory_order_acquire);
            if (loader) {
                if (auto* fn = reinterpret_cast<decltype(&CreateFile2)>(
                        loader->hle_registry().resolve_name("kernelx.dll", "CreateFile2"))) {
                    return fn(filename, access, share, disp, ext);
                }
            }
        }
    }
    return CreateFile2(filename, access, share, disp, ext);
}

BOOL WINAPI kx_host_DeleteFileA(LPCSTR filename) {
    if (filename && is_console_vfs_drive_path(filename)) {
        PeLoader* loader = g_active_loader.load(std::memory_order_acquire);
        if (loader) {
            if (auto* fn = reinterpret_cast<decltype(&DeleteFileA)>(
                    loader->hle_registry().resolve_name("kernelx.dll", "DeleteFileA"))) {
                return fn(filename);
            }
        }
    }
    return DeleteFileA(filename);
}

BOOL WINAPI kx_host_DeleteFileW(LPCWSTR filename) {
    if (filename) {
        const std::string utf8 = wide_to_utf8(filename);
        if (is_console_vfs_drive_path(utf8)) {
            PeLoader* loader = g_active_loader.load(std::memory_order_acquire);
            if (loader) {
                if (auto* fn = reinterpret_cast<decltype(&DeleteFileW)>(
                        loader->hle_registry().resolve_name("kernelx.dll", "DeleteFileW"))) {
                    return fn(filename);
                }
            }
        }
    }
    return DeleteFileW(filename);
}

bool is_virtual_hle_dll_probe(std::string_view utf8_path) {
    if (utf8_path.empty()) return false;
    PeLoader* loader = g_active_loader.load(std::memory_order_acquire);
    if (!loader) return false;
    const std::string base = basename_lower(utf8_path);
    if (base.size() < 5 || base.substr(base.size() - 4) != ".dll") return false;
    const std::string redir = to_lower_ascii(loader->get_redirected_dll(base));
    return loader->hle_registry().has_module(redir);
}

DWORD WINAPI kx_host_GetFileAttributesW(LPCWSTR filename) {
    if (filename) {
        const std::string utf8 = wide_to_utf8(filename);
        if (is_console_vfs_drive_path(utf8)) {
            PeLoader* loader = g_active_loader.load(std::memory_order_acquire);
            if (loader) {
                if (auto* fn = reinterpret_cast<decltype(&GetFileAttributesW)>(
                        loader->hle_registry().resolve_name("kernelx.dll", "GetFileAttributesW"))) {
                    return fn(filename);
                }
            }
        }
        const DWORD host_attr = GetFileAttributesW(filename);
        if (kestrel_trace_hle_enabled() && utf8.find(".dll") != std::string::npos) {
            std::fprintf(stderr, "[KestrelHLE] GetFileAttributesW(%s) -> host=0x%lX, hle_probe=%d\n", utf8.c_str(), host_attr, is_virtual_hle_dll_probe(utf8) ? 1 : 0);
            std::fflush(stderr);
        }
        if (host_attr == INVALID_FILE_ATTRIBUTES && is_virtual_hle_dll_probe(utf8)) {
            return FILE_ATTRIBUTE_NORMAL;
        }
        return host_attr;
    }
    return GetFileAttributesW(filename);
}

DWORD WINAPI kx_host_GetFileAttributesA(LPCSTR filename) {
    if (filename) {
        if (is_console_vfs_drive_path(filename)) {
            PeLoader* loader = g_active_loader.load(std::memory_order_acquire);
            if (loader) {
                if (auto* fn = reinterpret_cast<decltype(&GetFileAttributesA)>(
                        loader->hle_registry().resolve_name("kernelx.dll", "GetFileAttributesA"))) {
                    return fn(filename);
                }
            }
        }
        const DWORD host_attr = GetFileAttributesA(filename);
        if (host_attr == INVALID_FILE_ATTRIBUTES && is_virtual_hle_dll_probe(filename)) {
            return FILE_ATTRIBUTE_NORMAL;
        }
        return host_attr;
    }
    return GetFileAttributesA(filename);
}

BOOL WINAPI kx_host_GetFileAttributesExW(LPCWSTR filename, GET_FILEEX_INFO_LEVELS level, LPVOID info) {
    if (filename) {
        const std::string utf8 = wide_to_utf8(filename);
        if (is_console_vfs_drive_path(utf8)) {
            PeLoader* loader = g_active_loader.load(std::memory_order_acquire);
            if (loader) {
                if (auto* fn = reinterpret_cast<decltype(&GetFileAttributesExW)>(
                        loader->hle_registry().resolve_name("kernelx.dll", "GetFileAttributesExW"))) {
                    return fn(filename, level, info);
                }
            }
        }
    }
    return GetFileAttributesExW(filename, level, info);
}

}  // namespace

void register_guest_process_overlays(hle::Registry& reg) {
    // kernel32.dll overlays (automatically matched for kernelbase.dll and api-ms-win-core-*)
    reg.register_host_overlay("kernel32.dll", "GetModuleHandleA", reinterpret_cast<void*>(&kx_GetModuleHandleA));
    reg.register_host_overlay("kernel32.dll", "GetModuleHandleW", reinterpret_cast<void*>(&kx_GetModuleHandleW));
    reg.register_host_overlay("kernel32.dll", "GetModuleHandleExA", reinterpret_cast<void*>(&kx_GetModuleHandleExA));
    reg.register_host_overlay("kernel32.dll", "GetModuleHandleExW", reinterpret_cast<void*>(&kx_GetModuleHandleExW));
    reg.register_host_overlay("kernel32.dll", "GetModuleFileNameA", reinterpret_cast<void*>(&kx_GetModuleFileNameA));
    reg.register_host_overlay("kernel32.dll", "GetModuleFileNameW", reinterpret_cast<void*>(&kx_GetModuleFileNameW));
    reg.register_host_overlay("kernel32.dll", "GetCommandLineA", reinterpret_cast<void*>(&kx_GetCommandLineA));
    reg.register_host_overlay("kernel32.dll", "GetCommandLineW", reinterpret_cast<void*>(&kx_GetCommandLineW));
    reg.register_host_overlay("kernel32.dll", "LoadLibraryA", reinterpret_cast<void*>(&kx_LoadLibraryA));
    reg.register_host_overlay("kernel32.dll", "LoadLibraryW", reinterpret_cast<void*>(&kx_LoadLibraryW));
    reg.register_host_overlay("kernel32.dll", "LoadLibraryExA", reinterpret_cast<void*>(&kx_LoadLibraryExA));
    reg.register_host_overlay("kernel32.dll", "LoadLibraryExW", reinterpret_cast<void*>(&kx_LoadLibraryExW));
    reg.register_host_overlay("kernel32.dll", "FreeLibrary", reinterpret_cast<void*>(&kx_FreeLibrary));
    reg.register_host_overlay("kernel32.dll", "GetProcAddress", reinterpret_cast<void*>(&kx_GetProcAddress));
    reg.register_host_overlay("kernel32.dll", "ExitProcess", reinterpret_cast<void*>(&kx_ExitProcess));
    reg.register_host_overlay("kernel32.dll", "TerminateProcess", reinterpret_cast<void*>(&kx_TerminateProcess));
    reg.register_host_overlay("kernel32.dll", "CreateThread", reinterpret_cast<void*>(&kx_CreateThread));
    reg.register_host_overlay("kernel32.dll", "SetUnhandledExceptionFilter", reinterpret_cast<void*>(&kx_SetUnhandledExceptionFilter));
    reg.register_host_overlay("kernel32.dll", "CreateFileA", reinterpret_cast<void*>(&kx_host_CreateFileA));
    reg.register_host_overlay("kernel32.dll", "CreateFileW", reinterpret_cast<void*>(&kx_host_CreateFileW));
    reg.register_host_overlay("kernel32.dll", "CreateFile2", reinterpret_cast<void*>(&kx_host_CreateFile2));
    reg.register_host_overlay("kernel32.dll", "DeleteFileA", reinterpret_cast<void*>(&kx_host_DeleteFileA));
    reg.register_host_overlay("kernel32.dll", "DeleteFileW", reinterpret_cast<void*>(&kx_host_DeleteFileW));
    reg.register_host_overlay("kernel32.dll", "GetFileAttributesExW", reinterpret_cast<void*>(&kx_host_GetFileAttributesExW));
    reg.register_host_overlay("kernel32.dll", "GetFileAttributesA", reinterpret_cast<void*>(&kx_host_GetFileAttributesA));
    reg.register_host_overlay("kernel32.dll", "GetFileAttributesW", reinterpret_cast<void*>(&kx_host_GetFileAttributesW));

    // ucrtbase.dll overlays (automatically matched for api-ms-win-crt-*)
    reg.register_host_overlay("ucrtbase.dll", "exit", reinterpret_cast<void*>(&kx_crt_exit));
    reg.register_host_overlay("ucrtbase.dll", "_exit", reinterpret_cast<void*>(&kx_crt_quick_exit));
    reg.register_host_overlay("ucrtbase.dll", "_Exit", reinterpret_cast<void*>(&kx_crt_quick_exit));
    reg.register_host_overlay("ucrtbase.dll", "quick_exit", reinterpret_cast<void*>(&kx_crt_quick_exit));
    reg.register_host_overlay("ucrtbase.dll", "_crt_atexit", reinterpret_cast<void*>(&kx_crt_atexit));
    reg.register_host_overlay("ucrtbase.dll", "_crt_at_quick_exit", reinterpret_cast<void*>(&kx_crt_atexit));
    reg.register_host_overlay("ucrtbase.dll", "_get_narrow_winmain_command_line", reinterpret_cast<void*>(&kx_get_narrow_winmain_command_line));
    reg.register_host_overlay("ucrtbase.dll", "_get_wide_winmain_command_line", reinterpret_cast<void*>(&kx_get_wide_winmain_command_line));

    // Mirror core process/loader exports on kernelx.dll as well
    if (reg.has_module("kernelx.dll")) {
        reg.register_export("kernelx.dll", "GetModuleHandleA", reinterpret_cast<void*>(&kx_GetModuleHandleA));
        reg.register_export("kernelx.dll", "GetModuleHandleW", reinterpret_cast<void*>(&kx_GetModuleHandleW));
        reg.register_export("kernelx.dll", "GetModuleHandleExA", reinterpret_cast<void*>(&kx_GetModuleHandleExA));
        reg.register_export("kernelx.dll", "GetModuleHandleExW", reinterpret_cast<void*>(&kx_GetModuleHandleExW));
        reg.register_export("kernelx.dll", "GetModuleFileNameA", reinterpret_cast<void*>(&kx_GetModuleFileNameA));
        reg.register_export("kernelx.dll", "GetModuleFileNameW", reinterpret_cast<void*>(&kx_GetModuleFileNameW));
        reg.register_export("kernelx.dll", "GetCommandLineA", reinterpret_cast<void*>(&kx_GetCommandLineA));
        reg.register_export("kernelx.dll", "GetCommandLineW", reinterpret_cast<void*>(&kx_GetCommandLineW));
        reg.register_export("kernelx.dll", "LoadLibraryA", reinterpret_cast<void*>(&kx_LoadLibraryA));
        reg.register_export("kernelx.dll", "LoadLibraryW", reinterpret_cast<void*>(&kx_LoadLibraryW));
        reg.register_export("kernelx.dll", "LoadLibraryExA", reinterpret_cast<void*>(&kx_LoadLibraryExA));
        reg.register_export("kernelx.dll", "LoadLibraryExW", reinterpret_cast<void*>(&kx_LoadLibraryExW));
        reg.register_export("kernelx.dll", "FreeLibrary", reinterpret_cast<void*>(&kx_FreeLibrary));
        reg.register_export("kernelx.dll", "GetProcAddress", reinterpret_cast<void*>(&kx_GetProcAddress));
        reg.register_export("kernelx.dll", "ExitProcess", reinterpret_cast<void*>(&kx_ExitProcess));
        reg.register_export("kernelx.dll", "TerminateProcess", reinterpret_cast<void*>(&kx_TerminateProcess));
        reg.register_export("kernelx.dll", "CreateThread", reinterpret_cast<void*>(&kx_CreateThread));
    }
}

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
    if (main_module_ == target) main_module_ = nullptr;
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

MappedModule* PeLoader::find_loaded_module_by_name(std::string_view name) const {
    const std::string key = basename_lower(name);
    if (auto it = by_name_.find(key); it != by_name_.end()) return it->second;
    const std::string with_dll = key.ends_with(".dll") ? key : (key + ".dll");
    if (auto it = by_name_.find(with_dll); it != by_name_.end()) return it->second;
    return nullptr;
}

MappedModule* PeLoader::find_loaded_module_by_handle(HMODULE hmod) const {
    if (!hmod) return nullptr;
    const auto* ptr = reinterpret_cast<const uint8_t*>(hmod);
    for (const auto& m : modules_) {
        if (m->base == ptr) return m.get();
    }
    return nullptr;
}

MappedModule* PeLoader::find_loaded_module_containing(const void* addr) const {
    if (!addr) return nullptr;
    const auto a = reinterpret_cast<uintptr_t>(addr);
    for (const auto& m : modules_) {
        const auto b = reinterpret_cast<uintptr_t>(m->base);
        if (a >= b && a < b + m->size_of_image) return m.get();
    }
    return nullptr;
}

HMODULE PeLoader::get_or_create_hle_handle(std::string_view dll_lower) {
    std::lock_guard lock(runtime_mtx_);
    const std::string key = to_lower_ascii(dll_lower);
    if (auto it = hle_handles_by_name_.find(key); it != hle_handles_by_name_.end()) {
        return reinterpret_cast<HMODULE>(it->second);
    }
    const uintptr_t h = next_hle_handle_++;
    hle_handles_by_name_[key] = h;
    hle_names_by_handle_[h] = key;
    return reinterpret_cast<HMODULE>(h);
}

bool PeLoader::lookup_hle_handle(HMODULE hmod, std::string& out_dll_lower) const {
    std::lock_guard lock(runtime_mtx_);
    const auto it = hle_names_by_handle_.find(reinterpret_cast<uintptr_t>(hmod));
    if (it == hle_names_by_handle_.end()) return false;
    out_dll_lower = it->second;
    return true;
}

void PeLoader::record_guest_exit(int code) {
    guest_exit_code_.store(code, std::memory_order_release);
    guest_exited_.store(true, std::memory_order_release);
}

void PeLoader::register_guest_atexit(void(__cdecl* fn)()) {
    std::lock_guard lock(runtime_mtx_);
    guest_atexit_.push_back(fn);
}

void PeLoader::run_guest_atexit() {
    std::vector<void(__cdecl*)()> fns;
    {
        std::lock_guard lock(runtime_mtx_);
        fns.swap(guest_atexit_);
    }
    for (auto it = fns.rbegin(); it != fns.rend(); ++it) {
        if (*it) (*it)();
    }
}

void PeLoader::ensure_current_thread_tls() {
    const auto& prov = get_native_tls_provider();
    if (prov.slot_index == 0xFFFFFFFFu) return;

    const DWORD win_tls = get_loader_win32_tls_index();
    if (win_tls == TLS_OUT_OF_INDEXES) return;
    auto init_mask = reinterpret_cast<uintptr_t>(TlsGetValue(win_tls));

    auto** tls_array = reinterpret_cast<void**>(__readgsqword(0x58));
    if (!tls_array) return;

    size_t bit_idx = 0;
    for (const auto& mod : modules_) {
        const size_t my_bit = bit_idx++;
        if (mod->tls_slot == 0xFFFFFFFFu || my_bit >= 63) continue;
        const uintptr_t bit = uintptr_t{1} << my_bit;
        if (init_mask & bit) continue;

        uint8_t* dst = static_cast<uint8_t*>(tls_array[mod->tls_slot]);
        if (!dst) continue;
        const size_t total = mod->tls_raw_size + mod->tls_zero_fill;
        if (total > prov.capacity) continue;
        if (mod->tls_raw_size > 0 && mod->tls_raw_start != 0) {
            std::memcpy(dst, reinterpret_cast<const void*>(mod->tls_raw_start), mod->tls_raw_size);
        }
        if (mod->tls_zero_fill > 0) {
            std::memset(dst + mod->tls_raw_size, 0, mod->tls_zero_fill);
        }
        init_mask |= bit;
    }
    TlsSetValue(win_tls, reinterpret_cast<LPVOID>(init_mask));
}

void PeLoader::invoke_tls_callbacks_for_thread(DWORD reason) {
    for (const auto& mod : modules_) {
        run_tls_callbacks(*mod, reason);
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

    // 0. Check host overlays first
    if (!by_ordinal) {
        if (void* ov = hle_.resolve_host_overlay(target_lower, sym_str)) return ov;
    }

    // 1. Check HLE registry
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

    // 3. Fall back to host DLLs when un-shimmed or host-fallback is allowed
    HMODULE host_dll = nullptr;
    if (hle_.has_module(target_lower)) {
        if (hle_.allows_host_fallback(target_lower)) {
            const char* host_name = (target_lower == "kernelx.dll") ? "kernel32.dll" : target_dll.c_str();
            host_dll = GetModuleHandleA(host_name);
            if (!host_dll) host_dll = LoadLibraryA(host_name);
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
    std::error_code ec;
    auto abs_path = std::filesystem::absolute(path, ec);
    mod->full_path = ec ? path : abs_path;
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
    if (!raw_ptr->is_dll && !main_module_) {
        main_module_ = raw_ptr;
    }

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
    setup_static_tls(*raw_ptr);
    if (auto r = protect_sections(*raw_ptr); !r) {
        rollback_module(key, raw_ptr);
        return std::unexpected(r.error());
    }
    register_exception_table(*raw_ptr);

    {
        ScopedActiveLoader active_guard(this);
        ScopedGuestTls tls_guard(*this);
        run_tls_callbacks(*raw_ptr, DLL_PROCESS_ATTACH);

        if (raw_ptr->is_dll && raw_ptr->entry_rva != 0) {
            using DllMainFn = BOOL(WINAPI*)(HINSTANCE, DWORD, LPVOID);
            auto fn = reinterpret_cast<DllMainFn>(raw_ptr->base + raw_ptr->entry_rva);
            fn(reinterpret_cast<HINSTANCE>(raw_ptr->base), DLL_PROCESS_ATTACH, nullptr);
        }
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
            if (!sym.by_ordinal) {
                resolved = hle_.resolve_host_overlay(imp_mod.dll_name, sym.name);
                if (!resolved && redirected_name != imp_mod.dll_name) {
                    resolved = hle_.resolve_host_overlay(redirected_name, sym.name);
                }
                if (resolved) ++mod.hle_imports;
            }
            if (!resolved && hle_.has_module(dll_lower)) {
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
            if (!resolved && !host_dll && hle_.has_module(dll_lower) && hle_.allows_host_fallback(dll_lower)) {
                const char* host_name = (dll_lower == "kernelx.dll") ? "kernel32.dll" : redirected_name.c_str();
                host_dll = GetModuleHandleA(host_name);
                if (!host_dll) host_dll = LoadLibraryA(host_name);
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

void PeLoader::setup_static_tls(MappedModule& mod) {
    if (mod.size_of_image == 0 || mod.image.data_directories.size() <= IMAGE_DIRECTORY_ENTRY_TLS) return;
    const auto dir = mod.image.data_directories[IMAGE_DIRECTORY_ENTRY_TLS];
    if (dir.rva == 0 || dir.size < sizeof(IMAGE_TLS_DIRECTORY64)) return;
    if (!span_in_bounds(dir.rva, dir.size, mod.size_of_image)) return;

    const auto* tls = reinterpret_cast<const IMAGE_TLS_DIRECTORY64*>(mod.base + dir.rva);
    const uint64_t base_va = reinterpret_cast<uint64_t>(mod.base);

    size_t raw_size = 0;
    uint64_t raw_start = 0;
    if (tls->StartAddressOfRawData != 0 && tls->EndAddressOfRawData >= tls->StartAddressOfRawData) {
        if (tls->StartAddressOfRawData >= base_va &&
            span_in_bounds(tls->StartAddressOfRawData - base_va,
                           tls->EndAddressOfRawData - tls->StartAddressOfRawData,
                           mod.size_of_image)) {
            raw_start = tls->StartAddressOfRawData;
            raw_size = static_cast<size_t>(tls->EndAddressOfRawData - tls->StartAddressOfRawData);
        }
    }
    const size_t zero_fill = static_cast<size_t>(tls->SizeOfZeroFill);
    const uint32_t align_code = (tls->Characteristics >> 20) & 0xFu;
    size_t align = (align_code > 0 && align_code <= 13) ? (size_t{1} << (align_code - 1)) : 16u;
    if (align < 16u) align = 16u;

    const auto& prov = get_native_tls_provider();
    if (tls->AddressOfIndex != 0 && prov.slot_index != 0xFFFFFFFFu) {
        if (tls->AddressOfIndex >= base_va &&
            span_in_bounds(tls->AddressOfIndex - base_va, sizeof(uint32_t), mod.size_of_image)) {
            mod.tls_slot = prov.slot_index;
            mod.tls_raw_start = raw_start;
            mod.tls_raw_size = raw_size;
            mod.tls_zero_fill = zero_fill;
            mod.tls_align = align;
            *reinterpret_cast<uint32_t*>(tls->AddressOfIndex) = mod.tls_slot;
        }
    }
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

void PeLoader::run_tls_callbacks(MappedModule& mod, DWORD reason) {
    if (mod.size_of_image == 0 || mod.image.data_directories.size() <= IMAGE_DIRECTORY_ENTRY_TLS) return;
    const auto dir = mod.image.data_directories[IMAGE_DIRECTORY_ENTRY_TLS];
    if (dir.rva == 0 || dir.size < sizeof(IMAGE_TLS_DIRECTORY64)) return;
    if (!span_in_bounds(dir.rva, dir.size, mod.size_of_image)) return;
    const auto* tls = reinterpret_cast<const IMAGE_TLS_DIRECTORY64*>(mod.base + dir.rva);
    const uint64_t base_va = reinterpret_cast<uint64_t>(mod.base);
    if (tls->AddressOfCallBacks == 0 || tls->AddressOfCallBacks < base_va) return;
    const uint64_t cbs_rva = tls->AddressOfCallBacks - base_va;
    auto* cbs = reinterpret_cast<PIMAGE_TLS_CALLBACK*>(tls->AddressOfCallBacks);
    for (size_t i = 0; i < 64; ++i) {
        if (!span_in_bounds(cbs_rva + i * sizeof(PIMAGE_TLS_CALLBACK), sizeof(PIMAGE_TLS_CALLBACK), mod.size_of_image)) {
            break;
        }
        if (cbs[i] == nullptr) break;
        const uint64_t cb_va = reinterpret_cast<uint64_t>(cbs[i]);
        if (cb_va < base_va || cb_va - base_va >= mod.size_of_image) break;
        cbs[i](mod.base, reason, nullptr);
        if (reason == DLL_PROCESS_ATTACH) {
            ++mod.tls_callbacks_run;
        }
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

std::string PeLoader::format_address(uint64_t addr) const {
    if (auto* mod = find_loaded_module_containing(reinterpret_cast<const void*>(addr))) {
        const uint64_t rva = addr - reinterpret_cast<uint64_t>(mod->base);
        char buf[128]{};
        std::snprintf(buf, sizeof(buf), "%s+0x%llX (0x%016llX)",
                      mod->name.c_str(), static_cast<unsigned long long>(rva),
                      static_cast<unsigned long long>(addr));
        return buf;
    }
    HMODULE host_mod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(addr), &host_mod) && host_mod) {
        char path[MAX_PATH]{};
        if (GetModuleFileNameA(host_mod, path, MAX_PATH) > 0) {
            const uint64_t rva = addr - reinterpret_cast<uint64_t>(host_mod);
            char buf[160]{};
            std::snprintf(buf, sizeof(buf), "%s+0x%llX (0x%016llX)",
                          basename_lower(path).c_str(), static_cast<unsigned long long>(rva),
                          static_cast<unsigned long long>(addr));
            return buf;
        }
    }
    char buf[48]{};
    std::snprintf(buf, sizeof(buf), "0x%016llX", static_cast<unsigned long long>(addr));
    return buf;
}

Result<int> PeLoader::execute_entry(MappedModule& mod) {
    return execute_entry(mod, GuestRunOptions{});
}

Result<int> PeLoader::execute_entry(MappedModule& mod, const GuestRunOptions& options) {
    if (mod.entry_rva == 0 || mod.entry_rva >= mod.size_of_image) {
        return fail("Module has no valid entry point: " + mod.name);
    }

    main_module_ = &mod;
    guest_exited_.store(false, std::memory_order_release);
    guest_exit_code_.store(0, std::memory_order_release);
    {
        std::lock_guard lock(runtime_mtx_);
        guest_atexit_.clear();
    }

    const std::string exe_str = mod.full_path.empty() ? mod.name : mod.full_path.string();
    args_only_a_ = options.command_line_args;
    args_only_w_ = utf8_to_wide(args_only_a_);
    cmdline_a_ = "\"" + exe_str + "\"";
    if (!args_only_a_.empty()) {
        cmdline_a_ += " ";
        cmdline_a_ += args_only_a_;
    }
    cmdline_w_ = utf8_to_wide(cmdline_a_);

    GuestThreadLaunch launch{};
    launch.loader = this;
    launch.entry = reinterpret_cast<GuestEntryFn>(mod.base + mod.entry_rva);

    ScopedActiveLoader active_guard(this);
    HANDLE hThread = CreateThread(
        nullptr,
        4u * 1024u * 1024u,  // 4 MB stack reserve for real Win32/D3D12/CRT guest frames
        &guest_main_thread_proc,
        &launch,
        0,
        nullptr);
    if (!hThread) {
        return fail("Failed to spawn dedicated guest execution thread for " + mod.name);
    }

    const DWORD wait_rc = WaitForSingleObject(hThread, options.timeout_ms);
    if (wait_rc == WAIT_TIMEOUT) {
        TerminateThread(hThread, 0xDEAD0001u);
        CloseHandle(hThread);
        return fail("Guest execution timed out after " + std::to_string(options.timeout_ms) + " ms: " + mod.name);
    }
    DWORD thread_exit = 0;
    GetExitCodeThread(hThread, &thread_exit);
    CloseHandle(hThread);

    if (launch.crash.crashed) {
        std::ostringstream oss;
        char hdr[256]{};
        std::snprintf(hdr, sizeof(hdr), "Guest crashed with %s (0x%08lX) at %s",
                      exception_code_name(launch.crash.code),
                      static_cast<unsigned long>(launch.crash.code),
                      format_address(launch.crash.rip).c_str());
        oss << hdr;
        if (launch.crash.code == EXCEPTION_ACCESS_VIOLATION) {
            const char* op = (launch.crash.access_type == 0) ? "READ"
                           : (launch.crash.access_type == 1) ? "WRITE"
                           : "EXEC";
            char av[96]{};
            std::snprintf(av, sizeof(av), " [%s @ 0x%016llX]", op,
                          static_cast<unsigned long long>(launch.crash.access_addr));
            oss << av;
        }
        char regs[256]{};
        std::snprintf(regs, sizeof(regs),
                      " | RAX=0x%llX RCX=0x%llX RDX=0x%llX RSP=0x%llX",
                      static_cast<unsigned long long>(launch.crash.rax),
                      static_cast<unsigned long long>(launch.crash.rcx),
                      static_cast<unsigned long long>(launch.crash.rdx),
                      static_cast<unsigned long long>(launch.crash.rsp));
        oss << regs;
        if (launch.crash.instr_len > 0) {
            oss << " | Bytes:";
            for (size_t i = 0; i < launch.crash.instr_len; ++i) {
                char b[8]{};
                std::snprintf(b, sizeof(b), " %02X", launch.crash.instr_bytes[i]);
                oss << b;
            }
        }
        const auto stubs = hle_.called_stubs();
        if (!stubs.empty()) {
            oss << " | Called stubs:";
            for (size_t i = 0; i < std::min<size_t>(stubs.size(), 6u); ++i) {
                oss << " " << stubs[i].dll << "!" << stubs[i].symbol << "(" << stubs[i].calls << ")";
            }
        }
        return fail(oss.str());
    }

    if (guest_exited_.load(std::memory_order_acquire)) {
        return guest_exit_code_.load(std::memory_order_acquire);
    }
    return launch.exit_rc;
}

}  // namespace kestrel::loader
