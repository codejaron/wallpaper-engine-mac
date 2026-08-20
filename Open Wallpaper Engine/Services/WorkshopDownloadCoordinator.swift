import Foundation
import Darwin

final class WorkshopDownloadCoordinator {
    typealias State = SteamCmdService.DownloadState
    typealias ProgressUpdate = SteamCmdService.DownloadProgressUpdate

    private let queue = DispatchQueue(
        label: "open-wallpaper-engine.steamcmd.download",
        qos: .userInitiated
    )

    func enqueue(
        workshopId: String,
        cmdPath: String,
        username: String,
        expectedDownloadBytes: Int64,
        destinationDirectory: URL,
        onStateChange: @escaping @Sendable (State) -> Void
    ) {
        let workItem = DispatchWorkItem {
            Self.performDownload(
                workshopId: workshopId,
                cmdPath: cmdPath,
                username: username,
                expectedDownloadBytes: expectedDownloadBytes,
                destinationDirectory: destinationDirectory,
                onStateChange: onStateChange
            )
        }
        queue.async(execute: workItem)
    }

    static func parseProgress(_ output: String) -> ProgressUpdate? {
        let records = output.split(whereSeparator: { $0 == "\n" || $0 == "\r" })
        for record in records.reversed() {
            if let percentage = downloadPercentage(in: String(record)) {
                return ProgressUpdate(
                    progress: percentage / 100,
                    status: "Downloading..."
                )
            }

            let line = record.lowercased()
            if line.contains("validating") || line.contains("update state (0x5)") {
                return ProgressUpdate(progress: nil, status: "Validating...")
            }
            if line.contains("success") {
                return ProgressUpdate(
                    progress: 1,
                    status: "Download complete, importing..."
                )
            }
            if line.contains("0x101") {
                return ProgressUpdate(progress: nil, status: "Committing...")
            }
            if line.contains("workshop_download_item") || line.contains("downloading item") {
                return ProgressUpdate(progress: nil, status: "Preparing download...")
            }
            if line.contains("downloading") || line.contains("0x61") {
                return ProgressUpdate(progress: nil, status: "Downloading...")
            }
            if line.contains("logging in") || line.contains("logged in") {
                return ProgressUpdate(progress: nil, status: "Authenticating...")
            }
        }
        return nil
    }

    static func isValidWallpaperPackage(
        at directory: URL,
        fileManager: FileManager = .default
    ) -> Bool {
        var isDirectory: ObjCBool = false
        guard fileManager.fileExists(atPath: directory.path, isDirectory: &isDirectory),
              isDirectory.boolValue,
              let data = try? Data(contentsOf: directory.appending(path: "project.json")),
              (try? JSONDecoder().decode(WEProject.self, from: data)) != nil else {
            return false
        }
        return true
    }

