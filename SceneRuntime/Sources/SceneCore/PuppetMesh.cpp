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
constexpr std::uint32_t modelFlagPosition3 = 0x00000001U;
constexpr std::uint32_t modelFlagNormal = 0x00000002U;
constexpr std::uint32_t modelFlagTangent = 0x00000004U;
constexpr std::uint32_t modelFlagUv = 0x00000008U;
constexpr std::uint32_t modelFlagUv2 = 0x00000020U;
constexpr std::uint32_t modelFlagPosition4 = 0x00010000U;
constexpr std::uint32_t modelFlagSkinBlend = 0x00800000U;
constexpr std::uint32_t modelFlagSkinWeight = 0x01000000U;
constexpr std::uint32_t supportedModelFlags = modelFlagPosition3 |
    modelFlagNormal |
    modelFlagTangent | modelFlagUv | modelFlagUv2 | modelFlagPosition4 |
    modelFlagSkinBlend | modelFlagSkinWeight;
constexpr std::size_t maximumPuppetEntries = 4096;
constexpr std::size_t maximumPuppetFrames = 1'000'000;
constexpr std::uint32_t morphPayload400 = 0x00000400U;
constexpr std::uint32_t morphPayload800 = 0x00000800U;
constexpr std::uint32_t morphPayload1000 = 0x00001000U;
constexpr std::uint32_t morphModifier = 0x00002000U;
constexpr std::size_t maximumMorphTargets = puppetMaximumMorphTargets;

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

[[nodiscard]] std::uint16_t readUInt16(
    std::span<const std::uint8_t> bytes,
    std::size_t offset
) noexcept {
    return static_cast<std::uint16_t>(bytes[offset]) |
        static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(bytes[offset + 1]) << 8U
        );
}

