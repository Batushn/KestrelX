#include "kestrel/gpu/work_graph.hpp"

#include "kestrel/gpu/ir.hpp"
#include "kestrel/gpu/rdna2_isa.hpp"
#include "kestrel/hle/d3d12x.hpp"

#include <Windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>
#include <functional>
#include <queue>
#include <string_view>
#include <unordered_set>

using Microsoft::WRL::ComPtr;

namespace kestrel::gpu {

namespace {

// Clean-room runtime feature probe constants matching public DirectX 12 specs.
// Defined locally so KestrelX compiles cleanly against Windows SDK 10.0.22621.0
// without requiring newer Agility SDK headers.
constexpr UINT kD3D12FeatureOptions21 = 49;
constexpr UINT kD3DShaderModel68 = 0x68;
constexpr uint32_t kWorkGraphsTier1_0 = 10;

struct KxD3D12FeatureDataOptions21 {
    UINT work_graphs_tier = 0;
    UINT execute_indirect_tier = 10;
    BOOL sample_cmp_gradient_and_bias_supported = FALSE;
    BOOL extended_command_info_supported = FALSE;
};

constexpr uint64_t kIndirectSlotStrideBytes = 16;

uint64_t pack_edge_key(uint32_t producer, uint32_t consumer) noexcept {
    return (static_cast<uint64_t>(producer) << 32) | static_cast<uint64_t>(consumer);
}

}  // namespace

// ---- Bytecode container helpers ----

std::vector<uint8_t> pack_kxrn_compute_bytecode(uint32_t group_size_x, const std::vector<uint32_t>& dwords) {
    std::vector<uint8_t> out(8 + dwords.size() * sizeof(uint32_t));
    out[0] = 'K';
    out[1] = 'X';
    out[2] = 'R';
    out[3] = 'N';
    std::memcpy(out.data() + 4, &group_size_x, sizeof(uint32_t));
    if (!dwords.empty()) {
        std::memcpy(out.data() + 8, dwords.data(), dwords.size() * sizeof(uint32_t));
    }
    return out;
}

Result<std::vector<uint32_t>> extract_node_rdna2_dwords(
    const WorkGraphNodeDesc& node,
    uint32_t* out_kxrn_group_x) {
    if (!node.kxrn_bytecode.empty()) {
        if (node.kxrn_bytecode.size() < 12) {
            return fail(std::format("Node '{}': KXRN bytecode too short ({} bytes)",
                                    node.name, node.kxrn_bytecode.size()));
        }
        if (std::memcmp(node.kxrn_bytecode.data(), "KXRN", 4) != 0) {
            return fail(std::format("Node '{}': invalid KXRN container magic", node.name));
        }
        const size_t code_bytes = node.kxrn_bytecode.size() - 8;
        if (code_bytes == 0 || (code_bytes % 4) != 0) {
            return fail(std::format("Node '{}': KXRN payload size ({}) must be a non-zero multiple of 4",
                                    node.name, code_bytes));
        }
        if (out_kxrn_group_x) {
            uint32_t gx = 0;
            std::memcpy(&gx, node.kxrn_bytecode.data() + 4, sizeof(uint32_t));
            *out_kxrn_group_x = gx;
        }
        std::vector<uint32_t> words(code_bytes / 4);
        std::memcpy(words.data(), node.kxrn_bytecode.data() + 8, code_bytes);
        return words;
    }

    if (!node.rdna2_dwords.empty()) {
        if (out_kxrn_group_x) {
            *out_kxrn_group_x = 0;
        }
        return node.rdna2_dwords;
    }

    return fail(std::format("Node '{}': missing RDNA2 microcode (both kxrn_bytecode and rdna2_dwords are empty)",
                            node.name));
}

// ---- WorkGraphProgram ----

uint32_t WorkGraphProgram::add_node(WorkGraphNodeDesc desc) {
    const uint32_t idx = static_cast<uint32_t>(nodes.size());
    desc.node_index = idx;
    nodes.push_back(std::move(desc));
    return idx;
}

void WorkGraphProgram::add_edge(WorkGraphEdge edge) {
    edges.push_back(edge);
}

void WorkGraphProgram::add_edge(
    uint32_t producer_node,
    uint32_t consumer_node,
    uint32_t dispatch_args_byte_offset,
    uint32_t record_buffer_byte_offset,
    uint32_t max_dispatch_count) {
    WorkGraphEdge e{};
    e.producer_node = producer_node;
    e.consumer_node = consumer_node;
    e.dispatch_args_byte_offset = dispatch_args_byte_offset;
    e.record_buffer_byte_offset = record_buffer_byte_offset;
    e.max_dispatch_count = max_dispatch_count;
    edges.push_back(e);
}

uint64_t WorkGraphProgram::estimated_record_backing_bytes() const {
    uint64_t total = 0;
    for (const auto& node : nodes) {
        if (node.record_stride_bytes == 0 || node.max_output_records == 0) continue;
        const uint64_t stride = node.record_stride_bytes;
        const uint64_t count = node.max_output_records;
        if (stride > kMaxTotalBackingBytes / count) {
            return kMaxTotalBackingBytes + 1ull;
        }
        const uint64_t node_bytes = RecordRingAllocator::align_up(stride * count);
        if (node_bytes > kMaxTotalBackingBytes - total) {
            return kMaxTotalBackingBytes + 1ull;
        }
        total += node_bytes;
    }
    return total;
}

Result<std::vector<uint32_t>> WorkGraphProgram::topological_order() const {
    const size_t n = nodes.size();
    std::vector<std::vector<uint32_t>> adj(n);
    std::vector<uint32_t> in_degree(n, 0);

    for (const auto& e : edges) {
        if (e.producer_node >= n || e.consumer_node >= n) {
            return fail(std::format("WorkGraphEdge ({} -> {}) references out-of-range node index (node count = {})",
                                    e.producer_node, e.consumer_node, n));
        }
        adj[e.producer_node].push_back(e.consumer_node);
        ++in_degree[e.consumer_node];
    }

    // Min-heap ensures a deterministic topological order across platforms and runs.
    std::priority_queue<uint32_t, std::vector<uint32_t>, std::greater<uint32_t>> ready;
    for (uint32_t i = 0; i < static_cast<uint32_t>(n); ++i) {
        if (in_degree[i] == 0) {
            ready.push(i);
        }
    }

    std::vector<uint32_t> order;
    order.reserve(n);
    while (!ready.empty()) {
        const uint32_t u = ready.top();
        ready.pop();
        order.push_back(u);
        for (uint32_t v : adj[u]) {
            if (--in_degree[v] == 0) {
                ready.push(v);
            }
        }
    }

    if (order.size() != n) {
        return fail("WorkGraphProgram contains a cycle; Work Graph DAG must be acyclic");
    }
    return order;
}

Result<void> WorkGraphProgram::validate() const {
    if (nodes.empty()) {
        return fail("WorkGraphProgram must contain at least one node");
    }
    if (nodes.size() > kMaxNodes) {
        return fail(std::format("WorkGraphProgram node count ({}) exceeds limit ({})", nodes.size(), kMaxNodes));
    }

    std::unordered_set<std::string_view> seen_names;
    seen_names.reserve(nodes.size());

    uint64_t total_backing_bytes = 0;
    for (size_t i = 0; i < nodes.size(); ++i) {
        const auto& node = nodes[i];
        if (node.name.empty()) {
            return fail(std::format("WorkGraphNode[{}] has an empty name", i));
        }
        if (!seen_names.insert(node.name).second) {
            return fail(std::format("Duplicate WorkGraphNode name '{}'", node.name));
        }
        if (node.group_size_x == 0 || node.group_size_y == 0 || node.group_size_z == 0) {
            return fail(std::format("Node '{}': threadgroup dimensions must be >= 1", node.name));
        }
        if (node.group_size_x > 1024 || node.group_size_y > 1024 || node.group_size_z > 64) {
            return fail(std::format("Node '{}': threadgroup dimensions ({}, {}, {}) exceed D3D12 limits",
                                    node.name, node.group_size_x, node.group_size_y, node.group_size_z));
        }
        const uint64_t group_threads =
            static_cast<uint64_t>(node.group_size_x) *
            static_cast<uint64_t>(node.group_size_y) *
            static_cast<uint64_t>(node.group_size_z);
        if (group_threads > 1024ull) {
            return fail(std::format("Node '{}': total threads per group ({}) exceeds 1024",
                                    node.name, group_threads));
        }
        if ((node.record_stride_bytes % static_cast<uint32_t>(RecordRingAllocator::kAlignment)) != 0u) {
            return fail(std::format("Node '{}': record_stride_bytes ({}) must be 16-byte aligned",
                                    node.name, node.record_stride_bytes));
        }
        if (node.record_stride_bytes > 0 && node.max_output_records > 0) {
            const uint64_t stride = node.record_stride_bytes;
            const uint64_t count = node.max_output_records;
            if (stride > kMaxTotalBackingBytes / count) {
                return fail(std::format("Node '{}': record buffer size overflow ({} * {})",
                                        node.name, stride, count));
            }
            const uint64_t bytes = RecordRingAllocator::align_up(stride * count);
            if (bytes > kMaxTotalBackingBytes - total_backing_bytes) {
                return fail(std::format("WorkGraphProgram total record backing memory exceeds {} bytes",
                                        kMaxTotalBackingBytes));
            }
            total_backing_bytes += bytes;
        }

        auto dwords = extract_node_rdna2_dwords(node);
        if (!dwords) {
            return fail(dwords.error().message);
        }
    }

    std::unordered_set<uint64_t> seen_edges;
    seen_edges.reserve(edges.size());
    for (size_t i = 0; i < edges.size(); ++i) {
        const auto& e = edges[i];
        if (e.producer_node >= nodes.size() || e.consumer_node >= nodes.size()) {
            return fail(std::format("WorkGraphEdge[{}] ({} -> {}) references out-of-range node index (count = {})",
                                    i, e.producer_node, e.consumer_node, nodes.size()));
        }
        if (e.producer_node == e.consumer_node) {
            return fail(std::format("WorkGraphEdge[{}] self-loop cycle on node {}", i, e.producer_node));
        }
        if (!seen_edges.insert(pack_edge_key(e.producer_node, e.consumer_node)).second) {
            return fail(std::format("Duplicate WorkGraphEdge ({} -> {})", e.producer_node, e.consumer_node));
        }
        if (e.max_dispatch_count == 0) {
            return fail(std::format("WorkGraphEdge[{}] max_dispatch_count must be >= 1", i));
        }
        if ((e.dispatch_args_byte_offset % 4u) != 0u) {
            return fail(std::format("WorkGraphEdge[{}] dispatch_args_byte_offset ({}) must be 4-byte aligned",
                                    i, e.dispatch_args_byte_offset));
        }
        if ((e.record_buffer_byte_offset % static_cast<uint32_t>(RecordRingAllocator::kAlignment)) != 0u) {
            return fail(std::format("WorkGraphEdge[{}] record_buffer_byte_offset ({}) must be 16-byte aligned",
                                    i, e.record_buffer_byte_offset));
        }
    }

    auto order = topological_order();
    if (!order) {
        return fail(order.error().message);
    }
    return {};
}

// ---- RecordRingAllocator ----

RecordRingAllocator::RecordRingAllocator(uint64_t capacity_bytes, uint64_t base_gpu_va)
    : capacity_bytes_(capacity_bytes & ~(kAlignment - 1ull)),
      base_gpu_va_(base_gpu_va) {}

Result<RecordAllocation> RecordRingAllocator::allocate(uint64_t size_bytes) {
    if (size_bytes == 0 || capacity_bytes_ == 0) {
        ++failed_allocations_;
        return fail("RecordRingAllocator::allocate requires non-zero size and capacity");
    }
    if (size_bytes > capacity_bytes_) {
        ++failed_allocations_;
        return fail(std::format("RecordRingAllocator request ({} B) exceeds capacity ({} B)",
                                size_bytes, capacity_bytes_));
    }

    const uint64_t aligned = align_up(size_bytes);
    uint64_t offset = 0;
    bool wrapped = false;

    if (used_bytes_ == 0) {
        offset = 0;
        head_ = aligned;
        tail_ = 0;
        wasted_tail_bytes_ = 0;
        used_bytes_ = aligned;
    } else if (head_ > tail_) {
        if (aligned <= capacity_bytes_ - head_) {
            offset = head_;
            head_ += aligned;
            used_bytes_ += aligned;
        } else if (aligned <= tail_) {
            wasted_tail_bytes_ = capacity_bytes_ - head_;
            offset = 0;
            head_ = aligned;
            used_bytes_ += aligned;
            wrapped = true;
            ++wrap_count_;
        } else {
            ++failed_allocations_;
            return fail("RecordRingAllocator out of contiguous ring space");
        }
    } else {
        if (aligned <= tail_ - head_) {
            offset = head_;
            head_ += aligned;
            used_bytes_ += aligned;
        } else {
            ++failed_allocations_;
            return fail("RecordRingAllocator out of contiguous ring space");
        }
    }

    ++allocation_count_;
    if (used_bytes_ > peak_used_bytes_) {
        peak_used_bytes_ = used_bytes_;
    }

    RecordAllocation alloc{};
    alloc.offset = offset;
    alloc.gpu_va = base_gpu_va_ + offset;
    alloc.size_bytes = size_bytes;
    alloc.aligned_size_bytes = aligned;
    alloc.wrapped = wrapped;
    return alloc;
}

Result<void> RecordRingAllocator::release(const RecordAllocation& alloc) {
    if (alloc.aligned_size_bytes == 0 ||
        (alloc.aligned_size_bytes % kAlignment) != 0 ||
        alloc.aligned_size_bytes > used_bytes_) {
        return fail("RecordRingAllocator::release invalid allocation size");
    }
    if (alloc.offset != tail_) {
        return fail("RecordRingAllocator::release requires FIFO order matching oldest live allocation");
    }

    tail_ += alloc.aligned_size_bytes;
    used_bytes_ -= alloc.aligned_size_bytes;

    if (used_bytes_ == 0) {
        head_ = 0;
        tail_ = 0;
        wasted_tail_bytes_ = 0;
    } else if (tail_ + wasted_tail_bytes_ >= capacity_bytes_) {
        tail_ = 0;
        wasted_tail_bytes_ = 0;
    }
    return {};
}

void RecordRingAllocator::reset() {
    head_ = 0;
    tail_ = 0;
    used_bytes_ = 0;
    wasted_tail_bytes_ = 0;
}

RecordRingMetrics RecordRingAllocator::metrics() const noexcept {
    RecordRingMetrics m{};
    m.capacity_bytes = capacity_bytes_;
    m.used_bytes = used_bytes_;
    m.peak_used_bytes = peak_used_bytes_;
    m.free_bytes = (capacity_bytes_ >= used_bytes_) ? (capacity_bytes_ - used_bytes_) : 0;
    m.wasted_tail_bytes = wasted_tail_bytes_;
    m.allocation_count = allocation_count_;
    m.wrap_count = wrap_count_;
    m.failed_allocations = failed_allocations_;
    m.utilization = (capacity_bytes_ > 0)
                        ? (static_cast<double>(used_bytes_) / static_cast<double>(capacity_bytes_))
                        : 0.0;
    return m;
}

// ---- WorkGraphExecutor ----

struct WorkGraphExecutor::Impl {
    bool compiled = false;
    WorkGraphCapability capability{};
    WorkGraphProgram program{};
    std::vector<uint32_t> topo_order;
    std::vector<std::vector<uint32_t>> incoming_edge_indices;
    std::vector<std::vector<uint32_t>> outgoing_edge_indices;
    std::vector<std::vector<uint8_t>> node_dxil;
    std::vector<ComPtr<ID3D12PipelineState>> node_psos;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12RootSignature> root_sig;
    ComPtr<ID3D12CommandSignature> dispatch_cmd_sig;
    RecordRingAllocator record_allocator{4096};
};

WorkGraphExecutor::WorkGraphExecutor() : impl_(std::make_unique<Impl>()) {}
WorkGraphExecutor::~WorkGraphExecutor() = default;
WorkGraphExecutor::WorkGraphExecutor(WorkGraphExecutor&&) noexcept = default;
WorkGraphExecutor& WorkGraphExecutor::operator=(WorkGraphExecutor&&) noexcept = default;

WorkGraphCapability WorkGraphExecutor::CheckWorkGraphSupport(ID3D12Device* device) {
    WorkGraphCapability cap{};

#if defined(D3D12_WORK_GRAPHS_TIER_1_0)
    cap.sdk_headers_support_native = true;
#else
    cap.sdk_headers_support_native = false;
#endif

    if (!device) {
        cap.selected_mode = WorkGraphExecutionMode::FallbackExecuteIndirect;
        return cap;
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS1 opt1{};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &opt1, sizeof(opt1)))) {
        cap.supports_wave64 = (opt1.WaveLaneCountMin <= 64 && opt1.WaveLaneCountMax >= 64);
    }

    D3D12_FEATURE_DATA_SHADER_MODEL sm{};
    sm.HighestShaderModel = static_cast<D3D_SHADER_MODEL>(kD3DShaderModel68);
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm)))) {
        cap.highest_shader_model = static_cast<uint32_t>(sm.HighestShaderModel);
        cap.supports_sm_6_8 = (cap.highest_shader_model >= kD3DShaderModel68);
    } else {
        sm.HighestShaderModel = D3D_SHADER_MODEL_6_7;
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm)))) {
            cap.highest_shader_model = static_cast<uint32_t>(sm.HighestShaderModel);
        } else {
            sm.HighestShaderModel = D3D_SHADER_MODEL_6_6;
            if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm)))) {
                cap.highest_shader_model = static_cast<uint32_t>(sm.HighestShaderModel);
            }
        }
    }

    KxD3D12FeatureDataOptions21 opt21{};
    if (SUCCEEDED(device->CheckFeatureSupport(
            static_cast<D3D12_FEATURE>(kD3D12FeatureOptions21), &opt21, sizeof(opt21)))) {
        cap.driver_work_graphs_tier = opt21.work_graphs_tier;
        cap.driver_execute_indirect_tier = opt21.execute_indirect_tier;
        cap.driver_reports_work_graphs = (opt21.work_graphs_tier >= kWorkGraphsTier1_0);
    }

    cap.selected_mode = (cap.sdk_headers_support_native && cap.driver_reports_work_graphs)
                            ? WorkGraphExecutionMode::NativeWorkGraphs
                            : WorkGraphExecutionMode::FallbackExecuteIndirect;
    return cap;
}