    private static func performDownload(
        workshopId: String,
        cmdPath: String,
        username: String,
        expectedDownloadBytes: Int64,
        destinationDirectory: URL,
        onStateChange: @escaping @Sendable (State) -> Void
    ) {
        let progressReporter = WorkshopProgressReporter(
            initialProgress: 0,
            initialStatus: "Starting steamcmd...",
            onStateChange: onStateChange
        )
        progressReporter.publishCurrentState()

        let process = Process()
        let outputPipe = Pipe()
        let outputQueue = DispatchQueue(
            label: "open-wallpaper-engine.steamcmd.output.\(workshopId)"
        )
        let outputBuffer = WorkshopOutputBuffer()
        let handle = outputPipe.fileHandleForReading
        let contentLogMonitor = SteamContentLogMonitor(cmdPath: cmdPath)

        process.executableURL = URL(fileURLWithPath: cmdPath)
        process.currentDirectoryURL = URL(fileURLWithPath: cmdPath).deletingLastPathComponent()
        process.arguments = [
            "+login", username,
            "+workshop_download_item", "431960", workshopId, "validate",
            "+quit"
        ]
        process.standardOutput = outputPipe
        process.standardError = outputPipe

        handle.readabilityHandler = { fileHandle in
            let data = fileHandle.availableData
            guard !data.isEmpty else { return }
            outputQueue.async {
                guard let output = String(data: data, encoding: .utf8) else { return }
                outputBuffer.append(output)
                if let update = parseProgress(output) {
                    progressReporter.publish(update)
                }
            }
        }

        do {
            try process.run()
            let networkMonitor = ProcessTreeNetworkMonitor(
                rootProcessIdentifier: process.processIdentifier
            )
            defer { networkMonitor.stop() }
            // Workshop downloads intentionally have no wall-clock timeout.
            while process.isRunning {
                var reportedProgress: Double?

                networkMonitor.startIfPossible()
                if expectedDownloadBytes > 0,
                   let receivedBytes = networkMonitor.readReceivedBytes() {
                    reportedProgress = Double(receivedBytes) / Double(expectedDownloadBytes)
                }

                if let byteProgress = contentLogMonitor.readProgress() {
                    let contentLogProgress =
                        Double(byteProgress.downloadedBytes) /
                        Double(byteProgress.totalBytes)
                    reportedProgress = max(reportedProgress ?? 0, contentLogProgress)
                }

                if let reportedProgress {
                    progressReporter.publish(
                        ProgressUpdate(
                            progress: min(reportedProgress, 0.99),
                            status: "Downloading..."
                        )
                    )
                }

                Thread.sleep(forTimeInterval: 0.5)
            }
            process.waitUntilExit()
        } catch {
            handle.readabilityHandler = nil
            onStateChange(.failed("steamcmd failed to run: \(error.localizedDescription)"))
            return
        }

        handle.readabilityHandler = nil
        let remainingData = handle.readDataToEndOfFile()
        outputQueue.sync {
            if let remainingOutput = String(data: remainingData, encoding: .utf8) {
                outputBuffer.append(remainingOutput)
                if let update = parseProgress(remainingOutput) {
                    progressReporter.publish(update)
                }
            }
        }

        let fullOutput = outputBuffer.stringValue()
        let exitCode = process.terminationStatus

        if let commandFailure = commandFailure(output: fullOutput, exitCode: exitCode) {
            onStateChange(.failed(commandFailure))
            return
        }

        guard let sourceDirectory = downloadedItemDirectory(
            workshopId: workshopId,
            cmdPath: cmdPath
        ) else {
            onStateChange(.failed("steamcmd finished, but the downloaded files were not found"))
            return
        }

        onStateChange(.downloading(progress: 1, status: "Copying to library..."))
        do {
            try installDownloadedItem(
                from: sourceDirectory,
                to: destinationDirectory,
                workshopId: workshopId
            )
            onStateChange(.completed)
        } catch {
            onStateChange(.failed("Import failed: \(error.localizedDescription)"))
        }
    }

    private static func downloadPercentage(in output: String) -> Double? {
        let progressPattern = #"progress:\s*([0-9]+(?:\.[0-9]+)?)"#
        var searchRange = output.startIndex..<output.endIndex
        var latestPercentage: Double?

        while let match = output.range(
            of: progressPattern,
            options: [.regularExpression, .caseInsensitive],
            range: searchRange
        ) {
            let value = output[match]
                .split(separator: ":", maxSplits: 1)
                .last?
                .trimmingCharacters(in: .whitespacesAndNewlines)
            if let value, let percentage = Double(value) {
                latestPercentage = min(max(percentage, 0), 100)
            }
            searchRange = match.upperBound..<output.endIndex
        }
        return latestPercentage
    }

    static func parseContentLogByteProgress(_ output: String) -> WorkshopByteProgress? {
        let pattern = #"update started\s*:\s*download\s+([0-9]+)\/([0-9]+)"#
        guard let expression = try? NSRegularExpression(
            pattern: pattern,
            options: [.caseInsensitive]
        ) else {
            return nil
        }

        let range = NSRange(output.startIndex..., in: output)
        let matches = expression.matches(in: output, range: range)
        guard let match = matches.last,
              let downloadedRange = Range(match.range(at: 1), in: output),
              let totalRange = Range(match.range(at: 2), in: output),
              let downloadedBytes = UInt64(output[downloadedRange]),
              let totalBytes = UInt64(output[totalRange]),
              totalBytes > 0 else {
            return nil
        }

        return WorkshopByteProgress(
            downloadedBytes: min(downloadedBytes, totalBytes),
            totalBytes: totalBytes
        )
    }

