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

    private func rasterize(_ text: String, pointSize: Double = 32) throws -> (WESceneTextTestBitmapRef, WESceneTextTestBitmapInfo) {
        let font = try fontData()
        var error = [CChar](repeating: 0, count: 512)
        let handle = font.withUnsafeBytes { bytes in
            text.withCString { utf8 in
                we_scene_text_test_rasterize_font_bytes(
                    utf8,
                    pointSize,
                    bytes.bindMemory(to: UInt8.self).baseAddress,
                    bytes.count,
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
