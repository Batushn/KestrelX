#include "kestrel/hle/xgameruntime.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

namespace kestrel::hle {

namespace {

uint32_t g_title_id = 0x4B580001u;
std::atomic<uint32_t> g_users_added{0};
std::atomic<uint32_t> g_store_contexts{0};
std::atomic<uint32_t> g_package_queries{0};
std::atomic<uint32_t> g_async_completed{0};
std::atomic<uint32_t> g_next_user_index{0};
std::atomic<uint64_t> g_active_user_xuid{0x000900004B580001ull};

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

}  // namespace

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

    AsyncInternalState st{};
    st.magic = kAsyncMagic;
    st.status = S_OK;
    st.result_ptr = user;
    std::memcpy(async->internal, &st, sizeof(st));
    g_async_completed.fetch_add(1, std::memory_order_relaxed);

    if (async->queue && async->callback) {
        kx_XTaskQueueSubmitCallback(async->queue, 1, async, &async_completion_trampoline);
    } else if (async->callback) {
        async->callback(async);
    }
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

void set_xgameruntime_title_id(uint32_t title_id) { g_title_id = title_id; }

XGameRuntimeStats xgameruntime_stats() {
    return {
        g_users_added.load(std::memory_order_relaxed),
        g_store_contexts.load(std::memory_order_relaxed),
        g_package_queries.load(std::memory_order_relaxed),
        g_async_completed.load(std::memory_order_relaxed),
    };
}

void reset_xgameruntime_stats() {
    g_users_added = 0;
    g_store_contexts = 0;
    g_package_queries = 0;
    g_async_completed = 0;
    g_next_user_index = 0;
    g_active_user_xuid = 0x000900004B580001ull;
    std::lock_guard lock(g_user_table_mtx);
    g_users_by_controller.clear();
}

void register_xgameruntime(Registry& reg) {
    reg.register_module("xgameruntime.dll");
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
    reg.register_export("xgameruntime.dll", "XStoreCreateContext", reinterpret_cast<void*>(&kx_XStoreCreateContext));
    reg.register_export("xgameruntime.dll", "XStoreCloseContextHandle", reinterpret_cast<void*>(&kx_XStoreCloseContextHandle));
}

}  // namespace kestrel::hle
