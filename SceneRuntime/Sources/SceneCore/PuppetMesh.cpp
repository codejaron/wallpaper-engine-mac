#include <SceneCore/PuppetMesh.hpp>

#include <SceneCore/FormatError.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

namespace we::scene {
namespace {

constexpr std::size_t magicSize = 9;
constexpr std::size_t meshHeaderSize = sizeof(std::uint32_t) * 2;
constexpr std::size_t vertexStride = 80;
constexpr std::size_t positionOffset = 0;
constexpr std::size_t uvOffset = 72;
constexpr std::size_t indexLengthSize = sizeof(std::uint32_t);
constexpr std::size_t triangleIndexBytes = sizeof(std::uint16_t) * 3;

[[nodiscard]] bool hasBytes(
    std::size_t offset,
    std::size_t count,
    std::size_t limit
) noexcept {
    return offset <= limit && count <= limit - offset;
}

[[nodiscard]] std::uint32_t readUInt32(
    std::span<const std::uint8_t> bytes,
    std::size_t offset
) noexcept {
    return static_cast<std::uint32_t>(bytes[offset]) |
        (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
        (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) |
        (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
}

[[nodiscard]] float readFloat32(
    std::span<const std::uint8_t> bytes,
    std::size_t offset
) noexcept {
    const std::uint32_t bits = readUInt32(bytes, offset);
    float value = 0.0F;
    static_assert(sizeof(value) == sizeof(bits));
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

[[noreturn]] void fail(
    FormatErrorCode code,
    const std::string& source,
    std::size_t offset,
    std::string message
) {
    throw FormatError(code, source, offset, std::move(message));
}

}  // namespace

PuppetMesh PuppetMeshParser::parse(
    std::span<const std::uint8_t> bytes,
    std::string source
) {
    if (bytes.size() < magicSize) {
        fail(
            FormatErrorCode::unexpectedEndOfFile,
            source,
            bytes.size(),
            "Puppet model is shorter than its 9-byte MDLV header"
        );
    }

    const auto magicMatches = [&bytes](std::string_view magic) {
        if (magic.size() != 8 || bytes[8] != 0) return false;
        for (std::size_t index = 0; index < magic.size(); ++index) {
            if (bytes[index] != static_cast<std::uint8_t>(magic[index])) {
                return false;
            }
        }
        return true;
    };
    PuppetModelVersion version;
    if (magicMatches("MDLV0021")) {
        version = PuppetModelVersion::mdlv0021;
    } else if (magicMatches("MDLV0023")) {
        version = PuppetModelVersion::mdlv0023;
    } else {
        fail(
            FormatErrorCode::unsupportedFormat,
            source,
            0,
            "Unsupported puppet model header; expected MDLV0021 or MDLV0023"
        );
    }

    // MDLS terminates the model metadata. Linux treats the first marker as
    // the scan limit; retaining that rule is important for existing assets.
    std::size_t limit = bytes.size();
    for (std::size_t offset = magicSize; hasBytes(offset, 4, bytes.size()); ++offset) {
        if (bytes[offset] == 'M' && bytes[offset + 1] == 'D' &&
            bytes[offset + 2] == 'L' && bytes[offset + 3] == 'S') {
            limit = offset;
            break;
        }
    }

    struct Candidate final {
        std::size_t headerOffset = 0;
        std::uint32_t vertexBytes = 0;
        std::uint32_t indexBytes = 0;
    };
    std::optional<Candidate> candidate;

    // The format has no explicit mesh-block marker. Scan exactly as the
    // Linux implementation does, but perform checked arithmetic before every
    // slice so malformed data cannot wrap an offset.
    for (std::size_t offset = magicSize;
         hasBytes(offset, meshHeaderSize + indexLengthSize, limit);
         ++offset) {
        const std::uint32_t vertexBytes = readUInt32(bytes, offset + 4);
        if (vertexBytes == 0 || vertexBytes % vertexStride != 0) {
            continue;
        }
        const std::size_t verticesOffset = offset + meshHeaderSize;
        if (!hasBytes(verticesOffset, vertexBytes, limit)) {
            continue;
        }
        const std::size_t indexLengthOffset = verticesOffset + vertexBytes;
        if (!hasBytes(indexLengthOffset, indexLengthSize, limit)) {
            continue;
        }
        const std::uint32_t indexBytes = readUInt32(bytes, indexLengthOffset);
        if (indexBytes == 0 || indexBytes % triangleIndexBytes != 0) {
            continue;
        }
        const std::size_t indicesOffset = indexLengthOffset + indexLengthSize;
        if (!hasBytes(indicesOffset, indexBytes, limit)) {
            continue;
        }
        // Linux's loader consumes the first structurally valid MDLV block.
        // Keep that selection rule for parity: a later block can be a second
        // submesh or an unrelated payload, and must not make an otherwise
        // renderable asset fail as "ambiguous". The selected block is still
        // decoded and validated below; malformed vertex/index data therefore
        // fails explicitly rather than falling through to another candidate.
        candidate = Candidate{offset, vertexBytes, indexBytes};
        break;
    }

    if (!candidate) {
        fail(
            FormatErrorCode::invalidValue,
            source,
            magicSize,
            "Puppet model does not contain a usable MDLV mesh block"
        );
    }

    const Candidate selected = *candidate;
    const std::size_t vertexCount = selected.vertexBytes / vertexStride;
    const std::size_t indexCount = selected.indexBytes / sizeof(std::uint16_t);
    if (vertexCount == 0 || indexCount == 0 ||
        vertexCount > std::numeric_limits<std::uint16_t>::max() + std::size_t{1}) {
        fail(
            FormatErrorCode::invalidValue,
            source,
            selected.headerOffset,
            "Puppet model mesh has an invalid vertex or index count"
        );
    }

    const std::size_t verticesOffset = selected.headerOffset + meshHeaderSize;
    const std::size_t indicesOffset = verticesOffset + selected.vertexBytes +
        indexLengthSize;
    PuppetMesh result{
        .source = std::move(source),
        .version = version,
        .vertices = {},
        .indices = {},
    };
    result.vertices.reserve(vertexCount);
    result.indices.reserve(indexCount);

    for (std::size_t index = 0; index < vertexCount; ++index) {
        const std::size_t vertexOffset = verticesOffset + index * vertexStride;
        const float x = readFloat32(bytes, vertexOffset + positionOffset);
        const float y = readFloat32(bytes, vertexOffset + positionOffset + 4);
        const float z = readFloat32(bytes, vertexOffset + positionOffset + 8);
        const float u = readFloat32(bytes, vertexOffset + uvOffset);
        const float v = readFloat32(bytes, vertexOffset + uvOffset + 4);
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) ||
            !std::isfinite(u) || !std::isfinite(v)) {
            fail(
                FormatErrorCode::invalidValue,
                result.source,
                vertexOffset,
                "Puppet model vertex contains a non-finite position or UV"
            );
        }
        result.vertices.push_back({
            .position = {x, y, z},
            .texCoord = {u, v},
        });
    }

    for (std::size_t index = 0; index < indexCount; ++index) {
        const std::uint16_t value = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(bytes[indicesOffset + index * 2]) |
            (static_cast<std::uint16_t>(bytes[indicesOffset + index * 2 + 1]) << 8U)
        );
        if (value >= vertexCount) {
            fail(
                FormatErrorCode::invalidValue,
                result.source,
                indicesOffset + index * 2,
                "Puppet model index references a vertex outside the mesh"
            );
        }
        result.indices.push_back(value);
    }

    return result;
}

}  // namespace we::scene
