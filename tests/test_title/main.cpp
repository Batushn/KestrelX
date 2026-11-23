#include "guest_api.hpp"

#include <cstring>

// Note: kernelx.lib is linked BEFORE kernel32.lib in CMakeLists.txt so CreateFileA
// resolves to kernelx.dll!CreateFileA (intercepted by KestrelX Overlay VFS).

namespace {

volatile uint32_t g_tls_ran = 0;
volatile uint32_t g_task_fired = 0;

void NTAPI guest_tls_callback(PVOID, DWORD reason, PVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_tls_ran = 1;
    }
}

// Register a real PE TLS callback so PeLoader::run_tls_callbacks is exercised.
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:p_guest_tls_cb")
#pragma const_seg(".CRT$XLB")
extern "C" const PIMAGE_TLS_CALLBACK p_guest_tls_cb = guest_tls_callback;
#pragma const_seg()

void __stdcall on_task_callback(void* ctx, bool canceled) {
    if (!canceled && ctx) {
        *static_cast<volatile uint32_t*>(ctx) = 0x12345678u;
    }
}

int test_seh() {
    volatile int caught = 0;
    __try {
        RaiseException(0xE04B5801u, 0, 0, nullptr);
    } __except (GetExceptionCode() == 0xE04B5801u ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        caught = 1;
    }
    return caught;
}

int test_d3d12x_render_and_readback() {
    ID3D12Device* dev = nullptr;
    if (FAILED(D3D12XboxCreateDevice(nullptr, __uuidof(ID3D12Device), reinterpret_cast<void**>(&dev))) || !dev) {
        return 101;
    }

    uint8_t scratch[128];
    for (int i = 0; i < 128; ++i) scratch[i] = 0xAA;
    D3D12ZeroMemoryX(scratch, sizeof(scratch));
    for (int i = 0; i < 128; ++i) {
        if (scratch[i] != 0) { dev->Release(); return 102; }
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queue = nullptr;
    if (FAILED(dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&queue)))) {
        dev->Release();
        return 103;
    }

    ID3D12CommandAllocator* alloc = nullptr;
    if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), reinterpret_cast<void**>(&alloc)))) {
        queue->Release(); dev->Release();
        return 104;
    }

    ID3D12GraphicsCommandList* cmd = nullptr;
    if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&cmd)))) {
        alloc->Release(); queue->Release(); dev->Release();
        return 105;
    }

    D3D12_DESCRIPTOR_HEAP_DESC rtv_hd{};
    rtv_hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_hd.NumDescriptors = 1;
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    if (FAILED(dev->CreateDescriptorHeap(&rtv_hd, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&rtv_heap)))) {
        cmd->Release(); alloc->Release(); queue->Release(); dev->Release();
        return 106;
    }

    D3D12_HEAP_PROPERTIES default_hp{};
    default_hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC tex_desc{};
    tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex_desc.Width = 64;
    tex_desc.Height = 64;
    tex_desc.DepthOrArraySize = 1;
    tex_desc.MipLevels = 1;
    tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    tex_desc.SampleDesc.Count = 1;
    tex_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE cv{};
    cv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    cv.Color[0] = 64.0f / 255.0f;
    cv.Color[1] = 128.0f / 255.0f;
    cv.Color[2] = 191.0f / 255.0f;
    cv.Color[3] = 1.0f;

    ID3D12Resource* rt = nullptr;
    if (FAILED(dev->CreateCommittedResource(&default_hp, D3D12_HEAP_FLAG_NONE, &tex_desc,
                                            D3D12_RESOURCE_STATE_RENDER_TARGET, &cv,
                                            __uuidof(ID3D12Resource), reinterpret_cast<void**>(&rt)))) {
        rtv_heap->Release(); cmd->Release(); alloc->Release(); queue->Release(); dev->Release();
        return 107;
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT num_rows = 0;
    UINT64 row_size = 0, total_bytes = 0;
    dev->GetCopyableFootprints(&tex_desc, 0, 1, 0, &fp, &num_rows, &row_size, &total_bytes);

    D3D12_HEAP_PROPERTIES rb_hp{};
    rb_hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rb_desc{};
    rb_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rb_desc.Width = total_bytes;
    rb_desc.Height = 1;
    rb_desc.DepthOrArraySize = 1;
    rb_desc.MipLevels = 1;
    rb_desc.SampleDesc.Count = 1;
    rb_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ID3D12Resource* readback = nullptr;
    if (FAILED(dev->CreateCommittedResource(&rb_hp, D3D12_HEAP_FLAG_NONE, &rb_desc,
                                            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                            __uuidof(ID3D12Resource), reinterpret_cast<void**>(&readback)))) {
        rt->Release(); rtv_heap->Release(); cmd->Release(); alloc->Release(); queue->Release(); dev->Release();
        return 108;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtv_h = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    dev->CreateRenderTargetView(rt, nullptr, rtv_h);
    cmd->ClearRenderTargetView(rtv_h, cv.Color, 0, nullptr);

    D3D12_RESOURCE_BARRIER bar{};
    bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar.Transition.pResource = rt;
    bar.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    bar.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd->ResourceBarrier(1, &bar);

    D3D12_TEXTURE_COPY_LOCATION dst_loc{};
    dst_loc.pResource = readback;
    dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst_loc.PlacedFootprint = fp;

    D3D12_TEXTURE_COPY_LOCATION src_loc{};
    src_loc.pResource = rt;
    src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src_loc.SubresourceIndex = 0;

    cmd->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);
    cmd->Close();

    ID3D12Fence* fence = nullptr;
    dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&fence));
    HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    ID3D12CommandList* lists[] = {cmd};
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(fence, 1);
    if (fence->GetCompletedValue() < 1) {
        fence->SetEventOnCompletion(1, evt);
        WaitForSingleObject(evt, 10000);
    }
    CloseHandle(evt);

    int rc = 0;
    void* mapped = nullptr;
    D3D12_RANGE rr{0, static_cast<SIZE_T>(total_bytes)};
    if (SUCCEEDED(readback->Map(0, &rr, &mapped)) && mapped) {
        const auto* px = static_cast<const uint8_t*>(mapped);
        if (px[0] != 64 || px[1] != 128 || px[2] != 191 || px[3] != 255) {
            rc = 109;
        }
        D3D12_RANGE wr{0, 0};
        readback->Unmap(0, &wr);
    } else {
        rc = 110;
    }

    fence->Release();
    readback->Release();
    rt->Release();
    rtv_heap->Release();
    cmd->Release();
    alloc->Release();
    queue->Release();
    dev->Release();
    return rc;
}

}  // namespace

