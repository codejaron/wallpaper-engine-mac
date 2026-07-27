import Foundation
import SceneTextTestSupport
import XCTest

final class SceneTextTests: XCTestCase {
    private func fontData() throws -> Data {
        let url = try XCTUnwrap(Bundle.module.url(
            forResource: "Silkscreen-Regular", withExtension: "ttf", subdirectory: "Fixtures"
        ))
        return try Data(contentsOf: url)
    }

    private func rasterize(
        _ text: String,
        pointSize: Double = 32,
        maximumWidth: Double = 0,
        maximumRows: Int = 0,
        useEllipsis: Bool = false,
        characterSpacing: Double = 0,
        lineSpacing: Double = 0,
        horizontalAlignment: Int32 = 1
    ) throws -> (WESceneTextTestBitmapRef, WESceneTextTestBitmapInfo) {
        let font = try fontData()
        var error = [CChar](repeating: 0, count: 512)
        var layout = WESceneTextTestLayoutOptions(
            maximum_width: maximumWidth,
            maximum_rows: maximumRows,
            use_ellipsis: useEllipsis ? 1 : 0,
            character_spacing: characterSpacing,
            line_spacing: lineSpacing,
            horizontal_alignment: horizontalAlignment
        )
        let handle = font.withUnsafeBytes { bytes in
            text.withCString { utf8 in
                we_scene_text_test_rasterize_font_bytes_with_layout(
                    utf8,
                    pointSize,
                    bytes.bindMemory(to: UInt8.self).baseAddress,
                    bytes.count,
                    &layout,
                    &error,
                    error.count
                )
            }
        }
        guard let handle else {
            XCTFail("Rasterization failed: \(String(cString: error))")
            throw NSError(domain: "SceneTextTests", code: 1)
        }
        var info = WESceneTextTestBitmapInfo()
        XCTAssertEqual(we_scene_text_test_bitmap_info(handle, &info), 1)
        return (handle, info)
    }

    func testOpenSourceFontBytesRasterizeUnicodeToCoverage() throws {
        let (handle, info) = try rasterize("Café")
        defer { we_scene_text_test_bitmap_destroy(handle) }
        XCTAssertGreaterThan(info.width, 1)
        XCTAssertGreaterThan(info.height, 1)
        XCTAssertEqual(info.bytes_per_row, info.width)
        XCTAssertEqual(info.coverage_size, Int(info.width * info.height))
        let coverage = UnsafeBufferPointer(start: info.coverage, count: info.coverage_size)
        XCTAssertTrue(coverage.contains { $0 != 0 })
        XCTAssertGreaterThan(info.ascent, 0)
        XCTAssertGreaterThan(info.baseline_from_top, 0)
        XCTAssertEqual(info.line_count, 1)
        XCTAssertEqual(info.truncated, 0)
    }

    func testExplicitLineBreakProducesMultipleRasterRows() throws {
        let (singleHandle, singleInfo) = try rasterize("AAAA")
        defer { we_scene_text_test_bitmap_destroy(singleHandle) }
        let (multilineHandle, multilineInfo) = try rasterize("AA\nAA")
        defer { we_scene_text_test_bitmap_destroy(multilineHandle) }

        XCTAssertGreaterThan(multilineInfo.height, singleInfo.height)
        XCTAssertLessThan(multilineInfo.width, singleInfo.width)
        XCTAssertEqual(multilineInfo.line_count, 2)
        XCTAssertEqual(multilineInfo.truncated, 0)
    }

    func testWidthLimitWrapsAtCoreTextLineBreaks() throws {
        let (singleHandle, singleInfo) = try rasterize("AAAA AAAA")
        defer { we_scene_text_test_bitmap_destroy(singleHandle) }
        let (wrappedHandle, wrappedInfo) = try rasterize(
            "AAAA AAAA",
            maximumWidth: singleInfo.typographic_width * 0.6
        )
        defer { we_scene_text_test_bitmap_destroy(wrappedHandle) }

        XCTAssertGreaterThan(wrappedInfo.line_count, 1)
        XCTAssertGreaterThan(wrappedInfo.height, singleInfo.height)
        XCTAssertLessThan(wrappedInfo.width, singleInfo.width)
    }

    func testRowLimitTruncatesAndEllipsisChangesVisibleCoverage() throws {
        let text = "AAAA AAAA AAAA"
        let (measuredHandle, measuredInfo) = try rasterize("AAAA")
        defer { we_scene_text_test_bitmap_destroy(measuredHandle) }
        let width = measuredInfo.typographic_width + 1
        let (clippedHandle, clippedInfo) = try rasterize(
            text,
            maximumWidth: width,
            maximumRows: 2
        )
        defer { we_scene_text_test_bitmap_destroy(clippedHandle) }
        let (ellipsisHandle, ellipsisInfo) = try rasterize(
            text,
            maximumWidth: width,
            maximumRows: 2,
            useEllipsis: true
        )
        defer { we_scene_text_test_bitmap_destroy(ellipsisHandle) }

        XCTAssertEqual(clippedInfo.line_count, 2)
        XCTAssertEqual(clippedInfo.truncated, 1)
        XCTAssertEqual(ellipsisInfo.line_count, 2)
        XCTAssertEqual(ellipsisInfo.truncated, 1)
        XCTAssertNotEqual(
            Data(bytes: clippedInfo.coverage, count: clippedInfo.coverage_size),
            Data(bytes: ellipsisInfo.coverage, count: ellipsisInfo.coverage_size)
        )
    }

