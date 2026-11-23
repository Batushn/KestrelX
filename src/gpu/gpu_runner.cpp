#include "kestrel/gpu/gpu_runner.hpp"

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <chrono>
#include <cstring>
#include <format>

using Microsoft::WRL::ComPtr;

namespace kestrel::gpu {

namespace {

std::string wide_to_utf8(const wchar_t* ws) {
    if (!ws || !*ws) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws, -1, s.data(), n, nullptr, nullptr);
    return s;
}

}  // namespace

Result<std::vector<GpuAdapterInfo>> enumerate_d3d12_adapters() {
    ComPtr<IDXGIFactory6> factory;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return fail(std::format("CreateDXGIFactory2 failed: {:#x}", static_cast<uint32_t>(hr)));

    std::vector<GpuAdapterInfo> list;
    std::vector<uint64_t> seen_luids;
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;

        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);

        const uint64_t luid_key = (static_cast<uint64_t>(static_cast<uint32_t>(desc.AdapterLuid.HighPart)) << 32) |
                                  static_cast<uint64_t>(desc.AdapterLuid.LowPart);
        bool dup = false;
        for (uint64_t seen : seen_luids) {
            if (seen == luid_key) { dup = true; break; }
        }
        if (dup) continue;

        ComPtr<ID3D12Device> dev;
        if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev)))) {
            continue;
        }
        seen_luids.push_back(luid_key);

        GpuAdapterInfo info{};
        info.index = i;
        info.name = wide_to_utf8(desc.Description);
        info.vendor_id = desc.VendorId;
        info.device_id = desc.DeviceId;
        info.dedicated_vram_mb = desc.DedicatedVideoMemory / (1024ull * 1024ull);
        info.is_software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;

        D3D12_FEATURE_DATA_D3D12_OPTIONS1 opt1{};
        if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &opt1, sizeof(opt1)))) {
            info.wave_lane_count_min = opt1.WaveLaneCountMin;
            info.wave_lane_count_max = opt1.WaveLaneCountMax;
            info.supports_wave32 = (opt1.WaveLaneCountMin <= 32 && opt1.WaveLaneCountMax >= 32);
            info.supports_wave64 = (opt1.WaveLaneCountMin <= 64 && opt1.WaveLaneCountMax >= 64);
        }

        D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_7};
        if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm)))) {
            info.max_shader_model = static_cast<uint32_t>(sm.HighestShaderModel);
        } else {
            sm.HighestShaderModel = D3D_SHADER_MODEL_6_6;
            if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm)))) {
                info.max_shader_model = static_cast<uint32_t>(sm.HighestShaderModel);
            }
        }

        list.push_back(std::move(info));
    }
    return list;
}