[[nodiscard]] std::uint64_t readUInt64(
    std::span<const std::uint8_t> bytes,
    std::size_t offset
) noexcept {
    return static_cast<std::uint64_t>(readUInt32(bytes, offset)) |
        (static_cast<std::uint64_t>(readUInt32(bytes, offset + 4)) << 32U);
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
    const std::size_t end = serializedEnd;
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

[[nodiscard]] int sectionVersion(
    std::span<const std::uint8_t> bytes,
    std::size_t offset,
    std::string_view prefix
) noexcept {
    if (prefix.size() != 4 || !hasBytes(offset, magicSize, bytes.size()) ||
        bytes[offset + 8] != 0) {
        return -1;
    }
    for (std::size_t index = 0; index < prefix.size(); ++index) {
        if (bytes[offset + index] != static_cast<std::uint8_t>(prefix[index])) {
            return -1;
        }
    }
    int result = 0;
    for (std::size_t index = 4; index < 8; ++index) {
        const std::uint8_t digit = bytes[offset + index];
        if (digit < '0' || digit > '9') return -1;
        result = result * 10 + static_cast<int>(digit - '0');
    }
    return result;
}

void requireBytes(
    std::size_t cursor,
    std::size_t count,
    std::size_t limit,
    const std::string& source,
    std::string_view description
) {
    if (!hasBytes(cursor, count, limit)) {
        fail(
            FormatErrorCode::unexpectedEndOfFile,
            source,
            cursor,
            std::string(description) + " is truncated"
        );
    }
}

[[nodiscard]] std::vector<std::vector<float>> parseFloatCurveRecords(
    std::span<const std::uint8_t> bytes,
    std::size_t& cursor,
    std::size_t limit,
    std::uint32_t curveCount,
    std::size_t expectedSampleCount,
    const std::string& source,
    std::string_view description
) {
    std::vector<std::vector<float>> result;
    result.reserve(curveCount);
    for (std::uint32_t index = 0; index < curveCount; ++index) {
        requireBytes(cursor, 8, limit, source, description);
        cursor += 4;  // Exporter-owned curve identity.
        const std::uint32_t byteCount = readUInt32(bytes, cursor);
        cursor += 4;
        if (byteCount % sizeof(float) != 0 ||
            byteCount / sizeof(float) != expectedSampleCount ||
            !hasBytes(cursor, byteCount, limit)) {
            fail(
                FormatErrorCode::invalidValue,
                source,
                cursor - 8,
                std::string(description) + " has an invalid curve record"
            );
        }
        std::vector<float> samples;
        samples.reserve(byteCount / sizeof(float));
        while (samples.size() < byteCount / sizeof(float)) {
            const float value = readFloat32(bytes, cursor);
            cursor += sizeof(float);
            if (!std::isfinite(value)) {
                fail(
                    FormatErrorCode::invalidValue,
                    source,
                    cursor - sizeof(float),
                    std::string(description) + " contains a non-finite sample"
                );
            }
            samples.push_back(value);
        }
        result.push_back(std::move(samples));
    }
    return result;
}

[[nodiscard]] std::vector<std::vector<float>>
parseOptionalFloatCurveRecords(
    std::span<const std::uint8_t> bytes,
    std::size_t& cursor,
    std::size_t limit,
    std::uint32_t curveCount,
    std::size_t expectedSampleCount,
    const std::string& source,
    std::string_view description
) {
    requireBytes(cursor, 1, limit, source, description);
    const std::uint8_t present = bytes[cursor++];
    if (present > 1) {
        fail(
            FormatErrorCode::invalidValue, source, cursor - 1,
            std::string(description) + " presence flag must be zero or one"
        );
    }
    if (present == 0) return {};
    return parseFloatCurveRecords(
        bytes, cursor, limit, curveCount, expectedSampleCount, source,
        description
    );
}

void parsePuppetAnimations(
    std::span<const std::uint8_t> bytes,
    std::size_t offset,
    const std::string& source,
    PuppetMesh& mesh
) {
    const int version = sectionVersion(bytes, offset, "MDLA");
    if (version < 1 || version > 6) {
        fail(
            FormatErrorCode::unsupportedFormat,
            source,
            offset,
            "Unsupported puppet animation header; expected MDLA0001 through MDLA0006"
        );
    }
    std::size_t cursor = offset + magicSize;
    requireBytes(cursor, 8, bytes.size(), source, "Puppet animation header");
    const std::size_t limit = checkedSectionEnd(
        bytes, readUInt32(bytes, cursor), cursor + 8, source, cursor,
        "Puppet animation section end"
    );
    cursor += 4;
    const std::uint32_t animationCount = readUInt32(bytes, cursor);
    cursor += 4;
    if (animationCount > maximumPuppetEntries) {
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
        const std::size_t recordOffset = cursor;
        requireBytes(cursor, 8, limit, source, "Puppet animation record");
        PuppetAnimation animation;
        animation.id = readInt32(bytes, cursor);
        cursor += 8;  // Id followed by an exporter-reserved word.
        animation.name = readCString(
            bytes, cursor, limit, source, "Puppet animation name"
        );
        if (animation.name.empty()) {
            animation.name = readCString(
                bytes, cursor, limit, source, "Puppet animation alternate name"
            );
        }
        animation.playbackMode = readCString(
            bytes, cursor, limit, source, "Puppet animation playback mode"
        );
        requireBytes(cursor, 16, limit, source, "Puppet animation timing record");
        animation.framesPerSecond = readFloat32(bytes, cursor);
        cursor += 4;
        const std::int32_t frameCount = readInt32(bytes, cursor);
        cursor += 4;
        const std::int32_t timingReserved = readInt32(bytes, cursor);
        cursor += 4;
        const std::uint32_t trackCount = readUInt32(bytes, cursor);
        cursor += 4;
        if (!std::isfinite(animation.framesPerSecond) ||
            animation.framesPerSecond <= 0.0F || frameCount < 0 ||
            static_cast<std::size_t>(frameCount) > maximumPuppetFrames ||
            timingReserved != 0 || trackCount != mesh.bones.size()) {
            fail(
                FormatErrorCode::invalidValue,
                source,
                cursor - 16,
                "Puppet animation timing or bone-track count is invalid"
            );
        }
        animation.frameCount = static_cast<std::uint32_t>(frameCount);
        animation.boneTracks.reserve(trackCount);
        for (std::uint32_t trackIndex = 0; trackIndex < trackCount; ++trackIndex) {
            requireBytes(cursor, 8, limit, source, "Puppet animation track header");
            cursor += 4;  // Exporter-reserved track word.
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
            if (sampleCount !=
                static_cast<std::size_t>(animation.frameCount) + 1) {
                fail(
                    FormatErrorCode::invalidValue,
                    source,
                    cursor,
                    "Puppet animation track does not match its declared frame count"
                );
            }
            std::vector<PuppetTransformSample> track;
            track.reserve(sampleCount);
            for (std::size_t sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex) {
                PuppetTransformSample sample;
                for (float& component : sample.translation) {
                    component = readFloat32(bytes, cursor);
                    cursor += 4;
                }
                for (float& component : sample.rotation) {
                    component = readFloat32(bytes, cursor);
                    cursor += 4;
                }
                for (float& component : sample.scale) {
                    component = readFloat32(bytes, cursor);
                    cursor += 4;
                }
                const auto finite = [](const std::array<float, 3>& value) {
                    return std::ranges::all_of(value, [](float component) {
                        return std::isfinite(component);
                    });
                };
                if (!finite(sample.translation) || !finite(sample.rotation) ||
                    !finite(sample.scale)) {
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

        if (version >= 3) {
            const std::size_t sampleCount =
                static_cast<std::size_t>(animation.frameCount) + 1;

            // MDLA stores BlendMap curves first. The leading word is the
            // number of records, not a boolean translation-track flag. Each
            // record carries an exporter identity, byte count, and samples.
            requireBytes(
                cursor, sizeof(std::uint32_t), limit, source,
                "Puppet animation BlendMap curve count"
            );
            const std::uint32_t blendMapCurveCount = readUInt32(bytes, cursor);
            cursor += sizeof(std::uint32_t);
            if (blendMapCurveCount > puppetMaximumBlendMapScalars) {
                fail(
                    FormatErrorCode::invalidValue, source,
                    cursor - sizeof(std::uint32_t),
                    "Puppet animation declares more than 16 BlendMap curves"
                );
            }
            animation.blendMapTracks = parseFloatCurveRecords(
                bytes, cursor, limit, blendMapCurveCount, sampleCount, source,
                "Puppet animation BlendMap curves"
            );

            // The following optional group is indexed by bone and feeds the
            // runtime's g_BonesAlpha array. It is independent of BlendMap.
            animation.bonesAlphaTracks = parseOptionalFloatCurveRecords(
                bytes, cursor, limit, mesh.bones.size(), sampleCount, source,
                "Puppet animation BonesAlpha curves"
            );
        }

        if (version >= 4) {
            requireBytes(cursor, 1, limit, source, "Puppet morph track flag");
            const std::uint8_t hasMorphTracks = bytes[cursor++];
            if (hasMorphTracks > 1) {
                fail(
                    FormatErrorCode::invalidValue, source, cursor - 1,
                    "Puppet morph track flag must be zero or one"
                );
            }
            if (hasMorphTracks == 1) {
                animation.morphTracks.reserve(mesh.submeshes.size());
                for (std::size_t meshIndex = 0;
                     meshIndex < mesh.submeshes.size();
                     ++meshIndex) {
                    requireBytes(cursor, 4, limit, source, "Puppet morph track flags");
                    PuppetAnimation::MorphTrack track;
                    track.flags = readUInt32(bytes, cursor);
                    cursor += 4;
                    if ((track.flags & 1U) == 0) {
                        animation.morphTracks.push_back(std::move(track));
                        continue;
                    }
                    requireBytes(cursor, 6, limit, source, "Puppet morph track header");
                    track.scale = readFloat32(bytes, cursor);
                    cursor += 4;
                    const std::uint16_t curveCount = readUInt16(bytes, cursor);
                    cursor += 2;
                    if (!std::isfinite(track.scale) || track.scale <= 0.0F ||
                        curveCount == 0 || curveCount > maximumMorphTargets) {
                        fail(
                            FormatErrorCode::invalidValue, source, cursor - 6,
                            "Puppet morph track scale or curve count is invalid"
                        );
                    }
                    track.curves.reserve(curveCount);
                    for (std::uint16_t curveIndex = 0;
                         curveIndex < curveCount;
                         ++curveIndex) {
                        PuppetAnimation::MorphCurve curve;
                        requireBytes(cursor, 6, limit, source, "Puppet morph curve header");
                        curve.id = readUInt16(bytes, cursor);
                        cursor += 2;
                        const std::uint32_t curveBytes = readUInt32(bytes, cursor);
                        cursor += 4;
                        if (curve.id >= maximumMorphTargets ||
                            curveBytes % sizeof(float) != 0 ||
                            curveBytes / sizeof(float) !=
                                static_cast<std::size_t>(animation.frameCount) + 1 ||
                            !hasBytes(cursor, curveBytes, limit)) {
                            fail(
                                FormatErrorCode::invalidValue, source, cursor - 4,
                                "Puppet morph curve has an invalid id or sample count"
                            );
                        }
                        curve.values.reserve(curveBytes / sizeof(float));
                        while (curve.values.size() < curveBytes / sizeof(float)) {
                            const float value = readFloat32(bytes, cursor);
                            cursor += 4;
                            if (!std::isfinite(value)) {
                                fail(
                                    FormatErrorCode::invalidValue, source,
                                    cursor - 4,
                                    "Puppet morph curve contains a non-finite sample"
                                );
                            }
                            curve.values.push_back(value);
                        }
                        track.curves.push_back(std::move(curve));
                    }
                    animation.morphTracks.push_back(std::move(track));
                }
            }
        }

        if (version >= 5) {
            requireBytes(cursor, sizeof(float) * 6, limit, source, "Puppet animation bounds");
            for (std::size_t component = 0; component < 6; ++component) {
                const float value = readFloat32(bytes, cursor);
                cursor += 4;
                if (!std::isfinite(value)) {
                    fail(
                        FormatErrorCode::invalidValue, source, cursor - 4,
                        "Puppet animation bounds contain a non-finite component"
                    );
                }
            }
        }
        if (version == 6) {
            animation.v6ScalarTracks = parseOptionalFloatCurveRecords(
                bytes, cursor, limit, trackCount,
                static_cast<std::size_t>(animation.frameCount) + 1, source,
                "Puppet animation V6 scalar curves"
            );
        }

        requireBytes(cursor, 4, limit, source, "Puppet authored event count");
        const std::uint32_t authoredEventCount = readUInt32(bytes, cursor);
        cursor += 4;
        if (authoredEventCount > maximumPuppetEntries) {
            fail(
                FormatErrorCode::invalidValue, source, cursor - 4,
                "Puppet animation declares an impractical authored event count"
            );
        }
        for (std::uint32_t eventIndex = 0;
             eventIndex < authoredEventCount;
             ++eventIndex) {
            requireBytes(cursor, 4, limit, source, "Puppet authored event");
            cursor += 4;
            (void)readCString(
                bytes, cursor, limit, source, "Puppet authored event JSON"
            );
        }
        if (animationIndex + 1 < animationCount && hasBytes(cursor, 12, limit) &&
            readUInt32(bytes, cursor) == 0 &&
            readUInt32(bytes, cursor + 4) > 0 &&
            readUInt32(bytes, cursor + 4) <= 100'000 &&
            readUInt32(bytes, cursor + 8) == 0) {
            cursor += 4;
        }
        if (std::ranges::any_of(mesh.animations, [&](const PuppetAnimation& item) {
                return item.id == animation.id;
            })) {
            fail(
                FormatErrorCode::duplicateEntry,
                source,
                recordOffset,
                "Puppet model contains duplicate animation id " +
                    std::to_string(animation.id)
            );
        }
        mesh.animations.push_back(std::move(animation));
    }
    if (cursor + sizeof(std::uint32_t) == limit &&
        readUInt32(bytes, cursor) == 0) {
        cursor += sizeof(std::uint32_t);
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
    const int version = sectionVersion(bytes, offset, "MDLS");
    if (version < 1 || version > 4) {
        fail(
            FormatErrorCode::unsupportedFormat,
            source,
            offset,
            "Unsupported puppet skeleton header; expected MDLS0001 through MDLS0004"
        );
    }
    std::size_t cursor = offset + magicSize;
    requireBytes(cursor, 8, bytes.size(), source, "Puppet skeleton header");
    const std::size_t limit = checkedSectionEnd(
        bytes, readUInt32(bytes, cursor), cursor + 8, source, cursor,
        "Puppet skeleton section end"
    );
    cursor += 4;
    const std::uint16_t boneCount = readUInt16(bytes, cursor);
    cursor += 2;
    const std::uint16_t headerReserved = readUInt16(bytes, cursor);
    cursor += 2;
    if (boneCount == 0 || boneCount > maximumPuppetEntries ||
        headerReserved != 0) {
        fail(
            FormatErrorCode::invalidValue,
            source,
            offset + magicSize,
            "Puppet skeleton bone count or header padding is invalid"
        );
    }
    mesh.bones.reserve(boneCount);
    for (std::uint32_t boneIndex = 0; boneIndex < boneCount; ++boneIndex) {
        (void)readCString(bytes, cursor, limit, source, "Puppet bone name");
        requireBytes(cursor, 12, limit, source, "Puppet bone record");
        cursor += 4;  // Simulation type is consumed by the physics subsystem.
        const std::uint32_t serializedParent = readUInt32(bytes, cursor);
        cursor += 4;
        const std::uint32_t matrixBytes = readUInt32(bytes, cursor);
        cursor += 4;
        const int parent = serializedParent == std::numeric_limits<std::uint32_t>::max()
            ? -1
            : static_cast<int>(serializedParent);
        if (matrixBytes != sizeof(float) * 16 || parent < -1 ||
            parent >= static_cast<int>(boneIndex) ||
            !hasBytes(cursor, matrixBytes, limit)) {
            fail(
                FormatErrorCode::invalidValue,
                source,
                cursor - 12,
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
        (void)readCString(
            bytes, cursor, limit, source, "Puppet bone simulation JSON"
        );
        mesh.bones.push_back(std::move(bone));
    }
    // MDLS v2+ metadata after the bone records belongs to physics, IK, and
    // editor pivots. The absolute section boundary is authoritative, and the
    // render data consumed here is complete once every bone record is decoded.
    if (cursor > limit) {
        fail(
            FormatErrorCode::invalidOffset, source, cursor,
            "Puppet skeleton records cross the declared section boundary"
        );
    }
}

void parsePuppetMorphs(
    std::span<const std::uint8_t> bytes,
    std::size_t offset,
    const std::string& source,
    PuppetMesh& mesh
) {
    if (sectionVersion(bytes, offset, "MDMP") != 1) {
        fail(
            FormatErrorCode::unsupportedFormat, source, offset,
            "Unsupported puppet morph header; expected MDMP0001"
        );
    }
    std::size_t cursor = offset + magicSize;
    requireBytes(cursor, 4, bytes.size(), source, "Puppet morph header");
    const std::size_t limit = checkedSectionEnd(
        bytes, readUInt32(bytes, cursor), cursor + 4, source, cursor,
        "Puppet morph section end"
    );
    cursor += 4;
    const auto readTriples = [&] (
        std::size_t vertexCount,
        std::string_view description
    ) {
        requireBytes(cursor, 4, limit, source, description);
        const std::uint32_t byteCount = readUInt32(bytes, cursor);
        cursor += 4;
        if (vertexCount > std::numeric_limits<std::size_t>::max() / 6 ||
            byteCount != vertexCount * 6 ||
            !hasBytes(cursor, byteCount, limit)) {
            fail(
                FormatErrorCode::invalidValue, source, cursor - 4,
                std::string(description) + " length does not match its morph group"
            );
        }
        std::vector<std::array<std::uint16_t, 3>> result;
        result.reserve(vertexCount);
        for (std::size_t vertexIndex = 0;
             vertexIndex < vertexCount;
             ++vertexIndex) {
            result.push_back({
                readUInt16(bytes, cursor),
                readUInt16(bytes, cursor + 2),
                readUInt16(bytes, cursor + 4),
            });
            cursor += 6;
        }
        return result;
    };
    const auto readScalars = [&] (
        std::size_t vertexCount,
        std::string_view description
    ) {
        requireBytes(cursor, 4, limit, source, description);
        const std::uint32_t byteCount = readUInt32(bytes, cursor);
        cursor += 4;
        if (vertexCount > std::numeric_limits<std::size_t>::max() / 2 ||
            byteCount != vertexCount * 2 ||
            !hasBytes(cursor, byteCount, limit)) {
            fail(
                FormatErrorCode::invalidValue, source, cursor - 4,
                std::string(description) + " length does not match its morph group"
            );
        }
        std::vector<std::uint16_t> result;
        result.reserve(vertexCount);
        for (std::size_t vertexIndex = 0;
             vertexIndex < vertexCount;
             ++vertexIndex) {
            result.push_back(readUInt16(bytes, cursor));
            cursor += 2;
        }
        return result;
    };

    for (std::size_t meshIndex = 0;
         meshIndex < mesh.submeshes.size();
         ++meshIndex) {
        requireBytes(cursor, 2, limit, source, "Puppet morph group target count");
        const std::uint16_t targetCount = readUInt16(bytes, cursor);
        cursor += 2;
        if (targetCount == 0) continue;
        requireBytes(cursor, 8, limit, source, "Puppet morph group header");
        PuppetMorphData morph;
        morph.scale = readFloat32(bytes, cursor);
        cursor += 4;
        morph.vertexCount = readUInt32(bytes, cursor);
        cursor += 4;
        if (targetCount > maximumMorphTargets ||
            !std::isfinite(morph.scale) || morph.scale <= 0.0F ||
            morph.vertexCount == 0) {
            fail(
                FormatErrorCode::invalidValue, source, cursor - 10,
                "Puppet morph group scale, target count, or vertex count is invalid"
            );
        }
        morph.targets.reserve(targetCount);
        const std::uint32_t auxiliaryFlags =
            mesh.submeshes[meshIndex].auxiliaryFlags;
        for (std::uint16_t targetIndex = 0;
             targetIndex < targetCount;
             ++targetIndex) {
            requireBytes(cursor, 8, limit, source, "Puppet morph target header");
            PuppetMorphTarget target;
            target.shapeIdentity = readUInt64(bytes, cursor);
            cursor += 8;
            target.name = readCString(
                bytes, cursor, limit, source, "Puppet morph target name"
            );
            target.basePositions = readTriples(
                morph.vertexCount, "Puppet morph base-position payload"
            );
            if ((auxiliaryFlags & morphPayload400) != 0) {
                target.payload400 = readTriples(
                    morph.vertexCount, "Puppet morph 0x400 payload"
                );
            }
            if ((auxiliaryFlags & morphPayload800) != 0) {
                target.payload800 = readTriples(
                    morph.vertexCount, "Puppet morph 0x800 payload"
                );
            }
            if ((auxiliaryFlags & morphPayload1000) != 0) {
                target.payload1000 = readScalars(
                    morph.vertexCount, "Puppet morph 0x1000 payload"
                );
            }
            if ((auxiliaryFlags & morphModifier) != 0) {
                requireBytes(cursor, 16, limit, source, "Puppet morph modifier");
                PuppetMorphModifier modifierData{
                    .boneIndex = readUInt32(bytes, cursor),
                    .ruleMode = readUInt32(bytes, cursor + 4),
                    .ruleStart = readFloat32(bytes, cursor + 8),
                    .ruleEnd = readFloat32(bytes, cursor + 12),
                };
                cursor += 16;
                if (modifierData.boneIndex >= mesh.bones.size() ||
                    !std::isfinite(modifierData.ruleStart) ||
                    !std::isfinite(modifierData.ruleEnd)) {
                    fail(
                        FormatErrorCode::invalidValue, source, cursor - 16,
                        "Puppet morph modifier references an invalid bone or rule range"
                    );
                }
                target.modifier = modifierData;
            }
            morph.targets.push_back(std::move(target));
        }
        mesh.submeshes[meshIndex].morph = std::move(morph);
    }
    if (cursor != limit) {
        fail(
            FormatErrorCode::invalidValue, source, cursor,
            "Puppet morph section contains unconsumed data"
        );
    }
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

    const int modelVersion = sectionVersion(bytes, 0, "MDLV");
    PuppetModelVersion version;
    if (modelVersion == 13) {
        version = PuppetModelVersion::mdlv0013;
    } else if (modelVersion == 21) {
        version = PuppetModelVersion::mdlv0021;
    } else if (modelVersion == 23) {
        version = PuppetModelVersion::mdlv0023;
    } else {
        fail(
            FormatErrorCode::unsupportedFormat,
            source,
            0,
            "Unsupported puppet model header; expected MDLV0013, MDLV0021, or MDLV0023"
        );
    }

    std::size_t cursor = magicSize;
    requireBytes(cursor, 12, bytes.size(), source, "Puppet model header");
    const std::uint32_t headerFlags = readUInt32(bytes, cursor);
    cursor += 4;
    const std::uint32_t skinCount = readUInt32(bytes, cursor);
    cursor += 4;
    const std::uint32_t meshCount = readUInt32(bytes, cursor);
    cursor += 4;
    if (skinCount == 0 || skinCount > 256 || meshCount == 0 ||
        meshCount > maximumPuppetEntries) {
        fail(
            FormatErrorCode::invalidValue, source, magicSize,
            "Puppet model skin or mesh count is invalid"
        );
    }
    PuppetMesh result{
        .source = std::move(source),
        .version = version,
        .submeshes = {},
        .blendMapSubmeshIndex = std::nullopt,
        .bones = {},
        .animations = {},
    };
    struct SubmeshParseOffsets final {
        std::size_t vertexOffset = 0;
        std::optional<std::size_t> blendIndexOffset;
        std::size_t vertexStride = 0;
    };
    std::vector<SubmeshParseOffsets> submeshOffsets;
    result.submeshes.reserve(meshCount);
    submeshOffsets.reserve(meshCount);
    for (std::uint32_t meshIndex = 0; meshIndex < meshCount; ++meshIndex) {
        PuppetSubmesh submesh;
        submesh.materialPaths.reserve(skinCount);
        for (std::uint32_t skinIndex = 0; skinIndex < skinCount; ++skinIndex) {
            submesh.materialPaths.push_back(readCString(
                bytes, cursor, bytes.size(), result.source,
                "Puppet mesh material path"
            ));
        }
        requireBytes(cursor, 4, bytes.size(), result.source, "Puppet mesh flags");
        submesh.auxiliaryFlags = readUInt32(bytes, cursor);
        cursor += 4;
        if ((submesh.auxiliaryFlags & puppetSubmeshBlendMapFlag) != 0) {
            requireBytes(
                cursor, sizeof(std::uint32_t), bytes.size(), result.source,
                "Puppet mesh BlendMap row count"
            );
            const std::uint32_t rowCount = readUInt32(bytes, cursor);
            cursor += sizeof(std::uint32_t);
            if (rowCount == 0 || rowCount > 4) {
                fail(
                    FormatErrorCode::invalidValue, result.source,
                    cursor - sizeof(std::uint32_t),
                    "Puppet mesh BlendMap row count must be in 1...4"
                );
            }
            submesh.blendMapRowCount = rowCount;
            if (!result.blendMapSubmeshIndex) {
                result.blendMapSubmeshIndex = meshIndex;
            }
        }
        if (modelVersion >= 17) {
            requireBytes(cursor, sizeof(float) * 6, bytes.size(), result.source, "Puppet mesh bounds");
            for (std::size_t component = 0; component < 6; ++component) {
                const float value = readFloat32(bytes, cursor);
                cursor += 4;
                if (!std::isfinite(value)) {
                    fail(
                        FormatErrorCode::invalidValue, result.source,
                        cursor - 4,
                        "Puppet mesh bounds contain a non-finite component"
                    );
                }
            }
        }
        requireBytes(cursor, 8, bytes.size(), result.source, "Puppet mesh vertex header");
        const std::uint32_t meshFlags = modelVersion > 14
            ? readUInt32(bytes, cursor)
            : headerFlags;
        submesh.vertexFlags = meshFlags;
        if (modelVersion > 14) cursor += 4;
        const std::uint32_t unknownFlags = meshFlags & ~supportedModelFlags;
        if (unknownFlags != 0) {
            fail(
                FormatErrorCode::unsupportedFormat, result.source, cursor,
                "Puppet mesh uses unsupported vertex layout flags"
            );
        }
        const std::uint32_t positionFlags = meshFlags &
            (modelFlagPosition3 | modelFlagPosition4);
        if (positionFlags != modelFlagPosition3 &&
            positionFlags != modelFlagPosition4) {
            fail(
                FormatErrorCode::invalidValue, result.source, cursor,
                "Puppet mesh must declare exactly one position layout"
            );
        }
        std::size_t stride = sizeof(float) * 3;
        const std::optional<std::size_t> morphIndexOffset =
            (meshFlags & modelFlagPosition4) != 0
                ? std::optional<std::size_t>(stride)
                : std::nullopt;
        if (morphIndexOffset) stride += sizeof(float);
        const std::optional<std::size_t> normalOffset =
            (meshFlags & modelFlagNormal) != 0
                ? std::optional<std::size_t>(stride)
                : std::nullopt;
        if (normalOffset) stride += sizeof(float) * 3;
        const std::optional<std::size_t> tangentOffset =
            (meshFlags & modelFlagTangent) != 0
                ? std::optional<std::size_t>(stride)
                : std::nullopt;
        if (tangentOffset) stride += sizeof(float) * 4;
        const std::optional<std::size_t> blendIndexOffset =
            (meshFlags & modelFlagSkinBlend) != 0
                ? std::optional<std::size_t>(stride)
                : std::nullopt;
        if (blendIndexOffset) stride += sizeof(std::uint32_t) * 4;
        const std::optional<std::size_t> blendWeightOffset =
            (meshFlags & modelFlagSkinWeight) != 0
                ? std::optional<std::size_t>(stride)
                : std::nullopt;
        if (blendWeightOffset) stride += sizeof(float) * 4;
        const std::optional<std::size_t> uvOffset =
            (meshFlags & (modelFlagUv | modelFlagUv2)) != 0
                ? std::optional<std::size_t>(stride)
                : std::nullopt;
        if (uvOffset) stride += sizeof(float) * 2;
        const std::optional<std::size_t> uv2Offset =
            (meshFlags & modelFlagUv2) != 0
                ? std::optional<std::size_t>(stride)
                : std::nullopt;
        if (uv2Offset) stride += sizeof(float) * 2;
        if (submesh.blendMapRowCount &&
            (!blendIndexOffset || !uv2Offset)) {
            fail(
                FormatErrorCode::invalidValue, result.source, cursor,
                "Puppet BlendMap submesh requires blend indices and four-component texture coordinates"
            );
        }
        const std::uint32_t vertexBytes = readUInt32(bytes, cursor);
        cursor += 4;
        if (stride == 0 || vertexBytes == 0 || vertexBytes % stride != 0 ||
            !hasBytes(cursor, vertexBytes, bytes.size())) {
            fail(
                FormatErrorCode::invalidValue, result.source, cursor - 4,
                "Puppet mesh has an invalid vertex byte length"
            );
        }
        const std::size_t vertexCount = vertexBytes / stride;
        const std::size_t verticesOffset = cursor;
        submesh.vertices.reserve(vertexCount);
        for (std::size_t vertexIndex = 0;
             vertexIndex < vertexCount;
             ++vertexIndex) {
            const std::size_t vertexOffset = cursor + vertexIndex * stride;
            PuppetVertex vertex;
            for (std::size_t component = 0; component < 3; ++component) {
                vertex.position[component] = readFloat32(
                    bytes, vertexOffset + component * 4
                );
            }
            if (morphIndexOffset) {
                vertex.morphMapIndex = readFloat32(
                    bytes, vertexOffset + *morphIndexOffset
                );
            }
            if (normalOffset) {
                for (std::size_t component = 0; component < 3; ++component) {
                    vertex.normal[component] = readFloat32(
                        bytes, vertexOffset + *normalOffset + component * 4
                    );
                }
            }
            if (tangentOffset) {
                for (std::size_t component = 0; component < 4; ++component) {
                    vertex.tangent[component] = readFloat32(
                        bytes, vertexOffset + *tangentOffset + component * 4
                    );
                }
            }
            if (blendIndexOffset) {
                for (std::size_t component = 0; component < 4; ++component) {
                    vertex.boneIndices[component] = readUInt32(
                        bytes,
                        vertexOffset + *blendIndexOffset + component * 4
                    );
                }
            }
            if (blendWeightOffset) {
                for (std::size_t component = 0; component < 4; ++component) {
                    vertex.boneWeights[component] = readFloat32(
                        bytes,
                        vertexOffset + *blendWeightOffset + component * 4
                    );
                }
            } else if (blendIndexOffset) {
                vertex.boneWeights[0] = 1.0F;
            }
            if (uvOffset) {
                vertex.texCoord[0] = readFloat32(bytes, vertexOffset + *uvOffset);
                vertex.texCoord[1] = readFloat32(bytes, vertexOffset + *uvOffset + 4);
            }
            if (uv2Offset) {
                vertex.texCoord[2] = readFloat32(
                    bytes, vertexOffset + *uv2Offset
                );
                vertex.texCoord[3] = readFloat32(
                    bytes, vertexOffset + *uv2Offset + 4
                );
            }
            const auto finite = [](auto&& values) {
                return std::ranges::all_of(values, [](float value) {
                    return std::isfinite(value);
                });
            };
            if (!finite(vertex.position) ||
                !std::isfinite(vertex.morphMapIndex) ||
                vertex.morphMapIndex < 0.0F || !finite(vertex.normal) ||
                !finite(vertex.tangent) || !finite(vertex.boneWeights) ||
                !finite(vertex.texCoord) ||
                std::ranges::any_of(vertex.boneWeights, [](float value) {
                    return value < 0.0F;
                })) {
                fail(
                    FormatErrorCode::invalidValue, result.source,
                    vertexOffset,
                    "Puppet model vertex contains an invalid numeric component"
                );
            }
            submesh.vertices.push_back(vertex);
        }
        cursor += vertexBytes;
        requireBytes(cursor, 4, bytes.size(), result.source, "Puppet mesh index header");
        const std::uint32_t indexBytes = readUInt32(bytes, cursor);
        cursor += 4;
        const bool index32 = modelVersion >= 23 &&
            vertexCount > std::numeric_limits<std::uint16_t>::max();
        const std::size_t indexStride = index32
            ? sizeof(std::uint32_t)
            : sizeof(std::uint16_t);
        if (indexBytes == 0 || indexBytes % (indexStride * 3) != 0 ||
            !hasBytes(cursor, indexBytes, bytes.size())) {
            fail(
                FormatErrorCode::invalidValue, result.source, cursor - 4,
                "Puppet mesh has an invalid triangle index byte length"
            );
        }
        const std::size_t indexCount = indexBytes / indexStride;
        submesh.indices.reserve(indexCount);
        for (std::size_t index = 0; index < indexCount; ++index) {
            const std::uint32_t value = index32
                ? readUInt32(bytes, cursor + index * indexStride)
                : readUInt16(bytes, cursor + index * indexStride);
            if (value >= vertexCount) {
                fail(
                    FormatErrorCode::invalidValue, result.source,
                    cursor + index * indexStride,
                    "Puppet model index references a vertex outside the mesh"
                );
            }
            submesh.indices.push_back(value);
        }
        cursor += indexBytes;

        if (modelVersion >= 21) {
            requireBytes(cursor, 1, bytes.size(), result.source, "Puppet mesh part flag");
            const std::uint8_t hasVertexParts = bytes[cursor++];
            if (hasVertexParts == 1) {
                requireBytes(cursor, 1, bytes.size(), result.source, "Puppet mesh part vertex flag");
                const std::uint8_t hasPartVertices = bytes[cursor++];
                if (hasPartVertices != 0) {
                    requireBytes(cursor, 7, bytes.size(), result.source, "Puppet mesh part vertex header");
                    const std::uint16_t reserved = readUInt16(bytes, cursor);
                    cursor += 2;
                    cursor += 1;  // Serialized vertex section marker.
                    const std::uint32_t payloadBytes = readUInt32(bytes, cursor);
                    cursor += 4;
                    if (reserved != 0 || payloadBytes != vertexCount * 12 ||
                        !hasBytes(cursor, payloadBytes, bytes.size())) {
                        fail(
                            FormatErrorCode::invalidValue, result.source,
                            cursor - 7,
                            "Puppet mesh part vertex payload is invalid"
                        );
                    }
                    submesh.vertexPartRecords.reserve(vertexCount);
                    for (std::uint32_t vertexIndex = 0;
                         vertexIndex < vertexCount;
                         ++vertexIndex) {
                        submesh.vertexPartRecords.push_back({{
                            readUInt32(bytes, cursor),
                            readUInt32(bytes, cursor + 4),
                            readUInt32(bytes, cursor + 8),
                        }});
                        cursor += 12;
                    }
                }
            } else if (hasVertexParts != 0) {
                fail(
                    FormatErrorCode::invalidValue, result.source, cursor - 1,
                    "Puppet mesh part flag must be zero or one"
                );
            }
            requireBytes(cursor, 1, bytes.size(), result.source, "Puppet mesh draw-part flag");
            const std::uint8_t hasDrawParts = bytes[cursor++];
            if (hasDrawParts > 1) {
                fail(
                    FormatErrorCode::invalidValue, result.source, cursor - 1,
                    "Puppet mesh draw-part flag must be zero or one"
                );
            }
            if (hasDrawParts == 1) {
                requireBytes(cursor, 4, bytes.size(), result.source, "Puppet mesh draw-part length");
                const std::uint32_t partBytes = readUInt32(bytes, cursor);
                cursor += 4;
                if (partBytes % 16 != 0 ||
                    !hasBytes(cursor, partBytes, bytes.size())) {
                    fail(
                        FormatErrorCode::invalidValue, result.source,
                        cursor - 4,
                        "Puppet mesh draw-part byte length is invalid"
                    );
                }
                const std::size_t partCount = partBytes / 16;
                submesh.drawPartRecords.reserve(partCount);
                for (std::size_t partIndex = 0;
                     partIndex < partCount;
                     ++partIndex) {
                    submesh.drawPartRecords.push_back({{
                        readUInt32(bytes, cursor),
                        readUInt32(bytes, cursor + 4),
                        readUInt32(bytes, cursor + 8),
                        readUInt32(bytes, cursor + 12),
                    }});
                    cursor += 16;
                }
            }
            if (modelVersion > 21) {
                requireBytes(cursor, 4, bytes.size(), result.source, "Puppet mesh mask count");
                const std::uint32_t maskCount = readUInt32(bytes, cursor);
                cursor += 4;
                if (maskCount > maximumPuppetEntries) {
                    fail(
                        FormatErrorCode::invalidValue, result.source,
                        cursor - 4,
                        "Puppet mesh declares an impractical mask count"
                    );
                }
                for (std::uint32_t maskIndex = 0;
                     maskIndex < maskCount;
                    ++maskIndex) {
                    requireBytes(cursor, 8, bytes.size(), result.source, "Puppet mask header");
                    PuppetMaskRecord mask;
                    mask.id = readUInt32(bytes, cursor);
                    cursor += 4;
                    if (readUInt32(bytes, cursor) != 0) {
                        fail(
                            FormatErrorCode::invalidValue, result.source,
                            cursor,
                            "Puppet mask header padding is not zero"
                        );
                    }
                    cursor += 4;
                    mask.materialPath = readCString(
                        bytes, cursor, bytes.size(), result.source,
                        "Puppet mask material path"
                    );
                    requireBytes(cursor, 8, bytes.size(), result.source, "Puppet mask part list");
                    if (readUInt32(bytes, cursor) != 0) {
                        fail(
                            FormatErrorCode::invalidValue, result.source,
                            cursor,
                            "Puppet mask part-list padding is not zero"
                        );
                    }
                    cursor += 4;
                    const std::uint32_t firstCount = readUInt32(bytes, cursor);
                    cursor += 4;
                    requireBytes(
                        cursor, static_cast<std::size_t>(firstCount) * 4,
                        bytes.size(), result.source, "Puppet mask first part list"
                    );
                    mask.firstPartIndices.reserve(firstCount);
                    for (std::uint32_t index = 0; index < firstCount; ++index) {
                        mask.firstPartIndices.push_back(readUInt32(bytes, cursor));
                        cursor += 4;
                    }
                    requireBytes(cursor, 4, bytes.size(), result.source, "Puppet mask second part count");
                    const std::uint32_t secondCount = readUInt32(bytes, cursor);
                    cursor += 4;
                    requireBytes(
                        cursor, static_cast<std::size_t>(secondCount) * 4,
                        bytes.size(), result.source, "Puppet mask second part list"
                    );
                    mask.secondPartIndices.reserve(secondCount);
                    for (std::uint32_t index = 0; index < secondCount; ++index) {
                        mask.secondPartIndices.push_back(readUInt32(bytes, cursor));
                        cursor += 4;
                    }
                    submesh.masks.push_back(std::move(mask));
                }
            }
        }
        submeshOffsets.push_back({
            .vertexOffset = verticesOffset,
            .blendIndexOffset = blendIndexOffset,
            .vertexStride = stride,
        });
        result.submeshes.push_back(std::move(submesh));
    }

    if (sectionVersion(bytes, cursor, "MDLS") >= 0) {
        const std::size_t skeletonOffset = cursor;
        parsePuppetSkeleton(bytes, cursor, result.source, result);
        cursor = readUInt32(bytes, skeletonOffset + magicSize);
    }
    if (sectionVersion(bytes, cursor, "MDAT") >= 0) {
        const std::size_t attachmentOffset = cursor;
        requireBytes(cursor + magicSize, 4, bytes.size(), result.source, "Puppet attachment header");
        cursor = checkedSectionEnd(
            bytes, readUInt32(bytes, attachmentOffset + magicSize),
            attachmentOffset + magicSize + 4, result.source,
            attachmentOffset + magicSize, "Puppet attachment section end"
        );
    }
    if (sectionVersion(bytes, cursor, "MDLA") >= 0) {
        const std::size_t animationOffset = cursor;
        parsePuppetAnimations(bytes, cursor, result.source, result);
        cursor = readUInt32(bytes, animationOffset + magicSize);
    }
    if (sectionVersion(bytes, cursor, "MDMP") >= 0) {
        const std::size_t morphOffset = cursor;
        parsePuppetMorphs(bytes, cursor, result.source, result);
        cursor = readUInt32(bytes, morphOffset + magicSize);
    }
    if (sectionVersion(bytes, cursor, "MDLE") >= 0) {
        requireBytes(cursor + magicSize, 4, bytes.size(), result.source, "Puppet extension header");
        cursor = checkedSectionEnd(
            bytes, readUInt32(bytes, cursor + magicSize), cursor + magicSize + 4,
            result.source, cursor + magicSize, "Puppet extension section end"
        );
    }
    while (cursor < bytes.size() && bytes[cursor] == 0) ++cursor;
    if (cursor != bytes.size()) {
        fail(
            FormatErrorCode::unsupportedFormat, result.source, cursor,
            "Puppet model contains an unsupported trailing section"
        );
    }
    if (result.submeshes.empty() ||
        std::ranges::any_of(result.submeshes, [](const PuppetSubmesh& submesh) {
            return submesh.vertices.empty() || submesh.indices.empty();
        })) {
        fail(
            FormatErrorCode::invalidValue, result.source, magicSize,
            "Puppet model contains a submesh without drawable geometry"
        );
    }
    for (std::size_t meshIndex = 0;
         meshIndex < result.submeshes.size();
         ++meshIndex) {
        const PuppetSubmesh& submesh = result.submeshes[meshIndex];
        if (!submesh.blendMapRowCount) continue;
        const SubmeshParseOffsets& offsets = submeshOffsets[meshIndex];
        const std::uint32_t scalarCount = *submesh.blendMapRowCount * 4U;
        for (std::size_t vertexIndex = 0;
             vertexIndex < submesh.vertices.size();
             ++vertexIndex) {
            if (submesh.vertices[vertexIndex].boneIndices[0] >= scalarCount) {
                fail(
                    FormatErrorCode::invalidValue, result.source,
                    offsets.vertexOffset + vertexIndex * offsets.vertexStride +
                        *offsets.blendIndexOffset,
                    "Puppet BlendMap vertex references a scalar outside its declared rows"
                );
            }
        }
    }
    if (!result.bones.empty()) {
        for (std::size_t meshIndex = 0;
             meshIndex < result.submeshes.size();
             ++meshIndex) {
            const PuppetSubmesh& submesh = result.submeshes[meshIndex];
            const SubmeshParseOffsets& offsets = submeshOffsets[meshIndex];
            if (!offsets.blendIndexOffset) {
                fail(
                    FormatErrorCode::unsupportedFormat, result.source,
                    offsets.vertexOffset,
                    "Puppet skeleton requires blend indices in every submesh"
                );
            }
            if (submesh.blendMapRowCount) {
                continue;
            }
            for (std::size_t vertexIndex = 0;
                 vertexIndex < submesh.vertices.size();
                 ++vertexIndex) {
                const PuppetVertex& vertex = submesh.vertices[vertexIndex];
                for (std::size_t influence = 0; influence < 4; ++influence) {
                    if (vertex.boneWeights[influence] > 0.0F &&
                        vertex.boneIndices[influence] >= result.bones.size()) {
                        fail(
                            FormatErrorCode::invalidValue, result.source,
                            offsets.vertexOffset +
                                vertexIndex * offsets.vertexStride +
                                *offsets.blendIndexOffset + influence * 4,
                            "Puppet vertex references a bone outside the skeleton"
                        );
                    }
                }
            }
        }
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
            throw std::invalid_argument("Puppet transform matrix is singular");
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

[[nodiscard]] float puppetSampleScalar(
    const PuppetAnimation& animation,
    std::span<const float> track,
    double timeSeconds
) {
    if (!std::isfinite(timeSeconds)) {
        throw std::invalid_argument("Puppet animation time must be finite");
    }
    const std::size_t expectedSampleCount =
        static_cast<std::size_t>(animation.frameCount) + 1;
    if (track.size() != expectedSampleCount || track.empty() ||
        !std::isfinite(animation.framesPerSecond) ||
        animation.framesPerSecond <= 0.0F) {
        throw std::invalid_argument(
            "Puppet scalar animation track does not match its timeline"
        );
    }
    if (animation.frameCount == 0) return track.front();

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
        lower + 1, static_cast<std::size_t>(animation.frameCount)
    );
    const float fraction = static_cast<float>(
        framePosition - std::floor(framePosition)
    );
    return puppetMix(track[lower], track[upper], fraction);
}

}  // namespace

PuppetPoseState evaluatePuppetPose(
    const PuppetMesh& mesh,
    std::span<const PuppetAnimationLayerInput> layers
) {
    if (mesh.bones.empty()) return {};

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

    const auto shaderMatrix = [](const PuppetMatrix& matrix) {
        return PuppetSkinMatrix{
            static_cast<float>(matrix[0]),
            static_cast<float>(matrix[1]),
            static_cast<float>(matrix[2]),
            static_cast<float>(matrix[4]),
            static_cast<float>(matrix[5]),
            static_cast<float>(matrix[6]),
            static_cast<float>(matrix[8]),
            static_cast<float>(matrix[9]),
            static_cast<float>(matrix[10]),
            static_cast<float>(matrix[12]),
            static_cast<float>(matrix[13]),
            static_cast<float>(matrix[14]),
        };
    };
    PuppetPoseState result;
    result.skinMatrices.reserve(skinMatrices.size());
    result.inverseCurrentGlobalMatrices.reserve(currentGlobal.size());
    for (std::size_t boneIndex = 0;
         boneIndex < skinMatrices.size();
         ++boneIndex) {
        result.skinMatrices.push_back(shaderMatrix(skinMatrices[boneIndex]));
        result.inverseCurrentGlobalMatrices.push_back(
            shaderMatrix(puppetInverse(currentGlobal[boneIndex]))
        );
    }
    return result;
}

std::vector<PuppetSkinMatrix> evaluatePuppetSkinMatrices(
    const PuppetMesh& mesh,
    std::span<const PuppetAnimationLayerInput> layers
) {
    return evaluatePuppetPose(mesh, layers).skinMatrices;
}

PuppetBlendMapState evaluatePuppetBlendMap(
    const PuppetMesh& mesh,
    std::span<const PuppetAnimationLayerInput> layers
) {
    PuppetBlendMapState result{};
    for (const PuppetAnimationLayerInput& layer : layers) {
        if (!std::isfinite(layer.timeSeconds) || !std::isfinite(layer.blend) ||
            layer.blend < 0.0F || layer.blend > 1.0F) {
            throw std::invalid_argument(
                "Puppet BlendMap layer time and blend must be finite, with blend in 0...1"
            );
        }
        if (layer.blend == 0.0F) continue;
        const PuppetAnimation* animation = mesh.animation(layer.animationId);
        if (animation == nullptr) {
            throw std::invalid_argument(
                "Puppet BlendMap layer references unknown animation id " +
                std::to_string(layer.animationId)
            );
        }
        if (animation->blendMapTracks.size() > result.size()) {
            throw std::invalid_argument(
                "Puppet animation exceeds the 16-scalar BlendMap ABI"
            );
        }
        for (std::size_t scalarIndex = 0;
             scalarIndex < animation->blendMapTracks.size();
             ++scalarIndex) {
            const float sampled = puppetSampleScalar(
                *animation,
                animation->blendMapTracks[scalarIndex],
                layer.timeSeconds
            );
            if (!std::isfinite(sampled)) {
                throw std::invalid_argument(
                    "Puppet BlendMap track contains a non-finite sample"
                );
            }
            if (layer.additive) {
                result[scalarIndex] += sampled * layer.blend;
            } else {
                result[scalarIndex] = puppetMix(
                    result[scalarIndex], sampled, layer.blend
                );
            }
        }
    }
    return result;
}

PuppetBonesAlphaState evaluatePuppetBonesAlpha(
    const PuppetMesh& mesh,
    std::span<const PuppetAnimationLayerInput> layers
) {
    PuppetBonesAlphaState result(mesh.bones.size(), 1.0F);
    for (const PuppetAnimationLayerInput& layer : layers) {
        if (!std::isfinite(layer.timeSeconds) || !std::isfinite(layer.blend) ||
            layer.blend < 0.0F || layer.blend > 1.0F) {
            throw std::invalid_argument(
                "Puppet BonesAlpha layer time and blend must be finite, with blend in 0...1"
            );
        }
        if (layer.blend == 0.0F) continue;
        const PuppetAnimation* animation = mesh.animation(layer.animationId);
        if (animation == nullptr) {
            throw std::invalid_argument(
                "Puppet BonesAlpha layer references unknown animation id " +
                std::to_string(layer.animationId)
            );
        }
        if (animation->bonesAlphaTracks.empty()) continue;
        if (animation->bonesAlphaTracks.size() != result.size()) {
            throw std::invalid_argument(
                "Puppet BonesAlpha track count does not match the skeleton"
            );
        }
        for (std::size_t boneIndex = 0; boneIndex < result.size(); ++boneIndex) {
            const float sampled = puppetSampleScalar(
                *animation,
                animation->bonesAlphaTracks[boneIndex],
                layer.timeSeconds
            );
            if (!std::isfinite(sampled)) {
                throw std::invalid_argument(
                    "Puppet BonesAlpha track contains a non-finite sample"
                );
            }
            if (layer.additive) {
                const float current = result[boneIndex];
                result[boneIndex] = std::clamp(
                    current + (sampled - 1.0F) * layer.blend,
                    std::min(current, sampled),
                    std::max(current, sampled)
                );
            } else {
                result[boneIndex] = puppetMix(
                    result[boneIndex], sampled, layer.blend
                );
            }
        }
    }
    return result;
}

std::vector<std::array<float, 3>> applyPuppetSkinMatrices(
    const PuppetMesh& mesh,
    std::size_t submeshIndex,
    std::span<const PuppetSkinMatrix> skinMatrices
) {
    const PuppetSubmesh& submesh = mesh.submeshes.at(submeshIndex);
    std::vector<std::array<float, 3>> result;
    result.reserve(submesh.vertices.size());
    if (mesh.bones.empty() || submesh.blendMapRowCount) {
        for (const PuppetVertex& vertex : submesh.vertices) {
            result.push_back({
                vertex.position[0], vertex.position[1], vertex.position[2],
            });
        }
        return result;
    }
    if (skinMatrices.size() != mesh.bones.size()) {
        throw std::invalid_argument(
            "Puppet skin matrix count does not match the skeleton"
        );
    }

    for (const PuppetVertex& vertex : submesh.vertices) {
        std::array<double, 3> deformed{};
        double weightSum = 0.0;
        for (std::size_t influence = 0; influence < 4; ++influence) {
            const double weight = vertex.boneWeights[influence];
            if (weight <= 0.0) continue;
            const PuppetSkinMatrix& matrix =
                skinMatrices[vertex.boneIndices[influence]];
            const double x = vertex.position[0];
            const double y = vertex.position[1];
            const double z = vertex.position[2];
            deformed[0] += weight * (
                x * matrix[0] + y * matrix[3] + z * matrix[6] + matrix[9]
            );
            deformed[1] += weight * (
                x * matrix[1] + y * matrix[4] + z * matrix[7] + matrix[10]
            );
            deformed[2] += weight * (
                x * matrix[2] + y * matrix[5] + z * matrix[8] + matrix[11]
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

std::vector<std::array<float, 3>> evaluatePuppetPositions(
    const PuppetMesh& mesh,
    std::size_t submeshIndex,
    std::span<const PuppetAnimationLayerInput> layers
) {
    const std::vector<PuppetSkinMatrix> skinMatrices =
        evaluatePuppetSkinMatrices(mesh, layers);
    return applyPuppetSkinMatrices(mesh, submeshIndex, skinMatrices);
}

std::optional<PuppetMorphState> evaluatePuppetMorphState(
    const PuppetMesh& mesh,
    std::size_t submeshIndex,
    std::span<const PuppetAnimationLayerInput> layers
) {
    const PuppetSubmesh& submesh = mesh.submeshes.at(submeshIndex);
    if (!submesh.morph) return std::nullopt;
    const PuppetMorphData& morph = *submesh.morph;
    std::array<float, 11> targetWeights{};
    const auto sampleCurve = [](
        const PuppetAnimation& animation,
        const PuppetAnimation::MorphCurve& curve,
        double timeSeconds
    ) {
        if (!std::isfinite(timeSeconds)) {
            throw std::invalid_argument("Puppet morph animation time must be finite");
        }
        const double duration = static_cast<double>(animation.frameCount) /
            animation.framesPerSecond;
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
            lower + 1, static_cast<std::size_t>(animation.frameCount)
        );
        const float fraction = static_cast<float>(
            framePosition - std::floor(framePosition)
        );
        return puppetMix(curve.values.at(lower), curve.values.at(upper), fraction);
    };

    for (const PuppetAnimationLayerInput& layer : layers) {
        if (!std::isfinite(layer.timeSeconds) || !std::isfinite(layer.blend) ||
            layer.blend < 0.0F || layer.blend > 1.0F) {
            throw std::invalid_argument(
                "Puppet morph layer time and blend must be finite, with blend in 0...1"
            );
        }
        if (layer.blend == 0.0F) continue;
        const PuppetAnimation* animation = mesh.animation(layer.animationId);
        if (animation == nullptr) {
            throw std::invalid_argument(
                "Puppet morph layer references unknown animation id " +
                std::to_string(layer.animationId)
            );
        }
        if (animation->morphTracks.empty()) continue;
        if (animation->morphTracks.size() != mesh.submeshes.size()) {
            throw std::invalid_argument(
                "Puppet animation morph-track count does not match the model"
            );
        }
        const PuppetAnimation::MorphTrack& track =
            animation->morphTracks[submeshIndex];
        if ((track.flags & 1U) == 0) continue;
        if (track.scale != morph.scale) {
            throw std::invalid_argument(
                "Puppet animation morph track has no matching MDMP scale"
            );
        }
        for (const PuppetAnimation::MorphCurve& curve : track.curves) {
            if (curve.id >= morph.targets.size()) {
                throw std::invalid_argument(
                    "Puppet morph curve id exceeds its MDMP target count"
                );
            }
            const float sampled = sampleCurve(
                *animation, curve, layer.timeSeconds
            );
            float& weight = targetWeights[curve.id];
            if (layer.additive) {
                weight += (sampled - curve.values.front()) * layer.blend;
            } else {
                weight = puppetMix(weight, sampled, layer.blend);
            }
        }
    }

    PuppetMorphState result{.submeshIndex = submeshIndex};
    result.offsets[0] = static_cast<std::uint32_t>(morph.targets.size());
    result.weights[0] = morph.scale;
    for (std::size_t targetIndex = 0;
         targetIndex < morph.targets.size();
         ++targetIndex) {
        result.offsets[targetIndex + 1] = static_cast<std::uint32_t>(
            targetIndex * morph.vertexCount
        );
        result.weights[targetIndex + 1] = targetWeights[targetIndex];
    }
    return result;
}

std::optional<PuppetMorphModifierState> evaluatePuppetMorphModifierState(
    const PuppetMesh& mesh,
    std::size_t submeshIndex,
    const PuppetPoseState& pose
) {
    const PuppetSubmesh& submesh = mesh.submeshes.at(submeshIndex);
    if (!submesh.morph) return std::nullopt;
    const PuppetMorphData& morph = *submesh.morph;
    const bool hasModifiers = std::ranges::any_of(
        morph.targets,
        [](const PuppetMorphTarget& target) {
            return target.modifier.has_value();
        }
    );
    if (!hasModifiers) return std::nullopt;
    if (morph.targets.size() > puppetMaximumMorphTargets) {
        throw std::invalid_argument(
            "Puppet morph modifier target count exceeds the shader contract"
        );
    }
    if (pose.inverseCurrentGlobalMatrices.size() != mesh.bones.size()) {
        throw std::invalid_argument(
            "Puppet modifier pose does not match the model skeleton"
        );
    }

    constexpr PuppetSkinMatrix identity{
        1.0F, 0.0F, 0.0F,
        0.0F, 1.0F, 0.0F,
        0.0F, 0.0F, 1.0F,
        0.0F, 0.0F, 0.0F,
    };
    PuppetMorphModifierState result{.submeshIndex = submeshIndex};
    result.transforms.fill(identity);
    result.rules.fill({0.0F, -1.0F, 0.0F});
    for (std::size_t targetIndex = 0;
         targetIndex < morph.targets.size();
         ++targetIndex) {
        const std::optional<PuppetMorphModifier>& modifier =
            morph.targets[targetIndex].modifier;
        if (!modifier) {
            throw std::invalid_argument(
                "Puppet morph modifier group contains a target without modifier data"
            );
        }
        if (modifier->boneIndex >= pose.inverseCurrentGlobalMatrices.size()) {
            throw std::invalid_argument(
                "Puppet morph modifier references a bone outside the evaluated pose"
            );
        }
        if (!std::isfinite(modifier->ruleStart) ||
            !std::isfinite(modifier->ruleEnd)) {
            throw std::invalid_argument(
                "Puppet morph modifier contains an invalid rule"
            );
        }
        result.transforms[targetIndex] =
            pose.inverseCurrentGlobalMatrices[modifier->boneIndex];
        result.rules[targetIndex] = {
            modifier->ruleMode == 2 || modifier->ruleMode == 3 ? 1.0F : 0.0F,
            modifier->ruleStart,
            modifier->ruleEnd,
        };
    }
    return result;
}

}  // namespace we::scene
