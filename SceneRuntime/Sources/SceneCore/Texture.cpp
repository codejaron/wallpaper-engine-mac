#include <SceneCore/Texture.hpp>

#include <SceneCore/BinaryReader.hpp>
#include <SceneCore/FormatError.hpp>

#include <lz4.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>
#include <string_view>
#include <utility>

namespace we::scene {
namespace {

constexpr std::uint32_t knownTextureFlags =
    textureFlagNoInterpolation |
    textureFlagClampUVs |
    textureFlagIsGif |
    textureFlagClampUVsBorder |
    textureFlagVideo |
    textureFlagLUT |
    textureFlagAlphaChannelPriority;
constexpr std::size_t maximumImages = 4096;
constexpr std::size_t maximumMipmaps = 64;
constexpr std::size_t maximumFrames = 1'000'000;
constexpr std::size_t maximumMipmapBytes = 256ULL * 1024ULL * 1024ULL;
constexpr std::size_t maximumTextureBytes = 512ULL * 1024ULL * 1024ULL;

std::string readMagic(BinaryReader& reader) {
    const auto bytes = reader.readExact(9);
    return {
        reinterpret_cast<const char*>(bytes.data()),
        bytes.size(),
    };
}

bool magicEquals(const std::string& magic, std::string_view expected) {
    return magic.size() == 9 && expected.size() == 8 && magic.back() == '\0' &&
        std::string_view(magic.data(), 8) == expected;
}

[[noreturn]] void invalid(
    const BinaryReader& reader,
    FormatErrorCode code,
    std::string message
) {
    throw FormatError(code, reader.source(), reader.position(), std::move(message));
}

TextureFormat parseTextureFormat(
    std::uint32_t value,
    const BinaryReader& reader
) {
    switch (static_cast<TextureFormat>(value)) {
        case TextureFormat::argb8888:
        case TextureFormat::rgb888:
        case TextureFormat::rgb565:
        case TextureFormat::dxt5:
        case TextureFormat::dxt3:
        case TextureFormat::dxt1:
        case TextureFormat::rg88:
        case TextureFormat::r8:
        case TextureFormat::rg1616f:
        case TextureFormat::r16f:
        case TextureFormat::bc7:
        case TextureFormat::rgba1010102:
        case TextureFormat::rgba16161616f:
        case TextureFormat::rgb161616f:
        case TextureFormat::unknown:
            return static_cast<TextureFormat>(value);
    }
    invalid(reader, FormatErrorCode::unsupportedFormat,
            "Unknown Wallpaper Engine texture format: " +
                std::to_string(value));
}

TextureFileFormat parseFileFormat(
    std::uint32_t value,
    const BinaryReader& reader
) {
    if (value == static_cast<std::uint32_t>(TextureFileFormat::unknown) ||
        value <= 36) {
        return static_cast<TextureFileFormat>(value);
    }
    invalid(reader, FormatErrorCode::unsupportedFormat,
            "Unknown embedded image format: " + std::to_string(value));
}

void validateDimensions(
    const BinaryReader& reader,
    std::uint32_t width,
    std::uint32_t height,
    std::string_view label
) {
    if (width == 0 || height == 0 || width > 32768 || height > 32768) {
        invalid(reader, FormatErrorCode::invalidValue,
                std::string(label) + " has invalid dimensions " +
                    std::to_string(width) + "x" + std::to_string(height));
    }
}

void parseMetadata(
    Texture& texture,
    std::string_view metadata,
    const std::string& source
) {
    try {
        const nlohmann::json json = nlohmann::json::parse(metadata);
        if (!json.contains("spritesheetsequences") ||
            !json["spritesheetsequences"].is_array() ||
            json["spritesheetsequences"].empty()) {
            return;
        }

        const auto& sequence = json["spritesheetsequences"].front();
        if (!sequence.is_object()) {
            throw FormatError(
                FormatErrorCode::malformedMetadata,
                source,
                FormatError::noOffset,
                "The first spritesheet sequence is not an object"
            );
        }

        const int frames = sequence.value("frames", 0);
        const float frameWidth = sequence.value("width", 0.0F);
        const float frameHeight = sequence.value("height", 0.0F);
        const float duration = sequence.value("duration", 0.0F);
        if (frames <= 0 || !std::isfinite(frameWidth) ||
            !std::isfinite(frameHeight) || !std::isfinite(duration) ||
            frameWidth <= 0.0F || frameHeight <= 0.0F || duration < 0.0F ||
            texture.width == 0 || texture.height == 0) {
            throw FormatError(
                FormatErrorCode::malformedMetadata,
                source,
                FormatError::noOffset,
                "Spritesheet sequence contains invalid dimensions, frame count, or duration"
            );
        }

        const double columns = std::round(
            static_cast<double>(texture.width) / frameWidth
        );
        const double rows = std::round(
            static_cast<double>(texture.height) / frameHeight
        );
        if (!std::isfinite(columns) || !std::isfinite(rows) || columns < 1.0 ||
            rows < 1.0 ||
            columns > std::numeric_limits<std::uint32_t>::max() ||
            rows > std::numeric_limits<std::uint32_t>::max()) {
            throw FormatError(
                FormatErrorCode::malformedMetadata,
                source,
                FormatError::noOffset,
                "Spritesheet frame dimensions produce an invalid texture grid"
            );
        }
        texture.spritesheetColumns = static_cast<std::uint32_t>(columns);
        texture.spritesheetRows = static_cast<std::uint32_t>(rows);
        texture.spritesheetFrameCount = static_cast<std::uint32_t>(frames);
        texture.spritesheetDuration = duration;
        if (texture.spritesheetColumns == 0 || texture.spritesheetRows == 0 ||
            static_cast<std::uint64_t>(texture.spritesheetColumns) *
                texture.spritesheetRows < texture.spritesheetFrameCount) {
            throw FormatError(
                FormatErrorCode::malformedMetadata,
                source,
                FormatError::noOffset,
                "Spritesheet sequence does not fit within the texture grid"
            );
        }
    } catch (const FormatError&) {
        throw;
    } catch (const nlohmann::json::exception& error) {
        throw FormatError(
            FormatErrorCode::malformedMetadata,
            source,
            FormatError::noOffset,
            std::string("Invalid .tex-json metadata: ") + error.what()
        );
    }
}

std::size_t rawBytesPerPixel(TextureFormat format) noexcept {
    switch (format) {
        case TextureFormat::r8:
            return 1;
        case TextureFormat::rg88:
        case TextureFormat::rgb565:
        case TextureFormat::r16f:
            return 2;
        case TextureFormat::argb8888:
        case TextureFormat::rg1616f:
        case TextureFormat::rgba1010102:
            return 4;
        case TextureFormat::rgb888:
            return 3;
        case TextureFormat::rgba16161616f:
            return 8;
        case TextureFormat::rgb161616f:
            return 6;
        default:
            return 0;
    }
}

void validateDeclaredMipmapSize(
    const Texture& texture,
    const TextureMipmap& mipmap,
    const BinaryReader& reader
) {
    std::size_t expected = 0;
    if (TextureParser::isBlockCompressed(texture.format)) {
        expected = TextureParser::expectedBlockCompressedSize(
            texture.format,
            mipmap.width,
            mipmap.height
        );
    } else if (texture.fileFormat == TextureFileFormat::unknown) {
        const std::size_t bytesPerPixel = rawBytesPerPixel(texture.format);
        if (bytesPerPixel == 0) {
            return;
        }
        expected = static_cast<std::size_t>(mipmap.width) * mipmap.height *
            bytesPerPixel;
    } else {
        return;
    }

    if (static_cast<std::size_t>(mipmap.uncompressedSize) != expected) {
        invalid(reader, FormatErrorCode::invalidValue,
                "Texture mipmap declares " +
                    std::to_string(mipmap.uncompressedSize) +
                    " bytes; expected " + std::to_string(expected));
    }
}

std::pair<std::uint32_t, std::uint32_t> checkedGridDimensions(
    const Texture& texture,
    float frameWidth,
    float frameHeight,
    const BinaryReader& reader
) {
    if (!std::isfinite(frameWidth) || !std::isfinite(frameHeight) ||
        frameWidth < 1.0F || frameHeight < 1.0F) {
        invalid(reader, FormatErrorCode::invalidValue,
                "Texture animation frame dimensions must be finite and at least one pixel");
    }
    const double columns = std::round(
        static_cast<double>(texture.width) / frameWidth
    );
    const double rows = std::round(
        static_cast<double>(texture.height) / frameHeight
    );
    if (!std::isfinite(columns) || !std::isfinite(rows) || columns < 1.0 ||
        rows < 1.0 ||
        columns > std::numeric_limits<std::uint32_t>::max() ||
        rows > std::numeric_limits<std::uint32_t>::max()) {
        invalid(reader, FormatErrorCode::invalidValue,
                "Texture animation frame dimensions produce an invalid texture grid");
    }
    return {
        static_cast<std::uint32_t>(columns),
        static_cast<std::uint32_t>(rows),
    };
}

}  // namespace

Texture TextureParser::parse(
    std::span<const std::uint8_t> bytes,
    std::string source,
    std::optional<std::string_view> metadata
) {
    BinaryReader reader(bytes, source);
    Texture texture;

    if (!magicEquals(readMagic(reader), "TEXV0005")) {
        invalid(reader, FormatErrorCode::invalidMagic,
                "Expected TEXV0005 texture header");
    }
    if (!magicEquals(readMagic(reader), "TEXI0001")) {
        invalid(reader, FormatErrorCode::invalidMagic,
                "Expected TEXI0001 texture information header");
    }

    texture.format = parseTextureFormat(reader.readUInt32(), reader);
    texture.flags = reader.readUInt32();
    if ((texture.flags & ~knownTextureFlags) != 0) {
        invalid(reader, FormatErrorCode::invalidValue,
                "Texture contains unknown flags: " +
                    std::to_string(texture.flags));
    }
    texture.textureWidth = reader.readUInt32();
    texture.textureHeight = reader.readUInt32();
    texture.width = reader.readUInt32();
    texture.height = reader.readUInt32();
    validateDimensions(reader, texture.width, texture.height, "Texture");
    static_cast<void>(reader.readUInt32());

    const std::size_t containerStart = reader.position();
    std::string containerMagic = readMagic(reader);
    if (!containerMagic.starts_with("TEXB")) {
        // A set of shipped LUT textures has one additional TEXI field.
        reader.seek(containerStart + sizeof(std::uint32_t));
        texture.hasExtraTEXIField = true;
        containerMagic = readMagic(reader);
    }

    if (magicEquals(containerMagic, "TEXB0001")) {
        texture.containerVersion = TextureContainerVersion::texb0001;
    } else if (magicEquals(containerMagic, "TEXB0002")) {
        texture.containerVersion = TextureContainerVersion::texb0002;
    } else if (magicEquals(containerMagic, "TEXB0003")) {
        texture.containerVersion = TextureContainerVersion::texb0003;
    } else if (magicEquals(containerMagic, "TEXB0004")) {
        texture.containerVersion = TextureContainerVersion::texb0004;
    } else {
        invalid(reader, FormatErrorCode::invalidMagic,
                "Unknown TEXB container header: '" + containerMagic + "'");
    }

    texture.imageCount = reader.readUInt32();
    if (texture.imageCount == 0 || texture.imageCount > maximumImages) {
        invalid(reader, FormatErrorCode::invalidValue,
                "Texture image count is outside the supported range");
    }
    texture.images.resize(texture.imageCount);
    std::size_t decodedTextureBytes = 0;

    if (texture.containerVersion == TextureContainerVersion::texb0004) {
        texture.fileFormat = parseFileFormat(reader.readUInt32(), reader);
        texture.isVideoMp4 = reader.readUInt32() == 1;
        if (texture.fileFormat == TextureFileFormat::unknown && texture.isVideoMp4) {
            texture.fileFormat = TextureFileFormat::mp4;
        }
    } else if (texture.containerVersion == TextureContainerVersion::texb0003) {
        texture.fileFormat = parseFileFormat(reader.readUInt32(), reader);
    }
    if (texture.format == TextureFormat::unknown &&
        texture.fileFormat == TextureFileFormat::unknown &&
        !texture.isVideoMp4) {
        invalid(reader, FormatErrorCode::unsupportedFormat,
                "Texture format is UNKNOWN without an embedded image or video format");
    }

    for (auto& image : texture.images) {
        const std::uint32_t mipmapCount = reader.readUInt32();
        if (mipmapCount == 0 || mipmapCount > maximumMipmaps) {
            invalid(reader, FormatErrorCode::invalidValue,
                    "Texture mipmap count is outside the supported range");
        }
        image.mipmaps.reserve(mipmapCount);
        for (std::uint32_t index = 0; index < mipmapCount; ++index) {
            TextureMipmap mipmap;
            const bool hasV4EditorHeader =
                texture.containerVersion == TextureContainerVersion::texb0004 &&
                texture.isVideoMp4;
            if (hasV4EditorHeader) {
                static_cast<void>(reader.readUInt32());
                static_cast<void>(reader.readUInt32());
                mipmap.metadata = reader.readNullTerminatedString(1 << 20);
                static_cast<void>(reader.readUInt32());
            }

            mipmap.width = reader.readUInt32();
            mipmap.height = reader.readUInt32();
            validateDimensions(reader, mipmap.width, mipmap.height, "Mipmap");
            if (texture.hasExtraTEXIField) {
                static_cast<void>(reader.readUInt32());
            }
            if (texture.containerVersion == TextureContainerVersion::texb0002 ||
                texture.containerVersion == TextureContainerVersion::texb0003 ||
                texture.containerVersion == TextureContainerVersion::texb0004) {
                mipmap.compression = reader.readUInt32();
                mipmap.uncompressedSize = reader.readInt32();
            }
            mipmap.compressedSize = reader.readInt32();
            if (mipmap.compression == 0) {
                mipmap.uncompressedSize = mipmap.compressedSize;
            } else if (mipmap.compression != 1) {
                invalid(reader, FormatErrorCode::unsupportedFormat,
                        "Unsupported texture compression: " +
                            std::to_string(mipmap.compression));
            }

            if (mipmap.uncompressedSize <= 0 || mipmap.compressedSize <= 0 ||
                static_cast<std::uint64_t>(mipmap.uncompressedSize) > maximumMipmapBytes ||
                static_cast<std::uint64_t>(mipmap.compressedSize) > maximumMipmapBytes) {
                invalid(reader, FormatErrorCode::invalidValue,
                        "Texture mipmap byte size is invalid or exceeds the allocation limit");
            }
            const std::size_t decodedMipmapBytes =
                static_cast<std::size_t>(mipmap.uncompressedSize);
            if (decodedMipmapBytes > maximumTextureBytes - decodedTextureBytes) {
                invalid(reader, FormatErrorCode::invalidValue,
                        "Texture mipmaps exceed the cumulative allocation limit");
            }
            validateDeclaredMipmapSize(texture, mipmap, reader);
            decodedTextureBytes += decodedMipmapBytes;

            const auto compressed = reader.readExact(
                static_cast<std::size_t>(mipmap.compressedSize)
            );
            mipmap.bytes.resize(static_cast<std::size_t>(mipmap.uncompressedSize));
            if (mipmap.compression == 0) {
                std::copy(compressed.begin(), compressed.end(), mipmap.bytes.begin());
            } else {
                const int result = LZ4_decompress_safe(
                    reinterpret_cast<const char*>(compressed.data()),
                    reinterpret_cast<char*>(mipmap.bytes.data()),
                    mipmap.compressedSize,
                    mipmap.uncompressedSize
                );
                if (result != mipmap.uncompressedSize) {
                    invalid(reader, FormatErrorCode::decompressionFailed,
                            "LZ4 texture mipmap decompressed to " +
                                std::to_string(result) + " bytes; expected " +
                                std::to_string(mipmap.uncompressedSize));
                }
            }

            image.mipmaps.push_back(std::move(mipmap));
        }
    }

    if (texture.isAnimated()) {
        const std::string animationMagic = readMagic(reader);
        if (magicEquals(animationMagic, "TEXS0001")) {
            texture.animationVersion = TextureAnimationVersion::texs0001;
        } else if (magicEquals(animationMagic, "TEXS0002")) {
            texture.animationVersion = TextureAnimationVersion::texs0002;
        } else if (magicEquals(animationMagic, "TEXS0003")) {
            texture.animationVersion = TextureAnimationVersion::texs0003;
        } else {
            invalid(reader, FormatErrorCode::invalidMagic,
                    "Unknown TEXS animation header: '" + animationMagic + "'");
        }

        const std::uint32_t frameCount = reader.readUInt32();
        if (frameCount == 0 || frameCount > maximumFrames) {
            invalid(reader, FormatErrorCode::invalidValue,
                    "Texture animation frame count is outside the supported range");
        }
        if (texture.animationVersion == TextureAnimationVersion::texs0003) {
            texture.gifWidth = reader.readUInt32();
            texture.gifHeight = reader.readUInt32();
        }

        texture.frames.reserve(frameCount);
        for (std::uint32_t index = 0; index < frameCount; ++index) {
            TextureFrame frame;
            frame.frameNumber = reader.readUInt32();
            frame.frameTime = reader.readFloat32();
            if (texture.animationVersion == TextureAnimationVersion::texs0001) {
                frame.x = static_cast<float>(reader.readUInt32());
                frame.y = static_cast<float>(reader.readUInt32());
                frame.width = static_cast<float>(reader.readUInt32());
                frame.widthAux = static_cast<float>(reader.readUInt32());
                frame.heightAux = static_cast<float>(reader.readUInt32());
                frame.height = static_cast<float>(reader.readUInt32());
            } else {
                frame.x = reader.readFloat32();
                frame.y = reader.readFloat32();
                frame.width = reader.readFloat32();
                frame.widthAux = reader.readFloat32();
                frame.heightAux = reader.readFloat32();
                frame.height = reader.readFloat32();
            }
            if (frame.frameNumber >= texture.imageCount ||
                !std::isfinite(frame.frameTime) || frame.frameTime < 0.0F ||
                !std::isfinite(frame.x) || !std::isfinite(frame.y) ||
                !std::isfinite(frame.width) || !std::isfinite(frame.height) ||
                !std::isfinite(frame.widthAux) ||
                !std::isfinite(frame.heightAux) || frame.width < 1.0F ||
                frame.height < 1.0F ||
                frame.width > std::numeric_limits<std::uint32_t>::max() ||
                frame.height > std::numeric_limits<std::uint32_t>::max()) {
                invalid(reader, FormatErrorCode::invalidValue,
                        "Texture animation frame dimensions, image index, or timing are invalid");
            }
            texture.frames.push_back(frame);
        }

        const TextureFrame& first = texture.frames.front();
        const auto [columns, rows] = checkedGridDimensions(
            texture,
            first.width,
            first.height,
            reader
        );
        if (texture.gifWidth == 0 || texture.gifHeight == 0) {
            texture.gifWidth = static_cast<std::uint32_t>(first.width);
            texture.gifHeight = static_cast<std::uint32_t>(first.height);
        }
        texture.spritesheetColumns = columns;
        texture.spritesheetRows = rows;
        if (texture.spritesheetColumns > 0 && texture.spritesheetRows > 0 &&
            static_cast<std::uint64_t>(texture.spritesheetColumns) *
                texture.spritesheetRows >= frameCount) {
            texture.spritesheetFrameCount = frameCount;
            double totalDuration = 0.0;
            for (const auto& frame : texture.frames) {
                totalDuration += frame.frameTime;
            }
            if (!std::isfinite(totalDuration) ||
                totalDuration > std::numeric_limits<float>::max()) {
                invalid(reader, FormatErrorCode::invalidValue,
                        "Texture animation duration exceeds the supported range");
            }
            texture.spritesheetDuration = static_cast<float>(totalDuration);
        } else {
            texture.spritesheetColumns = 0;
            texture.spritesheetRows = 0;
        }
    }

    if (metadata.has_value()) {
        parseMetadata(texture, *metadata, source + ".tex-json");
    }
    return texture;
}

Texture TextureParser::parseFile(
    const std::filesystem::path& path,
    std::optional<std::string_view> metadata
) {
    const auto bytes = readBinaryFile(path);
    return parse(bytes, path.string(), metadata);
}

std::size_t TextureParser::expectedBlockCompressedSize(
    TextureFormat format,
    std::uint32_t width,
    std::uint32_t height
) {
    const std::size_t blockWidth = (static_cast<std::size_t>(width) + 3) / 4;
    const std::size_t blockHeight = (static_cast<std::size_t>(height) + 3) / 4;
    const std::size_t bytesPerBlock =
        format == TextureFormat::dxt1 ? 8 : 16;
    return blockWidth * blockHeight * bytesPerBlock;
}

bool TextureParser::isBlockCompressed(TextureFormat format) noexcept {
    return format == TextureFormat::dxt1 || format == TextureFormat::dxt3 ||
        format == TextureFormat::dxt5 || format == TextureFormat::bc7;
}

}  // namespace we::scene
