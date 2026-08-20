import Foundation
import Combine

@MainActor
final class SteamCmdService: ObservableObject {
    @Published var steamCmdPath: String?
    @Published var isLoggedIn = false
    @Published var steamUsername: String = ""
    @Published var loginError: String?
    @Published var isLoggingIn = false
    @Published private(set) var downloadJobs: [String: DownloadJob] = [:]

    enum DownloadState: Equatable, Sendable {
        case queued
        case downloading(progress: Double?, status: String)
        case completed
        case failed(String)
    }

    struct DownloadProgressUpdate: Equatable, Sendable {
        let progress: Double?
        let status: String
    }

    struct DownloadJob: Identifiable, Equatable, Sendable {
        let item: WorkshopItem
        let order: Int
        var state: DownloadState

        var id: String { item.id }
    }

    enum LoginResult: Equatable, Sendable {
        case success
        case steamGuardRequired
        case invalidCredentials
        case timedOut
        case failed

        var errorMessage: String? {
            switch self {
            case .success:
                return nil
            case .steamGuardRequired:
                return "Steam Guard code required"
            case .invalidCredentials:
                return "Invalid username or password"
            case .timedOut:
                return "Login timed out. Check Steam Mobile for an approval request, then try again."
            case .failed:
                return "Login failed. Check credentials and try again."
            }
        }
    }

    private struct CommandResult: Sendable {
        let output: String
        let exitCode: Int32
        let didTimeOut: Bool
    }

    private static let lastUsernameKey = "SteamLastUsername"
    private let workshopDownloader = WorkshopDownloadCoordinator()
    private var nextDownloadOrder = 0

    init() {
        detectSteamCmd()
        attemptCachedLogin()
    }

    /// Run a steamcmd process with proper pipe handling to avoid deadlocks.
    /// Reads stdout/stderr concurrently with process execution and applies a timeout.
    nonisolated private static func runSteamCmd(
        executablePath: String,
        arguments: [String],
        timeout: TimeInterval = 30
    ) -> CommandResult {
        let process = Process()
        let outputPipe = Pipe()
        process.executableURL = URL(fileURLWithPath: executablePath)
        process.arguments = arguments
        process.standardOutput = outputPipe
        process.standardError = outputPipe

        // Read pipe concurrently to prevent buffer deadlock
        let outputBuffer = SteamCmdOutputBuffer()
        let handle = outputPipe.fileHandleForReading
        handle.readabilityHandler = { fileHandle in
            let data = fileHandle.availableData
            if !data.isEmpty {
                outputBuffer.append(data)
            }
        }

        do {
            try process.run()
        } catch {
            handle.readabilityHandler = nil
            return CommandResult(
                output: "Failed to run steamcmd: \(error.localizedDescription)",
                exitCode: -1,
                didTimeOut: false
            )
        }

        // Wait with timeout
        let deadline = DispatchTime.now() + timeout
        let waitGroup = DispatchGroup()
        waitGroup.enter()
        DispatchQueue.global().async {
            process.waitUntilExit()
            waitGroup.leave()
        }

        if waitGroup.wait(timeout: deadline) == .timedOut {
            process.terminate()
            handle.readabilityHandler = nil
            let output = outputBuffer.stringValue()
            return CommandResult(output: output, exitCode: -1, didTimeOut: true)
        }

        handle.readabilityHandler = nil
        // Read any remaining data
        let remaining = handle.readDataToEndOfFile()
        outputBuffer.append(remaining)

        let output = outputBuffer.stringValue()
        return CommandResult(
            output: output,
            exitCode: process.terminationStatus,
            didTimeOut: false
        )
    }

    nonisolated static func loginResult(
        output: String,
        exitCode: Int32,
        didTimeOut: Bool
    ) -> LoginResult {
        if didTimeOut {
            return .timedOut
        }
        if output.contains("Logged in OK") || (output.contains("OK") && exitCode == 0) {
            return .success
        }
        if output.contains("Steam Guard") || output.contains("Two-factor") {
            return .steamGuardRequired
        }
        if output.contains("Invalid Password") || output.contains("FAILED") {
            return .invalidCredentials
        }
        return .failed
    }

    /// Automatically try cached session if we have a saved username and steamcmd is installed.
    private func attemptCachedLogin() {
        guard isInstalled, !isLoggedIn else { return }
        if let saved = UserDefaults.standard.string(forKey: Self.lastUsernameKey), !saved.isEmpty {
            loginWithCachedSession(username: saved)
        }
    }

