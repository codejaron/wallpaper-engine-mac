#include <SceneText/SceneText.hpp>

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <memory>
#include <utility>

namespace we::scene::text {
namespace {

constexpr std::uint32_t kMaximumDimension = 16384;
constexpr std::size_t kMaximumBitmapBytes = 256U * 1024U * 1024U;

template <typename T>
struct CFRelease final {
    void operator()(T value) const noexcept {
        if (value != nullptr) ::CFRelease(static_cast<CFTypeRef>(value));
    }
};

template <typename T>
using CFPtr = std::unique_ptr<std::remove_pointer_t<T>, CFRelease<T>>;

template <typename T>
CFPtr<T> owned(T value) {
    return CFPtr<T>(value);
}

CTFontRef createFont(const FontSource& source, double pointSize) {
    if (!source.data.empty() && !source.systemName.empty()) {
        throw Error(ErrorCode::invalidArgument, "Text font must use bytes or a system name, not both");
    }
    if (!source.data.empty()) {
        auto data = owned(CFDataCreate(
            kCFAllocatorDefault,
            source.data.data(),
            static_cast<CFIndex>(source.data.size())
        ));
        if (!data) throw Error(ErrorCode::fontCreation, "Failed to copy text font bytes");
        auto provider = owned(CGDataProviderCreateWithCFData(data.get()));
        auto graphicsFont = owned(provider ? CGFontCreateWithDataProvider(provider.get()) : nullptr);
        if (!graphicsFont) {
            throw Error(ErrorCode::fontCreation, "Text font bytes do not contain a supported font face");
        }
        CTFontRef font = CTFontCreateWithGraphicsFont(
            graphicsFont.get(), pointSize, nullptr, nullptr
        );
        if (!font) throw Error(ErrorCode::fontCreation, "CoreText could not create a font from bytes");
        return font;
    }
    if (source.systemName.empty()) {
        throw Error(ErrorCode::invalidArgument, "Text font source is empty");
    }
    auto name = owned(CFStringCreateWithBytes(
        kCFAllocatorDefault,
        reinterpret_cast<const UInt8*>(source.systemName.data()),
        static_cast<CFIndex>(source.systemName.size()),
        kCFStringEncodingUTF8,
        false
    ));
    if (!name) throw Error(ErrorCode::invalidUTF8, "System font name is not valid UTF-8");
    CTFontRef font = CTFontCreateWithName(name.get(), pointSize, nullptr);
    if (!font) throw Error(ErrorCode::fontCreation, "CoreText could not create system font '" + source.systemName + "'");
    const auto matchesRequestedName = [&](CFStringRef actual) {
        if (!actual) return false;
        const CFIndex length = CFStringGetLength(actual);
        const CFIndex maximum = CFStringGetMaximumSizeForEncoding(
            length, kCFStringEncodingUTF8
        ) + 1;
        std::string value(static_cast<std::size_t>(maximum), '\0');
        if (!CFStringGetCString(actual, value.data(), maximum, kCFStringEncodingUTF8)) {
            return false;
        }
        value.resize(std::char_traits<char>::length(value.c_str()));
        const auto normalize = [](std::string text) {
            std::ranges::transform(text, text.begin(), [](unsigned char byte) {
                return static_cast<char>(std::tolower(byte));
            });
            return text;
        };
        return normalize(value) == normalize(source.systemName);
    };
    auto postScriptName = owned(CTFontCopyPostScriptName(font));
    auto familyName = owned(CTFontCopyFamilyName(font));
    auto fullName = owned(CTFontCopyFullName(font));
    if (!matchesRequestedName(postScriptName.get()) &&
        !matchesRequestedName(familyName.get()) &&
        !matchesRequestedName(fullName.get())) {
        ::CFRelease(font);
        throw Error(
            ErrorCode::fontCreation,
            "System font does not exist: '" + source.systemName + "'"
        );
    }
    return font;
}

}  // namespace

Error::Error(ErrorCode code, std::string message)
    : std::runtime_error(std::move(message)), code_(code) {}

ErrorCode Error::code() const noexcept { return code_; }

FontSource FontSource::bytes(std::span<const std::uint8_t> value) {
    return {.data = value, .systemName = {}};
}

FontSource FontSource::system(std::string name) {
    return {.data = {}, .systemName = std::move(name)};
}

RasterizedText rasterize(const RasterRequest& request) {
    if (!std::isfinite(request.pointSize) || request.pointSize <= 0.0) {
        throw Error(ErrorCode::invalidArgument, "Text point size must be finite and greater than zero");
    }
    auto font = owned(createFont(request.font, request.pointSize));
    if (request.utf8.empty()) {
        return {
            .width = 1,
            .height = 1,
            .bytesPerRow = 1,
            .coverage = {0},
        };
    }

    auto string = owned(CFStringCreateWithBytes(
        kCFAllocatorDefault,
        reinterpret_cast<const UInt8*>(request.utf8.data()),
        static_cast<CFIndex>(request.utf8.size()),
        kCFStringEncodingUTF8,
        false
    ));
    if (!string) throw Error(ErrorCode::invalidUTF8, "Text is not valid UTF-8");

    auto attributed = owned(CFAttributedStringCreateMutable(kCFAllocatorDefault, 0));
    if (!attributed) throw Error(ErrorCode::layout, "Failed to allocate attributed text");
    CFAttributedStringReplaceString(
        attributed.get(), CFRangeMake(0, 0), string.get()
    );
    const CFIndex length = CFStringGetLength(string.get());
    CFAttributedStringSetAttribute(
        attributed.get(), CFRangeMake(0, length), kCTFontAttributeName, font.get()
    );
    CFAttributedStringSetAttribute(
        attributed.get(), CFRangeMake(0, length),
        kCTForegroundColorFromContextAttributeName, kCFBooleanTrue
    );
    auto line = owned(CTLineCreateWithAttributedString(attributed.get()));
    if (!line) throw Error(ErrorCode::layout, "CoreText could not lay out text");

    CGFloat ascent = 0.0;
    CGFloat descent = 0.0;
    CGFloat leading = 0.0;
    const double typographicWidth = CTLineGetTypographicBounds(
        line.get(), &ascent, &descent, &leading
    );
    const CGRect imageBounds = CTLineGetImageBounds(line.get(), nullptr);
    if (!std::isfinite(typographicWidth) || !std::isfinite(imageBounds.origin.x) ||
        !std::isfinite(imageBounds.origin.y) || !std::isfinite(imageBounds.size.width) ||
        !std::isfinite(imageBounds.size.height)) {
        throw Error(ErrorCode::layout, "CoreText returned non-finite text bounds");
    }

    const double minimumX = std::floor(std::min(0.0, static_cast<double>(CGRectGetMinX(imageBounds))));
    const double maximumX = std::ceil(std::max(typographicWidth, static_cast<double>(CGRectGetMaxX(imageBounds))));
    const double minimumY = std::floor(std::min(-static_cast<double>(descent), static_cast<double>(CGRectGetMinY(imageBounds))));
    const double maximumY = std::ceil(std::max(static_cast<double>(ascent), static_cast<double>(CGRectGetMaxY(imageBounds))));
    const double widthValue = std::max(1.0, maximumX - minimumX);
    const double heightValue = std::max(1.0, maximumY - minimumY);
    if (widthValue > kMaximumDimension || heightValue > kMaximumDimension) {
        throw Error(ErrorCode::resourceLimit, "Rasterized text dimensions exceed the supported limit");
    }
    const auto width = static_cast<std::uint32_t>(widthValue);
    const auto height = static_cast<std::uint32_t>(heightValue);
    if (static_cast<std::size_t>(width) > kMaximumBitmapBytes / height) {
        throw Error(ErrorCode::resourceLimit, "Rasterized text bitmap exceeds the supported byte limit");
    }

    std::vector<std::uint8_t> topDown(static_cast<std::size_t>(width) * height, 0);
    auto colorSpace = owned(CGColorSpaceCreateDeviceGray());
    auto context = owned(colorSpace ? CGBitmapContextCreate(
        topDown.data(), width, height, 8, width, colorSpace.get(),
        static_cast<CGBitmapInfo>(kCGImageAlphaNone)
    ) : nullptr);
    if (!context) throw Error(ErrorCode::rasterization, "CoreGraphics could not create text bitmap context");
    CGContextSetShouldAntialias(context.get(), true);
    CGContextSetShouldSmoothFonts(context.get(), true);
    CGContextSetGrayFillColor(context.get(), 1.0, 1.0);
    CGContextSetTextMatrix(context.get(), CGAffineTransformIdentity);
    CGContextSetTextPosition(context.get(), -minimumX, -minimumY);
    CTLineDraw(line.get(), context.get());
    CGContextFlush(context.get());
    return {
        .width = width,
        .height = height,
        .bytesPerRow = width,
        .coverage = std::move(topDown),
        .baselineFromTop = maximumY,
        .typographicWidth = typographicWidth,
        .ascent = ascent,
        .descent = descent,
    };
}

}  // namespace we::scene::text
