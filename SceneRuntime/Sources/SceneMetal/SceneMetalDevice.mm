#include "SceneMetalDevice.hpp"

#include "SceneVideoDecoder.hpp"

#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <utility>

namespace we::scene::metal {
namespace {

constexpr std::size_t maximumTextureBytes = 512 * 1024 * 1024;
constexpr std::uint32_t maximumTextureDimension = 16'384;
constexpr std::uint32_t maximumTexture3DDimension = 2'048;
constexpr NSUInteger vertexBufferIndex = 30;

template <typename T>
MetalObject retainOwnedObject(T object) {
    MetalObject result((__bridge void*)object);
#if !__has_feature(objc_arc)
    [object release];
#endif
    return result;
}

template <typename T>
T object(const MetalObject& value) {
    return (__bridge T)value.get();
}

template <typename T>
class CFReference final {
public:
    explicit CFReference(T value = nullptr) : value_(value) {}
    ~CFReference() {
        if (value_ != nullptr) CFRelease(value_);
    }
    CFReference(const CFReference&) = delete;
    CFReference& operator=(const CFReference&) = delete;
    [[nodiscard]] T get() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return value_ != nullptr;
    }

private:
    T value_;
};

struct DecodedImage final {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba8;
};

void validateResourceDimensions(
    std::uint32_t width,
    std::uint32_t height,
    std::size_t bytesPerPixel,
    std::string_view description
) {
    if (width == 0 || height == 0) {
        throw Error(
            ErrorCode::invalidArgument,
            std::string(description) + " dimensions must be non-zero"
        );
    }
    if (width > maximumTextureDimension || height > maximumTextureDimension) {
        throw Error(
            ErrorCode::resourceValidation,
            std::string(description) + " exceeds Metal's 2D texture limit"
        );
    }
    const std::size_t widthValue = width;
    const std::size_t heightValue = height;
    if (widthValue > std::numeric_limits<std::size_t>::max() / heightValue ||
        widthValue * heightValue >
            std::numeric_limits<std::size_t>::max() / bytesPerPixel ||
        widthValue * heightValue * bytesPerPixel > maximumTextureBytes) {
        throw Error(
            ErrorCode::resourceValidation,
            std::string(description) + " exceeds the 512 MiB resource limit"
        );
    }
}

void validateResourceDimensions3D(
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t depth,
    std::size_t bytesPerPixel,
    std::string_view description
) {
    if (width == 0 || height == 0 || depth == 0) {
        throw Error(
            ErrorCode::invalidArgument,
            std::string(description) + " dimensions must be non-zero"
        );
    }
    if (width > maximumTexture3DDimension ||
        height > maximumTexture3DDimension ||
        depth > maximumTexture3DDimension) {
        throw Error(
            ErrorCode::resourceValidation,
            std::string(description) + " exceeds Metal's 3D texture limit"
        );
    }
    const std::size_t widthValue = width;
    const std::size_t heightValue = height;
    const std::size_t depthValue = depth;
    if (widthValue > std::numeric_limits<std::size_t>::max() / heightValue ||
        widthValue * heightValue >
            std::numeric_limits<std::size_t>::max() / depthValue ||
        widthValue * heightValue * depthValue >
            std::numeric_limits<std::size_t>::max() / bytesPerPixel ||
        widthValue * heightValue * depthValue * bytesPerPixel >
            maximumTextureBytes) {
        throw Error(
            ErrorCode::resourceValidation,
            std::string(description) + " exceeds the 512 MiB resource limit"
        );
    }
}

MTLPixelFormat metalPixelFormat(PixelFormat format) {
    switch (format) {
        case PixelFormat::rgba8: return MTLPixelFormatRGBA8Unorm;
        case PixelFormat::bgra8: return MTLPixelFormatBGRA8Unorm;
        case PixelFormat::r8: return MTLPixelFormatR8Unorm;
        case PixelFormat::rg16f: return MTLPixelFormatRG16Float;
        case PixelFormat::r16f: return MTLPixelFormatR16Float;
        case PixelFormat::rgba16snorm: return MTLPixelFormatRGBA16Snorm;
        case PixelFormat::depth32f: return MTLPixelFormatDepth32Float;
    }
    throw Error(
        ErrorCode::resourceValidation,
        "Unknown Metal framebuffer pixel format"
    );
}

std::size_t bytesPerPixel(PixelFormat format) {
    switch (format) {
        case PixelFormat::rgba8: return 4;
        case PixelFormat::bgra8: return 4;
        case PixelFormat::r8: return 1;
        case PixelFormat::rg16f: return 4;
        case PixelFormat::r16f: return 2;
        case PixelFormat::rgba16snorm: return 8;
        case PixelFormat::depth32f: return 4;
    }
    throw Error(
        ErrorCode::resourceValidation,
        "Unknown Metal framebuffer byte layout"
    );
}

DecodedImage decodeEmbeddedImage(
    std::span<const std::uint8_t> bytes,
    std::string_view source
) {
    if (bytes.empty() ||
        bytes.size() >
            static_cast<std::size_t>(std::numeric_limits<CFIndex>::max())) {
        throw Error(
            ErrorCode::textureDecode,
            "Embedded texture image is empty or too large in '" +
                std::string(source) + "'"
        );
    }
    CFReference<CFDataRef> data(CFDataCreate(
        kCFAllocatorDefault,
        bytes.data(),
        static_cast<CFIndex>(bytes.size())
    ));
    if (!data) {
        throw Error(
            ErrorCode::textureDecode,
            "Creating image data failed for texture '" +
                std::string(source) + "'"
        );
    }
    CFReference<CGImageSourceRef> imageSource(
        CGImageSourceCreateWithData(data.get(), nullptr)
    );
    if (!imageSource || CGImageSourceGetCount(imageSource.get()) == 0) {
        throw Error(
            ErrorCode::textureDecode,
            "ImageIO could not recognize embedded image data in texture '" +
                std::string(source) + "'"
        );
    }
    CFReference<CGImageRef> image(
        CGImageSourceCreateImageAtIndex(imageSource.get(), 0, nullptr)
    );
    if (!image) {
        throw Error(
            ErrorCode::textureDecode,
            "ImageIO could not decode embedded image data in texture '" +
                std::string(source) + "'"
        );
    }

    const std::size_t width = CGImageGetWidth(image.get());
    const std::size_t height = CGImageGetHeight(image.get());
    if (width == 0 || height == 0 ||
        width > std::numeric_limits<std::uint32_t>::max() ||
        height > std::numeric_limits<std::uint32_t>::max()) {
        throw Error(
            ErrorCode::textureDecode,
            "Decoded image dimensions are invalid in texture '" +
                std::string(source) + "'"
        );
    }
    validateResourceDimensions(
        static_cast<std::uint32_t>(width),
        static_cast<std::uint32_t>(height),
        4,
        "Decoded texture image"
    );

    DecodedImage result{
        .width = static_cast<std::uint32_t>(width),
        .height = static_cast<std::uint32_t>(height),
        .rgba8 = std::vector<std::uint8_t>(width * height * 4),
    };
    CFReference<CGColorSpaceRef> colorSpace(CGColorSpaceCreateDeviceRGB());
    if (!colorSpace) {
        throw Error(
            ErrorCode::textureDecode,
            "Creating an RGB color space failed for texture '" +
                std::string(source) + "'"
        );
    }
    CFReference<CGContextRef> context(CGBitmapContextCreate(
        result.rgba8.data(),
        width,
        height,
        8,
        width * 4,
        colorSpace.get(),
        static_cast<CGBitmapInfo>(
            static_cast<std::uint32_t>(kCGImageAlphaPremultipliedLast) |
            static_cast<std::uint32_t>(kCGBitmapByteOrder32Big)
        )
    ));
    if (!context) {
        throw Error(
            ErrorCode::textureDecode,
            "Creating an RGBA8 decode surface failed for texture '" +
                std::string(source) + "'"
        );
    }
    CGContextSetBlendMode(context.get(), kCGBlendModeCopy);
    CGContextDrawImage(
        context.get(),
        CGRectMake(0, 0, static_cast<CGFloat>(width),
                   static_cast<CGFloat>(height)),
        image.get()
    );

    for (std::size_t offset = 0; offset < result.rgba8.size(); offset += 4) {
        const std::uint32_t alpha = result.rgba8[offset + 3];
        if (alpha == 0) {
            result.rgba8[offset] = 0;
            result.rgba8[offset + 1] = 0;
            result.rgba8[offset + 2] = 0;
            continue;
        }
        if (alpha == 255) continue;
        for (std::size_t component = 0; component < 3; ++component) {
            const std::uint32_t value = result.rgba8[offset + component];
            result.rgba8[offset + component] = static_cast<std::uint8_t>(
                std::min<std::uint32_t>(
                    255, (value * 255 + alpha / 2) / alpha
                )
            );
        }
    }
    return result;
}

struct TextureUploadFormat final {
    MTLPixelFormat pixelFormat = MTLPixelFormatRGBA8Unorm;
    bool compressed = false;
    std::size_t bytesPerPixel = 4;
    std::size_t blockBytes = 0;
};

TextureUploadFormat uploadFormat(
    const Texture& texture,
    std::string_view source
) {
    if (texture.fileFormat != TextureFileFormat::unknown) return {};
    switch (texture.format) {
        case TextureFormat::argb8888:
            return {};
        case TextureFormat::r8:
            return {
                .pixelFormat = MTLPixelFormatR8Unorm,
                .bytesPerPixel = 1,
            };
        case TextureFormat::rg88:
            return {
                .pixelFormat = MTLPixelFormatRG8Unorm,
                .bytesPerPixel = 2,
            };
        case TextureFormat::dxt1:
            return {
                .pixelFormat = MTLPixelFormatBC1_RGBA,
                .compressed = true,
                .blockBytes = 8,
            };
        case TextureFormat::dxt3:
            return {
                .pixelFormat = MTLPixelFormatBC2_RGBA,
                .compressed = true,
                .blockBytes = 16,
            };
        case TextureFormat::dxt5:
            return {
                .pixelFormat = MTLPixelFormatBC3_RGBA,
                .compressed = true,
                .blockBytes = 16,
            };
        case TextureFormat::bc7:
            return {
                .pixelFormat = MTLPixelFormatBC7_RGBAUnorm,
                .compressed = true,
                .blockBytes = 16,
            };
        case TextureFormat::rgb888:
        case TextureFormat::rgb565:
        case TextureFormat::rg1616f:
        case TextureFormat::r16f:
        case TextureFormat::rgba1010102:
        case TextureFormat::rgba16161616f:
        case TextureFormat::rgb161616f:
        case TextureFormat::unknown:
            break;
    }
    throw Error(
        ErrorCode::textureUpload,
        "Texture '" + std::string(source) + "' uses unsupported format " +
            std::to_string(static_cast<std::uint32_t>(texture.format))
    );
}

TextureWrap textureWrap(const Texture& texture) {
    if ((texture.flags & textureFlagClampUVs) != 0) {
        return TextureWrap::clampToEdge;
    }
    if ((texture.flags & textureFlagClampUVsBorder) != 0) {
        return TextureWrap::clampToBorder;
    }
    return TextureWrap::repeat;
}

TextureFilter textureFilter(const Texture& texture) {
    return (texture.flags & textureFlagNoInterpolation) != 0
        ? TextureFilter::nearest
        : TextureFilter::linear;
}

MTLVertexFormat metalVertexFormat(VertexFormat format) {
    switch (format) {
        case VertexFormat::float2: return MTLVertexFormatFloat2;
        case VertexFormat::float3: return MTLVertexFormatFloat3;
        case VertexFormat::float4: return MTLVertexFormatFloat4;
        case VertexFormat::uint4: return MTLVertexFormatUInt4;
    }
    throw Error(ErrorCode::resourceValidation, "Unknown Metal vertex format");
}

MTLSamplerAddressMode metalAddressMode(TextureWrap wrap) {
    switch (wrap) {
        case TextureWrap::repeat: return MTLSamplerAddressModeRepeat;
        case TextureWrap::clampToEdge: return MTLSamplerAddressModeClampToEdge;
        case TextureWrap::clampToBorder:
            return MTLSamplerAddressModeClampToBorderColor;
    }
    throw Error(ErrorCode::resourceValidation, "Unknown Metal sampler wrap");
}

NSString* string(std::string_view value) {
    return [[NSString alloc]
        initWithBytes:value.data()
        length:value.size()
        encoding:NSUTF8StringEncoding];
}

std::string diagnostic(NSError* error) {
    if (error == nil) return "Metal returned no diagnostic";
    const char* value = error.localizedDescription.UTF8String;
    return value == nullptr ? "Metal returned an unreadable diagnostic" : value;
}

std::string pipelineKey(const DrawRequest& request) {
    std::ostringstream output;
    output << static_cast<int>(request.destination->format) << ':'
           << request.destination->hasDepth() << ':'
           << static_cast<int>(request.state.blending) << ':'
           << request.state.alphaToCoverage << ':'
           << request.state.cullBackFaces << ':'
           << request.state.depthTest << ':'
           << request.state.depthCompareGreater << ':'
           << request.state.depthWrite << ':'
           << request.state.depthClamp << ':'
           << request.state.writeColor << ':'
           << request.state.writeAlpha << ':'
           << request.state.alphaSourceOne << ':'
           << request.vertexLayout.stride;
    for (const auto& attribute : request.vertexLayout.attributes) {
        output << ':' << attribute.location << ','
               << static_cast<int>(attribute.format) << ','
               << attribute.offset;
    }
    return output.str();
}

id<MTLSamplerState> samplerState(
    id<MTLDevice> device,
    const TextureResource& texture,
    std::optional<TextureFilter> filterOverride,
    bool comparison,
    bool comparisonGreater
) {
    MTLSamplerDescriptor* descriptor = [[MTLSamplerDescriptor alloc] init];
    descriptor.sAddressMode = metalAddressMode(texture.wrap);
    descriptor.tAddressMode = metalAddressMode(texture.wrap);
    descriptor.rAddressMode = metalAddressMode(texture.wrap);
    descriptor.borderColor = MTLSamplerBorderColorTransparentBlack;
    const TextureFilter requestedFilter =
        filterOverride.value_or(texture.filter);
    const MTLSamplerMinMagFilter filter =
        requestedFilter == TextureFilter::nearest
            ? MTLSamplerMinMagFilterNearest
            : MTLSamplerMinMagFilterLinear;
    descriptor.minFilter = filter;
    descriptor.magFilter = filter;
    descriptor.mipFilter = texture.mipmapCount > 1
        ? (requestedFilter == TextureFilter::nearest
               ? MTLSamplerMipFilterNearest
               : MTLSamplerMipFilterLinear)
        : MTLSamplerMipFilterNotMipmapped;
    descriptor.compareFunction = comparison
        ? (comparisonGreater
               ? MTLCompareFunctionGreater
               : MTLCompareFunctionLessEqual)
        : MTLCompareFunctionNever;
    id<MTLSamplerState> result = [device newSamplerStateWithDescriptor:descriptor];
#if !__has_feature(objc_arc)
    [descriptor release];
#endif
    return result;
}

TextureResource makeTexture(
    id<MTLDevice> device,
    MTLPixelFormat pixelFormat,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t mipmapCount,
    MTLTextureUsage usage,
    MTLStorageMode storageMode,
    TextureWrap wrap,
    TextureFilter filter,
    PixelFormat logicalFormat = PixelFormat::rgba8
) {
    MTLTextureDescriptor* descriptor = [[MTLTextureDescriptor alloc] init];
    descriptor.textureType = MTLTextureType2D;
    descriptor.pixelFormat = pixelFormat;
    descriptor.width = width;
    descriptor.height = height;
    descriptor.mipmapLevelCount = mipmapCount;
    descriptor.sampleCount = 1;
    descriptor.arrayLength = 1;
    descriptor.storageMode = storageMode;
    descriptor.usage = usage;
    id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
#if !__has_feature(objc_arc)
    [descriptor release];
#endif
    if (texture == nil) {
        throw Error(
            ErrorCode::textureUpload,
            "Metal failed to allocate a 2D texture"
        );
    }
    return {
        .texture = retainOwnedObject(texture),
        .width = width,
        .height = height,
        .mipmapCount = mipmapCount,
        .format = logicalFormat,
        .wrap = wrap,
        .filter = filter,
    };
}

TextureResource makeTexture3D(
    id<MTLDevice> device,
    MTLPixelFormat pixelFormat,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t depth,
    std::uint32_t mipmapCount,
    TextureWrap wrap,
    TextureFilter filter
) {
    validateResourceDimensions3D(
        width, height, depth, 1, "3D texture"
    );
    MTLTextureDescriptor* descriptor = [[MTLTextureDescriptor alloc] init];
    descriptor.textureType = MTLTextureType3D;
    descriptor.pixelFormat = pixelFormat;
    descriptor.width = width;
    descriptor.height = height;
    descriptor.depth = depth;
    descriptor.mipmapLevelCount = mipmapCount;
    descriptor.sampleCount = 1;
    descriptor.arrayLength = 1;
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
#if !__has_feature(objc_arc)
    [descriptor release];
#endif
    if (texture == nil) {
        throw Error(
            ErrorCode::textureUpload,
            "Metal failed to allocate a 3D texture"
        );
    }
    return {
        .texture = retainOwnedObject(texture),
        .width = width,
        .height = height,
        .depth = depth,
        .mipmapCount = mipmapCount,
        .format = PixelFormat::rgba8,
        .dimension = TranslatedMetalShaderPair::TextureDimension::texture3D,
        .wrap = wrap,
        .filter = filter,
    };
}

void replaceTextureBytes(
    TextureResource& resource,
    std::uint32_t level,
    std::uint32_t width,
    std::uint32_t height,
    const void* bytes,
    std::size_t bytesPerRow,
    std::size_t bytesPerImage
) {
    id<MTLTexture> texture = object<id<MTLTexture>>(resource.texture);
    [texture replaceRegion:MTLRegionMake2D(0, 0, width, height)
               mipmapLevel:level
                      slice:0
                  withBytes:bytes
                bytesPerRow:bytesPerRow
              bytesPerImage:bytesPerImage];
}

void replaceTexture3DBytes(
    TextureResource& resource,
    std::uint32_t level,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t depth,
    const void* bytes,
    std::size_t bytesPerRow,
    std::size_t bytesPerImage
) {
    id<MTLTexture> texture = object<id<MTLTexture>>(resource.texture);
    [texture replaceRegion:MTLRegionMake3D(0, 0, 0, width, height, depth)
               mipmapLevel:level
                      slice:0
                  withBytes:bytes
                bytesPerRow:bytesPerRow
              bytesPerImage:bytesPerImage];
}

std::vector<std::uint8_t> repackLUT(
    std::span<const std::uint8_t> flat,
    std::uint32_t flatWidth,
    std::uint32_t flatHeight,
    std::uint32_t sliceWidth,
    std::uint32_t sliceHeight,
    std::uint32_t depth,
    std::size_t bytesPerPixel,
    std::string_view source
) {
    validateResourceDimensions3D(
        sliceWidth, sliceHeight, depth, bytesPerPixel, "LUT texture"
    );
    const bool horizontal =
        flatWidth == sliceWidth * depth && flatHeight == sliceHeight;
    const bool vertical =
        flatWidth == sliceWidth && flatHeight == sliceHeight * depth;
    if (!horizontal && !vertical) {
        throw Error(
            ErrorCode::textureUpload,
            "LUT mipmap dimensions " + std::to_string(flatWidth) + "x" +
                std::to_string(flatHeight) + " do not match horizontal " +
                std::to_string(sliceWidth * depth) + "x" +
                std::to_string(sliceHeight) + " or vertical " +
                std::to_string(sliceWidth) + "x" +
                std::to_string(sliceHeight * depth) +
                " 3D slice layouts in '" +
                std::string(source) + "'"
        );
    }
    const std::size_t expected = static_cast<std::size_t>(flatWidth) *
        flatHeight * bytesPerPixel;
    if (flat.size() != expected) {
        throw Error(
            ErrorCode::textureUpload,
            "LUT mipmap byte count does not match its dimensions in '" +
                std::string(source) + "'"
        );
    }
    std::vector<std::uint8_t> volume(expected);
    const std::size_t flatRowBytes =
        static_cast<std::size_t>(flatWidth) * bytesPerPixel;
    const std::size_t sliceRowBytes =
        static_cast<std::size_t>(sliceWidth) * bytesPerPixel;
    const std::size_t sliceBytes =
        static_cast<std::size_t>(sliceHeight) * sliceRowBytes;
    for (std::uint32_t z = 0; z < depth; ++z) {
        for (std::uint32_t y = 0; y < sliceHeight; ++y) {
            const std::size_t sourceOffset = horizontal
                ? static_cast<std::size_t>(y) * flatRowBytes +
                    static_cast<std::size_t>(z) * sliceRowBytes
                : (static_cast<std::size_t>(z) * sliceHeight + y) *
                    flatRowBytes;
            const std::size_t destinationOffset =
                static_cast<std::size_t>(z) * sliceBytes +
                static_cast<std::size_t>(y) * sliceRowBytes;
            std::copy_n(
                flat.data() + sourceOffset,
                sliceRowBytes,
                volume.data() + destinationOffset
            );
        }
    }
    return volume;
}

}  // namespace

