//
//  WallpaperExplorer.swift
//  Open Wallpaper Engine
//
//  Created by Haren on 2023/8/15.
//

import SwiftUI

struct WallpaperExplorer: SubviewOfContentView {
    @ObservedObject var viewModel: ContentViewModel
    @ObservedObject var wallpaperViewModel: WallpaperViewModel
    
    init(contentViewModel viewModel: ContentViewModel, wallpaperViewModel: WallpaperViewModel) {
        self.viewModel = viewModel
        self.wallpaperViewModel = wallpaperViewModel
    }
    
    var body: some View {
        ScrollView {
            Group {
                // MARK: Items
                if viewModel.autoRefreshWallpapers.isEmpty &&
                    viewModel.visibleWorkshopDownloadJobs.isEmpty {
                    HStack {
                        Spacer()
                        Text("""
                            No wallpapers found for your search.
                            Expand or reset the categories in the filter sidebar or try another search term.
                            """)
                        .font(.title)
                        .foregroundStyle(Color.secondary)
                        .multilineTextAlignment(.center)
                        .lineLimit(nil)
                        .lineSpacing(10)
                        Spacer()
                    }
                    .fixedSize(horizontal: false, vertical: true)
                    .padding(.top, 50)
                } else {
                    LazyVGrid(columns: gridColumns, alignment: .leading) {
                        ForEach(viewModel.visibleWorkshopDownloadJobs) { job in
                            WorkshopDownloadExplorerItem(
                                viewModel: viewModel,
                                job: job
                            )
                        }

                        ForEach(viewModel.autoRefreshWallpapers, id: \.wallpaperDirectory) { wallpaper in
                            ExplorerItem(viewModel: viewModel, wallpaperViewModel: wallpaperViewModel, wallpaper: wallpaper)
                                .contextMenu {
                                    ExplorerItemMenu(contentViewModel: viewModel, wallpaperViewModel: wallpaperViewModel, current: wallpaper)
                                    ExplorerGlobalMenu(contentViewModel: viewModel, wallpaperViewModel: wallpaperViewModel)
                                }
                        }
                    }
                    .padding(.trailing)
                }
            }
            .background(OverlayScrollerConfigurator())
        }
        .overlay {
            VStack {
                Spacer()
                HStack {
                    ForEach(0..<viewModel.maxPage, id: \.self) { page in
                        Button("\(page + 1)") {
                            viewModel.currentPage = page + 1
                        }
                    }
                }
                .padding(.bottom)
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .layoutPriority(1)
    }

    private var gridColumns: [GridItem] {
        WallpaperGridLayout.columns(iconSize: viewModel.explorerIconSize)
    }
}

private struct WorkshopDownloadExplorerItem: View {
    @ObservedObject var viewModel: ContentViewModel
    let job: SteamCmdService.DownloadJob

    var body: some View {
        ZStack(alignment: .bottom) {
            WorkshopItemPreview(url: job.item.previewImageURL)

            VStack(alignment: .leading, spacing: 5) {
                Text(job.item.title)
                    .font(.footnote.weight(.medium))
                    .foregroundStyle(.white)
                    .lineLimit(2)

                switch job.state {
                case .queued:
                    downloadProgress(progress: 0, status: "Queued")
                case .downloading(let progress, let status):
                    downloadProgress(progress: progress ?? 0, status: status)
                case .failed(let message):
                    HStack(spacing: 6) {
                        Label("Failed", systemImage: "xmark.circle.fill")
                            .foregroundStyle(.red)
                        Spacer(minLength: 4)
                        Button("Retry") {
                            viewModel.steamCmd.downloadWorkshopItem(item: job.item)
                        }
                        .buttonStyle(.borderless)
                        .foregroundStyle(.white)
                    }
                    .font(.caption2)
                    Text(message)
                        .font(.caption2)
                        .foregroundStyle(.white.opacity(0.8))
                        .lineLimit(2)
                case .completed:
                    EmptyView()
                }
            }
            .padding(8)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(.black.opacity(0.7))
        }
        .clipShape(RoundedRectangle(cornerRadius: 2))
    }

    private func downloadProgress(progress: Double, status: String) -> some View {
        VStack(alignment: .leading, spacing: 3) {
            ProgressView(value: progress)
                .progressViewStyle(.linear)
                .tint(.blue)
            HStack(spacing: 5) {
                Text(status)
                    .lineLimit(1)
                Spacer(minLength: 4)
                Text(progress, format: .percent.precision(.fractionLength(0)))
                    .monospacedDigit()
            }
            .font(.caption2)
            .foregroundStyle(.white.opacity(0.9))
        }
    }
}

enum WallpaperGridLayout {
    static func columns(iconSize: CGFloat) -> [GridItem] {
        [
            GridItem(.adaptive(
                minimum: iconSize,
                maximum: iconSize * 1.5
            ))
        ]
    }
}

private struct OverlayScrollerConfigurator: NSViewRepresentable {
    func makeNSView(context: Context) -> NSView {
        OverlayScrollerConfigurationView()
    }

    func updateNSView(_ nsView: NSView, context: Context) {}
}

private final class OverlayScrollerConfigurationView: NSView {
    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        configureScroller()
    }

    override func layout() {
        super.layout()
        configureScroller()
    }

    private func configureScroller() {
        enclosingScrollView?.scrollerStyle = .overlay
    }
}

// MARK: - View Modifiers Extension
struct SelectedItem: ViewModifier {
    var selected: Bool
    
    init(_ selected: Bool) {
        self.selected = selected
    }
    
    func body(content: Content) -> some View {
        return content
            .border(Color.accentColor, width: selected ? 3 : 0)
    }
}

extension View {
    func selected(_ selected: Bool = true) -> some View {
        return modifier(SelectedItem(selected))
    }
}
