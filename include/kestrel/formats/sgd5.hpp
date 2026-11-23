#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kestrel::formats {

// Game-specific shader archive format (SGD5)
// Magic bytes: "SGD5" (0x35444753 in little-endian)
// Record marker: 0x000DDCE7
// Encapsulates individual compiled shader containers compressed via raw DEFLATE.
constexpr uint32_t SGD5_MAGIC = 0x35444753; // 'S','G','D','5'
constexpr uint32_t SGD5_RECORD_MARKER = 0x000DDCE7;

enum class Sgd5ShaderKind : uint8_t {
    StandardShader = 0x32,   // Vertex, Pixel, Geometry
    ComputeShader  = 0x3c,   // Compute dispatch pipelines
    MeshShader     = 0x41,   // Mesh / Amplification shader clusters
    MaterialFamily = 0x40,   // Material permutation family
    Extended       = 0x42,
    Unknown        = 0x00,
};

struct Sgd5Record {
    size_t offset = 0;
    std::string name;
    Sgd5ShaderKind kind = Sgd5ShaderKind::Unknown;
    uint32_t packed_size = 0;
    uint32_t unpacked_size = 0;
    std::span<const uint8_t> packed_payload;

    // Decompress the container bytes via raw DEFLATE
    [[nodiscard]] std::optional<std::vector<uint8_t>> unpack() const;
};

class Sgd5Archive {
public:
    // Check if buffer contains the SGD5 header
    static bool is_sgd5(std::span<const uint8_t> data);

    // Parse all valid records in the SGD5 stream or archive
    static std::vector<Sgd5Record> parse_records(std::span<const uint8_t> data);

    // Synthesize an SGD5 binary blob for tests and pipeline validation
    static std::vector<uint8_t> synthesize_archive(
        const std::vector<std::tuple<std::string, uint8_t, std::vector<uint8_t>>>& items
    );
};

}  // namespace kestrel::formats
