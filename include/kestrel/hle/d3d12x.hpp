#pragma once

#include "kestrel/hle/registry.hpp"
#include <d3d12.h>
#include <atomic>
#include <cstdint>
#include <filesystem>

namespace kestrel::hle {

struct D3D12XStats {
    uint64_t devices_created = 0;
    uint64_t queues_created = 0;
    uint64_t cmd_lists_created = 0;
    uint64_t resources_created = 0;
    uint64_t zero_memory_calls = 0;
    uint64_t present_calls = 0;
    uint64_t pso_recompiled = 0;
    uint64_t pso_cache_hits = 0;
    uint64_t gfx_pso_recompiled = 0;
    uint64_t gfx_pso_cache_hits = 0;
    uint64_t stream_pso_recompiled = 0;
    uint64_t disk_cache_hits = 0;
    uint64_t disk_cache_misses = 0;
    uint64_t device_qi_upgrades = 0;
    uint64_t draw_calls = 0;
    uint64_t dispatch_calls = 0;
    uint64_t barrier_calls = 0;
    uint64_t pso_binds = 0;
    uint64_t compute_queues_created = 0;
    uint64_t inter_queue_fences = 0;
};

// Pure COM wrapper around ID3D12GraphicsCommandList for draw/dispatch/barrier telemetry.
class KxGraphicsCommandListProxy final : public ID3D12GraphicsCommandList {
public:
    explicit KxGraphicsCommandListProxy(ID3D12GraphicsCommandList* inner);
    ~KxGraphicsCommandListProxy();

    ID3D12GraphicsCommandList* inner() const { return inner_; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override;
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR Name) override;
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppvDevice) override;
    D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE GetType() override;

