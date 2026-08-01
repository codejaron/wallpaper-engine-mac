import CoreGraphics
import Foundation
import ImageIO

struct SceneMediaArtwork: Equatable, Sendable {
    let thumbnail: SceneMediaThumbnailRGBA8
    let primaryColor: SceneMediaColor
    let secondaryColor: SceneMediaColor
    let tertiaryColor: SceneMediaColor
    let textColor: SceneMediaColor
    let highContrastColor: SceneMediaColor
}

enum SceneMediaArtworkProcessingError: LocalizedError, Equatable {
    case invalidImage
    case invalidDimensions
    case drawingFailed
    case noVisiblePixels

    var errorDescription: String? {
        switch self {
        case .invalidImage:
            return "Now Playing artwork is not a supported image"
        case .invalidDimensions:
            return "Now Playing artwork has invalid dimensions"
        case .drawingFailed:
            return "Rendering Now Playing artwork as RGBA8 failed"
        case .noVisiblePixels:
            return "Now Playing artwork contains no visible pixels"
        }
    }
}

enum SceneMediaArtworkProcessor {
    private struct ColorBucket {
        var count = 0
        var red = 0
        var green = 0
        var blue = 0

        var color: SceneMediaColor {
            let divisor = Double(max(count, 1) * 255)
            return SceneMediaColor(
                red: Double(red) / divisor,
                green: Double(green) / divisor,
                blue: Double(blue) / divisor
            )
        }
    }

    static func process(_ encodedImage: Data) throws -> SceneMediaArtwork {
        guard let source = CGImageSourceCreateWithData(
            encodedImage as CFData,
            nil
        ) else {
            throw SceneMediaArtworkProcessingError.invalidImage
        }
        let options: [CFString: Any] = [
            kCGImageSourceCreateThumbnailFromImageAlways: true,
            kCGImageSourceCreateThumbnailWithTransform: true,
            kCGImageSourceThumbnailMaxPixelSize: 1024,
            kCGImageSourceShouldCacheImmediately: true,
        ]
        guard let image = CGImageSourceCreateThumbnailAtIndex(
            source,
            0,
            options as CFDictionary
        ) else {
            throw SceneMediaArtworkProcessingError.invalidImage
        }
        let width = image.width
        let height = image.height
        guard width > 0,
              height > 0,
              width <= Int(UInt32.max / 4),
              height <= Int(UInt32.max),
              width <= Int.max / height / 4 else {
            throw SceneMediaArtworkProcessingError.invalidDimensions
        }

        var pixels = Data(count: width * height * 4)
        let rendered = pixels.withUnsafeMutableBytes { bytes -> Bool in
            guard let baseAddress = bytes.baseAddress,
                  let context = CGContext(
                    data: baseAddress,
                    width: width,
                    height: height,
                    bitsPerComponent: 8,
                    bytesPerRow: width * 4,
                    space: CGColorSpaceCreateDeviceRGB(),
                    bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue |
                        CGBitmapInfo.byteOrder32Big.rawValue
                  ) else {
                return false
            }
            context.interpolationQuality = .high
            context.draw(
                image,
                in: CGRect(x: 0, y: 0, width: width, height: height)
            )
            return true
        }
        guard rendered else {
            throw SceneMediaArtworkProcessingError.drawingFailed
        }

        let thumbnail = SceneMediaThumbnailRGBA8(
            width: UInt32(width),
            height: UInt32(height),
            bytesPerRow: UInt32(width * 4),
            pixels: pixels
        )
        let palette = try palette(for: thumbnail)
        return SceneMediaArtwork(
            thumbnail: thumbnail,
            primaryColor: palette[0],
            secondaryColor: palette[1],
            tertiaryColor: palette[2],
            textColor: contrastingColor(for: palette[0]),
            highContrastColor: contrastingColor(for: palette[0])
        )
    }

    static func palette(
        for thumbnail: SceneMediaThumbnailRGBA8
    ) throws -> [SceneMediaColor] {
        let width = UInt64(thumbnail.width)
        let height = UInt64(thumbnail.height)
        let totalPixelsResult = width.multipliedReportingOverflow(by: height)
        let storageLengthResult = UInt64(thumbnail.bytesPerRow)
            .multipliedReportingOverflow(by: height)
        guard width > 0,
              height > 0,
              !totalPixelsResult.overflow,
              totalPixelsResult.partialValue <= UInt64(Int.max),
              !storageLengthResult.overflow,
              UInt64(thumbnail.bytesPerRow) >= width * 4,
              UInt64(thumbnail.pixels.count) == storageLengthResult.partialValue else {
            throw SceneMediaArtworkProcessingError.invalidDimensions
        }
        let totalPixels = Int(totalPixelsResult.partialValue)

        let sampleStride = max(1, totalPixels / 65_536)
        var buckets: [Int: ColorBucket] = [:]
        thumbnail.pixels.withUnsafeBytes { bytes in
            let pixels = bytes.bindMemory(to: UInt8.self)
            for pixelIndex in stride(
                from: 0,
                to: totalPixels,
                by: sampleStride
            ) {
                let row = pixelIndex / Int(thumbnail.width)
                let column = pixelIndex % Int(thumbnail.width)
                let offset = row * Int(thumbnail.bytesPerRow) + column * 4
                let alpha = Int(pixels[offset + 3])
                guard alpha >= 32 else { continue }
                let red = min(255, Int(pixels[offset]) * 255 / alpha)
                let green = min(255, Int(pixels[offset + 1]) * 255 / alpha)
                let blue = min(255, Int(pixels[offset + 2]) * 255 / alpha)
                let key = (red >> 4) << 8 | (green >> 4) << 4 | (blue >> 4)
                var bucket = buckets[key, default: ColorBucket()]
                bucket.count += 1
                bucket.red += red
                bucket.green += green
                bucket.blue += blue
                buckets[key] = bucket
            }
        }
        guard !buckets.isEmpty else {
            throw SceneMediaArtworkProcessingError.noVisiblePixels
        }

        let ranked = buckets.map { key, bucket in
            let color = bucket.color
            let maximum = max(color.red, color.green, color.blue)
            let minimum = min(color.red, color.green, color.blue)
            let saturation = maximum > 0 ? (maximum - minimum) / maximum : 0
            let score = Double(bucket.count) * (0.35 + saturation * 0.65)
            return (key: key, score: score, color: color)
        }
            .sorted {
                if $0.score != $1.score { return $0.score > $1.score }
                return $0.key < $1.key
            }

        var selected: [SceneMediaColor] = []
        for candidate in ranked {
            if selected.allSatisfy({ colorDistanceSquared($0, candidate.color) >= 0.04 }) {
                selected.append(candidate.color)
            }
            if selected.count == 3 { break }
        }
        while selected.count < 3 {
            selected.append(selected.last ?? ranked[0].color)
        }
        return selected
    }

    private static func colorDistanceSquared(
        _ left: SceneMediaColor,
        _ right: SceneMediaColor
    ) -> Double {
        let red = left.red - right.red
        let green = left.green - right.green
        let blue = left.blue - right.blue
        return red * red + green * green + blue * blue
    }

    private static func contrastingColor(
        for color: SceneMediaColor
    ) -> SceneMediaColor {
        let luminance = 0.2126 * color.red +
            0.7152 * color.green +
            0.0722 * color.blue
        return luminance > 0.45
            ? .black
            : SceneMediaColor(red: 1, green: 1, blue: 1)
    }
}