Result<void> WorkGraphExecutor::compile(
    ID3D12Device* device,
    const WorkGraphProgram& program,
    DxcCompiler& dxc,
    WaveMode forced_wave_mode,
    bool auto_select_wave64) {
    if (!device) {
        return fail("WorkGraphExecutor::compile requires a valid ID3D12Device");
    }

    auto valid = program.validate();
    if (!valid) {
        return fail(valid.error().message);
    }
    auto order = program.topological_order();
    if (!order) {
        return fail(order.error().message);
    }

    auto cap = CheckWorkGraphSupport(device);

    // Build shared compute root signature matching KestrelX RDNA2 compute convention:
    //   [0] 8 x 32-bit root constants at b0 (kx_root0, kx_root1)
    //   [1] Root UAV at u0 (RWByteAddressBuffer g_mem)
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

    ComPtr<ID3DBlob> rs_blob;
    ComPtr<ID3DBlob> rs_err;
    HRESULT hr = D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &rs_err);
    if (FAILED(hr) || !rs_blob) {
        return fail(std::format("WorkGraphExecutor: D3D12SerializeRootSignature failed ({:#x})",
                                static_cast<uint32_t>(hr)));
    }

    ComPtr<ID3D12RootSignature> root_sig;
    hr = device->CreateRootSignature(
        0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(), IID_PPV_ARGS(&root_sig));
    if (FAILED(hr)) {
        return fail(std::format("WorkGraphExecutor: CreateRootSignature failed ({:#x})",
                                static_cast<uint32_t>(hr)));
    }

    // Build ExecuteIndirect command signature for device-generated D3D12_DISPATCH_ARGUMENTS.
    D3D12_INDIRECT_ARGUMENT_DESC arg_desc{};
    arg_desc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;

    D3D12_COMMAND_SIGNATURE_DESC sig_desc{};
    sig_desc.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
    sig_desc.NumArgumentDescs = 1;
    sig_desc.pArgumentDescs = &arg_desc;
    sig_desc.NodeMask = 0;

    ComPtr<ID3D12CommandSignature> dispatch_cmd_sig;
    hr = device->CreateCommandSignature(&sig_desc, nullptr, IID_PPV_ARGS(&dispatch_cmd_sig));
    if (FAILED(hr)) {
        return fail(std::format("WorkGraphExecutor: CreateCommandSignature(DISPATCH) failed ({:#x})",
                                static_cast<uint32_t>(hr)));
    }

    std::vector<std::vector<uint8_t>> compiled_dxil;
    std::vector<ComPtr<ID3D12PipelineState>> compiled_psos;
    compiled_dxil.reserve(program.nodes.size());
    compiled_psos.reserve(program.nodes.size());

    for (size_t i = 0; i < program.nodes.size(); ++i) {
        const auto& node = program.nodes[i];
        uint32_t kxrn_group_x = 0;
        auto dwords = extract_node_rdna2_dwords(node, &kxrn_group_x);
        if (!dwords) {
            return fail(dwords.error().message);
        }

        auto decoded = decode_rdna2(dwords->data(), dwords->size());
        auto ir = lift_rdna2_to_ir(decoded);
        if (ir.unknown_count > 0) {
            return fail(std::format("Node '{}': {} unsupported RDNA2 opcode(s)", node.name, ir.unknown_count));
        }

        const uint32_t gx = (kxrn_group_x > 0 && kxrn_group_x <= 1024) ? kxrn_group_x : node.group_size_x;
        const uint32_t gy = node.group_size_y;
        const uint32_t gz = node.group_size_z;
        const uint64_t group_threads = static_cast<uint64_t>(gx) * gy * gz;

        WaveMode node_wave = forced_wave_mode;
        if (auto_select_wave64 && cap.supports_wave64 && group_threads >= 64ull) {
            node_wave = WaveMode::Wave64;
        } else if (node_wave == WaveMode::Wave64 && (!cap.supports_wave64 || group_threads < 64ull)) {
            node_wave = WaveMode::Wave32;
        }

        RecompOptions ropt{};
        ropt.stage = ShaderStage::Compute;
        ropt.wave_mode = node_wave;
        ropt.group_size_x = gx;
        ropt.group_size_y = gy;
        ropt.group_size_z = gz;
        ropt.entry_point = "main";

        auto recomp = emit_hlsl_from_ir(ir, ropt);
        auto dxil = dxc.compile_hlsl(recomp.hlsl, "main", "cs_6_6");
        if (!dxil) {
            return fail(std::format("Node '{}': DXC compilation failed: {}", node.name, dxil.error().message));
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
        pso_desc.pRootSignature = root_sig.Get();
        pso_desc.CS.pShaderBytecode = dxil->dxil.data();
        pso_desc.CS.BytecodeLength = dxil->dxil.size();

        ComPtr<ID3D12PipelineState> pso;
        hr = device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso));
        if (FAILED(hr)) {
            return fail(std::format("Node '{}': CreateComputePipelineState failed ({:#x})",
                                    node.name, static_cast<uint32_t>(hr)));
        }

        compiled_dxil.push_back(std::move(dxil->dxil));
        compiled_psos.push_back(std::move(pso));
    }

    std::vector<std::vector<uint32_t>> incoming(program.nodes.size());
    std::vector<std::vector<uint32_t>> outgoing(program.nodes.size());
    for (uint32_t e = 0; e < static_cast<uint32_t>(program.edges.size()); ++e) {
        const auto& edge = program.edges[e];
        outgoing[edge.producer_node].push_back(e);
        incoming[edge.consumer_node].push_back(e);
    }

    const uint64_t backing_cap = std::max<uint64_t>(
        4096ull,
        RecordRingAllocator::align_up(
            program.estimated_record_backing_bytes() +
            static_cast<uint64_t>(program.edges.size()) * kIndirectSlotStrideBytes));

    impl_->compiled = true;
    impl_->capability = cap;
    impl_->program = program;
    impl_->topo_order = std::move(*order);
    impl_->incoming_edge_indices = std::move(incoming);
    impl_->outgoing_edge_indices = std::move(outgoing);
    impl_->node_dxil = std::move(compiled_dxil);
    impl_->node_psos = std::move(compiled_psos);
    impl_->device = device;
    impl_->root_sig = std::move(root_sig);
    impl_->dispatch_cmd_sig = std::move(dispatch_cmd_sig);
    impl_->record_allocator = RecordRingAllocator(backing_cap, 0);

    hle::record_work_graph_created();
    return {};
}

