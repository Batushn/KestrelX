#include "kestrel/hle/d3d12x.hpp"
#include "kestrel/hle/window_host.hpp"
#include "kestrel/gpu/dxc_compiler.hpp"
#include "kestrel/gpu/ir.hpp"
#include "kestrel/gpu/rdna2_isa.hpp"
#include "kestrel/gpu/rdna2_recomp.hpp"
#include "kestrel/gpu/shader_cache.hpp"

#include <dxgi1_6.h>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace kestrel::hle {

namespace {

// Private internal IID used to safely unwrap KxGraphicsCommandListProxy inside ExecuteCommandLists
// {9E1B5E73-8F24-4B9E-9A5E-4C12D30F1201}
constexpr GUID kIID_KxGraphicsCommandListProxy = {
    0x9e1b5e73, 0x8f24, 0x4b9e, {0x9a, 0x5e, 0x4c, 0x12, 0xd3, 0x0f, 0x12, 0x01}};

std::atomic<int> g_preferred_adapter{-1};
std::atomic<uint64_t> g_devices{0}, g_queues{0}, g_cmd_lists{0}, g_resources{0},
    g_zero_mem{0}, g_presents{0}, g_pso_recomp{0}, g_pso_cache_hits{0},
    g_gfx_pso_recomp{0}, g_gfx_pso_cache_hits{0}, g_stream_pso_recomp{0},
    g_disk_cache_hits{0}, g_disk_cache_misses{0}, g_device_qi_upgrades{0},
    g_draw_calls{0}, g_dispatch_calls{0}, g_barrier_calls{0}, g_pso_binds{0},
    g_compute_queues{0}, g_inter_queue_fences{0},
    g_vrs_rate_sets{0}, g_vrs_image_binds{0},
    g_work_graphs_created{0}, g_work_graph_dispatches{0},
    g_dispatch_mesh_calls{0}, g_enhanced_barrier_calls{0}, g_enhanced_barrier_groups{0},
    g_reserved_resources{0}, g_tile_mapping_updates{0}, g_tiles_mapped{0}, g_tiles_unmapped{0}, g_copy_tiles_calls{0};

std::mutex g_present_mtx;
bool g_windowed = false;
uint32_t g_win_w = 640;
uint32_t g_win_h = 640;
std::filesystem::path g_capture_path;
std::unique_ptr<WindowHost> g_window_host;
FramePacingConfig g_pacing_cfg{};
FramePacer g_headless_pacer;
FramePacingStats g_last_window_pacing{};
bool g_has_last_window_pacing = false;

std::mutex g_shader_cache_mtx;
std::unordered_map<uint64_t, std::vector<uint8_t>> g_dxil_cache;
gpu::DiskShaderCache g_disk_cache;
gpu::DxcCompiler g_dxc;
bool g_dxc_ready = false;

template <typename T>
constexpr size_t subobj_aligned_size() {
    constexpr size_t header = sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE);
    constexpr size_t payload_off = (header + alignof(T) - 1) & ~(alignof(T) - 1);
    constexpr size_t end_off = payload_off + sizeof(T);
    return (end_off + sizeof(void*) - 1) & ~(sizeof(void*) - 1);
}

size_t subobject_stride(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type) {
    switch (type) {
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: return subobj_aligned_size<ID3D12RootSignature*>();
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: return subobj_aligned_size<D3D12_SHADER_BYTECODE>();
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: return subobj_aligned_size<D3D12_STREAM_OUTPUT_DESC>();
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND: return subobj_aligned_size<D3D12_BLEND_DESC>();
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK: return subobj_aligned_size<UINT>();
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER: return subobj_aligned_size<D3D12_RASTERIZER_DESC>();
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: return subobj_aligned_size<D3D12_DEPTH_STENCIL_DESC>();
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: return subobj_aligned_size<D3D12_INPUT_LAYOUT_DESC>();
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE: return subobj_aligned_size<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>();
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY: return subobj_aligned_size<D3D12_PRIMITIVE_TOPOLOGY_TYPE>();
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: return subobj_aligned_size<D3D12_RT_FORMAT_ARRAY>();
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT: return subobj_aligned_size<DXGI_FORMAT>();
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC: return subobj_aligned_size<DXGI_SAMPLE_DESC>();
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK: return subobj_aligned_size<UINT>();
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO: return subobj_aligned_size<D3D12_CACHED_PIPELINE_STATE>();
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS: return subobj_aligned_size<D3D12_PIPELINE_STATE_FLAGS>();
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: return subobj_aligned_size<D3D12_DEPTH_STENCIL_DESC1>();
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: return subobj_aligned_size<D3D12_VIEW_INSTANCING_DESC>();
        default: return 0;
    }
}

// Clean-room native RDNA2 container headers:
//   "KXRN" + uint32 group_size_x + GFX10.3 dwords -> Compute       (cs_6_6)
//   "KXRV" + uint32 reserved     + GFX10.3 dwords -> Vertex        (vs_6_6)
//   "KXRP" + uint32 reserved     + GFX10.3 dwords -> Pixel         (ps_6_6)
//   "KXRM" + uint32 group_size_x + GFX10.3 dwords -> Mesh          (ms_6_6)
//   "KXRA" + uint32 group_size_x + GFX10.3 dwords -> Amplification (as_6_6)
bool try_recompile_kestrel_rdna2(
    const void* bytecode, SIZE_T length, gpu::ShaderStage expected_stage,
    bool prefer_wave64, std::vector<uint8_t>& out_dxil, bool& out_cache_hit,
    bool mesh_has_payload = false) {
    out_cache_hit = false;
    if (!bytecode || length < 12) return false;
    const auto* bytes = static_cast<const uint8_t*>(bytecode);

    const char* expected_magic = nullptr;
    switch (expected_stage) {
        case gpu::ShaderStage::Compute:       expected_magic = "KXRN"; break;
        case gpu::ShaderStage::Vertex:        expected_magic = "KXRV"; break;
        case gpu::ShaderStage::Pixel:         expected_magic = "KXRP"; break;
        case gpu::ShaderStage::Mesh:          expected_magic = "KXRM"; break;
        case gpu::ShaderStage::Amplification: expected_magic = "KXRA"; break;
    }
    if (!expected_magic || std::memcmp(bytes, expected_magic, 4) != 0) return false;

    uint32_t group_x = 64;
    if (expected_stage == gpu::ShaderStage::Compute) {
        std::memcpy(&group_x, bytes + 4, 4);
        if (group_x == 0 || group_x > 1024) group_x = 64;
    } else if (expected_stage == gpu::ShaderStage::Mesh ||
               expected_stage == gpu::ShaderStage::Amplification) {
        std::memcpy(&group_x, bytes + 4, 4);
        if (group_x == 0 || group_x > 128) group_x = 32;
    }

    const size_t code_bytes = length - 8;
    if (code_bytes == 0 || (code_bytes % 4) != 0) return false;

    const bool effective_wave64 = prefer_wave64 &&
        !((expected_stage == gpu::ShaderStage::Mesh || expected_stage == gpu::ShaderStage::Amplification) && group_x < 64);
    const gpu::WaveMode wave = effective_wave64 ? gpu::WaveMode::Wave64 : gpu::WaveMode::Wave32;
    const uint32_t cache_group_tag = group_x | (mesh_has_payload ? 0x80000000u : 0u);
    const uint64_t cache_key = gpu::DiskShaderCache::make_key(expected_stage, wave, cache_group_tag, bytes, length);

    {
        std::lock_guard lock(g_shader_cache_mtx);
        // 1. Check in-memory DXIL cache
        auto it = g_dxil_cache.find(cache_key);
        if (it != g_dxil_cache.end()) {
            out_dxil = it->second;
            out_cache_hit = true;
            if (expected_stage == gpu::ShaderStage::Compute) g_pso_cache_hits.fetch_add(1, std::memory_order_relaxed);
            else g_gfx_pso_cache_hits.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        // 2. Check persistent on-disk DXIL shader cache
        std::vector<uint8_t> disk_dxil;
        if (g_disk_cache.load(cache_key, disk_dxil)) {
            g_disk_cache_hits.fetch_add(1, std::memory_order_relaxed);
            g_dxil_cache[cache_key] = disk_dxil;
            out_dxil = std::move(disk_dxil);
            out_cache_hit = true;
            if (expected_stage == gpu::ShaderStage::Compute) g_pso_cache_hits.fetch_add(1, std::memory_order_relaxed);
            else g_gfx_pso_cache_hits.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        g_disk_cache_misses.fetch_add(1, std::memory_order_relaxed);
        if (!g_dxc_ready) {
            if (!g_dxc.initialize()) return false;
            g_dxc_ready = true;
        }
    }

    std::vector<uint32_t> words(code_bytes / 4);
    std::memcpy(words.data(), bytes + 8, code_bytes);

    auto decoded = gpu::decode_rdna2(words.data(), words.size());
    auto ir = gpu::lift_rdna2_to_ir(decoded);
    if (ir.unknown_count > 0) return false;

    auto recomp = gpu::emit_hlsl_from_ir(ir, {
        .stage = expected_stage,
        .wave_mode = wave,
        .group_size_x = group_x,
        .mesh_has_payload = mesh_has_payload,
    });
    const char* profile = "cs_6_6";
    switch (expected_stage) {
        case gpu::ShaderStage::Compute:       profile = "cs_6_6"; break;
        case gpu::ShaderStage::Vertex:        profile = "vs_6_6"; break;
        case gpu::ShaderStage::Pixel:         profile = "ps_6_6"; break;
        case gpu::ShaderStage::Mesh:          profile = "ms_6_6"; break;
        case gpu::ShaderStage::Amplification: profile = "as_6_6"; break;
    }
    auto compiled = g_dxc.compile_hlsl(recomp.hlsl, "main", profile);
    if (!compiled) return false;

    {
        std::lock_guard lock(g_shader_cache_mtx);
        (void)g_disk_cache.store(cache_key, compiled->dxil);
        g_dxil_cache[cache_key] = compiled->dxil;
    }
    out_dxil = std::move(compiled->dxil);
    if (expected_stage == gpu::ShaderStage::Compute) g_pso_recomp.fetch_add(1, std::memory_order_relaxed);
    else g_gfx_pso_recomp.fetch_add(1, std::memory_order_relaxed);
    return true;
}

HRESULT pick_hardware_adapter(IDXGIAdapter1** out_adapter) {
    *out_adapter = nullptr;
    IDXGIFactory6* factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return hr;

    const int pref = g_preferred_adapter.load(std::memory_order_relaxed);
    if (pref >= 0) {
        IDXGIAdapter1* ad = nullptr;
        if (SUCCEEDED(factory->EnumAdapters1(static_cast<UINT>(pref), &ad))) {
            DXGI_ADAPTER_DESC1 desc{};
            ad->GetDesc1(&desc);
            if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                *out_adapter = ad;
                factory->Release();
                return S_OK;
            }
            ad->Release();
        }
    }

    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* ad = nullptr;
        if (FAILED(factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&ad)))) {
            break;
        }
        DXGI_ADAPTER_DESC1 desc{};
        ad->GetDesc1(&desc);
        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            if (SUCCEEDED(D3D12CreateDevice(ad, D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device), nullptr))) {
                *out_adapter = ad;
                factory->Release();
                return S_OK;
            }
        }
        ad->Release();
    }
    factory->Release();
    return DXGI_ERROR_NOT_FOUND;
}

