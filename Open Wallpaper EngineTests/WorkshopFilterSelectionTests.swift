import AppKit
import ImageIO
import SwiftUI
import UniformTypeIdentifiers
import XCTest
@testable import Open_Wallpaper_Engine

final class DesktopWallpaperImageRendererTests: XCTestCase {
    func testRendersAtTheRequestedBackingPixelSize() throws {
        let source = try XCTUnwrap(
            bitmapRepresentation(width: 24, height: 16, color: .red).cgImage
        )

        let data = try DesktopWallpaperImageRenderer.pngData(
            from: source,
            pixelWidth: 300,
            pixelHeight: 180
        )
        let rendered = try XCTUnwrap(NSBitmapImageRep(data: data))

        XCTAssertEqual(rendered.pixelsWide, 300)
        XCTAssertEqual(rendered.pixelsHigh, 180)
    }

    func testAspectFillCropsTheSourceAroundItsCenter() throws {
        let source = try stripedBitmapRepresentation()

        let data = try DesktopWallpaperImageRenderer.pngData(
            from: try XCTUnwrap(source.cgImage),
            pixelWidth: 100,
            pixelHeight: 100
        )
        let rendered = try XCTUnwrap(NSBitmapImageRep(data: data))
        let center = try XCTUnwrap(rendered.colorAt(x: 50, y: 50))

        XCTAssertGreaterThan(center.greenComponent, 0.8)
        XCTAssertLessThan(center.redComponent, 0.2)
        XCTAssertLessThan(center.blueComponent, 0.2)
    }
}

final class FilterResultsSystemImageTests: XCTestCase {
    func testShowOnlyOptionsNeverUseAnEmptySystemImageName() {
        let hasOnlyValidImageNames = FRShowOnly.allOptions.allSatisfy { option in
            let imageName: String? = option.1
            return imageName == nil || imageName?.isEmpty == false
        }

        XCTAssertTrue(hasOnlyValidImageNames)
    }
}

final class ScenePropertyContentParserTests: XCTestCase {
    func testTextPropertyUsesMarkupInsteadOfItsNonStringRuntimeValue() throws {
        let property = ScenePropertyDefinition(
            key: "display",
            text: "<center><a href='https://example.com/creator'>"
                + "<img src='https://images.example/banner.gif' width='105%' alt='Banner'>"
                + "</a></center><h4>Hello<br>World</h4>",
            kind: .text,
            index: 0,
            order: 0,
            minimum: nil,
            maximum: nil,
            step: nil,
            precision: nil,
            fraction: nil,
            isReadOnly: true,
            options: [],
            defaultValue: .boolean(false)
        )
        let content = try ScenePropertyContentParser().parse(property.text)

        XCTAssertEqual(content.blocks, [
            .image(ScenePropertyImageBlock(
                url: URL(string: "https://images.example/banner.gif")!,
                link: URL(string: "https://example.com/creator")!,
                altText: "Banner",
                widthFraction: 1
            )),
            .text(ScenePropertyTextBlock(
                runs: [ScenePropertyTextRun(
                    text: "Hello\nWorld",
                    style: [],
                    link: nil
                )],
                headingLevel: 4,
                alignment: .leading
            )),
        ])
    }

    func testDropsActiveMarkupAndDoesNotLoadNonHTTPSImages() throws {
        let content = try ScenePropertyContentParser().parse(
            "<script>leak()</script><iframe src='https://tracker.example'></iframe>"
                + "<b>Safe</b><img src='http://tracker.example/pixel.gif' alt='Blocked image'>"
        )

        XCTAssertEqual(content.plainText, "SafeBlocked image")
        XCTAssertFalse(content.blocks.contains { block in
            if case .image = block { return true }
            return false
        })
    }

