import XCTest
@testable import Open_Wallpaper_Engine

final class SteamCmdServiceTests: XCTestCase {
    func testTimedOutLoginDirectsUserToSteamMobileApproval() {
        let result = SteamCmdService.loginResult(
            output: "",
            exitCode: -1,
            didTimeOut: true
        )

        XCTAssertEqual(result, .timedOut)
        XCTAssertEqual(
            result.errorMessage,
            "Login timed out. Check Steam Mobile for an approval request, then try again."
        )
    }

    func testDownloadProgressUsesLatestPercentageFromSteamCmdChunk() throws {
        let update = SteamCmdService.parseDownloadProgress(
            "Update state (0x61) downloading, progress: 12.50\r" +
                "Update state (0x61) downloading, progress: 47.25\r"
        )

        let progress = try XCTUnwrap(update?.progress)
        XCTAssertEqual(progress, 0.4725, accuracy: 0.0001)
        XCTAssertEqual(update?.status, "Downloading...")
    }

    func testDownloadProgressRecognizesCarriageReturnDelimitedStages() {
        let update = SteamCmdService.parseDownloadProgress(
            "Logging in user...\rDownloading item test-item...\r" +
                "Success. Downloaded item test-item."
        )

        XCTAssertEqual(update?.progress, 1)
        XCTAssertEqual(update?.status, "Download complete, importing...")
    }

    func testDownloadProgressUsesLatestStageAfterPercentage() {
        let update = SteamCmdService.parseDownloadProgress(
            "Update state (0x61) downloading, progress: 100.00\r" +
                "Update state (0x5) validating"
        )

        XCTAssertNil(update?.progress)
        XCTAssertEqual(update?.status, "Validating...")
    }

    func testContentLogProgressUsesLatestDownloadBaseline() throws {
        let progress = try XCTUnwrap(
            WorkshopDownloadCoordinator.parseContentLogByteProgress(
                "AppID 431960 update started : download 0/1000000, store 0/0\n" +
                    "AppID 431960 update started : download 250000/1000000, store 0/0"
            )
        )

        XCTAssertEqual(progress.downloadedBytes, 250_000)
        XCTAssertEqual(progress.totalBytes, 1_000_000)
    }

    func testContentLogProgressRejectsMissingOrZeroTotal() {
        XCTAssertNil(
            WorkshopDownloadCoordinator.parseContentLogByteProgress(
                "Current download rate: 24.000 Mbps"
            )
        )
        XCTAssertNil(
            WorkshopDownloadCoordinator.parseContentLogByteProgress(
                "update started : download 0/0"
            )
        )
    }

    func testContentLogTransferRateParsesObservedSteamFormats() throws {
        let connectionRate = try XCTUnwrap(
            WorkshopDownloadCoordinator.parseContentLogTransferRateMbps(
                "Increasing target number of download connections to 9 " +
                    "(rate was 0.000, now 8.000)"
            )
        )
        let periodicRate = try XCTUnwrap(
            WorkshopDownloadCoordinator.parseContentLogTransferRateMbps(
                "Current download rate: 24.000 Mbps"
            )
        )

        XCTAssertEqual(connectionRate, 8.000, accuracy: 0.0001)
        XCTAssertEqual(periodicRate, 24.000, accuracy: 0.0001)
    }

    func testContentLogEstimatorAdvancesBetweenObservedRateUpdates() throws {
        var calendar = Calendar(identifier: .gregorian)
        calendar.timeZone = .current
        let start = try XCTUnwrap(
            calendar.date(
                from: DateComponents(
                    year: 2025,
                    month: 1,
                    day: 2,
                    hour: 3,
                    minute: 4,
                    second: 5
                )
            )
        )
        var estimator = WorkshopContentLogProgressEstimator()
        estimator.consume(
            "[2025-01-02 03:04:05] AppID 431960 update started : " +
                "download 0/8000000, store 0/0\n" +
                "[2025-01-02 03:04:10] Increasing target number of download " +
                "connections to 9 (rate was 0.000, now 8.000)\n",
            observedAt: start.addingTimeInterval(10)
        )

        let progress = try XCTUnwrap(
            estimator.progress(at: start.addingTimeInterval(10))
        )
        XCTAssertEqual(progress.downloadedBytes, 5_000_000)
        XCTAssertEqual(progress.totalBytes, 8_000_000)
    }

    func testContentLogEstimatorReachesTotalWhenSteamStartsCommit() throws {
        var estimator = WorkshopContentLogProgressEstimator()
        let now = Date()
        estimator.consume(
            "AppID 431960 update started : download 0/8000000, store 0/0\n" +
                "AppID 431960 Workshop update changed : Running Update,Committing,\n",
            observedAt: now
        )

        XCTAssertEqual(
            estimator.progress(at: now),
            WorkshopByteProgress(downloadedBytes: 8_000_000, totalBytes: 8_000_000)
        )
    }

    func testNetworkByteSampleParserReadsProcessSummary() {
        XCTAssertEqual(
            NetworkByteSampleParser.receivedBytes(from: "transfer.42,5242880,"),
            5_242_880
        )
        XCTAssertNil(NetworkByteSampleParser.receivedBytes(from: ",bytes_in,"))
    }

    func testValidWallpaperPackageRequiresDecodableProjectFile() throws {
        let fileManager = FileManager.default
        let root = fileManager.temporaryDirectory
            .appending(path: UUID().uuidString, directoryHint: .isDirectory)
        try fileManager.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? fileManager.removeItem(at: root) }

        XCTAssertFalse(SteamCmdService.isValidWallpaperPackage(at: root))

        let project = WEProject(
            file: "wallpaper.mp4",
            preview: "preview.jpg",
            title: "Downloaded Workshop Item",
            type: "video"
        )
        try JSONEncoder().encode(project)
            .write(to: root.appending(path: "project.json"), options: .atomic)

        XCTAssertTrue(SteamCmdService.isValidWallpaperPackage(at: root))
    }

    func testResolvedDownloadStateUsesInstalledPackageAfterRelaunch() throws {
        let fileManager = FileManager.default
        let library = fileManager.temporaryDirectory
            .appending(path: UUID().uuidString, directoryHint: .isDirectory)
        let workshopId = "test-item"
        let itemDirectory = library
            .appending(path: workshopId, directoryHint: .isDirectory)
        try fileManager.createDirectory(at: itemDirectory, withIntermediateDirectories: true)
        defer { try? fileManager.removeItem(at: library) }

        let project = WEProject(
            file: "wallpaper.mp4",
            preview: "preview.jpg",
            title: "Previously Downloaded Item",
            type: "video"
        )
        try JSONEncoder().encode(project)
            .write(to: itemDirectory.appending(path: "project.json"), options: .atomic)

        XCTAssertEqual(
            SteamCmdService.resolvedDownloadState(
                for: workshopId,
                activeState: nil,
                libraryDirectory: library
            ),
            .completed
        )
    }
}
