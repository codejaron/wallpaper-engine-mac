import AppKit
import Foundation
import ImageIO
import Network

protocol RemoteImageDataLoading: Sendable {
    func data(from url: URL) async throws -> Data
}

enum RemoteImageLoadingError: LocalizedError, Equatable {
    case missingURL
    case insecureURL
    case unsafeHost
    case invalidResponse
    case httpStatus(Int)
    case unsupportedContentType(String?)
    case unsupportedImageFormat(String?)
    case emptyData
    case downloadTooLarge(maximumBytes: Int)
    case invalidImageData
    case imageTooLarge(maximumDimension: Int, maximumPixels: Int)
    case tooManyFrames(maximumFrames: Int)
    case animationTooLarge(maximumTotalPixels: Int)

    var errorDescription: String? {
        switch self {
        case .missingURL:
            return "This image does not provide a URL."
        case .insecureURL:
            return "Only HTTPS remote images are allowed."
        case .unsafeHost:
            return "Local and private network image URLs are not allowed."
        case .invalidResponse:
            return "The image server returned an invalid response."
        case .httpStatus(let statusCode):
            return "The image server returned HTTP \(statusCode)."
        case .unsupportedContentType(let contentType):
            return "The server returned unsupported content type: \(contentType ?? "missing")."
        case .unsupportedImageFormat(let format):
            return "The response uses an unsupported image format: \(format ?? "unknown")."
        case .emptyData:
            return "The image response was empty."
        case .downloadTooLarge(let maximumBytes):
            return "The image exceeds the \(ByteCountFormatter.string(fromByteCount: Int64(maximumBytes), countStyle: .file)) download limit."
        case .invalidImageData:
            return "The response is not a supported image."
        case .imageTooLarge(let maximumDimension, let maximumPixels):
            return "The image exceeds the \(maximumDimension)-pixel dimension or \(maximumPixels)-pixel area limit."
        case .tooManyFrames(let maximumFrames):
            return "The animation exceeds the \(maximumFrames)-frame limit."
        case .animationTooLarge(let maximumTotalPixels):
            return "The animation exceeds the \(maximumTotalPixels)-decoded-pixel limit."
        }
    }
}

struct RemoteImagePolicy: Sendable {
    static let `default` = RemoteImagePolicy()

    private static let supportedImageTypes: Set<String> = [
        "public.jpeg",
        "public.png",
        "com.compuserve.gif",
        "public.tiff",
        "com.microsoft.bmp",
        "org.webmproject.webp",
        "public.heic",
        "public.heif",
        "public.avif",
        "public.jpeg-2000",
    ]

    let maximumDownloadBytes: Int
    let maximumDimension: Int
    let maximumPixels: Int
    let maximumFrames: Int
    let maximumTotalPixels: Int

    init(
        maximumDownloadBytes: Int = 25 * 1_024 * 1_024,
        maximumDimension: Int = 8_192,
        maximumPixels: Int = 64_000_000,
        maximumFrames: Int = 600,
        maximumTotalPixels: Int = 300_000_000
    ) {
        self.maximumDownloadBytes = maximumDownloadBytes
        self.maximumDimension = maximumDimension
        self.maximumPixels = maximumPixels
        self.maximumFrames = maximumFrames
        self.maximumTotalPixels = maximumTotalPixels
    }

    func validate(url: URL) throws {
        guard url.scheme?.lowercased() == "https", let host = url.host else {
            throw RemoteImageLoadingError.insecureURL
        }
        guard url.user == nil,
              url.password == nil,
              isPotentiallyPublic(host: host) else {
            throw RemoteImageLoadingError.unsafeHost
        }
    }

    func validate(response: URLResponse) throws {
        guard let response = response as? HTTPURLResponse else {
            throw RemoteImageLoadingError.invalidResponse
        }
        guard (200..<300).contains(response.statusCode) else {
            throw RemoteImageLoadingError.httpStatus(response.statusCode)
        }
        guard response.mimeType?.lowercased().hasPrefix("image/") == true else {
            throw RemoteImageLoadingError.unsupportedContentType(response.mimeType)
        }
        if response.expectedContentLength > Int64(maximumDownloadBytes) {
            throw RemoteImageLoadingError.downloadTooLarge(
                maximumBytes: maximumDownloadBytes
            )
        }
    }

