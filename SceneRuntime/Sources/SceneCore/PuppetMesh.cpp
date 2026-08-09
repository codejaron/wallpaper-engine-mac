#include <SceneCore/PuppetMesh.hpp>

#include <SceneCore/FormatError.hpp>

#include <array>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <numbers>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace we::scene {
namespace {

constexpr std::size_t magicSize = 9;
constexpr std::size_t meshHeaderSize = sizeof(std::uint32_t) * 2;
constexpr std::size_t indexLengthSize = sizeof(std::uint32_t);
constexpr std::size_t triangleIndexBytes = sizeof(std::uint16_t) * 3;

struct PuppetModelLayout final {
    std::string_view magic;
    PuppetModelVersion version;
    std::size_t vertexStride;
    std::size_t positionOffset;
    std::size_t uvOffset;
    std::optional<std::size_t> boneIndexOffset;
    std::optional<std::size_t> boneWeightOffset;
};

constexpr std::array puppetModelLayouts{
    PuppetModelLayout{
        "MDLV0013", PuppetModelVersion::mdlv0013, 52, 0, 44,
        std::nullopt, std::nullopt,
    },
    PuppetModelLayout{
        "MDLV0021", PuppetModelVersion::mdlv0021, 80, 0, 72, 40, 56,
    },
    PuppetModelLayout{
        "MDLV0023", PuppetModelVersion::mdlv0023, 80, 0, 72, 40, 56,
    },
};

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

[[nodiscard]] std::int32_t readInt32(
    std::span<const std::uint8_t> bytes,
    std::size_t offset
) noexcept {
    return static_cast<std::int32_t>(readUInt32(bytes, offset));
}

[[noreturn]] void fail(
    FormatErrorCode code,
    const std::string& source,
    std::size_t offset,
    std::string message
) {
    throw FormatError(code, source, offset, std::move(message));
}

[[nodiscard]] bool markerMatches(
    std::span<const std::uint8_t> bytes,
    std::size_t offset,
    std::string_view marker
) noexcept {
    if (!hasBytes(offset, marker.size() + 1, bytes.size()) ||
        bytes[offset + marker.size()] != 0) {
        return false;
    }
    for (std::size_t index = 0; index < marker.size(); ++index) {
        if (bytes[offset + index] != static_cast<std::uint8_t>(marker[index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::string readCString(
    std::span<const std::uint8_t> bytes,
    std::size_t& offset,
    std::size_t limit,
    const std::string& source,
    std::string_view description
) {
    const std::size_t start = offset;
    while (offset < limit && bytes[offset] != 0) ++offset;
    if (offset >= limit) {
        fail(
            FormatErrorCode::unexpectedEndOfFile,
            source,
            start,
            std::string(description) + " is not null terminated"
        );
    }
    std::string result;
    result.reserve(offset - start);
    for (std::size_t index = start; index < offset; ++index) {
        result.push_back(static_cast<char>(bytes[index]));
    }
    ++offset;
    return result;
}

[[nodiscard]] std::size_t checkedSectionEnd(
    std::span<const std::uint8_t> bytes,
    std::uint32_t serializedEnd,
    std::size_t minimum,
    const std::string& source,
    std::size_t offset,
    std::string_view description
) {
    // MDLS stores the next section offset. MDLA stores the inclusive final
    // byte. Accept the exact byte size as well for exporters that normalized
    // the latter to an exclusive end.
    std::size_t end = serializedEnd;
    if (end < bytes.size()) ++end;
    if (end < minimum || end > bytes.size()) {
        fail(
            FormatErrorCode::invalidOffset,
            source,
            offset,
            std::string(description) + " points outside the puppet model"
        );
    }
    return end;
}

void parsePuppetAnimations(
    std::span<const std::uint8_t> bytes,
    std::size_t offset,
    const std::string& source,
    PuppetMesh& mesh
) {
    if (offset >= bytes.size()) return;
    if (!markerMatches(bytes, offset, "MDLA0006")) {
        fail(
            FormatErrorCode::unsupportedFormat,
            source,
            offset,
            "Unsupported puppet animation header; expected MDLA0006"
        );
    }
    std::size_t cursor = offset + magicSize;
    if (!hasBytes(cursor, 8, bytes.size())) {
        fail(
            FormatErrorCode::unexpectedEndOfFile,
            source,
            cursor,
            "Puppet animation header is truncated"
        );
    }
    const std::uint32_t serializedEnd = readUInt32(bytes, cursor);
    const std::size_t limit = checkedSectionEnd(
        bytes, serializedEnd, cursor + 8, source, cursor,
        "Puppet animation section end"
    );
    cursor += 4;
    const std::uint32_t animationCount = readUInt32(bytes, cursor);
    cursor += 4;
    if (animationCount > 4096) {
        fail(
            FormatErrorCode::invalidValue,
            source,
            cursor - 4,
            "Puppet model declares an impractical animation count"
        );
    }
    mesh.animations.reserve(animationCount);
    for (std::uint32_t animationIndex = 0;
         animationIndex < animationCount;
         ++animationIndex) {
        if (!hasBytes(cursor, 8, limit)) {
            fail(
                FormatErrorCode::unexpectedEndOfFile,
                source,
                cursor,
                "Puppet animation record is truncated"
            );
        }
        PuppetAnimation animation;
        animation.id = readInt32(bytes, cursor);
        cursor += 4;
        cursor += 4;  // Reserved animation flags.
        animation.name = readCString(
            bytes, cursor, limit, source, "Puppet animation name"
        );
        animation.playbackMode = readCString(
            bytes, cursor, limit, source, "Puppet animation playback mode"
        );
        if (!hasBytes(cursor, 16, limit)) {
            fail(
                FormatErrorCode::unexpectedEndOfFile,
                source,
                cursor,
                "Puppet animation timing record is truncated"
            );
        }
        animation.framesPerSecond = readFloat32(bytes, cursor);
        cursor += 4;
        animation.frameCount = readUInt32(bytes, cursor);
        cursor += 4;
        cursor += 4;  // Reserved timeline flags.
        const std::uint32_t trackCount = readUInt32(bytes, cursor);
        cursor += 4;
        if (!std::isfinite(animation.framesPerSecond) ||
            animation.framesPerSecond <= 0.0F ||
            animation.frameCount == 0 || animation.frameCount > 1'000'000 ||
            trackCount != mesh.bones.size()) {
            fail(
                FormatErrorCode::invalidValue,
                source,
                cursor - 16,
                "Puppet animation timing or bone-track count is invalid"
            );
        }
        animation.boneTracks.reserve(trackCount);
        for (std::uint32_t trackIndex = 0; trackIndex < trackCount; ++trackIndex) {
            if (!hasBytes(cursor, 8, limit)) {
                fail(
                    FormatErrorCode::unexpectedEndOfFile,
                    source,
                    cursor,
                    "Puppet animation track header is truncated"
                );
            }
            cursor += 4;  // Reserved track flags.
            const std::uint32_t trackBytes = readUInt32(bytes, cursor);
            cursor += 4;
            constexpr std::size_t sampleBytes = sizeof(float) * 9;
            if (trackBytes % sampleBytes != 0 ||
                !hasBytes(cursor, trackBytes, limit)) {
                fail(
                    FormatErrorCode::invalidValue,
                    source,
                    cursor - 4,
                    "Puppet animation track has an invalid byte length"
                );
            }
            const std::size_t sampleCount = trackBytes / sampleBytes;
            if (sampleCount < static_cast<std::size_t>(animation.frameCount) + 1) {
                fail(
                    FormatErrorCode::invalidValue,
                    source,
                    cursor,
                    "Puppet animation track does not cover its declared frame count"
                );
            }
            std::vector<PuppetTransformSample> track;
            track.reserve(sampleCount);
            for (std::size_t sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex) {
                PuppetTransformSample sample;
                for (std::size_t component = 0; component < 3; ++component) {
                    sample.translation[component] = readFloat32(bytes, cursor);
                    cursor += 4;
                }
                for (std::size_t component = 0; component < 3; ++component) {
                    sample.rotation[component] = readFloat32(bytes, cursor);
                    cursor += 4;
                }
                for (std::size_t component = 0; component < 3; ++component) {
                    sample.scale[component] = readFloat32(bytes, cursor);
                    cursor += 4;
                }
                const auto finiteComponents = [](const std::array<float, 3>& value) {
                    return std::ranges::all_of(value, [](float component) {
                        return std::isfinite(component);
                    });
                };
                if (!finiteComponents(sample.translation) ||
                    !finiteComponents(sample.rotation) ||
                    !finiteComponents(sample.scale)) {
                    fail(
                        FormatErrorCode::invalidValue,
                        source,
                        cursor - sampleBytes,
                        "Puppet animation sample contains a non-finite component"
                    );
                }
                track.push_back(sample);
            }
            animation.boneTracks.push_back(std::move(track));
        }
        // MDLA0006 appends one reserved transform-sized record per animation.
        constexpr std::size_t footerBytes = sizeof(float) * 9;
        if (!hasBytes(cursor, footerBytes, limit)) {
            fail(
                FormatErrorCode::unexpectedEndOfFile,
                source,
                cursor,
                "Puppet animation footer is truncated"
            );
        }
        cursor += footerBytes;
        if (std::ranges::any_of(mesh.animations, [&](const PuppetAnimation& item) {
                return item.id == animation.id;
            })) {
            fail(
                FormatErrorCode::duplicateEntry,
                source,
                offset,
                "Puppet model contains duplicate animation id " +
                    std::to_string(animation.id)
            );
        }
        mesh.animations.push_back(std::move(animation));
    }
    if (cursor != limit) {
        fail(
            FormatErrorCode::invalidValue,
            source,
            cursor,
            "Puppet animation section contains unconsumed data"
        );
    }
}

void parsePuppetSkeleton(
    std::span<const std::uint8_t> bytes,
    std::size_t offset,
    const std::string& source,
    PuppetMesh& mesh
) {
    if (!markerMatches(bytes, offset, "MDLS0003")) {
        fail(
            FormatErrorCode::unsupportedFormat,
            source,
            offset,
            "Unsupported puppet skeleton header; expected MDLS0003"
        );
    }
    std::size_t cursor = offset + magicSize;
    if (!hasBytes(cursor, 8, bytes.size())) {
        fail(
            FormatErrorCode::unexpectedEndOfFile,
            source,
            cursor,
            "Puppet skeleton header is truncated"
        );
    }
    const std::uint32_t animationOffset = readUInt32(bytes, cursor);
    cursor += 4;
    const std::uint32_t boneCount = readUInt32(bytes, cursor);
    cursor += 4;
    if (animationOffset <= cursor || animationOffset > bytes.size() ||
        boneCount == 0 || boneCount > 4096) {
        fail(
            FormatErrorCode::invalidValue,
            source,
            offset + magicSize,
            "Puppet skeleton section offsets or bone count are invalid"
        );
    }
    mesh.bones.reserve(boneCount);
    for (std::uint32_t boneIndex = 0; boneIndex < boneCount; ++boneIndex) {
        if (!hasBytes(cursor, 13, animationOffset)) {
            fail(
                FormatErrorCode::unexpectedEndOfFile,
                source,
                cursor,
                "Puppet bone record is truncated"
            );
        }
        cursor += 1;  // Reserved bone flags.
        const std::uint32_t recordVersion = readUInt32(bytes, cursor);
        cursor += 4;
        const std::int32_t parent = readInt32(bytes, cursor);
        cursor += 4;
        const std::uint32_t matrixBytes = readUInt32(bytes, cursor);
        cursor += 4;
        if (recordVersion != 1 || matrixBytes != sizeof(float) * 16 ||
            parent < -1 || parent >= static_cast<std::int32_t>(boneIndex) ||
            !hasBytes(cursor, matrixBytes, animationOffset)) {
            fail(
                FormatErrorCode::invalidValue,
                source,
                cursor - 13,
                "Puppet bone hierarchy or bind matrix is invalid"
            );
        }
        PuppetBone bone;
        bone.parent = parent;
        for (float& component : bone.bindLocalMatrix) {
            component = readFloat32(bytes, cursor);
            cursor += 4;
            if (!std::isfinite(component)) {
                fail(
                    FormatErrorCode::invalidValue,
                    source,
                    cursor - 4,
                    "Puppet bind matrix contains a non-finite component"
                );
            }
        }
        // Constraint JSON is preserved in the binary but not required for
        // deterministic timeline skinning. Consume it explicitly so malformed
        // records still fail at the correct boundary.
        (void)readCString(
            bytes, cursor, animationOffset, source, "Puppet bone constraint"
        );
        mesh.bones.push_back(std::move(bone));
    }
    parsePuppetAnimations(bytes, animationOffset, source, mesh);
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
    const PuppetModelLayout* layout = nullptr;
    for (const PuppetModelLayout& candidate : puppetModelLayouts) {
        if (magicMatches(candidate.magic)) {
            layout = &candidate;
            break;
        }
    }
    if (!layout) {
        fail(
            FormatErrorCode::unsupportedFormat,
            source,
            0,
            "Unsupported puppet model header; expected MDLV0013, MDLV0021, or MDLV0023"
        );
    }

    // MDLS terminates the model metadata. Linux treats the first marker as
    // the scan limit; retaining that rule is important for existing assets.
    std::size_t limit = bytes.size();
    std::optional<std::size_t> skeletonOffset;
    for (std::size_t offset = magicSize; hasBytes(offset, 4, bytes.size()); ++offset) {
        if (bytes[offset] == 'M' && bytes[offset + 1] == 'D' &&
            bytes[offset + 2] == 'L' && bytes[offset + 3] == 'S') {
            limit = offset;
            skeletonOffset = offset;
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
        if (vertexBytes == 0 || vertexBytes % layout->vertexStride != 0) {
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
    const std::size_t vertexCount = selected.vertexBytes / layout->vertexStride;
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
        .version = layout->version,
        .vertices = {},
        .indices = {},
        .bones = {},
        .animations = {},
    };
    result.vertices.reserve(vertexCount);
    result.indices.reserve(indexCount);

    for (std::size_t index = 0; index < vertexCount; ++index) {
        const std::size_t vertexOffset =
            verticesOffset + index * layout->vertexStride;
        const float x = readFloat32(
            bytes, vertexOffset + layout->positionOffset
        );
        const float y = readFloat32(
            bytes, vertexOffset + layout->positionOffset + 4
        );
        const float z = readFloat32(
            bytes, vertexOffset + layout->positionOffset + 8
        );
        const float u = readFloat32(bytes, vertexOffset + layout->uvOffset);
        const float v = readFloat32(bytes, vertexOffset + layout->uvOffset + 4);
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
        PuppetVertex& vertex = result.vertices.back();
        if (layout->boneIndexOffset && layout->boneWeightOffset) {
            for (std::size_t influence = 0; influence < 4; ++influence) {
                vertex.boneIndices[influence] = readUInt32(
                    bytes,
                    vertexOffset + *layout->boneIndexOffset + influence * 4
                );
                vertex.boneWeights[influence] = readFloat32(
                    bytes,
                    vertexOffset + *layout->boneWeightOffset + influence * 4
                );
                if (!std::isfinite(vertex.boneWeights[influence]) ||
                    vertex.boneWeights[influence] < 0.0F) {
                    fail(
                        FormatErrorCode::invalidValue,
                        result.source,
                        vertexOffset + *layout->boneWeightOffset + influence * 4,
                        "Puppet vertex contains an invalid bone weight"
                    );
                }
            }
        }
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

    if (skeletonOffset && markerMatches(bytes, *skeletonOffset, "MDLS0003")) {
        if (!layout->boneIndexOffset || !layout->boneWeightOffset) {
            fail(
                FormatErrorCode::unsupportedFormat,
                result.source,
                *skeletonOffset,
                "This puppet model version has animation data but no supported skin weights"
            );
        }
        parsePuppetSkeleton(bytes, *skeletonOffset, result.source, result);
        for (std::size_t vertexIndex = 0;
             vertexIndex < result.vertices.size();
             ++vertexIndex) {
            const PuppetVertex& vertex = result.vertices[vertexIndex];
            for (std::size_t influence = 0; influence < 4; ++influence) {
                if (vertex.boneWeights[influence] > 0.0F &&
                    vertex.boneIndices[influence] >= result.bones.size()) {
                    fail(
                        FormatErrorCode::invalidValue,
                        result.source,
                        verticesOffset + vertexIndex * layout->vertexStride +
                            *layout->boneIndexOffset + influence * 4,
                        "Puppet vertex references a bone outside the skeleton"
                    );
                }
            }
        }
    } else if (skeletonOffset && bytes.size() - *skeletonOffset != 4) {
        fail(
            FormatErrorCode::unsupportedFormat,
            result.source,
            *skeletonOffset,
            "Unsupported puppet skeleton header; expected MDLS0003"
        );
    }

    return result;
}

const PuppetAnimation* PuppetMesh::animation(int id) const noexcept {
    const auto found = std::ranges::find_if(
        animations,
        [id](const PuppetAnimation& item) { return item.id == id; }
    );
    return found == animations.end() ? nullptr : &*found;
}

namespace {

using PuppetMatrix = std::array<double, 16>;

[[nodiscard]] PuppetMatrix puppetMultiply(
    const PuppetMatrix& lhs,
    const PuppetMatrix& rhs
) noexcept {
    PuppetMatrix result{};
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            for (std::size_t inner = 0; inner < 4; ++inner) {
                result[row * 4 + column] +=
                    lhs[row * 4 + inner] * rhs[inner * 4 + column];
            }
        }
    }
    return result;
}

[[nodiscard]] PuppetMatrix puppetInverse(const PuppetMatrix& source) {
    double augmented[4][8]{};
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            augmented[row][column] = source[row * 4 + column];
        }
        augmented[row][row + 4] = 1.0;
    }
    for (std::size_t column = 0; column < 4; ++column) {
        std::size_t pivot = column;
        for (std::size_t row = column + 1; row < 4; ++row) {
            if (std::abs(augmented[row][column]) >
                std::abs(augmented[pivot][column])) {
                pivot = row;
            }
        }
        if (std::abs(augmented[pivot][column]) <= 1e-12) {
            throw std::invalid_argument("Puppet bind matrix is singular");
        }
        if (pivot != column) {
            for (std::size_t value = 0; value < 8; ++value) {
                std::swap(augmented[pivot][value], augmented[column][value]);
            }
        }
        const double divisor = augmented[column][column];
        for (double& value : augmented[column]) value /= divisor;
        for (std::size_t row = 0; row < 4; ++row) {
            if (row == column) continue;
            const double factor = augmented[row][column];
            for (std::size_t value = 0; value < 8; ++value) {
                augmented[row][value] -= factor * augmented[column][value];
            }
        }
    }
    PuppetMatrix result{};
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            result[row * 4 + column] = augmented[row][column + 4];
        }
    }
    return result;
}

[[nodiscard]] PuppetMatrix puppetMatrix(
    const PuppetTransformSample& transform
) noexcept {
    const double cx = std::cos(transform.rotation[0]);
    const double sx = std::sin(transform.rotation[0]);
    const double cy = std::cos(transform.rotation[1]);
    const double sy = std::sin(transform.rotation[1]);
    const double cz = std::cos(transform.rotation[2]);
    const double sz = std::sin(transform.rotation[2]);
    const double scaleX = transform.scale[0];
    const double scaleY = transform.scale[1];
    const double scaleZ = transform.scale[2];
    return {
        scaleX * cy * cz,
        scaleX * cy * sz,
        scaleX * -sy,
        0.0,
        scaleY * (sx * sy * cz - cx * sz),
        scaleY * (sx * sy * sz + cx * cz),
        scaleY * sx * cy,
        0.0,
        scaleZ * (cx * sy * cz + sx * sz),
        scaleZ * (cx * sy * sz - sx * cz),
        scaleZ * cx * cy,
        0.0,
        transform.translation[0],
        transform.translation[1],
        transform.translation[2],
        1.0,
    };
}

[[nodiscard]] PuppetTransformSample puppetDecompose(
    const PuppetMatrix& matrix
) {
    PuppetTransformSample result;
    result.translation = {
        static_cast<float>(matrix[12]),
        static_cast<float>(matrix[13]),
        static_cast<float>(matrix[14]),
    };
    const auto rowLength = [&](std::size_t row) {
        return std::sqrt(
            matrix[row * 4] * matrix[row * 4] +
            matrix[row * 4 + 1] * matrix[row * 4 + 1] +
            matrix[row * 4 + 2] * matrix[row * 4 + 2]
        );
    };
    const double scaleX = rowLength(0);
    const double scaleY = rowLength(1);
    const double scaleZ = rowLength(2);
    if (scaleX <= 1e-12 || scaleY <= 1e-12 || scaleZ <= 1e-12) {
        throw std::invalid_argument("Puppet bind transform has zero scale");
    }
    result.scale = {
        static_cast<float>(scaleX),
        static_cast<float>(scaleY),
        static_cast<float>(scaleZ),
    };
    const double r00 = matrix[0] / scaleX;
    const double r01 = matrix[1] / scaleX;
    const double r02 = matrix[2] / scaleX;
    const double r12 = matrix[6] / scaleY;
    const double r22 = matrix[10] / scaleZ;
    const double r10 = matrix[4] / scaleY;
    const double r11 = matrix[5] / scaleY;
    const double y = std::asin(std::clamp(-r02, -1.0, 1.0));
    const double cosineY = std::cos(y);
    double x = 0.0;
    double z = 0.0;
    if (std::abs(cosineY) > 1e-8) {
        x = std::atan2(r12, r22);
        z = std::atan2(r01, r00);
    } else {
        x = std::atan2(-r10, r11);
    }
    result.rotation = {
        static_cast<float>(x),
        static_cast<float>(y),
        static_cast<float>(z),
    };
    return result;
}

[[nodiscard]] float puppetMix(float from, float to, float amount) noexcept {
    return from + (to - from) * amount;
}

[[nodiscard]] float puppetMixAngle(float from, float to, float amount) noexcept {
    constexpr float fullRotation = 2.0F * std::numbers::pi_v<float>;
    const float delta = std::remainder(to - from, fullRotation);
    return from + delta * amount;
}

[[nodiscard]] PuppetTransformSample puppetSample(
    const PuppetAnimation& animation,
    std::size_t boneIndex,
    double timeSeconds
) {
    if (!std::isfinite(timeSeconds)) {
        throw std::invalid_argument("Puppet animation time must be finite");
    }
    const double duration =
        static_cast<double>(animation.frameCount) / animation.framesPerSecond;
    double localTime = timeSeconds;
    if (animation.playbackMode == "loop") {
        localTime = std::fmod(localTime, duration);
        if (localTime < 0.0) localTime += duration;
    } else {
        localTime = std::clamp(localTime, 0.0, duration);
    }
    const double framePosition = localTime * animation.framesPerSecond;
    const std::size_t lower = std::min(
        static_cast<std::size_t>(std::floor(framePosition)),
        static_cast<std::size_t>(animation.frameCount)
    );
    const std::size_t upper = std::min(
        lower + 1,
        static_cast<std::size_t>(animation.frameCount)
    );
    const float fraction = static_cast<float>(framePosition - std::floor(framePosition));
    const auto& track = animation.boneTracks.at(boneIndex);
    const PuppetTransformSample& from = track.at(lower);
    const PuppetTransformSample& to = track.at(upper);
    PuppetTransformSample result;
    for (std::size_t component = 0; component < 3; ++component) {
        result.translation[component] = puppetMix(
            from.translation[component], to.translation[component], fraction
        );
        result.rotation[component] = puppetMixAngle(
            from.rotation[component], to.rotation[component], fraction
        );
        result.scale[component] = puppetMix(
            from.scale[component], to.scale[component], fraction
        );
    }
    return result;
}

}  // namespace

std::vector<std::array<float, 3>> evaluatePuppetPositions(
    const PuppetMesh& mesh,
    std::span<const PuppetAnimationLayerInput> layers
) {
    std::vector<std::array<float, 3>> result;
    result.reserve(mesh.vertices.size());
    if (mesh.bones.empty() || layers.empty()) {
        for (const PuppetVertex& vertex : mesh.vertices) {
            result.push_back({
                vertex.position[0], vertex.position[1], vertex.position[2],
            });
        }
        return result;
    }

    std::vector<PuppetMatrix> bindLocal;
    std::vector<PuppetMatrix> bindGlobal(mesh.bones.size());
    std::vector<PuppetTransformSample> pose;
    bindLocal.reserve(mesh.bones.size());
    pose.reserve(mesh.bones.size());
    for (std::size_t boneIndex = 0; boneIndex < mesh.bones.size(); ++boneIndex) {
        PuppetMatrix matrix{};
        std::ranges::transform(
            mesh.bones[boneIndex].bindLocalMatrix,
            matrix.begin(),
            [](float component) { return static_cast<double>(component); }
        );
        bindLocal.push_back(matrix);
        pose.push_back(puppetDecompose(matrix));
        const int parent = mesh.bones[boneIndex].parent;
        bindGlobal[boneIndex] = parent < 0
            ? matrix
            : puppetMultiply(matrix, bindGlobal[static_cast<std::size_t>(parent)]);
    }

    for (const PuppetAnimationLayerInput& layer : layers) {
        if (!std::isfinite(layer.timeSeconds) || !std::isfinite(layer.blend) ||
            layer.blend < 0.0F || layer.blend > 1.0F) {
            throw std::invalid_argument(
                "Puppet animation layer time and blend must be finite, with blend in 0...1"
            );
        }
        if (layer.blend == 0.0F) continue;
        const PuppetAnimation* animation = mesh.animation(layer.animationId);
        if (animation == nullptr) {
            throw std::invalid_argument(
                "Puppet animation layer references unknown animation id " +
                std::to_string(layer.animationId)
            );
        }
        for (std::size_t boneIndex = 0; boneIndex < mesh.bones.size(); ++boneIndex) {
            const PuppetTransformSample sampled = puppetSample(
                *animation, boneIndex, layer.timeSeconds
            );
            if (layer.additive) {
                const PuppetTransformSample& base =
                    animation->boneTracks[boneIndex].front();
                for (std::size_t component = 0; component < 3; ++component) {
                    pose[boneIndex].translation[component] +=
                        (sampled.translation[component] - base.translation[component]) *
                        layer.blend;
                    pose[boneIndex].rotation[component] += std::remainder(
                        sampled.rotation[component] - base.rotation[component],
                        2.0F * std::numbers::pi_v<float>
                    ) * layer.blend;
                    if (std::abs(base.scale[component]) <= 1e-8F) {
                        throw std::invalid_argument(
                            "Additive puppet animation has a zero base scale"
                        );
                    }
                    pose[boneIndex].scale[component] *= puppetMix(
                        1.0F,
                        sampled.scale[component] / base.scale[component],
                        layer.blend
                    );
                }
            } else {
                for (std::size_t component = 0; component < 3; ++component) {
                    pose[boneIndex].translation[component] = puppetMix(
                        pose[boneIndex].translation[component],
                        sampled.translation[component],
                        layer.blend
                    );
                    pose[boneIndex].rotation[component] = puppetMixAngle(
                        pose[boneIndex].rotation[component],
                        sampled.rotation[component],
                        layer.blend
                    );
                    pose[boneIndex].scale[component] = puppetMix(
                        pose[boneIndex].scale[component],
                        sampled.scale[component],
                        layer.blend
                    );
                }
            }
        }
    }

    std::vector<PuppetMatrix> currentGlobal(mesh.bones.size());
    std::vector<PuppetMatrix> skinMatrices(mesh.bones.size());
    for (std::size_t boneIndex = 0; boneIndex < mesh.bones.size(); ++boneIndex) {
        const PuppetMatrix local = puppetMatrix(pose[boneIndex]);
        const int parent = mesh.bones[boneIndex].parent;
        currentGlobal[boneIndex] = parent < 0
            ? local
            : puppetMultiply(
                local, currentGlobal[static_cast<std::size_t>(parent)]
            );
        skinMatrices[boneIndex] = puppetMultiply(
            puppetInverse(bindGlobal[boneIndex]),
            currentGlobal[boneIndex]
        );
    }

    for (const PuppetVertex& vertex : mesh.vertices) {
        std::array<double, 3> deformed{};
        double weightSum = 0.0;
        for (std::size_t influence = 0; influence < 4; ++influence) {
            const double weight = vertex.boneWeights[influence];
            if (weight <= 0.0) continue;
            const PuppetMatrix& matrix = skinMatrices.at(
                vertex.boneIndices[influence]
            );
            const double x = vertex.position[0];
            const double y = vertex.position[1];
            const double z = vertex.position[2];
            deformed[0] += weight * (
                x * matrix[0] + y * matrix[4] + z * matrix[8] + matrix[12]
            );
            deformed[1] += weight * (
                x * matrix[1] + y * matrix[5] + z * matrix[9] + matrix[13]
            );
            deformed[2] += weight * (
                x * matrix[2] + y * matrix[6] + z * matrix[10] + matrix[14]
            );
            weightSum += weight;
        }
        if (weightSum <= 1e-8) {
            result.push_back({
                vertex.position[0], vertex.position[1], vertex.position[2],
            });
        } else {
            result.push_back({
                static_cast<float>(deformed[0] / weightSum),
                static_cast<float>(deformed[1] / weightSum),
                static_cast<float>(deformed[2] / weightSum),
            });
        }
    }
    return result;
}

}  // namespace we::scene
