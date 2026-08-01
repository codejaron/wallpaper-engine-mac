import Foundation

struct SceneNowPlayingObservation: Equatable, Sendable {
    let playbackState: SceneMediaPlaybackState
    let title: String
    let artist: String
    let contentType: String
    let albumTitle: String
    let genres: String
    let elapsedTime: Double
    let duration: Double
    let timestamp: Date?
    let playbackRate: Double
    let artworkData: Data?

    func position(at date: Date) -> Double {
        var result = elapsedTime
        if playbackState == .playing,
           playbackRate > 0,
           let timestamp {
            result += max(0, date.timeIntervalSince(timestamp)) * playbackRate
        }
        if duration > 0 {
            result = min(result, duration)
        }
        return max(0, result)
    }
}

@MainActor
protocol SceneNowPlayingSource: AnyObject {
    func start(changeHandler: @escaping @MainActor () -> Void) throws
    func stop()
    func fetch(
        completion: @escaping @MainActor (
            Result<SceneNowPlayingObservation?, Error>
        ) -> Void
    )
}

enum MediaRemoteNowPlayingError: LocalizedError, Equatable {
    case helperLibraryUnavailable(String)
    case perlUnavailable
    case processLaunchFailed(String)
    case notStarted
    case requestInFlight
    case requestTimedOut
    case helperTerminated(Int32, String)
    case helperOutputTooLarge
    case invalidResponse(String)
    case writeFailed(String)

    var errorDescription: String? {
        switch self {
        case .helperLibraryUnavailable(let path):
            return "The bundled MediaRemote helper is unavailable at \(path)"
        case .perlUnavailable:
            return "The macOS Perl runtime required by MediaRemote is unavailable"
        case .processLaunchFailed(let detail):
            return "Starting the MediaRemote helper failed: \(detail)"
        case .notStarted:
            return "The macOS Now Playing source is not running"
        case .requestInFlight:
            return "A macOS Now Playing request is already in progress"
        case .requestTimedOut:
            return "Reading macOS Now Playing information timed out"
        case .helperTerminated(let status, let detail):
            let suffix = detail.isEmpty ? "" : ": \(detail)"
            return "The MediaRemote helper exited with status \(status)\(suffix)"
        case .helperOutputTooLarge:
            return "The MediaRemote helper response exceeded the 64 MiB limit"
        case .invalidResponse(let detail):
            return "The MediaRemote helper returned invalid data: \(detail)"
        case .writeFailed(let detail):
            return "Requesting macOS Now Playing information failed: \(detail)"
        }
    }
}

@MainActor
final class MediaRemoteNowPlayingSource: SceneNowPlayingSource {
    typealias Completion = @MainActor (
        Result<SceneNowPlayingObservation?, Error>
    ) -> Void

    private static let perlURL = URL(fileURLWithPath: "/usr/bin/perl")
    private static let helperLibraryName = "MediaRemoteMini.dylib"
    private static let maximumResponseBytes = 64 * 1024 * 1024

    // This is the loader used by nowplaying-cli v2.1.0. The loaded adapter is
    // BSD-3-Clause code vendored under ThirdParty/MediaRemoteMini.
    private static let helperProgram = #"""
    use strict;
    use warnings;
    use DynaLoader;