    func detectSteamCmd() {
        // Check user-configured path first
        if let customPath = UserDefaults.standard.string(forKey: "SteamCmdPath"),
           FileManager.default.isExecutableFile(atPath: customPath) {
            steamCmdPath = customPath
            return
        }

        let homeDir = FileManager.default.homeDirectoryForCurrentUser.path
        let searchPaths = [
            // Homebrew / system installs
            "/usr/local/bin/steamcmd",
            "/opt/homebrew/bin/steamcmd",
            "/usr/bin/steamcmd",
            // Steam client / SDK locations
            "\(homeDir)/Library/Application Support/Steam/steamcmd",
            "\(homeDir)/Library/Application Support/Steam/steamcmd/steamcmd",
            "\(homeDir)/Library/Application Support/Steam/steamcmd.sh",
            // Standalone SteamCMD package (common extract locations)
            "\(homeDir)/steamcmd/steamcmd.sh",
            "\(homeDir)/steamcmd/steamcmd",
            "\(homeDir)/Downloads/steamcmd/steamcmd.sh",
            "\(homeDir)/Downloads/steamcmd/steamcmd",
            "/Applications/steamcmd/steamcmd.sh",
            "/Applications/steamcmd/steamcmd",
            "\(homeDir)/Projects/SteamSDK/tools/ContentBuilder/builder_osx/steamcmd",
        ]

        for path in searchPaths {
            if FileManager.default.fileExists(atPath: path) {
                steamCmdPath = path
                return
            }
        }

        // Try `which` as fallback — run on background thread to avoid blocking main thread
        DispatchQueue.global(qos: .utility).async { [weak self] in
            let process = Process()
            let pipe = Pipe()
            process.executableURL = URL(fileURLWithPath: "/usr/bin/which")
            process.arguments = ["steamcmd"]
            process.standardOutput = pipe
            process.standardError = FileHandle.nullDevice
            try? process.run()
            process.waitUntilExit()

            if process.terminationStatus == 0 {
                let data = pipe.fileHandleForReading.readDataToEndOfFile()
                if let path = String(data: data, encoding: .utf8)?.trimmingCharacters(in: .whitespacesAndNewlines),
                   !path.isEmpty {
                    Task { @MainActor [weak self] in
                        self?.steamCmdPath = path
                    }
                }
            }
        }
    }

    var isInstalled: Bool { steamCmdPath != nil }

    @Published var pathError: String?

    func setCustomPath(_ path: String) {
        guard FileManager.default.fileExists(atPath: path) else {
            pathError = "File not found at selected path."
            return
        }
        // Make executable if needed (e.g. steamcmd.sh from Steam package)
        if !FileManager.default.isExecutableFile(atPath: path) {
            try? FileManager.default.setAttributes(
                [.posixPermissions: 0o755], ofItemAtPath: path
            )
        }
        pathError = nil
        UserDefaults.standard.set(path, forKey: "SteamCmdPath")
        steamCmdPath = path
    }

    /// Attempt login with username and password. Steam Guard code is optional.
    func login(username: String, password: String, guardCode: String? = nil) {
        guard let cmdPath = steamCmdPath else { return }

        isLoggingIn = true
        loginError = nil
        steamUsername = username

        DispatchQueue.global(qos: .userInitiated).async {
            var args = ["+login", username, password]
            if let code = guardCode, !code.isEmpty {
                args = ["+login", username, password, code]
            }
            args += ["+quit"]

            let commandResult = Self.runSteamCmd(
                executablePath: cmdPath,
                arguments: args,
                timeout: 60
            )
            let loginResult = Self.loginResult(
                output: commandResult.output,
                exitCode: commandResult.exitCode,
                didTimeOut: commandResult.didTimeOut
            )

            Task { @MainActor [weak self] in
                guard let self else { return }
                self.isLoggingIn = false
                if loginResult == .success {
                    self.isLoggedIn = true
                    self.loginError = nil
                    UserDefaults.standard.set(username, forKey: Self.lastUsernameKey)
                } else {
                    self.loginError = loginResult.errorMessage
                }
            }
        }
    }