HRESULT __stdcall kx_D3D12CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL min_level, REFIID riid, void** out_dev) {
    if (!out_dev) return D3D12CreateDevice(adapter, min_level, riid, nullptr);
    *out_dev = nullptr;

    IDXGIAdapter1* chosen = nullptr;
    IUnknown* effective = adapter;
    if (!effective && SUCCEEDED(pick_hardware_adapter(&chosen))) effective = chosen;

    ID3D12Device* raw = nullptr;
    HRESULT hr = D3D12CreateDevice(effective, min_level, IID_PPV_ARGS(&raw));
    if (chosen) chosen->Release();
    if (FAILED(hr)) return hr;

    auto* proxy = new KxDeviceProxy(raw);
    raw->Release();
    g_devices.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard lock(g_present_mtx);
        if (g_headless_pacer.stats().frame_count == 0) {
            g_headless_pacer.begin_frame();
        }
    }

    hr = proxy->QueryInterface(riid, out_dev);
    proxy->Release();
    return hr;
}

struct D3D12XCreateDeviceParams { uint32_t version; uint32_t flags; };

HRESULT __stdcall kx_D3D12XboxCreateDevice(const D3D12XCreateDeviceParams*, REFIID riid, void** out_dev) {
    return kx_D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, riid, out_dev);
}

void __stdcall kx_D3D12ZeroMemoryX(void* dst, size_t bytes) {
    if (dst && bytes > 0) std::memset(dst, 0, bytes);
    g_zero_mem.fetch_add(1, std::memory_order_relaxed);
}

HRESULT __stdcall kx_D3D12SerializeRootSignature(
    const D3D12_ROOT_SIGNATURE_DESC* desc, D3D_ROOT_SIGNATURE_VERSION ver,
    ID3DBlob** blob, ID3DBlob** err) {
    return D3D12SerializeRootSignature(desc, ver, blob, err);
}

HRESULT __stdcall kx_D3D12PresentX(ID3D12CommandQueue* queue, uint32_t, const void* rt_or_planes, const void*) {
    g_presents.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard lock(g_present_mtx);
    bool handled_window_pacing = false;
    if (queue && rt_or_planes) {
        auto* rt = reinterpret_cast<ID3D12Resource*>(const_cast<void*>(rt_or_planes));
        ID3D12Device* dev = nullptr;
        if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&dev))) && dev) {
            if (!g_capture_path.empty()) {
                if (g_pacing_cfg.hud_enabled) {
                    if (!g_window_host) {
                        g_headless_pacer.end_frame_and_pace(0.42);
                        g_headless_pacer.record_present_kind(false);
                        handled_window_pacing = true;
                    }
                    const FramePacingStats st = g_window_host ? g_window_host->pacing_stats() : g_headless_pacer.stats();
                    (void)composite_hud_on_d3d12_texture(
                        dev, queue, rt, D3D12_RESOURCE_STATE_RENDER_TARGET,
                        st, "KestrelX D3D12.X", g_pacing_cfg.sync_mode);
                }
                (void)capture_d3d12_texture_bmp(dev, queue, rt, D3D12_RESOURCE_STATE_RENDER_TARGET, g_capture_path);
            }
            if (g_windowed) {
                if (!g_window_host) {
                    g_window_host = std::make_unique<WindowHost>();
                    if (!g_window_host->create(dev, queue, g_win_w, g_win_h,
                                               L"KestrelX - Xbox Series X|S Runtime", g_pacing_cfg)) {
                        g_window_host.reset();
                    }
                }
                if (g_window_host && g_window_host->is_open()) {
                    g_window_host->set_frame_pacing(g_pacing_cfg);
                    (void)g_window_host->present_resource(rt, D3D12_RESOURCE_STATE_RENDER_TARGET);
                    g_window_host->pump_messages();
                    handled_window_pacing = true;
                }
            }
            dev->Release();
        }
    }
    if (!handled_window_pacing) {
        g_headless_pacer.end_frame_and_pace(0.0);
        g_headless_pacer.record_present_kind(false);
    }
    return S_OK;
}

HRESULT __stdcall kx_D3D12WaitFrameEventX(uint64_t, uint32_t timeout_ms, void*) {
    std::lock_guard lock(g_present_mtx);
    if (g_window_host && g_window_host->is_open()) {
        (void)g_window_host->wait_frame_latency(timeout_ms > 0 ? timeout_ms : 100u);
    }
    return S_OK;
}

}  // namespace

// ---- KxGraphicsCommandListProxy ----

KxGraphicsCommandListProxy::KxGraphicsCommandListProxy(ID3D12GraphicsCommandList* inner) : inner_(inner) {
    if (inner_) {
        inner_->AddRef();
        (void)inner_->QueryInterface(IID_PPV_ARGS(&inner1_));
        (void)inner_->QueryInterface(IID_PPV_ARGS(&inner2_));
        (void)inner_->QueryInterface(IID_PPV_ARGS(&inner3_));
        (void)inner_->QueryInterface(IID_PPV_ARGS(&inner4_));
        (void)inner_->QueryInterface(IID_PPV_ARGS(&inner5_));
        (void)inner_->QueryInterface(IID_PPV_ARGS(&inner6_));
        (void)inner_->QueryInterface(IID_PPV_ARGS(&inner7_));
    }
}
KxGraphicsCommandListProxy::~KxGraphicsCommandListProxy() {
    if (inner7_) { inner7_->Release(); inner7_ = nullptr; }
    if (inner6_) { inner6_->Release(); inner6_ = nullptr; }
    if (inner5_) { inner5_->Release(); inner5_ = nullptr; }
    if (inner4_) { inner4_->Release(); inner4_ = nullptr; }
    if (inner3_) { inner3_->Release(); inner3_ = nullptr; }
    if (inner2_) { inner2_->Release(); inner2_ = nullptr; }
    if (inner1_) { inner1_->Release(); inner1_ = nullptr; }
    if (inner_) { inner_->Release(); inner_ = nullptr; }
}