Result<void> WorkGraphExecutor::record_commands(
    ID3D12GraphicsCommandList* cmd,
    ID3D12Resource* backing_uav_buffer,
    ID3D12Resource* indirect_args_buffer,
    uint64_t backing_buffer_size_bytes,
    const WorkGraphDispatchDesc& desc,
    uint32_t* out_direct_dispatches,
    uint32_t* out_indirect_dispatches,
    uint32_t* out_uav_barriers) {
    if (!impl_->compiled) {
        return fail("WorkGraphExecutor::record_commands called before compile()");
    }
    if (!cmd || !backing_uav_buffer || !indirect_args_buffer) {
        return fail("WorkGraphExecutor::record_commands requires valid command list and buffers");
    }

    for (size_t e = 0; e < impl_->program.edges.size(); ++e) {
        const auto& edge = impl_->program.edges[e];
        if (static_cast<uint64_t>(edge.dispatch_args_byte_offset) + sizeof(D3D12_DISPATCH_ARGUMENTS) >
            backing_buffer_size_bytes) {
            return fail(std::format("WorkGraphEdge[{}] dispatch_args_byte_offset ({}) exceeds backing buffer size ({})",
                                    e, edge.dispatch_args_byte_offset, backing_buffer_size_bytes));
        }
    }

    impl_->record_allocator.reset();
    impl_->record_allocator.set_base_gpu_va(backing_uav_buffer->GetGPUVirtualAddress());
    for (const auto& edge : impl_->program.edges) {
        const auto& prod = impl_->program.nodes[edge.producer_node];
        const uint64_t edge_bytes =
            (prod.record_stride_bytes > 0 && prod.max_output_records > 0)
                ? (static_cast<uint64_t>(prod.record_stride_bytes) * static_cast<uint64_t>(prod.max_output_records))
                : kIndirectSlotStrideBytes;
        auto alloc = impl_->record_allocator.allocate(edge_bytes);
        if (!alloc) {
            return fail(std::format("WorkGraphExecutor: record ring allocation failed: {}", alloc.error().message));
        }
    }

    uint32_t direct_dispatches = 0;
    uint32_t indirect_dispatches = 0;
    uint32_t uav_barriers = 0;
    D3D12_RESOURCE_STATES indirect_state = D3D12_RESOURCE_STATE_COPY_DEST;

    const D3D12_GPU_VIRTUAL_ADDRESS uav_va = backing_uav_buffer->GetGPUVirtualAddress();
    const std::array<uint32_t, 8> zero_roots{};

    auto emit_uav_barrier = [&]() {
        D3D12_RESOURCE_BARRIER uav_bar{};
        uav_bar.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav_bar.UAV.pResource = backing_uav_buffer;
        cmd->ResourceBarrier(1, &uav_bar);
        ++uav_barriers;
    };

    for (uint32_t node_idx : impl_->topo_order) {
        const auto& roots = (node_idx < desc.node_root_constants.size())
                                ? desc.node_root_constants[node_idx]
                                : zero_roots;

        cmd->SetComputeRootSignature(impl_->root_sig.Get());
        cmd->SetPipelineState(impl_->node_psos[node_idx].Get());
        cmd->SetComputeRoot32BitConstants(0, 8, roots.data(), 0);
        cmd->SetComputeRootUnorderedAccessView(1, uav_va);

        const auto& incoming = impl_->incoming_edge_indices[node_idx];
        if (incoming.empty()) {
            cmd->Dispatch(desc.entry_groups_x, desc.entry_groups_y, desc.entry_groups_z);
            ++direct_dispatches;
        } else {
            for (size_t k = 0; k < incoming.size(); ++k) {
                const uint32_t edge_idx = incoming[k];
                const UINT64 arg_offset = static_cast<UINT64>(edge_idx) * kIndirectSlotStrideBytes;
                cmd->ExecuteIndirect(
                    impl_->dispatch_cmd_sig.Get(),
                    1,
                    indirect_args_buffer,
                    arg_offset,
                    nullptr,
                    0);
                ++indirect_dispatches;
                if (k + 1 < incoming.size()) {
                    emit_uav_barrier();
                }
            }
        }

        // Synchronize UAV writes before either copying GPU-generated dispatch arguments for
        // downstream nodes or finalizing leaf node output.
        emit_uav_barrier();

        const auto& outgoing = impl_->outgoing_edge_indices[node_idx];
        if (!outgoing.empty()) {
            D3D12_RESOURCE_BARRIER pre_copy[2]{};
            UINT pre_count = 0;

            pre_copy[pre_count].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            pre_copy[pre_count].Transition.pResource = backing_uav_buffer;
            pre_copy[pre_count].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            pre_copy[pre_count].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            pre_copy[pre_count].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            ++pre_count;

            if (indirect_state != D3D12_RESOURCE_STATE_COPY_DEST) {
                pre_copy[pre_count].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                pre_copy[pre_count].Transition.pResource = indirect_args_buffer;
                pre_copy[pre_count].Transition.StateBefore = indirect_state;
                pre_copy[pre_count].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                pre_copy[pre_count].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                ++pre_count;
            }
            cmd->ResourceBarrier(pre_count, pre_copy);

            for (uint32_t edge_idx : outgoing) {
                const auto& edge = impl_->program.edges[edge_idx];
                cmd->CopyBufferRegion(
                    indirect_args_buffer,
                    static_cast<UINT64>(edge_idx) * kIndirectSlotStrideBytes,
                    backing_uav_buffer,
                    edge.dispatch_args_byte_offset,
                    sizeof(D3D12_DISPATCH_ARGUMENTS));
            }

            D3D12_RESOURCE_BARRIER post_copy[2]{};
            post_copy[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            post_copy[0].Transition.pResource = backing_uav_buffer;
            post_copy[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            post_copy[0].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            post_copy[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

            post_copy[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            post_copy[1].Transition.pResource = indirect_args_buffer;
            post_copy[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            post_copy[1].Transition.StateAfter = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
            post_copy[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cmd->ResourceBarrier(2, post_copy);
            indirect_state = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
        }
    }

    if (out_direct_dispatches) *out_direct_dispatches = direct_dispatches;
    if (out_indirect_dispatches) *out_indirect_dispatches = indirect_dispatches;
    if (out_uav_barriers) *out_uav_barriers = uav_barriers;

    hle::record_work_graph_dispatch(1);
    return {};
}

Result<WorkGraphExecutionResult> WorkGraphExecutor::execute(
    const WorkGraphDispatchDesc& desc,
    ID3D12CommandQueue* external_queue) {
    if (!impl_->compiled || !impl_->device) {
        return fail("WorkGraphExecutor::execute called before compile()");
    }
    if (desc.initial_buffer_u32.empty()) {
        return fail("WorkGraphExecutor::execute requires non-empty initial_buffer_u32");
    }

    ID3D12Device* dev = impl_->device.Get();
    ComPtr<ID3D12CommandQueue> owned_queue;
    ID3D12CommandQueue* queue = external_queue;
    if (!queue) {
        D3D12_COMMAND_QUEUE_DESC qdesc{};
        qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        HRESULT hr = dev->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&owned_queue));
        if (FAILED(hr)) {
            return fail(std::format("WorkGraphExecutor: CreateCommandQueue failed ({:#x})",
                                    static_cast<uint32_t>(hr)));
        }
        queue = owned_queue.Get();
    }

    const D3D12_COMMAND_LIST_TYPE list_type = queue->GetDesc().Type;
    ComPtr<ID3D12CommandAllocator> alloc;
    HRESULT hr = dev->CreateCommandAllocator(list_type, IID_PPV_ARGS(&alloc));
    if (FAILED(hr)) {
        return fail(std::format("WorkGraphExecutor: CreateCommandAllocator failed ({:#x})",
                                static_cast<uint32_t>(hr)));
    }

    ComPtr<ID3D12GraphicsCommandList> cmd;
    hr = dev->CreateCommandList(0, list_type, alloc.Get(), nullptr, IID_PPV_ARGS(&cmd));
    if (FAILED(hr)) {
        return fail(std::format("WorkGraphExecutor: CreateCommandList failed ({:#x})",
                                static_cast<uint32_t>(hr)));
    }

    const UINT64 byte_size = static_cast<UINT64>(desc.initial_buffer_u32.size()) * sizeof(uint32_t);
    const UINT64 indirect_bytes = std::max<UINT64>(
        64ull, static_cast<UINT64>(impl_->program.edges.size()) * kIndirectSlotStrideBytes);

    auto make_buf = [&](UINT64 size, D3D12_HEAP_TYPE ht, D3D12_RESOURCE_FLAGS flags,
                        D3D12_RESOURCE_STATES st, ComPtr<ID3D12Resource>& out) -> HRESULT {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = ht;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = size;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags = flags;
        return dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, st, nullptr, IID_PPV_ARGS(&out));
    };

    ComPtr<ID3D12Resource> upload_buf, gpu_buf, indirect_buf, readback_buf;
    if (FAILED(make_buf(byte_size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                        D3D12_RESOURCE_STATE_GENERIC_READ, upload_buf)) ||
        FAILED(make_buf(byte_size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_COPY_DEST, gpu_buf)) ||
        FAILED(make_buf(indirect_bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE,
                        D3D12_RESOURCE_STATE_COPY_DEST, indirect_buf)) ||
        FAILED(make_buf(byte_size, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                        D3D12_RESOURCE_STATE_COPY_DEST, readback_buf))) {
        return fail("WorkGraphExecutor: failed to allocate GPU buffers");
    }

    void* mapped = nullptr;
    if (FAILED(upload_buf->Map(0, nullptr, &mapped))) {
        return fail("WorkGraphExecutor: upload buffer Map failed");
    }
    std::memcpy(mapped, desc.initial_buffer_u32.data(), static_cast<size_t>(byte_size));
    upload_buf->Unmap(0, nullptr);

    cmd->CopyBufferRegion(gpu_buf.Get(), 0, upload_buf.Get(), 0, byte_size);

    D3D12_RESOURCE_BARRIER to_uav{};
    to_uav.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_uav.Transition.pResource = gpu_buf.Get();
    to_uav.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    to_uav.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    to_uav.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd->ResourceBarrier(1, &to_uav);

    uint32_t direct_dispatches = 0;
    uint32_t indirect_dispatches = 0;
    uint32_t uav_barriers = 0;
    auto rec = record_commands(
        cmd.Get(),
        gpu_buf.Get(),
        indirect_buf.Get(),
        byte_size,
        desc,
        &direct_dispatches,
        &indirect_dispatches,
        &uav_barriers);
    if (!rec) {
        return fail(rec.error().message);
    }

    D3D12_RESOURCE_BARRIER to_copy{};
    to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_copy.Transition.pResource = gpu_buf.Get();
    to_copy.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    to_copy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    to_copy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd->ResourceBarrier(1, &to_copy);

    cmd->CopyBufferRegion(readback_buf.Get(), 0, gpu_buf.Get(), 0, byte_size);
    if (FAILED(cmd->Close())) {
        return fail("WorkGraphExecutor: CommandList Close failed");
    }

    ComPtr<ID3D12Fence> fence;
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
        return fail("WorkGraphExecutor: CreateFence failed");
    }
    HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!evt) {
        return fail("WorkGraphExecutor: CreateEventW failed");
    }

    const auto t0 = std::chrono::steady_clock::now();
    ID3D12CommandList* lists[] = {cmd.Get()};
    queue->ExecuteCommandLists(1, lists);
    if (FAILED(queue->Signal(fence.Get(), 1))) {
        CloseHandle(evt);
        return fail("WorkGraphExecutor: CommandQueue Signal failed");
    }
    if (fence->GetCompletedValue() < 1) {
        if (FAILED(fence->SetEventOnCompletion(1, evt))) {
            CloseHandle(evt);
            return fail("WorkGraphExecutor: Fence SetEventOnCompletion failed");
        }
        const DWORD wait_rc = WaitForSingleObject(evt, 10000);
        if (wait_rc != WAIT_OBJECT_0) {
            CloseHandle(evt);
            return fail("WorkGraphExecutor: GPU execution timed out");
        }
    }
    const auto t1 = std::chrono::steady_clock::now();
    CloseHandle(evt);

    WorkGraphExecutionResult res{};
    res.capability = impl_->capability;
    res.executed_node_order = impl_->topo_order;
    res.direct_dispatches = direct_dispatches;
    res.indirect_dispatches = indirect_dispatches;
    res.uav_barriers = uav_barriers;
    res.allocator_metrics = impl_->record_allocator.metrics();
    res.gpu_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    res.output_u32.resize(desc.initial_buffer_u32.size());

    D3D12_RANGE rr{0, static_cast<SIZE_T>(byte_size)};
    if (FAILED(readback_buf->Map(0, &rr, &mapped))) {
        return fail("WorkGraphExecutor: readback buffer Map failed");
    }
    std::memcpy(res.output_u32.data(), mapped, static_cast<size_t>(byte_size));
    D3D12_RANGE wr{0, 0};
    readback_buf->Unmap(0, &wr);
    return res;
}

bool WorkGraphExecutor::is_compiled() const noexcept { return impl_->compiled; }
const WorkGraphCapability& WorkGraphExecutor::capability() const noexcept { return impl_->capability; }
const WorkGraphProgram& WorkGraphExecutor::program() const noexcept { return impl_->program; }
const std::vector<uint32_t>& WorkGraphExecutor::topological_order() const noexcept { return impl_->topo_order; }
const std::vector<std::vector<uint8_t>>& WorkGraphExecutor::compiled_node_dxil() const noexcept { return impl_->node_dxil; }
const RecordRingAllocator& WorkGraphExecutor::record_allocator() const noexcept { return impl_->record_allocator; }

}  // namespace kestrel::gpu
