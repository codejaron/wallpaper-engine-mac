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
#include <vector>

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

CFPtr<CFMutableAttributedStringRef> createAttributedString(
    CFStringRef string,
    CTFontRef font,
    double characterSpacing
) {
    auto attributed = owned(CFAttributedStringCreateMutable(kCFAllocatorDefault, 0));
    if (!attributed) {
        throw Error(ErrorCode::layout, "Failed to allocate attributed text");
    }
    CFAttributedStringReplaceString(
        attributed.get(), CFRangeMake(0, 0), string
    );
    const CFIndex length = CFStringGetLength(string);
    if (length == 0) return attributed;
    const CFRange range = CFRangeMake(0, length);
    CFAttributedStringSetAttribute(
        attributed.get(), range, kCTFontAttributeName, font
    );
    CFAttributedStringSetAttribute(
        attributed.get(), range,
        kCTForegroundColorFromContextAttributeName, kCFBooleanTrue
    );
    if (characterSpacing != 0.0) {
        const CGFloat value = static_cast<CGFloat>(characterSpacing);
        auto number = owned(CFNumberCreate(
            kCFAllocatorDefault, kCFNumberCGFloatType, &value
        ));
        if (!number) {
            throw Error(ErrorCode::layout, "Failed to allocate text character spacing");
        }
        CFAttributedStringSetAttribute(
            attributed.get(), range, kCTKernAttributeName, number.get()
        );
    }
    return attributed;
}

struct LineLayout final {
    CFRange sourceRange{};
    CFPtr<CTLineRef> line;
    double width = 0.0;
    CGRect imageBounds = CGRectNull;
    bool hasImageBounds = false;
    double originX = 0.0;
    double baselineY = 0.0;
};

void measureLine(LineLayout& layout) {
    CGFloat ascent = 0.0;
    CGFloat descent = 0.0;
    CGFloat leading = 0.0;
    layout.width = CTLineGetTypographicBounds(
        layout.line.get(), &ascent, &descent, &leading
    );
    if (!std::isfinite(layout.width) || !std::isfinite(ascent) ||
        !std::isfinite(descent) || !std::isfinite(leading)) {
        throw Error(ErrorCode::layout, "CoreText returned non-finite text metrics");
    }
    layout.imageBounds = CTLineGetImageBounds(layout.line.get(), nullptr);
    if (!CGRectIsNull(layout.imageBounds) &&
        !CGRectIsInfinite(layout.imageBounds)) {
        const auto finite = [](CGFloat value) {
            return std::isfinite(static_cast<double>(value));
        };
        if (!finite(layout.imageBounds.origin.x) ||
            !finite(layout.imageBounds.origin.y) ||
            !finite(layout.imageBounds.size.width) ||
            !finite(layout.imageBounds.size.height)) {
            throw Error(ErrorCode::layout, "CoreText returned non-finite text bounds");
        }
        layout.hasImageBounds =
            layout.imageBounds.size.width > 0.0 &&
            layout.imageBounds.size.height > 0.0;
    }
}

bool isLineBreak(UniChar character) noexcept {
    return character == '\n' || character == '\r' ||
           character == 0x2028 || character == 0x2029;
}

CFIndex lineBreakLength(CFStringRef string, CFIndex index, CFIndex length) {
    const UniChar character = CFStringGetCharacterAtIndex(string, index);
    if (character == '\r' && index + 1 < length &&
        CFStringGetCharacterAtIndex(string, index + 1) == '\n') {
        return 2;
    }
    return 1;
}

