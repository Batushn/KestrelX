#include "kestrel/hle/window_host.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>
#include <vector>

namespace kestrel::hle {

namespace {

constexpr wchar_t kWindowClassName[] = L"KestrelXHostWindowClass";

}  // namespace

WindowHost::~WindowHost() { shutdown(); }

LRESULT CALLBACK WindowHost::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<WindowHost*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    if (msg == WM_CLOSE || msg == WM_DESTROY) {
        if (self) self->close_requested_ = true;
        if (msg == WM_CLOSE) {
            DestroyWindow(hwnd);
            return 0;
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

Result<void> WindowHost::create(ID3D12Device* dev, ID3D12CommandQueue* queue,
                                uint32_t width, uint32_t height, const wchar_t* title) {
    if (!dev || !queue || width == 0 || height == 0) {
        return fail("WindowHost::create invalid arguments");
    }
    shutdown();

    HINSTANCE hinst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &WindowHost::wnd_proc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClassName;
    RegisterClassExW(&wc);

    RECT rc{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd_ = CreateWindowExW(
        0, kWindowClassName, title, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hinst, this);
    if (!hwnd_) {
        return fail("CreateWindowExW failed");
    }
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);

    IDXGIFactory4* factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        shutdown();
        return fail("CreateDXGIFactory1 failed");
    }

    DXGI_SWAP_CHAIN_DESC1 sc_desc{};
    sc_desc.Width = width;
    sc_desc.Height = height;
    sc_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sc_desc.SampleDesc.Count = 1;
    sc_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc_desc.BufferCount = 2;
    sc_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    IDXGISwapChain1* sc1 = nullptr;
    hr = factory->CreateSwapChainForHwnd(queue, hwnd_, &sc_desc, nullptr, nullptr, &sc1);
    factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
    factory->Release();
    if (FAILED(hr) || !sc1) {
        shutdown();
        return fail(std::format("CreateSwapChainForHwnd failed ({:#x})", static_cast<uint32_t>(hr)));
    }

    hr = sc1->QueryInterface(IID_PPV_ARGS(&swapchain_));
    sc1->Release();
    if (FAILED(hr) || !swapchain_) {
        shutdown();
        return fail("QueryInterface(IDXGISwapChain3) failed");
    }

    for (UINT i = 0; i < 2; ++i) {
        if (FAILED(swapchain_->GetBuffer(i, IID_PPV_ARGS(&buffers_[i])))) {
            shutdown();
            return fail("IDXGISwapChain3::GetBuffer failed");
        }
    }

    if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc_))) ||
        FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc_, nullptr, IID_PPV_ARGS(&cmd_))) ||
        FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) {
        shutdown();
        return fail("WindowHost D3D12 command/fence creation failed");
    }
    cmd_->Close();
    fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    dev_ = dev;
    dev_->AddRef();
    queue_ = queue;
    queue_->AddRef();
    width_ = width;
    height_ = height;
    close_requested_ = false;
    return {};
}

Result<void> WindowHost::present_resource(ID3D12Resource* guest_rt, D3D12_RESOURCE_STATES guest_state) {
    if (!swapchain_ || !queue_ || !alloc_ || !cmd_) {
        return fail("WindowHost not initialized");
    }

    if (guest_rt) {
        const UINT back_idx = swapchain_->GetCurrentBackBufferIndex();
        ID3D12Resource* back = buffers_[back_idx];

        alloc_->Reset();
        cmd_->Reset(alloc_, nullptr);

        D3D12_RESOURCE_BARRIER pre[2]{};
        UINT pre_n = 0;
        if (guest_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
            pre[pre_n].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            pre[pre_n].Transition = {guest_rt, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, guest_state, D3D12_RESOURCE_STATE_COPY_SOURCE};
            ++pre_n;
        }
        pre[pre_n].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        pre[pre_n].Transition = {back, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST};
        ++pre_n;
        cmd_->ResourceBarrier(pre_n, pre);

        const D3D12_RESOURCE_DESC src_desc = guest_rt->GetDesc();
        const UINT cw = std::min<UINT>(static_cast<UINT>(src_desc.Width), width_);
        const UINT ch = std::min<UINT>(src_desc.Height, height_);
        D3D12_TEXTURE_COPY_LOCATION dst_loc{back, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}};
        D3D12_TEXTURE_COPY_LOCATION src_loc{guest_rt, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}};
        D3D12_BOX box{0, 0, 0, cw, ch, 1};
        cmd_->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, &box);

        D3D12_RESOURCE_BARRIER post[2]{};
        UINT post_n = 0;
        if (guest_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
            post[post_n].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            post[post_n].Transition = {guest_rt, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE, guest_state};
            ++post_n;
        }
        post[post_n].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        post[post_n].Transition = {back, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT};
        ++post_n;
        cmd_->ResourceBarrier(post_n, post);

        cmd_->Close();
        ID3D12CommandList* lists[] = {cmd_};
        queue_->ExecuteCommandLists(1, lists);
    }

    HRESULT hr = swapchain_->Present(1, 0);
    if (FAILED(hr)) {
        return fail(std::format("IDXGISwapChain3::Present failed ({:#x})", static_cast<uint32_t>(hr)));
    }

    ++fence_val_;
    queue_->Signal(fence_, fence_val_);
    if (fence_->GetCompletedValue() < fence_val_) {
        fence_->SetEventOnCompletion(fence_val_, fence_event_);
        WaitForSingleObject(fence_event_, 5000);
    }

    ++frames_presented_;
    return {};
}