HRESULT STDMETHODCALLTYPE KxGraphicsCommandListProxy::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == kIID_KxGraphicsCommandListProxy) {
        *ppv = this;
        AddRef();
        return S_OK;
    }
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) ||
        riid == __uuidof(ID3D12DeviceChild) || riid == __uuidof(ID3D12CommandList) ||
        riid == __uuidof(ID3D12GraphicsCommandList)) {
        *ppv = static_cast<ID3D12GraphicsCommandList*>(this);
        AddRef();
        return S_OK;
    }
    if (riid == __uuidof(ID3D12GraphicsCommandList1) && inner1_) {
        *ppv = static_cast<ID3D12GraphicsCommandList1*>(this);
        AddRef();
        return S_OK;
    }
    if (riid == __uuidof(ID3D12GraphicsCommandList2) && inner2_) {
        *ppv = static_cast<ID3D12GraphicsCommandList2*>(this);
        AddRef();
        return S_OK;
    }
    if (riid == __uuidof(ID3D12GraphicsCommandList3) && inner3_) {
        *ppv = static_cast<ID3D12GraphicsCommandList3*>(this);
        AddRef();
        return S_OK;
    }
    if (riid == __uuidof(ID3D12GraphicsCommandList4) && inner4_) {
        *ppv = static_cast<ID3D12GraphicsCommandList4*>(this);
        AddRef();
        return S_OK;
    }
    if (riid == __uuidof(ID3D12GraphicsCommandList5) && inner5_) {
        *ppv = static_cast<ID3D12GraphicsCommandList5*>(this);
        AddRef();
        return S_OK;
    }
    if (riid == __uuidof(ID3D12GraphicsCommandList6) && inner6_) {
        *ppv = static_cast<ID3D12GraphicsCommandList6*>(this);
        AddRef();
        return S_OK;
    }
    if (riid == __uuidof(ID3D12GraphicsCommandList7) && inner7_) {
        *ppv = static_cast<ID3D12GraphicsCommandList7*>(this);
        AddRef();
        return S_OK;
    }
    return inner_->QueryInterface(riid, ppv);
}

ULONG STDMETHODCALLTYPE KxGraphicsCommandListProxy::AddRef() { return ref_.fetch_add(1, std::memory_order_relaxed) + 1; }
ULONG STDMETHODCALLTYPE KxGraphicsCommandListProxy::Release() {
    const ULONG next = ref_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (next == 0) delete this;
    return next;
}

HRESULT STDMETHODCALLTYPE KxGraphicsCommandListProxy::GetPrivateData(REFGUID g, UINT* s, void* d) { return inner_->GetPrivateData(g, s, d); }
HRESULT STDMETHODCALLTYPE KxGraphicsCommandListProxy::SetPrivateData(REFGUID g, UINT s, const void* d) { return inner_->SetPrivateData(g, s, d); }
HRESULT STDMETHODCALLTYPE KxGraphicsCommandListProxy::SetPrivateDataInterface(REFGUID g, const IUnknown* d) { return inner_->SetPrivateDataInterface(g, d); }
HRESULT STDMETHODCALLTYPE KxGraphicsCommandListProxy::SetName(LPCWSTR n) { return inner_->SetName(n); }
HRESULT STDMETHODCALLTYPE KxGraphicsCommandListProxy::GetDevice(REFIID r, void** o) { return inner_->GetDevice(r, o); }
D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE KxGraphicsCommandListProxy::GetType() { return inner_->GetType(); }

HRESULT STDMETHODCALLTYPE KxGraphicsCommandListProxy::Close() { return inner_->Close(); }
HRESULT STDMETHODCALLTYPE KxGraphicsCommandListProxy::Reset(ID3D12CommandAllocator* a, ID3D12PipelineState* p) { return inner_->Reset(a, p); }
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::ClearState(ID3D12PipelineState* p) { inner_->ClearState(p); }
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::DrawInstanced(UINT vc, UINT ic, UINT sv, UINT si) {
    g_draw_calls.fetch_add(1, std::memory_order_relaxed);
    inner_->DrawInstanced(vc, ic, sv, si);
}
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::DrawIndexedInstanced(UINT ixc, UINT ic, UINT si, INT bv, UINT st) {
    g_draw_calls.fetch_add(1, std::memory_order_relaxed);
    inner_->DrawIndexedInstanced(ixc, ic, si, bv, st);
}
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::Dispatch(UINT x, UINT y, UINT z) {
    g_dispatch_calls.fetch_add(1, std::memory_order_relaxed);
    inner_->Dispatch(x, y, z);
}
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::CopyBufferRegion(ID3D12Resource* d, UINT64 dof, ID3D12Resource* s, UINT64 sof, UINT64 nb) { inner_->CopyBufferRegion(d, dof, s, sof, nb); }
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION* d, UINT dx, UINT dy, UINT dz, const D3D12_TEXTURE_COPY_LOCATION* s, const D3D12_BOX* sb) { inner_->CopyTextureRegion(d, dx, dy, dz, s, sb); }
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::CopyResource(ID3D12Resource* d, ID3D12Resource* s) { inner_->CopyResource(d, s); }
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::CopyTiles(ID3D12Resource* tr, const D3D12_TILED_RESOURCE_COORDINATE* c, const D3D12_TILE_REGION_SIZE* sz, ID3D12Resource* b, UINT64 off, D3D12_TILE_COPY_FLAGS f) {
    g_copy_tiles_calls.fetch_add(1, std::memory_order_relaxed);
    inner_->CopyTiles(tr, c, sz, b, off, f);
}
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::ResolveSubresource(ID3D12Resource* d, UINT ds, ID3D12Resource* s, UINT ss, DXGI_FORMAT fmt) { inner_->ResolveSubresource(d, ds, s, ss, fmt); }
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY t) { inner_->IASetPrimitiveTopology(t); }
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::RSSetViewports(UINT n, const D3D12_VIEWPORT* v) { inner_->RSSetViewports(n, v); }
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::RSSetScissorRects(UINT n, const D3D12_RECT* r) { inner_->RSSetScissorRects(n, r); }
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::OMSetBlendFactor(const FLOAT f[4]) { inner_->OMSetBlendFactor(f); }
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::OMSetStencilRef(UINT r) { inner_->OMSetStencilRef(r); }
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::SetPipelineState(ID3D12PipelineState* p) {
    g_pso_binds.fetch_add(1, std::memory_order_relaxed);
    inner_->SetPipelineState(p);
}
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::ResourceBarrier(UINT n, const D3D12_RESOURCE_BARRIER* b) {
    g_barrier_calls.fetch_add(n, std::memory_order_relaxed);
    inner_->ResourceBarrier(n, b);
}
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::ExecuteBundle(ID3D12GraphicsCommandList* cl) {
    if (cl) {
        KxGraphicsCommandListProxy* p = nullptr;
        if (SUCCEEDED(cl->QueryInterface(kIID_KxGraphicsCommandListProxy, reinterpret_cast<void**>(&p))) && p) {
            inner_->ExecuteBundle(p->inner());
            p->Release();
            return;
        }
    }
    inner_->ExecuteBundle(cl);
}
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::SetDescriptorHeaps(UINT n, ID3D12DescriptorHeap* const* h) { inner_->SetDescriptorHeaps(n, h); }
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::SetComputeRootSignature(ID3D12RootSignature* rs) { inner_->SetComputeRootSignature(rs); }
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::SetGraphicsRootSignature(ID3D12RootSignature* rs) { inner_->SetGraphicsRootSignature(rs); }
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::SetComputeRootDescriptorTable(UINT i, D3D12_GPU_DESCRIPTOR_HANDLE h) { inner_->SetComputeRootDescriptorTable(i, h); }
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::SetGraphicsRootDescriptorTable(UINT i, D3D12_GPU_DESCRIPTOR_HANDLE h) { inner_->SetGraphicsRootDescriptorTable(i, h); }
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::SetComputeRoot32BitConstant(UINT i, UINT d, UINT off) { inner_->SetComputeRoot32BitConstant(i, d, off); }
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::SetGraphicsRoot32BitConstant(UINT i, UINT d, UINT off) { inner_->SetGraphicsRoot32BitConstant(i, d, off); }
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::SetComputeRoot32BitConstants(UINT i, UINT n, const void* d, UINT off) { inner_->SetComputeRoot32BitConstants(i, n, d, off); }
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::SetGraphicsRoot32BitConstants(UINT i, UINT n, const void* d, UINT off) { inner_->SetGraphicsRoot32BitConstants(i, n, d, off); }
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::SetComputeRootConstantBufferView(UINT i, D3D12_GPU_VIRTUAL_ADDRESS a) { inner_->SetComputeRootConstantBufferView(i, a); }
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::SetGraphicsRootConstantBufferView(UINT i, D3D12_GPU_VIRTUAL_ADDRESS a) { inner_->SetGraphicsRootConstantBufferView(i, a); }
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::SetComputeRootShaderResourceView(UINT i, D3D12_GPU_VIRTUAL_ADDRESS a) { inner_->SetComputeRootShaderResourceView(i, a); }
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::SetGraphicsRootShaderResourceView(UINT i, D3D12_GPU_VIRTUAL_ADDRESS a) { inner_->SetGraphicsRootShaderResourceView(i, a); }
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::SetComputeRootUnorderedAccessView(UINT i, D3D12_GPU_VIRTUAL_ADDRESS a) { inner_->SetComputeRootUnorderedAccessView(i, a); }
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::SetGraphicsRootUnorderedAccessView(UINT i, D3D12_GPU_VIRTUAL_ADDRESS a) { inner_->SetGraphicsRootUnorderedAccessView(i, a); }
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW* v) { inner_->IASetIndexBuffer(v); }
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::IASetVertexBuffers(UINT s, UINT n, const D3D12_VERTEX_BUFFER_VIEW* v) { inner_->IASetVertexBuffers(s, n, v); }
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::SOSetTargets(UINT s, UINT n, const D3D12_STREAM_OUTPUT_BUFFER_VIEW* v) { inner_->SOSetTargets(s, n, v); }
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::OMSetRenderTargets(UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE* r, BOOL sr, const D3D12_CPU_DESCRIPTOR_HANDLE* d) { inner_->OMSetRenderTargets(n, r, sr, d); }
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE h, D3D12_CLEAR_FLAGS f, FLOAT d, UINT8 s, UINT nr, const D3D12_RECT* rc) { inner_->ClearDepthStencilView(h, f, d, s, nr, rc); }
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE h, const FLOAT c[4], UINT nr, const D3D12_RECT* rc) { inner_->ClearRenderTargetView(h, c, nr, rc); }
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::ClearUnorderedAccessViewUint(D3D12_GPU_DESCRIPTOR_HANDLE gh, D3D12_CPU_DESCRIPTOR_HANDLE ch, ID3D12Resource* r, const UINT v[4], UINT nr, const D3D12_RECT* rc) { inner_->ClearUnorderedAccessViewUint(gh, ch, r, v, nr, rc); }
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::ClearUnorderedAccessViewFloat(D3D12_GPU_DESCRIPTOR_HANDLE gh, D3D12_CPU_DESCRIPTOR_HANDLE ch, ID3D12Resource* r, const FLOAT v[4], UINT nr, const D3D12_RECT* rc) { inner_->ClearUnorderedAccessViewFloat(gh, ch, r, v, nr, rc); }
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::DiscardResource(ID3D12Resource* r, const D3D12_DISCARD_REGION* rg) { inner_->DiscardResource(r, rg); }
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::BeginQuery(ID3D12QueryHeap* qh, D3D12_QUERY_TYPE t, UINT i) { inner_->BeginQuery(qh, t, i); }
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::EndQuery(ID3D12QueryHeap* qh, D3D12_QUERY_TYPE t, UINT i) { inner_->EndQuery(qh, t, i); }
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::ResolveQueryData(ID3D12QueryHeap* qh, D3D12_QUERY_TYPE t, UINT si, UINT n, ID3D12Resource* dst, UINT64 off) { inner_->ResolveQueryData(qh, t, si, n, dst, off); }
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::SetPredication(ID3D12Resource* b, UINT64 off, D3D12_PREDICATION_OP op) { inner_->SetPredication(b, off, op); }
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::SetMarker(UINT m, const void* d, UINT s) { inner_->SetMarker(m, d, s); }
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::BeginEvent(UINT m, const void* d, UINT s) { inner_->BeginEvent(m, d, s); }
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::EndEvent() { inner_->EndEvent(); }
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::ExecuteIndirect(ID3D12CommandSignature* cs, UINT mc, ID3D12Resource* ab, UINT64 ao, ID3D12Resource* cb, UINT64 co) { inner_->ExecuteIndirect(cs, mc, ab, ao, cb, co); }

