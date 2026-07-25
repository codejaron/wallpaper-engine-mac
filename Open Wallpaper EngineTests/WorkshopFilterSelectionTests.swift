import XCTest
@testable import Open_Wallpaper_Engine

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
