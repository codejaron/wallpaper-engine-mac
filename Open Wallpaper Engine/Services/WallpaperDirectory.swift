import Foundation

enum WallpaperDirectory {
    static func defaultURL(using fileManager: FileManager) -> URL {
        fileManager.urls(for: .documentDirectory, in: .userDomainMask)[0]
            .appending(path: "Open Wallpaper Engine", directoryHint: .isDirectory)
    }

    static func url(
        configuredPath: String?,
        using fileManager: FileManager
    ) -> URL {
        let path = configuredPath?.trimmingCharacters(in: .whitespacesAndNewlines)
        let directory = path.flatMap { $0.isEmpty ? nil : $0 }
            .map { URL(fileURLWithPath: $0, isDirectory: true) }
            ?? defaultURL(using: fileManager)
        let resolvedDirectory = directory.standardizedFileURL.resolvingSymlinksInPath()

        do {
            try fileManager.createDirectory(
                at: resolvedDirectory,
                withIntermediateDirectories: true
            )
        } catch {
            fatalError(
                "Unable to create wallpaper storage directory at " +
                    "\(resolvedDirectory.path): \(error.localizedDescription)"
            )
        }

        guard fileManager.isWritableFile(atPath: resolvedDirectory.path) else {
            fatalError(
                "Wallpaper storage directory is not writable: " +
                    resolvedDirectory.path
            )
        }
        return resolvedDirectory
    }
}

extension FileManager {
    /// The dedicated directory for storing wallpaper packages.
    /// Uses the configured storage directory, or `~/Documents/Open Wallpaper Engine/`
    /// when the user has not selected one. The directory is created if missing.
    @MainActor var wallpapersDirectory: URL {
        WallpaperDirectory.url(
            configuredPath: AppDelegate.shared.globalSettingsViewModel.settings
                .wallpaperStorageDirectory,
            using: self
        )
    }
}
