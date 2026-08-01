import Foundation
import SwiftSoup
import SwiftUI

struct ScenePropertyTextStyle: OptionSet, Equatable {
    let rawValue: Int

    static let bold = ScenePropertyTextStyle(rawValue: 1 << 0)
    static let italic = ScenePropertyTextStyle(rawValue: 1 << 1)
}

enum ScenePropertyTextAlignment: Equatable {
    case leading
    case center
    case trailing
}

struct ScenePropertyTextRun: Equatable {
    var text: String
    let style: ScenePropertyTextStyle
    let link: URL?
}

struct ScenePropertyTextBlock: Equatable {
    var runs: [ScenePropertyTextRun]
    let headingLevel: Int?
    let alignment: ScenePropertyTextAlignment
}

struct ScenePropertyImageBlock: Equatable {
    let url: URL
    let link: URL?
    let altText: String
    let widthFraction: Double
}

enum ScenePropertyContentBlock: Equatable {
    case text(ScenePropertyTextBlock)
    case image(ScenePropertyImageBlock)
    case divider
}

struct ScenePropertyContent: Equatable {
    let blocks: [ScenePropertyContentBlock]

    var plainText: String {
        blocks.compactMap { block in
            switch block {
            case .text(let text):
                return text.runs.map(\.text).joined()
            case .image(let image):
                return image.altText.isEmpty ? nil : image.altText
            case .divider:
                return nil
            }
        }
        .joined(separator: "\n")
        .trimmingCharacters(in: .whitespacesAndNewlines)
    }
}

enum ScenePropertyContentParsingError: LocalizedError, Equatable {
    case missingDocumentBody
    case markupTooLarge(maximumBytes: Int)
    case nestingTooDeep(maximumDepth: Int)

    var errorDescription: String? {
        switch self {
        case .missingDocumentBody:
            return "The property markup does not contain a document body."
        case .markupTooLarge(let maximumBytes):
            return "The property markup exceeds the \(maximumBytes)-byte limit."
        case .nestingTooDeep(let maximumDepth):
            return "The property markup exceeds the \(maximumDepth)-level nesting limit."
        }
    }
}

struct ScenePropertyContentParser {
    let maximumMarkupBytes: Int
    let maximumNestingDepth: Int

    init(
        maximumMarkupBytes: Int = 256 * 1_024,
        maximumNestingDepth: Int = 64
    ) {
        self.maximumMarkupBytes = maximumMarkupBytes
        self.maximumNestingDepth = maximumNestingDepth
    }

    func parse(_ source: String) throws -> ScenePropertyContent {
        guard source.utf8.count <= maximumMarkupBytes else {
            throw ScenePropertyContentParsingError.markupTooLarge(
                maximumBytes: maximumMarkupBytes
            )
        }
        let document = try SwiftSoup.parseBodyFragment(source)
        guard let body = document.body() else {
            throw ScenePropertyContentParsingError.missingDocumentBody
        }

        var builder = Builder()
        try parse(
            body.childNodesCopy(),
            context: Context(),
            depth: 0,
            into: &builder
        )
        builder.flushText()
        return ScenePropertyContent(blocks: builder.blocks)
    }

    private func parse(
        _ nodes: [Node],
        context: Context,
        depth: Int,
        into builder: inout Builder
    ) throws {
        guard depth <= maximumNestingDepth else {
            throw ScenePropertyContentParsingError.nestingTooDeep(
                maximumDepth: maximumNestingDepth
            )
        }
        for node in nodes {
            if let textNode = node as? TextNode {
                builder.appendText(normalizeWhitespace(textNode.getWholeText()), context: context)
                continue
            }
            guard let element = node as? Element else { continue }

            let tag = element.tagNameNormal().lowercased()
            switch tag {
            case "script", "style", "iframe", "object", "embed", "video", "audio",
                 "form", "input", "button", "meta", "link":
                continue
            case "br":
                builder.appendLineBreak(context: context)
            case "hr":
                builder.appendDivider()
            case "img":
                let altText = try element.attr("alt")
                    .trimmingCharacters(in: .whitespacesAndNewlines)
                guard let url = secureURL(try element.attr("src")) else {
                    if !altText.isEmpty {
                        builder.appendText(altText, context: context)
                    }
                    continue
                }
                builder.appendImage(ScenePropertyImageBlock(
                    url: url,
                    link: context.link,
                    altText: altText,
                    widthFraction: widthFraction(try element.attr("width"))
                ))
            case "a":
                var childContext = context
                childContext.link = secureURL(try element.attr("href"))
                try parseChildren(element, context: childContext, depth: depth, into: &builder)
            case "b", "strong":
                var childContext = context
                childContext.style.insert(.bold)
                try parseChildren(element, context: childContext, depth: depth, into: &builder)
            case "i", "em":
                var childContext = context
                childContext.style.insert(.italic)
                try parseChildren(element, context: childContext, depth: depth, into: &builder)
            case "center":
                builder.flushText()
                var childContext = context
                childContext.alignment = .center
                try parseChildren(element, context: childContext, depth: depth, into: &builder)
                builder.flushText()
            case "h1", "h2", "h3", "h4", "h5", "h6":
                builder.flushText()
                var childContext = context
                childContext.headingLevel = Int(tag.dropFirst())
                try parseChildren(element, context: childContext, depth: depth, into: &builder)
                builder.flushText()
            case "p", "div", "section", "article", "header", "footer", "ul", "ol":
                builder.flushText()
                try parseChildren(element, context: context, depth: depth, into: &builder)
                builder.flushText()
            case "li":
                builder.flushText()
                builder.appendText("• ", context: context)
                try parseChildren(element, context: context, depth: depth, into: &builder)
                builder.flushText()
            default:
                try parseChildren(element, context: context, depth: depth, into: &builder)
            }
        }
    }