    HRESULT STDMETHODCALLTYPE Close() override;
    HRESULT STDMETHODCALLTYPE Reset(ID3D12CommandAllocator* pAllocator, ID3D12PipelineState* pInitialState) override;
    void STDMETHODCALLTYPE ClearState(ID3D12PipelineState* pPipelineState) override;
    void STDMETHODCALLTYPE DrawInstanced(UINT VertexCountPerInstance, UINT InstanceCount, UINT StartVertexLocation, UINT StartInstanceLocation) override;
    void STDMETHODCALLTYPE DrawIndexedInstanced(UINT IndexCountPerInstance, UINT InstanceCount, UINT StartIndexLocation, INT BaseVertexLocation, UINT StartInstanceLocation) override;
    void STDMETHODCALLTYPE Dispatch(UINT ThreadGroupCountX, UINT ThreadGroupCountY, UINT ThreadGroupCountZ) override;
    void STDMETHODCALLTYPE CopyBufferRegion(ID3D12Resource* pDstBuffer, UINT64 DstOffset, ID3D12Resource* pSrcBuffer, UINT64 SrcOffset, UINT64 NumBytes) override;
    void STDMETHODCALLTYPE CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION* pDst, UINT DstX, UINT DstY, UINT DstZ, const D3D12_TEXTURE_COPY_LOCATION* pSrc, const D3D12_BOX* pSrcBox) override;
    void STDMETHODCALLTYPE CopyResource(ID3D12Resource* pDstResource, ID3D12Resource* pSrcResource) override;
    void STDMETHODCALLTYPE CopyTiles(ID3D12Resource* pTiledResource, const D3D12_TILED_RESOURCE_COORDINATE* pTileRegionStartCoordinate, const D3D12_TILE_REGION_SIZE* pTileRegionSize, ID3D12Resource* pBuffer, UINT64 BufferStartOffsetInBytes, D3D12_TILE_COPY_FLAGS Flags) override;
    void STDMETHODCALLTYPE ResolveSubresource(ID3D12Resource* pDstResource, UINT DstSubresource, ID3D12Resource* pSrcResource, UINT SrcSubresource, DXGI_FORMAT Format) override;
    void STDMETHODCALLTYPE IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY PrimitiveTopology) override;
    void STDMETHODCALLTYPE RSSetViewports(UINT NumViewports, const D3D12_VIEWPORT* pViewports) override;
    void STDMETHODCALLTYPE RSSetScissorRects(UINT NumRects, const D3D12_RECT* pRects) override;
    void STDMETHODCALLTYPE OMSetBlendFactor(const FLOAT BlendFactor[4]) override;
    void STDMETHODCALLTYPE OMSetStencilRef(UINT StencilRef) override;
    void STDMETHODCALLTYPE SetPipelineState(ID3D12PipelineState* pPipelineState) override;
    void STDMETHODCALLTYPE ResourceBarrier(UINT NumBarriers, const D3D12_RESOURCE_BARRIER* pBarriers) override;
    void STDMETHODCALLTYPE ExecuteBundle(ID3D12GraphicsCommandList* pCommandList) override;
    void STDMETHODCALLTYPE SetDescriptorHeaps(UINT NumDescriptorHeaps, ID3D12DescriptorHeap* const* ppDescriptorHeaps) override;
    void STDMETHODCALLTYPE SetComputeRootSignature(ID3D12RootSignature* pRootSignature) override;
    void STDMETHODCALLTYPE SetGraphicsRootSignature(ID3D12RootSignature* pRootSignature) override;
    void STDMETHODCALLTYPE SetComputeRootDescriptorTable(UINT RootParameterIndex, D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor) override;
    void STDMETHODCALLTYPE SetGraphicsRootDescriptorTable(UINT RootParameterIndex, D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor) override;
    void STDMETHODCALLTYPE SetComputeRoot32BitConstant(UINT RootParameterIndex, UINT SrcData, UINT DestOffsetIn32BitValues) override;
    void STDMETHODCALLTYPE SetGraphicsRoot32BitConstant(UINT RootParameterIndex, UINT SrcData, UINT DestOffsetIn32BitValues) override;
    void STDMETHODCALLTYPE SetComputeRoot32BitConstants(UINT RootParameterIndex, UINT Num32BitValuesToSet, const void* pSrcData, UINT DestOffsetIn32BitValues) override;
    void STDMETHODCALLTYPE SetGraphicsRoot32BitConstants(UINT RootParameterIndex, UINT Num32BitValuesToSet, const void* pSrcData, UINT DestOffsetIn32BitValues) override;
    void STDMETHODCALLTYPE SetComputeRootConstantBufferView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override;
    void STDMETHODCALLTYPE SetGraphicsRootConstantBufferView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override;
    void STDMETHODCALLTYPE SetComputeRootShaderResourceView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override;
    void STDMETHODCALLTYPE SetGraphicsRootShaderResourceView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override;
    void STDMETHODCALLTYPE SetComputeRootUnorderedAccessView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override;
    void STDMETHODCALLTYPE SetGraphicsRootUnorderedAccessView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override;
    void STDMETHODCALLTYPE IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW* pView) override;
    void STDMETHODCALLTYPE IASetVertexBuffers(UINT StartSlot, UINT NumViews, const D3D12_VERTEX_BUFFER_VIEW* pViews) override;
    void STDMETHODCALLTYPE SOSetTargets(UINT StartSlot, UINT NumViews, const D3D12_STREAM_OUTPUT_BUFFER_VIEW* pViews) override;
    void STDMETHODCALLTYPE OMSetRenderTargets(UINT NumRenderTargetDescriptors, const D3D12_CPU_DESCRIPTOR_HANDLE* pRenderTargetDescriptors, BOOL RTsSingleHandleToDescriptorRange, const D3D12_CPU_DESCRIPTOR_HANDLE* pDepthStencilDescriptor) override;
    void STDMETHODCALLTYPE ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView, D3D12_CLEAR_FLAGS ClearFlags, FLOAT Depth, UINT8 Stencil, UINT NumRects, const D3D12_RECT* pRects) override;
    void STDMETHODCALLTYPE ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE RenderTargetView, const FLOAT ColorRGBA[4], UINT NumRects, const D3D12_RECT* pRects) override;
    void STDMETHODCALLTYPE ClearUnorderedAccessViewUint(D3D12_GPU_DESCRIPTOR_HANDLE ViewGPUHandleInCurrentHeap, D3D12_CPU_DESCRIPTOR_HANDLE ViewCPUHandle, ID3D12Resource* pResource, const UINT Values[4], UINT NumRects, const D3D12_RECT* pRects) override;
    void STDMETHODCALLTYPE ClearUnorderedAccessViewFloat(D3D12_GPU_DESCRIPTOR_HANDLE ViewGPUHandleInCurrentHeap, D3D12_CPU_DESCRIPTOR_HANDLE ViewCPUHandle, ID3D12Resource* pResource, const FLOAT Values[4], UINT NumRects, const D3D12_RECT* pRects) override;
    void STDMETHODCALLTYPE DiscardResource(ID3D12Resource* pResource, const D3D12_DISCARD_REGION* pRegion) override;
    void STDMETHODCALLTYPE BeginQuery(ID3D12QueryHeap* pQueryHeap, D3D12_QUERY_TYPE Type, UINT Index) override;
    void STDMETHODCALLTYPE EndQuery(ID3D12QueryHeap* pQueryHeap, D3D12_QUERY_TYPE Type, UINT Index) override;
    void STDMETHODCALLTYPE ResolveQueryData(ID3D12QueryHeap* pQueryHeap, D3D12_QUERY_TYPE Type, UINT StartIndex, UINT NumElements, ID3D12Resource* pDestinationBuffer, UINT64 AlignedDestinationBufferOffset) override;
    void STDMETHODCALLTYPE SetPredication(ID3D12Resource* pBuffer, UINT64 AlignedBufferOffset, D3D12_PREDICATION_OP Operation) override;
    void STDMETHODCALLTYPE SetMarker(UINT Metadata, const void* pData, UINT Size) override;
    void STDMETHODCALLTYPE BeginEvent(UINT Metadata, const void* pData, UINT Size) override;
    void STDMETHODCALLTYPE EndEvent() override;
    void STDMETHODCALLTYPE ExecuteIndirect(ID3D12CommandSignature* pCommandSignature, UINT MaxCommandCount, ID3D12Resource* pArgumentBuffer, UINT64 ArgumentBufferOffset, ID3D12Resource* pCountBuffer, UINT64 CountBufferOffset) override;

