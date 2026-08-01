import Foundation

private enum SceneMediaArtworkTaskResult: Sendable {
    case success(SceneMediaArtwork)
    case failure(String)
}

@MainActor
final class SceneNowPlayingMonitor {
    private let provider: SceneMediaSnapshotProvider
    private let source: SceneNowPlayingSource
    private let timelineInterval: TimeInterval
    private var timelineTimer: Timer?
    private var refreshInFlight = false
    private var refreshPending = false
    private var started = false
    private var latestObservation: SceneNowPlayingObservation?
    private var cachedArtworkData: Data?
    private var cachedArtwork: SceneMediaArtwork?
    private var failedArtworkData: Data?
    private var artworkIssue: String?
    private var processingArtworkData: Data?
    private var artworkTask: Task<Void, Never>?
    private var artworkGeneration = 0

    init(
        provider: SceneMediaSnapshotProvider,
        source: SceneNowPlayingSource? = nil,
        timelineInterval: TimeInterval = 1
    ) {
        self.provider = provider
        self.source = source ?? MediaRemoteNowPlayingSource()
        self.timelineInterval = timelineInterval
    }

    func start() {
        guard !started else { return }
        do {
            try source.start { [weak self] in
                self?.requestRefresh()
            }
            started = true
            provider.reportInputIssue(nil)
            timelineTimer = Timer.scheduledTimer(
                withTimeInterval: timelineInterval,
                repeats: true
            ) { [weak self] _ in
                guard let self else { return }
                Task { @MainActor [self] in
                    self.tickTimeline()
                }
            }
            requestRefresh()
        } catch {
            source.stop()
            markUnavailable(
                because: "Now Playing unavailable: \(error.localizedDescription)"
            )
        }
    }

    func stop() {
        guard started || timelineTimer != nil || artworkTask != nil else { return }
        started = false
        timelineTimer?.invalidate()
        timelineTimer = nil
        source.stop()
        artworkGeneration &+= 1
        artworkTask?.cancel()
        artworkTask = nil
        refreshInFlight = false
        refreshPending = false
        latestObservation = nil
        cachedArtworkData = nil
        cachedArtwork = nil
        failedArtworkData = nil
        artworkIssue = nil
        processingArtworkData = nil
        markUnavailable(because: nil)
    }

    private func requestRefresh() {
        guard started else { return }
        guard !refreshInFlight else {
            refreshPending = true
            return
        }
        refreshInFlight = true
        source.fetch { [weak self] result in
            guard let self else { return }
            self.refreshInFlight = false
            guard self.started else {
                self.refreshPending = false
                return
            }
            self.handle(result)
            if self.refreshPending {
                self.refreshPending = false
                self.requestRefresh()
            }
        }
    }

    private func tickTimeline() {
        guard started,
              let observation = latestObservation,
              observation.playbackState == .playing else { return }
        let artwork = cachedArtworkData == observation.artworkData
            ? cachedArtwork
            : nil
        publish(content(for: observation, artwork: artwork))
    }

    private func handle(
        _ result: Result<SceneNowPlayingObservation?, Error>
    ) {
        switch result {
        case .failure(let error):
            latestObservation = nil
            clearArtworkState()
            markUnavailable(
                because: "Reading Now Playing failed: \(error.localizedDescription)"
            )
        case .success(nil):
            provider.reportInputIssue(nil)
            latestObservation = nil
            clearArtworkState()
            publish(.stopped)
        case .success(let observation?):
            latestObservation = observation
            reconcileArtwork(for: observation)
        }
    }

    private func reconcileArtwork(for observation: SceneNowPlayingObservation) {
        guard let artworkData = observation.artworkData else {
            clearArtworkState()
            provider.reportInputIssue(nil)
            publish(content(for: observation, artwork: nil))
            return
        }
        if cachedArtworkData == artworkData, let cachedArtwork {
            provider.reportInputIssue(nil)
            publish(content(for: observation, artwork: cachedArtwork))
            return
        }
        if failedArtworkData == artworkData {
            provider.reportInputIssue(artworkIssue)
            publish(content(for: observation, artwork: nil))
            return
        }
        if processingArtworkData == artworkData {
            provider.reportInputIssue(nil)
            publish(content(for: observation, artwork: nil))
            return
        }

        artworkGeneration &+= 1
        let generation = artworkGeneration
        artworkTask?.cancel()
        cachedArtworkData = nil
        cachedArtwork = nil
        failedArtworkData = nil
        artworkIssue = nil
        processingArtworkData = artworkData
        provider.reportInputIssue(nil)
        publish(content(for: observation, artwork: nil))
        artworkTask = Task.detached(priority: .utility) { [weak self] in
            let result: SceneMediaArtworkTaskResult
            do {
                result = .success(try SceneMediaArtworkProcessor.process(artworkData))
            } catch {
                result = .failure(error.localizedDescription)
            }
            guard !Task.isCancelled else { return }
            await self?.finishArtwork(
                result,
                sourceData: artworkData,
                generation: generation
            )
        }
    }

    private func finishArtwork(
        _ result: SceneMediaArtworkTaskResult,
        sourceData: Data,
        generation: Int
    ) {
        guard generation == artworkGeneration,
              processingArtworkData == sourceData else { return }
        artworkTask = nil
        processingArtworkData = nil
        guard latestObservation?.artworkData == sourceData,
              let observation = latestObservation else { return }
        switch result {
        case .success(let artwork):
            cachedArtworkData = sourceData
            cachedArtwork = artwork
            failedArtworkData = nil
            artworkIssue = nil
            provider.reportInputIssue(nil)
            publish(content(for: observation, artwork: artwork))
        case .failure(let detail):
            cachedArtworkData = nil
            cachedArtwork = nil
            failedArtworkData = sourceData
            artworkIssue = "Processing Now Playing artwork failed: \(detail)"
            provider.reportInputIssue(artworkIssue)
            publish(content(for: observation, artwork: nil))
        }
    }

    private func clearArtworkState() {
        artworkGeneration &+= 1
        artworkTask?.cancel()
        artworkTask = nil
        cachedArtworkData = nil
        cachedArtwork = nil
        failedArtworkData = nil
        artworkIssue = nil
        processingArtworkData = nil
    }

    private func content(
        for observation: SceneNowPlayingObservation,
        artwork: SceneMediaArtwork?
    ) -> SceneMediaContent {
        SceneMediaContent(
            playbackState: observation.playbackState,
            title: observation.title,
            artist: observation.artist,
            contentType: observation.contentType,
            albumTitle: observation.albumTitle,
            subTitle: "",
            albumArtist: "",
            genres: observation.genres,
            position: observation.position(at: Date()),
            duration: observation.duration,
            thumbnail: artwork?.thumbnail,
            primaryColor: artwork?.primaryColor ?? .black,
            secondaryColor: artwork?.secondaryColor ?? .black,
            tertiaryColor: artwork?.tertiaryColor ?? .black,
            textColor: artwork?.textColor ?? .black,
            highContrastColor: artwork?.highContrastColor ?? .black
        )
    }

    private func publish(_ content: SceneMediaContent) {
        do {
            try provider.publish(content)
        } catch {
            provider.reportInputIssue(
                "Publishing Now Playing information failed: \(error.localizedDescription)"
            )
        }
    }

    private func markUnavailable(because issue: String?) {
        do {
            try provider.markUnavailable()
            provider.reportInputIssue(issue)
        } catch {
            let failure = "Updating Now Playing availability failed: \(error.localizedDescription)"
            provider.reportInputIssue(
                issue.map { "\($0); \(failure)" } ?? failure
            )
        }
    }
}