    my $lib = shift @ARGV or die "Missing MediaRemote helper library\n";
    my $handle = DynaLoader::dl_load_file($lib, 0)
      or die "Failed to load MediaRemote helper: " . DynaLoader::dl_error() . "\n";
    my $symbol = DynaLoader::dl_find_symbol($handle, "adapter_get_env")
      or die "MediaRemote helper is missing adapter_get_env\n";
    my $function = DynaLoader::dl_install_xsub(
      "main::adapter_get_env",
      $symbol
    );
    $| = 1;
    while (defined(my $request = <STDIN>)) {
      &$function();
    }
    """#

    private let configuredHelperLibraryURL: URL?
    private let pollingInterval: TimeInterval
    private let requestTimeout: TimeInterval

    private var process: Process?
    private var standardInput: FileHandle?
    private var standardOutput: FileHandle?
    private var standardError: FileHandle?
    private var outputBuffer = Data()
    private var errorBuffer = Data()
    private var pendingCompletion: Completion?
    private var requestGeneration = 0
    private var pollingTimer: Timer?
    private var changeHandler: (@MainActor () -> Void)?

    init(
        helperLibraryURL: URL? = nil,
        pollingInterval: TimeInterval = 2,
        requestTimeout: TimeInterval = 3
    ) {
        configuredHelperLibraryURL = helperLibraryURL
        self.pollingInterval = pollingInterval
        self.requestTimeout = requestTimeout
    }

    func start(changeHandler: @escaping @MainActor () -> Void) throws {
        guard process == nil else { return }
        guard FileManager.default.isExecutableFile(
            atPath: Self.perlURL.path
        ) else {
            throw MediaRemoteNowPlayingError.perlUnavailable
        }

        let libraryURL = configuredHelperLibraryURL ??
            Self.bundledHelperLibraryURL()
        guard let libraryURL,
              FileManager.default.isReadableFile(atPath: libraryURL.path) else {
            throw MediaRemoteNowPlayingError.helperLibraryUnavailable(
                libraryURL?.path ??
                    Bundle.main.bundleURL
                        .appendingPathComponent("Contents/Frameworks")
                        .appendingPathComponent(Self.helperLibraryName)
                        .path
            )
        }

        let nextProcess = Process()
        let inputPipe = Pipe()
        let outputPipe = Pipe()
        let errorPipe = Pipe()
        nextProcess.executableURL = Self.perlURL
        nextProcess.arguments = [
            "-MDynaLoader",
            "-e",
            Self.helperProgram,
            libraryURL.path,
        ]
        nextProcess.standardInput = inputPipe
        nextProcess.standardOutput = outputPipe
        nextProcess.standardError = errorPipe

        outputBuffer.removeAll(keepingCapacity: true)
        errorBuffer.removeAll(keepingCapacity: true)
        standardInput = inputPipe.fileHandleForWriting
        standardOutput = outputPipe.fileHandleForReading
        standardError = errorPipe.fileHandleForReading
        process = nextProcess
        self.changeHandler = changeHandler

        standardOutput?.readabilityHandler = { [weak self] handle in
            let data = handle.availableData
            Task { @MainActor [weak self] in
                self?.receiveStandardOutput(data)
            }
        }
        standardError?.readabilityHandler = { [weak self] handle in
            let data = handle.availableData
            Task { @MainActor [weak self] in
                self?.receiveStandardError(data)
            }
        }
        nextProcess.terminationHandler = { [weak self] terminated in
            Task { @MainActor [weak self] in
                self?.helperDidTerminate(terminated)
            }
        }

        do {
            try nextProcess.run()
        } catch {
            detachProcess(clearChangeHandler: true)
            throw MediaRemoteNowPlayingError.processLaunchFailed(
                error.localizedDescription
            )
        }

        if pollingInterval > 0 {
            pollingTimer = Timer.scheduledTimer(
                withTimeInterval: pollingInterval,
                repeats: true
            ) { [weak self] _ in
                Task { @MainActor [weak self] in
                    self?.changeHandler?()
                }
            }
        }
    }

    func stop() {
        requestGeneration &+= 1
        pendingCompletion = nil
        let runningProcess = process
        detachProcess(clearChangeHandler: true)
        if runningProcess?.isRunning == true {
            runningProcess?.terminate()
        }
    }

    func fetch(completion: @escaping Completion) {
        guard let process, process.isRunning, let standardInput else {
            completion(.failure(MediaRemoteNowPlayingError.notStarted))
            return
        }
        guard pendingCompletion == nil else {
            completion(.failure(MediaRemoteNowPlayingError.requestInFlight))
            return
        }

        pendingCompletion = completion
        requestGeneration &+= 1
        let generation = requestGeneration
        errorBuffer.removeAll(keepingCapacity: true)
        do {
            try standardInput.write(contentsOf: Data([0x0A]))
        } catch {
            failRunningHelper(
                MediaRemoteNowPlayingError.writeFailed(
                    error.localizedDescription
                )
            )
            return
        }

        DispatchQueue.main.asyncAfter(deadline: .now() + requestTimeout) {
            MainActor.assumeIsolated { [weak self] in
                guard let self,
                      self.requestGeneration == generation,
                      self.pendingCompletion != nil else { return }
                self.failRunningHelper(
                    MediaRemoteNowPlayingError.requestTimedOut
                )
            }
        }
    }

    nonisolated static func observation(
        fromHelperPayload payload: NSDictionary
    ) throws -> SceneNowPlayingObservation {
        let title = string(payload["title"])
        let artist = string(payload["artist"])
        let album = string(payload["album"])
        let artworkData: Data?
        let encodedArtwork = string(payload["artworkData"])
        if encodedArtwork.isEmpty {
            artworkData = nil
        } else {
            guard let decoded = Data(base64Encoded: encodedArtwork),
                  !decoded.isEmpty else {
                throw MediaRemoteNowPlayingError.invalidResponse(
                    "artworkData is not valid non-empty base64"
                )
            }
            artworkData = decoded
        }

        let duration = try nonNegativeNumber(
            payload["duration"],
            key: "duration"
        ) ?? 0
        let elapsedTime = try nonNegativeNumber(
            payload["elapsedTime"],
            key: "elapsedTime"
        ) ?? 0
        guard !title.isEmpty ||
                !artist.isEmpty ||
                !album.isEmpty ||
                artworkData != nil ||
                duration > 0 else {
            throw MediaRemoteNowPlayingError.invalidResponse(
                "the payload contains no media metadata"
            )
        }

        let playing = bool(payload["playing"]) ?? false
        let reportedRate = try nonNegativeNumber(
            payload["playbackRate"],
            key: "playbackRate"
        )
        let playbackRate = playing ? max(reportedRate ?? 1, 0) : 0
        let mediaType = string(payload["mediaType"])
        let isMusic = bool(payload["isMusicApp"]) == true ||
            mediaType.localizedCaseInsensitiveContains("audio")
        let contentType: String
        if isMusic {
            contentType = "music"
        } else if mediaType.localizedCaseInsensitiveContains("video") {
            contentType = "video"
        } else {
            contentType = mediaType
        }

        let genres: String
        if let values = payload["genre"] as? [String] {
            genres = values.joined(separator: ", ")
        } else {
            genres = string(payload["genre"])
        }
        let timestampText = string(payload["timestamp"])
        let timestamp: Date?
        if timestampText.isEmpty {
            timestamp = nil
        } else {
            timestamp = ISO8601DateFormatter().date(from: timestampText)
            guard timestamp != nil else {
                throw MediaRemoteNowPlayingError.invalidResponse(
                    "timestamp is not ISO-8601"
                )
            }
        }

        return SceneNowPlayingObservation(
            playbackState: playing ? .playing : .paused,
            title: title,
            artist: artist,
            contentType: contentType,
            albumTitle: album,
            genres: genres,
            elapsedTime: elapsedTime,
            duration: duration,
            timestamp: timestamp,
            playbackRate: playbackRate,
            artworkData: artworkData
        )
    }

    private static func bundledHelperLibraryURL() -> URL? {
        Bundle.main.privateFrameworksURL?
            .appendingPathComponent(helperLibraryName)
    }

    private func receiveStandardOutput(_ data: Data) {
        guard !data.isEmpty else { return }
        outputBuffer.append(data)
        guard outputBuffer.count <= Self.maximumResponseBytes else {
            failRunningHelper(
                MediaRemoteNowPlayingError.helperOutputTooLarge
            )
            return
        }

        while let newline = outputBuffer.firstIndex(of: 0x0A) {
            let line = Data(outputBuffer[..<newline])
            outputBuffer.removeSubrange(outputBuffer.startIndex...newline)
            guard let completion = pendingCompletion else {
                failRunningHelper(
                    MediaRemoteNowPlayingError.invalidResponse(
                        "an unsolicited response was received"
                    )
                )
                return
            }
            pendingCompletion = nil
            completion(Self.parseResponse(line))
        }
    }

    private func receiveStandardError(_ data: Data) {
        guard !data.isEmpty else { return }
        errorBuffer.append(data)
        if errorBuffer.count > 8_192 {
            errorBuffer.removeFirst(errorBuffer.count - 8_192)
        }
    }

    private func helperDidTerminate(_ terminated: Process) {
        guard process === terminated else { return }
        let detail = standardErrorDetail()
        let completion = pendingCompletion
        let handler = changeHandler
        pendingCompletion = nil
        detachProcess(clearChangeHandler: true)
        let error = MediaRemoteNowPlayingError.helperTerminated(
            terminated.terminationStatus,
            detail
        )
        if let completion {
            completion(.failure(error))
        } else {
            handler?()
        }
    }

    private func failRunningHelper(_ error: MediaRemoteNowPlayingError) {
        let completion = pendingCompletion
        pendingCompletion = nil
        let runningProcess = process
        detachProcess(clearChangeHandler: true)
        if runningProcess?.isRunning == true {
            runningProcess?.terminate()
        }
        completion?(.failure(error))
    }

    private func detachProcess(clearChangeHandler: Bool) {
        pollingTimer?.invalidate()
        pollingTimer = nil
        standardOutput?.readabilityHandler = nil
        standardError?.readabilityHandler = nil
        try? standardInput?.close()
        try? standardOutput?.close()
        try? standardError?.close()
        standardInput = nil
        standardOutput = nil
        standardError = nil
        process?.terminationHandler = nil
        process = nil
        outputBuffer.removeAll(keepingCapacity: true)
        errorBuffer.removeAll(keepingCapacity: true)
        if clearChangeHandler {
            changeHandler = nil
        }
    }

    private func standardErrorDetail() -> String {
        String(data: errorBuffer, encoding: .utf8)?
            .trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
    }

    nonisolated private static func parseResponse(
        _ data: Data
    ) -> Result<SceneNowPlayingObservation?, Error> {
        do {
            let text = String(data: data, encoding: .utf8)?
                .trimmingCharacters(in: .whitespacesAndNewlines)
            guard let text, !text.isEmpty else {
                throw MediaRemoteNowPlayingError.invalidResponse(
                    "the response is empty or not UTF-8"
                )
            }
            if text == "null" {
                return .success(nil)
            }
            let value = try JSONSerialization.jsonObject(with: data)
            guard let payload = value as? NSDictionary else {
                throw MediaRemoteNowPlayingError.invalidResponse(
                    "the response root is not an object"
                )
            }
            return .success(try observation(fromHelperPayload: payload))
        } catch {
            return .failure(error)
        }
    }

    nonisolated private static func string(_ value: Any?) -> String {
        (value as? String)?
            .trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
    }

    nonisolated private static func bool(_ value: Any?) -> Bool? {
        (value as? NSNumber)?.boolValue
    }

    nonisolated private static func nonNegativeNumber(
        _ value: Any?,
        key: String
    ) throws -> Double? {
        guard let value else { return nil }
        guard let number = value as? NSNumber else {
            throw MediaRemoteNowPlayingError.invalidResponse(
                "\(key) is not numeric"
            )
        }
        let result = number.doubleValue
        guard result.isFinite, result >= 0 else {
            throw MediaRemoteNowPlayingError.invalidResponse(
                "\(key) is not a finite non-negative number"
            )
        }
        return result
    }
}
