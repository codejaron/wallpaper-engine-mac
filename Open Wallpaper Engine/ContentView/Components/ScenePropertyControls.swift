import AppKit
import SwiftUI

struct ScenePropertyControls: View {
    @ObservedObject var wallpaperViewModel: WallpaperViewModel
    let screenId: String
    let wallpaper: WEWallpaper
    @State private var inputError: String?

    private var catalog: ScenePropertyCatalog? {
        wallpaperViewModel.scenePropertyCatalog(for: screenId, wallpaper: wallpaper)
    }

    private var overrides: [String: ScenePropertyValue] {
        wallpaperViewModel.scenePropertyOverrides(for: screenId, wallpaper: wallpaper)
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            if let inputError {
                Label(inputError, systemImage: "exclamationmark.triangle.fill")
                    .foregroundStyle(.red)
            }
            if let error = wallpaperViewModel.scenePropertyPersistenceError {
                VStack(alignment: .leading, spacing: 6) {
                    Label(error, systemImage: "exclamationmark.triangle.fill")
                        .foregroundStyle(.red)
                        .fixedSize(horizontal: false, vertical: true)
                    Button("Discard Invalid Saved Properties") {
                        wallpaperViewModel.discardScenePropertyPersistence()
                    }
                }
            }
            if !wallpaperViewModel.isScreenEnabled(screenId) {
                Label(
                    "Enable this display to load and edit its Scene properties.",
                    systemImage: "display.trianglebadge.exclamationmark"
                )
                .foregroundStyle(.secondary)
            } else if let catalog {
                ForEach(catalog.properties) { property in
                    propertyControl(property)
                }
                if !overrides.isEmpty {
                    Button(role: .destructive) {
                        wallpaperViewModel.resetSceneProperties(
                            for: screenId,
                            wallpaper: wallpaper
                        )
                    } label: {
                        Label("Reset Scene Properties", systemImage: "arrow.triangle.2.circlepath")
                            .frame(maxWidth: .infinity)
                    }
                }
            } else if let error = wallpaperViewModel.sceneRuntimeError(
                for: screenId,
                wallpaper: wallpaper
            ) {
                Label(error, systemImage: "exclamationmark.triangle.fill")
                    .foregroundStyle(.red)
                    .fixedSize(horizontal: false, vertical: true)
            } else {
                HStack(spacing: 8) {
                    ProgressView().controlSize(.small)
                    Text("Loading Scene properties from the runtime…")
                        .foregroundStyle(.secondary)
                }
            }
        }
    }

    @ViewBuilder
    private func propertyControl(_ property: ScenePropertyDefinition) -> some View {
        switch property.kind {
        case .group:
            VStack(alignment: .leading, spacing: 4) {
                Text(property.text).font(.headline)
                Divider()
            }
        case .text:
            HStack(alignment: .top) {
                Text(property.text)
                Spacer()
                Text(stringValue(for: property) ?? "Missing runtime text value")
                    .foregroundStyle(stringValue(for: property) == nil ? .red : .secondary)
                    .multilineTextAlignment(.trailing)
            }
        case .boolean:
            if let value = booleanValue(for: property) {
                Toggle(property.text, isOn: Binding(
                    get: { booleanValue(for: property) ?? value },
                    set: { set(.boolean($0), for: property) }
                ))
                .disabled(property.isReadOnly)
            } else {
                invalidValue(property)
            }
        case .slider:
            slider(property)
        case .combo:
            if let value = stringValue(for: property),
               !property.options.isEmpty,
               property.options.contains(where: { $0.value == value }) {
                HStack {
                    Text(property.text)
                    Spacer()
                    Picker(property.text, selection: Binding(
                        get: { stringValue(for: property) ?? value },
                        set: { set(.string($0), for: property) }
                    )) {
                        ForEach(property.options) { option in
                            Text(option.label).tag(option.value)
                        }
                    }
                    .labelsHidden()
                    .frame(maxWidth: 150)
                    .disabled(property.isReadOnly)
                }
            } else {
                invalidValue(property)
            }
        case .color:
            colorPicker(property)
        case .textInput:
            if let value = stringValue(for: property) {
                TextField(property.text, text: Binding(
                    get: { stringValue(for: property) ?? value },
                    set: { set(.string($0), for: property) }
                ))
                .disabled(property.isReadOnly)
            } else {
                invalidValue(property)
            }
        case .sceneTexture, .file, .directory, .userShortcut:
            VStack(alignment: .leading, spacing: 2) {
                Text(property.text)
                Text("This Scene property type is visible but is not safely editable yet.")
                    .font(.caption)
                    .foregroundStyle(.orange)
            }
        }
    }

    @ViewBuilder
    private func slider(_ property: ScenePropertyDefinition) -> some View {
        if let value = numberValue(for: property),
           let minimum = property.minimum,
           let maximum = property.maximum,
           minimum.isFinite,
           maximum.isFinite,
           minimum <= maximum,
           property.step.map({ $0.isFinite && $0 > 0 }) ?? true {
            VStack(alignment: .leading, spacing: 4) {
                HStack {
                    Text(property.text)
                    Spacer()
                    Text(formatted(value, precision: property.precision))
                        .monospacedDigit()
                }
                let binding = Binding<Double>(
                    get: { numberValue(for: property) ?? value },
                    set: { newValue in
                        switch effectiveValue(for: property) {
                        case .integer:
                            set(.integer(Int64(newValue.rounded())), for: property)
                        case .number:
                            set(.number(newValue), for: property)
                        default:
                            inputError = "\(property.text) no longer has a numeric runtime value."
                        }
                    }
                )
                if let step = property.step, step > 0 {
                    Slider(value: binding, in: minimum...maximum, step: step)
                } else {
                    Slider(value: binding, in: minimum...maximum)
                }
            }
            .disabled(property.isReadOnly)
        } else {
            VStack(alignment: .leading, spacing: 2) {
                invalidValue(property)
                Text("The runtime did not provide a valid slider range.")
                    .font(.caption)
                    .foregroundStyle(.red)
            }
        }
    }

    @ViewBuilder
    private func colorPicker(_ property: ScenePropertyDefinition) -> some View {
        if let string = stringValue(for: property),
           let color = SceneRuntimeColor(string: string) {
            ColorPicker(property.text, selection: Binding(
                get: {
                    guard let current = stringValue(for: property),
                          let parsed = SceneRuntimeColor(string: current) else {
                        return color.color
                    }
                    return parsed.color
                },
                set: { newColor in
                    guard let converted = SceneRuntimeColor(
                        color: newColor,
                        componentCount: color.componentCount
                    ) else {
                        inputError = "The selected color could not be converted to runtime RGB values."
                        return
                    }
                    inputError = nil
                    let encoded = converted.encoded
                    set(.string(encoded), for: property)
                }
            ), supportsOpacity: color.componentCount == 4)
            .disabled(property.isReadOnly)
        } else {
            invalidValue(property)
        }
    }

    private func effectiveValue(for property: ScenePropertyDefinition) -> ScenePropertyValue? {
        overrides[property.key] ?? property.defaultValue
    }

    private func booleanValue(for property: ScenePropertyDefinition) -> Bool? {
        guard case .boolean(let value) = effectiveValue(for: property) else { return nil }
        return value
    }

    private func numberValue(for property: ScenePropertyDefinition) -> Double? {
        switch effectiveValue(for: property) {
        case .integer(let value): return Double(value)
        case .number(let value): return value
        default: return nil
        }
    }

    private func stringValue(for property: ScenePropertyDefinition) -> String? {
        guard case .string(let value) = effectiveValue(for: property) else { return nil }
        return value
    }

    private func set(_ value: ScenePropertyValue, for property: ScenePropertyDefinition) {
        wallpaperViewModel.setSceneProperty(
            value,
            key: property.key,
            for: screenId,
            wallpaper: wallpaper
        )
    }

    private func formatted(_ value: Double, precision: Int?) -> String {
        let digits = max(0, min(precision ?? 2, 8))
        return String(format: "%.*f", digits, value)
    }

    private func invalidValue(_ property: ScenePropertyDefinition) -> some View {
        Label(
            "\(property.text): invalid or missing runtime value",
            systemImage: "exclamationmark.triangle.fill"
        )
        .foregroundStyle(.red)
        .fixedSize(horizontal: false, vertical: true)
    }
}

