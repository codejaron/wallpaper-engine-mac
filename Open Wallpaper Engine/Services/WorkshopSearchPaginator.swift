import Foundation

protocol WorkshopAPIClient {
    func searchItems(
        query: String,
        tags: [String],
        matchAllTags: Bool,
        sortOrder: WorkshopSortOrder,
        page: Int,
        perPage: Int
    ) async throws -> [WorkshopItem]
}

extension WorkshopAPIService: WorkshopAPIClient {}

@MainActor
final class WorkshopSearchPaginator {
    struct Query {
        let text: String
        let filters: WorkshopFilterSelection
        let sortOrder: WorkshopSortOrder
    }

    private let api: any WorkshopAPIClient
    private let query: Query
    private let visiblePageSize: Int
    private let steamPageSize: Int
    private let maximumSteamPage: Int

    private var bufferedItems: [WorkshopItem] = []
    private var seenItemIDs: Set<String> = []
    private var nextSteamPage = 1
    private var steamResultsExhausted = false

    private(set) var hasMoreResults = true

    init(
        api: any WorkshopAPIClient,
        query: Query,
        visiblePageSize: Int = 20,
        steamPageSize: Int = 50,
        maximumSteamPage: Int = 1_000
    ) {
        self.api = api
        self.query = query
        self.visiblePageSize = visiblePageSize
        self.steamPageSize = steamPageSize
        self.maximumSteamPage = maximumSteamPage
    }

    func nextPage() async throws -> [WorkshopItem] {
        var results: [WorkshopItem] = []
        moveBufferedItems(into: &results)

        while results.count < visiblePageSize && !steamResultsExhausted {
            guard nextSteamPage <= maximumSteamPage else {
                steamResultsExhausted = true
                break
            }

            let requestedPage = nextSteamPage
            let candidates = try await api.searchItems(
                query: query.text,
                tags: query.filters.candidateTags,
                matchAllTags: false,
                sortOrder: query.sortOrder,
                page: requestedPage,
                perPage: steamPageSize
            )

            nextSteamPage += 1
            if candidates.count < steamPageSize || requestedPage == maximumSteamPage {
                steamResultsExhausted = true
            }

            for item in candidates where query.filters.matches(tags: item.tags) {
                if seenItemIDs.insert(item.id).inserted {
                    bufferedItems.append(item)
                }
            }

            moveBufferedItems(into: &results)
        }

        hasMoreResults = !bufferedItems.isEmpty || !steamResultsExhausted
        return results
    }

    private func moveBufferedItems(into results: inout [WorkshopItem]) {
        let count = min(visiblePageSize - results.count, bufferedItems.count)
        guard count > 0 else { return }

        results.append(contentsOf: bufferedItems.prefix(count))
        bufferedItems.removeFirst(count)
    }
}
