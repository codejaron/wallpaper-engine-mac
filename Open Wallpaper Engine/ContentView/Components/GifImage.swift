//
//  GifImage.swift
//  Open Wallpaper Engine
//
//  Created by Haren on 2023/8/15.
//

import Cocoa
import SwiftUI

struct GifImage: NSViewRepresentable {
    private enum Source {
        case resource(String)
        case url(URL)
        case image(NSImage)

        var identity: SourceIdentity {
            switch self {
            case .resource(let name):
                return .resource(name)
            case .url(let url):
                return .url(url)
            case .image(let image):
                return .image(ObjectIdentifier(image))
            }
        }

        func load() -> NSImage? {
            switch self {
            case .resource(let name):
                guard let url = Bundle.main.url(forResource: name, withExtension: "gif") else {
                    return nil
                }
                return NSImage(contentsOf: url)
            case .url(let url):
                return NSImage(contentsOf: url)
            case .image(let image):
                return image
            }
        }
    }

    fileprivate enum SourceIdentity: Equatable {
        case resource(String)
        case url(URL)
        case image(ObjectIdentifier)
    }

    final class Coordinator {
        fileprivate var loadedSource: SourceIdentity?
    }

    private let source: Source
    private var isResizable = false
    private let animates: Bool

    init(_ gifName: String, animates: Bool = true) {
        source = .resource(gifName)
        self.animates = animates
    }

    init(contentsOf url: URL, animates: Bool = true) {
        source = .url(url)
        self.animates = animates
    }

    init(image: NSImage, animates: Bool = true) {
        source = .image(image)
        self.animates = animates
    }

    func makeCoordinator() -> Coordinator {
        Coordinator()
    }

    func makeNSView(context: Context) -> NSImageView {
        let imageView = NSImageView()
        imageView.canDrawSubviewsIntoLayer = true
        imageView.imageScaling = .scaleProportionallyUpOrDown
        update(imageView, coordinator: context.coordinator)
        return imageView
    }

    func updateNSView(_ imageView: NSImageView, context: Context) {
        update(imageView, coordinator: context.coordinator)
    }

    func sizeThatFits(
        _ proposal: ProposedViewSize,
        nsView: NSImageView,
        context: Context
    ) -> CGSize? {
        guard isResizable else { return nil }

        if let width = proposal.width, let height = proposal.height {
            return CGSize(width: width, height: height)
        }

        let imageSize = nsView.image?.size ?? .zero
        let aspectRatio = imageSize.width > 0 && imageSize.height > 0
            ? imageSize.width / imageSize.height
            : 1

        if let width = proposal.width {
            return CGSize(width: width, height: width / aspectRatio)
        }
        if let height = proposal.height {
            return CGSize(width: height * aspectRatio, height: height)
        }
        return imageSize == .zero ? nil : imageSize
    }

    func resizable(
        capInsets: EdgeInsets = EdgeInsets(),
        resizingMode: Image.ResizingMode = .stretch
    ) -> Self {
        var view = self
        view.isResizable = true
        return view
    }

    private func update(_ imageView: NSImageView, coordinator: Coordinator) {
        imageView.animates = animates
        imageView.imageScaling = .scaleProportionallyUpOrDown

        guard coordinator.loadedSource != source.identity else { return }

        let image = source.load()
        image?.representations
            .compactMap { $0 as? NSBitmapImageRep }
            .forEach { $0.setProperty(.loopCount, withValue: 0) }
        imageView.image = image
        coordinator.loadedSource = source.identity
    }
}
