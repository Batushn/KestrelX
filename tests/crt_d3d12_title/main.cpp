#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

// Full-CRT standard Win32 + D3D12 + DXGI + Static TLS test executable.
// Built with standard MSVC CRT (wWinMainCRTStartup, /MD, /SUBSYSTEM:WINDOWS, NO /NODEFAULTLIB,
// NO custom /ENTRY:) to prove KestrelX can load and execute unmodified Win32/D3D12 PE binaries.

namespace {

thread_local uint32_t g_tls_magic = 0x5A17C0DEu;
thread_local uint64_t g_tls_counter = 0;

std::atomic<uint32_t> g_atexit_ran{0};

void __cdecl on_guest_atexit() {
    g_atexit_ran.fetch_add(1, std::memory_order_relaxed);
}

DWORD WINAPI tls_worker_proc(LPVOID) {
    // Worker thread must receive a fresh initialized copy of .tls (0x5A17C0DEu, counter == 0)
    if (g_tls_magic != 0x5A17C0DEu) return 101u;
    if (g_tls_counter != 0u) return 102u;
    g_tls_magic = 0xAABBCCDDu;
    g_tls_counter = 999u;
    if (g_tls_magic != 0xAABBCCDDu || g_tls_counter != 999u) return 103u;
    return 0u;
}

int run_tls_and_identity_mode(HINSTANCE hInstance, LPCWSTR lpCmdLine) {
    // 1. Verify initial static TLS template copy on main guest thread
    if (g_tls_magic != 0x5A17C0DEu || g_tls_counter != 0u) return 10;
    g_tls_magic = 0x11223344u;
    g_tls_counter = 42u;

    // 2. Verify CreateThread propagates static TLS to child threads with full per-thread isolation
    HANDLE worker = CreateThread(nullptr, 0, &tls_worker_proc, nullptr, 0, nullptr);
    if (!worker) return 11;
    WaitForSingleObject(worker, 5000);
    DWORD worker_rc = 999u;
    GetExitCodeThread(worker, &worker_rc);
    CloseHandle(worker);
    if (worker_rc != 0u) return static_cast<int>(worker_rc);
    if (g_tls_magic != 0x11223344u || g_tls_counter != 42u) return 12;

    // 3. Verify guest process identity overlays
    HMODULE self = GetModuleHandleW(nullptr);
    if (!self || self != hInstance) return 13;

    wchar_t path_buf[MAX_PATH]{};
    DWORD path_len = GetModuleFileNameW(nullptr, path_buf, MAX_PATH);
    if (path_len == 0) return 14;
    std::wstring full_path(path_buf, path_len);
    if (full_path.find(L"kestrel_crt_d3d12_title.exe") == std::wstring::npos) return 15;

    LPCWSTR raw_cmd = GetCommandLineW();
    if (!raw_cmd || std::wstring(raw_cmd).find(L"--mode=tls") == std::wstring::npos) return 16;
    if (!lpCmdLine || std::wstring(lpCmdLine).find(L"--mode=tls") == std::wstring::npos) return 17;

    // 4. Verify dynamic LoadLibraryW / GetProcAddress against HLE console module + D3D12 ordinal #101
    HMODULE h_xg = LoadLibraryW(L"xgameruntime.dll");
    if (!h_xg) return 18;
    using XGameRuntimeInitFn = HRESULT(__stdcall*)();
    auto init_fn = reinterpret_cast<XGameRuntimeInitFn>(GetProcAddress(h_xg, "XGameRuntimeInitialize"));
    if (!init_fn) return 19;
    if (FAILED(init_fn())) return 20;
    if (!FreeLibrary(h_xg)) return 21;

    HMODULE h_d3d = LoadLibraryW(L"d3d12.dll");
    if (!h_d3d) return 22;
    FARPROC ord101 = GetProcAddress(h_d3d, MAKEINTRESOURCEA(101));
    if (!ord101) return 23;
    FreeLibrary(h_d3d);

    // 5. Register atexit handler and terminate via ExitProcess(77) to prove host isolation
    std::atexit(&on_guest_atexit);
    ExitProcess(77u);
    return 99;
}

LRESULT CALLBACK d3d12_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CLOSE) {
        DestroyWindow(hwnd);
        return 0;
    }
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int run_d3d12_swapchain_mode(HINSTANCE hInstance) {
    constexpr UINT kW = 256;
    constexpr UINT kH = 256;
    constexpr UINT kBuffers = 2;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &d3d12_wnd_proc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"KestrelCrtD3D12TitleClass";
    RegisterClassExW(&wc);

    RECT rc{0, 0, static_cast<LONG>(kW), static_cast<LONG>(kH)};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"KestrelX Full-CRT D3D12 Title",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) {
        UnregisterClassW(wc.lpszClassName, hInstance);
        return 30;
    }

    IDXGIFactory4* factory = nullptr;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))) || !factory) {
        DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, hInstance);
        return 31;
    }

    ID3D12Device* dev = nullptr;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev))) || !dev) {
        factory->Release();
        DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, hInstance);
        return 32;
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queue = nullptr;
    if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) || !queue) {
        dev->Release();
        factory->Release();
        DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, hInstance);
        return 33;
    }

    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width = kW;
    scd.Height = kH;
    scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = kBuffers;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    IDXGISwapChain1* sc1 = nullptr;
    if (FAILED(factory->CreateSwapChainForHwnd(queue, hwnd, &scd, nullptr, nullptr, &sc1)) || !sc1) {
        queue->Release();
        dev->Release();
        factory->Release();
        DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, hInstance);
        return 34;
    }

    IDXGISwapChain3* sc3 = nullptr;
    if (FAILED(sc1->QueryInterface(IID_PPV_ARGS(&sc3))) || !sc3) {
        sc1->Release();
        queue->Release();
        dev->Release();
        factory->Release();
        DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, hInstance);
        return 35;
    }
    sc1->Release();

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = kBuffers;
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap))) || !rtv_heap) return 36;
    const UINT rtv_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    ID3D12Resource* rts[kBuffers]{};
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_h = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kBuffers; ++i) {
        if (FAILED(sc3->GetBuffer(i, IID_PPV_ARGS(&rts[i]))) || !rts[i]) return 37;
        dev->CreateRenderTargetView(rts[i], nullptr, rtv_h);
        rtv_h.ptr += rtv_inc;
    }

    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* cl = nullptr;
    ID3D12Fence* fence = nullptr;
    if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)))) return 38;
    if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&cl)))) return 39;
    cl->Close();
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return 40;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT64 fence_val = 0;

    MSG msg{};
    bool running = true;
    uint32_t safety_iters = 0;
    while (running && safety_iters < 64u) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running) break;

        const UINT idx = sc3->GetCurrentBackBufferIndex();
        alloc->Reset();
        cl->Reset(alloc, nullptr);

        D3D12_RESOURCE_BARRIER to_rt{};
        to_rt.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_rt.Transition.pResource = rts[idx];
        to_rt.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        to_rt.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        to_rt.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cl->ResourceBarrier(1, &to_rt);

        D3D12_CPU_DESCRIPTOR_HANDLE dst_rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        dst_rtv.ptr += static_cast<SIZE_T>(idx) * rtv_inc;
        const float clear_rgba[4] = {0.10f, 0.80f, 0.30f, 1.0f};
        cl->ClearRenderTargetView(dst_rtv, clear_rgba, 0, nullptr);

        D3D12_RESOURCE_BARRIER to_pres = to_rt;
        to_pres.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        to_pres.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        cl->ResourceBarrier(1, &to_pres);

        cl->Close();
        ID3D12CommandList* lists[] = {cl};
        queue->ExecuteCommandLists(1, lists);
        sc3->Present(0, 0);

        ++fence_val;
        queue->Signal(fence, fence_val);
        if (fence->GetCompletedValue() < fence_val) {
            fence->SetEventOnCompletion(fence_val, ev);
            WaitForSingleObject(ev, 5000);
        }
        ++safety_iters;
    }

    CloseHandle(ev);
    fence->Release();
    cl->Release();
    alloc->Release();
    for (UINT i = 0; i < kBuffers; ++i) rts[i]->Release();
    rtv_heap->Release();
    sc3->Release();
    queue->Release();
    dev->Release();
    factory->Release();
    UnregisterClassW(wc.lpszClassName, hInstance);
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR lpCmdLine, int) {
    const std::wstring args = lpCmdLine ? lpCmdLine : L"";
    if (args.find(L"--mode=d3d12") != std::wstring::npos) {
        return run_d3d12_swapchain_mode(hInstance);
    }
    return run_tls_and_identity_mode(hInstance, lpCmdLine);
}