MetalObject::MetalObject(void* borrowedObject) noexcept
    : value_(borrowedObject) {
    if (value_ != nullptr) {
        CFRetain(static_cast<CFTypeRef>(value_));
    }
}

MetalObject::~MetalObject() { reset(); }

MetalObject::MetalObject(MetalObject&& other) noexcept
    : value_(std::exchange(other.value_, nullptr)) {}

MetalObject& MetalObject::operator=(MetalObject&& other) noexcept {
    if (this == &other) return *this;
    reset();
    value_ = std::exchange(other.value_, nullptr);
    return *this;
}

void MetalObject::reset() noexcept {
    if (value_ == nullptr) return;
    CFRelease(static_cast<CFTypeRef>(value_));
    value_ = nullptr;
}

struct Program::Impl final {
    MetalObject vertexLibrary;
    MetalObject fragmentLibrary;
    MetalObject vertexFunction;
    MetalObject fragmentFunction;
    std::mutex mutex;
    std::map<std::string, MetalObject> pipelines;
};

Program::Program(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Program::~Program() = default;

AssetTextureResource::~AssetTextureResource() {
    if (videoDecoder != nullptr) destroyVideoDecoder(videoDecoder);
}

AssetTextureResource::AssetTextureResource(
    AssetTextureResource&& other
) noexcept
    : images(std::move(other.images)),
      imageWidths(std::move(other.imageWidths)),
      imageHeights(std::move(other.imageHeights)),
      resolution(std::exchange(other.resolution, {})),
      format(std::exchange(other.format, TextureFormat::unknown)),
      flags(std::exchange(other.flags, textureFlagNone)),
      frames(std::move(other.frames)),
      spritesheetColumns(std::exchange(other.spritesheetColumns, 0)),
      spritesheetRows(std::exchange(other.spritesheetRows, 0)),
      spritesheetFrameCount(std::exchange(other.spritesheetFrameCount, 0)),
      spritesheetDuration(std::exchange(other.spritesheetDuration, 0.0F)),
      videoDecoder(std::exchange(other.videoDecoder, nullptr)),
      lastUploadedVideoFrameSerial(std::exchange(
          other.lastUploadedVideoFrameSerial, 0
      )),
      lastVideoUpdateFrame(std::exchange(other.lastVideoUpdateFrame, 0)),
      video(std::exchange(other.video, false)) {}

AssetTextureResource& AssetTextureResource::operator=(
    AssetTextureResource&& other
) noexcept {
    if (this == &other) return *this;
    if (videoDecoder != nullptr) destroyVideoDecoder(videoDecoder);
    images = std::move(other.images);
    imageWidths = std::move(other.imageWidths);
    imageHeights = std::move(other.imageHeights);
    resolution = std::exchange(other.resolution, {});
    format = std::exchange(other.format, TextureFormat::unknown);
    flags = std::exchange(other.flags, textureFlagNone);
    frames = std::move(other.frames);
    spritesheetColumns = std::exchange(other.spritesheetColumns, 0);
    spritesheetRows = std::exchange(other.spritesheetRows, 0);
    spritesheetFrameCount = std::exchange(other.spritesheetFrameCount, 0);
    spritesheetDuration = std::exchange(other.spritesheetDuration, 0.0F);
    videoDecoder = std::exchange(other.videoDecoder, nullptr);
    lastUploadedVideoFrameSerial = std::exchange(
        other.lastUploadedVideoFrameSerial, 0
    );
    lastVideoUpdateFrame = std::exchange(other.lastVideoUpdateFrame, 0);
    video = std::exchange(other.video, false);
    return *this;
}

Device::Device() {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (device == nil) {
        throw Error(
            ErrorCode::contextCreation,
            "Metal is unavailable on the current Mac"
        );
    }
    device_ = retainOwnedObject(device);
    id<MTLCommandQueue> queue = [device newCommandQueue];
    if (queue == nil) {
        throw Error(
            ErrorCode::contextCreation,
            "Metal failed to create a command queue"
        );
    }
    commandQueue_ = retainOwnedObject(queue);
}

Device::Device(void* borrowedMetalDevice) {
    if (borrowedMetalDevice == nullptr) {
        throw Error(
            ErrorCode::invalidArgument,
            "Borrowed Metal device is null"
        );
    }
    id candidate = (__bridge id)borrowedMetalDevice;
    if (![candidate conformsToProtocol:@protocol(MTLDevice)]) {
        throw Error(
            ErrorCode::invalidArgument,
            "Borrowed object does not conform to MTLDevice"
        );
    }
    device_ = MetalObject(borrowedMetalDevice);
    id<MTLCommandQueue> queue =
        [(__bridge id<MTLDevice>)borrowedMetalDevice newCommandQueue];
    if (queue == nil) {
        throw Error(
            ErrorCode::contextCreation,
            "Metal failed to create a command queue"
        );
    }
    commandQueue_ = retainOwnedObject(queue);
}

Device::~Device() = default;

Device::Session Device::activate() { return Session(*this); }

Device::Session::Session(Device& device)
    : device_(device), lock_(device.mutex_) {
    id<MTLCommandQueue> queue =
        object<id<MTLCommandQueue>>(device_.commandQueue_);
    id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
    if (commandBuffer == nil) {
        throw Error(
            ErrorCode::contextCreation,
            "Metal failed to create a command buffer"
        );
    }
    commandBuffer_ = MetalObject((__bridge void*)commandBuffer);
}

Device::Session::~Session() {
    if (finished_) return;
    try {
        finish(false);
    } catch (const std::exception& error) {
        std::fprintf(
            stderr,
            "SceneMetal command submission failed during cleanup: %s\n",
            error.what()
        );
    }
}

std::shared_ptr<Program> Device::Session::createProgram(
    const TranslatedMetalShaderPair& shaders
) {
    if (shaders.vertex.empty() || shaders.fragment.empty() ||
        shaders.vertexEntryPoint.empty() || shaders.fragmentEntryPoint.empty()) {
        throw Error(
            ErrorCode::shaderCompilation,
            "Metal shader source and entry points are required"
        );
    }
    id<MTLDevice> device = object<id<MTLDevice>>(device_.device_);
    MTLCompileOptions* options = [[MTLCompileOptions alloc] init];
    options.fastMathEnabled = NO;
    options.languageVersion = MTLLanguageVersion2_4;
    NSError* vertexError = nil;
    NSString* vertexSource = string(shaders.vertex);
    id<MTLLibrary> vertexLibrary = [device
        newLibraryWithSource:vertexSource
        options:options
        error:&vertexError];
#if !__has_feature(objc_arc)
    [vertexSource release];
#endif
    if (vertexLibrary == nil) {
#if !__has_feature(objc_arc)
        [options release];
#endif
        throw Error(
            ErrorCode::shaderCompilation,
            "Metal vertex compilation failed: " + diagnostic(vertexError)
        );
    }
    NSError* fragmentError = nil;
    NSString* fragmentSource = string(shaders.fragment);
    id<MTLLibrary> fragmentLibrary = [device
        newLibraryWithSource:fragmentSource
        options:options
        error:&fragmentError];
#if !__has_feature(objc_arc)
    [fragmentSource release];
    [options release];
#endif
    if (fragmentLibrary == nil) {
#if !__has_feature(objc_arc)
        [vertexLibrary release];
#endif
        throw Error(
            ErrorCode::shaderCompilation,
            "Metal fragment compilation failed: " + diagnostic(fragmentError)
        );
    }

    NSString* vertexName = string(shaders.vertexEntryPoint);
    NSString* fragmentName = string(shaders.fragmentEntryPoint);
    id<MTLFunction> vertexFunction =
        [vertexLibrary newFunctionWithName:vertexName];
    id<MTLFunction> fragmentFunction =
        [fragmentLibrary newFunctionWithName:fragmentName];
#if !__has_feature(objc_arc)
    [vertexName release];
    [fragmentName release];
#endif
    if (vertexFunction == nil || fragmentFunction == nil) {
#if !__has_feature(objc_arc)
        [vertexLibrary release];
        [fragmentLibrary release];
        [vertexFunction release];
        [fragmentFunction release];
#endif
        throw Error(
            ErrorCode::programLink,
            "Metal library is missing a translated shader entry point"
        );
    }

    auto impl = std::make_unique<Program::Impl>();
    impl->vertexLibrary = retainOwnedObject(vertexLibrary);
    impl->fragmentLibrary = retainOwnedObject(fragmentLibrary);
    impl->vertexFunction = retainOwnedObject(vertexFunction);
    impl->fragmentFunction = retainOwnedObject(fragmentFunction);
    return std::shared_ptr<Program>(new Program(std::move(impl)));
}

void Device::Session::destroyProgram(
    std::shared_ptr<Program>& program
) noexcept {
    program.reset();
}

void Device::Session::uploadBuffer(
    BufferResource& destination,
    std::span<const std::byte> bytes
) {
    if (bytes.empty()) {
        destination.buffer.reset();
        destination.capacity = 0;
        return;
    }
    // A previous frame's command buffer may still reference destination.
    // Allocate immutable storage for this frame instead of mutating memory
    // that Metal has not finished consuming.
    id<MTLDevice> device = object<id<MTLDevice>>(device_.device_);
    id<MTLBuffer> buffer = [device
        newBufferWithLength:bytes.size()
        options:MTLResourceStorageModeShared];
    if (buffer == nil) {
        throw Error(
            ErrorCode::resourceValidation,
            "Metal failed to allocate a geometry buffer"
        );
    }
    destination.buffer = retainOwnedObject(buffer);
    destination.capacity = bytes.size();
    std::memcpy(buffer.contents, bytes.data(), bytes.size());
}

void Device::Session::destroyBuffer(BufferResource& buffer) noexcept {
    buffer.buffer.reset();
    buffer.capacity = 0;
}

FramebufferResource Device::Session::createFramebuffer(
    PixelFormat format,
    std::uint32_t width,
    std::uint32_t height,
    TextureWrap wrap,
    bool depthAttachment
) {
    validateResourceDimensions(
        width, height, bytesPerPixel(format), "Metal framebuffer"
    );
    id<MTLDevice> device = object<id<MTLDevice>>(device_.device_);
    FramebufferResource result{
        .colorTexture = makeTexture(
            device,
            metalPixelFormat(format),
            width,
            height,
            1,
            MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget,
            MTLStorageModePrivate,
            wrap,
            TextureFilter::linear,
            format
        ),
        .width = width,
        .height = height,
        .format = format,
    };
    if (depthAttachment) ensureDepthAttachment(result);
    return result;
}

void Device::Session::ensureDepthAttachment(
    FramebufferResource& framebuffer
) {
    if (!framebuffer) {
        throw Error(
            ErrorCode::resourceValidation,
            "A color target is required before adding Metal depth storage"
        );
    }
    if (framebuffer.depthTexture) return;
    id<MTLDevice> device = object<id<MTLDevice>>(device_.device_);
    MTLTextureDescriptor* descriptor = [[MTLTextureDescriptor alloc] init];
    descriptor.textureType = MTLTextureType2D;
    descriptor.pixelFormat = MTLPixelFormatDepth32Float;
    descriptor.width = framebuffer.width;
    descriptor.height = framebuffer.height;
    descriptor.mipmapLevelCount = 1;
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> depth = [device newTextureWithDescriptor:descriptor];
#if !__has_feature(objc_arc)
    [descriptor release];
#endif
    if (depth == nil) {
        throw Error(
            ErrorCode::framebufferCreation,
            "Metal failed to allocate depth storage"
        );
    }
    framebuffer.depthTexture = retainOwnedObject(depth);
    framebuffer.depthSampleTexture = {
        .texture = MetalObject((__bridge void*)depth),
        .width = framebuffer.width,
        .height = framebuffer.height,
        .mipmapCount = 1,
        .format = PixelFormat::depth32f,
        // Depth sampling must use the same address contract as the color
        // target. Shadow atlases are clamp-to-border; forcing edge here lets
        // PCF taps leak into the neighboring atlas tile.
        .wrap = framebuffer.colorTexture.wrap,
        .filter = TextureFilter::nearest,
    };
}

void Device::Session::destroyFramebuffer(
    FramebufferResource& framebuffer
) noexcept {
    framebuffer.colorTexture = {};
    framebuffer.depthTexture.reset();
    framebuffer.depthSampleTexture = {};
    framebuffer.width = 0;
    framebuffer.height = 0;
    framebuffer.format = PixelFormat::rgba8;
}

AssetTextureResource Device::Session::uploadTexture(
    const Texture& texture,
    std::string_view source
) {
    if (texture.images.empty()) {
        throw Error(
            ErrorCode::textureUpload,
            "Texture has no images: '" + std::string(source) + "'"
        );
    }
    AssetTextureResource result;
    result.format = texture.format;
    result.flags = texture.flags;
    result.frames = texture.frames;
    result.spritesheetColumns = texture.spritesheetColumns;
    result.spritesheetRows = texture.spritesheetRows;
    result.spritesheetFrameCount = texture.spritesheetFrameCount;
    result.spritesheetDuration = texture.spritesheetDuration;
    result.imageWidths.resize(texture.images.size());
    result.imageHeights.resize(texture.images.size());
    id<MTLDevice> device = object<id<MTLDevice>>(device_.device_);

    if (texture.isVideoMp4 ||
        (texture.flags & textureFlagVideo) != 0) {
        if (texture.images.front().mipmaps.empty()) {
            throw Error(
                ErrorCode::textureDecode,
                "Video texture has no encoded payload: '" +
                    std::string(source) + "'"
            );
        }
        const auto& payload = texture.images.front().mipmaps.front().bytes;
        const std::string sourceName(source);
        result.videoDecoder = createVideoDecoder(
            payload.data(), payload.size(), sourceName.c_str()
        );
        if (result.videoDecoder == nullptr) {
            throw Error(
                ErrorCode::textureDecode,
                "Creating the video decoder failed for '" +
                    std::string(source) + "'"
            );
        }
        VideoFrame frame;
        if (!copyLatestVideoFrame(result.videoDecoder, frame) ||
            frame.bytes == nullptr || frame.width == 0 || frame.height == 0) {
            throw Error(
                ErrorCode::textureDecode,
                "Decoding the first video frame failed for '" +
                    std::string(source) + "'"
            );
        }
        validateResourceDimensions(
            frame.width, frame.height, 4, "Video texture frame"
        );
        result.images.push_back(makeTexture(
            device,
            frame.pixelFormat == VideoFramePixelFormat::bgra8
                ? MTLPixelFormatBGRA8Unorm
                : MTLPixelFormatRGBA8Unorm,
            frame.width,
            frame.height,
            1,
            MTLTextureUsageShaderRead,
            MTLStorageModeShared,
            textureWrap(texture),
            textureFilter(texture)
        ));
        replaceTextureBytes(
            result.images.front(),
            0,
            frame.width,
            frame.height,
            frame.bytes,
            frame.bytesPerRow,
            frame.byteCount
        );
        result.imageWidths.front() = frame.width;
        result.imageHeights.front() = frame.height;
        result.resolution = {
            static_cast<float>(frame.width),
            static_cast<float>(frame.height),
            static_cast<float>(texture.width),
            static_cast<float>(texture.height),
        };
        result.lastUploadedVideoFrameSerial = frame.serial;
        result.video = true;
        return result;
    }

    const TextureUploadFormat format = uploadFormat(texture, source);
    if ((texture.flags & textureFlagLUT) != 0) {
        if (texture.isAnimated() || texture.images.size() != 1 ||
            texture.textureWidth == 0 || texture.textureHeight == 0 ||
            texture.width % texture.textureWidth != 0 ||
            texture.height != texture.textureHeight) {
            throw Error(
                ErrorCode::textureUpload,
                "LUT texture does not contain a valid tiled 3D layout: '" +
                    std::string(source) + "'"
            );
        }
        if (format.compressed) {
            throw Error(
                ErrorCode::textureUpload,
                "Compressed LUT textures are not supported as Metal 3D textures: '" +
                    std::string(source) + "'"
            );
        }
        const std::uint32_t lutDepth = texture.width / texture.textureWidth;
        validateResourceDimensions3D(
            texture.textureWidth,
            texture.textureHeight,
            lutDepth,
            format.bytesPerPixel,
            "LUT texture"
        );
        const TextureImage& image = texture.images.front();
        if (image.mipmaps.empty()) {
            throw Error(
                ErrorCode::textureUpload,
                "LUT texture has no mipmaps: '" + std::string(source) + "'"
            );
        }
        std::optional<DecodedImage> decodedBase;
        if (texture.fileFormat != TextureFileFormat::unknown) {
            decodedBase = decodeEmbeddedImage(
                image.mipmaps.front().bytes, source
            );
        }
        const std::uint32_t mipmapCount = decodedBase
            ? 1
            : static_cast<std::uint32_t>(image.mipmaps.size());
        TextureResource uploaded = makeTexture3D(
            device,
            format.pixelFormat,
            texture.textureWidth,
            texture.textureHeight,
            lutDepth,
            mipmapCount,
            textureWrap(texture),
            textureFilter(texture)
        );
        for (std::size_t level = 0; level < mipmapCount; ++level) {
            const std::uint32_t sliceWidth = std::max(
                1U, texture.textureWidth >> level
            );
            const std::uint32_t sliceHeight = std::max(
                1U, texture.textureHeight >> level
            );
            const std::uint32_t depth = std::max(1U, lutDepth >> level);
            std::span<const std::uint8_t> flat;
            std::uint32_t flatWidth = 0;
            std::uint32_t flatHeight = 0;
            if (decodedBase) {
                if (level != 0) {
                    throw Error(
                        ErrorCode::textureUpload,
                        "Embedded LUT unexpectedly contains multiple mipmaps: '" +
                            std::string(source) + "'"
                    );
                }
                flat = decodedBase->rgba8;
                flatWidth = decodedBase->width;
                flatHeight = decodedBase->height;
            } else {
                const TextureMipmap& mipmap = image.mipmaps[level];
                flat = mipmap.bytes;
                flatWidth = mipmap.width;
                flatHeight = mipmap.height;
            }
            const std::vector<std::uint8_t> volume = repackLUT(
                flat,
                flatWidth,
                flatHeight,
                sliceWidth,
                sliceHeight,
                depth,
                format.bytesPerPixel,
                source
            );
            replaceTexture3DBytes(
                uploaded,
                static_cast<std::uint32_t>(level),
                sliceWidth,
                sliceHeight,
                depth,
                volume.data(),
                static_cast<std::size_t>(sliceWidth) * format.bytesPerPixel,
                static_cast<std::size_t>(sliceWidth) * sliceHeight *
                    format.bytesPerPixel
            );
        }
        result.images.push_back(std::move(uploaded));
        result.imageWidths.front() = texture.textureWidth;
        result.imageHeights.front() = texture.textureHeight;
        result.resolution = {
            static_cast<float>(texture.textureWidth),
            static_cast<float>(texture.textureHeight),
            static_cast<float>(lutDepth),
            1.0F,
        };
        return result;
    }
    result.images.reserve(texture.images.size());
    for (std::size_t imageIndex = 0;
         imageIndex < texture.images.size(); ++imageIndex) {
        const TextureImage& image = texture.images[imageIndex];
        if (image.mipmaps.empty()) {
            throw Error(
                ErrorCode::textureUpload,
                "Texture image has no mipmaps in '" +
                    std::string(source) + "'"
            );
        }
        std::optional<DecodedImage> decodedBase;
        std::uint32_t baseWidth = image.mipmaps.front().width;
        std::uint32_t baseHeight = image.mipmaps.front().height;
        if (texture.fileFormat != TextureFileFormat::unknown) {
            decodedBase = decodeEmbeddedImage(
                image.mipmaps.front().bytes, source
            );
            baseWidth = decodedBase->width;
            baseHeight = decodedBase->height;
        }
        validateResourceDimensions(
            baseWidth,
            baseHeight,
            format.compressed ? 1 : format.bytesPerPixel,
            "Texture image"
        );
        const std::uint32_t mipmapCount =
            texture.fileFormat == TextureFileFormat::unknown
                ? static_cast<std::uint32_t>(image.mipmaps.size())
                : 1;
        TextureResource uploaded = makeTexture(
            device,
            format.pixelFormat,
            baseWidth,
            baseHeight,
            mipmapCount,
            MTLTextureUsageShaderRead,
            MTLStorageModeShared,
            textureWrap(texture),
            textureFilter(texture)
        );
        result.imageWidths[imageIndex] = baseWidth;
        result.imageHeights[imageIndex] = baseHeight;

        if (decodedBase) {
            replaceTextureBytes(
                uploaded,
                0,
                decodedBase->width,
                decodedBase->height,
                decodedBase->rgba8.data(),
                static_cast<std::size_t>(decodedBase->width) * 4,
                decodedBase->rgba8.size()
            );
        } else {
            for (std::size_t level = 0;
                 level < image.mipmaps.size(); ++level) {
                const TextureMipmap& mipmap = image.mipmaps[level];
                validateResourceDimensions(
                    mipmap.width,
                    mipmap.height,
                    format.compressed ? 1 : format.bytesPerPixel,
                    "Texture mipmap"
                );
                std::size_t rowBytes =
                    static_cast<std::size_t>(mipmap.width) *
                    format.bytesPerPixel;
                std::size_t imageBytes = mipmap.bytes.size();
                if (format.compressed) {
                    const std::size_t expected =
                        TextureParser::expectedBlockCompressedSize(
                            texture.format, mipmap.width, mipmap.height
                        );
                    if (mipmap.bytes.size() != expected) {
                        throw Error(
                            ErrorCode::textureUpload,
                            "Compressed mipmap byte count mismatch in '" +
                                std::string(source) + "'"
                        );
                    }
                    rowBytes =
                        ((static_cast<std::size_t>(mipmap.width) + 3) / 4) *
                        format.blockBytes;
                }
                replaceTextureBytes(
                    uploaded,
                    static_cast<std::uint32_t>(level),
                    mipmap.width,
                    mipmap.height,
                    mipmap.bytes.data(),
                    rowBytes,
                    imageBytes
                );
            }
        }
        result.images.push_back(std::move(uploaded));
    }

    if (texture.isAnimated()) {
        result.resolution = {
            static_cast<float>(texture.textureWidth),
            static_cast<float>(texture.textureHeight),
            static_cast<float>(texture.gifWidth),
            static_cast<float>(texture.gifHeight),
        };
    } else if (texture.fileFormat != TextureFileFormat::unknown) {
        result.resolution = {
            static_cast<float>(result.imageWidths.front()),
            static_cast<float>(result.imageHeights.front()),
            static_cast<float>(texture.width),
            static_cast<float>(texture.height),
        };
    } else {
        result.resolution = {
            static_cast<float>(texture.textureWidth),
            static_cast<float>(texture.textureHeight),
            static_cast<float>(texture.width),
            static_cast<float>(texture.height),
        };
    }
    return result;
}

void Device::Session::requestVideoTextureFrame(
    AssetTextureResource& texture,
    double timeSeconds
) {
    if (!texture.video || texture.videoDecoder == nullptr) return;
    if (!requestVideoFrame(texture.videoDecoder, timeSeconds)) {
        throw Error(
            ErrorCode::textureDecode,
            "Unable to decode a video texture frame"
        );
    }
}

bool Device::Session::updateVideoTexture(
    AssetTextureResource& texture,
    std::uint64_t frameSequence
) {
    if (!texture.video || texture.videoDecoder == nullptr ||
        texture.images.empty()) {
        return false;
    }
    if (texture.lastVideoUpdateFrame == frameSequence) return false;
    texture.lastVideoUpdateFrame = frameSequence;
    VideoFrame frame;
    if (!copyLatestVideoFrame(texture.videoDecoder, frame) ||
        frame.bytes == nullptr || frame.byteCount == 0 ||
        frame.bytesPerRow < static_cast<std::size_t>(frame.width) * 4) {
        throw Error(
            ErrorCode::textureDecode,
            "Unable to decode a video texture frame"
        );
    }
    if (frame.serial == texture.lastUploadedVideoFrameSerial) return false;
    validateResourceDimensions(
        frame.width, frame.height, 4, "Video texture frame"
    );
    id<MTLTexture> current =
        object<id<MTLTexture>>(texture.images.front().texture);
    const MTLPixelFormat expectedFormat =
        frame.pixelFormat == VideoFramePixelFormat::bgra8
            ? MTLPixelFormatBGRA8Unorm
            : MTLPixelFormatRGBA8Unorm;
    if (texture.imageWidths.front() != frame.width ||
        texture.imageHeights.front() != frame.height ||
        current.pixelFormat != expectedFormat) {
        id<MTLDevice> device = object<id<MTLDevice>>(device_.device_);
        texture.images.front() = makeTexture(
            device,
            expectedFormat,
            frame.width,
            frame.height,
            1,
            MTLTextureUsageShaderRead,
            MTLStorageModeShared,
            texture.images.front().wrap,
            texture.images.front().filter
        );
        texture.imageWidths.front() = frame.width;
        texture.imageHeights.front() = frame.height;
        texture.resolution[0] = static_cast<float>(frame.width);
        texture.resolution[1] = static_cast<float>(frame.height);
    }
    replaceTextureBytes(
        texture.images.front(),
        0,
        frame.width,
        frame.height,
        frame.bytes,
        frame.bytesPerRow,
        frame.byteCount
    );
    texture.lastUploadedVideoFrameSerial = frame.serial;
    return true;
}

void Device::Session::destroyTexture(
    AssetTextureResource& texture
) noexcept {
    if (texture.videoDecoder != nullptr) {
        destroyVideoDecoder(texture.videoDecoder);
        texture.videoDecoder = nullptr;
    }
    texture.images.clear();
    texture.imageWidths.clear();
    texture.imageHeights.clear();
    texture.resolution = {};
    texture.format = TextureFormat::unknown;
    texture.flags = textureFlagNone;
    texture.frames.clear();
    texture.spritesheetColumns = 0;
    texture.spritesheetRows = 0;
    texture.spritesheetFrameCount = 0;
    texture.spritesheetDuration = 0.0F;
    texture.lastUploadedVideoFrameSerial = 0;
    texture.lastVideoUpdateFrame = 0;
    texture.video = false;
}

TextureResource Device::Session::uploadCoverageTexture(
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t bytesPerRow,
    std::span<const std::uint8_t> coverage
) {
    validateResourceDimensions(width, height, 1, "Text coverage texture");
    if (bytesPerRow < width ||
        static_cast<std::size_t>(bytesPerRow) * height != coverage.size()) {
        throw Error(
            ErrorCode::invalidArgument,
            "Text coverage row layout is invalid"
        );
    }
    id<MTLDevice> device = object<id<MTLDevice>>(device_.device_);
    TextureResource result = makeTexture(
        device,
        MTLPixelFormatR8Unorm,
        width,
        height,
        1,
        MTLTextureUsageShaderRead,
        MTLStorageModeShared,
        TextureWrap::clampToEdge,
        TextureFilter::linear,
        PixelFormat::r8
    );
    replaceTextureBytes(
        result, 0, width, height, coverage.data(), bytesPerRow,
        coverage.size()
    );
    return result;
}

TextureResource Device::Session::uploadRGBA8Texture(
    std::uint32_t width,
    std::uint32_t height,
    std::span<const std::uint8_t> pixels
) {
    validateResourceDimensions(width, height, 4, "Host RGBA8 texture");
    const std::size_t expected =
        static_cast<std::size_t>(width) * height * 4;
    if (pixels.size() != expected) {
        throw Error(
            ErrorCode::invalidArgument,
            "Host RGBA8 texture requires tightly packed pixel storage"
        );
    }
    id<MTLDevice> device = object<id<MTLDevice>>(device_.device_);
    TextureResource result = makeTexture(
        device,
        MTLPixelFormatRGBA8Unorm,
        width,
        height,
        1,
        MTLTextureUsageShaderRead,
        MTLStorageModeShared,
        TextureWrap::clampToEdge,
        TextureFilter::linear
    );
    replaceTextureBytes(
        result,
        0,
        width,
        height,
        pixels.data(),
        static_cast<std::size_t>(width) * 4,
        pixels.size()
    );
    return result;
}

TextureResource Device::Session::uploadRGBA16SnormTexture(
    std::uint32_t width,
    std::uint32_t height,
    std::span<const std::uint16_t> pixels
) {
    validateResourceDimensions(width, height, 8, "Puppet morph texture");
    const std::size_t expected =
        static_cast<std::size_t>(width) * height * 4;
    if (pixels.size() != expected) {
        throw Error(
            ErrorCode::invalidArgument,
            "Puppet morph texture requires tightly packed RGBA16 storage"
        );
    }
    id<MTLDevice> device = object<id<MTLDevice>>(device_.device_);
    TextureResource result = makeTexture(
        device,
        MTLPixelFormatRGBA16Snorm,
        width,
        height,
        1,
        MTLTextureUsageShaderRead,
        MTLStorageModeShared,
        TextureWrap::clampToEdge,
        TextureFilter::nearest,
        PixelFormat::rgba16snorm
    );
    replaceTextureBytes(
        result,
        0,
        width,
        height,
        pixels.data(),
        static_cast<std::size_t>(width) * 8,
        pixels.size_bytes()
    );
    return result;
}

void Device::Session::destroyTexture(TextureResource& texture) noexcept {
    texture = {};
}

void Device::Session::clear(
    FramebufferResource& framebuffer,
    std::array<float, 4> color,
    bool clearDepth,
    float depthValue
) {
    if (!framebuffer) {
        throw Error(
            ErrorCode::resourceValidation,
            "Metal clear requires a framebuffer"
        );
    }
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture =
        object<id<MTLTexture>>(framebuffer.colorTexture.texture);
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(
        color[0], color[1], color[2], color[3]
    );
    if (framebuffer.hasDepth()) {
        pass.depthAttachment.texture =
            object<id<MTLTexture>>(framebuffer.depthTexture);
        pass.depthAttachment.loadAction = clearDepth
            ? MTLLoadActionClear
            : MTLLoadActionLoad;
        pass.depthAttachment.storeAction = MTLStoreActionStore;
        if (!std::isfinite(depthValue) || depthValue < 0.0F || depthValue > 1.0F) {
            throw Error(
                ErrorCode::resourceValidation,
                "Metal depth clear value must be finite and in the range 0...1"
            );
        }
        pass.depthAttachment.clearDepth = depthValue;
    }
    id<MTLCommandBuffer> commandBuffer =
        object<id<MTLCommandBuffer>>(commandBuffer_);
    id<MTLRenderCommandEncoder> encoder =
        [commandBuffer renderCommandEncoderWithDescriptor:pass];
    if (encoder == nil) {
        throw Error(ErrorCode::draw, "Metal failed to encode a clear pass");
    }
    [encoder endEncoding];
}

void Device::Session::copy(
    const TextureResource& source,
    TextureResource& destination
) {
    if (!source || !destination || source.width != destination.width ||
        source.height != destination.height) {
        throw Error(
            ErrorCode::invalidArgument,
            "Metal texture copy requires matching non-empty textures"
        );
    }
    id<MTLCommandBuffer> commandBuffer =
        object<id<MTLCommandBuffer>>(commandBuffer_);
    id<MTLBlitCommandEncoder> encoder = [commandBuffer blitCommandEncoder];
    if (encoder == nil) {
        throw Error(ErrorCode::draw, "Metal failed to encode a texture copy");
    }
    [encoder copyFromTexture:object<id<MTLTexture>>(source.texture)
                  sourceSlice:0
                  sourceLevel:0
                 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(source.width, source.height, 1)
                    toTexture:object<id<MTLTexture>>(destination.texture)
             destinationSlice:0
             destinationLevel:0
            destinationOrigin:MTLOriginMake(0, 0, 0)];
    [encoder endEncoding];
}

void Device::Session::draw(
    const DrawRequest& request,
    std::uint32_t vertexStart,
    std::uint32_t vertexCount
) {
    encodeDraw(request, vertexStart, vertexCount, false);
}

void Device::Session::drawIndexed(
    const DrawRequest& request,
    std::uint32_t indexCount,
    bool index32
) {
    encodeDraw(request, std::nullopt, indexCount, index32);
}

FramebufferResource Device::Session::framebufferForDrawable(
    void* metalDrawable
) {
    if (metalDrawable == nullptr) {
        throw Error(
            ErrorCode::invalidArgument,
            "Metal drawable is required for presentation"
        );
    }
    id candidate = (__bridge id)metalDrawable;
    if (![candidate conformsToProtocol:@protocol(CAMetalDrawable)]) {
        throw Error(
            ErrorCode::invalidArgument,
            "Presentation object does not conform to CAMetalDrawable"
        );
    }
    id<MTLTexture> texture = ((id<CAMetalDrawable>)candidate).texture;
    if (texture == nil || texture.device !=
            object<id<MTLDevice>>(device_.device_)) {
        throw Error(
            ErrorCode::invalidArgument,
            "Metal drawable texture is empty or belongs to another device"
        );
    }
    if (texture.width == 0 || texture.height == 0 ||
        texture.width > maximumTextureDimension ||
        texture.height > maximumTextureDimension) {
        throw Error(
            ErrorCode::resourceValidation,
            "Metal drawable dimensions exceed the supported texture range"
        );
    }
    PixelFormat format;
    switch (texture.pixelFormat) {
        case MTLPixelFormatRGBA8Unorm:
            format = PixelFormat::rgba8;
            break;
        case MTLPixelFormatBGRA8Unorm:
            format = PixelFormat::bgra8;
            break;
        default:
            throw Error(
                ErrorCode::invalidArgument,
                "Metal drawable must use RGBA8Unorm or BGRA8Unorm"
            );
    }
    const auto width = static_cast<std::uint32_t>(texture.width);
    const auto height = static_cast<std::uint32_t>(texture.height);
    return {
        .colorTexture = TextureResource{
            .texture = MetalObject((__bridge void*)texture),
            .width = width,
            .height = height,
            .mipmapCount = 1,
            .format = format,
            .wrap = TextureWrap::clampToEdge,
            .filter = TextureFilter::linear,
        },
        .width = width,
        .height = height,
        .format = format,
    };
}

void Device::Session::encodeDraw(
    const DrawRequest& request,
    std::optional<std::uint32_t> vertexStart,
    std::uint32_t count,
    bool index32
) {
    const bool hasVertexInput = !request.vertexLayout.attributes.empty();
    if (!request.program || request.destination == nullptr ||
        !*request.destination ||
        request.instanceCount == 0 ||
        (hasVertexInput &&
         (request.vertexBuffer == nullptr || !request.vertexBuffer->buffer ||
          request.vertexLayout.stride == 0))) {
        throw Error(
            ErrorCode::invalidArgument,
            "Metal draw request is incomplete"
        );
    }
    if (!vertexStart &&
        (request.indexBuffer == nullptr || !request.indexBuffer->buffer)) {
        throw Error(
            ErrorCode::invalidArgument,
            "Indexed Metal draw requires an index buffer"
        );
    }
    Program::Impl& program = *request.program->impl_;
    const std::string key = pipelineKey(request);
    id<MTLRenderPipelineState> pipeline = nil;
    {
        std::lock_guard lock(program.mutex);
        const auto cached = program.pipelines.find(key);
        if (cached != program.pipelines.end()) {
            pipeline = object<id<MTLRenderPipelineState>>(cached->second);
        } else {
            id<MTLDevice> device = object<id<MTLDevice>>(device_.device_);
            MTLRenderPipelineDescriptor* descriptor =
                [[MTLRenderPipelineDescriptor alloc] init];
            descriptor.vertexFunction =
                object<id<MTLFunction>>(program.vertexFunction);
            descriptor.fragmentFunction =
                object<id<MTLFunction>>(program.fragmentFunction);
            descriptor.colorAttachments[0].pixelFormat =
                metalPixelFormat(request.destination->format);
            descriptor.depthAttachmentPixelFormat =
                request.destination->hasDepth()
                    ? MTLPixelFormatDepth32Float
                    : MTLPixelFormatInvalid;
            MTLRenderPipelineColorAttachmentDescriptor* color =
                descriptor.colorAttachments[0];
            descriptor.alphaToCoverageEnabled = request.state.alphaToCoverage;
            color.writeMask = request.state.writeColor
                ? static_cast<MTLColorWriteMask>(
                      MTLColorWriteMaskRed | MTLColorWriteMaskGreen |
                      MTLColorWriteMaskBlue |
                      (request.state.writeAlpha
                           ? MTLColorWriteMaskAlpha
                           : MTLColorWriteMaskNone)
                  )
                : MTLColorWriteMaskNone;
            if (request.state.blending != BlendMode::replace) {
                color.blendingEnabled = YES;
                color.rgbBlendOperation = MTLBlendOperationAdd;
                color.alphaBlendOperation = MTLBlendOperationAdd;
                color.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
                color.sourceAlphaBlendFactor = request.state.alphaSourceOne
                    ? MTLBlendFactorOne
                    : MTLBlendFactorSourceAlpha;
                color.destinationRGBBlendFactor =
                    request.state.blending == BlendMode::additive
                        ? MTLBlendFactorOne
                        : MTLBlendFactorOneMinusSourceAlpha;
                color.destinationAlphaBlendFactor =
                    request.state.blending == BlendMode::additive
                        ? MTLBlendFactorOne
                        : MTLBlendFactorOneMinusSourceAlpha;
            }
            MTLVertexDescriptor* vertexDescriptor =
                [[MTLVertexDescriptor alloc] init];
            for (const auto& attribute : request.vertexLayout.attributes) {
                if (attribute.location >= 31) {
#if !__has_feature(objc_arc)
                    [vertexDescriptor release];
                    [descriptor release];
#endif
                    throw Error(
                        ErrorCode::resourceValidation,
                        "Metal vertex attribute location exceeds 30"
                    );
                }
                vertexDescriptor.attributes[attribute.location].format =
                    metalVertexFormat(attribute.format);
                vertexDescriptor.attributes[attribute.location].offset =
                    attribute.offset;
                vertexDescriptor.attributes[attribute.location].bufferIndex =
                    vertexBufferIndex;
            }
            if (hasVertexInput) {
                vertexDescriptor.layouts[vertexBufferIndex].stride =
                    request.vertexLayout.stride;
                vertexDescriptor.layouts[vertexBufferIndex].stepFunction =
                    MTLVertexStepFunctionPerVertex;
            }
            descriptor.vertexDescriptor = vertexDescriptor;
            NSError* error = nil;
            id<MTLRenderPipelineState> created = [device
                newRenderPipelineStateWithDescriptor:descriptor
                error:&error];
#if !__has_feature(objc_arc)
            [vertexDescriptor release];
            [descriptor release];
#endif
            if (created == nil) {
                throw Error(
                    ErrorCode::programLink,
                    "Metal pipeline creation failed: " + diagnostic(error)
                );
            }
            auto [inserted, didInsert] = program.pipelines.try_emplace(key);
            inserted->second = retainOwnedObject(created);
            pipeline = object<id<MTLRenderPipelineState>>(inserted->second);
        }
    }

    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture =
        object<id<MTLTexture>>(request.destination->colorTexture.texture);
    pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    if (request.destination->hasDepth()) {
        pass.depthAttachment.texture =
            object<id<MTLTexture>>(request.destination->depthTexture);
        pass.depthAttachment.loadAction = MTLLoadActionLoad;
        pass.depthAttachment.storeAction = MTLStoreActionStore;
    }
    id<MTLCommandBuffer> commandBuffer =
        object<id<MTLCommandBuffer>>(commandBuffer_);
    id<MTLRenderCommandEncoder> encoder =
        [commandBuffer renderCommandEncoderWithDescriptor:pass];
    if (encoder == nil) {
        throw Error(ErrorCode::draw, "Metal failed to create a draw encoder");
    }
    [encoder setRenderPipelineState:pipeline];
    const DrawRequest::Region viewport = request.viewport.value_or(
        DrawRequest::Region{
            .width = request.destination->width,
            .height = request.destination->height,
        }
    );
    const DrawRequest::Region scissor = request.scissor.value_or(
        DrawRequest::Region{
            .x = viewport.x,
            .y = viewport.y,
            .width = viewport.width,
            .height = viewport.height,
        }
    );
    const auto validRegion = [&](const DrawRequest::Region& region) {
        return region.width > 0 && region.height > 0 &&
            static_cast<std::uint64_t>(region.x) + region.width <=
                request.destination->width &&
            static_cast<std::uint64_t>(region.y) + region.height <=
                request.destination->height;
    };
    if (!validRegion(viewport) || !validRegion(scissor)) {
        [encoder endEncoding];
        throw Error(
            ErrorCode::resourceValidation,
            "Metal draw viewport/scissor exceeds its destination"
        );
    }
    [encoder setViewport:MTLViewport{
        static_cast<double>(viewport.x),
        static_cast<double>(viewport.y),
        static_cast<double>(viewport.width),
        static_cast<double>(viewport.height),
        0.0,
        1.0,
    }];
    [encoder setScissorRect:MTLScissorRect{
        scissor.x,
        scissor.y,
        scissor.width,
        scissor.height,
    }];
    [encoder setCullMode:request.state.cullBackFaces
        ? MTLCullModeBack
        : MTLCullModeNone];
    [encoder setFrontFacingWinding:MTLWindingClockwise];
    [encoder setDepthClipMode:request.state.depthClamp
        ? MTLDepthClipModeClamp
        : MTLDepthClipModeClip];
    [encoder setDepthBias:request.depthBias
               slopeScale:request.depthSlopeScale
                    clamp:request.depthBiasClamp];

    id<MTLDevice> device = object<id<MTLDevice>>(device_.device_);
    MTLDepthStencilDescriptor* depthDescriptor =
        [[MTLDepthStencilDescriptor alloc] init];
    depthDescriptor.depthCompareFunction = !request.state.depthTest
        ? MTLCompareFunctionAlways
        : request.state.depthCompareGreater
            ? MTLCompareFunctionGreater
            : MTLCompareFunctionLessEqual;
    depthDescriptor.depthWriteEnabled = request.state.depthWrite;
    id<MTLDepthStencilState> depthState =
        [device newDepthStencilStateWithDescriptor:depthDescriptor];
#if !__has_feature(objc_arc)
    [depthDescriptor release];
#endif
    if (depthState == nil) {
        [encoder endEncoding];
        throw Error(
            ErrorCode::draw,
            "Metal failed to create depth-stencil state"
        );
    }
    [encoder setDepthStencilState:depthState];

    if (hasVertexInput) {
        id<MTLBuffer> vertexBuffer =
            object<id<MTLBuffer>>(request.vertexBuffer->buffer);
        [encoder setVertexBuffer:vertexBuffer
                          offset:request.vertexBufferOffset
                         atIndex:vertexBufferIndex];
    }
    for (const auto& uniform : request.uniforms) {
        if (uniform.bytes == nullptr || uniform.byteCount == 0) {
            [encoder endEncoding];
            throw Error(
                ErrorCode::resourceValidation,
                "Metal uniform binding has no bytes"
            );
        }
        if (uniform.vertexBufferIndex) {
            [encoder setVertexBytes:uniform.bytes
                            length:uniform.byteCount
                           atIndex:*uniform.vertexBufferIndex];
        }
        if (uniform.fragmentBufferIndex) {
            [encoder setFragmentBytes:uniform.bytes
                              length:uniform.byteCount
                             atIndex:*uniform.fragmentBufferIndex];
        }
    }
    std::vector<MetalObject> samplers;
    samplers.reserve(request.textures.size());
    for (const auto& binding : request.textures) {
        if (binding.texture == nullptr || !*binding.texture) {
            [encoder endEncoding];
            throw Error(
                ErrorCode::resourceValidation,
                "Metal texture binding is empty"
            );
        }
        id<MTLTexture> texture =
            object<id<MTLTexture>>(binding.texture->texture);
        id<MTLSamplerState> sampler = samplerState(
            device, *binding.texture, binding.filterOverride,
            binding.comparison, binding.comparisonGreater
        );
        if (sampler == nil) {
            [encoder endEncoding];
            throw Error(
                ErrorCode::resourceValidation,
                "Metal failed to create a sampler state"
            );
        }
        samplers.push_back(retainOwnedObject(sampler));
        if (binding.vertexTextureIndex) {
            [encoder setVertexTexture:texture
                              atIndex:*binding.vertexTextureIndex];
        }
        if (binding.vertexSamplerIndex) {
            [encoder setVertexSamplerState:sampler
                                   atIndex:*binding.vertexSamplerIndex];
        }
        if (binding.fragmentTextureIndex) {
            [encoder setFragmentTexture:texture
                                atIndex:*binding.fragmentTextureIndex];
        }
        if (binding.fragmentSamplerIndex) {
            [encoder setFragmentSamplerState:sampler
                                     atIndex:*binding.fragmentSamplerIndex];
        }
    }
    if (vertexStart) {
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                    vertexStart:*vertexStart
                    vertexCount:count
                  instanceCount:request.instanceCount];
    } else {
        [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                            indexCount:count
                             indexType:index32
                                 ? MTLIndexTypeUInt32
                                 : MTLIndexTypeUInt16
                           indexBuffer:object<id<MTLBuffer>>(
                               request.indexBuffer->buffer
                           )
                     indexBufferOffset:request.indexBufferOffset
                       instanceCount:request.instanceCount
                          baseVertex:0
                        baseInstance:0];
    }
    [encoder endEncoding];
#if !__has_feature(objc_arc)
    [depthState release];
#endif
}