    static func parseContentLogTransferRateMbps(_ output: String) -> Double? {
        let patterns = [
            #"current download rate:\s*([0-9]+(?:\.[0-9]+)?)\s*mbps"#,
            #"rate was\s*[0-9]+(?:\.[0-9]+)?,\s*now\s*([0-9]+(?:\.[0-9]+)?)"#,
        ]

        for pattern in patterns {
            guard let expression = try? NSRegularExpression(
                pattern: pattern,
                options: [.caseInsensitive]
            ) else {
                continue
            }

            let range = NSRange(output.startIndex..., in: output)
            guard let match = expression.matches(in: output, range: range).last,
                  let rateRange = Range(match.range(at: 1), in: output),
                  let rate = Double(output[rateRange]),
                  rate >= 0 else {
                continue
            }
            return rate
        }

        return nil
    }

    private static func downloadedItemDirectory(workshopId: String, cmdPath: String) -> URL? {
        let cmdURL = URL(fileURLWithPath: cmdPath)
        let possibleSteamAppsDirectories = [
            cmdURL.deletingLastPathComponent().appending(path: "steamapps"),
            FileManager.default.homeDirectoryForCurrentUser
                .appending(path: "Library/Application Support/Steam/steamapps"),
            FileManager.default.homeDirectoryForCurrentUser
                .appending(path: "Steam/steamapps"),
        ]

        return possibleSteamAppsDirectories.lazy
            .map {
                $0.appending(
                    path: "workshop/content/431960/\(workshopId)",
                    directoryHint: .isDirectory
                )
            }
            .first(where: { isValidWallpaperPackage(at: $0) })
    }

    private static func installDownloadedItem(
        from sourceDirectory: URL,
        to destinationDirectory: URL,
        workshopId: String
    ) throws {
        let fileManager = FileManager.default
        guard isValidWallpaperPackage(at: sourceDirectory, fileManager: fileManager) else {
            throw DownloadError.invalidPackage(sourceDirectory.path)
        }

        if fileManager.fileExists(atPath: destinationDirectory.path) {
            guard isValidWallpaperPackage(at: destinationDirectory, fileManager: fileManager) else {
                throw DownloadError.invalidExistingDestination(destinationDirectory.path)
            }
            return
        }

        let temporaryDirectory = destinationDirectory
            .deletingLastPathComponent()
            .appending(
                path: ".\(workshopId).import-\(UUID().uuidString)",
                directoryHint: .isDirectory
            )

        do {
            try fileManager.copyItem(at: sourceDirectory, to: temporaryDirectory)
            guard isValidWallpaperPackage(at: temporaryDirectory, fileManager: fileManager) else {
                throw DownloadError.invalidPackage(temporaryDirectory.path)
            }
            try fileManager.moveItem(at: temporaryDirectory, to: destinationDirectory)
        } catch {
            if fileManager.fileExists(atPath: temporaryDirectory.path) {
                do {
                    try fileManager.removeItem(at: temporaryDirectory)
                } catch let cleanupError {
                    print(
                        "Failed to clean temporary Workshop import at " +
                            "\(temporaryDirectory.path): \(cleanupError)"
                    )
                }
            }
            throw error
        }
    }

    private static func commandFailure(output: String, exitCode: Int32) -> String? {
        let errorLine = output.components(separatedBy: .newlines)
            .map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }
            .first {
                $0.range(of: "error", options: .caseInsensitive) != nil ||
                    $0.range(of: "failed", options: .caseInsensitive) != nil
            }
        if let errorLine, !errorLine.isEmpty {
            return errorLine
        }
        if exitCode != 0 {
            return "steamcmd exited with code \(exitCode)"
        }
        return nil
    }

    private enum DownloadError: LocalizedError {
        case invalidPackage(String)
        case invalidExistingDestination(String)

        var errorDescription: String? {
            switch self {
            case .invalidPackage(let path):
                return "Downloaded wallpaper package is invalid: \(path)"
            case .invalidExistingDestination(let path):
                return "An invalid item already exists at the destination: \(path)"
            }
        }
    }
}

struct WorkshopByteProgress: Equatable, Sendable {
    let downloadedBytes: UInt64
    let totalBytes: UInt64
}