    func testRejectsOversizedAndDeeplyNestedMarkup() {
        let sizeLimitedParser = ScenePropertyContentParser(maximumMarkupBytes: 4)
        XCTAssertThrowsError(try sizeLimitedParser.parse("12345")) { error in
            XCTAssertEqual(
                error as? ScenePropertyContentParsingError,
                .markupTooLarge(maximumBytes: 4)
            )
        }

        let depthLimitedParser = ScenePropertyContentParser(maximumNestingDepth: 2)
        XCTAssertThrowsError(
            try depthLimitedParser.parse("<div><div><div>Text</div></div></div>")
        ) { error in
            XCTAssertEqual(
                error as? ScenePropertyContentParsingError,
                .nestingTooDeep(maximumDepth: 2)
            )
        }
    }
}

final class RemoteImagePolicyTests: XCTestCase {
    func testRejectsNonHTTPSURLs() {
        let policy = RemoteImagePolicy.default

        XCTAssertThrowsError(try policy.validate(
            url: URL(string: "http://tracker.example/image.png")!
        )) { error in
            XCTAssertEqual(error as? RemoteImageLoadingError, .insecureURL)
        }
    }

    func testRejectsLocalAndPrivateNetworkURLs() {
        let policy = RemoteImagePolicy.default
        let blockedURLs = [
            "https://localhost/image.png",
            "https://preview.local/image.png",
            "https://127.0.0.1/image.png",
            "https://10.0.0.1/image.png",
            "https://192.168.1.1/image.png",
            "https://[::1]/image.png",
            "https://[fd00::1]/image.png",
        ]

        for source in blockedURLs {
            XCTAssertThrowsError(try policy.validate(url: XCTUnwrap(URL(string: source)))) {
                error in
                XCTAssertEqual(error as? RemoteImageLoadingError, .unsafeHost)
            }
        }
        XCTAssertNoThrow(try policy.validate(
            url: XCTUnwrap(URL(string: "https://images.example/image.gif"))
        ))
    }

    func testRejectsNonRasterImageFormats() throws {
        XCTAssertThrowsError(try RemoteImagePolicy.default.decode(pdfData())) { error in
            XCTAssertEqual(
                error as? RemoteImageLoadingError,
                .unsupportedImageFormat("com.adobe.pdf")
            )
        }
    }

    func testAcceptsGenericBinaryResponseWhenPayloadIsARealImage() throws {
        let response = try XCTUnwrap(HTTPURLResponse(
            url: URL(string: "https://images.steamusercontent.com/preview")!,
            statusCode: 200,
            httpVersion: nil,
            headerFields: ["Content-Type": "application/octet-stream"]
        ))

        XCTAssertNoThrow(try RemoteImagePolicy.default.validate(response: response))
        XCTAssertNoThrow(try RemoteImagePolicy.default.decode(
            pngData(width: 20, height: 10, color: .red)
        ))
    }

    func testGenericBinaryResponseStillRejectsNonImagePayload() throws {
        let response = try XCTUnwrap(HTTPURLResponse(
            url: URL(string: "https://images.steamusercontent.com/preview")!,
            statusCode: 200,
            httpVersion: nil,
            headerFields: ["Content-Type": "application/octet-stream"]
        ))

        XCTAssertNoThrow(try RemoteImagePolicy.default.validate(response: response))
        XCTAssertThrowsError(try RemoteImagePolicy.default.decode(Data("not an image".utf8))) {
            error in
            XCTAssertEqual(
                error as? RemoteImageLoadingError,
                .unsupportedImageFormat(nil)
            )
        }
    }

    func testRejectsUnrelatedContentTypeBeforeDownloadingPayload() throws {
        let response = try XCTUnwrap(HTTPURLResponse(
            url: URL(string: "https://images.steamusercontent.com/preview")!,
            statusCode: 200,
            httpVersion: nil,
            headerFields: ["Content-Type": "text/html"]
        ))

        XCTAssertThrowsError(try RemoteImagePolicy.default.validate(response: response)) {
            error in
            XCTAssertEqual(
                error as? RemoteImageLoadingError,
                .unsupportedContentType("text/html")
            )
        }
    }

