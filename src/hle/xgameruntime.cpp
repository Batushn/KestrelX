#include "kestrel/hle/xgameruntime.hpp"

#include <Windows.h>
#include <deque>
#include <mutex>

namespace kestrel::hle {

namespace {

uint32_t g_title_id = 0x4B580001u;
using TaskCallback = void(__stdcall*)(void* context, bool canceled);

struct TaskItem { TaskCallback cb; void* ctx; };
struct TaskQueue {
    std::mutex mtx;
    std::deque<TaskItem> work;
    std::deque<TaskItem> completion;
};

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

HRESULT __stdcall kx_XTaskQueueSubmitCallback(void* queue, uint32_t port, void* ctx, TaskCallback cb) {
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

}  // namespace

void set_xgameruntime_title_id(uint32_t title_id) { g_title_id = title_id; }

void register_xgameruntime(Registry& reg) {
    reg.register_module("xgameruntime.dll");
    reg.register_export("xgameruntime.dll", "XGameGetXboxTitleId", reinterpret_cast<void*>(&kx_XGameGetXboxTitleId));
    reg.register_export("xgameruntime.dll", "XSystemGetDeviceType", reinterpret_cast<void*>(&kx_XSystemGetDeviceType));
    reg.register_export("xgameruntime.dll", "XTaskQueueCreate", reinterpret_cast<void*>(&kx_XTaskQueueCreate));
    reg.register_export("xgameruntime.dll", "XTaskQueueSubmitCallback", reinterpret_cast<void*>(&kx_XTaskQueueSubmitCallback));
    reg.register_export("xgameruntime.dll", "XTaskQueueDispatch", reinterpret_cast<void*>(&kx_XTaskQueueDispatch));
    reg.register_export("xgameruntime.dll", "XTaskQueueCloseHandle", reinterpret_cast<void*>(&kx_XTaskQueueCloseHandle));
}

}  // namespace kestrel::hle
