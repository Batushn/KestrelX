#include "kestrel/hle/window_host.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <format>
#include <fstream>
#include <thread>
#include <vector>

namespace kestrel::hle {

namespace {

constexpr wchar_t kWindowClassName[] = L"KestrelXHostWindowClass";

// Compact clean-room 5x7 bitmap font (each glyph is 7 rows of 5 bits, MSB on left).
struct Glyph5x7 {
    char ch;
    uint8_t rows[7];
};

constexpr Glyph5x7 kFont5x7[] = {
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
    {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x06, 0x08, 0x10, 0x1F}},
    {'3', {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
    {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
    {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
    {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},
    {'D', {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
    {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}},
    {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'J', {0x07, 0x02, 0x02, 0x02, 0x12, 0x12, 0x0C}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}},
    {'N', {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}},
    {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
    {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11}},
    {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}},
    {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
    {':', {0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}},
    {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},
    {'+', {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00}},
    {'/', {0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10}},
    {'[', {0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E}},
    {']', {0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E}},
    {'|', {0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'%', {0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03}},
};

const uint8_t* find_glyph(char c) {
    const char up = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    for (const auto& g : kFont5x7) {
        if (g.ch == up) return g.rows;
    }
    return kFont5x7[0].rows;
}

void put_px(std::vector<uint8_t>& buf, uint32_t w, uint32_t h,
            uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    if (x >= w || y >= h) return;
    const size_t idx = (static_cast<size_t>(y) * w + x) * 4u;
    buf[idx + 0] = r;
    buf[idx + 1] = g;
    buf[idx + 2] = b;
    buf[idx + 3] = a;
}

void fill_rect(std::vector<uint8_t>& buf, uint32_t w, uint32_t h,
               uint32_t x0, uint32_t y0, uint32_t rw, uint32_t rh,
               uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    for (uint32_t dy = 0; dy < rh; ++dy) {
        for (uint32_t dx = 0; dx < rw; ++dx) {
            put_px(buf, w, h, x0 + dx, y0 + dy, r, g, b, a);
        }
    }
}

void draw_text_5x7(std::vector<uint8_t>& buf, uint32_t w, uint32_t h,
                   uint32_t x, uint32_t y, std::string_view text,
                   uint8_t r, uint8_t g, uint8_t b) {
    uint32_t cx = x;
    for (char ch : text) {
        const uint8_t* rows = find_glyph(ch);
        for (uint32_t row = 0; row < 7; ++row) {
            const uint8_t bits = rows[row];
            for (uint32_t col = 0; col < 5; ++col) {
                if ((bits & (1u << (4u - col))) != 0) {
                    put_px(buf, w, h, cx + col, y + row, r, g, b, 255);
                }
            }
        }
        cx += 6;
        if (cx + 5 >= w) break;
    }
}

std::string query_adapter_label(ID3D12Device* dev) {
    if (!dev) return "D3D12";
    const LUID luid = dev->GetAdapterLuid();
    IDXGIFactory4* f = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f))) || !f) return "D3D12";
    IDXGIAdapter1* ad = nullptr;
    std::string out = "D3D12";
    if (SUCCEEDED(f->EnumAdapterByLuid(luid, IID_PPV_ARGS(&ad))) && ad) {
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(ad->GetDesc1(&desc))) {
            char narrow[128]{};
            int n = WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, narrow, sizeof(narrow), nullptr, nullptr);
            if (n > 1) out = narrow;
        }
        ad->Release();
    }
    f->Release();
    return out;
}

}  // namespace

// ---- FramePacer ----

void FramePacer::configure(const FramePacingConfig& cfg) {
    std::lock_guard lock(mtx_);
    cfg_ = cfg;
}

FramePacingConfig FramePacer::config() const {
    std::lock_guard lock(mtx_);
    return cfg_;
}

void FramePacer::reset() {
    std::lock_guard lock(mtx_);
    has_frame_start_ = false;
    frame_count_ = 0;
    last_frame_ms_ = 0.0;
    min_frame_ms_ = 0.0;
    max_frame_ms_ = 0.0;
    present_latency_ms_ = 0.0;
    paced_sleeps_ = 0;
    tearing_presents_ = 0;
    vsync_presents_ = 0;
    history_.clear();
}

void FramePacer::begin_frame() {
    std::lock_guard lock(mtx_);
    frame_start_ = std::chrono::steady_clock::now();
    has_frame_start_ = true;
}