    /// Try login with cached session (no password needed if previously authenticated).
    func loginWithCachedSession(username: String) {
        guard let cmdPath = steamCmdPath else { return }

        isLoggingIn = true
        loginError = nil
        steamUsername = username

        DispatchQueue.global(qos: .userInitiated).async {
            let commandResult = Self.runSteamCmd(
                executablePath: cmdPath,
                arguments: ["+login", username, "+quit"],
                timeout: 30
            )
            let loginResult = Self.loginResult(
                output: commandResult.output,
                exitCode: commandResult.exitCode,
                didTimeOut: commandResult.didTimeOut
            )

            Task { @MainActor [weak self] in
                guard let self else { return }
                self.isLoggingIn = false
                if loginResult == .success {
                    self.isLoggedIn = true
                    UserDefaults.standard.set(username, forKey: Self.lastUsernameKey)
                } else if loginResult == .timedOut {
                    self.loginError = loginResult.errorMessage
                } else {
                    self.loginError = "Cached session expired. Please log in with password."
                }
            }
        }
    }

    /// Download a workshop item by its ID. SteamCMD shares mutable state, so all
    /// downloads run on one serial queue instead of launching competing processes.
    @MainActor
    func downloadWorkshopItem(item: WorkshopItem) {
        let workshopId = item.id
        guard !workshopId.isEmpty, WorkshopId(rawValue: workshopId) != nil else {
            setDownloadState(.failed("Invalid Workshop item ID"), for: item)
            return
        }

        let destinationDirectory = FileManager.default.wallpapersDirectory
            .appending(path: workshopId, directoryHint: .isDirectory)

        if Self.isValidWallpaperPackage(at: destinationDirectory) {
            setDownloadState(.completed, for: item)
            return
        }

        if let state = downloadJobs[workshopId]?.state {
            switch state {
            case .queued, .downloading:
                return
            case .completed, .failed:
                break
            }
        }

        guard let cmdPath = steamCmdPath else {
            setDownloadState(.failed("steamcmd is not installed"), for: item)
            return
        }
        guard isLoggedIn else {
            setDownloadState(.failed("Steam login is required"), for: item)
            return
        }

        let username = steamUsername
        setDownloadState(.queued, for: item)
        workshopDownloader.enqueue(
            workshopId: workshopId,
            cmdPath: cmdPath,
            username: username,
            expectedDownloadBytes: Int64(item.fileSize),
            destinationDirectory: destinationDirectory
        ) { [weak self] state in
            Task { @MainActor [weak self] in
                self?.setDownloadState(state, for: item)
            }
        }
    }

    @MainActor
    func downloadState(for workshopId: String) -> DownloadState? {
        Self.resolvedDownloadState(
            for: workshopId,
            activeState: downloadJobs[workshopId]?.state,
            libraryDirectory: FileManager.default.wallpapersDirectory
        )
    }

    nonisolated static func parseDownloadProgress(_ output: String) -> DownloadProgressUpdate? {
        WorkshopDownloadCoordinator.parseProgress(output)
    }

    nonisolated static func resolvedDownloadState(
        for workshopId: String,
        activeState: DownloadState?,
        libraryDirectory: URL
    ) -> DownloadState? {
        let destinationDirectory = libraryDirectory
            .appending(path: workshopId, directoryHint: .isDirectory)
        if isValidWallpaperPackage(at: destinationDirectory) {
            return .completed
        }
        if case .completed? = activeState {
            return nil
        }
        return activeState
    }

    nonisolated static func isValidWallpaperPackage(
        at directory: URL,
        fileManager: FileManager = .default
    ) -> Bool {
        WorkshopDownloadCoordinator.isValidWallpaperPackage(
            at: directory,
            fileManager: fileManager
        )
    }

    private func setDownloadState(_ state: DownloadState, for item: WorkshopItem) {
        if var job = downloadJobs[item.id] {
            job.state = state
            downloadJobs[item.id] = job
        } else {
            downloadJobs[item.id] = DownloadJob(
                item: item,
                order: nextDownloadOrder,
                state: state
            )
            nextDownloadOrder += 1
        }
    }
}

private final class SteamCmdOutputBuffer: @unchecked Sendable {
    private let lock = NSLock()
    private var data = Data()

    func append(_ newData: Data) {
        lock.lock()
        data.append(newData)
        lock.unlock()
    }

    func stringValue() -> String {
        lock.lock()
        let snapshot = data
        lock.unlock()
        return String(data: snapshot, encoding: .utf8) ?? ""
    }
}