    func testRejectsImagesBeyondPixelLimits() throws {
        let policy = RemoteImagePolicy(
            maximumDownloadBytes: 1_000_000,
            maximumDimension: 10,
            maximumPixels: 100,
            maximumFrames: 10,
            maximumTotalPixels: 1_000
        )

        XCTAssertThrowsError(try policy.decode(
            pngData(width: 20, height: 10, color: .red)
        )) { error in
            XCTAssertEqual(
                error as? RemoteImageLoadingError,
                .imageTooLarge(maximumDimension: 10, maximumPixels: 100)
            )
        }
    }

    func testCacheCostUsesDecodedPixelsInsteadOfCompressedBytes() throws {
        let decoded = try RemoteImagePolicy.default.decode(
            pngData(width: 20, height: 10, color: .red)
        )

        XCTAssertEqual(decoded.cacheCost, 20 * 10 * 4)
    }

    @MainActor
    func testLoaderRejectsInsecureURLBeforeCallingNetworkClient() async {
        let dataLoader = StubRemoteImageDataLoader(responses: [])
        let loader = RemoteImageLoader(
            dataLoader: dataLoader,
            cache: RemoteImageCache(countLimit: 1)
        )

        await loader.load(url: URL(string: "http://tracker.example/image.png")!)

        XCTAssertEqual(loader.errorMessage, RemoteImageLoadingError.insecureURL.localizedDescription)
        let requestCount = await dataLoader.requestCount
        XCTAssertEqual(requestCount, 0)
    }
}

@MainActor
final class GifImageLayoutTests: XCTestCase {
    func testResizableImageUsesTheSwiftUIAspectRatioConstraint() throws {
        let imageURL = try makePNG(width: 200, height: 100, color: .red)
        defer { try? FileManager.default.removeItem(at: imageURL) }

        let image = GifImage(contentsOf: imageURL, animates: false)
            .resizable()
            .aspectRatio(1, contentMode: .fit)
            .frame(width: 200)
        let hostingView = NSHostingView(rootView: image)

        hostingView.layoutSubtreeIfNeeded()

        XCTAssertEqual(hostingView.fittingSize.width, 200, accuracy: 0.5)
        XCTAssertEqual(hostingView.fittingSize.height, 200, accuracy: 0.5)
    }

    func testUnrelatedUpdatesDoNotReloadAnUnchangedImageSource() throws {
        let imageURL = try makePNG(width: 20, height: 20, color: .red)
        defer { try? FileManager.default.removeItem(at: imageURL) }

        let model = GifImageUpdateModel()
        let hostingView = NSHostingView(rootView: GifImageUpdateHarness(model: model, url: imageURL))
        hostingView.layoutSubtreeIfNeeded()

        let imageView = try XCTUnwrap(firstImageView(in: hostingView))
        XCTAssertGreaterThan(try centerColor(of: imageView).redComponent, 0.8)

        try writePNG(to: imageURL, width: 20, height: 20, color: .blue)
        model.animates = true
        RunLoop.main.run(until: Date().addingTimeInterval(0.05))
        hostingView.layoutSubtreeIfNeeded()

        let colorAfterUpdate = try centerColor(of: imageView)
        XCTAssertGreaterThan(colorAfterUpdate.redComponent, 0.8)
        XCTAssertLessThan(colorAfterUpdate.blueComponent, 0.2)
    }

    private func makePNG(width: Int, height: Int, color: NSColor) throws -> URL {
        let url = FileManager.default.temporaryDirectory
            .appendingPathComponent("gif-image-test-\(UUID().uuidString).png")
        try writePNG(to: url, width: width, height: height, color: color)
        return url
    }

    private func writePNG(to url: URL, width: Int, height: Int, color: NSColor) throws {
        try pngData(width: width, height: height, color: color).write(to: url, options: .atomic)
    }

    private func firstImageView(in view: NSView) -> NSImageView? {
        if let imageView = view as? NSImageView {
            return imageView
        }
        return view.subviews.lazy.compactMap(firstImageView).first
    }

