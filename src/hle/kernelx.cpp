#include "kestrel/hle/kernelx.hpp"

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
    if (g_vfs && filename) {
        const bool for_write = (access & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA)) != 0 ||
                               disp == CREATE_NEW || disp == CREATE_ALWAYS || disp == OPEN_ALWAYS ||
                               disp == TRUNCATE_EXISTING;
        auto resolved = g_vfs->resolve(filename, for_write);
        if (resolved) {
            return CreateFileW(resolved->c_str(), access, share, sec, disp, flags, tmpl);
        }
        if (std::string_view(filename).find(':') != std::string_view::npos) {
            SetLastError(ERROR_FILE_NOT_FOUND);
            return INVALID_HANDLE_VALUE;
        }
    }
    return CreateFileA(filename, access, share, sec, disp, flags, tmpl);
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
    reg.register_export("xmem.dll", "XMemAlloc", reinterpret_cast<void*>(&kx_XMemAlloc));
    reg.register_export("xmem.dll", "XMemFree", reinterpret_cast<void*>(&kx_XMemFree));
    reg.register_export("xmem.dll", "XMemSize", reinterpret_cast<void*>(&kx_XMemSize));
}

KernelXStats kernelx_stats() { std::lock_guard lock(g_mem_mtx); return g_stats; }
void reset_kernelx_stats() { std::lock_guard lock(g_mem_mtx); g_stats = {}; }

}  // namespace kestrel::hle
