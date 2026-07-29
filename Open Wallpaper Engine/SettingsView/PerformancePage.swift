//
//  PerformancePage.swift
//  Open Wallpaper Engine
//
//  Created by Haren on 2023/8/12.
//

import SwiftUI

struct PerformancePage: SettingsPage {
    @ObservedObject var viewModel: GlobalSettingsViewModel
    
    @State private var isEditingFPS = false
    
    init(globalSettings viewModel: GlobalSettingsViewModel) {
        self.viewModel = viewModel
    }
    
    var body: some View {
        Form {
            Section {
                Picker("Other Application Focused:", selection: $viewModel.settings.otherApplicationFocused) {
                    Text("Keep Running").tag(GSPlayback.keepRunning)
                    Text("Mute").tag(GSPlayback.mute)
                    Text("Pause").tag(GSPlayback.pause)
                }
                
                Picker(
                    "When Another App Window Is Fullscreen or Maximized:",
                    selection: $viewModel.settings.otherApplicationFullscreen
                ) {
                    Text("Keep Running").tag(GSPlayback.keepRunning)
                    Text("Mute").tag(GSPlayback.mute)
                    Text("Pause").tag(GSPlayback.pause)
                    Text("Stop (free memory)").tag(GSPlayback.stop)
                }
                
                Picker("Other Application Playing Audio:", selection: $viewModel.settings.otherApplicationPlayingAudio) {
                    Text("Keep Running").tag(GSPlayback.keepRunning)
                    Text("Mute").tag(GSPlayback.mute)
                    Text("Pause").tag(GSPlayback.pause)
                }
                if viewModel.settings.requiresSystemAudioCaptureForAudioRule {
                    Label(
                        "Enable System Audio Capture in General settings for this rule to work.",
                        systemImage: "exclamationmark.triangle.fill"
                    )
                    .font(.footnote)
                    .foregroundStyle(.orange)
                    .fixedSize(horizontal: false, vertical: true)
                }
                
                Picker("Laptop on battery", selection: $viewModel.settings.laptopOnBattery) {
                    Text("Keep Running").tag(GSPlayback.keepRunning)
                    Text("Pause").tag(GSPlayback.pause)
                    Text("Stop (free memory)").tag(GSPlayback.stop)
                }
                
                HStack {
                    Text("Application Rules")
                    Spacer()
                    Button {
                        
                    } label: {
                        Text("Edit").frame(width: 100)
                    }
                    .buttonStyle(.borderedProminent)
                    .disabled(true)
                }

                if let issue = viewModel.playbackPolicyIssue {
                    Label(issue, systemImage: "exclamationmark.triangle.fill")
                        .font(.footnote)
                        .foregroundStyle(.red)
                        .fixedSize(horizontal: false, vertical: true)
                }
            } header: {
                Label("Playback", systemImage: "play.fill")
            }
            Section {
                HStack(spacing: 1) {
                    Button {
                        viewModel.setQuality(.low)
                    } label: {
                        Text("Low").frame(maxWidth: .infinity)
                    }
                    Divider()
                    Button {
                        viewModel.setQuality(.medium)
                    } label: {
                        Text("Medium").frame(maxWidth: .infinity)
                    }
                    Divider()
                    Button {
                        viewModel.setQuality(.high)
                    } label: {
                        Text("High").frame(maxWidth: .infinity)
                    }
                    Divider()
                    Button {
                        viewModel.setQuality(.ultra)
                    } label: {
                        Text("Ultra").frame(maxWidth: .infinity)
                    }
                }
                .padding(6)
                .background(Color(nsColor: NSColor.unemphasizedSelectedContentBackgroundColor))
                .buttonStyle(.borderless)
                .clipShape(RoundedRectangle(cornerRadius: 5.0))
                .disabled(true)
                Picker("Anti-aliasing", selection: $viewModel.settings.antiAliasing) {
                    Text("None").tag(GSAntiAliasingQuality.none)
                    Text("MSAA x2").tag(GSAntiAliasingQuality.msaa_x2)
                    Text("MSAA x4").tag(GSAntiAliasingQuality.msaa_x4)
                    Text("MSAA x8").tag(GSAntiAliasingQuality.msaa_x8)
                }
                .disabled(true)
                Picker("Post-Processing", selection: $viewModel.settings.postProcessing) {
                    Text("Disabled").tag(GSPostProcessingQuality.disabled)
                    Text("Enabled").tag(GSPostProcessingQuality.enabled)
                    Text("Ultra").tag(GSPostProcessingQuality.ultra)
                }
                .disabled(true)
                Picker("Texture Resolution", selection: $viewModel.settings.textureResolution) {
                    Text("High Quality").tag(GSTextureResolutionQuality.highQuality)
                    Text("High Performance").tag(GSTextureResolutionQuality.highPerformance)
                    Text("Automatic").tag(GSTextureResolutionQuality.automatic)
                }
                .disabled(true)
                HStack {
                    Text("FPS")
                    Spacer()
                    Slider(value: $viewModel.settings.fps, in: 10...120)
                        .frame(width: 150)
                    if isEditingFPS {
                        TextField("FPS", text: Binding<String>(get: {
                            String(format: "%.00f", viewModel.settings.fps)
                        }, set: {
                            if let newValue = Double($0) {
                                if newValue > 120 {
                                    viewModel.settings.fps = 120
                                } else if newValue < 10 {
                                    viewModel.settings.fps = 10
                                } else {
                                    viewModel.settings.fps = newValue
                                }
                            }
                        }))
                        .textFieldStyle(.roundedBorder)
                        .labelsHidden()
                        .frame(width: 50, height: 20)
                        .onSubmit {
                            isEditingFPS = false
                        }
                    } else {
                        Text(String(format: "%.00f", viewModel.settings.fps))
                            .frame(width: 25)
                            .onTapGesture(count: 2) {
                                isEditingFPS = true
                            }
                    }
                }
                .overlay {
                    HStack {
                        Spacer(); Spacer()
                        if !isEditingFPS {
                            if viewModel.settings.fps > 60 {
                                Image(systemName: "exclamationmark.triangle.fill")
                                    .foregroundStyle(.red)
                                    .help("High FPS may slow down your PC! We're serious, this is too much 🔥.")
                            } else if viewModel.settings.fps > 30 {
                                Image(systemName: "exclamationmark.triangle.fill")
                                    .foregroundStyle(.yellow)
                                    .help("High FPS may slow down your PC!")
                            }
                        }
                        Spacer()
                    }
                    
                }
                HStack {
                    Text("Reflections")
                        .foregroundStyle(.secondary)
                    Spacer()
                    Toggle("Reflection", isOn: $viewModel.settings.reflections)
                        .toggleStyle(.checkbox)
                        .labelsHidden()
                }
                .disabled(true)
                Picker("Scene scaling", selection: $viewModel.settings.scenePresentationScaling) {
                    Text("Automatic").tag(GSScenePresentationScaling.automatic)
                    Text("Stretch").tag(GSScenePresentationScaling.stretch)
                    Text("Aspect fit").tag(GSScenePresentationScaling.aspectFit)
                    Text("Aspect fill").tag(GSScenePresentationScaling.aspectFill)
                }
                Toggle(
                    "Span scene wallpapers across displays",
                    isOn: $viewModel.settings.sceneSpanAcrossScreens
                )
                Text("Span mode uses one virtual canvas and presents the current display's slice. It is intended for the same Scene wallpaper on each enabled display.")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            } header: {
                Label("Quality", systemImage: "memorychip.fill")
                Text("FPS, scaling, and spanning apply only to Scene wallpapers. Disabled quality options are not yet supported.")
            }
        }
        .formStyle(.grouped)
    }
}
