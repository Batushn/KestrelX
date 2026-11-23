#pragma once

#include "kestrel/common/types.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>
#include <dxgi1_4.h>
#include <windows.h>

#include <cstdint>
#include <filesystem>

namespace kestrel::hle {

// Clean-room Win32 host window + DXGI flip-model swapchain (IDXGISwapChain3)
// wired to D3D12PresentX for live windowed presentation.
class WindowHost {
public:
    WindowHost() = default;
    ~WindowHost();

    WindowHost(const WindowHost&) = delete;
    WindowHost& operator=(const WindowHost&) = delete;

    Result<void> create(ID3D12Device* dev, ID3D12CommandQueue* queue,
                        uint32_t width, uint32_t height,
                        const wchar_t* title = L"KestrelX — Xbox Series X|S Runtime");
    Result<void> present_resource(ID3D12Resource* guest_rt,
                                  D3D12_RESOURCE_STATES guest_state = D3D12_RESOURCE_STATE_RENDER_TARGET);
    bool pump_messages();
    void shutdown();

    bool is_open() const { return hwnd_ != nullptr && !close_requested_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    uint64_t frames_presented() const { return frames_presented_; }

private:
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    HWND hwnd_ = nullptr;
    bool close_requested_ = false;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint64_t frames_presented_ = 0;

    ID3D12Device* dev_ = nullptr;
    ID3D12CommandQueue* queue_ = nullptr;
    IDXGISwapChain3* swapchain_ = nullptr;
    ID3D12Resource* buffers_[2] = {nullptr, nullptr};
    ID3D12CommandAllocator* alloc_ = nullptr;
    ID3D12GraphicsCommandList* cmd_ = nullptr;
    ID3D12Fence* fence_ = nullptr;
    HANDLE fence_event_ = nullptr;
    uint64_t fence_val_ = 0;
};

// Headless-safe GPU readback helper: copies an RGBA8 D3D12 2D render target
// and writes a standard 24-bit Windows BMP screenshot to disk.
Result<void> capture_d3d12_texture_bmp(
    ID3D12Device* dev,
    ID3D12CommandQueue* queue,
    ID3D12Resource* rt,
    D3D12_RESOURCE_STATES current_state,
    const std::filesystem::path& bmp_path);

}  // namespace kestrel::hle
