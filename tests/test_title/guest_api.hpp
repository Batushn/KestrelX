#pragma once

#include <Windows.h>
#include <d3d12.h>
#include <cstdint>

extern "C" {

// Clean-room companion guest DLL exports
#ifdef KESTREL_GUEST_DLL_EXPORTS
#define KESTREL_GUEST_API __declspec(dllexport)
#else
#define KESTREL_GUEST_API __declspec(dllimport)
#endif

KESTREL_GUEST_API uint32_t __cdecl kestrel_guest_dll_compute(uint32_t x);
KESTREL_GUEST_API uint32_t __cdecl kestrel_guest_dll_attached();

// Synthetic Xbox console imports resolved by KestrelX HLE
struct KxTitleMemoryStatus {
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

__declspec(dllimport) void* __stdcall XMemAlloc(size_t size, uint64_t attributes);
__declspec(dllimport) void __stdcall XMemFree(void* ptr, uint64_t attributes);
__declspec(dllimport) size_t __stdcall XMemSize(void* ptr, uint64_t attributes);
__declspec(dllimport) BOOL __stdcall TitleMemoryStatus(KxTitleMemoryStatus* out);
__declspec(dllimport) uint32_t __stdcall GetConsoleType();

// Resolved from kernelx.dll via explicit import alias (CreateFileA is in kernelx.def)
// Note: We import kernelx!CreateFileA via a dedicated declaration to avoid colliding with kernel32!CreateFileA.
__declspec(dllimport) HRESULT __stdcall XGameGetXboxTitleId(uint32_t* out_id);
__declspec(dllimport) uint32_t __stdcall XSystemGetDeviceType();
using KxTaskCallback = void(__stdcall*)(void* context, bool canceled);
__declspec(dllimport) HRESULT __stdcall XTaskQueueCreate(uint32_t work_mode, uint32_t comp_mode, void** out_queue);
__declspec(dllimport) HRESULT __stdcall XTaskQueueSubmitCallback(void* queue, uint32_t port, void* ctx, KxTaskCallback cb);
__declspec(dllimport) bool __stdcall XTaskQueueDispatch(void* queue, uint32_t port, uint32_t timeout_ms);
__declspec(dllimport) void __stdcall XTaskQueueCloseHandle(void* queue);

__declspec(dllimport) HRESULT __stdcall D3D12XboxCreateDevice(const void* params, REFIID riid, void** out_dev);
__declspec(dllimport) void __stdcall D3D12ZeroMemoryX(void* dst, size_t size);

}  // extern "C"