    private func centerColor(of imageView: NSImageView) throws -> NSColor {
        let image = try XCTUnwrap(imageView.image)
        let representation = try XCTUnwrap(NSBitmapImageRep(data: try XCTUnwrap(image.tiffRepresentation)))
        return try XCTUnwrap(representation.colorAt(
            x: representation.pixelsWide / 2,
            y: representation.pixelsHigh / 2
        ))
    }
}

@MainActor
final class WorkshopLayoutTests: XCTestCase {
    func testWorkshopPreviewUsesTheSquareSteamThumbnailAspectRatio() {
        let preview = WorkshopItemPreview(url: nil)
            .frame(width: 320)
        let hostingView = NSHostingView(rootView: preview)

        hostingView.layoutSubtreeIfNeeded()

        XCTAssertEqual(hostingView.fittingSize.width, 320, accuracy: 0.5)
        XCTAssertEqual(hostingView.fittingSize.height, 320, accuracy: 0.5)
    }
}

@MainActor
final class WallpaperExplorerLayoutTests: XCTestCase {
    func testDefaultGridDensityFitsAtLeastThreeColumnsInEightHundredPoints() {
        let grid = LazyVGrid(columns: WallpaperGridLayout.columns(iconSize: 200)) {
            ForEach(0..<6) { index in
                GridCellProbe(id: index)
                    .frame(height: 100)
            }
        }
        .frame(width: 800)
        let hostingView = NSHostingView(rootView: grid)

        hostingView.layoutSubtreeIfNeeded()

        let probes = descendantViews(of: GridCellProbeView.self, in: hostingView)
        let rowOrigins = probes.map { $0.convert($0.bounds, to: hostingView).minY }
        let firstRowOrigin = rowOrigins.min() ?? 0
        let firstRowCount = rowOrigins.filter { abs($0 - firstRowOrigin) < 0.5 }.count
        XCTAssertGreaterThanOrEqual(firstRowCount, 3)
    }

    func testVerticalScrollerDoesNotChangeTheGridViewportWidth() throws {
        let contentViewModel = ContentViewModel()
        let wallpaperViewModel = WallpaperViewModel()
        let explorer = WallpaperExplorer(
            contentViewModel: contentViewModel,
            wallpaperViewModel: wallpaperViewModel
        )
        .frame(width: 800, height: 600)
        let hostingView = NSHostingView(rootView: explorer)

        hostingView.layoutSubtreeIfNeeded()
        RunLoop.main.run(until: Date().addingTimeInterval(0.05))
        hostingView.layoutSubtreeIfNeeded()

        let scrollView = try XCTUnwrap(firstScrollView(in: hostingView))
        let originallyHasVerticalScroller = scrollView.hasVerticalScroller
        defer { scrollView.hasVerticalScroller = originallyHasVerticalScroller }

        scrollView.hasVerticalScroller = false
        scrollView.tile()
        let widthWithoutScroller = scrollView.contentSize.width

        scrollView.hasVerticalScroller = true
        scrollView.tile()
        let widthWithScroller = scrollView.contentSize.width

        XCTAssertEqual(widthWithScroller, widthWithoutScroller, accuracy: 0.5)
    }

    private func firstScrollView(in view: NSView) -> NSScrollView? {
        if let scrollView = view as? NSScrollView {
            return scrollView
        }
        return view.subviews.lazy.compactMap { self.firstScrollView(in: $0) }.first
    }

    private func descendantViews<ViewType: NSView>(
        of type: ViewType.Type,
        in view: NSView
    ) -> [ViewType] {
        let current = (view as? ViewType).map { [$0] } ?? []
        return current + view.subviews.flatMap { descendantViews(of: type, in: $0) }
    }
}

@MainActor
final class WorkshopPreviewImageLoaderTests: XCTestCase {
    private let url = URL(string: "https://example.com/preview")!

