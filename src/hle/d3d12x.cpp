#include "kestrel/hle/d3d12x.hpp"
#include "kestrel/hle/window_host.hpp"
#include "kestrel/gpu/dxc_compiler.hpp"
#include "kestrel/gpu/ir.hpp"
#include "kestrel/gpu/rdna2_isa.hpp"
#include "kestrel/gpu/rdna2_recomp.hpp"

#include <dxgi1_6.h>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace kestrel::hle {

namespace {

std::atomic<int> g_preferred_adapter{-1};
std::atomic<uint64_t> g_devices{0}, g_queues{0}, g_cmd_lists{0}, g_resources{0},
    g_zero_mem{0}, g_presents{0}, g_pso_recomp{0}, g_pso_cache_hits{0},
    g_gfx_pso_recomp{0}, g_gfx_pso_cache_hits{0};

std::mutex g_present_mtx;
bool g_windowed = false;
uint32_t g_win_w = 640;
uint32_t g_win_h = 640;
std::filesystem::path g_capture_path;
std::unique_ptr<WindowHost> g_window_host;

std::mutex g_shader_cache_mtx;
std::unordered_map<uint64_t, std::vector<uint8_t>> g_dxil_cache;
gpu::DxcCompiler g_dxc;
bool g_dxc_ready = false;

uint64_t fnv1a_64(const uint8_t* data, size_t len, uint64_t seed = 14695981039346656037ull) {
    uint64_t h = seed;
    for (size_t i = 0; i < len; ++i) {
        h ^= data[i];
        h *= 1099511628211ull;
    }
    return h;
}

// Clean-room native RDNA2 container headers:
//   "KXRN" + uint32 group_size_x + GFX10.3 dwords -> Compute (cs_6_6)
//   "KXRV" + uint32 reserved     + GFX10.3 dwords -> Vertex  (vs_6_6)
//   "KXRP" + uint32 reserved     + GFX10.3 dwords -> Pixel   (ps_6_6)
bool try_recompile_kestrel_rdna2(
    const void* bytecode, SIZE_T length, gpu::ShaderStage expected_stage,
    bool prefer_wave64, std::vector<uint8_t>& out_dxil, bool& out_cache_hit) {
    out_cache_hit = false;
    if (!bytecode || length < 12) return false;
    const auto* bytes = static_cast<const uint8_t*>(bytecode);

    const char* expected_magic = (expected_stage == gpu::ShaderStage::Compute) ? "KXRN"
                               : (expected_stage == gpu::ShaderStage::Vertex)  ? "KXRV"
                                                                               : "KXRP";
    if (std::memcmp(bytes, expected_magic, 4) != 0) return false;

    uint32_t group_x = 64;
    if (expected_stage == gpu::ShaderStage::Compute) {
        std::memcpy(&group_x, bytes + 4, 4);
        if (group_x == 0 || group_x > 1024) group_x = 64;
    }

    const size_t code_bytes = length - 8;
    if (code_bytes == 0 || (code_bytes % 4) != 0) return false;

    const gpu::WaveMode wave = prefer_wave64 ? gpu::WaveMode::Wave64 : gpu::WaveMode::Wave32;
    const uint64_t seed = (static_cast<uint64_t>(expected_stage) << 8) | static_cast<uint64_t>(wave);
    const uint64_t cache_key = fnv1a_64(bytes, length, seed);

    {
        std::lock_guard lock(g_shader_cache_mtx);
        auto it = g_dxil_cache.find(cache_key);
        if (it != g_dxil_cache.end()) {
            out_dxil = it->second;
            out_cache_hit = true;
            if (expected_stage == gpu::ShaderStage::Compute) g_pso_cache_hits.fetch_add(1, std::memory_order_relaxed);
            else g_gfx_pso_cache_hits.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
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

    auto recomp = gpu::emit_hlsl_from_ir(ir, {.stage = expected_stage, .wave_mode = wave, .group_size_x = group_x});
    const char* profile = (expected_stage == gpu::ShaderStage::Compute) ? "cs_6_6"
                        : (expected_stage == gpu::ShaderStage::Vertex)  ? "vs_6_6"
                                                                        : "ps_6_6";
    auto compiled = g_dxc.compile_hlsl(recomp.hlsl, "main", profile);
    if (!compiled) return false;

    {
        std::lock_guard lock(g_shader_cache_mtx);
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
    if (queue && rt_or_planes) {
        auto* rt = reinterpret_cast<ID3D12Resource*>(const_cast<void*>(rt_or_planes));
        std::lock_guard lock(g_present_mtx);
        ID3D12Device* dev = nullptr;
        if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&dev))) && dev) {
            if (!g_capture_path.empty()) {
                (void)capture_d3d12_texture_bmp(dev, queue, rt, D3D12_RESOURCE_STATE_RENDER_TARGET, g_capture_path);
            }
            if (g_windowed) {
                if (!g_window_host) {
                    g_window_host = std::make_unique<WindowHost>();
                    if (!g_window_host->create(dev, queue, g_win_w, g_win_h)) {
                        g_window_host.reset();
                    }
                }
                if (g_window_host && g_window_host->is_open()) {
                    (void)g_window_host->present_resource(rt, D3D12_RESOURCE_STATE_RENDER_TARGET);
                    g_window_host->pump_messages();
                }
            }
            dev->Release();
        }
    }
    return S_OK;
}