    func decode(_ data: Data) throws -> RemoteImageDecodingResult {
        guard !data.isEmpty else { throw RemoteImageLoadingError.emptyData }
        guard data.count <= maximumDownloadBytes else {
            throw RemoteImageLoadingError.downloadTooLarge(
                maximumBytes: maximumDownloadBytes
            )
        }
        guard let source = CGImageSourceCreateWithData(data as CFData, nil) else {
            throw RemoteImageLoadingError.invalidImageData
        }
        let imageType = CGImageSourceGetType(source) as String?
        guard let imageType, Self.supportedImageTypes.contains(imageType) else {
            throw RemoteImageLoadingError.unsupportedImageFormat(imageType)
        }

        let frameCount = CGImageSourceGetCount(source)
        guard frameCount > 0 else { throw RemoteImageLoadingError.invalidImageData }
        guard frameCount <= maximumFrames else {
            throw RemoteImageLoadingError.tooManyFrames(maximumFrames: maximumFrames)
        }

        var totalPixels = 0
        for frameIndex in 0..<frameCount {
            guard let properties = CGImageSourceCopyPropertiesAtIndex(
                source,
                frameIndex,
                nil
            ) as? [CFString: Any],
            let width = (properties[kCGImagePropertyPixelWidth] as? NSNumber)?.intValue,
            let height = (properties[kCGImagePropertyPixelHeight] as? NSNumber)?.intValue,
            width > 0,
            height > 0 else {
                throw RemoteImageLoadingError.invalidImageData
            }

            let (pixels, overflowed) = width.multipliedReportingOverflow(by: height)
            guard !overflowed,
                  width <= maximumDimension,
                  height <= maximumDimension,
                  pixels <= maximumPixels else {
                throw RemoteImageLoadingError.imageTooLarge(
                    maximumDimension: maximumDimension,
                    maximumPixels: maximumPixels
                )
            }
            let (nextTotal, totalOverflowed) = totalPixels.addingReportingOverflow(pixels)
            guard !totalOverflowed, nextTotal <= maximumTotalPixels else {
                throw RemoteImageLoadingError.animationTooLarge(
                    maximumTotalPixels: maximumTotalPixels
                )
            }
            totalPixels = nextTotal
        }

        guard let image = NSImage(data: data), image.isValid else {
            throw RemoteImageLoadingError.invalidImageData
        }
        return RemoteImageDecodingResult(
            image: image,
            cacheCost: totalPixels * 4
        )
    }

    private func isPotentiallyPublic(host source: String) -> Bool {
        let host = source.lowercased().trimmingCharacters(
            in: CharacterSet(charactersIn: ".")
        )
        guard !host.isEmpty else { return false }

        let reservedNames = ["localhost", "local", "lan", "home.arpa", "internal"]
        guard !reservedNames.contains(where: {
            host == $0 || host.hasSuffix(".\($0)")
        }) else {
            return false
        }

        if let address = IPv4Address(host) {
            return isPublicIPv4(Array(address.rawValue))
        }
        if let address = IPv6Address(host) {
            return isPublicIPv6(Array(address.rawValue))
        }

        guard host.contains("."), !host.contains(":") else { return false }
        return host.unicodeScalars.contains(where: CharacterSet.letters.contains)
    }

    private func isPublicIPv4(_ bytes: [UInt8]) -> Bool {
        guard bytes.count == 4 else { return false }
        let first = bytes[0]
        let second = bytes[1]

        switch (first, second) {
        case (0, _), (10, _), (127, _), (169, 254), (192, 168):
            return false
        case (100, 64...127), (172, 16...31), (198, 18...19):
            return false
        case (224...255, _):
            return false
        default:
            return true
        }
    }

    private func isPublicIPv6(_ bytes: [UInt8]) -> Bool {
        guard bytes.count == 16 else { return false }
        if bytes.allSatisfy({ $0 == 0 }) { return false }
        if bytes.dropLast().allSatisfy({ $0 == 0 }), bytes.last == 1 { return false }

        let isIPv4Mapped = bytes.prefix(10).allSatisfy({ $0 == 0 })
            && bytes[10] == 0xff
            && bytes[11] == 0xff
        let isIPv4Compatible = bytes.prefix(12).allSatisfy({ $0 == 0 })
        if isIPv4Mapped || isIPv4Compatible {
            return isPublicIPv4(Array(bytes.suffix(4)))
        }

        if bytes[0] & 0xfe == 0xfc { return false }
        if bytes[0] == 0xfe, bytes[1] & 0xc0 != 0 { return false }
        if bytes[0] == 0xff { return false }
        if bytes[0] == 0x20, bytes[1] == 0x02 {
            return isPublicIPv4(Array(bytes[2...5]))
        }
        return true
    }
}

struct RemoteImageDecodingResult {
    let image: NSImage
    let cacheCost: Int
}

