#include "kestrel/hle/d3d12x.hpp"

#include <dxgi1_6.h>
#include <atomic>
#include <cstring>

namespace kestrel::hle {

namespace {

std::atomic<int> g_preferred_adapter{-1};
std::atomic<uint64_t> g_devices{0}, g_queues{0}, g_cmd_lists{0}, g_resources{0}, g_zero_mem{0}, g_presents{0};

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

}  // namespace

KxDeviceProxy::KxDeviceProxy(ID3D12Device* inner) : inner_(inner) { if (inner_) inner_->AddRef(); }
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
    HRESULT hr = inner_->CreateCommandQueue(d, r, o);
    if (SUCCEEDED(hr)) g_queues.fetch_add(1, std::memory_order_relaxed);
    return hr;
}
HRESULT STDMETHODCALLTYPE KxDeviceProxy::CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE t, REFIID r, void** o) { return inner_->CreateCommandAllocator(t, r, o); }
HRESULT STDMETHODCALLTYPE KxDeviceProxy::CreateGraphicsPipelineState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d, REFIID r, void** o) { return inner_->CreateGraphicsPipelineState(d, r, o); }
HRESULT STDMETHODCALLTYPE KxDeviceProxy::CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC* d, REFIID r, void** o) { return inner_->CreateComputePipelineState(d, r, o); }
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

void register_d3d12x(Registry& reg) {
    reg.register_module("d3d12_xs.dll");
    reg.register_module("d3d12_x.dll");
    for (const char* mod : {"d3d12_xs.dll", "d3d12_x.dll"}) {
        reg.register_export(mod, "D3D12CreateDevice", reinterpret_cast<void*>(&kx_D3D12CreateDevice));
        reg.register_export(mod, "D3D12XboxCreateDevice", reinterpret_cast<void*>(&kx_D3D12XboxCreateDevice));
        reg.register_export(mod, "D3D12ZeroMemoryX", reinterpret_cast<void*>(&kx_D3D12ZeroMemoryX));
        reg.register_export(mod, "D3D12SerializeRootSignature", reinterpret_cast<void*>(&kx_D3D12SerializeRootSignature));
    }
}

D3D12XStats d3d12x_stats() {
    return {g_devices.load(), g_queues.load(), g_cmd_lists.load(), g_resources.load(), g_zero_mem.load(), g_presents.load()};
}
void reset_d3d12x_stats() { g_devices = g_queues = g_cmd_lists = g_resources = g_zero_mem = g_presents = 0; }

}  // namespace kestrel::hle