void FramePacer::end_frame_and_pace(double present_latency_ms) {
    double target_fps = 0.0;
    std::chrono::steady_clock::time_point start_tp{};
    {
        std::lock_guard lock(mtx_);
        target_fps = cfg_.target_fps;
        if (!has_frame_start_) {
            frame_start_ = std::chrono::steady_clock::now();
            has_frame_start_ = true;
            ++frame_count_;
            present_latency_ms_ = present_latency_ms;
            return;
        }
        start_tp = frame_start_;
    }

    if (target_fps > 1.0) {
        const auto target_ns = static_cast<int64_t>(1'000'000'000.0 / target_fps);
        const auto deadline = start_tp + std::chrono::nanoseconds(target_ns);
        auto now = std::chrono::steady_clock::now();
        if (now < deadline) {
            const auto remaining = deadline - now;
            if (remaining > std::chrono::milliseconds(2)) {
                const auto sleep_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    remaining - std::chrono::milliseconds(1)).count();
                HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
                if (timer) {
                    LARGE_INTEGER due{};
                    due.QuadPart = -static_cast<LONGLONG>(sleep_ns / 100);  // 100ns units, relative
                    if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
                        WaitForSingleObject(timer, 200);
                    }
                    CloseHandle(timer);
                } else {
                    std::this_thread::sleep_for(remaining - std::chrono::milliseconds(2));
                }
                std::lock_guard lock(mtx_);
                ++paced_sleeps_;
            }
            while (std::chrono::steady_clock::now() < deadline) {
                std::this_thread::yield();
            }
        }
    }

    const auto end_tp = std::chrono::steady_clock::now();
    const double dt_ms = std::chrono::duration<double, std::milli>(end_tp - start_tp).count();

    std::lock_guard lock(mtx_);
    ++frame_count_;
    last_frame_ms_ = dt_ms;
    present_latency_ms_ = present_latency_ms;
    if (min_frame_ms_ == 0.0 || dt_ms < min_frame_ms_) min_frame_ms_ = dt_ms;
    if (dt_ms > max_frame_ms_) max_frame_ms_ = dt_ms;
    if (history_.size() >= kHistoryCapacity) {
        history_.erase(history_.begin());
    }
    history_.push_back(dt_ms);
    frame_start_ = end_tp;
}

void FramePacer::record_present_kind(bool used_tearing) {
    std::lock_guard lock(mtx_);
    if (used_tearing) {
        ++tearing_presents_;
    } else {
        ++vsync_presents_;
    }
}

FramePacingStats FramePacer::stats() const {
    std::lock_guard lock(mtx_);
    FramePacingStats out{};
    out.frame_count = frame_count_;
    out.last_frame_ms = last_frame_ms_;
    out.min_frame_ms = min_frame_ms_;
    out.max_frame_ms = max_frame_ms_;
    out.present_latency_ms = present_latency_ms_;
    out.paced_sleeps = paced_sleeps_;
    out.tearing_presents = tearing_presents_;
    out.vsync_presents = vsync_presents_;
    out.recent_frame_ms.reserve(history_.size());
    for (double v : history_) {
        out.recent_frame_ms.push_back(static_cast<float>(v));
    }

    if (!history_.empty()) {
        double sum = 0.0;
        for (double v : history_) sum += v;
        out.avg_frame_ms = sum / static_cast<double>(history_.size());
        if (out.avg_frame_ms > 0.001) {
            out.current_fps = 1000.0 / out.avg_frame_ms;
        }
        std::vector<double> sorted = history_;
        std::sort(sorted.begin(), sorted.end());
        const size_t p99_idx = (sorted.size() * 99u) / 100u;
        out.p99_frame_ms = sorted[(std::min)(p99_idx, sorted.size() - 1u)];
    }
    return out;
}

// ---- Telemetry HUD Rasterizer & GPU Compositor ----