// ID3D12GraphicsCommandList1
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::AtomicCopyBufferUINT(ID3D12Resource* db, UINT64 dof, ID3D12Resource* sb, UINT64 sof, UINT deps, ID3D12Resource* const* dres, const D3D12_SUBRESOURCE_RANGE_UINT64* drng) {
    if (inner1_) inner1_->AtomicCopyBufferUINT(db, dof, sb, sof, deps, dres, drng);
}
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::AtomicCopyBufferUINT64(ID3D12Resource* db, UINT64 dof, ID3D12Resource* sb, UINT64 sof, UINT deps, ID3D12Resource* const* dres, const D3D12_SUBRESOURCE_RANGE_UINT64* drng) {
    if (inner1_) inner1_->AtomicCopyBufferUINT64(db, dof, sb, sof, deps, dres, drng);
}
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::OMSetDepthBounds(FLOAT mn, FLOAT mx) {
    if (inner1_) inner1_->OMSetDepthBounds(mn, mx);
}
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::SetSamplePositions(UINT nsp, UINT np, D3D12_SAMPLE_POSITION* sp) {
    if (inner1_) inner1_->SetSamplePositions(nsp, np, sp);
}
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::ResolveSubresourceRegion(ID3D12Resource* dr, UINT ds, UINT dx, UINT dy, ID3D12Resource* sr, UINT ss, D3D12_RECT* rect, DXGI_FORMAT fmt, D3D12_RESOLVE_MODE mode) {
    if (inner1_) inner1_->ResolveSubresourceRegion(dr, ds, dx, dy, sr, ss, rect, fmt, mode);
}
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::SetViewInstanceMask(UINT mask) {
    if (inner1_) inner1_->SetViewInstanceMask(mask);
}

// ID3D12GraphicsCommandList2
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::WriteBufferImmediate(UINT count, const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER* params, const D3D12_WRITEBUFFERIMMEDIATE_MODE* modes) {
    if (inner2_) inner2_->WriteBufferImmediate(count, params, modes);
}

// ID3D12GraphicsCommandList3
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::SetProtectedResourceSession(ID3D12ProtectedResourceSession* sess) {
    if (inner3_) inner3_->SetProtectedResourceSession(sess);
}

// ID3D12GraphicsCommandList4
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::BeginRenderPass(UINT nrt, const D3D12_RENDER_PASS_RENDER_TARGET_DESC* rts, const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* ds, D3D12_RENDER_PASS_FLAGS fl) {
    if (inner4_) inner4_->BeginRenderPass(nrt, rts, ds, fl);
}
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::EndRenderPass() {
    if (inner4_) inner4_->EndRenderPass();
}
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::InitializeMetaCommand(ID3D12MetaCommand* mc, const void* params, SIZE_T sz) {
    if (inner4_) inner4_->InitializeMetaCommand(mc, params, sz);
}
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::ExecuteMetaCommand(ID3D12MetaCommand* mc, const void* params, SIZE_T sz) {
    if (inner4_) inner4_->ExecuteMetaCommand(mc, params, sz);
}
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::BuildRaytracingAccelerationStructure(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* d, UINT npb, const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* pb) {
    if (inner4_) inner4_->BuildRaytracingAccelerationStructure(d, npb, pb);
}
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::EmitRaytracingAccelerationStructurePostbuildInfo(const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* d, UINT nsrc, const D3D12_GPU_VIRTUAL_ADDRESS* src) {
    if (inner4_) inner4_->EmitRaytracingAccelerationStructurePostbuildInfo(d, nsrc, src);
}
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::CopyRaytracingAccelerationStructure(D3D12_GPU_VIRTUAL_ADDRESS dst, D3D12_GPU_VIRTUAL_ADDRESS src, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE mode) {
    if (inner4_) inner4_->CopyRaytracingAccelerationStructure(dst, src, mode);
}
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::SetPipelineState1(ID3D12StateObject* so) {
    if (inner4_) inner4_->SetPipelineState1(so);
}
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::DispatchRays(const D3D12_DISPATCH_RAYS_DESC* d) {
    if (inner4_) inner4_->DispatchRays(d);
}

