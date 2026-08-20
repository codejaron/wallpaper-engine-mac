//
//  VideoWallpaperViewModel.swift
//  Open Wallpaper Engine
//
//  Created by Haren on 2023/8/14.
//

import AVKit
import SwiftUI
import Combine

@MainActor
class VideoWallpaperViewModel: ObservableObject {
    let screenId: String
    var currentWallpaper: WEWallpaper {
        didSet {
            loadCurrentWallpaper()
        }
    }

    var playRate: Float = 0 {
        didSet {
            applyEffectiveRate()
        }
    }

    var playVolume: Float = 0 {
        didSet {
            applyAudioOutputState()
        }
    }

    let player = AVPlayer()
    private var cancellables = Set<AnyCancellable>()
    private var itemStatusCancellable: AnyCancellable?

    init(wallpaper currentWallpaper: WEWallpaper, screenId: String) {
        self.screenId = screenId
        self.currentWallpaper = currentWallpaper
        loadCurrentWallpaper()

        // Host conditions and user intent are already combined by the shared
        // WallpaperViewModel. This adapter must not create a second sleep state.
        let wvm = AppDelegate.shared.wallpaperViewModel
        wvm.$effectivePlayRate
            .receive(on: DispatchQueue.main)
            .sink { [weak self] rate in
                self?.playRate = rate
            }
            .store(in: &cancellables)
        wvm.$effectivePlayVolume
            .receive(on: DispatchQueue.main)
            .sink { [weak self] volume in
                self?.playVolume = volume
            }
            .store(in: &cancellables)
        applyAudioOutputState()
        AppDelegate.shared.sceneAudioOwnerCoordinator.$ownerScreenId
            .receive(on: DispatchQueue.main)
            .sink { [weak self] _ in self?.applyAudioOutputState() }
            .store(in: &cancellables)
        AppDelegate.shared.globalSettingsViewModel.$settings
            .map(\.audioOutput)
            .removeDuplicates()
            .receive(on: DispatchQueue.main)
            .sink { [weak self] _ in self?.applyAudioOutputState() }
            .store(in: &cancellables)
    }

    deinit {
        NotificationCenter.default.removeObserver(self)
    }

    func prepareForDisplay() {
        guard player.currentItem == nil else { return }
        loadCurrentWallpaper()
    }

    func releasePlaybackResources() {
        itemStatusCancellable?.cancel()
        itemStatusCancellable = nil
        if let item = player.currentItem {
            NotificationCenter.default.removeObserver(
                self,
                name: .AVPlayerItemDidPlayToEndTime,
                object: item
            )
        }
        player.pause()
        player.replaceCurrentItem(with: nil)
    }

    private func loadCurrentWallpaper() {
        releasePlaybackResources()
        let item = AVPlayerItem(
            url: currentWallpaper.wallpaperDirectory.appending(
                path: currentWallpaper.project.file
            )
        )
        player.replaceCurrentItem(with: item)
        itemStatusCancellable = item.publisher(
            for: \.status,
            options: [.initial, .new]
        )
            .receive(on: DispatchQueue.main)
            .sink { [weak self] _ in self?.applyAudioOutputState() }
        NotificationCenter.default.addObserver(
            self,
            selector: #selector(playerDidFinishPlaying(_:)),
            name: .AVPlayerItemDidPlayToEndTime,
            object: item
        )
        // Replacing an item resets AVPlayer to paused and may reset volume.
        applyEffectiveRate()
        applyAudioOutputState()
    }

    @objc private func playerDidFinishPlaying(_ notification: Notification) {
        // Replay video
        self.player.seek(to: CMTime.zero)
        applyEffectiveRate()
    }

    @objc private func playerDidStopPlaying(_ notification: Notification) {
        // Resume playback
        applyEffectiveRate()
    }

    private func applyEffectiveRate() {
        player.rate = playRate
    }

    private func applyAudioOutputState() {
        let enabled = AppDelegate.shared.globalSettingsViewModel.settings.audioOutput
        let owner = AppDelegate.shared.sceneAudioOwnerCoordinator.isAudible(screenId: screenId)
        let shouldEnableAudio = owner && enabled && playVolume > 0
        player.isMuted = !shouldEnableAudio
        if shouldEnableAudio {
            player.volume = playVolume
        }
        for track in player.currentItem?.tracks ?? []
        where track.assetTrack?.mediaType == .audio {
            track.isEnabled = shouldEnableAudio
        }
    }
}