extern "C" int __cdecl KestrelGuestEntry() {
    // 1. Verify companion guest DLL attachment + export call
    if (kestrel_guest_dll_attached() != 1) return 1;
    if (kestrel_guest_dll_compute(21) != 42) return 2;

    // 2. Verify TLS callback executed before entry
    if (g_tls_ran != 1) return 3;

    // 3. Verify x64 SEH unwind registered via RtlAddFunctionTable
    if (test_seh() != 1) return 4;

    // 4. Verify kernelx.dll HLE (memory + console type)
    if (GetConsoleType() != 4) return 5;
    void* mem = XMemAlloc(4096, 0);
    if (!mem || XMemSize(mem, 0) != 4096) return 6;
    KxTitleMemoryStatus ms{};
    ms.length = sizeof(ms);
    if (!TitleMemoryStatus(&ms) || ms.total_mem < (8ull << 30) || ms.title_used < 4096) return 7;
    XMemFree(mem, 0);

    // 5. Verify Overlay VFS via kernelx!CreateFileA (read GAME:\, write S:\)
    HANDLE hf = CreateFileA("GAME:\\data\\readonly.txt", GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf == INVALID_HANDLE_VALUE) return 8;
    char buf[64]{};
    DWORD nread = 0;
    ReadFile(hf, buf, sizeof(buf) - 1, &nread, nullptr);
    CloseHandle(hf);
    if (nread < 25 || buf[0] != 'K' || buf[1] != 'e' || buf[7] != 'X') return 9;

    HANDLE hsave = CreateFileA("S:\\save.bin", GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hsave == INVALID_HANDLE_VALUE) return 10;
    const char payload[] = "KESTREL-SAVE-OK";
    DWORD nwritten = 0;
    WriteFile(hsave, payload, sizeof(payload) - 1, &nwritten, nullptr);
    CloseHandle(hsave);
    if (nwritten != sizeof(payload) - 1) return 11;

    // 6. Verify xgameruntime.dll HLE (title id + device type + task queue)
    uint32_t tid = 0;
    if (FAILED(XGameGetXboxTitleId(&tid)) || tid != 0x4B580001u) return 12;
    if (XSystemGetDeviceType() != 4) return 13;
    void* tq = nullptr;
    if (FAILED(XTaskQueueCreate(0, 0, &tq)) || !tq) return 14;
    if (FAILED(XTaskQueueSubmitCallback(tq, 0, const_cast<uint32_t*>(&g_task_fired), on_task_callback))) return 15;
    if (!XTaskQueueDispatch(tq, 0, 0)) return 16;
    XTaskQueueCloseHandle(tq);
    if (g_task_fired != 0x12345678u) return 17;

    // 7. Verify d3d12_xs.dll KxDeviceProxy on real D3D12 GPU (RTV clear + fence + pixel readback)
    return test_d3d12x_render_and_readback();
}