private struct SceneRuntimeColor {
    let red: Double
    let green: Double
    let blue: Double
    let alpha: Double
    let componentCount: Int

    init?(string: String) {
        let components = string.split(whereSeparator: \.isWhitespace).compactMap {
            Double($0)
        }
        guard components.count == 3 || components.count == 4,
              components.allSatisfy({ $0.isFinite && (0...1).contains($0) }) else {
            return nil
        }
        red = components[0]
        green = components[1]
        blue = components[2]
        alpha = components.count == 4 ? components[3] : 1
        componentCount = components.count
    }

    init?(color: Color, componentCount: Int) {
        guard let converted = NSColor(color).usingColorSpace(.deviceRGB) else { return nil }
        red = converted.redComponent
        green = converted.greenComponent
        blue = converted.blueComponent
        alpha = converted.alphaComponent
        self.componentCount = componentCount
    }

    var color: Color {
        Color(
            nsColor: NSColor(
                deviceRed: red,
                green: green,
                blue: blue,
                alpha: alpha
            )
        )
    }

    var encoded: String {
        if componentCount == 4 {
            return String(
                format: "%.6f %.6f %.6f %.6f",
                locale: Locale(identifier: "en_US_POSIX"),
                red,
                green,
                blue,
                alpha
            )
        }
        return String(
            format: "%.6f %.6f %.6f",
            locale: Locale(identifier: "en_US_POSIX"),
            red,
            green,
            blue
        )
    }
}