// ID3D12GraphicsCommandList5 (VRS Tier 1/2)
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::RSSetShadingRate(D3D12_SHADING_RATE baseShadingRate, const D3D12_SHADING_RATE_COMBINER* combiners) {
    g_vrs_rate_sets.fetch_add(1, std::memory_order_relaxed);
    if (inner5_) inner5_->RSSetShadingRate(baseShadingRate, combiners);
}
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::RSSetShadingRateImage(ID3D12Resource* shadingRateImage) {
    g_vrs_image_binds.fetch_add(1, std::memory_order_relaxed);
    if (inner5_) inner5_->RSSetShadingRateImage(shadingRateImage);
}

// ID3D12GraphicsCommandList6
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::DispatchMesh(UINT ThreadGroupCountX, UINT ThreadGroupCountY, UINT ThreadGroupCountZ) {
    g_dispatch_mesh_calls.fetch_add(1, std::memory_order_relaxed);
    if (inner6_) inner6_->DispatchMesh(ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
}

// ID3D12GraphicsCommandList7 (Enhanced Barriers)
void STDMETHODCALLTYPE KxGraphicsCommandListProxy::Barrier(UINT32 NumBarrierGroups, const D3D12_BARRIER_GROUP* pBarrierGroups) {
    g_enhanced_barrier_calls.fetch_add(1, std::memory_order_relaxed);
    g_enhanced_barrier_groups.fetch_add(NumBarrierGroups, std::memory_order_relaxed);
    if (inner7_) {
        inner7_->Barrier(NumBarrierGroups, pBarrierGroups);
    }
}

// ---- KxCommandQueueProxy ----

KxCommandQueueProxy::KxCommandQueueProxy(ID3D12CommandQueue* inner) : inner_(inner) {
    if (inner_) inner_->AddRef();
}
KxCommandQueueProxy::~KxCommandQueueProxy() {
    if (inner_) { inner_->Release(); inner_ = nullptr; }
}

HRESULT STDMETHODCALLTYPE KxCommandQueueProxy::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) ||
        riid == __uuidof(ID3D12DeviceChild) || riid == __uuidof(ID3D12Pageable) ||
        riid == __uuidof(ID3D12CommandQueue)) {
        *ppv = static_cast<ID3D12CommandQueue*>(this);
        AddRef();
        return S_OK;
    }
    return inner_->QueryInterface(riid, ppv);
}

ULONG STDMETHODCALLTYPE KxCommandQueueProxy::AddRef() { return ref_.fetch_add(1, std::memory_order_relaxed) + 1; }
ULONG STDMETHODCALLTYPE KxCommandQueueProxy::Release() {
    const ULONG next = ref_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (next == 0) delete this;
    return next;
}

HRESULT STDMETHODCALLTYPE KxCommandQueueProxy::GetPrivateData(REFGUID g, UINT* s, void* d) { return inner_->GetPrivateData(g, s, d); }
HRESULT STDMETHODCALLTYPE KxCommandQueueProxy::SetPrivateData(REFGUID g, UINT s, const void* d) { return inner_->SetPrivateData(g, s, d); }
HRESULT STDMETHODCALLTYPE KxCommandQueueProxy::SetPrivateDataInterface(REFGUID g, const IUnknown* d) { return inner_->SetPrivateDataInterface(g, d); }
HRESULT STDMETHODCALLTYPE KxCommandQueueProxy::SetName(LPCWSTR n) { return inner_->SetName(n); }
HRESULT STDMETHODCALLTYPE KxCommandQueueProxy::GetDevice(REFIID r, void** o) { return inner_->GetDevice(r, o); }
void STDMETHODCALLTYPE KxCommandQueueProxy::UpdateTileMappings(ID3D12Resource* res, UINT n, const D3D12_TILED_RESOURCE_COORDINATE* c, const D3D12_TILE_REGION_SIZE* sz, ID3D12Heap* h, UINT nr, const D3D12_TILE_RANGE_FLAGS* f, const UINT* ho, const UINT* tc, D3D12_TILE_MAPPING_FLAGS fl) {
    g_tile_mapping_updates.fetch_add(1, std::memory_order_relaxed);
    if (nr > 0 && f) {
        for (UINT i = 0; i < nr; ++i) {
            const UINT cnt = tc ? tc[i] : 1u;
            if (f[i] & D3D12_TILE_RANGE_FLAG_NULL) {
                g_tiles_unmapped.fetch_add(cnt, std::memory_order_relaxed);
            } else {
                g_tiles_mapped.fetch_add(cnt, std::memory_order_relaxed);
            }
        }
    } else if (n > 0 && sz) {
        for (UINT i = 0; i < n; ++i) {
            g_tiles_mapped.fetch_add(sz[i].NumTiles, std::memory_order_relaxed);
        }
    }
    inner_->UpdateTileMappings(res, n, c, sz, h, nr, f, ho, tc, fl);
}
void STDMETHODCALLTYPE KxCommandQueueProxy::CopyTileMappings(ID3D12Resource* dr, const D3D12_TILED_RESOURCE_COORDINATE* dc, ID3D12Resource* sr, const D3D12_TILED_RESOURCE_COORDINATE* sc, const D3D12_TILE_REGION_SIZE* sz, D3D12_TILE_MAPPING_FLAGS fl) {
    g_tile_mapping_updates.fetch_add(1, std::memory_order_relaxed);
    if (sz) g_tiles_mapped.fetch_add(sz->NumTiles, std::memory_order_relaxed);
    inner_->CopyTileMappings(dr, dc, sr, sc, sz, fl);
}
void STDMETHODCALLTYPE KxCommandQueueProxy::ExecuteCommandLists(UINT n, ID3D12CommandList* const* ls) {
    if (n == 0 || !ls) {
        inner_->ExecuteCommandLists(n, ls);
        return;
    }
    std::vector<ID3D12CommandList*> unwrapped(n, nullptr);
    for (UINT i = 0; i < n; ++i) {
        unwrapped[i] = ls[i];
        if (ls[i]) {
            KxGraphicsCommandListProxy* p = nullptr;
            if (SUCCEEDED(ls[i]->QueryInterface(kIID_KxGraphicsCommandListProxy, reinterpret_cast<void**>(&p))) && p) {
                unwrapped[i] = p->inner();
                p->Release();
            }
        }
    }
    inner_->ExecuteCommandLists(n, unwrapped.data());
}
void STDMETHODCALLTYPE KxCommandQueueProxy::SetMarker(UINT m, const void* d, UINT s) { inner_->SetMarker(m, d, s); }
void STDMETHODCALLTYPE KxCommandQueueProxy::BeginEvent(UINT m, const void* d, UINT s) { inner_->BeginEvent(m, d, s); }
void STDMETHODCALLTYPE KxCommandQueueProxy::EndEvent() { inner_->EndEvent(); }
HRESULT STDMETHODCALLTYPE KxCommandQueueProxy::Signal(ID3D12Fence* f, UINT64 v) { g_inter_queue_fences.fetch_add(1, std::memory_order_relaxed); return inner_->Signal(f, v); }
HRESULT STDMETHODCALLTYPE KxCommandQueueProxy::Wait(ID3D12Fence* f, UINT64 v) { g_inter_queue_fences.fetch_add(1, std::memory_order_relaxed); return inner_->Wait(f, v); }
HRESULT STDMETHODCALLTYPE KxCommandQueueProxy::GetTimestampFrequency(UINT64* f) { return inner_->GetTimestampFrequency(f); }
HRESULT STDMETHODCALLTYPE KxCommandQueueProxy::GetClockCalibration(UINT64* g, UINT64* c) { return inner_->GetClockCalibration(g, c); }
D3D12_COMMAND_QUEUE_DESC STDMETHODCALLTYPE KxCommandQueueProxy::GetDesc() { return inner_->GetDesc(); }

// ---- KxDeviceProxy ----

KxDeviceProxy::KxDeviceProxy(ID3D12Device* inner) : inner_(inner) {
    if (inner_) {
        inner_->AddRef();
        (void)inner_->QueryInterface(IID_PPV_ARGS(&inner1_));
        (void)inner_->QueryInterface(IID_PPV_ARGS(&inner2_));
        D3D12_FEATURE_DATA_D3D12_OPTIONS1 opt1{};
        if (SUCCEEDED(inner_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &opt1, sizeof(opt1)))) {
            supports_wave64_ = (opt1.WaveLaneCountMin <= 64 && opt1.WaveLaneCountMax >= 64);
        }
    }
}
KxDeviceProxy::~KxDeviceProxy() {
    if (inner2_) { inner2_->Release(); inner2_ = nullptr; }
    if (inner1_) { inner1_->Release(); inner1_ = nullptr; }
    if (inner_) { inner_->Release(); inner_ = nullptr; }
}

