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
            // Remove observer for old item before replacing
            if let oldItem = self.player.currentItem {
                NotificationCenter.default.removeObserver(self, name: .AVPlayerItemDidPlayToEndTime, object: oldItem)
            }
            let newItem = AVPlayerItem(url: currentWallpaper.wallpaperDirectory.appending(path: currentWallpaper.project.file))
            self.player.replaceCurrentItem(with: newItem)
            NotificationCenter.default.addObserver(self, selector: #selector(playerDidFinishPlaying(_:)), name: .AVPlayerItemDidPlayToEndTime, object: newItem)
            // Force-apply rate and volume — replaceCurrentItem resets player to paused
            self.applyEffectiveRate()
            self.applyEffectiveVolume()
        }
    }

    var playRate: Float = 0 {
        didSet {
            applyEffectiveRate()
        }
    }

    var playVolume: Float = 0 {
        didSet {
            applyEffectiveVolume()
        }
    }

    var player = AVPlayer()
    private var cancellables = Set<AnyCancellable>()

    init(wallpaper currentWallpaper: WEWallpaper, screenId: String) {
        self.screenId = screenId
        self.currentWallpaper = currentWallpaper
        self.player = AVPlayer(url: currentWallpaper.wallpaperDirectory.appending(path: currentWallpaper.project.file))
        NotificationCenter.default.addObserver(self, selector: #selector(playerDidFinishPlaying(_:)), name: .AVPlayerItemDidPlayToEndTime, object: self.player.currentItem)

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
        applyEffectiveVolume()
        AppDelegate.shared.sceneAudioOwnerCoordinator.$ownerScreenId
            .receive(on: DispatchQueue.main)
            .sink { [weak self] _ in self?.applyEffectiveVolume() }
            .store(in: &cancellables)
        AppDelegate.shared.globalSettingsViewModel.$settings
            .map(\.audioOutput)
            .removeDuplicates()
            .receive(on: DispatchQueue.main)
            .sink { [weak self] _ in self?.applyEffectiveVolume() }
            .store(in: &cancellables)
    }

    deinit {
        NotificationCenter.default.removeObserver(self)
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

    private func applyEffectiveVolume() {
        let enabled = AppDelegate.shared.globalSettingsViewModel.settings.audioOutput
        let owner = AppDelegate.shared.sceneAudioOwnerCoordinator.isAudible(screenId: screenId)
        player.volume = owner && enabled ? playVolume : 0
    }
}
