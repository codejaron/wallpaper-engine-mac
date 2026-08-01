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
                if viewModel.autoRefreshWallpapers.isEmpty {
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