HRESULT STDMETHODCALLTYPE KxDeviceProxy::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12Device)) {
        *ppv = static_cast<ID3D12Device*>(this);
        AddRef();
        return S_OK;
    }
    if (riid == __uuidof(ID3D12Device1) && inner1_) {
        *ppv = static_cast<ID3D12Device1*>(this);
        AddRef();
        g_device_qi_upgrades.fetch_add(1, std::memory_order_relaxed);
        return S_OK;
    }
    if (riid == __uuidof(ID3D12Device2) && inner2_) {
        *ppv = static_cast<ID3D12Device2*>(this);
        AddRef();
        g_device_qi_upgrades.fetch_add(1, std::memory_order_relaxed);
        return S_OK;
    }
    // Forward ID3D12Device3..ID3D12Device10 (and newer interfaces) cleanly to the underlying hardware device
    HRESULT hr = inner_->QueryInterface(riid, ppv);
    if (SUCCEEDED(hr)) {
        g_device_qi_upgrades.fetch_add(1, std::memory_order_relaxed);
    }
    return hr;
}

ULONG STDMETHODCALLTYPE KxDeviceProxy::AddRef() { return ref_.fetch_add(1, std::memory_order_relaxed) + 1; }
ULONG STDMETHODCALLTYPE KxDeviceProxy::Release() {
    const ULONG next = ref_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (next == 0) delete this;
    return next;
}

HRESULT STDMETHODCALLTYPE KxDeviceProxy::GetPrivateData(REFGUID g, UINT* s, void* d) { return inner_->GetPrivateData(g, s, d); }
HRESULT STDMETHODCALLTYPE KxDeviceProxy::SetPrivateData(REFGUID g, UINT s, const void* d) { return inner_->SetPrivateData(g, s, d); }
HRESULT STDMETHODCALLTYPE KxDeviceProxy::SetPrivateDataInterface(REFGUID g, const IUnknown* d) { return inner_->SetPrivateDataInterface(g, d); }
HRESULT STDMETHODCALLTYPE KxDeviceProxy::SetName(LPCWSTR n) { return inner_->SetName(n); }
UINT STDMETHODCALLTYPE KxDeviceProxy::GetNodeCount() { return inner_->GetNodeCount(); }

HRESULT STDMETHODCALLTYPE KxDeviceProxy::CreateCommandQueue(const D3D12_COMMAND_QUEUE_DESC* d, REFIID r, void** o) {
    ID3D12CommandQueue* raw = nullptr;
    HRESULT hr = inner_->CreateCommandQueue(d, IID_PPV_ARGS(&raw));
    if (FAILED(hr)) return hr;
    g_queues.fetch_add(1, std::memory_order_relaxed);
    if (d && d->Type == D3D12_COMMAND_LIST_TYPE_COMPUTE) {
        g_compute_queues.fetch_add(1, std::memory_order_relaxed);
    }
    auto* proxy = new KxCommandQueueProxy(raw);
    raw->Release();
    hr = proxy->QueryInterface(r, o);
    proxy->Release();
    return hr;
}

HRESULT STDMETHODCALLTYPE KxDeviceProxy::CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE t, REFIID r, void** o) { return inner_->CreateCommandAllocator(t, r, o); }
HRESULT STDMETHODCALLTYPE KxDeviceProxy::CreateGraphicsPipelineState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d, REFIID r, void** o) {
    if (d) {
        std::vector<uint8_t> vs_dxil, ps_dxil;
        bool patched_any = false;
        bool cache_hit = false;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC patched = *d;

        if (d->VS.pShaderBytecode && d->VS.BytecodeLength >= 12) {
            if (try_recompile_kestrel_rdna2(d->VS.pShaderBytecode, d->VS.BytecodeLength,
                                            gpu::ShaderStage::Vertex, supports_wave64_, vs_dxil, cache_hit)) {
                patched.VS.pShaderBytecode = vs_dxil.data();
                patched.VS.BytecodeLength = vs_dxil.size();
                patched_any = true;
            }
        }
        if (d->PS.pShaderBytecode && d->PS.BytecodeLength >= 12) {
            if (try_recompile_kestrel_rdna2(d->PS.pShaderBytecode, d->PS.BytecodeLength,
                                            gpu::ShaderStage::Pixel, supports_wave64_, ps_dxil, cache_hit)) {
                patched.PS.pShaderBytecode = ps_dxil.data();
                patched.PS.BytecodeLength = ps_dxil.size();
                patched_any = true;
            }
        }
        if (patched_any) {
            return inner_->CreateGraphicsPipelineState(&patched, r, o);
        }
    }
    return inner_->CreateGraphicsPipelineState(d, r, o);
}

HRESULT STDMETHODCALLTYPE KxDeviceProxy::CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC* d, REFIID r, void** o) {
    if (d && d->CS.pShaderBytecode && d->CS.BytecodeLength >= 12) {
        std::vector<uint8_t> dxil;
        bool cache_hit = false;
        if (try_recompile_kestrel_rdna2(d->CS.pShaderBytecode, d->CS.BytecodeLength,
                                        gpu::ShaderStage::Compute, supports_wave64_, dxil, cache_hit)) {
            D3D12_COMPUTE_PIPELINE_STATE_DESC patched = *d;
            patched.CS.pShaderBytecode = dxil.data();
            patched.CS.BytecodeLength = dxil.size();
            return inner_->CreateComputePipelineState(&patched, r, o);
        }
    }
    return inner_->CreateComputePipelineState(d, r, o);
}