private:
    std::atomic<ULONG> ref_{1};
    ID3D12GraphicsCommandList* inner_ = nullptr;
};

// Pure COM wrapper around ID3D12CommandQueue (zero driver vtable patching).
class KxCommandQueueProxy final : public ID3D12CommandQueue {
public:
    explicit KxCommandQueueProxy(ID3D12CommandQueue* inner);
    ~KxCommandQueueProxy();

    ID3D12CommandQueue* inner() const { return inner_; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override;
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR Name) override;
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppvDevice) override;

    void STDMETHODCALLTYPE UpdateTileMappings(ID3D12Resource* pResource, UINT NumResourceRegions, const D3D12_TILED_RESOURCE_COORDINATE* pResourceRegionStartCoordinates, const D3D12_TILE_REGION_SIZE* pResourceRegionSizes, ID3D12Heap* pHeap, UINT NumRanges, const D3D12_TILE_RANGE_FLAGS* pRangeFlags, const UINT* pHeapRangeStartOffsets, const UINT* pRangeTileCounts, D3D12_TILE_MAPPING_FLAGS Flags) override;
    void STDMETHODCALLTYPE CopyTileMappings(ID3D12Resource* pDstResource, const D3D12_TILED_RESOURCE_COORDINATE* pDstRegionStartCoordinate, ID3D12Resource* pSrcResource, const D3D12_TILED_RESOURCE_COORDINATE* pSrcRegionStartCoordinate, const D3D12_TILE_REGION_SIZE* pRegionSize, D3D12_TILE_MAPPING_FLAGS Flags) override;
    void STDMETHODCALLTYPE ExecuteCommandLists(UINT NumCommandLists, ID3D12CommandList* const* ppCommandLists) override;
    void STDMETHODCALLTYPE SetMarker(UINT Metadata, const void* pData, UINT Size) override;
    void STDMETHODCALLTYPE BeginEvent(UINT Metadata, const void* pData, UINT Size) override;
    void STDMETHODCALLTYPE EndEvent() override;
    HRESULT STDMETHODCALLTYPE Signal(ID3D12Fence* pFence, UINT64 Value) override;
    HRESULT STDMETHODCALLTYPE Wait(ID3D12Fence* pFence, UINT64 Value) override;
    HRESULT STDMETHODCALLTYPE GetTimestampFrequency(UINT64* pFrequency) override;
    HRESULT STDMETHODCALLTYPE GetClockCalibration(UINT64* pGpuTimestamp, UINT64* pCpuTimestamp) override;
    D3D12_COMMAND_QUEUE_DESC STDMETHODCALLTYPE GetDesc() override;

private:
    std::atomic<ULONG> ref_{1};
    ID3D12CommandQueue* inner_ = nullptr;
};

