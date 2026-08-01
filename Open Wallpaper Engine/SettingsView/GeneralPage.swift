//
//  GeneralPage.swift
//  Open Wallpaper Engine
//
//  Created by Haren on 2023/8/12.
//

import AppKit
import SwiftUI

struct GeneralPage: SettingsPage {
    @ObservedObject var viewModel: GlobalSettingsViewModel
    @ObservedObject private var mediaSnapshotProvider: SceneMediaSnapshotProvider
    @State private var assetsDirectoryIssue: String?
    @State private var wallpaperStorageDirectoryIssue: String?
    
    init(globalSettings viewModel: GlobalSettingsViewModel) {
        self.viewModel = viewModel
        mediaSnapshotProvider = AppDelegate.shared.sceneMediaSnapshotProvider
    }
    
    var body: some View {
        Form {
            // MARK: Automatic Startup
            Section {
                Toggle("Start with macOS", isOn: $viewModel.settings.autoStart)
//                Toggle("Safe start after hibernation", isOn: $viewModel.settings.safeMode)
            } header: {
                Label("Automatic Startup", systemImage: "star.fill")
            }
            // MARK: Basic Setup
            Section {
                Picker("Language", selection: $viewModel.settings.language) {
                    Text("Follow System").tag(GSLocalization.followSystem)
                    Text("English").tag(GSLocalization.en_US)
                    Text("Chinese Simplified").tag(GSLocalization.zh_CN)
                }.disabled(true)
            } header: {
                Label("Basic Setup", systemImage: "gearshape.fill")
            }
            // MARK: macOS
            Section {
                Toggle("Adjust Menu Bar Color", isOn: $viewModel.settings.adjustMenuBarTint)
            } header: {
                Label("macOS", systemImage: "apple.logo")
            }
            // MARK: Appearance
            Section {
                Picker("Theme", selection: $viewModel.settings.appearance) {
                    Text("Light").tag(GSAppearance.light)
                    Text("Dark").tag(GSAppearance.dark)
                    Text("Auto").tag(GSAppearance.followSystem)
                }
            } header: {
                Label("Appearance", systemImage: "paintpalette.fill")
            }
            // MARK: Audio
            Section {
                Toggle(isOn: $viewModel.settings.audioOutput) {
                    Text("Audio Output")
                }
                Toggle(
                    "System Audio Capture",
                    isOn: $viewModel.settings.systemAudioCaptureEnabled
                )
                .help(
                    "Enables audio-reactive wallpaper effects and the Other Application Playing Audio rule."
                )
                if let issue = mediaSnapshotProvider.inputIssue {
                    Label(issue, systemImage: "exclamationmark.triangle.fill")
                        .font(.footnote)
                        .foregroundStyle(.red)
                        .fixedSize(horizontal: false, vertical: true)
                }
                Toggle(isOn: $viewModel.settings.reloadWhenChangingOutputDevice) {
                    Text("Reload when changing output device")
                }.disabled(true)
            } header: {
                Label("Audio", systemImage: "speaker.3.fill")
            }
            // MARK: Video
            Section {
                Picker("Video Framework", selection: $viewModel.settings.videoFramework) {
                    Text("Apple AVKit").tag(GSVideoFramework.avkit)
                }
            } header: {
                Label("Video", systemImage: "film")
            }
            // MARK: Advanced
            Section {
                LabeledContent("Wallpaper Storage") {
                    HStack(spacing: 8) {
                        Text(configuredWallpaperStorageDirectory ?? defaultWallpaperStorageDirectory)
                            .lineLimit(1)
                            .truncationMode(.middle)
                            .foregroundStyle(
                                configuredWallpaperStorageDirectory == nil ? .secondary : .primary
                            )
                            .frame(maxWidth: .infinity, alignment: .trailing)
                        Button(action: selectWallpaperStorageDirectory) {
                            Image(systemName: "folder")
                        }
                        .help("Choose wallpaper storage folder")
                        if configuredWallpaperStorageDirectory != nil {
                            Button(action: resetWallpaperStorageDirectory) {
                                Image(systemName: "xmark.circle")
                            }
                            .help("Restore default wallpaper storage folder")
                        }
                    }
                }
                Text("New downloads and imports are saved here. Existing wallpapers are not moved.")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                if let issue = wallpaperStorageDirectoryIssue {
                    Label(issue, systemImage: "exclamationmark.triangle.fill")
                        .font(.footnote)
                        .foregroundStyle(.red)
                        .fixedSize(horizontal: false, vertical: true)
                }
                LabeledContent("Wallpaper Engine assets") {
                    HStack(spacing: 8) {
                        Text(configuredAssetsDirectory ?? "Not selected")
                            .lineLimit(1)
                            .truncationMode(.middle)
                            .foregroundStyle(
                                configuredAssetsDirectory == nil ? .secondary : .primary
                            )
                            .frame(maxWidth: .infinity, alignment: .trailing)
                        Button(action: selectAssetsDirectory) {
                            Image(systemName: "folder")
                        }
                        .help("Choose Wallpaper Engine assets directory")
                        if configuredAssetsDirectory != nil {
                            Button(action: clearAssetsDirectory) {
                                Image(systemName: "xmark.circle")
                            }
                            .help("Clear assets directory")
                        }
                    }
                }
                if let issue = assetsDirectoryIssue ?? configuredAssetsValidationIssue {
                    Label(issue, systemImage: "exclamationmark.triangle.fill")
                        .font(.footnote)
                        .foregroundStyle(.red)
                        .fixedSize(horizontal: false, vertical: true)
                }
                Picker("Process Piority", selection: $viewModel.settings.processPiority) {
                    Text("Normal").tag(GSProcessPiority.normal)
                    Text("Below Normal").tag(GSProcessPiority.belowNormal)
                }
                Toggle("Pause when VRAM is exhausted", isOn: $viewModel.settings.pauseOnVRAMExhausted)
                Toggle("Restart after crashing", isOn: $viewModel.settings.restartAfterCrashing)
            } header: {
                Label("Advanced", systemImage: "wrench.and.screwdriver.fill")
            }
            // MARK: Developers
            Section {
                Picker("Log Level", selection: $viewModel.settings.logLevel) {
                    Text("None").tag(GSLogLevel.none)
                    Text("Errors Only").tag(GSLogLevel.error)
                    Text("Verbose").tag(GSLogLevel.verbose)
                }
            } header: {
                Label("Developer", systemImage: "number")
            }
            // MARK: Reset
            Section {
                HStack {
                    Text("Reset Config")
                    Spacer()
                    Button {
                        viewModel.settings = GlobalSettings()
                    } label: {
                        Text("Reset").frame(width: 100)
                    }
                    .tint(Color.red)
                    .buttonStyle(.borderedProminent)
                }
            } header: {
                Label("Reset", systemImage: "exclamationmark.triangle.fill")
            }
        }.formStyle(.grouped)
    }

