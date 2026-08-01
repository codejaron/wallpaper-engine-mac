import Foundation

enum ScenePropertyValue: Codable, Equatable, Hashable {
    case boolean(Bool)
    case integer(Int64)
    case number(Double)
    case string(String)

    private enum CodingKeys: String, CodingKey { case type, value }
    private enum ValueType: String, Codable { case boolean, integer, number, string }

    init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        switch try container.decode(ValueType.self, forKey: .type) {
        case .boolean:
            self = .boolean(try container.decode(Bool.self, forKey: .value))
        case .integer:
            self = .integer(try container.decode(Int64.self, forKey: .value))
        case .number:
            let value = try container.decode(Double.self, forKey: .value)
            guard value.isFinite else {
                throw DecodingError.dataCorruptedError(
                    forKey: .value,
                    in: container,
                    debugDescription: "Scene property numbers must be finite"
                )
            }
            self = .number(value)
        case .string:
            self = .string(try container.decode(String.self, forKey: .value))
        }
    }

    func encode(to encoder: Encoder) throws {
        var container = encoder.container(keyedBy: CodingKeys.self)
        switch self {
        case .boolean(let value):
            try container.encode(ValueType.boolean, forKey: .type)
            try container.encode(value, forKey: .value)
        case .integer(let value):
            try container.encode(ValueType.integer, forKey: .type)
            try container.encode(value, forKey: .value)
        case .number(let value):
            guard value.isFinite else {
                throw EncodingError.invalidValue(
                    value,
                    EncodingError.Context(
                        codingPath: encoder.codingPath,
                        debugDescription: "Scene property numbers must be finite"
                    )
                )
            }
            try container.encode(ValueType.number, forKey: .type)
            try container.encode(value, forKey: .value)
        case .string(let value):
            try container.encode(ValueType.string, forKey: .type)
            try container.encode(value, forKey: .value)
        }
    }
}

enum ScenePropertyKind: Equatable {
    case boolean
    case slider
    case combo
    case color
    case text
    case sceneTexture
    case file
    case directory
    case textInput
    case userShortcut
    case group
}

struct ScenePropertyOption: Equatable, Identifiable {
    let value: String
    let label: String

    var id: String { value }
}

struct ScenePropertyDefinition: Equatable, Identifiable {
    let key: String
    let text: String
    let kind: ScenePropertyKind
    let index: Int?
    let order: Int?
    let minimum: Double?
    let maximum: Double?
    let step: Double?
    let precision: Int?
    let fraction: Bool?
    let isReadOnly: Bool
    let options: [ScenePropertyOption]
    let defaultValue: ScenePropertyValue?

    var id: String { key }
}

struct ScenePropertyCatalog: Equatable {
    let wallpaperIdentity: String
    let properties: [ScenePropertyDefinition]
}

struct ScenePropertyPersistence: Codable, Equatable {
    static let currentVersion = 1

    let version: Int
    var screens: [String: [String: [String: ScenePropertyValue]]]

    init(
        version: Int = currentVersion,
        screens: [String: [String: [String: ScenePropertyValue]]] = [:]
    ) {
        self.version = version
        self.screens = screens
    }

    func values(screenId: String, wallpaperIdentity: String) -> [String: ScenePropertyValue] {
        screens[screenId]?[wallpaperIdentity] ?? [:]
    }

    mutating func set(
        _ value: ScenePropertyValue,
        key: String,
        screenId: String,
        wallpaperIdentity: String
    ) -> Bool {
        var screenValues = screens[screenId] ?? [:]
        var wallpaperValues = screenValues[wallpaperIdentity] ?? [:]
        guard wallpaperValues[key] != value else { return false }
        wallpaperValues[key] = value
        screenValues[wallpaperIdentity] = wallpaperValues
        screens[screenId] = screenValues
        return true
    }

    mutating func reset(screenId: String, wallpaperIdentity: String) -> Bool {
        guard screens[screenId]?[wallpaperIdentity] != nil else { return false }
        screens[screenId]?[wallpaperIdentity] = nil
        if screens[screenId]?.isEmpty == true {
            screens[screenId] = nil
        }
        return true
    }

    mutating func remove(wallpaperIdentity: String) -> Bool {
        var changed = false
        for screenId in Array(screens.keys) where screens[screenId]?[wallpaperIdentity] != nil {
            screens[screenId]?[wallpaperIdentity] = nil
            if screens[screenId]?.isEmpty == true {
                screens[screenId] = nil
            }
            changed = true
        }
        return changed
    }

    mutating func remove(wallpaperDirectory: URL) -> Bool {
        let canonicalDirectory = Self.canonicalPath(wallpaperDirectory)
        var changed = false
        for screenId in Array(screens.keys) {
            let identities = screens[screenId].map { Array($0.keys) } ?? []
            for identity in identities {
                let sceneDirectory = URL(fileURLWithPath: identity).deletingLastPathComponent()
                guard Self.canonicalPath(sceneDirectory) == canonicalDirectory else { continue }
                screens[screenId]?[identity] = nil
                changed = true
            }
            if screens[screenId]?.isEmpty == true {
                screens[screenId] = nil
            }
        }
        return changed
    }

    private static func canonicalPath(_ url: URL) -> String {
        url.standardizedFileURL.resolvingSymlinksInPath().path
    }
}

extension WEWallpaper {
    var scenePropertyIdentity: String {
        wallpaperDirectory
            .appending(path: project.file)
            .standardizedFileURL
            .resolvingSymlinksInPath()
            .path
    }
}