HRESULT STDMETHODCALLTYPE KxDeviceProxy::CreateCommandList(UINT m, D3D12_COMMAND_LIST_TYPE t, ID3D12CommandAllocator* a, ID3D12PipelineState* i, REFIID r, void** o) {
    if (r == __uuidof(ID3D12CommandList) ||
        r == __uuidof(ID3D12GraphicsCommandList) ||
        r == __uuidof(ID3D12GraphicsCommandList1) ||
        r == __uuidof(ID3D12GraphicsCommandList2) ||
        r == __uuidof(ID3D12GraphicsCommandList3) ||
        r == __uuidof(ID3D12GraphicsCommandList4) ||
        r == __uuidof(ID3D12GraphicsCommandList5)) {
        ID3D12GraphicsCommandList* raw = nullptr;
        HRESULT hr = inner_->CreateCommandList(m, t, a, i, IID_PPV_ARGS(&raw));
        if (FAILED(hr)) return hr;
        g_cmd_lists.fetch_add(1, std::memory_order_relaxed);
        auto* proxy = new KxGraphicsCommandListProxy(raw);
        raw->Release();
        hr = proxy->QueryInterface(r, o);
        proxy->Release();
        return hr;
    }
    HRESULT hr = inner_->CreateCommandList(m, t, a, i, r, o);
    if (SUCCEEDED(hr)) g_cmd_lists.fetch_add(1, std::memory_order_relaxed);
    return hr;
}
HRESULT STDMETHODCALLTYPE KxDeviceProxy::CheckFeatureSupport(D3D12_FEATURE f, void* d, UINT s) { return inner_->CheckFeatureSupport(f, d, s); }
HRESULT STDMETHODCALLTYPE KxDeviceProxy::CreateDescriptorHeap(const D3D12_DESCRIPTOR_HEAP_DESC* d, REFIID r, void** o) { return inner_->CreateDescriptorHeap(d, r, o); }
UINT STDMETHODCALLTYPE KxDeviceProxy::GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE t) { return inner_->GetDescriptorHandleIncrementSize(t); }
HRESULT STDMETHODCALLTYPE KxDeviceProxy::CreateRootSignature(UINT m, const void* b, SIZE_T l, REFIID r, void** o) { return inner_->CreateRootSignature(m, b, l, r, o); }
void STDMETHODCALLTYPE KxDeviceProxy::CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h) { inner_->CreateConstantBufferView(d, h); }
void STDMETHODCALLTYPE KxDeviceProxy::CreateShaderResourceView(ID3D12Resource* res, const D3D12_SHADER_RESOURCE_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h) { inner_->CreateShaderResourceView(res, d, h); }
void STDMETHODCALLTYPE KxDeviceProxy::CreateUnorderedAccessView(ID3D12Resource* res, ID3D12Resource* cnt, const D3D12_UNORDERED_ACCESS_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h) { inner_->CreateUnorderedAccessView(res, cnt, d, h); }
void STDMETHODCALLTYPE KxDeviceProxy::CreateRenderTargetView(ID3D12Resource* res, const D3D12_RENDER_TARGET_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h) { inner_->CreateRenderTargetView(res, d, h); }
void STDMETHODCALLTYPE KxDeviceProxy::CreateDepthStencilView(ID3D12Resource* res, const D3D12_DEPTH_STENCIL_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h) { inner_->CreateDepthStencilView(res, d, h); }
void STDMETHODCALLTYPE KxDeviceProxy::CreateSampler(const D3D12_SAMPLER_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h) { inner_->CreateSampler(d, h); }
void STDMETHODCALLTYPE KxDeviceProxy::CopyDescriptors(UINT nd, const D3D12_CPU_DESCRIPTOR_HANDLE* ds, const UINT* dsz, UINT ns, const D3D12_CPU_DESCRIPTOR_HANDLE* ss, const UINT* ssz, D3D12_DESCRIPTOR_HEAP_TYPE t) { inner_->CopyDescriptors(nd, ds, dsz, ns, ss, ssz, t); }
void STDMETHODCALLTYPE KxDeviceProxy::CopyDescriptorsSimple(UINT n, D3D12_CPU_DESCRIPTOR_HANDLE d, D3D12_CPU_DESCRIPTOR_HANDLE s, D3D12_DESCRIPTOR_HEAP_TYPE t) { inner_->CopyDescriptorsSimple(n, d, s, t); }
D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE KxDeviceProxy::GetResourceAllocationInfo(UINT m, UINT n, const D3D12_RESOURCE_DESC* d) { return inner_->GetResourceAllocationInfo(m, n, d); }
D3D12_HEAP_PROPERTIES STDMETHODCALLTYPE KxDeviceProxy::GetCustomHeapProperties(UINT m, D3D12_HEAP_TYPE t) { return inner_->GetCustomHeapProperties(m, t); }
HRESULT STDMETHODCALLTYPE KxDeviceProxy::CreateCommittedResource(const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS hf, const D3D12_RESOURCE_DESC* d, D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE* cv, REFIID r, void** o) {
    HRESULT hr = inner_->CreateCommittedResource(hp, hf, d, st, cv, r, o);
    if (SUCCEEDED(hr)) g_resources.fetch_add(1, std::memory_order_relaxed);
    return hr;
}
HRESULT STDMETHODCALLTYPE KxDeviceProxy::CreateHeap(const D3D12_HEAP_DESC* d, REFIID r, void** o) { return inner_->CreateHeap(d, r, o); }
HRESULT STDMETHODCALLTYPE KxDeviceProxy::CreatePlacedResource(ID3D12Heap* h, UINT64 off, const D3D12_RESOURCE_DESC* d, D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE* cv, REFIID r, void** o) {
    HRESULT hr = inner_->CreatePlacedResource(h, off, d, st, cv, r, o);
    if (SUCCEEDED(hr)) g_resources.fetch_add(1, std::memory_order_relaxed);
    return hr;
}
HRESULT STDMETHODCALLTYPE KxDeviceProxy::CreateReservedResource(const D3D12_RESOURCE_DESC* d, D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE* cv, REFIID r, void** o) {
    HRESULT hr = inner_->CreateReservedResource(d, st, cv, r, o);
    if (SUCCEEDED(hr)) g_reserved_resources.fetch_add(1, std::memory_order_relaxed);
    return hr;
}
HRESULT STDMETHODCALLTYPE KxDeviceProxy::CreateSharedHandle(ID3D12DeviceChild* obj, const SECURITY_ATTRIBUTES* a, DWORD acc, LPCWSTR n, HANDLE* h) { return inner_->CreateSharedHandle(obj, a, acc, n, h); }
HRESULT STDMETHODCALLTYPE KxDeviceProxy::OpenSharedHandle(HANDLE h, REFIID r, void** o) { return inner_->OpenSharedHandle(h, r, o); }
HRESULT STDMETHODCALLTYPE KxDeviceProxy::OpenSharedHandleByName(LPCWSTR n, DWORD acc, HANDLE* h) { return inner_->OpenSharedHandleByName(n, acc, h); }
HRESULT STDMETHODCALLTYPE KxDeviceProxy::MakeResident(UINT n, ID3D12Pageable* const* o) { return inner_->MakeResident(n, o); }
HRESULT STDMETHODCALLTYPE KxDeviceProxy::Evict(UINT n, ID3D12Pageable* const* o) { return inner_->Evict(n, o); }
HRESULT STDMETHODCALLTYPE KxDeviceProxy::CreateFence(UINT64 v, D3D12_FENCE_FLAGS f, REFIID r, void** o) { return inner_->CreateFence(v, f, r, o); }
HRESULT STDMETHODCALLTYPE KxDeviceProxy::GetDeviceRemovedReason() { return inner_->GetDeviceRemovedReason(); }
void STDMETHODCALLTYPE KxDeviceProxy::GetCopyableFootprints(const D3D12_RESOURCE_DESC* d, UINT f, UINT n, UINT64 off, D3D12_PLACED_SUBRESOURCE_FOOTPRINT* l, UINT* nr, UINT64* rs, UINT64* tb) { inner_->GetCopyableFootprints(d, f, n, off, l, nr, rs, tb); }
HRESULT STDMETHODCALLTYPE KxDeviceProxy::CreateQueryHeap(const D3D12_QUERY_HEAP_DESC* d, REFIID r, void** o) { return inner_->CreateQueryHeap(d, r, o); }
HRESULT STDMETHODCALLTYPE KxDeviceProxy::SetStablePowerState(BOOL e) { return inner_->SetStablePowerState(e); }
HRESULT STDMETHODCALLTYPE KxDeviceProxy::CreateCommandSignature(const D3D12_COMMAND_SIGNATURE_DESC* d, ID3D12RootSignature* rs, REFIID r, void** o) { return inner_->CreateCommandSignature(d, rs, r, o); }
void STDMETHODCALLTYPE KxDeviceProxy::GetResourceTiling(ID3D12Resource* res, UINT* nt, D3D12_PACKED_MIP_INFO* pm, D3D12_TILE_SHAPE* ts, UINT* ns, UINT fs, D3D12_SUBRESOURCE_TILING* st) { inner_->GetResourceTiling(res, nt, pm, ts, ns, fs, st); }
LUID STDMETHODCALLTYPE KxDeviceProxy::GetAdapterLuid() { return inner_->GetAdapterLuid(); }

// ID3D12Device1
HRESULT STDMETHODCALLTYPE KxDeviceProxy::CreatePipelineLibrary(const void* b, SIZE_T l, REFIID r, void** o) {
    return inner1_ ? inner1_->CreatePipelineLibrary(b, l, r, o) : E_NOINTERFACE;
}
HRESULT STDMETHODCALLTYPE KxDeviceProxy::SetEventOnMultipleFenceCompletion(ID3D12Fence* const* f, const UINT64* v, UINT n, D3D12_MULTIPLE_FENCE_WAIT_FLAGS fl, HANDLE ev) {
    return inner1_ ? inner1_->SetEventOnMultipleFenceCompletion(f, v, n, fl, ev) : E_NOINTERFACE;
}
HRESULT STDMETHODCALLTYPE KxDeviceProxy::SetResidencyPriority(UINT n, ID3D12Pageable* const* o, const D3D12_RESIDENCY_PRIORITY* p) {
    return inner1_ ? inner1_->SetResidencyPriority(n, o, p) : E_NOINTERFACE;
}