    func testCharacterAndLineSpacingChangeTheCorrespondingAxis() throws {
        let (plainHandle, plainInfo) = try rasterize("AAAA")
        defer { we_scene_text_test_bitmap_destroy(plainHandle) }
        let (spacedHandle, spacedInfo) = try rasterize(
            "AAAA", characterSpacing: 4
        )
        defer { we_scene_text_test_bitmap_destroy(spacedHandle) }
        XCTAssertGreaterThan(spacedInfo.typographic_width, plainInfo.typographic_width)

        let (plainLinesHandle, plainLinesInfo) = try rasterize("AA\nAA")
        defer { we_scene_text_test_bitmap_destroy(plainLinesHandle) }
        let (spacedLinesHandle, spacedLinesInfo) = try rasterize(
            "AA\nAA", lineSpacing: 8
        )
        defer { we_scene_text_test_bitmap_destroy(spacedLinesHandle) }
        XCTAssertGreaterThan(spacedLinesInfo.height, plainLinesInfo.height)
    }

    func testHorizontalAlignmentPositionsShortLinesInsideTheTextBlock() throws {
        let (leftHandle, leftInfo) = try rasterize(
            "AAAA\nA", horizontalAlignment: 0
        )
        defer { we_scene_text_test_bitmap_destroy(leftHandle) }
        let (rightHandle, rightInfo) = try rasterize(
            "AAAA\nA", horizontalAlignment: 2
        )
        defer { we_scene_text_test_bitmap_destroy(rightHandle) }

        XCTAssertEqual(leftInfo.width, rightInfo.width)
        XCTAssertEqual(leftInfo.height, rightInfo.height)
        XCTAssertNotEqual(
            Data(bytes: leftInfo.coverage, count: leftInfo.coverage_size),
            Data(bytes: rightInfo.coverage, count: rightInfo.coverage_size)
        )
    }

    func testRasterizationIsDeterministicAndIndependentOfRenderColor() throws {
        let (firstHandle, firstInfo) = try rasterize("Coverage cache")
        defer { we_scene_text_test_bitmap_destroy(firstHandle) }
        let (secondHandle, secondInfo) = try rasterize("Coverage cache")
        defer { we_scene_text_test_bitmap_destroy(secondHandle) }
        XCTAssertEqual(firstInfo.width, secondInfo.width)
        XCTAssertEqual(firstInfo.height, secondInfo.height)
        XCTAssertEqual(
            Data(bytes: firstInfo.coverage, count: firstInfo.coverage_size),
            Data(bytes: secondInfo.coverage, count: secondInfo.coverage_size)
        )
        // SceneText intentionally exposes coverage only. Color and alpha remain
        // renderer inputs and therefore cannot invalidate this raster result.
    }

    func testEmptyTextReturnsTransparentOnePixelBitmap() throws {
        let (handle, info) = try rasterize("")
        defer { we_scene_text_test_bitmap_destroy(handle) }
        XCTAssertEqual(info.width, 1)
        XCTAssertEqual(info.height, 1)
        XCTAssertEqual(info.coverage_size, 1)
        XCTAssertEqual(info.coverage?.pointee, 0)
    }

    func testCorruptFontFailsExplicitly() {
        let corrupt = Data("not a font".utf8)
        var error = [CChar](repeating: 0, count: 512)
        let handle = corrupt.withUnsafeBytes { bytes in
            "visible failure".withCString { utf8 in
                we_scene_text_test_rasterize_font_bytes(
                    utf8, 32,
                    bytes.bindMemory(to: UInt8.self).baseAddress,
                    bytes.count, &error, error.count
                )
            }
        }
        XCTAssertNil(handle)
        XCTAssertTrue(String(cString: error).contains("font"))
    }

    func testEmptyTextStillValidatesFontBytes() {
        let corrupt = Data("not a font".utf8)
        var error = [CChar](repeating: 0, count: 512)
        let handle = corrupt.withUnsafeBytes { bytes in
            "".withCString { utf8 in
                we_scene_text_test_rasterize_font_bytes(
                    utf8, 32,
                    bytes.bindMemory(to: UInt8.self).baseAddress,
                    bytes.count, &error, error.count
                )
            }
        }
        XCTAssertNil(handle)
        XCTAssertTrue(String(cString: error).contains("font"))
    }

    func testMissingSystemFontFailsInsteadOfFallingBack() {
        var error = [CChar](repeating: 0, count: 512)
        let handle = "text".withCString { utf8 in
            "DefinitelyMissingWallpaperEngineFont_7A1C".withCString { font in
                we_scene_text_test_rasterize_system_font(
                    utf8, 32, font, &error, error.count
                )
            }
        }
        XCTAssertNil(handle)
        XCTAssertTrue(String(cString: error).contains("does not exist"))
    }

    func testInvalidPointSizeFailsExplicitly() throws {
        let font = try fontData()
        var error = [CChar](repeating: 0, count: 512)
        let handle = font.withUnsafeBytes { bytes in
            "text".withCString { utf8 in
                we_scene_text_test_rasterize_font_bytes(
                    utf8, 0,
                    bytes.bindMemory(to: UInt8.self).baseAddress,
                    bytes.count, &error, error.count
                )
            }
        }
        XCTAssertNil(handle)
        XCTAssertTrue(String(cString: error).contains("point size"))
    }
}
