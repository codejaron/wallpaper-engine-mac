//
//  WebWallpaperView.swift
//  Open Wallpaper Engine
//
//  Created by Haren on 2023/8/13.
//

import Cocoa
import SwiftUI
import WebKit

struct WebWallpaperView: NSViewRepresentable {
    @ObservedObject var wallpaperViewModel: WallpaperViewModel
    @StateObject var viewModel: WebWallpaperViewModel
    let screenId: String

    init(wallpaperViewModel: WallpaperViewModel, screenId: String) {
        self.wallpaperViewModel = wallpaperViewModel
        self.screenId = screenId
        self._viewModel = StateObject(
            wrappedValue: WebWallpaperViewModel(
                wallpaper: wallpaperViewModel.wallpaper(for: screenId),
                screenId: screenId
            )
        )
    }

    func makeNSView(context: Context) -> WKWebView {
        let configuration = WKWebViewConfiguration()
        configuration.allowsAirPlayForMediaPlayback = true
        configuration.mediaTypesRequiringUserActionForPlayback = []
        configuration.userContentController.addUserScript(
            WebWallpaperViewModel.makeMediaPolicyUserScript()
        )
        configuration.userContentController.addUserScript(
            WebWallpaperViewModel.makePropertyListenerUserScript()
        )

        let nsView = WKWebView(frame: .zero, configuration: configuration)
        nsView.navigationDelegate = viewModel
        viewModel.attach(nsView)
        Self.loadWallpaper(nsView, viewModel: viewModel)
        return nsView
    }

    /// Load wallpaper — uses loadHTMLString for URL-based wallpapers (YouTube/Vimeo)
    /// so the origin isn't file://, or loadFileURL for local wallpapers.
    private static func loadWallpaper(_ webView: WKWebView, viewModel: WebWallpaperViewModel) {
        let fileUrl = viewModel.fileUrl
        // Check if the HTML contains a redirect/embed to an external URL
        if let html = try? String(contentsOf: fileUrl, encoding: .utf8),
           html.contains("youtube.com") || html.contains("vimeo.com") {
            // Load as HTML string with https origin so YouTube/Vimeo embeds work
            webView.loadHTMLString(html, baseURL: URL(string: "https://localhost"))
        } else {
            webView.loadFileURL(fileUrl, allowingReadAccessTo: viewModel.readAccessURL)
        }
    }

    func updateNSView(_ nsView: WKWebView, context: Context) {
        let selectedWallpaper = wallpaperViewModel.wallpaper(for: screenId)
        let currentWallpaper = viewModel.currentWallpaper
        let needsReload = selectedWallpaper.wallpaperDirectory
            .appending(path: selectedWallpaper.project.file) != currentWallpaper.wallpaperDirectory
            .appending(path: currentWallpaper.project.file)

        viewModel.updateWallpaper(selectedWallpaper)
        if needsReload {
            Self.loadWallpaper(nsView, viewModel: viewModel)
        }
        viewModel.attach(nsView)
    }

    static func dismantleNSView(_ nsView: WKWebView, coordinator: ()) {
        (nsView.navigationDelegate as? WebWallpaperViewModel)?.detach(nsView)
        nsView.stopLoading()
        nsView.navigationDelegate = nil
    }
}