enum NetworkByteSampleParser {
    static func receivedBytes(from line: String) -> UInt64? {
        let fields = line.split(separator: ",", omittingEmptySubsequences: false)
        guard fields.count > 1 else { return nil }
        return UInt64(fields[1].trimmingCharacters(in: .whitespacesAndNewlines))
    }
}

private final class ProcessTreeNetworkMonitor: @unchecked Sendable {
    private let rootProcessIdentifier: pid_t
    private let lock = NSLock()
    private var monitorProcess: Process?
    private var pendingOutput = ""
    private var receivedBytes: UInt64?

    init(rootProcessIdentifier: pid_t) {
        self.rootProcessIdentifier = rootProcessIdentifier
    }

    func startIfPossible() {
        lock.lock()
        let hasStarted = monitorProcess != nil
        lock.unlock()
        guard !hasStarted else { return }

        let processIdentifiers = Self.processTree(root: rootProcessIdentifier)
        guard let networkProcessIdentifier = processIdentifiers
            .dropFirst()
            .first(where: { Self.processName(for: $0) == "steamcmd" }) else {
            return
        }

        let process = Process()
        let outputPipe = Pipe()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/nettop")
        process.arguments = [
            "-P", "-L", "0", "-x", "-n",
            "-p", String(networkProcessIdentifier),
            "-s", "1",
            "-J", "bytes_in",
        ]
        process.standardOutput = outputPipe
        process.standardError = FileHandle.nullDevice

        outputPipe.fileHandleForReading.readabilityHandler = { [weak self] handle in
            let data = handle.availableData
            guard !data.isEmpty,
                  let output = String(data: data, encoding: .utf8) else {
                return
            }
            self?.consume(output)
        }

        do {
            try process.run()
            lock.lock()
            monitorProcess = process
            lock.unlock()
        } catch {
            outputPipe.fileHandleForReading.readabilityHandler = nil
            print("Failed to start network progress monitor: \(error)")
        }
    }

    func readReceivedBytes() -> UInt64? {
        lock.lock()
        let snapshot = receivedBytes
        lock.unlock()
        return snapshot
    }

    func stop() {
        lock.lock()
        let process = monitorProcess
        monitorProcess = nil
        lock.unlock()

        guard let process else { return }
        if process.isRunning {
            process.terminate()
            process.waitUntilExit()
        }
        (process.standardOutput as? Pipe)?.fileHandleForReading.readabilityHandler = nil
    }

    private func consume(_ output: String) {
        lock.lock()
        pendingOutput += output
        while let newline = pendingOutput.firstIndex(of: "\n") {
            let line = String(pendingOutput[..<newline])
            pendingOutput.removeSubrange(...newline)
            if let sample = NetworkByteSampleParser.receivedBytes(from: line) {
                receivedBytes = max(receivedBytes ?? 0, sample)
            }
        }
        lock.unlock()
    }

    private static func processTree(root: pid_t) -> [pid_t] {
        var result: [pid_t] = []
        var pending = [root]
        var seen: Set<pid_t> = []

        while let processIdentifier = pending.popLast() {
            guard seen.insert(processIdentifier).inserted else { continue }
            result.append(processIdentifier)
            pending.append(contentsOf: childProcessIdentifiers(of: processIdentifier))
        }
        return result
    }

    private static func childProcessIdentifiers(of parent: pid_t) -> [pid_t] {
        var capacity = 16
        while capacity <= 256 {
            var children = [pid_t](repeating: 0, count: capacity)
            let count = children.withUnsafeMutableBytes { buffer in
                proc_listchildpids(parent, buffer.baseAddress, Int32(buffer.count))
            }
            guard count > 0 else { return [] }
            if count < capacity {
                return Array(children.prefix(Int(count)))
            }
            capacity *= 2
        }
        return []
    }

    private static func processName(for processIdentifier: pid_t) -> String? {
        var buffer = [CChar](repeating: 0, count: 256)
        let length = buffer.withUnsafeMutableBytes { bytes in
            proc_name(processIdentifier, bytes.baseAddress, UInt32(bytes.count))
        }
        guard length > 0 else { return nil }
        return String(cString: buffer)
    }
}

struct WorkshopContentLogProgressEstimator {
    private static let maximumRateAge: TimeInterval = 65
    private static let megabitsToBytes = 125_000.0

