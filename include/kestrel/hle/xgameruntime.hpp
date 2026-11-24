#pragma once

#include "kestrel/formats/gdk_container.hpp"
#include "kestrel/hle/registry.hpp"
#include "kestrel/vfs/overlay_vfs.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <filesystem>
#include <string_view>

namespace kestrel::hle {

struct XGameRuntimeStats {
    uint32_t users_added = 0;
    uint32_t store_contexts = 0;
    uint32_t package_queries = 0;
    uint32_t async_completed = 0;
    uint32_t runtime_inits = 0;
    uint32_t licenses_acquired = 0;
    uint32_t active_licenses = 0;
    uint32_t packages_mounted = 0;
    uint32_t containers_decrypted = 0;
    uint64_t decrypted_bytes = 0;
};

// Public GDK XAsyncBlock ABI layout
struct KxXAsyncBlock {
    void* queue = nullptr;
    void* context = nullptr;
    void(__stdcall* callback)(KxXAsyncBlock* async) = nullptr;
    uint8_t internal[32]{};
};

// Public GDK XStore license & query descriptors
struct KxXStoreCanAcquireLicenseResult {
    uint32_t status = 1;  // 1 = Licensable
    char store_id[64]{};
};

struct KxXStoreAddonLicense {
    char sku_store_id[64]{};
    char in_app_offer_token[64]{};
    bool is_active = false;
    uint64_t expiration_date = 0;
};

void set_xgameruntime_title_id(uint32_t title_id);
void set_xgameruntime_vfs(vfs::OverlayVfs* vfs);
formats::GdkKeyRing& xgameruntime_keyring();
void register_gdk_package_license(
    std::string_view package_or_store_id,
    const formats::GdkKeyId& key_id,
    std::string_view license_token,
    const std::filesystem::path& container_path = {},
    std::string_view mount_prefix = "GAME:\\");
void register_xgameruntime(Registry& reg);
XGameRuntimeStats xgameruntime_stats();
void reset_xgameruntime_stats();

// Direct callable HLE implementations
HRESULT __stdcall kx_XGameRuntimeInitialize();
void __stdcall kx_XGameRuntimeUninitialize();
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
HRESULT __stdcall kx_XPackageMountWithUiAsync(const char* package_id, KxXAsyncBlock* async);
HRESULT __stdcall kx_XPackageMountWithUiResult(KxXAsyncBlock* async, void** out_mount);
HRESULT __stdcall kx_XPackageGetMountPathSize(void* mount, size_t* path_size);
HRESULT __stdcall kx_XPackageGetMountPath(void* mount, size_t path_size, char* path);
void __stdcall kx_XPackageCloseMountHandle(void* mount);
HRESULT __stdcall kx_XStoreCreateContext(void* user, void** out_ctx);
void __stdcall kx_XStoreCloseContextHandle(void* ctx);
HRESULT __stdcall kx_XStoreAcquireLicenseForPackageAsync(void* store_ctx, const char* package_id, KxXAsyncBlock* async);
HRESULT __stdcall kx_XStoreAcquireLicenseForPackageResult(KxXAsyncBlock* async, void** out_license);
HRESULT __stdcall kx_XStoreAcquireLicenseForDurablesAsync(void* store_ctx, const char* store_id, KxXAsyncBlock* async);
HRESULT __stdcall kx_XStoreAcquireLicenseForDurablesResult(KxXAsyncBlock* async, void** out_license);
HRESULT __stdcall kx_XStoreCanAcquireLicenseForStoreIdAsync(void* store_ctx, const char* store_id, KxXAsyncBlock* async);
HRESULT __stdcall kx_XStoreCanAcquireLicenseForStoreIdResult(KxXAsyncBlock* async, KxXStoreCanAcquireLicenseResult* out_result);
bool __stdcall kx_XStoreIsLicenseValid(void* license);
void __stdcall kx_XStoreCloseLicenseHandle(void* license);
HRESULT __stdcall kx_XStoreQueryAddOnLicensesAsync(void* store_ctx, KxXAsyncBlock* async);
HRESULT __stdcall kx_XStoreQueryAddOnLicensesResultCount(KxXAsyncBlock* async, uint32_t* count);
HRESULT __stdcall kx_XStoreQueryAddOnLicensesResult(KxXAsyncBlock* async, uint32_t count, KxXStoreAddonLicense* licenses);

}  // namespace kestrel::hle