// Pure COM wrapper around a real PC ID3D12Device / ID3D12Device1 / ID3D12Device2
// with clean QueryInterface forwarding up to ID3D12Device10.
// Automatically intercepts native RDNA2 containers ("KXRN"/"KXRV"/"KXRP") in
// CreateComputePipelineState, CreateGraphicsPipelineState, and CreatePipelineState
// (D3D12_PIPELINE_STATE_STREAM_DESC), recompiling to SM 6.6 DXIL with RAM + on-disk cache.
class KxDeviceProxy final : public ID3D12Device2 {
public:
    explicit KxDeviceProxy(ID3D12Device* inner);
    ~KxDeviceProxy();

    ID3D12Device* inner() const { return inner_; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override;
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR Name) override;

    UINT STDMETHODCALLTYPE GetNodeCount() override;
    HRESULT STDMETHODCALLTYPE CreateCommandQueue(const D3D12_COMMAND_QUEUE_DESC* pDesc, REFIID riid, void** ppCommandQueue) override;
    HRESULT STDMETHODCALLTYPE CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE type, REFIID riid, void** ppCommandAllocator) override;
    HRESULT STDMETHODCALLTYPE CreateGraphicsPipelineState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pDesc, REFIID riid, void** ppPipelineState) override;
    HRESULT STDMETHODCALLTYPE CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC* pDesc, REFIID riid, void** ppPipelineState) override;
    HRESULT STDMETHODCALLTYPE CreateCommandList(UINT nodeMask, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator* pCommandAllocator, ID3D12PipelineState* pInitialState, REFIID riid, void** ppCommandList) override;
    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(D3D12_FEATURE Feature, void* pFeatureSupportData, UINT FeatureSupportDataSize) override;
    HRESULT STDMETHODCALLTYPE CreateDescriptorHeap(const D3D12_DESCRIPTOR_HEAP_DESC* pDescriptorHeapDesc, REFIID riid, void** ppvHeap) override;
    UINT STDMETHODCALLTYPE GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapType) override;
    HRESULT STDMETHODCALLTYPE CreateRootSignature(UINT nodeMask, const void* pBlobWithRootSignature, SIZE_T blobLengthInBytes, REFIID riid, void** ppvRootSignature) override;
    void STDMETHODCALLTYPE CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override;
    void STDMETHODCALLTYPE CreateShaderResourceView(ID3D12Resource* pResource, const D3D12_SHADER_RESOURCE_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override;
    void STDMETHODCALLTYPE CreateUnorderedAccessView(ID3D12Resource* pResource, ID3D12Resource* pCounterResource, const D3D12_UNORDERED_ACCESS_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override;
    void STDMETHODCALLTYPE CreateRenderTargetView(ID3D12Resource* pResource, const D3D12_RENDER_TARGET_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override;
    void STDMETHODCALLTYPE CreateDepthStencilView(ID3D12Resource* pResource, const D3D12_DEPTH_STENCIL_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override;
    void STDMETHODCALLTYPE CreateSampler(const D3D12_SAMPLER_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override;
    void STDMETHODCALLTYPE CopyDescriptors(UINT NumDestDescriptorRanges, const D3D12_CPU_DESCRIPTOR_HANDLE* pDestDescriptorRangeStarts, const UINT* pDestDescriptorRangeSizes, UINT NumSrcDescriptorRanges, const D3D12_CPU_DESCRIPTOR_HANDLE* pSrcDescriptorRangeStarts, const UINT* pSrcDescriptorRangeSizes, D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType) override;
    void STDMETHODCALLTYPE CopyDescriptorsSimple(UINT NumDescriptors, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptorRangeStart, D3D12_CPU_DESCRIPTOR_HANDLE SrcDescriptorRangeStart, D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType) override;
    D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE GetResourceAllocationInfo(UINT visibleMask, UINT numResourceDescs, const D3D12_RESOURCE_DESC* pResourceDescs) override;
    D3D12_HEAP_PROPERTIES STDMETHODCALLTYPE GetCustomHeapProperties(UINT nodeMask, D3D12_HEAP_TYPE heapType) override;
    HRESULT STDMETHODCALLTYPE CreateCommittedResource(const D3D12_HEAP_PROPERTIES* pHeapProperties, D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialResourceState, const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riidResource, void** ppvResource) override;
    HRESULT STDMETHODCALLTYPE CreateHeap(const D3D12_HEAP_DESC* pDesc, REFIID riid, void** ppvHeap) override;
    HRESULT STDMETHODCALLTYPE CreatePlacedResource(ID3D12Heap* pHeap, UINT64 HeapOffset, const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riid, void** ppvResource) override;
    HRESULT STDMETHODCALLTYPE CreateReservedResource(const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riid, void** ppvResource) override;
    HRESULT STDMETHODCALLTYPE CreateSharedHandle(ID3D12DeviceChild* pObject, const SECURITY_ATTRIBUTES* pAttributes, DWORD Access, LPCWSTR Name, HANDLE* pHandle) override;
    HRESULT STDMETHODCALLTYPE OpenSharedHandle(HANDLE NTHandle, REFIID riid, void** ppvObj) override;
    HRESULT STDMETHODCALLTYPE OpenSharedHandleByName(LPCWSTR Name, DWORD Access, HANDLE* pNTHandle) override;
    HRESULT STDMETHODCALLTYPE MakeResident(UINT NumObjects, ID3D12Pageable* const* ppObjects) override;
    HRESULT STDMETHODCALLTYPE Evict(UINT NumObjects, ID3D12Pageable* const* ppObjects) override;
    HRESULT STDMETHODCALLTYPE CreateFence(UINT64 InitialValue, D3D12_FENCE_FLAGS Flags, REFIID riid, void** ppFence) override;
    HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() override;
    void STDMETHODCALLTYPE GetCopyableFootprints(const D3D12_RESOURCE_DESC* pResourceDesc, UINT FirstSubresource, UINT NumSubresources, UINT64 BaseOffset, D3D12_PLACED_SUBRESOURCE_FOOTPRINT* pLayouts, UINT* pNumRows, UINT64* pRowSizeInBytes, UINT64* pTotalBytes) override;
    HRESULT STDMETHODCALLTYPE CreateQueryHeap(const D3D12_QUERY_HEAP_DESC* pDesc, REFIID riid, void** ppvHeap) override;
    HRESULT STDMETHODCALLTYPE SetStablePowerState(BOOL Enable) override;
    HRESULT STDMETHODCALLTYPE CreateCommandSignature(const D3D12_COMMAND_SIGNATURE_DESC* pDesc, ID3D12RootSignature* pRootSignature, REFIID riid, void** ppvCommandSignature) override;
    void STDMETHODCALLTYPE GetResourceTiling(ID3D12Resource* pTiledResource, UINT* pNumTilesForEntireResource, D3D12_PACKED_MIP_INFO* pPackedMipDesc, D3D12_TILE_SHAPE* pStandardTileShapeForNonPackedMips, UINT* pNumSubresourceTilings, UINT FirstSubresourceTilingToGet, D3D12_SUBRESOURCE_TILING* pSubresourceTilingsForNonPackedMips) override;
    LUID STDMETHODCALLTYPE GetAdapterLuid() override;

    // ID3D12Device1
    HRESULT STDMETHODCALLTYPE CreatePipelineLibrary(const void* pLibraryBlob, SIZE_T BlobLength, REFIID riid, void** ppPipelineLibrary) override;
    HRESULT STDMETHODCALLTYPE SetEventOnMultipleFenceCompletion(ID3D12Fence* const* ppFences, const UINT64* pFenceValues, UINT NumFences, D3D12_MULTIPLE_FENCE_WAIT_FLAGS Flags, HANDLE hEvent) override;
    HRESULT STDMETHODCALLTYPE SetResidencyPriority(UINT NumObjects, ID3D12Pageable* const* ppObjects, const D3D12_RESIDENCY_PRIORITY* pPriorities) override;

    // ID3D12Device2
    HRESULT STDMETHODCALLTYPE CreatePipelineState(const D3D12_PIPELINE_STATE_STREAM_DESC* pDesc, REFIID riid, void** ppPipelineState) override;

private:
    std::atomic<ULONG> ref_{1};
    ID3D12Device* inner_ = nullptr;
    ID3D12Device1* inner1_ = nullptr;
    ID3D12Device2* inner2_ = nullptr;
    bool supports_wave64_ = false;
};

void set_d3d12x_preferred_adapter(int adapter_index);
void set_d3d12x_windowed(bool enabled, uint32_t width = 640, uint32_t height = 640);
void set_d3d12x_capture_path(const std::filesystem::path& bmp_path);
void set_d3d12x_shader_cache_dir(const std::filesystem::path& cache_dir);
void clear_d3d12x_memory_shader_cache();
void shutdown_d3d12x_window();
void register_d3d12x(Registry& reg);
D3D12XStats d3d12x_stats();
void reset_d3d12x_stats();

}  // namespace kestrel::hle