std::vector<uint8_t> render_telemetry_hud_rgba8(
    const FramePacingStats& stats,
    std::string_view gpu_label,
    PresentSyncMode sync_mode) {
    constexpr uint32_t W = kTelemetryHudWidth;
    constexpr uint32_t H = kTelemetryHudHeight;
    std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4u, 0);

    // Dark slate background + crisp green border + left accent bar
    fill_rect(px, W, H, 0, 0, W, H, 13, 17, 23, 255);
    fill_rect(px, W, H, 0, 0, W, 1, 46, 164, 79, 255);
    fill_rect(px, W, H, 0, H - 1, W, 1, 46, 164, 79, 255);
    fill_rect(px, W, H, 0, 0, 1, H, 46, 164, 79, 255);
    fill_rect(px, W, H, W - 1, 0, 1, H, 46, 164, 79, 255);
    fill_rect(px, W, H, 1, 1, 3, H - 2, 86, 211, 100, 255);

    const char* mode_tag = "VSYNC";
    if (sync_mode == PresentSyncMode::VariableRefreshTearing) mode_tag = "TEAR";
    else if (sync_mode == PresentSyncMode::Uncapped) mode_tag = "FAST";

    std::string short_gpu(gpu_label);
    if (short_gpu.size() > 17) short_gpu.resize(17);

    const std::string line1 = "KESTRELX PRESENTX HUD";
    const std::string line2 = std::format("FPS:{:5.1f}  FT:{:5.2f}MS", stats.current_fps, stats.avg_frame_ms);
    const std::string line3 = std::format("P99:{:5.2f}MS LAT:{:4.2f}MS", stats.p99_frame_ms, stats.present_latency_ms);
    const std::string line4 = std::format("{} [{}]", short_gpu, mode_tag);

    draw_text_5x7(px, W, H, 8, 5, line1, 86, 211, 100);
    draw_text_5x7(px, W, H, 8, 15, line2, 240, 246, 252);
    draw_text_5x7(px, W, H, 8, 25, line3, 180, 210, 255);
    draw_text_5x7(px, W, H, 8, 35, line4, 210, 153, 34);

    // Real-time frametime sparkline graph box (y = 46..70, x = 8..256)
    constexpr uint32_t gx = 8;
    constexpr uint32_t gy = 46;
    constexpr uint32_t gw = W - 16;
    constexpr uint32_t gh = 24;
    fill_rect(px, W, H, gx, gy, gw, gh, 22, 27, 34, 255);
    const uint32_t ref_y = gy + gh - (gh / 2);
    fill_rect(px, W, H, gx, ref_y, gw, 1, 48, 54, 61, 255);

    if (!stats.recent_frame_ms.empty()) {
        const size_t count = (std::min)(stats.recent_frame_ms.size(), static_cast<size_t>(gw / 2));
        const size_t start_idx = stats.recent_frame_ms.size() - count;
        for (size_t i = 0; i < count; ++i) {
            const float ft = stats.recent_frame_ms[start_idx + i];
            const float norm = (std::min)(ft / 33.33f, 1.0f);
            const uint32_t bar_h = (std::max)(2u, static_cast<uint32_t>(norm * static_cast<float>(gh - 2)));
            uint8_t br = 86, bg = 211, bb = 100;
            if (ft > 25.0f) {
                br = 248; bg = 81; bb = 73;
            } else if (ft > 17.5f) {
                br = 210; bg = 153; bb = 34;
            }
            const uint32_t bx = gx + static_cast<uint32_t>(i) * 2u;
            fill_rect(px, W, H, bx, gy + gh - bar_h, 2, bar_h, br, bg, bb, 255);
        }
    }
    return px;
}

