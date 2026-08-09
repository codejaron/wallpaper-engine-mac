#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>

#include "SceneVideoDecoder.hpp"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace we::scene::gl {
namespace {

constexpr double timeComparisonTolerance = 1.0e-9;
constexpr double maximumSequentialDecodeJump = 0.25;
constexpr double seekLookbehindSeconds = 1.0;

struct PixelStorage final {
    CVPixelBufferRef pixelBuffer = nullptr;
    std::vector<std::uint8_t> convertedRGBA8;
    std::size_t byteCount = 0;
    std::size_t bytesPerRow = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    VideoFramePixelFormat pixelFormat = VideoFramePixelFormat::bgra8;

    ~PixelStorage() {
        if (pixelBuffer != nullptr) {
            CVPixelBufferRelease(pixelBuffer);
        }
    }

    PixelStorage(const PixelStorage&) = delete;
    PixelStorage& operator=(const PixelStorage&) = delete;

    explicit PixelStorage(CVPixelBufferRef buffer) : pixelBuffer(buffer) {
        CVPixelBufferRetain(pixelBuffer);
    }
};

struct PixelView final {
    std::shared_ptr<const PixelStorage> storage;
    const std::uint8_t* bytes = nullptr;
    bool locked = false;

    ~PixelView() {
        if (locked) {
            CVPixelBufferUnlockBaseAddress(
                storage->pixelBuffer, kCVPixelBufferLock_ReadOnly
            );
        }
    }
};

struct DecodedFrame final {
    std::shared_ptr<PixelStorage> pixels;
    CMTime presentationTime = kCMTimeInvalid;
    std::uint64_t readerEpoch = 0;
};

struct Decoder;

struct ReaderState final {
    AVAssetReader* reader = nil;
    AVAssetReaderVideoCompositionOutput* output = nil;
    std::shared_ptr<DecodedFrame> current;
    std::shared_ptr<DecodedFrame> next;
    double lastTarget = -1.0;
    std::uint64_t epoch = 0;
    bool reachedEnd = false;

    void reset(Decoder& decoder, double target);
    [[nodiscard]] std::shared_ptr<DecodedFrame> readNext(Decoder& decoder);
    [[nodiscard]] std::shared_ptr<DecodedFrame> frameAt(
        Decoder& decoder,
        double target
    );
};

struct Decoder final {
    NSURL* url = nil;
    AVAsset* asset = nil;
    AVAssetTrack* videoTrack = nil;
    AVVideoComposition* videoComposition = nil;
    double durationSeconds = 0.0;
    bool containsAlpha = false;
    bool premultipliedAlpha = false;
    std::string source;

    ReaderState reader;
    std::mutex mutex;
    std::condition_variable condition;
    std::thread worker;
    bool stopping = false;
    bool requestPending = false;
    double requestedTime = 0.0;
    std::uint64_t requestGeneration = 0;
    std::uint64_t completedGeneration = 0;
    std::optional<std::string> failure;
    std::shared_ptr<DecodedFrame> published;
    std::uint64_t publishedSerial = 0;

