//
//  ExplorerTopBar.swift
//  Open Wallpaper Engine
//
//  Created by Haren on 2023/8/15.
//

import SwiftUI

struct BrowserTopBar<LeadingAccessory: View, TrailingControls: View>: View {
    @Binding private var searchText: String

    private let onSearchSubmit: () -> Void
    private let onFilter: () -> Void
    private let leadingAccessory: LeadingAccessory
    private let trailingControls: TrailingControls

    init(
        searchText: Binding<String>,
        onSearchSubmit: @escaping () -> Void,
        onFilter: @escaping () -> Void,
        @ViewBuilder leadingAccessory: () -> LeadingAccessory,
        @ViewBuilder trailingControls: () -> TrailingControls
    ) {
        self._searchText = searchText
        self.onSearchSubmit = onSearchSubmit
        self.onFilter = onFilter
        self.leadingAccessory = leadingAccessory()
        self.trailingControls = trailingControls()
    }

    var body: some View {
        HStack {
            TextField("Search", text: $searchText)
                .textFieldStyle(.roundedBorder)
                .frame(width: 160)
                .onSubmit(onSearchSubmit)

            Button(action: onFilter) {
                Label("Filter Results", systemImage: "checklist.checked")
            }
            .buttonStyle(.borderedProminent)

            leadingAccessory
            Spacer()
            trailingControls
        }
    }
}

struct ExplorerTopBar: SubviewOfContentView {
    @ObservedObject var viewModel: ContentViewModel
    
    @EnvironmentObject var globalSettingsViewModel: GlobalSettingsViewModel
    
    init(contentViewModel viewModel: ContentViewModel) {
        self.viewModel = viewModel
    }
    
    var body: some View {
        BrowserTopBar(
            searchText: $viewModel.searchText,
            onSearchSubmit: {},
            onFilter: {
                viewModel.isFilterReveal.toggle()
            }
        ) {
            if globalSettingsViewModel.settings.autoRefresh {
                Button {
                    viewModel.refresh()
                } label: {
                    Image(systemName: "arrow.triangle.2.circlepath")
                }
            }
        } trailingControls: {
            Button { 
                if viewModel.sortingSequence == .decrease {
                    viewModel.sortingSequence = .increase
                } else {
                    viewModel.sortingSequence = .decrease
                }
            } label: {
                Image(systemName: viewModel.sortingSequence == .increase ?
                      "arrowtriangle.down.fill" : "arrowtriangle.up.fill")
            }
            .buttonStyle(.plain)
            Picker("Sort By", selection: $viewModel.sortingBy) {
                ForEach(WEWallpaperSortingMethod.allCases) { method in
                    Text(method.rawValue).tag(method.rawValue)
                }
            }
            .labelsHidden()
            .frame(width: 160)
        }
    }
}