// ID3D12Device2 — stream-based PSO creation with automatic KXRN/KXRV/KXRP/KXRM/KXRA RDNA2->DXIL patching
HRESULT STDMETHODCALLTYPE KxDeviceProxy::CreatePipelineState(const D3D12_PIPELINE_STATE_STREAM_DESC* d, REFIID r, void** o) {
    if (!inner2_) return E_NOINTERFACE;
    if (!d || !d->pPipelineStateSubobjectStream || d->SizeInBytes == 0) {
        return inner2_->CreatePipelineState(d, r, o);
    }

    std::vector<uint8_t> stream_copy(d->SizeInBytes);
    std::memcpy(stream_copy.data(), d->pPipelineStateSubobjectStream, d->SizeInBytes);

    std::vector<std::vector<uint8_t>> owned_dxil;
    owned_dxil.reserve(6);
    bool patched_any = false;

    constexpr size_t bc_off = (sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE) + alignof(D3D12_SHADER_BYTECODE) - 1) &
                              ~(alignof(D3D12_SHADER_BYTECODE) - 1);

    // Pre-scan for an Amplification Shader subobject so paired Mesh Shaders emit `in payload KxMeshPayload`
    bool has_amplification = false;
    {
        size_t scan = 0;
        while (scan + sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE) <= stream_copy.size()) {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type{};
            std::memcpy(&type, stream_copy.data() + scan, sizeof(type));
            const size_t stride = subobject_stride(type);
            if (stride == 0 || scan + stride > stream_copy.size()) break;
            if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS) {
                D3D12_SHADER_BYTECODE bc{};
                std::memcpy(&bc, stream_copy.data() + scan + bc_off, sizeof(bc));
                if (bc.pShaderBytecode && bc.BytecodeLength > 0) {
                    has_amplification = true;
                }
            }
            scan += stride;
        }
    }

    size_t offset = 0;
    while (offset + sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE) <= stream_copy.size()) {
        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type{};
        std::memcpy(&type, stream_copy.data() + offset, sizeof(type));
        const size_t stride = subobject_stride(type);
        if (stride == 0 || offset + stride > stream_copy.size()) break;

        if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS ||
            type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS ||
            type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS ||
            type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS ||
            type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS) {
            D3D12_SHADER_BYTECODE bc{};
            std::memcpy(&bc, stream_copy.data() + offset + bc_off, sizeof(bc));
            const auto stage = (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS) ? gpu::ShaderStage::Vertex
                             : (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS) ? gpu::ShaderStage::Pixel
                             : (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS) ? gpu::ShaderStage::Mesh
                             : (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS) ? gpu::ShaderStage::Amplification
                                                                                : gpu::ShaderStage::Compute;
            const bool mesh_payload = (stage == gpu::ShaderStage::Mesh) && has_amplification;
            std::vector<uint8_t> dxil;
            bool cache_hit = false;
            if (try_recompile_kestrel_rdna2(bc.pShaderBytecode, bc.BytecodeLength, stage,
                                            supports_wave64_, dxil, cache_hit, mesh_payload)) {
                owned_dxil.push_back(std::move(dxil));
                D3D12_SHADER_BYTECODE patched_bc{owned_dxil.back().data(), owned_dxil.back().size()};
                std::memcpy(stream_copy.data() + offset + bc_off, &patched_bc, sizeof(patched_bc));
                patched_any = true;
            }
        }
        offset += stride;
    }

    if (patched_any) {
        g_stream_pso_recomp.fetch_add(1, std::memory_order_relaxed);
        D3D12_PIPELINE_STATE_STREAM_DESC patched_desc{stream_copy.size(), stream_copy.data()};
        return inner2_->CreatePipelineState(&patched_desc, r, o);
    }
    return inner2_->CreatePipelineState(d, r, o);
}

void set_d3d12x_preferred_adapter(int adapter_index) { g_preferred_adapter.store(adapter_index, std::memory_order_relaxed); }

void set_d3d12x_windowed(bool enabled, uint32_t width, uint32_t height) {
    std::lock_guard lock(g_present_mtx);
    g_windowed = enabled;
    g_win_w = width;
    g_win_h = height;
}

void set_d3d12x_capture_path(const std::filesystem::path& bmp_path) {
    std::lock_guard lock(g_present_mtx);
    g_capture_path = bmp_path;
}

void set_d3d12x_frame_pacing(const FramePacingConfig& config) {
    std::lock_guard lock(g_present_mtx);
    g_pacing_cfg = config;
    g_headless_pacer.configure(config);
    if (g_window_host) {
        g_window_host->set_frame_pacing(config);
    }
}

FramePacingStats d3d12x_frame_pacing_stats() {
    std::lock_guard lock(g_present_mtx);
    if (g_window_host && g_window_host->frames_presented() > 0) {
        return g_window_host->pacing_stats();
    }
    if (g_has_last_window_pacing) {
        return g_last_window_pacing;
    }
    return g_headless_pacer.stats();
}

void set_d3d12x_shader_cache_dir(const std::filesystem::path& cache_dir) {
    std::lock_guard lock(g_shader_cache_mtx);
    g_disk_cache.set_root_dir(cache_dir);
}

void clear_d3d12x_memory_shader_cache() {
    std::lock_guard lock(g_shader_cache_mtx);
    g_dxil_cache.clear();
}

void shutdown_d3d12x_window() {
    std::lock_guard lock(g_present_mtx);
    if (g_window_host && g_window_host->frames_presented() > 0) {
        g_last_window_pacing = g_window_host->pacing_stats();
        g_has_last_window_pacing = true;
    }
    g_window_host.reset();
}

HRESULT __stdcall kx_D3D12GetInterface(REFCLSID, REFIID, void** ppv) {
    if (!ppv) return E_INVALIDARG;
    *ppv = nullptr;
    return S_OK;
}

void register_d3d12x(Registry& reg) {
    reg.register_module("d3d12_xs.dll");
    reg.register_module("d3d12_x.dll");
    reg.register_module("d3d12.dll");
    reg.register_module("d3d12core.dll");
    for (const char* mod : {"d3d12_xs.dll", "d3d12_x.dll", "d3d12.dll", "d3d12core.dll"}) {
        reg.register_export(mod, "D3D12CreateDevice", reinterpret_cast<void*>(&kx_D3D12CreateDevice));
        reg.register_export(mod, "D3D12XboxCreateDevice", reinterpret_cast<void*>(&kx_D3D12XboxCreateDevice));
        reg.register_export(mod, "D3D12ZeroMemoryX", reinterpret_cast<void*>(&kx_D3D12ZeroMemoryX));
        reg.register_export(mod, "D3D12SerializeRootSignature", reinterpret_cast<void*>(&kx_D3D12SerializeRootSignature));
        reg.register_export(mod, "D3D12PresentX", reinterpret_cast<void*>(&kx_D3D12PresentX));
        reg.register_export(mod, "D3D12WaitFrameEventX", reinterpret_cast<void*>(&kx_D3D12WaitFrameEventX));
        reg.register_export(mod, "D3D12GetInterface", reinterpret_cast<void*>(&kx_D3D12GetInterface));
    }
}

void record_work_graph_created() {
    g_work_graphs_created.fetch_add(1, std::memory_order_relaxed);
}

void record_work_graph_dispatch(uint64_t count) {
    g_work_graph_dispatches.fetch_add(count, std::memory_order_relaxed);
}

D3D12XStats d3d12x_stats() {
    return {g_devices.load(), g_queues.load(), g_cmd_lists.load(), g_resources.load(),
            g_zero_mem.load(), g_presents.load(), g_pso_recomp.load(), g_pso_cache_hits.load(),
            g_gfx_pso_recomp.load(), g_gfx_pso_cache_hits.load(), g_stream_pso_recomp.load(),
            g_disk_cache_hits.load(), g_disk_cache_misses.load(), g_device_qi_upgrades.load(),
            g_draw_calls.load(), g_dispatch_calls.load(), g_barrier_calls.load(), g_pso_binds.load(),
            g_compute_queues.load(), g_inter_queue_fences.load(),
            g_vrs_rate_sets.load(), g_vrs_image_binds.load(),
            g_work_graphs_created.load(), g_work_graph_dispatches.load(),
            g_dispatch_mesh_calls.load(), g_enhanced_barrier_calls.load(), g_enhanced_barrier_groups.load(),
            g_reserved_resources.load(), g_tile_mapping_updates.load(), g_tiles_mapped.load(), g_tiles_unmapped.load(), g_copy_tiles_calls.load()};
}

void reset_d3d12x_stats() {
    g_devices = g_queues = g_cmd_lists = g_resources = g_zero_mem = g_presents =
        g_pso_recomp = g_pso_cache_hits = g_gfx_pso_recomp = g_gfx_pso_cache_hits =
        g_stream_pso_recomp = g_disk_cache_hits = g_disk_cache_misses = g_device_qi_upgrades =
        g_draw_calls = g_dispatch_calls = g_barrier_calls = g_pso_binds = g_compute_queues =
        g_inter_queue_fences = g_vrs_rate_sets = g_vrs_image_binds =
        g_work_graphs_created = g_work_graph_dispatches = g_dispatch_mesh_calls = g_enhanced_barrier_calls = g_enhanced_barrier_groups =
        g_reserved_resources = g_tile_mapping_updates = g_tiles_mapped = g_tiles_unmapped = g_copy_tiles_calls = 0;
    {
        std::lock_guard lock(g_shader_cache_mtx);
        g_dxil_cache.clear();
        g_disk_cache.reset_stats();
    }
    {
        std::lock_guard lock(g_present_mtx);
        g_window_host.reset();
        g_windowed = false;
        g_capture_path.clear();
        g_pacing_cfg = {};
        g_headless_pacer.reset();
        g_headless_pacer.configure(g_pacing_cfg);
        g_last_window_pacing = {};
        g_has_last_window_pacing = false;
    }
}

}  // namespace kestrel::hle
