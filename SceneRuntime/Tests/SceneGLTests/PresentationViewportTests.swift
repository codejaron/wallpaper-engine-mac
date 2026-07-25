import SceneGLTestSupport
import SceneRuntimeBridge
import XCTest

final class PresentationViewportTests: XCTestCase {
    func testStretchSlicesAdjacentDisplaysAndMapsPointerThroughCanvas() {
        var left = viewport(canvasWidth: 8, canvasHeight: 4, x: 0, width: 4)
        var right = viewport(canvasWidth: 8, canvasHeight: 4, x: 4, width: 4)
        var leftResult = WESceneGLTestPresentationResult()
        var rightResult = WESceneGLTestPresentationResult()

        XCTAssertEqual(
            transform(
                sourceWidth: 4, sourceHeight: 4, viewport: &left,
                scaling: WE_SCENE_PRESENTATION_STRETCH,
                pointerX: 0.5, pointerY: 0.5, result: &leftResult
            ),
            1
        )
        XCTAssertEqual(
            transform(
                sourceWidth: 4, sourceHeight: 4, viewport: &right,
                scaling: WE_SCENE_PRESENTATION_STRETCH,
                pointerX: 0.5, pointerY: 0.5, result: &rightResult
            ),
            1
        )

        XCTAssertEqual(leftResult.has_content, 1)
        assertRect(leftResult.source, x: 0, y: 0, width: 2, height: 4)
        assertRect(leftResult.destination, x: 0, y: 0, width: 4, height: 4)
        XCTAssertEqual(leftResult.mapped_pointer_x, 0.25, accuracy: 1e-12)
        XCTAssertEqual(leftResult.mapped_pointer_y, 0.5, accuracy: 1e-12)

        XCTAssertEqual(rightResult.has_content, 1)
        assertRect(rightResult.source, x: 2, y: 0, width: 2, height: 4)
        assertRect(rightResult.destination, x: 0, y: 0, width: 4, height: 4)
        XCTAssertEqual(rightResult.mapped_pointer_x, 0.75, accuracy: 1e-12)
        XCTAssertEqual(rightResult.mapped_pointer_y, 0.5, accuracy: 1e-12)
    }

    func testAspectFitDisplayOutsideContentClearsAndClampsPointerToSceneEdge() {
        var display = viewport(
            canvasWidth: 8, canvasHeight: 4, x: 0, width: 2,
            drawableWidth: 2
        )
        var result = WESceneGLTestPresentationResult()

        XCTAssertEqual(
            transform(
                sourceWidth: 4, sourceHeight: 4, viewport: &display,
                scaling: WE_SCENE_PRESENTATION_ASPECT_FIT,
                pointerX: 0.5, pointerY: 0.5, result: &result
            ),
            1
        )
        XCTAssertEqual(result.has_content, 0)
        XCTAssertEqual(result.mapped_pointer_x, 0, accuracy: 1e-12)
        XCTAssertEqual(result.mapped_pointer_y, 0.5, accuracy: 1e-12)

        var pixels = [UInt8](repeating: 255, count: 2 * 4 * 4)
        XCTAssertEqual(
            we_scene_gl_test_blit_presentation_slice(
                &display,
                Int32(WE_SCENE_PRESENTATION_ASPECT_FIT.rawValue),
                &pixels,
                pixels.count
            ),
            1
        )
        XCTAssertEqual(pixels, [UInt8](repeating: 0, count: pixels.count))
    }

    func testAspectFillCropsSourceBeforeSlicingCurrentDisplay() {
        var right = viewport(
            canvasWidth: 4, canvasHeight: 4, x: 2, width: 2,
            drawableWidth: 4
        )
        var result = WESceneGLTestPresentationResult()

        XCTAssertEqual(
            transform(
                sourceWidth: 8, sourceHeight: 4, viewport: &right,
                scaling: WE_SCENE_PRESENTATION_ASPECT_FILL,
                pointerX: 0.5, pointerY: 0.25, result: &result
            ),
            1
        )
        assertRect(result.source, x: 4, y: 0, width: 2, height: 4)
        assertRect(result.destination, x: 0, y: 0, width: 4, height: 4)
        XCTAssertEqual(result.mapped_pointer_x, 0.625, accuracy: 1e-12)
        XCTAssertEqual(result.mapped_pointer_y, 0.25, accuracy: 1e-12)
    }

    func testBlitReadsTheSelectedHalfWithUserVisibleVerticalOrientation() {
        var left = viewport(canvasWidth: 8, canvasHeight: 4, x: 0, width: 4)
        var right = viewport(canvasWidth: 8, canvasHeight: 4, x: 4, width: 4)
        var leftPixels = [UInt8](repeating: 0, count: 4 * 4 * 4)
        var rightPixels = [UInt8](repeating: 0, count: 4 * 4 * 4)

        XCTAssertEqual(
            we_scene_gl_test_blit_presentation_slice(
                &left,
                Int32(WE_SCENE_PRESENTATION_STRETCH.rawValue),
                &leftPixels,
                leftPixels.count
            ),
            1
        )
        XCTAssertEqual(
            we_scene_gl_test_blit_presentation_slice(
                &right,
                Int32(WE_SCENE_PRESENTATION_STRETCH.rawValue),
                &rightPixels,
                rightPixels.count
            ),
            1
        )
        XCTAssertEqual(
            leftPixels,
            repeatedPixel([255, 0, 0, 255], count: 8)
                + repeatedPixel([0, 0, 255, 255], count: 8)
        )
        XCTAssertEqual(
            rightPixels,
            repeatedPixel([0, 255, 0, 255], count: 8)
                + repeatedPixel([255, 255, 0, 255], count: 8)
        )
    }