bool WindowHost::pump_messages() {
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) close_requested_ = true;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return !close_requested_;
}

void WindowHost::shutdown() {
    if (fence_event_) { CloseHandle(fence_event_); fence_event_ = nullptr; }
    if (fence_) { fence_->Release(); fence_ = nullptr; }
    if (cmd_) { cmd_->Release(); cmd_ = nullptr; }
    if (alloc_) { alloc_->Release(); alloc_ = nullptr; }
    for (auto*& b : buffers_) {
        if (b) { b->Release(); b = nullptr; }
    }
    if (swapchain_) { swapchain_->Release(); swapchain_ = nullptr; }
    if (queue_) { queue_->Release(); queue_ = nullptr; }
    if (dev_) { dev_->Release(); dev_ = nullptr; }
    if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
    width_ = height_ = 0;
}

Result<void> capture_d3d12_texture_bmp(
    ID3D12Device* dev,
    ID3D12CommandQueue* queue,
    ID3D12Resource* rt,
    D3D12_RESOURCE_STATES current_state,
    const std::filesystem::path& bmp_path) {
    if (!dev || !queue || !rt) {
        return fail("capture_d3d12_texture_bmp invalid args");
    }

    const D3D12_RESOURCE_DESC desc = rt->GetDesc();
    const uint32_t w = static_cast<uint32_t>(desc.Width);
    const uint32_t h = desc.Height;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT num_rows = 0;
    UINT64 row_bytes = 0, total_bytes = 0;
    dev->GetCopyableFootprints(&desc, 0, 1, 0, &fp, &num_rows, &row_bytes, &total_bytes);

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
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* cmd = nullptr;
    ID3D12Fence* fence = nullptr;

    auto cleanup = [&]() {
        if (fence) fence->Release();
        if (cmd) cmd->Release();
        if (alloc) alloc->Release();
        if (readback) readback->Release();
    };

    if (FAILED(dev->CreateCommittedResource(&rb_hp, D3D12_HEAP_FLAG_NONE, &rb_desc,
                                            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))) ||
        FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
        FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&cmd))) ||
        FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
        cleanup();
        return fail("Failed to create capture D3D12 objects");
    }

    if (current_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {rt, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, current_state, D3D12_RESOURCE_STATE_COPY_SOURCE};
        cmd->ResourceBarrier(1, &b);
    }

    D3D12_TEXTURE_COPY_LOCATION dst_loc{readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
    dst_loc.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION src_loc{rt, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}};
    cmd->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);

    if (current_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {rt, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE, current_state};
        cmd->ResourceBarrier(1, &b);
    }

    cmd->Close();
    ID3D12CommandList* lists[] = {cmd};
    queue->ExecuteCommandLists(1, lists);
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    queue->Signal(fence, 1);
    fence->SetEventOnCompletion(1, ev);
    WaitForSingleObject(ev, 5000);
    CloseHandle(ev);

    void* mapped = nullptr;
    D3D12_RANGE range{0, static_cast<SIZE_T>(total_bytes)};
    if (FAILED(readback->Map(0, &range, &mapped))) {
        cleanup();
        return fail("Failed to map readback texture");
    }

    const uint32_t bmp_row_stride = (w * 3u + 3u) & ~3u;
    const uint32_t img_bytes = bmp_row_stride * h;
    std::vector<uint8_t> pixels(img_bytes, 0);
    const auto* src_bytes = static_cast<const uint8_t*>(mapped);

    // BMP stores rows bottom-up in BGR order
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* src_row = src_bytes + static_cast<size_t>(y) * fp.Footprint.RowPitch;
        uint8_t* dst_row = pixels.data() + static_cast<size_t>(h - 1u - y) * bmp_row_stride;
        for (uint32_t x = 0; x < w; ++x) {
            dst_row[x * 3 + 0] = src_row[x * 4 + 2];  // B
            dst_row[x * 3 + 1] = src_row[x * 4 + 1];  // G
            dst_row[x * 3 + 2] = src_row[x * 4 + 0];  // R
        }
    }

    D3D12_RANGE empty{0, 0};
    readback->Unmap(0, &empty);
    cleanup();

    if (bmp_path.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(bmp_path.parent_path(), ec);
    }

    std::ofstream ofs(bmp_path, std::ios::binary);
    if (!ofs) {
        return fail("Failed to open BMP output file");
    }

    BITMAPFILEHEADER bfh{};
    bfh.bfType = 0x4D42;  // 'BM'
    bfh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    bfh.bfSize = bfh.bfOffBits + img_bytes;

    BITMAPINFOHEADER bih{};
    bih.biSize = sizeof(BITMAPINFOHEADER);
    bih.biWidth = static_cast<LONG>(w);
    bih.biHeight = static_cast<LONG>(h);
    bih.biPlanes = 1;
    bih.biBitCount = 24;
    bih.biCompression = BI_RGB;
    bih.biSizeImage = img_bytes;

    ofs.write(reinterpret_cast<const char*>(&bfh), sizeof(bfh));
    ofs.write(reinterpret_cast<const char*>(&bih), sizeof(bih));
    ofs.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
    return {};
}

}  // namespace kestrel::hle
