#import <AVFoundation/AVFoundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>

#include "SceneVideoDecoder.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace we::scene::gl {
namespace {

struct Decoder final {
    NSURL* url = nil;
    AVAsset* asset = nil;
    AVAssetImageGenerator* generator = nil;
    std::vector<std::uint8_t> pixels;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::mutex mutex;

    ~Decoder() {
        generator = nil;
        asset = nil;
        if (url != nil) {
            [[NSFileManager defaultManager] removeItemAtURL:url error:nil];
            url = nil;
        }
    }
};

bool copyCGImage(CGImageRef image, Decoder& decoder) {
    const std::size_t width = CGImageGetWidth(image);
    const std::size_t height = CGImageGetHeight(image);
    if (width == 0 || height == 0 || width > UINT32_MAX || height > UINT32_MAX) {
        return false;
    }
    decoder.pixels.assign(width * height * 4, 0);
    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    if (colorSpace == nullptr) return false;
    CGContextRef context = CGBitmapContextCreate(
        decoder.pixels.data(), width, height, 8, width * 4, colorSpace,
        kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big
    );
    CGColorSpaceRelease(colorSpace);
    if (context == nullptr) return false;
    CGContextSetBlendMode(context, kCGBlendModeCopy);
    CGContextDrawImage(context, CGRectMake(0, 0, width, height), image);
    CGContextRelease(context);

    // Convert premultiplied RGBA to the straight-alpha bytes expected by the
    // OpenGL upload path. Keep CoreGraphics' top-down rows unchanged so video,
    // TEX and embedded images share the same scene orientation contract.
    for (std::size_t offset = 0; offset < decoder.pixels.size(); offset += 4) {
        const std::uint32_t alpha = decoder.pixels[offset + 3];
        if (alpha == 0) {
            decoder.pixels[offset] = 0;
            decoder.pixels[offset + 1] = 0;
            decoder.pixels[offset + 2] = 0;
        } else if (alpha != 255) {
            for (std::size_t component = 0; component < 3; ++component) {
                const std::uint32_t value = decoder.pixels[offset + component];
                decoder.pixels[offset + component] = static_cast<std::uint8_t>(
                    std::min<std::uint32_t>(255, (value * 255 + alpha / 2) / alpha)
                );
            }
        }
    }
    decoder.width = static_cast<std::uint32_t>(width);
    decoder.height = static_cast<std::uint32_t>(height);
    return true;
}

}  // namespace

void* createVideoDecoder(
    const std::uint8_t* bytes,
    std::size_t byteCount,
    const char* source
) {
    if (bytes == nullptr || byteCount == 0) return nullptr;
    @autoreleasepool {
        // The model source is a logical asset path and may contain directory
        // separators. It must never participate in the temporary filesystem
        // path: doing so made ordinary assets such as materials/video/foo.mp4
        // fail because the corresponding temporary parent directories did not
        // exist. The caller still owns the source string for diagnostics.
        static_cast<void>(source);
        NSString* filename = [NSString stringWithFormat:
            @"we-video-%@.mp4", [[NSUUID UUID] UUIDString]
        ];
        NSString* path = [NSTemporaryDirectory() stringByAppendingPathComponent:filename];
        NSData* data = [NSData dataWithBytes:bytes length:byteCount];
        if (![data writeToFile:path options:NSDataWritingAtomic error:nil]) return nullptr;
        NSURL* url = [NSURL fileURLWithPath:path];
        AVAsset* asset = [AVAsset assetWithURL:url];
        if (asset == nil || asset.tracks.count == 0) {
            [[NSFileManager defaultManager] removeItemAtURL:url error:nil];
            return nullptr;
        }
        Decoder* decoder = new Decoder();
        decoder->url = url;
        decoder->asset = asset;
        decoder->generator = [[AVAssetImageGenerator alloc] initWithAsset:asset];
        decoder->generator.appliesPreferredTrackTransform = YES;
        decoder->generator.requestedTimeToleranceBefore = kCMTimeZero;
        decoder->generator.requestedTimeToleranceAfter = kCMTimeZero;
        return decoder;
    }
}

void destroyVideoDecoder(void* value) noexcept {
    delete static_cast<Decoder*>(value);
}

bool decodeVideoFrame(
    void* value,
    double timeSeconds,
    VideoFrameRGBA8& output
) noexcept {
    if (value == nullptr || !std::isfinite(timeSeconds) || timeSeconds < 0) return false;
    Decoder* decoder = static_cast<Decoder*>(value);
    std::lock_guard guard(decoder->mutex);
    @autoreleasepool {
        CMTime duration = decoder->asset.duration;
        double durationSeconds = CMTimeGetSeconds(duration);
        double target = timeSeconds;
        if (std::isfinite(durationSeconds) && durationSeconds > 0) {
            target = std::fmod(timeSeconds, durationSeconds);
            if (target < 0) target += durationSeconds;
        }
        CMTime requested = CMTimeMakeWithSeconds(target, 600);
        NSError* error = nil;
        CGImageRef image = [decoder->generator copyCGImageAtTime:requested actualTime:nil error:&error];
        if (image == nullptr) return false;
        const bool copied = copyCGImage(image, *decoder);
        CGImageRelease(image);
        if (!copied) return false;
        output.bytes = decoder->pixels.data();
        output.byteCount = decoder->pixels.size();
        output.width = decoder->width;
        output.height = decoder->height;
        return true;
    }
}

}  // namespace we::scene::gl