Result<ComputeDispatchResult> run_compute_dxil_on_gpu(
    uint32_t adapter_index,
    const std::vector<uint8_t>& dxil,
    const std::vector<uint32_t>& initial_buffer_u32,
    const uint32_t root_constants[8],
    uint32_t groups_x) {
    if (dxil.empty() || initial_buffer_u32.empty()) {
        return fail("DXIL or buffer is empty");
    }

    ComPtr<IDXGIFactory6> factory;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return fail("CreateDXGIFactory2 failed");

    ComPtr<IDXGIAdapter1> adapter;
    hr = factory->EnumAdapters1(adapter_index, &adapter);
    if (FAILED(hr)) return fail(std::format("Adapter {} not found", adapter_index));

    DXGI_ADAPTER_DESC1 adesc{};
    adapter->GetDesc1(&adesc);

    ComPtr<ID3D12Device> dev;
    hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev));
    if (FAILED(hr)) return fail(std::format("D3D12CreateDevice failed: {:#x}", static_cast<uint32_t>(hr)));

    // Root signature: [0] 8 root 32-bit constants (b0), [1] root UAV (u0)
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.RegisterSpace = 0;
    params[0].Constants.Num32BitValues = 8;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[1].Descriptor.ShaderRegister = 0;
    params[1].Descriptor.RegisterSpace = 0;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rs_desc{};
    rs_desc.NumParameters = 2;
    rs_desc.pParameters = params;
    rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> rs_blob, rs_err;
    hr = D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &rs_err);
    if (FAILED(hr)) return fail("D3D12SerializeRootSignature failed");

    ComPtr<ID3D12RootSignature> root_sig;
    hr = dev->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(), IID_PPV_ARGS(&root_sig));
    if (FAILED(hr)) return fail("CreateRootSignature failed");

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = root_sig.Get();
    pso_desc.CS.pShaderBytecode = dxil.data();
    pso_desc.CS.BytecodeLength = dxil.size();

    ComPtr<ID3D12PipelineState> pso;
    hr = dev->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso));
    if (FAILED(hr)) {
        return fail(std::format("CreateComputePipelineState failed on {}: {:#x}",
                                           wide_to_utf8(adesc.Description), static_cast<uint32_t>(hr)));
    }

    D3D12_COMMAND_QUEUE_DESC qdesc{};
    qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    hr = dev->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&queue));
    if (FAILED(hr)) return fail("CreateCommandQueue failed");

    ComPtr<ID3D12CommandAllocator> alloc;
    hr = dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    if (FAILED(hr)) return fail("CreateCommandAllocator failed");

    ComPtr<ID3D12GraphicsCommandList> cmd;
    hr = dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), pso.Get(), IID_PPV_ARGS(&cmd));
    if (FAILED(hr)) return fail("CreateCommandList failed");

    const UINT64 byte_size = static_cast<UINT64>(initial_buffer_u32.size()) * sizeof(uint32_t);
    auto make_buf = [&](D3D12_HEAP_TYPE ht, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES st, ComPtr<ID3D12Resource>& out) -> HRESULT {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = ht;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = byte_size;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags = flags;
        return dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, st, nullptr, IID_PPV_ARGS(&out));
    };

    ComPtr<ID3D12Resource> upload_buf, gpu_buf, readback_buf;
    if (FAILED(make_buf(D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, upload_buf)) ||
        FAILED(make_buf(D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST, gpu_buf)) ||
        FAILED(make_buf(D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, readback_buf))) {
        return fail("Failed to allocate GPU compute buffers");
    }

    void* mapped = nullptr;
    if (FAILED(upload_buf->Map(0, nullptr, &mapped))) return fail("Upload Map failed");
    std::memcpy(mapped, initial_buffer_u32.data(), byte_size);
    upload_buf->Unmap(0, nullptr);

    cmd->CopyBufferRegion(gpu_buf.Get(), 0, upload_buf.Get(), 0, byte_size);

    auto barrier = [&](D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = gpu_buf.Get();
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = after;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(1, &b);
    };

    barrier(D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmd->SetComputeRootSignature(root_sig.Get());
    cmd->SetComputeRoot32BitConstants(0, 8, root_constants, 0);
    cmd->SetComputeRootUnorderedAccessView(1, gpu_buf->GetGPUVirtualAddress());
    cmd->Dispatch(groups_x, 1, 1);

    D3D12_RESOURCE_BARRIER uav_bar{};
    uav_bar.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav_bar.UAV.pResource = gpu_buf.Get();
    cmd->ResourceBarrier(1, &uav_bar);

    barrier(D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->CopyBufferRegion(readback_buf.Get(), 0, gpu_buf.Get(), 0, byte_size);
    if (FAILED(cmd->Close())) return fail("CommandList Close failed");

    ComPtr<ID3D12Fence> fence;
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
        return fail("CreateFence failed");
    }
    HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    const auto t0 = std::chrono::steady_clock::now();
    ID3D12CommandList* lists[] = {cmd.Get()};
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(fence.Get(), 1);
    if (fence->GetCompletedValue() < 1) {
        fence->SetEventOnCompletion(1, evt);
        WaitForSingleObject(evt, 10000);
    }
    const auto t1 = std::chrono::steady_clock::now();
    CloseHandle(evt);

    ComputeDispatchResult res{};
    res.adapter_name = wide_to_utf8(adesc.Description);
    res.gpu_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    res.output_u32.resize(initial_buffer_u32.size());

    D3D12_RANGE rr{0, static_cast<SIZE_T>(byte_size)};
    if (FAILED(readback_buf->Map(0, &rr, &mapped))) return fail("Readback Map failed");
    std::memcpy(res.output_u32.data(), mapped, byte_size);
    D3D12_RANGE wr{0, 0};
    readback_buf->Unmap(0, &wr);
    return res;
}

}  // namespace kestrel::gpu
