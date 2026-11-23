#pragma once

#include "kestrel/common/types.hpp"
#include "kestrel/gpu/dxc_compiler.hpp"
#include "kestrel/gpu/rdna2_recomp.hpp"

#include <d3d12.h>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace kestrel::gpu {

enum class WorkGraphNodeKind : uint8_t {
    Broadcasting = 0,  // Launches a grid of thread groups per dispatch record
    Coalescing = 1,    // Coalesces multiple input records into a single thread group
    Thread = 2,        // Launches one thread per input record
};

struct WorkGraphNodeDesc {
    std::string name;
    uint32_t node_index = 0;
    WorkGraphNodeKind kind = WorkGraphNodeKind::Broadcasting;
    uint32_t group_size_x = 64;
    uint32_t group_size_y = 1;
    uint32_t group_size_z = 1;
    uint32_t record_stride_bytes = 0;    // Must be 0 or 16-byte aligned
    uint32_t max_output_records = 0;     // Maximum records produced by this node
    std::vector<uint8_t> kxrn_bytecode;  // Optional "KXRN" container ("KXRN" + u32 group_x + GFX10.3 dwords)
    std::vector<uint32_t> rdna2_dwords;  // Optional raw GFX10.3 compute microcode dwords
};

struct WorkGraphEdge {
    uint32_t producer_node = 0;
    uint32_t consumer_node = 0;
    uint32_t dispatch_args_byte_offset = 0;   // Byte offset in u0 backing buffer for D3D12_DISPATCH_ARGUMENTS (4B aligned)
    uint32_t record_buffer_byte_offset = 0;   // Byte offset in u0 backing buffer for producer->consumer records (16B aligned)
    uint32_t max_dispatch_count = 1;          // Max indirect dispatches along this edge (>= 1)
};

// Pack raw GFX10.3 compute dwords into a clean-room KestrelX "KXRN" container.
std::vector<uint8_t> pack_kxrn_compute_bytecode(uint32_t group_size_x, const std::vector<uint32_t>& dwords);

// Extract raw GFX10.3 instruction dwords from either kxrn_bytecode or rdna2_dwords.
Result<std::vector<uint32_t>> extract_node_rdna2_dwords(
    const WorkGraphNodeDesc& node,
    uint32_t* out_kxrn_group_x = nullptr);

struct WorkGraphProgram {
    static constexpr uint32_t kMaxNodes = 256;
    static constexpr uint64_t kMaxTotalBackingBytes = 256ull * 1024ull * 1024ull;

    std::string name;
    std::vector<WorkGraphNodeDesc> nodes;
    std::vector<WorkGraphEdge> edges;

    uint32_t add_node(WorkGraphNodeDesc desc);
    void add_edge(WorkGraphEdge edge);
    void add_edge(
        uint32_t producer_node,
        uint32_t consumer_node,
        uint32_t dispatch_args_byte_offset = 0,
        uint32_t record_buffer_byte_offset = 0,
        uint32_t max_dispatch_count = 1);

    Result<void> validate() const;
    Result<std::vector<uint32_t>> topological_order() const;
    uint64_t estimated_record_backing_bytes() const;
};

struct RecordAllocation {
    uint64_t offset = 0;
    uint64_t gpu_va = 0;
    uint64_t size_bytes = 0;
    uint64_t aligned_size_bytes = 0;
    bool wrapped = false;
};

struct RecordRingMetrics {
    uint64_t capacity_bytes = 0;
    uint64_t used_bytes = 0;
    uint64_t peak_used_bytes = 0;
    uint64_t free_bytes = 0;
    uint64_t wasted_tail_bytes = 0;
    uint64_t allocation_count = 0;
    uint64_t wrap_count = 0;
    uint64_t failed_allocations = 0;
    double utilization = 0.0;
};

// Deterministic 16-byte aligned CPU/GPU backing record ring buffer allocator.
class RecordRingAllocator {
public:
    static constexpr uint64_t kAlignment = 16;

    explicit RecordRingAllocator(uint64_t capacity_bytes, uint64_t base_gpu_va = 0);

    static constexpr uint64_t align_up(uint64_t bytes) noexcept {
        return (bytes + (kAlignment - 1ull)) & ~(kAlignment - 1ull);
    }

    Result<RecordAllocation> allocate(uint64_t size_bytes);
    Result<void> release(const RecordAllocation& alloc);
    void reset();

