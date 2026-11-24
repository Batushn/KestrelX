#include "kestrel/hle/gameinput.hpp"
#include "kestrel/hle/registry.hpp"
#include "kestrel/hle/xgameruntime.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>

namespace {

kestrel::hle::Registry& shim_registry() {
    static kestrel::hle::Registry reg;
    static const bool initialized = []() {
        kestrel::hle::register_xgameruntime(reg);
        kestrel::hle::register_gameinput(reg);
        return true;
    }();
    (void)initialized;
    return reg;
}

template <typename Fn>
Fn resolve_xgr(const char* name) {
    return reinterpret_cast<Fn>(shim_registry().resolve_name("xgameruntime.dll", name));
}

}  // namespace

extern "C" {

HRESULT __stdcall InitializeApiImpl(uint64_t version) {
    auto fn = resolve_xgr<HRESULT(__stdcall*)(uint64_t)>("InitializeApiImpl");
    return fn ? fn(version) : E_NOTIMPL;
}

HRESULT __stdcall InitializeApiImplEx(uint64_t version, const void* options) {
    auto fn = resolve_xgr<HRESULT(__stdcall*)(uint64_t, const void*)>("InitializeApiImplEx");
    return fn ? fn(version, options) : E_NOTIMPL;
}

HRESULT __stdcall InitializeApiImplEx2(uint64_t version, const void* options) {
    auto fn = resolve_xgr<HRESULT(__stdcall*)(uint64_t, const void*)>("InitializeApiImplEx2");
    return fn ? fn(version, options) : E_NOTIMPL;
}

void __stdcall UninitializeApiImpl() {
    auto fn = resolve_xgr<void(__stdcall*)()>("UninitializeApiImpl");
    if (fn) fn();
}

HRESULT __stdcall QueryApiImpl(const void* iid, void** out_impl) {
    auto fn = resolve_xgr<HRESULT(__stdcall*)(const void*, void**)>("QueryApiImpl");
    return fn ? fn(iid, out_impl) : E_NOINTERFACE;
}

HRESULT __stdcall XGameRuntimeInitialize() {
    auto fn = resolve_xgr<HRESULT(__stdcall*)()>("XGameRuntimeInitialize");
    return fn ? fn() : E_NOTIMPL;
}

HRESULT __stdcall XGameRuntimeInitializeWithOptions(const void* options) {
    auto fn = resolve_xgr<HRESULT(__stdcall*)(const void*)>("XGameRuntimeInitializeWithOptions");
    return fn ? fn(options) : E_NOTIMPL;
}

void __stdcall XGameRuntimeUninitialize() {
    auto fn = resolve_xgr<void(__stdcall*)()>("XGameRuntimeUninitialize");
    if (fn) fn();
}

bool __stdcall XGameRuntimeIsFeatureAvailable(uint32_t feature) {
    auto fn = resolve_xgr<bool(__stdcall*)(uint32_t)>("XGameRuntimeIsFeatureAvailable");
    return fn ? fn(feature) : false;
}

bool __stdcall XErrorReport(HRESULT hr, const char* msg) {
    auto fn = resolve_xgr<bool(__stdcall*)(HRESULT, const char*)>("XErrorReport");
    return fn ? fn(hr, msg) : true;
}

void __stdcall XErrorSetCallback(void* cb, void* ctx) {
    auto fn = resolve_xgr<void(__stdcall*)(void*, void*)>("XErrorSetCallback");
    if (fn) fn(cb, ctx);
}

void __stdcall XErrorSetOptions(uint32_t opts_dbg, uint32_t opts_nodbg) {
    auto fn = resolve_xgr<void(__stdcall*)(uint32_t, uint32_t)>("XErrorSetOptions");
    if (fn) fn(opts_dbg, opts_nodbg);
}

HRESULT __stdcall XGameGetXboxTitleId(uint32_t* title_id) {
    auto fn = resolve_xgr<HRESULT(__stdcall*)(uint32_t*)>("XGameGetXboxTitleId");
    return fn ? fn(title_id) : E_NOTIMPL;
}

uint32_t __stdcall XSystemGetDeviceType() {
    auto fn = resolve_xgr<uint32_t(__stdcall*)()>("XSystemGetDeviceType");
    return fn ? fn() : 0u;
}

HRESULT __stdcall XTaskQueueCreate(uint32_t work_mode, uint32_t comp_mode, void** out_queue) {
    auto fn = resolve_xgr<HRESULT(__stdcall*)(uint32_t, uint32_t, void**)>("XTaskQueueCreate");
    return fn ? fn(work_mode, comp_mode, out_queue) : E_NOTIMPL;
}

HRESULT __stdcall XTaskQueueSubmitCallback(void* queue, uint32_t port, void* ctx, void(__stdcall* cb)(void*, bool)) {
    auto fn = resolve_xgr<HRESULT(__stdcall*)(void*, uint32_t, void*, void(__stdcall*)(void*, bool))>("XTaskQueueSubmitCallback");
    return fn ? fn(queue, port, ctx, cb) : E_NOTIMPL;
}

bool __stdcall XTaskQueueDispatch(void* queue, uint32_t port, uint32_t timeout_ms) {
    auto fn = resolve_xgr<bool(__stdcall*)(void*, uint32_t, uint32_t)>("XTaskQueueDispatch");
    return fn ? fn(queue, port, timeout_ms) : false;
}

void __stdcall XTaskQueueCloseHandle(void* queue) {
    auto fn = resolve_xgr<void(__stdcall*)(void*)>("XTaskQueueCloseHandle");
    if (fn) fn(queue);
}

HRESULT __stdcall XAsyncGetStatus(void* async, bool wait) {
    auto fn = resolve_xgr<HRESULT(__stdcall*)(void*, bool)>("XAsyncGetStatus");
    return fn ? fn(async, wait) : E_NOTIMPL;
}

HRESULT __stdcall XUserAddAsync(uint32_t options, void* async) {
    auto fn = resolve_xgr<HRESULT(__stdcall*)(uint32_t, void*)>("XUserAddAsync");
    return fn ? fn(options, async) : E_NOTIMPL;
}

HRESULT __stdcall XUserAddResult(void* async, void** out_user) {
    auto fn = resolve_xgr<HRESULT(__stdcall*)(void*, void**)>("XUserAddResult");
    return fn ? fn(async, out_user) : E_NOTIMPL;
}

HRESULT __stdcall XUserGetId(void* user, uint64_t* out_id) {
    auto fn = resolve_xgr<HRESULT(__stdcall*)(void*, uint64_t*)>("XUserGetId");
    return fn ? fn(user, out_id) : E_NOTIMPL;
}

HRESULT __stdcall XUserGetGamertag(void* user, uint32_t component, size_t size, char* buf, size_t* used) {
    auto fn = resolve_xgr<HRESULT(__stdcall*)(void*, uint32_t, size_t, char*, size_t*)>("XUserGetGamertag");
    return fn ? fn(user, component, size, buf, used) : E_NOTIMPL;
}

void __stdcall XUserCloseHandle(void* user) {
    auto fn = resolve_xgr<void(__stdcall*)(void*)>("XUserCloseHandle");
    if (fn) fn(user);
}

HRESULT __stdcall XPackageGetCurrentProcessPackageIdentifier(size_t buf_size, char* buf) {
    auto fn = resolve_xgr<HRESULT(__stdcall*)(size_t, char*)>("XPackageGetCurrentProcessPackageIdentifier");
    return fn ? fn(buf_size, buf) : E_NOTIMPL;
}

HRESULT __stdcall XPackageMountWithUiAsync(const char* pkg_id, void* async) {
    auto fn = resolve_xgr<HRESULT(__stdcall*)(const char*, void*)>("XPackageMountWithUiAsync");
    return fn ? fn(pkg_id, async) : E_NOTIMPL;
}

HRESULT __stdcall XPackageMountWithUiResult(void* async, void** out_mount) {
    auto fn = resolve_xgr<HRESULT(__stdcall*)(void*, void**)>("XPackageMountWithUiResult");
    return fn ? fn(async, out_mount) : E_NOTIMPL;
}

HRESULT __stdcall XPackageGetMountPathSize(void* mount, size_t* path_size) {
    auto fn = resolve_xgr<HRESULT(__stdcall*)(void*, size_t*)>("XPackageGetMountPathSize");
    return fn ? fn(mount, path_size) : E_NOTIMPL;
}

HRESULT __stdcall XPackageGetMountPath(void* mount, size_t path_size, char* path) {
    auto fn = resolve_xgr<HRESULT(__stdcall*)(void*, size_t, char*)>("XPackageGetMountPath");
    return fn ? fn(mount, path_size, path) : E_NOTIMPL;
}

void __stdcall XPackageCloseMountHandle(void* mount) {
    auto fn = resolve_xgr<void(__stdcall*)(void*)>("XPackageCloseMountHandle");
    if (fn) fn(mount);
}

HRESULT __stdcall XStoreCreateContext(void* user, void** out_ctx) {
    auto fn = resolve_xgr<HRESULT(__stdcall*)(void*, void**)>("XStoreCreateContext");
    return fn ? fn(user, out_ctx) : E_NOTIMPL;
}

void __stdcall XStoreCloseContextHandle(void* ctx) {
    auto fn = resolve_xgr<void(__stdcall*)(void*)>("XStoreCloseContextHandle");
    if (fn) fn(ctx);
}

HRESULT __stdcall GameInputCreate(void** out_gi) {
    auto fn = resolve_xgr<HRESULT(__stdcall*)(void**)>("GameInputCreate");
    return fn ? fn(out_gi) : E_NOTIMPL;
}

}  // extern "C"

BOOL WINAPI DllMain(HINSTANCE /*hinst*/, DWORD /*reason*/, LPVOID /*reserved*/) {
    return TRUE;
}