    private var downloadedBytes: Double?
    private var totalBytes: UInt64?
    private var bytesPerSecond = 0.0
    private var lastIntegratedAt: Date?
    private var rateObservedAt: Date?

    private let timestampFormatter: DateFormatter = {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.calendar = Calendar(identifier: .gregorian)
        formatter.timeZone = .current
        formatter.dateFormat = "yyyy-MM-dd HH:mm:ss"
        return formatter
    }()

    mutating func consume(_ output: String, observedAt: Date) {
        let records = output.split(whereSeparator: { $0 == "\n" || $0 == "\r" })

        for record in records {
            let line = String(record)
            let eventDate = timestamp(in: line) ?? observedAt
            advance(to: eventDate)

            if let baseline = WorkshopDownloadCoordinator.parseContentLogByteProgress(line) {
                downloadedBytes = Double(baseline.downloadedBytes)
                totalBytes = baseline.totalBytes
                bytesPerSecond = 0
                rateObservedAt = nil
                lastIntegratedAt = eventDate
                continue
            }

            if downloadedBytes != nil,
               let rate = WorkshopDownloadCoordinator.parseContentLogTransferRateMbps(line) {
                bytesPerSecond = rate * Self.megabitsToBytes
                rateObservedAt = eventDate
                lastIntegratedAt = eventDate
                continue
            }

            let normalizedLine = line.lowercased()
            if normalizedLine.contains("appid 431960"),
               normalizedLine.contains("committing"),
               let totalBytes {
                downloadedBytes = Double(totalBytes)
                bytesPerSecond = 0
                rateObservedAt = nil
                lastIntegratedAt = eventDate
            }
        }

        advance(to: observedAt)
    }

    mutating func progress(at date: Date) -> WorkshopByteProgress? {
        advance(to: date)
        guard let downloadedBytes, let totalBytes, totalBytes > 0 else {
            return nil
        }

        return WorkshopByteProgress(
            downloadedBytes: min(UInt64(downloadedBytes.rounded(.down)), totalBytes),
            totalBytes: totalBytes
        )
    }

    private mutating func advance(to date: Date) {
        guard let lastIntegratedAt else {
            self.lastIntegratedAt = date
            return
        }
        guard date > lastIntegratedAt else { return }
        guard var downloadedBytes, let totalBytes else {
            self.lastIntegratedAt = date
            return
        }

        if bytesPerSecond > 0, let rateObservedAt {
            let rateExpiresAt = rateObservedAt.addingTimeInterval(Self.maximumRateAge)
            let effectiveEnd = min(date, rateExpiresAt)
            if effectiveEnd > lastIntegratedAt {
                downloadedBytes += bytesPerSecond * effectiveEnd.timeIntervalSince(lastIntegratedAt)
                self.downloadedBytes = min(downloadedBytes, Double(totalBytes))
            }
            if date >= rateExpiresAt {
                bytesPerSecond = 0
                self.rateObservedAt = nil
            }
        }

        self.lastIntegratedAt = date
    }

    private func timestamp(in line: String) -> Date? {
        guard line.first == "[",
              let closingBracket = line.firstIndex(of: "]") else {
            return nil
        }
        let start = line.index(after: line.startIndex)
        return timestampFormatter.date(from: String(line[start..<closingBracket]))
    }
}

private final class WorkshopProgressReporter: @unchecked Sendable {
    private let lock = NSLock()
    private let onStateChange: @Sendable (SteamCmdService.DownloadState) -> Void
    private var progress: Double
    private var status: String
    private var lastPublishedProgress = -1.0
    private var lastPublishedStatus = ""

    init(
        initialProgress: Double,
        initialStatus: String,
        onStateChange: @escaping @Sendable (SteamCmdService.DownloadState) -> Void
    ) {
        progress = initialProgress
        status = initialStatus
        self.onStateChange = onStateChange
    }

    func publishCurrentState() {
        publish(.init(progress: progress, status: status))
    }