    func testLoadsAndDecodesPreviewImage() async throws {
        let dataLoader = StubRemoteImageDataLoader(responses: [
            .success(try pngData(width: 20, height: 10, color: .red)),
        ])
        let loader = RemoteImageLoader(
            dataLoader: dataLoader,
            cache: RemoteImageCache(countLimit: 10)
        )

        await loader.load(url: url)

        XCTAssertNotNil(loader.image)
        XCTAssertNil(loader.errorMessage)
        let requestCount = await dataLoader.requestCount
        XCTAssertEqual(requestCount, 1)
    }

    func testFailureIsVisibleAndRetryLoadsFreshData() async throws {
        let dataLoader = StubRemoteImageDataLoader(responses: [
            .failure(.unavailable),
            .success(try pngData(width: 20, height: 10, color: .blue)),
        ])
        let loader = RemoteImageLoader(
            dataLoader: dataLoader,
            cache: RemoteImageCache(countLimit: 10)
        )

        await loader.load(url: url)
        XCTAssertEqual(loader.errorMessage, PreviewStubError.unavailable.localizedDescription)

        await loader.retry()

        XCTAssertNotNil(loader.image)
        XCTAssertNil(loader.errorMessage)
        let requestCount = await dataLoader.requestCount
        XCTAssertEqual(requestCount, 2)
    }

    func testDecodedImagesAreSharedThroughTheCache() async throws {
        let dataLoader = StubRemoteImageDataLoader(responses: [
            .success(try pngData(width: 20, height: 10, color: .green)),
        ])
        let cache = RemoteImageCache(countLimit: 10)
        let firstLoader = RemoteImageLoader(dataLoader: dataLoader, cache: cache)
        let secondLoader = RemoteImageLoader(dataLoader: dataLoader, cache: cache)

        await firstLoader.load(url: url)
        await secondLoader.load(url: url)

        XCTAssertNotNil(firstLoader.image)
        XCTAssertTrue(firstLoader.image === secondLoader.image)
        let requestCount = await dataLoader.requestCount
        XCTAssertEqual(requestCount, 1)
    }

    func testAnimatedGIFKeepsAllFramesAndUsesAnAnimatingImageView() async throws {
        let dataLoader = StubRemoteImageDataLoader(responses: [
            .success(try animatedGIFData()),
        ])
        let loader = RemoteImageLoader(
            dataLoader: dataLoader,
            cache: RemoteImageCache(countLimit: 10)
        )

        await loader.load(url: url)

        let image = try XCTUnwrap(loader.image)
        let representation = try XCTUnwrap(image.representations.compactMap { $0 as? NSBitmapImageRep }.first)
        XCTAssertEqual(representation.value(forProperty: .frameCount) as? Int, 2)

        let hostingView = NSHostingView(rootView: GifImage(image: image, animates: true).frame(width: 100, height: 100))
        hostingView.layoutSubtreeIfNeeded()
        let imageView = try XCTUnwrap(firstImageView(in: hostingView))
        XCTAssertTrue(imageView.animates)
    }

    private func firstImageView(in view: NSView) -> NSImageView? {
        if let imageView = view as? NSImageView {
            return imageView
        }
        return view.subviews.lazy.compactMap { self.firstImageView(in: $0) }.first
    }
}

@MainActor
final class WorkshopFilterSelectionTests: XCTestCase {
    func testMatchesAnyOptionWithinEachGroupAndEveryActiveGroup() {
        let selection = WorkshopFilterSelection(
            contentRatings: ["Everyone", "Mature"],
            types: ["Video"],
            genres: ["Anime"]
        )

        XCTAssertTrue(selection.matches(tags: ["Everyone", "Video", "Anime"]))
        XCTAssertTrue(selection.matches(tags: ["Mature", "Video", "Anime"]))
        XCTAssertFalse(selection.matches(tags: ["Questionable", "Video", "Anime"]))
        XCTAssertFalse(selection.matches(tags: ["Everyone", "Scene", "Anime"]))
        XCTAssertFalse(selection.matches(tags: ["Everyone", "Video", "Nature"]))
    }