    private func parseChildren(
        _ element: Element,
        context: Context,
        depth: Int,
        into builder: inout Builder
    ) throws {
        try parse(
            element.childNodesCopy(),
            context: context,
            depth: depth + 1,
            into: &builder
        )
    }

    private func secureURL(_ source: String) -> URL? {
        let trimmed = source.trimmingCharacters(in: .whitespacesAndNewlines)
        guard let url = URL(string: trimmed),
              url.scheme?.lowercased() == "https",
              url.host != nil else {
            return nil
        }
        return url
    }

    private func widthFraction(_ source: String) -> Double {
        let trimmed = source.trimmingCharacters(in: .whitespacesAndNewlines)
        guard trimmed.hasSuffix("%"),
              let percentage = Double(trimmed.dropLast()),
              percentage.isFinite else {
            return 1
        }
        return min(max(percentage / 100, 0.1), 1)
    }

    private func normalizeWhitespace(_ source: String) -> String {
        guard !source.isEmpty else { return "" }
        let components = source.split(whereSeparator: \.isWhitespace)
        guard !components.isEmpty else { return " " }

        var result = components.map(String.init).joined(separator: " ")
        if source.first?.isWhitespace == true { result.insert(" ", at: result.startIndex) }
        if source.last?.isWhitespace == true { result.append(" ") }
        return result
    }
}

private extension ScenePropertyContentParser {
    struct Context {
        var style: ScenePropertyTextStyle = []
        var link: URL?
        var headingLevel: Int?
        var alignment = ScenePropertyTextAlignment.leading
    }

    struct Builder {
        var blocks: [ScenePropertyContentBlock] = []
        private var runs: [ScenePropertyTextRun] = []
        private var headingLevel: Int?
        private var alignment = ScenePropertyTextAlignment.leading

        mutating func appendText(_ source: String, context: Context) {
            guard !source.isEmpty else { return }
            prepareTextBlock(for: context)

            var text = source
            if runs.isEmpty {
                text = text.trimmingCharacters(in: .whitespaces)
            } else if runs.last?.text.last?.isWhitespace == true,
                      text.first?.isWhitespace == true {
                text.removeFirst()
            }
            guard !text.isEmpty else { return }

            if runs.last?.style == context.style, runs.last?.link == context.link {
                runs[runs.count - 1].text.append(text)
            } else {
                runs.append(ScenePropertyTextRun(
                    text: text,
                    style: context.style,
                    link: context.link
                ))
            }
        }

        mutating func appendLineBreak(context: Context) {
            guard !runs.isEmpty else { return }
            if runs.last?.text.last != "\n" {
                appendText("\n", context: context)
            }
        }

        mutating func appendImage(_ image: ScenePropertyImageBlock) {
            flushText()
            blocks.append(.image(image))
        }

        mutating func appendDivider() {
            flushText()
            blocks.append(.divider)
        }

        mutating func flushText() {
            guard !runs.isEmpty else { return }
            runs[0].text = runs[0].text.trimmingCharacters(in: .whitespacesAndNewlines)
            runs[runs.count - 1].text = runs[runs.count - 1].text
                .trimmingCharacters(in: .whitespacesAndNewlines)
            runs.removeAll(where: { $0.text.isEmpty })
            if !runs.isEmpty {
                blocks.append(.text(ScenePropertyTextBlock(
                    runs: runs,
                    headingLevel: headingLevel,
                    alignment: alignment
                )))
            }
            runs.removeAll(keepingCapacity: true)
            headingLevel = nil
            alignment = .leading
        }

        private mutating func prepareTextBlock(for context: Context) {
            if !runs.isEmpty,
               (headingLevel != context.headingLevel || alignment != context.alignment) {
                flushText()
            }
            if runs.isEmpty {
                headingLevel = context.headingLevel
                alignment = context.alignment
            }
        }
    }
}

enum ScenePropertyParsedContent {
    case content(ScenePropertyContent)
    case error(String)
}

enum ScenePropertyContentResolver {
    private final class Box {
        let value: ScenePropertyParsedContent

        init(_ value: ScenePropertyParsedContent) {
            self.value = value
        }
    }

    private static let cache = NSCache<NSString, Box>()

