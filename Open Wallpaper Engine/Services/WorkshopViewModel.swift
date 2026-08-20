import Foundation
import SwiftUI
import Combine

class WorkshopViewModel: ObservableObject {
    @Published var items: [WorkshopItem] = []
    @Published var searchText = ""
    @Published var sortOrder: WorkshopSortOrder = .trending
    @Published var isLoading = false
    @Published var errorMessage: String?
    @Published var currentPage = 1
    @Published var filters = WorkshopFilterSelection()
    @Published var isFilterReveal = false
    @Published private(set) var hasMoreResults = true

    let steamCmd: SteamCmdService
    private let api: any WorkshopAPIClient
    private var cancellable: AnyCancellable?
    private var paginator: WorkshopSearchPaginator?
    private var searchGeneration = UUID()

    static let contentRatingTags = ["Everyone", "Questionable", "Mature"]

    static let typeTags = ["Scene", "Video", "Web", "Application"]

    static let genreTags = [
        "Abstract", "Animal", "Anime", "Cartoon", "CGI",
        "Cyberpunk", "Fantasy", "Game", "Girls", "Guys",
        "Landscape", "Medieval", "Memes", "MMD", "Music",
        "Nature", "Pixel Art", "Relaxing", "Retro", "Sci-Fi",
        "Sports", "Technology", "Television", "Vehicle",
    ]

    static let resolutionTags = [
        "1920 x 1080", "2560 x 1440", "3840 x 2160",
        "3440 x 1440", "1440 x 2560",
    ]

    init(
        steamCmd: SteamCmdService,
        api: any WorkshopAPIClient = WorkshopAPIService()
    ) {
        self.steamCmd = steamCmd
        self.api = api
        // Forward steamCmd changes (e.g. downloadProgress) to trigger view updates
        self.cancellable = steamCmd.objectWillChange.sink { [weak self] _ in
            self?.objectWillChange.send()
        }
    }

    @MainActor
    func search() async {
        let generation = UUID()
        searchGeneration = generation
        let paginator = WorkshopSearchPaginator(
            api: api,
            query: .init(text: searchText, filters: filters, sortOrder: sortOrder)
        )
        self.paginator = paginator

        isLoading = true
        errorMessage = nil
        currentPage = 1
        items = []
        hasMoreResults = true
        defer {
            if searchGeneration == generation {
                isLoading = false
            }
        }

        do {
            let results = try await paginator.nextPage()
            guard searchGeneration == generation else { return }
            items = results
            hasMoreResults = paginator.hasMoreResults
        } catch is CancellationError {
            return
        } catch {
            guard searchGeneration == generation else { return }
            errorMessage = error.localizedDescription
        }
    }

    @MainActor
    func loadMore() async {
        guard !isLoading, hasMoreResults, let paginator else { return }

        let generation = searchGeneration
        isLoading = true
        errorMessage = nil
        defer {
            if searchGeneration == generation {
                isLoading = false
            }
        }

        do {
            let results = try await paginator.nextPage()
            guard searchGeneration == generation else { return }
            items.append(contentsOf: results)
            hasMoreResults = paginator.hasMoreResults
            if !results.isEmpty {
                currentPage += 1
            }
        } catch is CancellationError {
            return
        } catch {
            guard searchGeneration == generation else { return }
            errorMessage = error.localizedDescription
        }
    }

    @MainActor
    func download(item: WorkshopItem) {
        steamCmd.downloadWorkshopItem(item: item)
    }

    @MainActor
    func downloadState(for item: WorkshopItem) -> SteamCmdService.DownloadState? {
        steamCmd.downloadState(for: item.id)
    }

    func isTagSelected(_ tag: String, in group: WorkshopFilterGroup) -> Bool {
        filters.contains(tag, in: group)
    }

    func setTag(_ tag: String, in group: WorkshopFilterGroup, isSelected: Bool) {
        filters.set(tag, in: group, isSelected: isSelected)
        currentPage = 1
    }

    func resetFilters() {
        filters = WorkshopFilterSelection()
        currentPage = 1
    }
}