    func publish(_ update: SteamCmdService.DownloadProgressUpdate) {
        lock.lock()
        if let updatedProgress = update.progress {
            progress = max(progress, min(max(updatedProgress, 0), 1))
        }
        status = update.status

        let shouldPublish = status != lastPublishedStatus ||
            abs(progress - lastPublishedProgress) >= 0.001 ||
            progress == 1
        let state = SteamCmdService.DownloadState.downloading(
            progress: progress,
            status: status
        )
        if shouldPublish {
            lastPublishedProgress = progress
            lastPublishedStatus = status
        }
        lock.unlock()

        if shouldPublish {
            onStateChange(state)
        }
    }
}

private final class SteamContentLogMonitor {
    private var offsets: [URL: UInt64]
    private var pendingOutput: [URL: String] = [:]
    private var estimators: [URL: WorkshopContentLogProgressEstimator]

    init(cmdPath: String, fileManager: FileManager = .default) {
        let homeDirectory = fileManager.homeDirectoryForCurrentUser
        let resolvedCmdDirectory = URL(fileURLWithPath: cmdPath)
            .resolvingSymlinksInPath()
            .deletingLastPathComponent()
        let candidates = [
            homeDirectory.appending(path: "Library/Application Support/Steam/logs/content_log.txt"),
            resolvedCmdDirectory.appending(path: "logs/content_log.txt"),
        ]

        let uniqueCandidates = Set(candidates)
        offsets = Dictionary(uniqueKeysWithValues: uniqueCandidates.map { url in
            (url, Self.fileSize(at: url, fileManager: fileManager))
        })
        estimators = Dictionary(uniqueKeysWithValues: uniqueCandidates.map { url in
            (url, WorkshopContentLogProgressEstimator())
        })
    }

    func readProgress(fileManager: FileManager = .default) -> WorkshopByteProgress? {
        let observedAt = Date()

        for url in Array(offsets.keys) {
            let previousOffset = offsets[url] ?? 0
            let currentSize = Self.fileSize(at: url, fileManager: fileManager)
            let readOffset = currentSize < previousOffset ? 0 : previousOffset
            if currentSize < previousOffset {
                pendingOutput[url] = ""
            }
            guard currentSize > readOffset,
                  let handle = try? FileHandle(forReadingFrom: url) else {
                offsets[url] = currentSize
                continue
            }

            do {
                try handle.seek(toOffset: readOffset)
                let data = try handle.readToEnd() ?? Data()
                if let output = String(data: data, encoding: .utf8) {
                    let combinedOutput = (pendingOutput[url] ?? "") + output
                    if let lastNewline = combinedOutput.lastIndex(of: "\n") {
                        let completeOutput = String(combinedOutput[...lastNewline])
                        pendingOutput[url] = String(
                            combinedOutput[combinedOutput.index(after: lastNewline)...]
                        )
                        var estimator = estimators[url] ?? WorkshopContentLogProgressEstimator()
                        estimator.consume(completeOutput, observedAt: observedAt)
                        estimators[url] = estimator
                    } else {
                        pendingOutput[url] = combinedOutput
                    }
                }
            } catch {
                print("Failed to read Steam content log at \(url.path): \(error)")
            }
            try? handle.close()
            offsets[url] = currentSize
        }

        var mostAdvancedProgress: WorkshopByteProgress?
        for url in Array(estimators.keys) {
            guard var estimator = estimators[url] else { continue }
            let progress = estimator.progress(at: observedAt)
            estimators[url] = estimator

            guard let progress else { continue }
            guard let current = mostAdvancedProgress else {
                mostAdvancedProgress = progress
                continue
            }
            let currentFraction = Double(current.downloadedBytes) / Double(current.totalBytes)
            let candidateFraction = Double(progress.downloadedBytes) / Double(progress.totalBytes)
            if candidateFraction > currentFraction {
                mostAdvancedProgress = progress
            }
        }
        return mostAdvancedProgress
    }

    private static func fileSize(at url: URL, fileManager: FileManager) -> UInt64 {
        guard let attributes = try? fileManager.attributesOfItem(atPath: url.path),
              let size = attributes[.size] as? NSNumber else {
            return 0
        }
        return size.uint64Value
    }
}

private final class WorkshopOutputBuffer: @unchecked Sendable {
    private let lock = NSLock()
    private var output = ""

    func append(_ newOutput: String) {
        lock.lock()
        output += newOutput
        lock.unlock()
    }

    func stringValue() -> String {
        lock.lock()
        let snapshot = output
        lock.unlock()
        return snapshot
    }
}