CFPtr<CTLineRef> ellipsizedLine(
    CFStringRef source,
    CFRange range,
    CTFontRef font,
    double characterSpacing,
    double maximumWidth
) {
    auto visible = owned(CFStringCreateWithSubstring(
        kCFAllocatorDefault, source, range
    ));
    auto value = owned(visible ? CFStringCreateMutableCopy(
        kCFAllocatorDefault, 0, visible.get()
    ) : nullptr);
    if (!value) {
        throw Error(ErrorCode::layout, "Failed to allocate truncated text");
    }
    const UniChar ellipsis = 0x2026;
    CFStringAppendCharacters(value.get(), &ellipsis, 1);
    auto attributed = createAttributedString(
        value.get(), font, characterSpacing
    );
    auto sourceLine = owned(CTLineCreateWithAttributedString(attributed.get()));
    if (!sourceLine) {
        throw Error(ErrorCode::layout, "CoreText could not lay out truncated text");
    }
    if (maximumWidth <= 0.0) return sourceLine;

    auto tokenString = owned(CFStringCreateWithCharacters(
        kCFAllocatorDefault, &ellipsis, 1
    ));
    auto tokenAttributed = createAttributedString(
        tokenString.get(), font, characterSpacing
    );
    auto tokenLine = owned(CTLineCreateWithAttributedString(tokenAttributed.get()));
    auto result = owned(CTLineCreateTruncatedLine(
        sourceLine.get(), static_cast<double>(maximumWidth),
        kCTLineTruncationEnd, tokenLine.get()
    ));
    if (!result) {
        throw Error(
            ErrorCode::layout,
            "Text maximum width is too small to fit the ellipsis token"
        );
    }
    return result;
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
    if (!std::isfinite(request.maximumWidth) || request.maximumWidth < 0.0) {
        throw Error(ErrorCode::invalidArgument, "Text maximum width must be finite and non-negative");
    }
    if (!std::isfinite(request.characterSpacing) ||
        !std::isfinite(request.lineSpacing)) {
        throw Error(ErrorCode::invalidArgument, "Text spacing must be finite");
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

    auto attributed = createAttributedString(
        string.get(), font.get(), request.characterSpacing
    );
    const CFIndex length = CFStringGetLength(string.get());
    auto typesetter = owned(CTTypesetterCreateWithAttributedString(attributed.get()));
    if (!typesetter) {
        throw Error(ErrorCode::layout, "CoreText could not create a text typesetter");
    }

    const double fontAscent = CTFontGetAscent(font.get());
    const double fontDescent = CTFontGetDescent(font.get());
    const double fontLeading = std::max(0.0, static_cast<double>(CTFontGetLeading(font.get())));
    const double baseLineHeight = fontAscent + fontDescent + fontLeading;
    const double lineAdvance = baseLineHeight + request.lineSpacing;
    if (!std::isfinite(lineAdvance) || lineAdvance <= 0.0) {
        throw Error(
            ErrorCode::invalidArgument,
            "Text line spacing must leave a positive line advance"
        );
    }

    std::vector<LineLayout> lines;
    bool truncated = false;
    const auto rowLimitReached = [&] {
        return request.maximumRows != 0 &&
               lines.size() >= request.maximumRows;
    };
    const auto appendLine = [&](CFRange range) {
        auto line = owned(CTTypesetterCreateLine(typesetter.get(), range));
        if (!line) {
            throw Error(ErrorCode::layout, "CoreText could not lay out a text line");
        }
        LineLayout layout{
            .sourceRange = range,
            .line = std::move(line),
        };
        measureLine(layout);
        lines.push_back(std::move(layout));
        const double minimumHeight =
            baseLineHeight +
            static_cast<double>(lines.size() - 1) * lineAdvance;
        if (minimumHeight > static_cast<double>(kMaximumDimension)) {
            throw Error(
                ErrorCode::resourceLimit,
                "Rasterized text height exceeds the supported limit"
            );
        }
    };

    CFIndex paragraphStart = 0;
    while (paragraphStart <= length) {
        CFIndex paragraphEnd = paragraphStart;
        while (paragraphEnd < length &&
               !isLineBreak(CFStringGetCharacterAtIndex(string.get(), paragraphEnd))) {
            ++paragraphEnd;
        }

        if (paragraphStart == paragraphEnd) {
            if (rowLimitReached()) {
                truncated = true;
                break;
            }
            appendLine(CFRangeMake(paragraphStart, 0));
        } else {
            CFIndex lineStart = paragraphStart;
            while (lineStart < paragraphEnd) {
                if (rowLimitReached()) {
                    truncated = true;
                    break;
                }
                CFIndex count = paragraphEnd - lineStart;
                if (request.maximumWidth > 0.0) {
                    count = CTTypesetterSuggestLineBreak(
                        typesetter.get(), lineStart, request.maximumWidth
                    );
                    count = std::min(count, paragraphEnd - lineStart);
                    if (count <= 0) {
                        count = CTTypesetterSuggestClusterBreak(
                            typesetter.get(), lineStart, request.maximumWidth
                        );
                        count = std::min(count, paragraphEnd - lineStart);
                    }
                    if (count <= 0) {
                        const CFRange cluster =
                            CFStringGetRangeOfComposedCharactersAtIndex(
                                string.get(), lineStart
                            );
                        count = std::min(
                            cluster.length, paragraphEnd - lineStart
                        );
                    }
                }
                if (count <= 0) {
                    throw Error(ErrorCode::layout, "CoreText could not advance text layout");
                }
                appendLine(CFRangeMake(lineStart, count));
                lineStart += count;
            }
            if (truncated) break;
        }

        if (paragraphEnd == length) break;
        paragraphStart = paragraphEnd + lineBreakLength(
            string.get(), paragraphEnd, length
        );
        if (paragraphStart == length) {
            if (rowLimitReached()) {
                truncated = true;
            } else {
                appendLine(CFRangeMake(length, 0));
            }
            break;
        }
    }

    if (lines.empty()) {
        throw Error(ErrorCode::layout, "Text layout did not produce a line");
    }
    if (truncated && request.useEllipsis) {
        LineLayout& last = lines.back();
        last.line = ellipsizedLine(
            string.get(), last.sourceRange, font.get(),
            request.characterSpacing, request.maximumWidth
        );
        measureLine(last);
    }

    double typographicWidth = 0.0;
    for (const LineLayout& line : lines) {
        typographicWidth = std::max(typographicWidth, line.width);
    }
    const double typographicHeight =
        baseLineHeight +
        static_cast<double>(lines.size() - 1) * lineAdvance;

    double minimumX = 0.0;
    double maximumX = typographicWidth;
    double minimumY = -fontDescent - fontLeading -
        static_cast<double>(lines.size() - 1) * lineAdvance;
    double maximumY = fontAscent;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        LineLayout& line = lines[index];
        switch (request.horizontalAlignment) {
            case HorizontalAlignment::left:
                line.originX = 0.0;
                break;
            case HorizontalAlignment::center:
                line.originX = (typographicWidth - line.width) * 0.5;
                break;
            case HorizontalAlignment::right:
                line.originX = typographicWidth - line.width;
                break;
        }
        line.baselineY = -static_cast<double>(index) * lineAdvance;
        if (!line.hasImageBounds) continue;
        minimumX = std::min(
            minimumX,
            line.originX + static_cast<double>(CGRectGetMinX(line.imageBounds))
        );
        maximumX = std::max(
            maximumX,
            line.originX + static_cast<double>(CGRectGetMaxX(line.imageBounds))
        );
        minimumY = std::min(
            minimumY,
            line.baselineY + static_cast<double>(CGRectGetMinY(line.imageBounds))
        );
        maximumY = std::max(
            maximumY,
            line.baselineY + static_cast<double>(CGRectGetMaxY(line.imageBounds))
        );
    }
    minimumX = std::floor(minimumX);
    maximumX = std::ceil(maximumX);
    minimumY = std::floor(minimumY);
    maximumY = std::ceil(maximumY);
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
    for (const LineLayout& line : lines) {
        CGContextSetTextPosition(
            context.get(),
            line.originX - minimumX,
            line.baselineY - minimumY
        );
        CTLineDraw(line.line.get(), context.get());
    }
    CGContextFlush(context.get());
    return {
        .width = width,
        .height = height,
        .bytesPerRow = width,
        .coverage = std::move(topDown),
        .baselineFromTop = maximumY,
        .logicalLeftFromBitmap = -minimumX,
        .logicalTopFromBitmap = maximumY - fontAscent,
        .typographicWidth = typographicWidth,
        .typographicHeight = typographicHeight,
        .ascent = fontAscent,
        .descent = fontDescent,
        .lineCount = lines.size(),
        .truncated = truncated,
    };
}

}  // namespace we::scene::text