    static func resolve(_ source: String) -> ScenePropertyParsedContent {
        let localized = Bundle.main.localizedString(forKey: source, value: nil, table: nil)
        let resolvedSource = localized == source ? source : localized
        let key = resolvedSource as NSString
        if let cached = cache.object(forKey: key) { return cached.value }

        let parsed: ScenePropertyParsedContent
        do {
            parsed = .content(try ScenePropertyContentParser().parse(resolvedSource))
        } catch {
            parsed = .error(error.localizedDescription)
        }
        cache.setObject(Box(parsed), forKey: key)
        return parsed
    }

    static func plainText(_ source: String) -> String {
        switch resolve(source) {
        case .content(let content):
            return content.plainText
        case .error(let message):
            return "Invalid property markup: \(message)"
        }
    }
}

struct ScenePropertyContentView: View {
    private let parsed: ScenePropertyParsedContent
    private let fillsWidth: Bool

    init(_ source: String, fillsWidth: Bool = true) {
        parsed = ScenePropertyContentResolver.resolve(source)
        self.fillsWidth = fillsWidth
    }

    var body: some View {
        switch parsed {
        case .content(let content):
            VStack(alignment: .leading, spacing: 4) {
                ForEach(Array(content.blocks.enumerated()), id: \.offset) { _, block in
                    switch block {
                    case .text(let text):
                        ScenePropertyTextBlockView(block: text, fillsWidth: fillsWidth)
                    case .image(let image):
                        ScenePropertyRemoteImageView(image: image)
                    case .divider:
                        Divider()
                    }
                }
            }
        case .error(let message):
            Label("Invalid property markup: \(message)", systemImage: "exclamationmark.triangle.fill")
                .foregroundStyle(.red)
                .fixedSize(horizontal: false, vertical: true)
        }
    }
}

private struct ScenePropertyTextBlockView: View {
    let block: ScenePropertyTextBlock
    let fillsWidth: Bool

    var body: some View {
        let text = Text(attributedString)
            .multilineTextAlignment(block.alignment.textAlignment)
            .fixedSize(horizontal: false, vertical: true)

        Group {
            if fillsWidth {
                text.frame(maxWidth: .infinity, alignment: block.alignment.frameAlignment)
            } else {
                text
            }
        }
        .font(block.headingLevel.font)
    }

    private var attributedString: AttributedString {
        var result = AttributedString()
        for run in block.runs {
            var segment = AttributedString(run.text)
            var intent: InlinePresentationIntent = []
            if run.style.contains(.bold) { intent.insert(.stronglyEmphasized) }
            if run.style.contains(.italic) { intent.insert(.emphasized) }
            if !intent.isEmpty { segment.inlinePresentationIntent = intent }
            segment.link = run.link
            result.append(segment)
        }
        return result
    }
}

private struct ScenePropertyRemoteImageView: View {
    let image: ScenePropertyImageBlock
    @StateObject private var loader = RemoteImageLoader()

    var body: some View {
        Group {
            switch loader.state {
            case .loaded(let loadedImage):
                if let link = image.link {
                    Link(destination: link) {
                        renderedImage(loadedImage)
                    }
                    .buttonStyle(.plain)
                } else {
                    renderedImage(loadedImage)
                }
            case .failed(let message):
                VStack(spacing: 5) {
                    Image(systemName: "exclamationmark.triangle")
                        .foregroundStyle(.secondary)
                    Text(image.altText.isEmpty ? "Remote property image failed to load" : image.altText)
                        .font(.caption)
                        .multilineTextAlignment(.center)
                    Text(message)
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                        .multilineTextAlignment(.center)
                    Button("Retry") {
                        Task { await loader.retry() }
                    }
                    .buttonStyle(.borderless)
                    .controlSize(.small)
                }
                .frame(maxWidth: .infinity, minHeight: 60)
            case .idle, .loading:
                ProgressView()
                    .controlSize(.small)
                    .frame(maxWidth: .infinity, minHeight: 60)
            }
        }
        .task(id: image.url) {
            await loader.load(url: image.url)
        }
    }

    private func renderedImage(_ loadedImage: NSImage) -> some View {
        GifImage(image: loadedImage, animates: true)
            .resizable()
            .aspectRatio(contentMode: .fit)
            .frame(maxWidth: .infinity)
            .containerRelativeFrame(.horizontal, alignment: .center) { width, _ in
                width * CGFloat(image.widthFraction)
            }
            .accessibilityLabel(image.altText.isEmpty ? "Remote property image" : image.altText)
    }
}

private extension ScenePropertyTextAlignment {
    var textAlignment: TextAlignment {
        switch self {
        case .leading: return .leading
        case .center: return .center
        case .trailing: return .trailing
        }
    }

    var frameAlignment: Alignment {
        switch self {
        case .leading: return .leading
        case .center: return .center
        case .trailing: return .trailing
        }
    }
}

private extension Optional where Wrapped == Int {
    var font: Font? {
        switch self {
        case 1: return .title2
        case 2: return .title3
        case 3, 4: return .headline
        case 5: return .subheadline
        case 6: return .caption
        case nil: return nil
        default: return .body
        }
    }
}