    ~Decoder() {
        {
            std::lock_guard guard(mutex);
            stopping = true;
        }
        condition.notify_one();
        if (worker.joinable()) worker.join();

        reader = {};
        videoComposition = nil;
        videoTrack = nil;
        asset = nil;
        if (url != nil) {
            [[NSFileManager defaultManager] removeItemAtURL:url error:nil];
            url = nil;
        }
    }
};

[[nodiscard]] std::size_t checkedByteCount(
    std::size_t bytesPerRow,
    std::size_t height
) {
    if (height != 0 && bytesPerRow >
            std::numeric_limits<std::size_t>::max() / height) {
        throw std::overflow_error("video pixel buffer byte count overflows size_t");
    }
    return bytesPerRow * height;
}

[[nodiscard]] std::uint8_t unpremultiply(
    std::uint8_t value,
    std::uint8_t alpha
) noexcept {
    if (alpha == 0) return 0;
    if (alpha == 255) return value;
    return static_cast<std::uint8_t>(std::min<std::uint32_t>(
        255,
        (static_cast<std::uint32_t>(value) * 255 + alpha / 2) / alpha
    ));
}

[[nodiscard]] bool pixelBufferIsOpaque(CVPixelBufferRef buffer) noexcept {
    const CFTypeRef value = CVBufferCopyAttachment(
        buffer, kCVImageBufferAlphaChannelIsOpaque, nullptr
    );
    const bool opaque = value == kCFBooleanTrue;
    if (value != nullptr) CFRelease(value);
    return opaque;
}

[[nodiscard]] bool pixelBufferUsesPremultipliedAlpha(
    CVPixelBufferRef buffer,
    const Decoder& decoder
) noexcept {
    if (!decoder.containsAlpha || pixelBufferIsOpaque(buffer)) return false;
    const CFTypeRef mode = CVBufferCopyAttachment(
        buffer, kCVImageBufferAlphaChannelModeKey, nullptr
    );
    bool premultiplied = decoder.premultipliedAlpha;
    if (mode != nullptr) {
        premultiplied = CFEqual(
            mode, kCVImageBufferAlphaChannelMode_PremultipliedAlpha
        );
        CFRelease(mode);
    }
    return premultiplied;
}

[[nodiscard]] std::shared_ptr<PixelStorage> makePixelStorage(
    CVPixelBufferRef buffer,
    const Decoder& decoder
) {
    if (buffer == nullptr || CVPixelBufferGetPixelFormatType(buffer) !=
            kCVPixelFormatType_32BGRA) {
        throw std::runtime_error("AVFoundation returned a non-BGRA video frame");
    }
    const std::size_t width = CVPixelBufferGetWidth(buffer);
    const std::size_t height = CVPixelBufferGetHeight(buffer);
    const std::size_t bytesPerRow = CVPixelBufferGetBytesPerRow(buffer);
    if (width == 0 || height == 0 || width > UINT32_MAX ||
        height > UINT32_MAX || bytesPerRow < width * 4 ||
        bytesPerRow % 4 != 0) {
        throw std::runtime_error("AVFoundation returned invalid video dimensions");
    }

    auto result = std::make_shared<PixelStorage>(buffer);
    result->width = static_cast<std::uint32_t>(width);
    result->height = static_cast<std::uint32_t>(height);
    if (pixelBufferUsesPremultipliedAlpha(buffer, decoder)) {
        if (CVPixelBufferLockBaseAddress(
                buffer, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) {
            throw std::runtime_error(
                "Unable to lock the decoded video pixel buffer"
            );
        }
        const auto* source = static_cast<const std::uint8_t*>(
            CVPixelBufferGetBaseAddress(buffer)
        );
        if (source == nullptr) {
            CVPixelBufferUnlockBaseAddress(
                buffer, kCVPixelBufferLock_ReadOnly
            );
            throw std::runtime_error(
                "Decoded video pixel buffer has no base address"
            );
        }
        const std::size_t tightBytesPerRow = width * 4;
        try {
            result->convertedRGBA8.resize(checkedByteCount(
                tightBytesPerRow, height
            ));
            for (std::size_t y = 0; y < height; ++y) {
                const std::uint8_t* sourceRow = source + y * bytesPerRow;
                std::uint8_t* destinationRow =
                    result->convertedRGBA8.data() + y * tightBytesPerRow;
                for (std::size_t x = 0; x < width; ++x) {
                    const std::uint8_t* sourcePixel = sourceRow + x * 4;
                    std::uint8_t* destinationPixel = destinationRow + x * 4;
                    const std::uint8_t alpha = sourcePixel[3];
                    destinationPixel[0] = unpremultiply(sourcePixel[2], alpha);
                    destinationPixel[1] = unpremultiply(sourcePixel[1], alpha);
                    destinationPixel[2] = unpremultiply(sourcePixel[0], alpha);
                    destinationPixel[3] = alpha;
                }
            }
        } catch (...) {
            CVPixelBufferUnlockBaseAddress(
                buffer, kCVPixelBufferLock_ReadOnly
            );
            throw;
        }
        CVPixelBufferUnlockBaseAddress(
            buffer, kCVPixelBufferLock_ReadOnly
        );
        result->bytesPerRow = tightBytesPerRow;
        result->byteCount = result->convertedRGBA8.size();
        result->pixelFormat = VideoFramePixelFormat::rgba8;
        CVPixelBufferRelease(result->pixelBuffer);
        result->pixelBuffer = nullptr;
    } else {
        result->bytesPerRow = bytesPerRow;
        result->byteCount = checkedByteCount(bytesPerRow, height);
        result->pixelFormat = VideoFramePixelFormat::bgra8;
    }
    return result;
}

[[nodiscard]] std::shared_ptr<PixelView> makePixelView(
    const std::shared_ptr<PixelStorage>& storage
) {
    auto result = std::make_shared<PixelView>();
    result->storage = storage;
    if (!storage->convertedRGBA8.empty()) {
        result->bytes = storage->convertedRGBA8.data();
        return result;
    }
    if (CVPixelBufferLockBaseAddress(
            storage->pixelBuffer,
            kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) {
        throw std::runtime_error("Unable to lock the decoded video pixel buffer");
    }
    result->locked = true;
    result->bytes = static_cast<const std::uint8_t*>(
        CVPixelBufferGetBaseAddress(storage->pixelBuffer)
    );
    if (result->bytes == nullptr) {
        throw std::runtime_error(
            "Decoded video pixel buffer has no base address"
        );
    }
    return result;
}

[[nodiscard]] bool trackContainsAlpha(AVAssetTrack* track) noexcept {
    for (id value in track.formatDescriptions) {
        const auto description = (__bridge CMFormatDescriptionRef)value;
        const CFDictionaryRef extensions =
            CMFormatDescriptionGetExtensions(description);
        if (extensions == nullptr) continue;
        const CFTypeRef contains = CFDictionaryGetValue(
            extensions, kCMFormatDescriptionExtension_ContainsAlphaChannel
        );
        if (contains == kCFBooleanTrue) return true;

        const CFTypeRef depth = CFDictionaryGetValue(
            extensions, kCMFormatDescriptionExtension_Depth
        );
        if (depth != nullptr && CFGetTypeID(depth) == CFNumberGetTypeID()) {
            int depthValue = 0;
            if (CFNumberGetValue(
                    static_cast<CFNumberRef>(depth),
                    kCFNumberIntType,
                    &depthValue) && depthValue == 32) {
                return true;
            }
        }
    }
    return false;
}

[[nodiscard]] bool trackUsesStraightAlpha(AVAssetTrack* track) noexcept {
    for (id value in track.formatDescriptions) {
        const auto description = (__bridge CMFormatDescriptionRef)value;
        const CFDictionaryRef extensions =
            CMFormatDescriptionGetExtensions(description);
        if (extensions == nullptr) continue;
        const CFTypeRef mode = CFDictionaryGetValue(
            extensions, kCMFormatDescriptionExtension_AlphaChannelMode
        );
        if (mode != nullptr) {
            return CFEqual(
                mode,
                kCMFormatDescriptionAlphaChannelMode_StraightAlpha
            );
        }
    }
    return false;
}

[[nodiscard]] double normalizedTime(
    const Decoder& decoder,
    double timeSeconds
) noexcept {
    if (decoder.durationSeconds <= 0.0) return timeSeconds;
    double result = std::fmod(timeSeconds, decoder.durationSeconds);
    if (result < 0.0) result += decoder.durationSeconds;
    return result;
}

void ReaderState::reset(Decoder& decoder, double target) {
    reader = nil;
    output = nil;
    current.reset();
    next.reset();
    reachedEnd = false;
    lastTarget = -1.0;
    ++epoch;

    NSError* error = nil;
    reader = [[AVAssetReader alloc] initWithAsset:decoder.asset error:&error];
    if (reader == nil) {
        const char* reason = error == nil
            ? nullptr : error.localizedDescription.UTF8String;
        throw std::runtime_error(
            reason == nullptr ? "Unable to create AVAssetReader" : reason
        );
    }
    const double seekStartSeconds = std::max(
        0.0, target - seekLookbehindSeconds
    );
    const CMTime seekStart = CMTimeMakeWithSeconds(seekStartSeconds, 600);
    const CMTime assetDuration = decoder.asset.duration;
    if (CMTIME_IS_NUMERIC(assetDuration) &&
        CMTimeCompare(assetDuration, seekStart) > 0) {
        reader.timeRange = CMTimeRangeMake(
            seekStart, CMTimeSubtract(assetDuration, seekStart)
        );
    }
    NSDictionary* settings = @{
        (id)kCVPixelBufferPixelFormatTypeKey:
            @(kCVPixelFormatType_32BGRA),
        (id)kCVPixelBufferIOSurfacePropertiesKey: @{},
    };
    output = [[AVAssetReaderVideoCompositionOutput alloc]
        initWithVideoTracks:@[decoder.videoTrack]
        videoSettings:settings
    ];
    output.alwaysCopiesSampleData = NO;
    output.videoComposition = decoder.videoComposition;
    if (![reader canAddOutput:output]) {
        throw std::runtime_error(
            "AVAssetReader rejected the video composition output"
        );
    }
    [reader addOutput:output];
    if (![reader startReading]) {
        const char* reason = reader.error.localizedDescription.UTF8String;
        throw std::runtime_error(
            reason == nullptr ? "AVAssetReader failed to start" : reason
        );
    }
}

std::shared_ptr<DecodedFrame> ReaderState::readNext(Decoder& decoder) {
    if (reachedEnd) return nullptr;
    CMSampleBufferRef sample = [output copyNextSampleBuffer];
    if (sample == nullptr) {
        if (reader.status == AVAssetReaderStatusFailed) {
            const char* reason = reader.error.localizedDescription.UTF8String;
            throw std::runtime_error(
                reason == nullptr ? "AVAssetReader failed while decoding" : reason
            );
        }
        reachedEnd = true;
        return nullptr;
    }
    try {
        CVPixelBufferRef buffer = static_cast<CVPixelBufferRef>(
            CMSampleBufferGetImageBuffer(sample)
        );
        const CMTime presentationTime =
            CMSampleBufferGetPresentationTimeStamp(sample);
        if (buffer == nullptr || !CMTIME_IS_NUMERIC(presentationTime)) {
            throw std::runtime_error(
                "AVFoundation returned a video sample without pixels or time"
            );
        }
        auto result = std::make_shared<DecodedFrame>();
        result->pixels = makePixelStorage(buffer, decoder);
        result->presentationTime = presentationTime;
        result->readerEpoch = epoch;
        CFRelease(sample);
        return result;
    } catch (...) {
        CFRelease(sample);
        throw;
    }
}

std::shared_ptr<DecodedFrame> ReaderState::frameAt(
    Decoder& decoder,
    double target
) {
    if (reader == nil || target + timeComparisonTolerance < lastTarget ||
        target - lastTarget > maximumSequentialDecodeJump) {
        reset(decoder, target);
    }
    lastTarget = target;
    if (!next && !reachedEnd) next = readNext(decoder);

    while (next) {
        const double nextTime = CMTimeGetSeconds(next->presentationTime);
        if (current && nextTime > target + timeComparisonTolerance) break;
        current = std::move(next);
        next = readNext(decoder);
    }
    if (!current) {
        throw std::runtime_error("Video decoder produced no sample for the requested time");
    }
    return current;
}

[[nodiscard]] bool sameDecodedFrame(
    const DecodedFrame& lhs,
    const DecodedFrame& rhs
) noexcept {
    return lhs.readerEpoch == rhs.readerEpoch &&
        CMTimeCompare(lhs.presentationTime, rhs.presentationTime) == 0;
}

void publishDecodedFrame(
    Decoder& decoder,
    std::shared_ptr<DecodedFrame> frame
) {
    if (!decoder.published ||
        !sameDecodedFrame(*decoder.published, *frame)) {
        decoder.published = std::move(frame);
        ++decoder.publishedSerial;
    }
}

void decoderWorker(Decoder* decoder) noexcept {
    for (;;) {
        double target = 0.0;
        std::uint64_t generation = 0;
        {
            std::unique_lock lock(decoder->mutex);
            decoder->condition.wait(lock, [&] {
                return decoder->stopping || decoder->requestPending;
            });
            if (decoder->stopping) return;
            target = decoder->requestedTime;
            generation = decoder->requestGeneration;
            decoder->requestPending = false;
        }

        std::shared_ptr<DecodedFrame> decoded;
        std::optional<std::string> failure;
        @autoreleasepool {
            try {
                decoded = decoder->reader.frameAt(*decoder, target);
            } catch (const std::exception& error) {
                failure = error.what();
            } catch (...) {
                failure = "unknown AVFoundation decoder failure";
            }
        }

        std::lock_guard guard(decoder->mutex);
        if (decoder->stopping) return;
        if (generation != decoder->requestGeneration) {
            continue;
        }
        decoder->completedGeneration = generation;
        decoder->failure = std::move(failure);
        if (decoder->failure) {
            std::fprintf(
                stderr,
                "Scene video decoder failed for '%s': %s\n",
                decoder->source.c_str(),
                decoder->failure->c_str()
            );
        } else {
            publishDecodedFrame(*decoder, std::move(decoded));
        }
    }
}

}  // namespace

void* createVideoDecoder(
    const std::uint8_t* bytes,
    std::size_t byteCount,
    const char* source
) {
    if (bytes == nullptr || byteCount == 0) return nullptr;
    @autoreleasepool {
        // Logical asset names may contain directory separators; only the UUID
        // participates in the temporary path.
        NSString* filename = [NSString stringWithFormat:
            @"we-video-%@.mp4", [[NSUUID UUID] UUIDString]
        ];
        NSString* path = [NSTemporaryDirectory()
            stringByAppendingPathComponent:filename];
        NSData* data = [NSData dataWithBytes:bytes length:byteCount];
        if (![data writeToFile:path options:NSDataWritingAtomic error:nil]) {
            return nullptr;
        }
        NSURL* url = [NSURL fileURLWithPath:path];
        AVAsset* asset = [AVAsset assetWithURL:url];
        AVAssetTrack* track = [[asset tracksWithMediaType:AVMediaTypeVideo]
            firstObject];
        if (track == nil) {
            [[NSFileManager defaultManager] removeItemAtURL:url error:nil];
            return nullptr;
        }

        auto decoder = std::make_unique<Decoder>();
        try {
            decoder->url = url;
            decoder->asset = asset;
            decoder->videoTrack = track;
            decoder->videoComposition =
                [AVVideoComposition
                    videoCompositionWithPropertiesOfAsset:asset];
            if (decoder->videoComposition == nil) {
                throw std::runtime_error(
                    "AVFoundation could not create the video composition"
                );
            }
            decoder->durationSeconds = CMTimeGetSeconds(asset.duration);
            decoder->containsAlpha = trackContainsAlpha(track);
            // Alpha-bearing video defaults to premultiplied unless its format
            // description explicitly declares straight alpha.
            decoder->premultipliedAlpha = decoder->containsAlpha &&
                !trackUsesStraightAlpha(track);
            decoder->source = source == nullptr ? "<unknown>" : source;
            std::shared_ptr<DecodedFrame> initial =
                decoder->reader.frameAt(*decoder, 0.0);
            publishDecodedFrame(*decoder, std::move(initial));
            decoder->worker = std::thread(decoderWorker, decoder.get());
            return decoder.release();
        } catch (const std::exception& error) {
            std::fprintf(
                stderr,
                "Unable to initialize scene video decoder for '%s': %s\n",
                decoder->source.empty() ? "<unknown>" : decoder->source.c_str(),
                error.what()
            );
            return nullptr;
        }
    }
}

void destroyVideoDecoder(void* value) noexcept {
    delete static_cast<Decoder*>(value);
}

bool requestVideoFrame(void* value, double timeSeconds) noexcept {
    if (value == nullptr || !std::isfinite(timeSeconds) || timeSeconds < 0.0) {
        return false;
    }
    Decoder* decoder = static_cast<Decoder*>(value);
    const double target = normalizedTime(*decoder, timeSeconds);
    {
        std::lock_guard guard(decoder->mutex);
        if (decoder->stopping) return false;
        if (decoder->failure &&
            decoder->completedGeneration == decoder->requestGeneration) {
            // Surface every asynchronous failure once before permitting a
            // later frame to retry. A continuously advancing request stream
            // must not hide decode failures behind the last published frame.
            decoder->failure.reset();
            return false;
        }
        if (std::abs(target - decoder->requestedTime) <=
                timeComparisonTolerance) {
            return true;
        }
        decoder->requestedTime = target;
        ++decoder->requestGeneration;
        decoder->requestPending = true;
    }
    decoder->condition.notify_one();
    return true;
}

bool copyLatestVideoFrame(void* value, VideoFrame& output) noexcept {
    output = {};
    if (value == nullptr) return false;
    Decoder* decoder = static_cast<Decoder*>(value);
    std::lock_guard guard(decoder->mutex);
    if (decoder->failure &&
        decoder->completedGeneration == decoder->requestGeneration) {
        return false;
    }
    if (!decoder->published || !decoder->published->pixels ||
        decoder->publishedSerial == 0) {
        return false;
    }
    try {
        const std::shared_ptr<PixelStorage>& pixels =
            decoder->published->pixels;
        std::shared_ptr<PixelView> view = makePixelView(pixels);
        output.storage = view;
        output.bytes = view->bytes;
        output.byteCount = pixels->byteCount;
        output.bytesPerRow = pixels->bytesPerRow;
        output.width = pixels->width;
        output.height = pixels->height;
        output.serial = decoder->publishedSerial;
        output.pixelFormat = pixels->pixelFormat;
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace we::scene::gl
