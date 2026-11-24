#pragma once

#include "kestrel/hle/registry.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>

namespace kestrel::hle {

struct XGameRuntimeStats {
    uint32_t users_added = 0;
    uint32_t store_contexts = 0;
    uint32_t package_queries = 0;
    uint32_t async_completed = 0;
};

// Public GDK XAsyncBlock ABI layout
struct KxXAsyncBlock {
    void* queue = nullptr;
    void* context = nullptr;
    void(__stdcall* callback)(KxXAsyncBlock* async) = nullptr;
    uint8_t internal[32]{};
};

void set_xgameruntime_title_id(uint32_t title_id);
void register_xgameruntime(Registry& reg);
XGameRuntimeStats xgameruntime_stats();
void reset_xgameruntime_stats();

// Direct callable HLE implementations
HRESULT __stdcall kx_XGameGetXboxTitleId(uint32_t* out_id);
uint32_t __stdcall kx_XSystemGetDeviceType();
HRESULT __stdcall kx_XTaskQueueCreate(uint32_t work_mode, uint32_t comp_mode, void** out_queue);
HRESULT __stdcall kx_XTaskQueueSubmitCallback(void* queue, uint32_t port, void* ctx, void(__stdcall* cb)(void*, bool));
bool __stdcall kx_XTaskQueueDispatch(void* queue, uint32_t port, uint32_t timeout_ms);
void __stdcall kx_XTaskQueueCloseHandle(void* queue);
HRESULT __stdcall kx_XAsyncGetStatus(KxXAsyncBlock* async, bool wait);
HRESULT __stdcall kx_XUserAddAsync(uint32_t options, KxXAsyncBlock* async);
HRESULT __stdcall kx_XUserAddResult(KxXAsyncBlock* async, void** out_user);
HRESULT __stdcall kx_XUserGetId(void* user, uint64_t* out_id);
HRESULT __stdcall kx_XUserGetGamertag(void* user, uint32_t format, size_t buf_size, char* buf, size_t* out_used);
HRESULT __stdcall kx_XUserFindControllerUser(uint32_t controller_index, void** out_user);
HRESULT __stdcall kx_XUserSetActive(void* user);
uint64_t __stdcall kx_XUserGetActiveXuid();
void __stdcall kx_XUserCloseHandle(void* user);
HRESULT __stdcall kx_XPackageGetCurrentProcessPackageIdentifier(size_t buf_size, char* buf);
HRESULT __stdcall kx_XStoreCreateContext(void* user, void** out_ctx);
void __stdcall kx_XStoreCloseContextHandle(void* ctx);

}  // namespace kestrel::hle
