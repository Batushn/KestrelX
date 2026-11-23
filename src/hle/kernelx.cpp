#include "kestrel/hle/kernelx.hpp"
#include "kestrel/common/types.hpp"

#include <Windows.h>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace kestrel::hle {

namespace {

struct TitleMemoryStatusStruct {
    uint32_t length;
    uint32_t reserved;
    uint64_t total_mem;
    uint64_t avail_mem;
    uint64_t legacy_used;
    uint64_t legacy_peak;
    uint64_t legacy_avail;
    uint64_t title_used;
    uint64_t title_avail;
};

std::mutex g_mem_mtx;
std::unordered_map<void*, size_t> g_allocs;
KernelXStats g_stats{};
vfs::OverlayVfs* g_vfs = nullptr;
constexpr uint64_t kSeriesXTitleBudget = 13824ull * 1024ull * 1024ull;

bool is_write_open(DWORD access, DWORD disp) {
    return (access & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA)) != 0 ||
           disp == CREATE_NEW || disp == CREATE_ALWAYS || disp == OPEN_ALWAYS ||
           disp == TRUNCATE_EXISTING;
}

bool is_win32_device_path(std::string_view path) {
    if (path.starts_with("\\\\.\\")) return true;
    const std::string low = to_lower_ascii(path);
    return low == "con" || low == "conin$" || low == "conout$" ||
           low == "nul" || low == "prn" || low == "aux";
}

void* __stdcall kx_XMemAlloc(size_t size, uint64_t attributes) {
    (void)attributes;
    if (size == 0) size = 1;
    const size_t align = 64;
    void* ptr = _aligned_malloc((size + align - 1) & ~(align - 1), align);
    if (!ptr) return nullptr;
    std::memset(ptr, 0, size);
    std::lock_guard lock(g_mem_mtx);
    g_allocs[ptr] = size;
    ++g_stats.alloc_count;
    g_stats.live_bytes += size;
    if (g_stats.live_bytes > g_stats.peak_bytes) g_stats.peak_bytes = g_stats.live_bytes;
    return ptr;
}

void __stdcall kx_XMemFree(void* ptr, uint64_t attributes) {
    (void)attributes;
    if (!ptr) return;
    std::lock_guard lock(g_mem_mtx);
    auto it = g_allocs.find(ptr);
    if (it != g_allocs.end()) {
        g_stats.live_bytes -= it->second;
        ++g_stats.free_count;
        g_allocs.erase(it);
    }
    _aligned_free(ptr);
}

size_t __stdcall kx_XMemSize(void* ptr, uint64_t attributes) {
    (void)attributes;
    if (!ptr) return 0;
    std::lock_guard lock(g_mem_mtx);
    auto it = g_allocs.find(ptr);
    return it == g_allocs.end() ? 0 : it->second;
}

BOOL __stdcall kx_TitleMemoryStatus(TitleMemoryStatusStruct* out) {
    if (!out || out->length < sizeof(TitleMemoryStatusStruct)) return FALSE;
    std::lock_guard lock(g_mem_mtx);
    out->total_mem = kSeriesXTitleBudget;
    out->avail_mem = kSeriesXTitleBudget > g_stats.live_bytes ? (kSeriesXTitleBudget - g_stats.live_bytes) : 0;
    out->legacy_used = 0;
    out->legacy_peak = 0;
    out->legacy_avail = out->avail_mem;
    out->title_used = g_stats.live_bytes;
    out->title_avail = out->avail_mem;
    return TRUE;
}

uint32_t __stdcall kx_GetConsoleType() { return 4; }