void Device::Session::present(void* metalDrawable) {
    if (metalDrawable == nullptr) {
        throw Error(
            ErrorCode::invalidArgument,
            "Metal drawable is required for presentation"
        );
    }
    id candidate = (__bridge id)metalDrawable;
    if (![candidate conformsToProtocol:@protocol(CAMetalDrawable)]) {
        throw Error(
            ErrorCode::invalidArgument,
            "Presentation object does not conform to CAMetalDrawable"
        );
    }
    id<MTLCommandBuffer> commandBuffer =
        object<id<MTLCommandBuffer>>(commandBuffer_);
    [commandBuffer presentDrawable:(id<CAMetalDrawable>)candidate];
}

void Device::Session::readRGBA8(
    const FramebufferResource& framebuffer,
    std::span<std::uint8_t> output
) {
    if (!framebuffer ||
        (framebuffer.format != PixelFormat::rgba8 &&
         framebuffer.format != PixelFormat::bgra8)) {
        throw Error(
            ErrorCode::readback,
            "RGBA8 readback requires an RGBA8 or BGRA8 Metal framebuffer"
        );
    }
    const std::size_t rowBytes =
        static_cast<std::size_t>(framebuffer.width) * 4;
    const std::size_t required = rowBytes * framebuffer.height;
    if (output.size() < required) {
        throw Error(
            ErrorCode::invalidArgument,
            "RGBA8 output buffer is too small: expected " +
                std::to_string(required) + " bytes, provided " +
                std::to_string(output.size())
        );
    }
    const std::size_t alignedRowBytes = (rowBytes + 255) & ~std::size_t(255);
    id<MTLDevice> device = object<id<MTLDevice>>(device_.device_);
    id<MTLBuffer> buffer = [device
        newBufferWithLength:alignedRowBytes * framebuffer.height
        options:MTLResourceStorageModeShared];
    if (buffer == nil) {
        throw Error(
            ErrorCode::readback,
            "Metal failed to allocate the readback buffer"
        );
    }
    MetalObject retainedBuffer = retainOwnedObject(buffer);
    id<MTLCommandBuffer> commandBuffer =
        object<id<MTLCommandBuffer>>(commandBuffer_);
    id<MTLBlitCommandEncoder> encoder = [commandBuffer blitCommandEncoder];
    if (encoder == nil) {
        throw Error(
            ErrorCode::readback,
            "Metal failed to encode framebuffer readback"
        );
    }
    [encoder copyFromTexture:object<id<MTLTexture>>(
                                 framebuffer.colorTexture.texture
                             )
                  sourceSlice:0
                  sourceLevel:0
                 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(
                       framebuffer.width, framebuffer.height, 1
                   )
                     toBuffer:buffer
            destinationOffset:0
       destinationBytesPerRow:alignedRowBytes
     destinationBytesPerImage:alignedRowBytes * framebuffer.height];
    [encoder endEncoding];
    finish(true);
    const auto* bytes = static_cast<const std::uint8_t*>(buffer.contents);
    for (std::size_t row = 0; row < framebuffer.height; ++row) {
        const auto* source = bytes + row * alignedRowBytes;
        auto* destination = output.data() + row * rowBytes;
        if (framebuffer.format == PixelFormat::rgba8) {
            std::memcpy(destination, source, rowBytes);
            continue;
        }
        for (std::size_t column = 0; column < framebuffer.width; ++column) {
            const std::size_t offset = column * 4;
            destination[offset] = source[offset + 2];
            destination[offset + 1] = source[offset + 1];
            destination[offset + 2] = source[offset];
            destination[offset + 3] = source[offset + 3];
        }
    }
}