    private var configuredAssetsDirectory: String? {
        let value = viewModel.settings.wallpaperEngineAssetsDirectory?
            .trimmingCharacters(in: .whitespacesAndNewlines)
        guard let value, !value.isEmpty else { return nil }
        return value
    }

    private var configuredWallpaperStorageDirectory: String? {
        let value = viewModel.settings.wallpaperStorageDirectory?
            .trimmingCharacters(in: .whitespacesAndNewlines)
        guard let value, !value.isEmpty else { return nil }
        return value
    }

    private var defaultWallpaperStorageDirectory: String {
        WallpaperDirectory.defaultURL(using: .default).path
    }

    private var configuredAssetsValidationIssue: String? {
        guard let configuredAssetsDirectory else { return nil }
        do {
            try Self.validateAssetsDirectory(
                URL(fileURLWithPath: configuredAssetsDirectory, isDirectory: true)
            )
            return nil
        } catch {
            return error.localizedDescription
        }
    }

    private func selectAssetsDirectory() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.allowsMultipleSelection = false
        panel.canCreateDirectories = false
        panel.prompt = "Select"
        if let configuredAssetsDirectory {
            panel.directoryURL = URL(
                fileURLWithPath: configuredAssetsDirectory,
                isDirectory: true
            )
        }
        guard panel.runModal() == .OK, let selectedURL = panel.url else { return }