HANDLE __stdcall kx_CreateFileA(
    LPCSTR filename, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sec,
    DWORD disp, DWORD flags, HANDLE tmpl) {
    if (g_vfs && filename && !is_win32_device_path(filename)) {
        auto resolved = g_vfs->resolve(filename, is_write_open(access, disp));
        if (resolved) {
            return CreateFileW(resolved->c_str(), access, share, sec, disp, flags, tmpl);
        }
        SetLastError(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    return CreateFileA(filename, access, share, sec, disp, flags, tmpl);
}

HANDLE __stdcall kx_CreateFileW(
    LPCWSTR filename, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sec,
    DWORD disp, DWORD flags, HANDLE tmpl) {
    if (g_vfs && filename) {
        const std::string utf8 = wide_to_utf8(filename);
        if (!is_win32_device_path(utf8)) {
            auto resolved = g_vfs->resolve(utf8, is_write_open(access, disp));
            if (resolved) {
                return CreateFileW(resolved->c_str(), access, share, sec, disp, flags, tmpl);
            }
            SetLastError(ERROR_FILE_NOT_FOUND);
            return INVALID_HANDLE_VALUE;
        }
    }
    return CreateFileW(filename, access, share, sec, disp, flags, tmpl);
}

BOOL __stdcall kx_DeleteFileA(LPCSTR filename) {
    if (g_vfs && filename && !is_win32_device_path(filename)) {
        auto r = g_vfs->remove_file(filename);
        if (r) return TRUE;
        SetLastError(ERROR_FILE_NOT_FOUND);
        return FALSE;
    }
    return DeleteFileA(filename);
}

BOOL __stdcall kx_DeleteFileW(LPCWSTR filename) {
    if (g_vfs && filename) {
        const std::string utf8 = wide_to_utf8(filename);
        if (!is_win32_device_path(utf8)) {
            auto r = g_vfs->remove_file(utf8);
            if (r) return TRUE;
            SetLastError(ERROR_FILE_NOT_FOUND);
            return FALSE;
        }
    }
    return DeleteFileW(filename);
}

BOOL __stdcall kx_GetFileAttributesExW(LPCWSTR filename, GET_FILEEX_INFO_LEVELS level, LPVOID info) {
    if (g_vfs && filename) {
        const std::string utf8 = wide_to_utf8(filename);
        if (!is_win32_device_path(utf8)) {
            auto resolved = g_vfs->resolve(utf8, false);
            if (resolved) {
                return GetFileAttributesExW(resolved->c_str(), level, info);
            }
            SetLastError(ERROR_FILE_NOT_FOUND);
            return FALSE;
        }
    }
    return GetFileAttributesExW(filename, level, info);
}

HANDLE __stdcall kx_CreateFile2(
    LPCWSTR filename, DWORD access, DWORD share, DWORD disp, LPCREATEFILE2_EXTENDED_PARAMETERS ext) {
    {
        std::lock_guard lock(g_mem_mtx);
        ++g_stats.create_file2_calls;
    }
    if (g_vfs && filename) {
        const std::string utf8 = wide_to_utf8(filename);
        if (!is_win32_device_path(utf8)) {
            auto resolved = g_vfs->resolve(utf8, is_write_open(access, disp));
            if (resolved) {
                return CreateFile2(resolved->c_str(), access, share, disp, ext);
            }
            SetLastError(ERROR_FILE_NOT_FOUND);
            return INVALID_HANDLE_VALUE;
        }
    }
    return CreateFile2(filename, access, share, disp, ext);
}

BOOL __stdcall kx_ReadFileEx(
    HANDLE file, LPVOID buffer, DWORD bytes_to_read,
    LPOVERLAPPED overlapped, LPOVERLAPPED_COMPLETION_ROUTINE completion) {
    const BOOL ok = ReadFileEx(file, buffer, bytes_to_read, overlapped, completion);
    if (ok) {
        std::lock_guard lock(g_mem_mtx);
        ++g_stats.async_reads;
    }
    return ok;
}

}  // namespace

void set_kernelx_vfs(vfs::OverlayVfs* vfs) { g_vfs = vfs; }

void register_kernelx(Registry& reg) {
    reg.register_module("kernelx.dll");
    reg.register_module("xmem.dll");
    reg.register_export("kernelx.dll", "XMemAlloc", reinterpret_cast<void*>(&kx_XMemAlloc));
    reg.register_export("kernelx.dll", "XMemFree", reinterpret_cast<void*>(&kx_XMemFree));
    reg.register_export("kernelx.dll", "XMemSize", reinterpret_cast<void*>(&kx_XMemSize));
    reg.register_export("kernelx.dll", "TitleMemoryStatus", reinterpret_cast<void*>(&kx_TitleMemoryStatus));
    reg.register_export("kernelx.dll", "GetConsoleType", reinterpret_cast<void*>(&kx_GetConsoleType));
    reg.register_export("kernelx.dll", "CreateFileA", reinterpret_cast<void*>(&kx_CreateFileA));
    reg.register_export("kernelx.dll", "CreateFileW", reinterpret_cast<void*>(&kx_CreateFileW));
    reg.register_export("kernelx.dll", "CreateFile2", reinterpret_cast<void*>(&kx_CreateFile2));
    reg.register_export("kernelx.dll", "ReadFileEx", reinterpret_cast<void*>(&kx_ReadFileEx));
    reg.register_export("kernelx.dll", "DeleteFileA", reinterpret_cast<void*>(&kx_DeleteFileA));
    reg.register_export("kernelx.dll", "DeleteFileW", reinterpret_cast<void*>(&kx_DeleteFileW));
    reg.register_export("kernelx.dll", "GetFileAttributesExW", reinterpret_cast<void*>(&kx_GetFileAttributesExW));
    reg.register_export("xmem.dll", "XMemAlloc", reinterpret_cast<void*>(&kx_XMemAlloc));
    reg.register_export("xmem.dll", "XMemFree", reinterpret_cast<void*>(&kx_XMemFree));
    reg.register_export("xmem.dll", "XMemSize", reinterpret_cast<void*>(&kx_XMemSize));
}

KernelXStats kernelx_stats() { std::lock_guard lock(g_mem_mtx); return g_stats; }
void reset_kernelx_stats() { std::lock_guard lock(g_mem_mtx); g_stats = {}; }

}  // namespace kestrel::hle