Result<void> composite_hud_on_d3d12_texture(
    ID3D12Device* dev,
    ID3D12CommandQueue* queue,
    ID3D12Resource* rt,
    D3D12_RESOURCE_STATES current_state,
    const FramePacingStats& stats,
    std::string_view gpu_label,
    PresentSyncMode sync_mode,
    uint32_t origin_x,
    uint32_t origin_y) {
    if (!dev || !queue || !rt) {
        return fail("composite_hud_on_d3d12_texture invalid args");
    }

    const D3D12_RESOURCE_DESC rt_desc = rt->GetDesc();
    if (rt_desc.Width <= origin_x + 16 || rt_desc.Height <= origin_y + 16) {
        return {};
    }

    const UINT copy_w = (std::min)(kTelemetryHudWidth, static_cast<UINT>(rt_desc.Width - origin_x));
    const UINT copy_h = (std::min)(kTelemetryHudHeight, rt_desc.Height - origin_y);
    const auto hud_rgba = render_telemetry_hud_rgba8(stats, gpu_label, sync_mode);

    D3D12_RESOURCE_DESC hud_tex_desc{};
    hud_tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    hud_tex_desc.Width = kTelemetryHudWidth;
    hud_tex_desc.Height = kTelemetryHudHeight;
    hud_tex_desc.DepthOrArraySize = 1;
    hud_tex_desc.MipLevels = 1;
    hud_tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    hud_tex_desc.SampleDesc.Count = 1;

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT num_rows = 0;
    UINT64 row_bytes = 0;
    UINT64 total_bytes = 0;
    dev->GetCopyableFootprints(&hud_tex_desc, 0, 1, 0, &fp, &num_rows, &row_bytes, &total_bytes);

    D3D12_HEAP_PROPERTIES up_hp{};
    up_hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC up_desc{};
    up_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    up_desc.Width = total_bytes;
    up_desc.Height = 1;
    up_desc.DepthOrArraySize = 1;
    up_desc.MipLevels = 1;
    up_desc.SampleDesc.Count = 1;
    up_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ID3D12Resource* upload = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* cmd = nullptr;
    ID3D12Fence* fence = nullptr;

    auto cleanup = [&]() {
        if (fence) fence->Release();
        if (cmd) cmd->Release();
        if (alloc) alloc->Release();
        if (upload) upload->Release();
    };

    if (FAILED(dev->CreateCommittedResource(&up_hp, D3D12_HEAP_FLAG_NONE, &up_desc,
                                            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload))) ||
        FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
        FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&cmd))) ||
        FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
        cleanup();
        return fail("Failed to create D3D12 HUD composite objects");
    }

    void* mapped = nullptr;
    D3D12_RANGE empty{0, 0};
    if (FAILED(upload->Map(0, &empty, &mapped))) {
        cleanup();
        return fail("Failed to map HUD upload buffer");
    }
    auto* dst = static_cast<uint8_t*>(mapped);
    for (uint32_t y = 0; y < kTelemetryHudHeight; ++y) {
        std::memcpy(dst + static_cast<size_t>(y) * fp.Footprint.RowPitch,
                    hud_rgba.data() + static_cast<size_t>(y) * kTelemetryHudWidth * 4u,
                    kTelemetryHudWidth * 4u);
    }
    upload->Unmap(0, nullptr);

    if (current_state != D3D12_RESOURCE_STATE_COPY_DEST) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {rt, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, current_state, D3D12_RESOURCE_STATE_COPY_DEST};
        cmd->ResourceBarrier(1, &b);
    }

    D3D12_TEXTURE_COPY_LOCATION dst_loc{rt, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}};
    D3D12_TEXTURE_COPY_LOCATION src_loc{upload, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
    src_loc.PlacedFootprint = fp;
    D3D12_BOX box{0, 0, 0, copy_w, copy_h, 1};
    cmd->CopyTextureRegion(&dst_loc, origin_x, origin_y, 0, &src_loc, &box);

    if (current_state != D3D12_RESOURCE_STATE_COPY_DEST) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {rt, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST, current_state};
        cmd->ResourceBarrier(1, &b);
    }

    cmd->Close();
    ID3D12CommandList* lists[] = {cmd};
    queue->ExecuteCommandLists(1, lists);
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    queue->Signal(fence, 1);
    if (fence->GetCompletedValue() < 1) {
        fence->SetEventOnCompletion(1, ev);
        WaitForSingleObject(ev, 5000);
    }
    CloseHandle(ev);
    cleanup();
    return {};
}

// ---- WindowHost ----

WindowHost::~WindowHost() { shutdown(); }

LRESULT CALLBACK WindowHost::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<WindowHost*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    if (msg == WM_SYSKEYDOWN && wp == VK_RETURN && ((lp & (1ll << 29)) != 0)) {
        if (self) self->toggle_borderless_fullscreen();
        return 0;
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
                                uint32_t width, uint32_t height, const wchar_t* title,
                                const FramePacingConfig& pacing) {
    if (!dev || !queue || width == 0 || height == 0) {
        return fail("WindowHost::create invalid arguments");
    }
    shutdown();
    pacing_cfg_ = pacing;
    pacer_.configure(pacing);
    pacer_.reset();
    adapter_label_ = query_adapter_label(dev);

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
    GetWindowRect(hwnd_, &windowed_rect_);
    windowed_style_ = WS_OVERLAPPEDWINDOW;
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);

    IDXGIFactory4* factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        shutdown();
        return fail("CreateDXGIFactory1 failed");
    }

    BOOL allow_tearing = FALSE;
    IDXGIFactory5* factory5 = nullptr;
    if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory5))) && factory5) {
        (void)factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow_tearing, sizeof(allow_tearing));
        factory5->Release();
    }
    tearing_supported_ = (allow_tearing == TRUE);

    DXGI_SWAP_CHAIN_DESC1 sc_desc{};
    sc_desc.Width = width;
    sc_desc.Height = height;
    sc_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sc_desc.SampleDesc.Count = 1;
    sc_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc_desc.BufferCount = 2;
    sc_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sc_desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
                    (tearing_supported_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u);

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

    (void)swapchain_->SetMaximumFrameLatency((std::max)(1u, (std::min)(pacing.max_frame_latency, 3u)));
    frame_latency_waitable_ = swapchain_->GetFrameLatencyWaitableObject();

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
    if (pacing.borderless_fullscreen) {
        set_borderless_fullscreen(true);
    }
    return {};
}

