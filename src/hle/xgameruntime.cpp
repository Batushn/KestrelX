#include "kestrel/hle/xgameruntime.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace kestrel::hle {

namespace {

uint32_t g_title_id = 0x4B580001u;
std::atomic<uint32_t> g_users_added{0};
std::atomic<uint32_t> g_store_contexts{0};
std::atomic<uint32_t> g_package_queries{0};
std::atomic<uint32_t> g_async_completed{0};
std::atomic<uint32_t> g_runtime_inits{0};
std::atomic<uint32_t> g_licenses_acquired{0};
std::atomic<uint32_t> g_active_licenses{0};
std::atomic<uint32_t> g_packages_mounted{0};
std::atomic<uint32_t> g_containers_decrypted{0};
std::atomic<uint64_t> g_decrypted_bytes{0};
std::atomic<uint32_t> g_next_user_index{0};
std::atomic<uint64_t> g_active_user_xuid{0x000900004B580001ull};
std::atomic<bool> g_runtime_initialized{false};

vfs::OverlayVfs* g_vfs = nullptr;
formats::GdkKeyRing g_keyring;

using TaskCallback = void(__stdcall*)(void* context, bool canceled);

struct TaskItem {
    TaskCallback cb = nullptr;
    void* ctx = nullptr;
};

struct TaskQueue {
    std::mutex mtx;
    std::deque<TaskItem> work;
    std::deque<TaskItem> completion;
};

struct KxUserHandle {
    uint64_t xuid = 0x000900004B580001ull;
    std::string gamertag = "KestrelPlayer";
    uint32_t local_id = 1;
    uint32_t controller_index = 0;
};

std::mutex g_user_table_mtx;
std::unordered_map<uint32_t, KxUserHandle*> g_users_by_controller;

struct KxStoreContext {
    uint64_t owner_xuid = 0;
    bool license_active = true;
};

struct RegisteredPackageEntry {
    formats::GdkKeyId key_id{};
    std::string license_token;
    std::filesystem::path container_path;
    std::string mount_prefix = "GAME:\\";
};

std::mutex g_pkg_mtx;
std::unordered_map<std::string, RegisteredPackageEntry> g_registered_packages;
std::vector<KxXStoreAddonLicense> g_addon_licenses;

struct KxStoreLicenseHandle {
    std::string package_or_store_id;
    bool is_valid = false;
    bool is_durable = false;
};

struct KxPackageMountHandle {
    std::string package_id;
    std::string mount_path;
};

struct KxCanAcquireAsyncResult {
    KxXStoreCanAcquireLicenseResult value{};
};

struct KxAddonQueryAsyncResult {
    std::vector<KxXStoreAddonLicense> items;
};

struct AsyncInternalState {
    uint32_t magic = 0;
    HRESULT status = E_PENDING;
    void* result_ptr = nullptr;
};
static_assert(sizeof(AsyncInternalState) <= sizeof(KxXAsyncBlock::internal));

constexpr uint32_t kAsyncMagic = 0x4B584153u;  // "KXAS"

void __stdcall async_completion_trampoline(void* ctx, bool) {
    auto* async = static_cast<KxXAsyncBlock*>(ctx);
    if (async && async->callback) {
        async->callback(async);
    }
}

void complete_async(KxXAsyncBlock* async, HRESULT hr, void* result_ptr) {
    AsyncInternalState st{};
    st.magic = kAsyncMagic;
    st.status = hr;
    st.result_ptr = result_ptr;
    std::memcpy(async->internal, &st, sizeof(st));
    g_async_completed.fetch_add(1, std::memory_order_relaxed);

    if (async->queue && async->callback) {
        kx_XTaskQueueSubmitCallback(async->queue, 1, async, &async_completion_trampoline);
    } else if (async->callback) {
        async->callback(async);
    }
}

void copy_cstr_trunc(char* dst, size_t dst_cap, std::string_view src) {
    if (!dst || dst_cap == 0) return;
    const size_t n = (std::min)(src.size(), dst_cap - 1);
    if (n > 0) std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

HRESULT acquire_license_impl(void* store_ctx, const char* id, bool is_durable, KxXAsyncBlock* async) {
    if (!store_ctx || !id || id[0] == '\0' || !async) return E_INVALIDARG;
    const std::string key_str(id);

    HRESULT status = S_OK;
    bool valid = true;

    RegisteredPackageEntry entry{};
    bool found_entry = false;
    {
        std::lock_guard lock(g_pkg_mtx);
        auto it = g_registered_packages.find(key_str);
        if (it != g_registered_packages.end()) {
            entry = it->second;
            found_entry = true;
        }
    }

    if (found_entry) {
        if (!entry.license_token.empty()) {
            auto derived = formats::GdkKeyRing::derive_key_from_token(entry.license_token, entry.key_id);
            if (derived) {
                g_keyring.register_key(entry.key_id, *derived);
            } else {
                status = E_FAIL;
                valid = false;
            }
        } else if (!g_keyring.find_key(entry.key_id).has_value()) {
            status = E_ACCESSDENIED;
            valid = false;
        }
    }

    if (SUCCEEDED(status) && is_durable) {
        std::lock_guard lock(g_pkg_mtx);
        bool updated = false;
        for (auto& addon : g_addon_licenses) {
            if (std::string_view(addon.sku_store_id) == key_str) {
                addon.is_active = true;
                updated = true;
                break;
            }
        }
        if (!updated) {
            KxXStoreAddonLicense addon{};
            copy_cstr_trunc(addon.sku_store_id, sizeof(addon.sku_store_id), key_str);
            copy_cstr_trunc(addon.in_app_offer_token, sizeof(addon.in_app_offer_token), "kestrel-durable-token");
            addon.is_active = true;
            addon.expiration_date = 0x7FFFFFFFFFFFFFFFull;
            g_addon_licenses.push_back(addon);
        }
    }

    auto* lic = new KxStoreLicenseHandle();
    lic->package_or_store_id = key_str;
    lic->is_valid = valid;
    lic->is_durable = is_durable;

    complete_async(async, status, lic);
    return S_OK;
}

HRESULT acquire_license_result_impl(KxXAsyncBlock* async, void** out_license) {
    if (!async || !out_license) return E_INVALIDARG;
    AsyncInternalState st{};
    std::memcpy(&st, async->internal, sizeof(st));
    if (st.magic != kAsyncMagic) return E_PENDING;

    auto* lic = static_cast<KxStoreLicenseHandle*>(st.result_ptr);
    if (FAILED(st.status)) {
        delete lic;
        st.result_ptr = nullptr;
        std::memcpy(async->internal, &st, sizeof(st));
        *out_license = nullptr;
        return st.status;
    }
    if (!lic) return E_PENDING;

    *out_license = lic;
    g_licenses_acquired.fetch_add(1, std::memory_order_relaxed);
    g_active_licenses.fetch_add(1, std::memory_order_relaxed);
    return S_OK;
}

}  // namespace

HRESULT __stdcall kx_XGameRuntimeInitialize() {
    g_runtime_initialized.store(true, std::memory_order_relaxed);
    g_runtime_inits.fetch_add(1, std::memory_order_relaxed);
    return S_OK;
}

void __stdcall kx_XGameRuntimeUninitialize() {
    g_runtime_initialized.store(false, std::memory_order_relaxed);
}

HRESULT __stdcall kx_XGameGetXboxTitleId(uint32_t* out_id) {
    if (!out_id) return E_INVALIDARG;
    *out_id = g_title_id;
    return S_OK;
}

uint32_t __stdcall kx_XSystemGetDeviceType() { return 4; }

HRESULT __stdcall kx_XTaskQueueCreate(uint32_t, uint32_t, void** out_queue) {
    if (!out_queue) return E_INVALIDARG;
    *out_queue = new TaskQueue();
    return S_OK;
}

HRESULT __stdcall kx_XTaskQueueSubmitCallback(void* queue, uint32_t port, void* ctx, void(__stdcall* cb)(void*, bool)) {
    if (!queue || !cb) return E_INVALIDARG;
    auto* q = static_cast<TaskQueue*>(queue);
    std::lock_guard lock(q->mtx);
    (port == 0 ? q->work : q->completion).push_back({cb, ctx});
    return S_OK;
}

bool __stdcall kx_XTaskQueueDispatch(void* queue, uint32_t port, uint32_t) {
    if (!queue) return false;
    auto* q = static_cast<TaskQueue*>(queue);
    TaskItem item{};
    {
        std::lock_guard lock(q->mtx);
        auto& dq = (port == 0) ? q->work : q->completion;
        if (dq.empty()) return false;
        item = dq.front();
        dq.pop_front();
    }
    item.cb(item.ctx, false);
    return true;
}

void __stdcall kx_XTaskQueueCloseHandle(void* queue) { delete static_cast<TaskQueue*>(queue); }

HRESULT __stdcall kx_XAsyncGetStatus(KxXAsyncBlock* async, bool) {
    if (!async) return E_INVALIDARG;
    AsyncInternalState st{};
    std::memcpy(&st, async->internal, sizeof(st));
    if (st.magic != kAsyncMagic) return E_PENDING;
    return st.status;
}

HRESULT __stdcall kx_XUserAddAsync(uint32_t, KxXAsyncBlock* async) {
    if (!async) return E_INVALIDARG;
    const uint32_t user_idx = g_next_user_index.fetch_add(1, std::memory_order_relaxed) + 1;
    auto* user = new KxUserHandle();
    user->local_id = user_idx;
    user->xuid = 0x000900004B580000ull + user_idx;
    user->gamertag = (user_idx == 1) ? "KestrelPlayer" : ("KestrelPlayer" + std::to_string(user_idx));
    user->controller_index = user_idx - 1;

    {
        std::lock_guard lock(g_user_table_mtx);
        g_users_by_controller[user->controller_index] = user;
    }
    if (user_idx == 1) {
        g_active_user_xuid.store(user->xuid, std::memory_order_relaxed);
    }

    complete_async(async, S_OK, user);
    return S_OK;
}

HRESULT __stdcall kx_XUserAddResult(KxXAsyncBlock* async, void** out_user) {
    if (!async || !out_user) return E_INVALIDARG;
    AsyncInternalState st{};
    std::memcpy(&st, async->internal, sizeof(st));
    if (st.magic != kAsyncMagic || !st.result_ptr) return E_PENDING;
    *out_user = st.result_ptr;
    g_users_added.fetch_add(1, std::memory_order_relaxed);
    return S_OK;
}

HRESULT __stdcall kx_XUserGetId(void* user, uint64_t* out_id) {
    if (!user || !out_id) return E_INVALIDARG;
    *out_id = static_cast<KxUserHandle*>(user)->xuid;
    return S_OK;
}

HRESULT __stdcall kx_XUserGetGamertag(void* user, uint32_t, size_t buf_size, char* buf, size_t* out_used) {
    if (!user || !buf || buf_size == 0) return E_INVALIDARG;
    const auto& tag = static_cast<KxUserHandle*>(user)->gamertag;
    const size_t needed = tag.size() + 1;
    if (buf_size < needed) {
        if (out_used) *out_used = needed;
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }
    std::memcpy(buf, tag.c_str(), needed);
    if (out_used) *out_used = needed;
    return S_OK;
}

HRESULT __stdcall kx_XUserFindControllerUser(uint32_t controller_index, void** out_user) {
    if (!out_user) return E_INVALIDARG;
    std::lock_guard lock(g_user_table_mtx);
    auto it = g_users_by_controller.find(controller_index);
    if (it != g_users_by_controller.end()) {
        *out_user = it->second;
        return S_OK;
    }
    *out_user = nullptr;
    return HRESULT_FROM_WIN32(ERROR_NO_SUCH_USER);
}

HRESULT __stdcall kx_XUserSetActive(void* user) {
    if (!user) return E_INVALIDARG;
    auto* u = static_cast<KxUserHandle*>(user);
    g_active_user_xuid.store(u->xuid, std::memory_order_relaxed);
    return S_OK;
}

uint64_t __stdcall kx_XUserGetActiveXuid() {
    return g_active_user_xuid.load(std::memory_order_relaxed);
}

void __stdcall kx_XUserCloseHandle(void* user) {
    if (!user) return;
    auto* u = static_cast<KxUserHandle*>(user);
    {
        std::lock_guard lock(g_user_table_mtx);
        for (auto it = g_users_by_controller.begin(); it != g_users_by_controller.end(); ) {
            if (it->second == u) {
                it = g_users_by_controller.erase(it);
            } else {
                ++it;
            }
        }
    }
    delete u;
}

HRESULT __stdcall kx_XPackageGetCurrentProcessPackageIdentifier(size_t buf_size, char* buf) {
    if (!buf || buf_size == 0) return E_INVALIDARG;
    static constexpr const char kPkgId[] = "KestrelX.CleanRoom.Title_1.0.0.0_x64__kestrel";
    constexpr size_t kLen = sizeof(kPkgId);
    if (buf_size < kLen) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    std::memcpy(buf, kPkgId, kLen);
    g_package_queries.fetch_add(1, std::memory_order_relaxed);
    return S_OK;
}

HRESULT __stdcall kx_XPackageMountWithUiAsync(const char* package_id, KxXAsyncBlock* async) {
    if (!package_id || package_id[0] == '\0' || !async) return E_INVALIDARG;
    const std::string pkg_key(package_id);

    RegisteredPackageEntry entry{};
    bool found_entry = false;
    {
        std::lock_guard lock(g_pkg_mtx);
        auto it = g_registered_packages.find(pkg_key);
        if (it != g_registered_packages.end()) {
            entry = it->second;
            found_entry = true;
        }
    }

    std::filesystem::path container_path;
    std::string mount_prefix = "GAME:\\";
    if (found_entry) {
        container_path = entry.container_path;
        mount_prefix = entry.mount_prefix;
    } else {
        std::error_code ec;
        std::filesystem::path p(pkg_key);
        if (std::filesystem::exists(p, ec) && std::filesystem::is_regular_file(p, ec)) {
            container_path = p;
        }
    }

    auto* mount = new KxPackageMountHandle();
    mount->package_id = pkg_key;
    mount->mount_path = g_vfs ? g_vfs->overlay_root().string() : "G:\\";

    HRESULT status = S_OK;
    if (!container_path.empty()) {
        auto parsed = formats::GdkContentContainer::parse_file(container_path);
        if (!parsed) {
            status = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        } else if (g_vfs) {
            auto mounted = parsed->mount_to_vfs(*g_vfs, g_keyring, mount_prefix);
            if (!mounted) {
                status = E_ACCESSDENIED;
            } else {
                uint64_t total_seg_bytes = 0;
                for (const auto& seg : parsed->segments()) {
                    total_seg_bytes += seg.logical_size;
                }
                g_packages_mounted.fetch_add(1, std::memory_order_relaxed);
                if (parsed->header().cipher == formats::GdkCipherMode::Aes128Ctr) {
                    g_containers_decrypted.fetch_add(1, std::memory_order_relaxed);
                    g_decrypted_bytes.fetch_add(total_seg_bytes, std::memory_order_relaxed);
                }
            }
        } else {
            g_packages_mounted.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        if (g_vfs) {
            mount->mount_path = g_vfs->game_root().string();
        }
        g_packages_mounted.fetch_add(1, std::memory_order_relaxed);
    }

    complete_async(async, status, mount);
    return S_OK;
}

HRESULT __stdcall kx_XPackageMountWithUiResult(KxXAsyncBlock* async, void** out_mount) {
    if (!async || !out_mount) return E_INVALIDARG;
    AsyncInternalState st{};
    std::memcpy(&st, async->internal, sizeof(st));
    if (st.magic != kAsyncMagic) return E_PENDING;

    auto* mount = static_cast<KxPackageMountHandle*>(st.result_ptr);
    if (FAILED(st.status)) {
        delete mount;
        st.result_ptr = nullptr;
        std::memcpy(async->internal, &st, sizeof(st));
        *out_mount = nullptr;
        return st.status;
    }
    if (!mount) return E_PENDING;
    *out_mount = mount;
    return S_OK;
}

HRESULT __stdcall kx_XPackageGetMountPathSize(void* mount, size_t* path_size) {
    if (!mount || !path_size) return E_INVALIDARG;
    *path_size = static_cast<KxPackageMountHandle*>(mount)->mount_path.size() + 1;
    return S_OK;
}

HRESULT __stdcall kx_XPackageGetMountPath(void* mount, size_t path_size, char* path) {
    if (!mount || !path || path_size == 0) return E_INVALIDARG;
    const auto& mp = static_cast<KxPackageMountHandle*>(mount)->mount_path;
    const size_t needed = mp.size() + 1;
    if (path_size < needed) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    std::memcpy(path, mp.c_str(), needed);
    return S_OK;
}

void __stdcall kx_XPackageCloseMountHandle(void* mount) {
    delete static_cast<KxPackageMountHandle*>(mount);
}

HRESULT __stdcall kx_XStoreCreateContext(void* user, void** out_ctx) {
    if (!out_ctx) return E_INVALIDARG;
    auto* ctx = new KxStoreContext();
    if (user) ctx->owner_xuid = static_cast<KxUserHandle*>(user)->xuid;
    *out_ctx = ctx;
    g_store_contexts.fetch_add(1, std::memory_order_relaxed);
    return S_OK;
}

void __stdcall kx_XStoreCloseContextHandle(void* ctx) {
    delete static_cast<KxStoreContext*>(ctx);
}

HRESULT __stdcall kx_XStoreAcquireLicenseForPackageAsync(void* store_ctx, const char* package_id, KxXAsyncBlock* async) {
    return acquire_license_impl(store_ctx, package_id, /*is_durable=*/false, async);
}

HRESULT __stdcall kx_XStoreAcquireLicenseForPackageResult(KxXAsyncBlock* async, void** out_license) {
    return acquire_license_result_impl(async, out_license);
}

HRESULT __stdcall kx_XStoreAcquireLicenseForDurablesAsync(void* store_ctx, const char* store_id, KxXAsyncBlock* async) {
    return acquire_license_impl(store_ctx, store_id, /*is_durable=*/true, async);
}

HRESULT __stdcall kx_XStoreAcquireLicenseForDurablesResult(KxXAsyncBlock* async, void** out_license) {
    return acquire_license_result_impl(async, out_license);
}

HRESULT __stdcall kx_XStoreCanAcquireLicenseForStoreIdAsync(void* store_ctx, const char* store_id, KxXAsyncBlock* async) {
    if (!store_ctx || !store_id || store_id[0] == '\0' || !async) return E_INVALIDARG;
    auto* res = new KxCanAcquireAsyncResult();
    res->value.status = 1;  // Licensable
    copy_cstr_trunc(res->value.store_id, sizeof(res->value.store_id), store_id);
    complete_async(async, S_OK, res);
    return S_OK;
}

HRESULT __stdcall kx_XStoreCanAcquireLicenseForStoreIdResult(KxXAsyncBlock* async, KxXStoreCanAcquireLicenseResult* out_result) {
    if (!async || !out_result) return E_INVALIDARG;
    AsyncInternalState st{};
    std::memcpy(&st, async->internal, sizeof(st));
    if (st.magic != kAsyncMagic || !st.result_ptr) return E_PENDING;
    auto* res = static_cast<KxCanAcquireAsyncResult*>(st.result_ptr);
    *out_result = res->value;
    delete res;
    st.result_ptr = nullptr;
    std::memcpy(async->internal, &st, sizeof(st));
    return st.status;
}

bool __stdcall kx_XStoreIsLicenseValid(void* license) {
    if (!license) return false;
    return static_cast<KxStoreLicenseHandle*>(license)->is_valid;
}

void __stdcall kx_XStoreCloseLicenseHandle(void* license) {
    if (!license) return;
    auto* lic = static_cast<KxStoreLicenseHandle*>(license);
    if (lic->is_valid) {
        uint32_t prev = g_active_licenses.load(std::memory_order_relaxed);
        while (prev > 0 && !g_active_licenses.compare_exchange_weak(prev, prev - 1, std::memory_order_relaxed)) {}
    }
    lic->is_valid = false;
    delete lic;
}

HRESULT __stdcall kx_XStoreQueryAddOnLicensesAsync(void* store_ctx, KxXAsyncBlock* async) {
    if (!store_ctx || !async) return E_INVALIDARG;
    auto* res = new KxAddonQueryAsyncResult();
    {
        std::lock_guard lock(g_pkg_mtx);
        res->items = g_addon_licenses;
    }
    complete_async(async, S_OK, res);
    return S_OK;
}

HRESULT __stdcall kx_XStoreQueryAddOnLicensesResultCount(KxXAsyncBlock* async, uint32_t* count) {
    if (!async || !count) return E_INVALIDARG;
    AsyncInternalState st{};
    std::memcpy(&st, async->internal, sizeof(st));
    if (st.magic != kAsyncMagic || !st.result_ptr) return E_PENDING;
    auto* res = static_cast<KxAddonQueryAsyncResult*>(st.result_ptr);
    *count = static_cast<uint32_t>(res->items.size());
    return st.status;
}

HRESULT __stdcall kx_XStoreQueryAddOnLicensesResult(KxXAsyncBlock* async, uint32_t count, KxXStoreAddonLicense* licenses) {
    if (!async || (count > 0 && !licenses)) return E_INVALIDARG;
    AsyncInternalState st{};
    std::memcpy(&st, async->internal, sizeof(st));
    if (st.magic != kAsyncMagic || !st.result_ptr) return E_PENDING;
    auto* res = static_cast<KxAddonQueryAsyncResult*>(st.result_ptr);
    const size_t n = (std::min)(static_cast<size_t>(count), res->items.size());
    for (size_t i = 0; i < n; ++i) {
        licenses[i] = res->items[i];
    }
    delete res;
    st.result_ptr = nullptr;
    std::memcpy(async->internal, &st, sizeof(st));
    return st.status;
}

void set_xgameruntime_title_id(uint32_t title_id) { g_title_id = title_id; }

void set_xgameruntime_vfs(vfs::OverlayVfs* vfs) { g_vfs = vfs; }

formats::GdkKeyRing& xgameruntime_keyring() { return g_keyring; }

void register_gdk_package_license(
    std::string_view package_or_store_id,
    const formats::GdkKeyId& key_id,
    std::string_view license_token,
    const std::filesystem::path& container_path,
    std::string_view mount_prefix) {
    std::lock_guard lock(g_pkg_mtx);
    RegisteredPackageEntry entry{};
    entry.key_id = key_id;
    entry.license_token = std::string(license_token);
    entry.container_path = container_path;
    entry.mount_prefix = mount_prefix.empty() ? std::string("GAME:\\") : std::string(mount_prefix);
    g_registered_packages[std::string(package_or_store_id)] = std::move(entry);
}

XGameRuntimeStats xgameruntime_stats() {
    return {
        g_users_added.load(std::memory_order_relaxed),
        g_store_contexts.load(std::memory_order_relaxed),
        g_package_queries.load(std::memory_order_relaxed),
        g_async_completed.load(std::memory_order_relaxed),
        g_runtime_inits.load(std::memory_order_relaxed),
        g_licenses_acquired.load(std::memory_order_relaxed),
        g_active_licenses.load(std::memory_order_relaxed),
        g_packages_mounted.load(std::memory_order_relaxed),
        g_containers_decrypted.load(std::memory_order_relaxed),
        g_decrypted_bytes.load(std::memory_order_relaxed),
    };
}

void reset_xgameruntime_stats() {
    g_users_added = 0;
    g_store_contexts = 0;
    g_package_queries = 0;
    g_async_completed = 0;
    g_runtime_inits = 0;
    g_licenses_acquired = 0;
    g_active_licenses = 0;
    g_packages_mounted = 0;
    g_containers_decrypted = 0;
    g_decrypted_bytes = 0;
    g_next_user_index = 0;
    g_active_user_xuid = 0x000900004B580001ull;
    g_runtime_initialized = false;
    g_keyring.clear();
    {
        std::lock_guard lock(g_user_table_mtx);
        g_users_by_controller.clear();
    }
    {
        std::lock_guard lock(g_pkg_mtx);
        g_registered_packages.clear();
        g_addon_licenses.clear();
    }
}

void register_xgameruntime(Registry& reg) {
    reg.register_module("xgameruntime.dll");
    reg.register_export("xgameruntime.dll", "XGameRuntimeInitialize", reinterpret_cast<void*>(&kx_XGameRuntimeInitialize));
    reg.register_export("xgameruntime.dll", "XGameRuntimeUninitialize", reinterpret_cast<void*>(&kx_XGameRuntimeUninitialize));
    reg.register_export("xgameruntime.dll", "XGameGetXboxTitleId", reinterpret_cast<void*>(&kx_XGameGetXboxTitleId));
    reg.register_export("xgameruntime.dll", "XSystemGetDeviceType", reinterpret_cast<void*>(&kx_XSystemGetDeviceType));
    reg.register_export("xgameruntime.dll", "XTaskQueueCreate", reinterpret_cast<void*>(&kx_XTaskQueueCreate));
    reg.register_export("xgameruntime.dll", "XTaskQueueSubmitCallback", reinterpret_cast<void*>(&kx_XTaskQueueSubmitCallback));
    reg.register_export("xgameruntime.dll", "XTaskQueueDispatch", reinterpret_cast<void*>(&kx_XTaskQueueDispatch));
    reg.register_export("xgameruntime.dll", "XTaskQueueCloseHandle", reinterpret_cast<void*>(&kx_XTaskQueueCloseHandle));
    reg.register_export("xgameruntime.dll", "XAsyncGetStatus", reinterpret_cast<void*>(&kx_XAsyncGetStatus));
    reg.register_export("xgameruntime.dll", "XUserAddAsync", reinterpret_cast<void*>(&kx_XUserAddAsync));
    reg.register_export("xgameruntime.dll", "XUserAddResult", reinterpret_cast<void*>(&kx_XUserAddResult));
    reg.register_export("xgameruntime.dll", "XUserGetId", reinterpret_cast<void*>(&kx_XUserGetId));
    reg.register_export("xgameruntime.dll", "XUserGetGamertag", reinterpret_cast<void*>(&kx_XUserGetGamertag));
    reg.register_export("xgameruntime.dll", "XUserFindControllerUser", reinterpret_cast<void*>(&kx_XUserFindControllerUser));
    reg.register_export("xgameruntime.dll", "XUserSetActive", reinterpret_cast<void*>(&kx_XUserSetActive));
    reg.register_export("xgameruntime.dll", "XUserGetActiveXuid", reinterpret_cast<void*>(&kx_XUserGetActiveXuid));
    reg.register_export("xgameruntime.dll", "XUserCloseHandle", reinterpret_cast<void*>(&kx_XUserCloseHandle));
    reg.register_export("xgameruntime.dll", "XPackageGetCurrentProcessPackageIdentifier", reinterpret_cast<void*>(&kx_XPackageGetCurrentProcessPackageIdentifier));
    reg.register_export("xgameruntime.dll", "XPackageMountWithUiAsync", reinterpret_cast<void*>(&kx_XPackageMountWithUiAsync));
    reg.register_export("xgameruntime.dll", "XPackageMountWithUiResult", reinterpret_cast<void*>(&kx_XPackageMountWithUiResult));
    reg.register_export("xgameruntime.dll", "XPackageGetMountPathSize", reinterpret_cast<void*>(&kx_XPackageGetMountPathSize));
    reg.register_export("xgameruntime.dll", "XPackageGetMountPath", reinterpret_cast<void*>(&kx_XPackageGetMountPath));
    reg.register_export("xgameruntime.dll", "XPackageCloseMountHandle", reinterpret_cast<void*>(&kx_XPackageCloseMountHandle));
    reg.register_export("xgameruntime.dll", "XStoreCreateContext", reinterpret_cast<void*>(&kx_XStoreCreateContext));
    reg.register_export("xgameruntime.dll", "XStoreCloseContextHandle", reinterpret_cast<void*>(&kx_XStoreCloseContextHandle));
    reg.register_export("xgameruntime.dll", "XStoreAcquireLicenseForPackageAsync", reinterpret_cast<void*>(&kx_XStoreAcquireLicenseForPackageAsync));
    reg.register_export("xgameruntime.dll", "XStoreAcquireLicenseForPackageResult", reinterpret_cast<void*>(&kx_XStoreAcquireLicenseForPackageResult));
    reg.register_export("xgameruntime.dll", "XStoreAcquireLicenseForDurablesAsync", reinterpret_cast<void*>(&kx_XStoreAcquireLicenseForDurablesAsync));
    reg.register_export("xgameruntime.dll", "XStoreAcquireLicenseForDurablesResult", reinterpret_cast<void*>(&kx_XStoreAcquireLicenseForDurablesResult));
    reg.register_export("xgameruntime.dll", "XStoreCanAcquireLicenseForStoreIdAsync", reinterpret_cast<void*>(&kx_XStoreCanAcquireLicenseForStoreIdAsync));
    reg.register_export("xgameruntime.dll", "XStoreCanAcquireLicenseForStoreIdResult", reinterpret_cast<void*>(&kx_XStoreCanAcquireLicenseForStoreIdResult));
    reg.register_export("xgameruntime.dll", "XStoreIsLicenseValid", reinterpret_cast<void*>(&kx_XStoreIsLicenseValid));
    reg.register_export("xgameruntime.dll", "XStoreCloseLicenseHandle", reinterpret_cast<void*>(&kx_XStoreCloseLicenseHandle));
    reg.register_export("xgameruntime.dll", "XStoreQueryAddOnLicensesAsync", reinterpret_cast<void*>(&kx_XStoreQueryAddOnLicensesAsync));
    reg.register_export("xgameruntime.dll", "XStoreQueryAddOnLicensesResultCount", reinterpret_cast<void*>(&kx_XStoreQueryAddOnLicensesResultCount));
    reg.register_export("xgameruntime.dll", "XStoreQueryAddOnLicensesResult", reinterpret_cast<void*>(&kx_XStoreQueryAddOnLicensesResult));
}

}  // namespace kestrel::hle
