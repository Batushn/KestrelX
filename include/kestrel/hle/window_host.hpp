#pragma once

#include "kestrel/common/types.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>
#include <dxgi1_5.h>
#include <windows.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace kestrel::hle {

enum class PresentSyncMode : uint8_t {
    VSync = 0,
    VariableRefreshTearing = 1,
    Uncapped = 2,
};

struct FramePacingConfig {
    double target_fps = 0.0;          // 0.0 = uncapped / native sync
    PresentSyncMode sync_mode = PresentSyncMode::VSync;
    uint32_t max_frame_latency = 1;   // Waitable swapchain latency (1..3)
    bool hud_enabled = false;         // Low-latency PresentX telemetry HUD overlay
    bool borderless_fullscreen = false;
};

struct FramePacingStats {
    uint64_t frame_count = 0;
    double last_frame_ms = 0.0;
    double avg_frame_ms = 0.0;
    double p99_frame_ms = 0.0;
    double min_frame_ms = 0.0;
    double max_frame_ms = 0.0;
    double current_fps = 0.0;
    double present_latency_ms = 0.0;
    uint64_t paced_sleeps = 0;
    uint64_t tearing_presents = 0;
    uint64_t vsync_presents = 0;
    std::vector<float> recent_frame_ms;
};

// High-resolution hybrid (sleep + sub-ms spin) frame limiter & sliding-window
// latency / P99 frametime telemetry tracker for PresentX.
class FramePacer {
public:
    void configure(const FramePacingConfig& cfg);
    FramePacingConfig config() const;
    void reset();

    void begin_frame();
    void end_frame_and_pace(double present_latency_ms = 0.0);
    void record_present_kind(bool used_tearing);
    FramePacingStats stats() const;

private:
    static constexpr size_t kHistoryCapacity = 120;

    mutable std::mutex mtx_;
    FramePacingConfig cfg_{};
    bool has_frame_start_ = false;
    std::chrono::steady_clock::time_point frame_start_{};
    uint64_t frame_count_ = 0;
    double last_frame_ms_ = 0.0;
    double min_frame_ms_ = 0.0;
    double max_frame_ms_ = 0.0;
    double present_latency_ms_ = 0.0;
    uint64_t paced_sleeps_ = 0;
    uint64_t tearing_presents_ = 0;
    uint64_t vsync_presents_ = 0;
    std::vector<double> history_;
};

// Clean-room 5x7 bitmap-font PresentX telemetry HUD rasterizer (236x76 RGBA8).
constexpr uint32_t kTelemetryHudWidth = 236;
constexpr uint32_t kTelemetryHudHeight = 76;

std::vector<uint8_t> render_telemetry_hud_rgba8(
    const FramePacingStats& stats,
    std::string_view gpu_label,
    PresentSyncMode sync_mode);

// GPU-side non-destructive copy-overlay that stamps the RGBA8 PresentX HUD onto a
// D3D12 render target or swapchain backbuffer using an ALIGNED upload footprint.
Result<void> composite_hud_on_d3d12_texture(
    ID3D12Device* dev,
    ID3D12CommandQueue* queue,
    ID3D12Resource* rt,
    D3D12_RESOURCE_STATES current_state,
    const FramePacingStats& stats,
    std::string_view gpu_label,
    PresentSyncMode sync_mode,
    uint32_t origin_x = 8,
    uint32_t origin_y = 8);

// Clean-room Win32 host window + DXGI waitable flip-model swapchain (IDXGISwapChain3)
// wired to D3D12PresentX with tearing VRR, borderless fullscreen toggle, and HUD.
class WindowHost {
public:
    WindowHost() = default;
    ~WindowHost();

    WindowHost(const WindowHost&) = delete;
    WindowHost& operator=(const WindowHost&) = delete;

    Result<void> create(ID3D12Device* dev, ID3D12CommandQueue* queue,
                        uint32_t width, uint32_t height,
                        const wchar_t* title = L"KestrelX - Xbox Series X|S Runtime",
                        const FramePacingConfig& pacing = {});
    Result<void> present_resource(ID3D12Resource* guest_rt,
                                  D3D12_RESOURCE_STATES guest_state = D3D12_RESOURCE_STATE_RENDER_TARGET);
    bool wait_frame_latency(uint32_t timeout_ms = 100);
    void set_frame_pacing(const FramePacingConfig& pacing);
    void set_borderless_fullscreen(bool enabled);
    void toggle_borderless_fullscreen();
    bool is_borderless_fullscreen() const { return borderless_fullscreen_; }
    bool tearing_supported() const { return tearing_supported_; }
    FramePacingStats pacing_stats() const { return pacer_.stats(); }
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
    bool borderless_fullscreen_ = false;
    bool tearing_supported_ = false;
    RECT windowed_rect_{};
    LONG windowed_style_ = WS_OVERLAPPEDWINDOW;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint64_t frames_presented_ = 0;

    FramePacingConfig pacing_cfg_{};
    FramePacer pacer_;
    std::string adapter_label_ = "D3D12";

    ID3D12Device* dev_ = nullptr;
    ID3D12CommandQueue* queue_ = nullptr;
    IDXGISwapChain3* swapchain_ = nullptr;
    ID3D12Resource* buffers_[2] = {nullptr, nullptr};
    ID3D12CommandAllocator* alloc_ = nullptr;
    ID3D12GraphicsCommandList* cmd_ = nullptr;
    ID3D12Fence* fence_ = nullptr;
    HANDLE fence_event_ = nullptr;
    HANDLE frame_latency_waitable_ = nullptr;
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