private final class RejectRemoteImageRedirects: NSObject, URLSessionTaskDelegate {
    func urlSession(
        _ session: URLSession,
        task: URLSessionTask,
        willPerformHTTPRedirection response: HTTPURLResponse,
        newRequest request: URLRequest,
        completionHandler: @escaping (URLRequest?) -> Void
    ) {
        completionHandler(nil)
    }
}

struct RestrictedRemoteImageDataLoader: RemoteImageDataLoading {
    static let shared = RestrictedRemoteImageDataLoader()

    private static let session: URLSession = {
        let configuration = URLSessionConfiguration.ephemeral
        configuration.httpCookieStorage = nil
        configuration.urlCredentialStorage = nil
        configuration.httpShouldSetCookies = false
        configuration.urlCache = nil
        configuration.requestCachePolicy = .reloadIgnoringLocalCacheData
        configuration.timeoutIntervalForRequest = 20
        configuration.timeoutIntervalForResource = 30
        return URLSession(
            configuration: configuration,
            delegate: RejectRemoteImageRedirects(),
            delegateQueue: nil
        )
    }()

    let policy: RemoteImagePolicy

    init(policy: RemoteImagePolicy = .default) {
        self.policy = policy
    }

    func data(from url: URL) async throws -> Data {
        try policy.validate(url: url)

        var request = URLRequest(url: url)
        request.setValue("image/avif,image/webp,image/apng,image/*", forHTTPHeaderField: "Accept")
        request.setValue(nil, forHTTPHeaderField: "Cookie")
        request.setValue(nil, forHTTPHeaderField: "Referer")

        let (bytes, response) = try await Self.session.bytes(for: request)
        try policy.validate(response: response)

        var data = Data()
        if response.expectedContentLength > 0 {
            data.reserveCapacity(min(Int(response.expectedContentLength), policy.maximumDownloadBytes))
        }
        for try await byte in bytes {
            guard data.count < policy.maximumDownloadBytes else {
                throw RemoteImageLoadingError.downloadTooLarge(
                    maximumBytes: policy.maximumDownloadBytes
                )
            }
            data.append(byte)
        }
        guard !data.isEmpty else { throw RemoteImageLoadingError.emptyData }
        return data
    }
}

final class RemoteImageCache {
    static let shared = RemoteImageCache()

    private let images = NSCache<NSURL, NSImage>()

    init(countLimit: Int = 200, totalCostLimit: Int = 256 * 1_024 * 1_024) {
        images.countLimit = countLimit
        images.totalCostLimit = totalCostLimit
    }

    func image(for url: URL) -> NSImage? {
        images.object(forKey: url as NSURL)
    }

    func insert(_ image: NSImage, for url: URL, cost: Int) {
        images.setObject(image, forKey: url as NSURL, cost: cost)
    }

    func removeImage(for url: URL) {
        images.removeObject(forKey: url as NSURL)
    }
}

@MainActor
final class RemoteImageLoader: ObservableObject {
    enum State {
        case idle
        case loading
        case loaded(NSImage)
        case failed(String)
    }

    @Published private(set) var state: State = .idle

    private let dataLoader: any RemoteImageDataLoading
    private let cache: RemoteImageCache
    private let policy: RemoteImagePolicy
    private var currentURL: URL?

    init(
        dataLoader: any RemoteImageDataLoading = RestrictedRemoteImageDataLoader.shared,
        cache: RemoteImageCache = .shared,
        policy: RemoteImagePolicy = .default
    ) {
        self.dataLoader = dataLoader
        self.cache = cache
        self.policy = policy
    }

    var image: NSImage? {
        guard case .loaded(let image) = state else { return nil }
        return image
    }

    var errorMessage: String? {
        guard case .failed(let message) = state else { return nil }
        return message
    }

    func load(url: URL?, ignoringCache: Bool = false) async {
        currentURL = url

        guard let url else {
            state = .failed(RemoteImageLoadingError.missingURL.localizedDescription)
            return
        }

        do {
            try policy.validate(url: url)
            if ignoringCache {
                cache.removeImage(for: url)
            } else if let cachedImage = cache.image(for: url) {
                state = .loaded(cachedImage)
                return
            }

            state = .loading
            let data = try await dataLoader.data(from: url)
            try Task.checkCancellation()
            guard currentURL == url else { return }
            let decoded = try policy.decode(data)
            cache.insert(decoded.image, for: url, cost: decoded.cacheCost)
            state = .loaded(decoded.image)
        } catch is CancellationError {
            return
        } catch {
            guard currentURL == url else { return }
            state = .failed(error.localizedDescription)
        }
    }

    func retry() async {
        await load(url: currentURL, ignoringCache: true)
    }
}
