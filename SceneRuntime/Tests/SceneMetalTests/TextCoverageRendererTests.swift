import XCTest
import SceneMetalTestSupport

final class TextCoverageRendererTests: XCTestCase {
    func testCoverageIsAlphaBlendedAndReused() {
        var pixels = [UInt8](repeating: 0, count: 8 * 8 * 4)
        var cacheCount = 0
        XCTAssertEqual(we_scene_metal_test_render_text(&pixels, pixels.count, &cacheCount), 1)
        XCTAssertEqual(cacheCount, 1)

        // The transformed quad covers the center 4x4 pixels. A fully covered
        // red glyph sample must alter red/green/blue while preserving a
        // non-zero destination alpha; the untouched corner remains exact.
        XCTAssertEqual(Array(pixels[0..<4]), [51, 102, 153, 255])
        let center = (3 * 8 + 3) * 4
        XCTAssertGreaterThan(pixels[center], 51)
        XCTAssertLessThan(pixels[center + 1], 102)
        XCTAssertLessThan(pixels[center + 2], 153)
        XCTAssertGreaterThan(pixels[center + 3], 0)
    }

    func testDynamicCoverageCacheIsBounded() {
        var cacheCount = 0
        XCTAssertEqual(we_scene_metal_test_text_cache_bound(256, &cacheCount), 1)
        XCTAssertEqual(cacheCount, 64)
    }

    func testTopDownCoverageKeepsWallpaperEngineInternalOrientation() {
        var pixels = [UInt8](repeating: 0, count: 2 * 2 * 4)
        XCTAssertEqual(
            we_scene_metal_test_render_text_orientation(&pixels, pixels.count),
            1
        )
        // Metal textures and readback both use a top-left row origin.
        let topLeftAlpha = pixels[3]
        let bottomLeftAlpha = pixels[(2 * 4) + 3]
        XCTAssertGreaterThan(topLeftAlpha, 0)
        XCTAssertEqual(bottomLeftAlpha, 0)
    }
}