    func testEmptyGroupDoesNotRestrictResults() {
        let selection = WorkshopFilterSelection(
            contentRatings: [],
            types: ["Video"],
            genres: []
        )

        XCTAssertTrue(selection.matches(tags: ["Mature", "Video", "Nature"]))
        XCTAssertFalse(selection.matches(tags: ["Everyone", "Scene", "Anime"]))
    }

    func testCandidateTagsComeFromOneActiveGroup() {
        let selection = WorkshopFilterSelection(
            contentRatings: ["Everyone", "Mature"],
            types: ["Video"],
            genres: ["Anime", "Nature"]
        )

        XCTAssertEqual(selection.candidateTags, ["Video"])
    }

    func testPaginatorFillsPageAcrossSteamPagesAndKeepsOverflow() async throws {
        let api = StubWorkshopAPI(pages: [
            1: [
                item("1", tags: ["Everyone", "Video", "Anime"]),
                item("2", tags: ["Questionable", "Video", "Anime"]),
                item("3", tags: ["Everyone", "Video", "Landscape"]),
            ],
            2: [
                item("4", tags: ["Mature", "Video", "Nature"]),
                item("5", tags: ["Everyone", "Video", "Anime"]),
            ],
        ])
        let paginator = WorkshopSearchPaginator(
            api: api,
            query: .init(
                text: "",
                filters: WorkshopFilterSelection(
                    contentRatings: ["Everyone", "Mature"],
                    types: ["Video"],
                    genres: ["Anime", "Nature"]
                ),
                sortOrder: .trending
            ),
            visiblePageSize: 2,
            steamPageSize: 3
        )

        let firstPage = try await paginator.nextPage()
        XCTAssertEqual(firstPage.map(\.id), ["1", "4"])
        XCTAssertTrue(paginator.hasMoreResults)

        let secondPage = try await paginator.nextPage()
        XCTAssertEqual(secondPage.map(\.id), ["5"])
        XCTAssertFalse(paginator.hasMoreResults)

        XCTAssertEqual(api.requests.map(\.page), [1, 2])
        XCTAssertTrue(api.requests.allSatisfy { $0.tags == ["Video"] })
        XCTAssertTrue(api.requests.allSatisfy { !$0.matchAllTags })
    }

    private func item(_ id: String, tags: [String]) -> WorkshopItem {
        WorkshopItem(
            id: id,
            title: id,
            previewURL: nil,
            tags: tags,
            subscriptions: 0,
            fileSize: 0,
            creatorAppId: nil,
            description: nil
        )
    }
}

@MainActor
private final class GifImageUpdateModel: ObservableObject {
    @Published var animates = false
}

private struct GifImageUpdateHarness: View {
    @ObservedObject var model: GifImageUpdateModel
    let url: URL

    var body: some View {
        GifImage(contentsOf: url, animates: model.animates)
            .resizable()
            .frame(width: 100, height: 100)
    }
}

private final class GridCellProbeView: NSView {
    let id: Int

    init(id: Int) {
        self.id = id
        super.init(frame: .zero)
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }
}

private struct GridCellProbe: NSViewRepresentable {
    let id: Int

    func makeNSView(context: Context) -> GridCellProbeView {
        GridCellProbeView(id: id)
    }

    func updateNSView(_ nsView: GridCellProbeView, context: Context) {}
}

private enum PreviewStubError: LocalizedError {
    case unavailable
    case exhausted

    var errorDescription: String? {
        switch self {
        case .unavailable:
            return "Preview unavailable."
        case .exhausted:
            return "No stub response remains."
        }
    }
}

private actor StubRemoteImageDataLoader: RemoteImageDataLoading {
    enum Response {
        case success(Data)
        case failure(PreviewStubError)
    }

    private var responses: [Response]
    private(set) var requestCount = 0

    init(responses: [Response]) {
        self.responses = responses
    }

    func data(from url: URL) async throws -> Data {
        requestCount += 1
        guard !responses.isEmpty else { throw PreviewStubError.exhausted }

        switch responses.removeFirst() {
        case .success(let data):
            return data
        case .failure(let error):
            throw error
        }
    }
}

