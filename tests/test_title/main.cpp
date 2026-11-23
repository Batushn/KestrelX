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

    constexpr uint32_t kRtW = 256;
    constexpr uint32_t kRtH = 256;

    D3D12_HEAP_PROPERTIES default_hp{};
    default_hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC tex_desc{};
    tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex_desc.Width = kRtW;
    tex_desc.Height = kRtH;
    tex_desc.DepthOrArraySize = 1;
    tex_desc.MipLevels = 1;
    tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    tex_desc.SampleDesc.Count = 1;
    tex_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE cv{};
    cv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    cv.Color[0] = 18.0f / 255.0f;
    cv.Color[1] = 26.0f / 255.0f;
    cv.Color[2] = 46.0f / 255.0f;
    cv.Color[3] = 1.0f;

    ID3D12Resource* rt = nullptr;
    if (FAILED(dev->CreateCommittedResource(&default_hp, D3D12_HEAP_FLAG_NONE, &tex_desc,
                                            D3D12_RESOURCE_STATE_RENDER_TARGET, &cv,
                                            __uuidof(ID3D12Resource), reinterpret_cast<void**>(&rt)))) {
        rtv_heap->Release(); cmd->Release(); alloc->Release(); queue->Release(); dev->Release();
        return 107;
    }

    // Build empty Root Signature for graphics pipeline
    D3D12_ROOT_SIGNATURE_DESC empty_rsd{};
    ID3DBlob* gfx_rs_blob = nullptr;
    ID3DBlob* gfx_rs_err = nullptr;
    ID3D12RootSignature* gfx_rs = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&empty_rsd, D3D_ROOT_SIGNATURE_VERSION_1, &gfx_rs_blob, &gfx_rs_err)) ||
        !gfx_rs_blob ||
        FAILED(dev->CreateRootSignature(0, gfx_rs_blob->GetBufferPointer(), gfx_rs_blob->GetBufferSize(),
                                        __uuidof(ID3D12RootSignature), reinterpret_cast<void**>(&gfx_rs)))) {
        if (gfx_rs_blob) gfx_rs_blob->Release();
        if (gfx_rs_err) gfx_rs_err->Release();
        return 116;
    }
    gfx_rs_blob->Release();
    if (gfx_rs_err) gfx_rs_err->Release();

    // Clean-room RDNA2 (GFX10.3) Vertex Shader ("KXRV"):
    //   v0 = SV_VertexID (0=Top Red, 1=Bottom-Right Green, 2=Bottom-Left Blue)
    auto enc_vmov = [](uint32_t vd, uint32_t src0) -> uint32_t {
        return (0x3Fu << 25) | ((vd & 0xFFu) << 17) | (1u << 9) | (src0 & 0x1FFu);
    };
    auto enc_vcmp_eq_u32 = [](uint32_t src0, uint32_t vsrc1) -> uint32_t {
        return (0x3Eu << 25) | (0xC2u << 17) | ((vsrc1 & 0xFFu) << 9) | (src0 & 0x1FFu);
    };
    auto enc_vcndmask = [](uint32_t vd, uint32_t src0, uint32_t vsrc1) -> uint32_t {
        return (1u << 25) | ((vd & 0xFFu) << 17) | ((vsrc1 & 0xFFu) << 9) | (src0 & 0x1FFu);
    };
    auto enc_exp_w0 = [](uint32_t target, uint32_t en, bool done) -> uint32_t {
        return (0x3Eu << 26) | (done ? (1u << 11) : 0u) | ((target & 0x3Fu) << 4) | (en & 0xFu);
    };
    auto enc_exp_w1 = [](uint32_t v0, uint32_t v1, uint32_t v2, uint32_t v3) -> uint32_t {
        return ((v3 & 0xFFu) << 24) | ((v2 & 0xFFu) << 16) | ((v1 & 0xFFu) << 8) | (v0 & 0xFFu);
    };

    const uint32_t kxrv_vs[] = {
        0x5652584Bu, 0u,  // "KXRV" + reserved
        // Vertex 0: pos=(0.0, 0.75, 0.0, 1.0), color=(1.0, 0.15, 0.25, 1.0)
        enc_vmov(1, 128),                       // v1 = 0.0
        enc_vmov(2, 255), 0x3F400000u,          // v2 = 0.75f
        enc_vmov(3, 128),                       // v3 = 0.0
        enc_vmov(4, 242),                       // v4 = 1.0
        enc_vmov(5, 242),                       // v5 = 1.0 (R)
        enc_vmov(6, 255), 0x3E19999Au,          // v6 = 0.15f (G)
        enc_vmov(7, 255), 0x3E800000u,          // v7 = 0.25f (B)
        enc_vmov(8, 242),                       // v8 = 1.0 (A)
        // If v0 == 1: pos=(0.75, -0.65, 0, 1), color=(0.15, 0.95, 0.35, 1.0)
        enc_vcmp_eq_u32(129, 0),
        enc_vmov(9, 255), 0x3F400000u, enc_vcndmask(1, 256 + 1, 9),
        enc_vmov(9, 255), 0xBF266666u, enc_vcndmask(2, 256 + 2, 9),
        enc_vmov(9, 255), 0x3E19999Au, enc_vcndmask(5, 256 + 5, 9),
        enc_vmov(9, 255), 0x3F733333u, enc_vcndmask(6, 256 + 6, 9),
        enc_vmov(9, 255), 0x3EB33333u, enc_vcndmask(7, 256 + 7, 9),
        // If v0 == 2: pos=(-0.75, -0.65, 0, 1), color=(0.20, 0.45, 1.0, 1.0)
        enc_vcmp_eq_u32(130, 0),
        enc_vmov(9, 255), 0xBF400000u, enc_vcndmask(1, 256 + 1, 9),
        enc_vmov(9, 255), 0xBF266666u, enc_vcndmask(2, 256 + 2, 9),
        enc_vmov(9, 255), 0x3E4CCCCDu, enc_vcndmask(5, 256 + 5, 9),
        enc_vmov(9, 255), 0x3EE66666u, enc_vcndmask(6, 256 + 6, 9),
        enc_vmov(9, 242),              enc_vcndmask(7, 256 + 7, 9),
        // Export pos0 and param0
        enc_exp_w0(12, 0xF, false), enc_exp_w1(1, 2, 3, 4),
        enc_exp_w0(32, 0xF, true),  enc_exp_w1(5, 6, 7, 8),
        (0x17Fu << 23) | (1u << 16),  // s_endpgm
    };

    // Clean-room RDNA2 (GFX10.3) Pixel Shader ("KXRP"): export interpolated param0 -> mrt0
    const uint32_t kxrp_ps[] = {
        0x5052584Bu, 0u,  // "KXRP" + reserved
        enc_exp_w0(0, 0xF, true), enc_exp_w1(0, 1, 2, 3),
        (0x17Fu << 23) | (1u << 16),  // s_endpgm
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC gpd{};
    gpd.pRootSignature = gfx_rs;
    gpd.VS = {kxrv_vs, sizeof(kxrv_vs)};
    gpd.PS = {kxrp_ps, sizeof(kxrp_ps)};
    gpd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    gpd.SampleMask = UINT_MAX;
    gpd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    gpd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    gpd.RasterizerState.DepthClipEnable = TRUE;
    gpd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    gpd.NumRenderTargets = 1;
    gpd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    gpd.SampleDesc.Count = 1;

    ID3D12PipelineState* gfx_pso1 = nullptr;
    ID3D12PipelineState* gfx_pso2 = nullptr;
    if (FAILED(dev->CreateGraphicsPipelineState(&gpd, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&gfx_pso1))) || !gfx_pso1) {
        gfx_rs->Release();
        return 117;
    }
    if (FAILED(dev->CreateGraphicsPipelineState(&gpd, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&gfx_pso2))) || !gfx_pso2) {
        gfx_pso1->Release();
        gfx_rs->Release();
        return 118;
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
        gfx_pso2->Release(); gfx_pso1->Release(); gfx_rs->Release();
        rt->Release(); rtv_heap->Release(); cmd->Release(); alloc->Release(); queue->Release(); dev->Release();
        return 108;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtv_h = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    dev->CreateRenderTargetView(rt, nullptr, rtv_h);

    D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(kRtW), static_cast<float>(kRtH), 0.0f, 1.0f};
    D3D12_RECT scissor{0, 0, static_cast<LONG>(kRtW), static_cast<LONG>(kRtH)};
    cmd->ClearRenderTargetView(rtv_h, cv.Color, 0, nullptr);
    cmd->SetGraphicsRootSignature(gfx_rs);
    cmd->SetPipelineState(gfx_pso1);
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &scissor);
    cmd->OMSetRenderTargets(1, &rtv_h, FALSE, nullptr);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(3, 1, 0, 0);

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

    D3D12_RESOURCE_BARRIER back_bar{};
    back_bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    back_bar.Transition.pResource = rt;
    back_bar.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    back_bar.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    back_bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd->ResourceBarrier(1, &back_bar);

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
        const auto* base = static_cast<const uint8_t*>(mapped);
        auto px_at = [&](uint32_t x, uint32_t y) -> const uint8_t* {
            return base + static_cast<size_t>(y) * fp.Footprint.RowPitch + x * 4u;
        };
        const uint8_t* bg = px_at(4, 4);
        const uint8_t* top = px_at(128, 48);      // Near red apex
        const uint8_t* br = px_at(200, 195);      // Near green bottom-right
        const uint8_t* bl = px_at(56, 195);       // Near blue bottom-left
        const uint8_t* mid = px_at(128, 136);     // Center interpolated

        if (bg[0] != 18 || bg[1] != 26 || bg[2] != 46 || bg[3] != 255) rc = 109;
        else if (!(top[0] > 180 && top[0] > top[1] + 80 && top[0] > top[2] + 80)) rc = 119;
        else if (!(br[1] > 150 && br[1] > br[0] + 50)) rc = 120;
        else if (!(bl[2] > 170 && bl[2] > bl[0] + 80)) rc = 121;
        else if (static_cast<int>(mid[0]) + mid[1] + mid[2] < 240) rc = 122;

        D3D12_RANGE wr{0, 0};
        readback->Unmap(0, &wr);
    } else {
        rc = 110;
    }

    // Exercise KxDeviceProxy::CreateComputePipelineState with a raw RDNA2 ("KXRN") compute shader!
    if (rc == 0) {
        D3D12_ROOT_PARAMETER rp[2]{};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rp[0].Constants.ShaderRegister = 0;
        rp[0].Constants.Num32BitValues = 8;
        rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        rp[1].Descriptor.ShaderRegister = 0;
        rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = 2;
        rsd.pParameters = rp;

        ID3DBlob* rs_blob = nullptr;
        ID3DBlob* rs_err = nullptr;
        ID3D12RootSignature* rs = nullptr;
        if (SUCCEEDED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &rs_err)) && rs_blob &&
            SUCCEEDED(dev->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
                                               __uuidof(ID3D12RootSignature), reinterpret_cast<void**>(&rs)))) {
            const uint32_t kxrn_shader[] = {
                0x4E52584Bu,                                              // "KXRN"
                64u,                                                      // numthreads(64,1,1)
                (26u << 25) | (1u << 17) | (0u << 9) | 130u,              // v_lshlrev_b32 v1, 2, v0
                (0x37u << 26) | (12u << 18) | (2u << 14),                 // global_load_dword w0
                (2u << 24) | (127u << 16) | (2u << 8) | 1u,               // global_load_dword w1 (v2, v1)
                (37u << 25) | (3u << 17) | (2u << 9) | (128u + 10u),      // v_add_nc_u32 v3, 10, v2
                (0x37u << 26) | (28u << 18) | (2u << 14),                 // global_store_dword w0
                (3u << 24) | (127u << 16) | (3u << 8) | 1u,               // global_store_dword w1 (v1, v3)
                (0x17Fu << 23) | (1u << 16),                              // s_endpgm
            };

            D3D12_COMPUTE_PIPELINE_STATE_DESC cpd{};
            cpd.pRootSignature = rs;
            cpd.CS.pShaderBytecode = kxrn_shader;
            cpd.CS.BytecodeLength = sizeof(kxrn_shader);

            ID3D12PipelineState* pso1 = nullptr;
            ID3D12PipelineState* pso2 = nullptr;
            if (FAILED(dev->CreateComputePipelineState(&cpd, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&pso1))) || !pso1) {
                rc = 111;
            } else if (FAILED(dev->CreateComputePipelineState(&cpd, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&pso2))) || !pso2) {
                rc = 112;
            }
            if (pso2) pso2->Release();
            if (pso1) pso1->Release();
            rs->Release();
        } else {
            rc = 113;
        }
        if (rs_blob) rs_blob->Release();
        if (rs_err) rs_err->Release();
    }

    if (rc == 0) {
        if (FAILED(D3D12PresentX(queue, 1, rt, nullptr))) rc = 114;
        if (FAILED(D3D12WaitFrameEventX(0, 0, nullptr))) rc = 115;
    }

    gfx_pso2->Release();
    gfx_pso1->Release();
    gfx_rs->Release();
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

    // 5. Verify Overlay VFS via kernelx!CreateFileA + CreateFileW + GetFileAttributesExW + DeleteFileW
    HANDLE hf = CreateFileA("GAME:\\data\\readonly.txt", GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf == INVALID_HANDLE_VALUE) return 8;
    char buf[64]{};
    DWORD nread = 0;
    ReadFile(hf, buf, sizeof(buf) - 1, &nread, nullptr);
    CloseHandle(hf);
    if (nread < 25 || buf[0] != 'K' || buf[1] != 'e' || buf[7] != 'X') return 9;

    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(L"GAME:\\data\\readonly.txt", GetFileExInfoStandard, &fad) || fad.nFileSizeLow < 25) return 18;

    HANDLE htmp = CreateFileW(L"T:\\scratch.tmp", GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (htmp == INVALID_HANDLE_VALUE) return 19;
    CloseHandle(htmp);
    if (!DeleteFileW(L"T:\\scratch.tmp")) return 20;

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

    // 7. Verify gameinput.dll HLE controller reading
    IGuestGameInput* gi = nullptr;
    if (FAILED(GameInputCreate(&gi)) || !gi) return 21;
    IGuestGameInputReading* reading = nullptr;
    if (SUCCEEDED(gi->GetCurrentReading(0x00010000u, nullptr, &reading)) && reading) {
        GuestGamepadState gs{};
        reading->GetGamepadState(&gs);
        reading->Release();
    }
    gi->Release();

    // 8. Verify xaudio2_9.dll HLE stereo PCM submission
    IGuestXAudio2* xa = nullptr;
    if (FAILED(XAudio2Create(&xa, 0, 0)) || !xa) return 22;
    IGuestMasteringVoice* mv = nullptr;
    if (FAILED(xa->CreateMasteringVoice(&mv, 2, 48000)) || !mv) return 23;
    GuestWaveFormat wf{3, 2, 48000, 32};
    IGuestSourceVoice* sv = nullptr;
    if (FAILED(xa->CreateSourceVoice(&sv, &wf)) || !sv) return 24;
    float pcm[128]{};
    for (int i = 0; i < 64; ++i) {
        pcm[i * 2 + 0] = (i & 1) ? 0.25f : -0.25f;
        pcm[i * 2 + 1] = pcm[i * 2 + 0];
    }
    GuestAudioBuffer ab{0, sizeof(pcm), reinterpret_cast<const uint8_t*>(pcm)};
    if (FAILED(sv->Start(0)) || FAILED(sv->SubmitSourceBuffer(&ab))) return 25;
    sv->DestroyVoice();
    mv->DestroyVoice();
    xa->Release();

    // 9. Verify d3d12_xs.dll KxDeviceProxy on real D3D12 GPU (RDNA2 VS/PS RGB triangle + CS PSO + PresentX)
    return test_d3d12x_render_and_readback();
}
