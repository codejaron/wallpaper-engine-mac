import AppKit
import ImageIO
import SwiftUI
import UniformTypeIdentifiers
import XCTest
@testable import Open_Wallpaper_Engine

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
        let dataLoader = StubWorkshopPreviewDataLoader(responses: [
            .success(try pngData(width: 20, height: 10, color: .red)),
        ])
        let loader = WorkshopPreviewImageLoader(
            dataLoader: dataLoader,
            cache: WorkshopPreviewImageCache(countLimit: 10)
        )

        await loader.load(url: url)

        XCTAssertNotNil(loader.image)
        XCTAssertNil(loader.errorMessage)
        let requestCount = await dataLoader.requestCount
        XCTAssertEqual(requestCount, 1)
    }

    func testFailureIsVisibleAndRetryLoadsFreshData() async throws {
        let dataLoader = StubWorkshopPreviewDataLoader(responses: [
            .failure(.unavailable),
            .success(try pngData(width: 20, height: 10, color: .blue)),
        ])
        let loader = WorkshopPreviewImageLoader(
            dataLoader: dataLoader,
            cache: WorkshopPreviewImageCache(countLimit: 10)
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
        let dataLoader = StubWorkshopPreviewDataLoader(responses: [
            .success(try pngData(width: 20, height: 10, color: .green)),
        ])
        let cache = WorkshopPreviewImageCache(countLimit: 10)
        let firstLoader = WorkshopPreviewImageLoader(dataLoader: dataLoader, cache: cache)
        let secondLoader = WorkshopPreviewImageLoader(dataLoader: dataLoader, cache: cache)

        await firstLoader.load(url: url)
        await secondLoader.load(url: url)

        XCTAssertNotNil(firstLoader.image)
        XCTAssertTrue(firstLoader.image === secondLoader.image)
        let requestCount = await dataLoader.requestCount
        XCTAssertEqual(requestCount, 1)
    }

    func testAnimatedGIFKeepsAllFramesAndUsesAnAnimatingImageView() async throws {
        let dataLoader = StubWorkshopPreviewDataLoader(responses: [
            .success(try animatedGIFData()),
        ])
        let loader = WorkshopPreviewImageLoader(
            dataLoader: dataLoader,
            cache: WorkshopPreviewImageCache(countLimit: 10)
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

private actor StubWorkshopPreviewDataLoader: WorkshopPreviewDataLoading {
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