private func pngData(width: Int, height: Int, color: NSColor) throws -> Data {
    let representation = try bitmapRepresentation(width: width, height: height, color: color)
    return try XCTUnwrap(representation.representation(using: .png, properties: [:]))
}

private func pdfData() throws -> Data {
    let output = NSMutableData()
    let consumer = try XCTUnwrap(CGDataConsumer(data: output))
    var mediaBox = CGRect(x: 0, y: 0, width: 10, height: 10)
    let context = try XCTUnwrap(CGContext(
        consumer: consumer,
        mediaBox: &mediaBox,
        nil
    ))
    context.beginPDFPage(nil)
    context.setFillColor(NSColor.red.cgColor)
    context.fill(mediaBox)
    context.endPDFPage()
    context.closePDF()
    return output as Data
}

private func animatedGIFData() throws -> Data {
    let output = NSMutableData()
    let destination = try XCTUnwrap(CGImageDestinationCreateWithData(
        output,
        UTType.gif.identifier as CFString,
        2,
        nil
    ))
    let gifProperties = [
        kCGImagePropertyGIFDictionary: [kCGImagePropertyGIFLoopCount: 0],
    ] as CFDictionary
    CGImageDestinationSetProperties(destination, gifProperties)

    for color in [NSColor.black, NSColor.white] {
        let representation = try bitmapRepresentation(width: 20, height: 20, color: color)
        let image = try XCTUnwrap(representation.cgImage)
        let frameProperties = [
            kCGImagePropertyGIFDictionary: [kCGImagePropertyGIFDelayTime: 0.1],
        ] as CFDictionary
        CGImageDestinationAddImage(destination, image, frameProperties)
    }

    XCTAssertTrue(CGImageDestinationFinalize(destination))
    return output as Data
}

private func bitmapRepresentation(width: Int, height: Int, color: NSColor) throws -> NSBitmapImageRep {
    let representation = try XCTUnwrap(NSBitmapImageRep(
        bitmapDataPlanes: nil,
        pixelsWide: width,
        pixelsHigh: height,
        bitsPerSample: 8,
        samplesPerPixel: 4,
        hasAlpha: true,
        isPlanar: false,
        colorSpaceName: .deviceRGB,
        bytesPerRow: 0,
        bitsPerPixel: 0
    ))
    let context = try XCTUnwrap(NSGraphicsContext(bitmapImageRep: representation))
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = context
    color.setFill()
    NSRect(x: 0, y: 0, width: width, height: height).fill()
    NSGraphicsContext.restoreGraphicsState()
    return representation
}

private func stripedBitmapRepresentation() throws -> NSBitmapImageRep {
    let representation = try bitmapRepresentation(
        width: 300,
        height: 100,
        color: .green
    )
    let context = try XCTUnwrap(NSGraphicsContext(bitmapImageRep: representation))
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = context
    NSColor.red.setFill()
    NSRect(x: 0, y: 0, width: 100, height: 100).fill()
    NSColor.blue.setFill()
    NSRect(x: 200, y: 0, width: 100, height: 100).fill()
    NSGraphicsContext.restoreGraphicsState()
    return representation
}

private final class StubWorkshopAPI: WorkshopAPIClient {
    struct Request {
        let tags: [String]
        let matchAllTags: Bool
        let page: Int
    }

    private let pages: [Int: [WorkshopItem]]
    private(set) var requests: [Request] = []

    init(pages: [Int: [WorkshopItem]]) {
        self.pages = pages
    }

    func searchItems(
        query: String,
        tags: [String],
        matchAllTags: Bool,
        sortOrder: WorkshopSortOrder,
        page: Int,
        perPage: Int
    ) async throws -> [WorkshopItem] {
        requests.append(Request(tags: tags, matchAllTags: matchAllTags, page: page))
        return pages[page] ?? []
    }
}