        do {
            let assetsURL = try Self.resolveAssetsDirectory(from: selectedURL)
            viewModel.settings.wallpaperEngineAssetsDirectory = assetsURL.path
            assetsDirectoryIssue = nil
        } catch {
            assetsDirectoryIssue = error.localizedDescription
        }
    }

    private func clearAssetsDirectory() {
        viewModel.settings.wallpaperEngineAssetsDirectory = nil
        assetsDirectoryIssue = nil
    }

    private func selectWallpaperStorageDirectory() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.allowsMultipleSelection = false
        panel.canCreateDirectories = true
        panel.prompt = "Select"
        panel.directoryURL = URL(
            fileURLWithPath: configuredWallpaperStorageDirectory
                ?? defaultWallpaperStorageDirectory,
            isDirectory: true
        )
        guard panel.runModal() == .OK, let selectedURL = panel.url else { return }

        let directory = selectedURL.standardizedFileURL.resolvingSymlinksInPath()
        guard FileManager.default.isWritableFile(atPath: directory.path) else {
            wallpaperStorageDirectoryIssue =
                "The selected wallpaper storage folder is not writable."
            return
        }

        viewModel.settings.wallpaperStorageDirectory = directory.path
        AppDelegate.shared.contentViewModel.refresh()
        wallpaperStorageDirectoryIssue = nil
    }

    private func resetWallpaperStorageDirectory() {
        viewModel.settings.wallpaperStorageDirectory = nil
        AppDelegate.shared.contentViewModel.refresh()
        wallpaperStorageDirectoryIssue = nil
    }

    private static func resolveAssetsDirectory(from selectedURL: URL) throws -> URL {
        let selectedURL = selectedURL.standardizedFileURL.resolvingSymlinksInPath()
        do {
            try validateAssetsDirectory(selectedURL)
            return selectedURL
        } catch let directError {
            let nestedAssetsURL = selectedURL.appendingPathComponent(
                "assets",
                isDirectory: true
            )
            do {
                try validateAssetsDirectory(nestedAssetsURL)
                return nestedAssetsURL.standardizedFileURL.resolvingSymlinksInPath()
            } catch {
                if selectedURL.lastPathComponent.caseInsensitiveCompare("assets") == .orderedSame {
                    throw directError
                }
                throw AssetsDirectorySelectionError.invalidSelection
            }
        }
    }

    private static func validateAssetsDirectory(_ url: URL) throws {
        var isDirectory: ObjCBool = false
        guard FileManager.default.fileExists(
            atPath: url.path,
            isDirectory: &isDirectory
        ), isDirectory.boolValue else {
            throw AssetsDirectorySelectionError.invalidSelection
        }
        guard FileManager.default.isReadableFile(atPath: url.path) else {
            throw AssetsDirectorySelectionError.unreadable
        }

        let shadersURL = url.appendingPathComponent("shaders", isDirectory: true)
        var shadersIsDirectory: ObjCBool = false
        guard FileManager.default.fileExists(
            atPath: shadersURL.path,
            isDirectory: &shadersIsDirectory
        ), shadersIsDirectory.boolValue else {
            throw AssetsDirectorySelectionError.missingShaders
        }
        guard FileManager.default.isReadableFile(atPath: shadersURL.path) else {
            throw AssetsDirectorySelectionError.unreadable
        }
    }
}

private enum AssetsDirectorySelectionError: LocalizedError {
    case invalidSelection
    case missingShaders
    case unreadable

    var errorDescription: String? {
        switch self {
        case .invalidSelection:
            return "Choose the assets directory or its Wallpaper Engine parent directory."
        case .missingShaders:
            return "The selected assets directory does not contain a shaders directory."
        case .unreadable:
            return "The selected assets directory is not readable."
        }
    }
}