    func testBlitMapsVerticalDisplaySlicesBeforeFlippingSceneOutput() {
        var bottom = viewport(
            canvasWidth: 4, canvasHeight: 8,
            x: 0, y: 0, width: 4, height: 4,
            drawableWidth: 4, drawableHeight: 4
        )
        var top = viewport(
            canvasWidth: 4, canvasHeight: 8,
            x: 0, y: 4, width: 4, height: 4,
            drawableWidth: 4, drawableHeight: 4
        )
        var bottomPixels = [UInt8](repeating: 0, count: 4 * 4 * 4)
        var topPixels = [UInt8](repeating: 0, count: 4 * 4 * 4)

        XCTAssertEqual(
            we_scene_gl_test_blit_presentation_slice(
                &bottom,
                Int32(WE_SCENE_PRESENTATION_STRETCH.rawValue),
                &bottomPixels,
                bottomPixels.count
            ),
            1
        )
        XCTAssertEqual(
            we_scene_gl_test_blit_presentation_slice(
                &top,
                Int32(WE_SCENE_PRESENTATION_STRETCH.rawValue),
                &topPixels,
                topPixels.count
            ),
            1
        )
        XCTAssertEqual(
            bottomPixels,
            sideBySideRows(
                left: [0, 0, 255, 255],
                right: [255, 255, 0, 255],
                rowCount: 4
            )
        )
        XCTAssertEqual(
            topPixels,
            sideBySideRows(
                left: [255, 0, 0, 255],
                right: [0, 255, 0, 255],
                rowCount: 4
            )
        )
    }

    func testViewportValidationRejectsZeroOutOfBoundsAndOpenGLOverflow() {
        let invalid: [WESceneGLTestPresentationViewport] = [
            viewport(
                canvasWidth: 8, canvasHeight: 4, x: 0, width: 0,
                drawableWidth: 4
            ),
            viewport(
                canvasWidth: 8, canvasHeight: 4, x: 5, width: 4,
                drawableWidth: 4
            ),
            viewport(
                canvasWidth: 8, canvasHeight: 4, x: 0, width: 4,
                drawableWidth: 0
            ),
            viewport(
                canvasWidth: UInt32(Int32.max) + 1, canvasHeight: 4,
                x: 0, width: 1, drawableWidth: 1
            ),
        ]

        for var candidate in invalid {
            var result = WESceneGLTestPresentationResult()
            XCTAssertEqual(
                transform(
                    sourceWidth: 4, sourceHeight: 4, viewport: &candidate,
                    scaling: WE_SCENE_PRESENTATION_STRETCH,
                    pointerX: 0.5, pointerY: 0.5, result: &result
                ),
                0
            )
        }
    }

    private func viewport(
        canvasWidth: UInt32,
        canvasHeight: UInt32,
        x: UInt32,
        y: UInt32 = 0,
        width: UInt32,
        height: UInt32? = nil,
        drawableWidth: UInt32 = 4,
        drawableHeight: UInt32? = nil
    ) -> WESceneGLTestPresentationViewport {
        let viewportHeight = height ?? canvasHeight
        return WESceneGLTestPresentationViewport(
            canvas_width: canvasWidth,
            canvas_height: canvasHeight,
            viewport_x: x,
            viewport_y: y,
            viewport_width: width,
            viewport_height: viewportHeight,
            drawable_width: drawableWidth,
            drawable_height: drawableHeight ?? viewportHeight
        )
    }

    private func transform(
        sourceWidth: UInt32,
        sourceHeight: UInt32,
        viewport: inout WESceneGLTestPresentationViewport,
        scaling: WEScenePresentationScaling,
        pointerX: Double,
        pointerY: Double,
        result: inout WESceneGLTestPresentationResult
    ) -> Int32 {
        we_scene_gl_test_presentation_transform(
            sourceWidth,
            sourceHeight,
            &viewport,
            Int32(scaling.rawValue),
            pointerX,
            pointerY,
            &result
        )
    }

    private func assertRect(
        _ rect: WESceneGLTestPresentationRect,
        x: UInt32,
        y: UInt32,
        width: UInt32,
        height: UInt32,
        file: StaticString = #filePath,
        line: UInt = #line
    ) {
        XCTAssertEqual(rect.x, x, file: file, line: line)
        XCTAssertEqual(rect.y, y, file: file, line: line)
        XCTAssertEqual(rect.width, width, file: file, line: line)
        XCTAssertEqual(rect.height, height, file: file, line: line)
    }

    private func repeatedPixel(_ pixel: [UInt8], count: Int) -> [UInt8] {
        Array(repeating: pixel, count: count).flatMap { $0 }
    }

    private func sideBySideRows(
        left: [UInt8],
        right: [UInt8],
        rowCount: Int
    ) -> [UInt8] {
        Array(
            repeating: repeatedPixel(left, count: 2)
                + repeatedPixel(right, count: 2),
            count: rowCount
        ).flatMap { $0 }
    }
}