void WindowHost::set_frame_pacing(const FramePacingConfig& pacing) {
    pacing_cfg_ = pacing;
    pacer_.configure(pacing);
    if (swapchain_) {
        (void)swapchain_->SetMaximumFrameLatency((std::max)(1u, (std::min)(pacing.max_frame_latency, 3u)));
    }
    if (pacing.borderless_fullscreen != borderless_fullscreen_) {
        set_borderless_fullscreen(pacing.borderless_fullscreen);
    }
}

void WindowHost::set_borderless_fullscreen(bool enabled) {
    if (!hwnd_ || enabled == borderless_fullscreen_) return;
    if (enabled) {
        GetWindowRect(hwnd_, &windowed_rect_);
        windowed_style_ = GetWindowLongW(hwnd_, GWL_STYLE);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongW(hwnd_, GWL_STYLE, windowed_style_ & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(hwnd_, HWND_TOP,
                     mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        borderless_fullscreen_ = true;
    } else {
        SetWindowLongW(hwnd_, GWL_STYLE, windowed_style_);
        SetWindowPos(hwnd_, nullptr,
                     windowed_rect_.left, windowed_rect_.top,
                     windowed_rect_.right - windowed_rect_.left,
                     windowed_rect_.bottom - windowed_rect_.top,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        borderless_fullscreen_ = false;
    }
}

void WindowHost::toggle_borderless_fullscreen() {
    set_borderless_fullscreen(!borderless_fullscreen_);
}

bool WindowHost::wait_frame_latency(uint32_t timeout_ms) {
    if (!frame_latency_waitable_) return false;
    return WaitForSingleObjectEx(frame_latency_waitable_, timeout_ms, TRUE) == WAIT_OBJECT_0;
}

Result<void> WindowHost::present_resource(ID3D12Resource* guest_rt, D3D12_RESOURCE_STATES guest_state) {
    if (!swapchain_ || !queue_ || !alloc_ || !cmd_) {
        return fail("WindowHost not initialized");
    }

    const auto present_start = std::chrono::steady_clock::now();
    ID3D12Resource* back = buffers_[swapchain_->GetCurrentBackBufferIndex()];

    if (guest_rt) {
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

    if (pacing_cfg_.hud_enabled && back) {
        (void)composite_hud_on_d3d12_texture(
            dev_, queue_, back, D3D12_RESOURCE_STATE_PRESENT,
            pacer_.stats(), adapter_label_, pacing_cfg_.sync_mode);
    }

    UINT sync_interval = 1;
    UINT present_flags = 0;
    bool used_tearing = false;
    if (pacing_cfg_.sync_mode == PresentSyncMode::VariableRefreshTearing) {
        sync_interval = 0;
        if (tearing_supported_ && !borderless_fullscreen_) {
            present_flags = DXGI_PRESENT_ALLOW_TEARING;
            used_tearing = true;
        }
    } else if (pacing_cfg_.sync_mode == PresentSyncMode::Uncapped) {
        sync_interval = 0;
    }

    HRESULT hr = swapchain_->Present(sync_interval, present_flags);
    if (FAILED(hr)) {
        return fail(std::format("IDXGISwapChain3::Present failed ({:#x})", static_cast<uint32_t>(hr)));
    }
    pacer_.record_present_kind(used_tearing);

    ++fence_val_;
    queue_->Signal(fence_, fence_val_);
    if (fence_->GetCompletedValue() < fence_val_) {
        fence_->SetEventOnCompletion(fence_val_, fence_event_);
        WaitForSingleObject(fence_event_, 5000);
    }

    const auto present_end = std::chrono::steady_clock::now();
    const double lat_ms = std::chrono::duration<double, std::milli>(present_end - present_start).count();
    pacer_.end_frame_and_pace(lat_ms);

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
    if (frame_latency_waitable_) { CloseHandle(frame_latency_waitable_); frame_latency_waitable_ = nullptr; }
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
    borderless_fullscreen_ = false;
    tearing_supported_ = false;
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