HRESULT __stdcall kx_D3D12WaitFrameEventX(uint64_t, uint32_t, void*) {
    return S_OK;
}

}  // namespace

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
void STDMETHODCALLTYPE KxCommandQueueProxy::UpdateTileMappings(ID3D12Resource* res, UINT n, const D3D12_TILED_RESOURCE_COORDINATE* c, const D3D12_TILE_REGION_SIZE* sz, ID3D12Heap* h, UINT nr, const D3D12_TILE_RANGE_FLAGS* f, const UINT* ho, const UINT* tc, D3D12_TILE_MAPPING_FLAGS fl) { inner_->UpdateTileMappings(res, n, c, sz, h, nr, f, ho, tc, fl); }
void STDMETHODCALLTYPE KxCommandQueueProxy::CopyTileMappings(ID3D12Resource* dr, const D3D12_TILED_RESOURCE_COORDINATE* dc, ID3D12Resource* sr, const D3D12_TILED_RESOURCE_COORDINATE* sc, const D3D12_TILE_REGION_SIZE* sz, D3D12_TILE_MAPPING_FLAGS fl) { inner_->CopyTileMappings(dr, dc, sr, sc, sz, fl); }
void STDMETHODCALLTYPE KxCommandQueueProxy::ExecuteCommandLists(UINT n, ID3D12CommandList* const* ls) { inner_->ExecuteCommandLists(n, ls); }
void STDMETHODCALLTYPE KxCommandQueueProxy::SetMarker(UINT m, const void* d, UINT s) { inner_->SetMarker(m, d, s); }
void STDMETHODCALLTYPE KxCommandQueueProxy::BeginEvent(UINT m, const void* d, UINT s) { inner_->BeginEvent(m, d, s); }
void STDMETHODCALLTYPE KxCommandQueueProxy::EndEvent() { inner_->EndEvent(); }
HRESULT STDMETHODCALLTYPE KxCommandQueueProxy::Signal(ID3D12Fence* f, UINT64 v) { return inner_->Signal(f, v); }
HRESULT STDMETHODCALLTYPE KxCommandQueueProxy::Wait(ID3D12Fence* f, UINT64 v) { return inner_->Wait(f, v); }
HRESULT STDMETHODCALLTYPE KxCommandQueueProxy::GetTimestampFrequency(UINT64* f) { return inner_->GetTimestampFrequency(f); }
HRESULT STDMETHODCALLTYPE KxCommandQueueProxy::GetClockCalibration(UINT64* g, UINT64* c) { return inner_->GetClockCalibration(g, c); }
D3D12_COMMAND_QUEUE_DESC STDMETHODCALLTYPE KxCommandQueueProxy::GetDesc() { return inner_->GetDesc(); }

// ---- KxDeviceProxy ----

KxDeviceProxy::KxDeviceProxy(ID3D12Device* inner) : inner_(inner) {
    if (inner_) {
        inner_->AddRef();
        D3D12_FEATURE_DATA_D3D12_OPTIONS1 opt1{};
        if (SUCCEEDED(inner_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &opt1, sizeof(opt1)))) {
            supports_wave64_ = (opt1.WaveLaneCountMin <= 64 && opt1.WaveLaneCountMax >= 64);
        }
    }
}
KxDeviceProxy::~KxDeviceProxy() { if (inner_) { inner_->Release(); inner_ = nullptr; } }

HRESULT STDMETHODCALLTYPE KxDeviceProxy::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12Device)) {
        *ppv = static_cast<ID3D12Device*>(this);
        AddRef();
        return S_OK;
    }
    return inner_->QueryInterface(riid, ppv);
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
HRESULT STDMETHODCALLTYPE KxDeviceProxy::CreateReservedResource(const D3D12_RESOURCE_DESC* d, D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE* cv, REFIID r, void** o) { return inner_->CreateReservedResource(d, st, cv, r, o); }
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

void shutdown_d3d12x_window() {
    std::lock_guard lock(g_present_mtx);
    g_window_host.reset();
}

void register_d3d12x(Registry& reg) {
    reg.register_module("d3d12_xs.dll");
    reg.register_module("d3d12_x.dll");
    for (const char* mod : {"d3d12_xs.dll", "d3d12_x.dll"}) {
        reg.register_export(mod, "D3D12CreateDevice", reinterpret_cast<void*>(&kx_D3D12CreateDevice));
        reg.register_export(mod, "D3D12XboxCreateDevice", reinterpret_cast<void*>(&kx_D3D12XboxCreateDevice));
        reg.register_export(mod, "D3D12ZeroMemoryX", reinterpret_cast<void*>(&kx_D3D12ZeroMemoryX));
        reg.register_export(mod, "D3D12SerializeRootSignature", reinterpret_cast<void*>(&kx_D3D12SerializeRootSignature));
        reg.register_export(mod, "D3D12PresentX", reinterpret_cast<void*>(&kx_D3D12PresentX));
        reg.register_export(mod, "D3D12WaitFrameEventX", reinterpret_cast<void*>(&kx_D3D12WaitFrameEventX));
    }
}

D3D12XStats d3d12x_stats() {
    return {g_devices.load(), g_queues.load(), g_cmd_lists.load(), g_resources.load(),
            g_zero_mem.load(), g_presents.load(), g_pso_recomp.load(), g_pso_cache_hits.load(),
            g_gfx_pso_recomp.load(), g_gfx_pso_cache_hits.load()};
}
void reset_d3d12x_stats() {
    g_devices = g_queues = g_cmd_lists = g_resources = g_zero_mem = g_presents =
        g_pso_recomp = g_pso_cache_hits = g_gfx_pso_recomp = g_gfx_pso_cache_hits = 0;
    {
        std::lock_guard lock(g_shader_cache_mtx);
        g_dxil_cache.clear();
    }
    {
        std::lock_guard lock(g_present_mtx);
        g_window_host.reset();
        g_windowed = false;
        g_capture_path.clear();
    }
}

}  // namespace kestrel::hle