    uint64_t capacity_bytes() const noexcept { return capacity_bytes_; }
    uint64_t base_gpu_va() const noexcept { return base_gpu_va_; }
    void set_base_gpu_va(uint64_t gpu_va) noexcept { base_gpu_va_ = gpu_va; }
    RecordRingMetrics metrics() const noexcept;

private:
    uint64_t capacity_bytes_ = 0;
    uint64_t base_gpu_va_ = 0;
    uint64_t head_ = 0;
    uint64_t tail_ = 0;
    uint64_t used_bytes_ = 0;
    uint64_t wasted_tail_bytes_ = 0;
    uint64_t peak_used_bytes_ = 0;
    uint64_t allocation_count_ = 0;
    uint64_t wrap_count_ = 0;
    uint64_t failed_allocations_ = 0;
};

enum class WorkGraphExecutionMode : uint8_t {
    FallbackExecuteIndirect = 0,
    NativeWorkGraphs = 1,
};

struct WorkGraphCapability {
    bool sdk_headers_support_native = false;
    bool driver_reports_work_graphs = false;
    uint32_t driver_work_graphs_tier = 0;
    uint32_t driver_execute_indirect_tier = 10;
    bool supports_sm_6_8 = false;
    uint32_t highest_shader_model = 0;
    bool supports_wave64 = false;
    WorkGraphExecutionMode selected_mode = WorkGraphExecutionMode::FallbackExecuteIndirect;
};

struct WorkGraphDispatchDesc {
    uint32_t entry_groups_x = 1;
    uint32_t entry_groups_y = 1;
    uint32_t entry_groups_z = 1;
    std::vector<uint32_t> initial_buffer_u32;
    std::vector<std::array<uint32_t, 8>> node_root_constants;
};

struct WorkGraphExecutionResult {
    WorkGraphCapability capability;
    std::vector<uint32_t> output_u32;
    std::vector<uint32_t> executed_node_order;
    uint32_t direct_dispatches = 0;
    uint32_t indirect_dispatches = 0;
    uint32_t uav_barriers = 0;
    RecordRingMetrics allocator_metrics;
    double gpu_ms = 0.0;
};

// Clean-room D3D12 Work Graphs & Device-Generated Dispatch Node Executor.
// Compiles RDNA2 compute nodes to SM 6.6 DXIL PSOs and executes producer -> consumer
// DAGs using GPU-generated D3D12_DISPATCH_ARGUMENTS, UAV barriers, and ExecuteIndirect.
class WorkGraphExecutor {
public:
    WorkGraphExecutor();
    ~WorkGraphExecutor();
    WorkGraphExecutor(const WorkGraphExecutor&) = delete;
    WorkGraphExecutor& operator=(const WorkGraphExecutor&) = delete;
    WorkGraphExecutor(WorkGraphExecutor&&) noexcept;
    WorkGraphExecutor& operator=(WorkGraphExecutor&&) noexcept;

    // Probe native Work Graph capability via D3D12 runtime feature queries and SDK headers.
    static WorkGraphCapability CheckWorkGraphSupport(ID3D12Device* device);

    // Validate the graph, recompile every node's RDNA2 microcode (decode -> lift -> HLSL -> DXC cs_6_6),
    // create D3D12 compute PSOs, compute root signature, and ExecuteIndirect dispatch command signature.
    Result<void> compile(
        ID3D12Device* device,
        const WorkGraphProgram& program,
        DxcCompiler& dxc,
        WaveMode forced_wave_mode = WaveMode::Wave32,
        bool auto_select_wave64 = true);

    // Record Work Graph node dispatches, UAV barriers, and GPU-to-GPU ExecuteIndirect argument copies
    // onto an open graphics/compute command list.
    // `backing_uav_buffer` must enter in D3D12_RESOURCE_STATE_UNORDERED_ACCESS (and exits in the same state).
    // `indirect_args_buffer` must enter in D3D12_RESOURCE_STATE_COPY_DEST with capacity >= max(1, edges.size()) * 16.
    Result<void> record_commands(
        ID3D12GraphicsCommandList* cmd,
        ID3D12Resource* backing_uav_buffer,
        ID3D12Resource* indirect_args_buffer,
        uint64_t backing_buffer_size_bytes,
        const WorkGraphDispatchDesc& desc,
        uint32_t* out_direct_dispatches = nullptr,
        uint32_t* out_indirect_dispatches = nullptr,
        uint32_t* out_uav_barriers = nullptr);

    // Upload `desc.initial_buffer_u32`, execute the compiled Work Graph end-to-end on the GPU,
    // synchronize via D3D12 fence, and read back the updated GPU buffer.
    Result<WorkGraphExecutionResult> execute(
        const WorkGraphDispatchDesc& desc,
        ID3D12CommandQueue* external_queue = nullptr);

    bool is_compiled() const noexcept;
    const WorkGraphCapability& capability() const noexcept;
    const WorkGraphProgram& program() const noexcept;
    const std::vector<uint32_t>& topological_order() const noexcept;
    const std::vector<std::vector<uint8_t>>& compiled_node_dxil() const noexcept;
    const RecordRingAllocator& record_allocator() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace kestrel::gpu