void Device::Session::readRGBA8Async(
    const FramebufferResource& framebuffer,
    std::function<void(std::vector<std::uint8_t>, std::string)> completion
) {
    if (!completion) {
        throw Error(
            ErrorCode::invalidArgument,
            "RGBA8 readback completion is required"
        );
    }
    if (!framebuffer ||
        (framebuffer.format != PixelFormat::rgba8 &&
         framebuffer.format != PixelFormat::bgra8)) {
        throw Error(
            ErrorCode::readback,
            "RGBA8 readback requires an RGBA8 or BGRA8 Metal framebuffer"
        );
    }
    const std::size_t rowBytes =
        static_cast<std::size_t>(framebuffer.width) * 4;
    const std::size_t required = rowBytes * framebuffer.height;
    const std::size_t alignedRowBytes = (rowBytes + 255) & ~std::size_t(255);
    id<MTLDevice> device = object<id<MTLDevice>>(device_.device_);
    id<MTLBuffer> buffer = [device
        newBufferWithLength:alignedRowBytes * framebuffer.height
        options:MTLResourceStorageModeShared];
    if (buffer == nil) {
        throw Error(
            ErrorCode::readback,
            "Metal failed to allocate the asynchronous readback buffer"
        );
    }
    MetalObject retainedBuffer = retainOwnedObject(buffer);
    id<MTLCommandBuffer> commandBuffer =
        object<id<MTLCommandBuffer>>(commandBuffer_);
    id<MTLBlitCommandEncoder> encoder = [commandBuffer blitCommandEncoder];
    if (encoder == nil) {
        throw Error(
            ErrorCode::readback,
            "Metal failed to encode asynchronous framebuffer readback"
        );
    }
    [encoder copyFromTexture:object<id<MTLTexture>>(
                                 framebuffer.colorTexture.texture
                             )
                  sourceSlice:0
                  sourceLevel:0
                 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(
                       framebuffer.width, framebuffer.height, 1
                   )
                     toBuffer:buffer
            destinationOffset:0
       destinationBytesPerRow:alignedRowBytes
     destinationBytesPerImage:alignedRowBytes * framebuffer.height];
    [encoder endEncoding];

    const PixelFormat format = framebuffer.format;
    const std::uint32_t height = framebuffer.height;
    auto handler = std::move(completion);
    [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> completed) {
        if (completed.status == MTLCommandBufferStatusError) {
            handler({}, "Metal command buffer failed: " + diagnostic(completed.error));
            return;
        }
        try {
            std::vector<std::uint8_t> pixels(required);
            const auto* bytes = static_cast<const std::uint8_t*>(buffer.contents);
            for (std::size_t row = 0; row < height; ++row) {
                const auto* source = bytes + row * alignedRowBytes;
                auto* destination = pixels.data() + row * rowBytes;
                if (format == PixelFormat::rgba8) {
                    std::memcpy(destination, source, rowBytes);
                    continue;
                }
                for (std::size_t column = 0; column < rowBytes / 4; ++column) {
                    const std::size_t offset = column * 4;
                    destination[offset] = source[offset + 2];
                    destination[offset + 1] = source[offset + 1];
                    destination[offset + 2] = source[offset];
                    destination[offset + 3] = source[offset + 3];
                }
            }
            handler(std::move(pixels), {});
        } catch (const std::exception& error) {
            handler({}, "Copying asynchronous RGBA8 readback failed: " +
                std::string(error.what()));
        } catch (...) {
            handler({}, "Copying asynchronous RGBA8 readback failed");
        }
    }];
    finish(false);
}

void Device::Session::finish(bool waitForCompletion) {
    if (finished_) return;
    id<MTLCommandBuffer> commandBuffer =
        object<id<MTLCommandBuffer>>(commandBuffer_);
    if (!waitForCompletion) {
        [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> completed) {
            if (completed.status != MTLCommandBufferStatusError) return;
            const std::string message = diagnostic(completed.error);
            std::fprintf(
                stderr,
                "SceneMetal asynchronous command buffer failed: %s\n",
                message.c_str()
            );
        }];
    }
    [commandBuffer commit];
    if (waitForCompletion) [commandBuffer waitUntilCompleted];
    if (waitForCompletion &&
        commandBuffer.status == MTLCommandBufferStatusError) {
        const std::string message = diagnostic(commandBuffer.error);
        finished_ = true;
        throw Error(
            ErrorCode::draw,
            "Metal command buffer failed: " + message
        );
    }
    finished_ = true;
}

}  // namespace we::scene::metal
