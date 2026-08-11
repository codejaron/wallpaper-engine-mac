import Foundation
import SceneRuntimeBridge
import XCTest

final class FrameExecutorPixelTests: XCTestCase {
    func testSamplerMetadataAndOptimizedOutVertexAttributeUseTextureBindingContract() throws {
        let loaded = try loadFixture(
            fragmentSource: """
            uniform sampler2D g_Texture0; // {"material":"mask","hidden":true}
            void main() {
                gl_FragColor = texSample2D(g_Texture0, vec2(0.5, 0.5));
            }
            """,
            vertexSource: """
            attribute vec3 a_Position;
            attribute vec2 a_TexCoord;
            void main() {
                gl_Position = vec4(a_Position, 1.0);
            }
            """
        )
        defer { destroy(loaded) }

        try render(loaded.executor)
        XCTAssertEqual(
            try readPixels(loaded.executor),
            repeatedPixel([255, 0, 0, 255])
        )
    }

    func testSamplerMetadataDefaultBindsMissingFrameTextureSlot() throws {
        let loaded = try loadFixture(
            fragmentSource: """
            uniform sampler2D g_Texture1; // {"default":"green"}
            void main() {
                gl_FragColor = texSample2D(g_Texture1, vec2(0.5, 0.5));
            }
            """,
            includeUnboundGreenTexture: true
        )
        defer { destroy(loaded) }

        try render(loaded.executor)
        XCTAssertEqual(
            try readPixels(loaded.executor),
            repeatedPixel([0, 255, 0, 255]),
            "A sampler metadata default must bind its declared texture instead of silently sampling texture unit zero"
        )
    }

    func testResolutionOnlyTextureSlotUsesThePassInputProvider() throws {
        let loaded = try loadFixture(
            fragmentSource: """
            varying vec4 v_ResolutionDelta;
            void main() {
                bool ok = all(lessThan(
                    abs(v_ResolutionDelta),
                    vec4(0.0001)
                ));
                gl_FragColor = ok ? vec4(0.0, 1.0, 0.0, 1.0)
                                  : vec4(1.0, 0.0, 0.0, 1.0);
            }
            """,
            vertexSource: """
            uniform mat4 g_ModelViewProjectionMatrix;
            uniform vec4 g_Texture0Resolution;
            uniform vec4 g_Texture1Resolution;
            attribute vec3 a_Position;
            attribute vec2 a_TexCoord;
            varying vec4 v_ResolutionDelta;
            void main() {
                v_ResolutionDelta = g_Texture1Resolution
                    - g_Texture0Resolution;
                gl_Position = g_ModelViewProjectionMatrix
                    * vec4(a_Position, 1.0);
            }
            """
        )
        defer { destroy(loaded) }

        try render(loaded.executor)
        try assertNoExecutorIssues(loaded.executor)
        XCTAssertEqual(
            try readPixels(loaded.executor),
            repeatedPixel([0, 255, 0, 255]),
            "A resolution-only optional slot must inherit the real pass input instead of remaining unbound"
        )
    }

    func testFragmentSamplerDefaultOverridesVertexDefault() throws {
        let loaded = try loadFixture(
            fragmentSource: """
            uniform sampler2D g_Texture1; // {"default":"green"}
            void main() {
                gl_FragColor = texSample2D(g_Texture1, vec2(0.5, 0.5));
            }
            """,
            vertexSource: """
            uniform sampler2D g_Texture1; // {"default":"red"}
            attribute vec3 a_Position;
            attribute vec2 a_TexCoord;
            void main() {
                gl_Position = vec4(a_Position, 1.0);
            }
            """,
            includeUnboundGreenTexture: true
        )
        defer { destroy(loaded) }

        try render(loaded.executor)
        XCTAssertEqual(
            try readPixels(loaded.executor),
            repeatedPixel([0, 255, 0, 255]),
            "Fragment metadata is later in the Linux provider chain and must override the vertex default"
        )
    }

    func testUnavailableFragmentSamplerDefaultFallsBackToVertexDefault() throws {
        let loaded = try loadFixture(
            fragmentSource: """
            uniform sampler2D g_Texture1; // {"default":"texture_that_is_not_available"}
            void main() {
                gl_FragColor = texSample2D(g_Texture1, vec2(0.5, 0.5));
            }
            """,
            vertexSource: """
            uniform sampler2D g_Texture1; // {"default":"red"}
            attribute vec3 a_Position;
            attribute vec2 a_TexCoord;
            void main() {
                gl_Position = vec4(a_Position, 1.0);
            }
            """
        )
        defer { destroy(loaded) }

        try render(loaded.executor)
        XCTAssertEqual(
            try readPixels(loaded.executor),
            repeatedPixel([255, 0, 0, 255]),
            "An unavailable fragment default must not suppress an earlier ready provider"
        )
    }

    func testSceneTexturePropertyParticipatesInTheTextureProviderChain() throws {
        let loaded = try loadFixture(
            fragmentSource: textureOnlyFragmentShader,
            includeUnboundGreenTexture: true,
            sceneTexturePropertyKey: "selected_texture"
        )
        defer { destroy(loaded) }

        try render(loaded.executor, timeSeconds: 1)
        XCTAssertEqual(
            try readPixels(loaded.executor),
            repeatedPixel([255, 0, 0, 255]),
            "An unbound scene-texture property must leave the ordinary material texture available"
        )

        try setString(
            loaded.model,
            key: "selected_texture",
            value: "green"
        )
        try render(loaded.executor, timeSeconds: 2)
        XCTAssertEqual(
            try readPixels(loaded.executor),
            repeatedPixel([0, 255, 0, 255]),
            "A ready scene-texture property must override the ordinary material texture in the same slot"
        )

        try setString(
            loaded.model,
            key: "selected_texture",
            value: "texture_that_is_not_available"
        )
        try render(loaded.executor, timeSeconds: 3)
        XCTAssertEqual(
            try readPixels(loaded.executor),
            repeatedPixel([255, 0, 0, 255]),
            "An unavailable high-priority provider must fall through to the next ready candidate"
        )
    }

    func testMediaThumbnailTexturesUseRealCurrentAndPreviousHostPixels() throws {
        let loaded = try loadFixture(
            fragmentSource: """
            uniform sampler2D g_Texture0;
            uniform sampler2D g_Texture1;
            void main() {
                vec4 current = texSample2D(g_Texture0, vec2(0.5, 0.5));
                vec4 previous = texSample2D(g_Texture1, vec2(0.5, 0.5));
                gl_FragColor = vec4(
                    current.g,
                    previous.r,
                    current.b + previous.g * 0.5,
                    1.0
                );
            }
            """,
            hostTextureNames: [
                "$mediaThumbnail",
                "$mediaPreviousThumbnail",
            ]
        )
        defer { destroy(loaded) }

        try render(loaded.executor, timeSeconds: 1)
        XCTAssertEqual(
            try readPixels(loaded.executor),
            repeatedPixel([0, 255, 0, 255]),
            "Absent host pixels must keep the ordinary material texture as the provider fallback"
        )

        try setMediaThumbnail(
            loaded.executor,
            revision: 1,
            pixel: [0, 255, 0, 255],
            overwriteBorrowedPixelsAfterSet: true
        )
        try render(loaded.executor, timeSeconds: 2)
        XCTAssertEqual(
            try readPixels(loaded.executor),
            repeatedPixel([255, 255, 0, 255]),
            "The current cover must sample copied host pixels while previous still falls back"
        )

        try setMediaThumbnail(
            loaded.executor,
            revision: 2,
            pixel: [0, 0, 255, 255]
        )
        try render(loaded.executor, timeSeconds: 3)
        XCTAssertEqual(
            try readPixels(loaded.executor),
            repeatedPixel([0, 0, 255, 255]),
            "A new album-art revision must preserve the first cover as $mediaPreviousThumbnail"
        )

        var error: WESceneRuntimeErrorRef?
        XCTAssertEqual(
            we_scene_frame_executor_clear_media_thumbnail(
                loaded.executor, 3, &error
            ),
            1,
            errorMessage(error)
        )
        XCTAssertNil(error)
        try render(loaded.executor, timeSeconds: 4)
        XCTAssertEqual(
            try readPixels(loaded.executor),
            repeatedPixel([0, 0, 0, 255]),
            "Clearing a new cover revision must expose the normal fallback as current and retain the last cover as previous"
        )
    }

    func testMediaThumbnailRevisionIdentifiesImmutablePixels() throws {
        let loaded = try loadFixture(fragmentSource: constantRedFragmentShader)
        defer { destroy(loaded) }

        try setMediaThumbnail(
            loaded.executor,
            revision: 7,
            pixel: [0, 255, 0, 255]
        )
        try setMediaThumbnail(
            loaded.executor,
            revision: 7,
            pixel: [0, 255, 0, 255]
        )

        XCTAssertThrowsError(
            try setMediaThumbnail(
                loaded.executor,
                revision: 7,
                pixel: [0, 0, 255, 255]
            )
        ) { error in
            XCTAssertTrue(error.localizedDescription.contains("immutable"))
        }

        var error: WESceneRuntimeErrorRef?
        XCTAssertEqual(
            we_scene_frame_executor_clear_media_thumbnail(
                loaded.executor, 7, &error
            ),
            0
        )
        XCTAssertTrue(errorMessage(error).contains("immutable"))
        we_scene_runtime_error_destroy(error)
    }

    func testActiveSamplerWithoutFrameTextureOrDefaultSkipsObject() throws {
        let loaded = try loadFixture(
            fragmentSource: """
            uniform sampler2D g_Texture1; // {"hidden":true}
            void main() {
                gl_FragColor = texSample2D(g_Texture1, vec2(0.5, 0.5));
            }
            """
        )
        defer { destroy(loaded) }

        try render(loaded.executor)
        XCTAssertEqual(try readPixels(loaded.executor), repeatedPixel([0, 0, 0, 255]))
        try assertSingleSkippedObjectIssue(
            loaded.executor,
            objectIndex: 0,
            objectId: 1,
            operationIndex: 0,
            messageContains: ["g_Texture1", "slot 1"]
        )
    }

    func testActiveIntegerSamplerSkipsOnlyItsObject() throws {
        let loaded = try loadFixture(
            fragmentSource: """
            uniform sampler2D g_Texture0;
            uniform isampler2D g_Texture1;
            varying vec2 v_TexCoord;
            void main() {
                if (texture(g_Texture1, v_TexCoord).x == 123456789) {
                    discard;
                }
                gl_FragColor = texture(g_Texture0, v_TexCoord);
            }
            """,
            includeHealthySecondImage: true
        )
        defer { destroy(loaded) }

        try render(loaded.executor)
        XCTAssertEqual(
            try readPixels(loaded.executor),
            repeatedPixel([0, 255, 0, 255]),
            "An unsupported integer sampler must not suppress a later healthy object"
        )
        try assertSingleSkippedObjectIssue(
            loaded.executor,
            objectIndex: 0,
            objectId: 1,
            operationIndex: 0,
            messageContains: ["sampler", "unsupported"]
        )
    }

    func testRuntimeSamplerDefaultIsNotRewrittenAsAnAssetTexture() throws {
        let loaded = try loadFixture(
            fragmentSource: """
            uniform sampler2D g_Texture1; // {"default":"_rt_FullFrameBuffer"}
            void main() {
                gl_FragColor = texSample2D(g_Texture1, vec2(0.5, 0.5));
            }
            """
        )
        defer { destroy(loaded) }

        try render(loaded.executor)
        XCTAssertEqual(try readPixels(loaded.executor), repeatedPixel([0, 0, 0, 255]))
        try assertSingleSkippedObjectIssue(
            loaded.executor,
            objectIndex: 0,
            objectId: 1,
            operationIndex: 0,
            messageContains: ["g_Texture1", "render destination", "_rt_FullFrameBuffer"]
        )
    }

    func testUnusedPositionAttributeFollowsLinuxOptionalBindingContract() throws {
        let loaded = try loadFixture(
            fragmentSource: constantRedFragmentShader,
            vertexSource: """
            void main() {
                gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
            }
            """
        )
        defer { destroy(loaded) }

        try render(loaded.executor)
        XCTAssertEqual(try readPixels(loaded.executor), repeatedPixel([0, 0, 0, 255]))
        var issueCount = 99
        var error: WESceneRuntimeErrorRef?
        XCTAssertEqual(
            we_scene_frame_executor_issue_count(
                loaded.executor, &issueCount, &error
            ),
            1,
            errorMessage(error)
        )
        XCTAssertEqual(issueCount, 0)
        XCTAssertNil(error)
    }

    func testLazyMultiTextureUploadPreservesEarlierTextureSlots() throws {
        let loaded = try loadFixture(
            fragmentSource: textureOnlyFragmentShader,
            includeSecondTexture: true
        )
        defer { destroy(loaded) }

        try render(loaded.executor)
        let first = try readPixels(loaded.executor)
        XCTAssertEqual(first, repeatedPixel([255, 0, 0, 255]))

        try render(loaded.executor)
        XCTAssertEqual(
            try readPixels(loaded.executor),
            first,
            "Lazy texture upload must not make the first frame differ from cached frames"
        )
    }

    func testEmbeddedPNGPreservesTopDownImageOrientation() throws {
        let loaded = try loadFixture(
            fragmentSource: textureOnlyFragmentShader,
            textureData: makeEmbeddedPNGTexture2x2()
        )
        defer { destroy(loaded) }

        try render(loaded.executor)
        XCTAssertEqual(
            try readPixels(loaded.executor),
            [255, 0, 0, 255, 255, 0, 0, 255,
             0, 0, 255, 255, 0, 0, 255, 255]
        )
    }

    func testRawRGBA8PreservesTheSameTopDownOrientationAsEmbeddedPNG() throws {
        let loaded = try loadFixture(
            fragmentSource: textureOnlyFragmentShader,
            textureData: makeRGBA8Texture2x2(pixels: [
                255, 0, 0, 255, 255, 0, 0, 255,
                0, 0, 255, 255, 0, 0, 255, 255,
            ])
        )
        defer { destroy(loaded) }

        try render(loaded.executor)
        XCTAssertEqual(
            try readPixels(loaded.executor),
            [255, 0, 0, 255, 255, 0, 0, 255,
             0, 0, 255, 255, 0, 0, 255, 255]
        )
    }

    func testPrimaryRawTextureFormatOverridesAuthoredTextureZeroCombo() throws {
        let cases: [(format: UInt32, authored: Int, expected: Int, bytes: [UInt8])] = [
            (format: 9, authored: 8, expected: 9, bytes: [255, 255, 255, 255]),
            (format: 8, authored: 9, expected: 8, bytes: [255, 0, 255, 0, 255, 0, 255, 0]),
        ]
        for value in cases {
            let loaded = try loadFixture(
                fragmentSource: """
                varying vec2 v_TexCoord;
                void main() {
                #if TEX0FORMAT == \(value.expected)
                    gl_FragColor = vec4(0.0, 1.0, 0.0, 1.0);
                #else
                    gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0);
                #endif
                }
                """,
                textureData: makeRawTexture2x2(
                    format: value.format,
                    bytes: value.bytes
                ),
                baseCombos: ["TEX0FORMAT": value.authored]
            )
            defer { destroy(loaded) }

            try render(loaded.executor)
            XCTAssertEqual(
                try readPixels(loaded.executor),
                repeatedPixel([0, 255, 0, 255]),
                "The primary raw texture format must replace an authored TEX0FORMAT value"
            )
        }
    }

    func testEffectPassTextureZeroFormatUsesRenderablePrimaryTexture() throws {
        let loaded = try loadFixture(
            fragmentSource: """
            varying vec2 v_TexCoord;
            void main() {
            #if TEX0FORMAT == 9
                gl_FragColor = vec4(0.0, 1.0, 0.0, 1.0);
            #else
                gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0);
            #endif
            }
            """,
            commandMode: .copy,
            textureData: makeRawTexture2x2(
                format: 9,
                bytes: [255, 255, 255, 255]
            ),
            baseCombos: ["TEX0FORMAT": 8]
        )
        defer { destroy(loaded) }

        try render(loaded.executor)
        XCTAssertEqual(
            try readPixels(loaded.executor),
            repeatedPixel([0, 255, 0, 255]),
            "Effect passes must inherit TEX0FORMAT from the renderable primary texture, not their framebuffer input"
        )
    }

    func testMagentaCompositeTintRunsBeforeColorBlendCompatibilityPass() throws {
        let loaded = try loadFixture(
            fragmentSource: constantGreenFragmentShader,
            compositeTintColor: "1 0 1",
            colorBlendMode: 7
        )
        defer { destroy(loaded) }

        try render(loaded.executor)
        try assertNoExecutorIssues(loaded.executor)
        XCTAssertEqual(
            try readPixels(loaded.executor),
            repeatedPixel([0, 255, 255, 255]),
            "The magenta COMPOSITE=2 tint must feed the later colorBlendMode compatibility pass"
        )
    }

    func testImageScenePlaneSurvivesPositiveNearClip() throws {
        let loaded = try loadFixture(
            fragmentSource: constantRedFragmentShader,
            nearPlane: 0.01
        )
        defer { destroy(loaded) }

        try render(loaded.executor)
        XCTAssertEqual(try readPixels(loaded.executor), repeatedPixel([255, 0, 0, 255]))
    }

    func testPuppetMeshVersionsRenderAnIndexedPartialImage() throws {
        for version in ["MDLV0013", "MDLV0021", "MDLV0023"] {
            let loaded = try loadFixture(
                fragmentSource: constantRedFragmentShader,
                puppetData: makePuppetMesh(version: version)
            )
            defer { destroy(loaded) }

            try render(loaded.executor)
            let pixels = try readPixels(loaded.executor)
            let redPixels = stride(from: 0, to: pixels.count, by: 4).filter {
                pixels[$0] == 255 && pixels[$0 + 1] == 0 &&
                    pixels[$0 + 2] == 0 && pixels[$0 + 3] == 255
            }
            XCTAssertGreaterThan(redPixels.count, 0, "\(version) must draw its mesh")
            XCTAssertLessThan(redPixels.count, 4, "\(version) must not fall back to a full quad")
        }
    }

    func testPuppetAnimationLayerDeformsWeightedVerticesAsTimeAdvances() throws {
        let loaded = try loadFixture(
            fragmentSource: constantRedFragmentShader,
            projectionWidth: 4,
            projectionHeight: 2,
            imageOrigin: "2 1 0",
            imageSize: "2 2",
            puppetData: makeAnimatedPuppetMesh(),
            puppetAnimationLayer: [
                "additive": false,
                "animation": 7,
                "blend": 1.0,
                "id": 11,
                "name": "Translation",
                "rate": 1.0,
                "visible": true,
            ]
        )
        defer { destroy(loaded) }

        try render(loaded.executor, timeSeconds: 0)
        let initialPixels = try readPixels(loaded.executor)
        try render(loaded.executor, timeSeconds: 0.5)
        let deformedPixels = try readPixels(loaded.executor)

        XCTAssertNotEqual(
            deformedPixels,
            initialPixels,
            "An active animation layer must deform the mesh through its bone weights"
        )
        XCTAssertTrue(containsOpaqueRedPixel(initialPixels))
        XCTAssertTrue(containsOpaqueRedPixel(deformedPixels))
    }

    func testPuppetMeshUsesTheFirstValidBlockWhenPayloadContainsAnotherBlock() throws {
        // Linux stops at the first structurally valid MDLV block. Keep a
        // second valid block in the payload to guard against accidentally
        // rejecting multi-block assets as ambiguous.
        var payload = makePuppetMesh(version: "MDLV0021")
        payload.removeLast(4) // Remove the first block's MDLS terminator.
        payload.append(makePuppetMesh(version: "MDLV0023"))

        let loaded = try loadFixture(
            fragmentSource: constantRedFragmentShader,
            puppetData: payload
        )
        defer { destroy(loaded) }

        try render(loaded.executor)
        let pixels = try readPixels(loaded.executor)
        let redPixels = stride(from: 0, to: pixels.count, by: 4).filter {
            pixels[$0] == 255 && pixels[$0 + 1] == 0 &&
                pixels[$0 + 2] == 0 && pixels[$0 + 3] == 255
        }
        XCTAssertGreaterThan(redPixels.count, 0)
        XCTAssertLessThan(redPixels.count, 4)
    }

    func testMalformedPuppetMeshesFailDuringModelLoading() throws {
        var truncated = makePuppetMesh()
        truncated.removeLast(7)
        try assertPuppetModelLoadFails(truncated, containing: "mesh block")

        var badStride = makePuppetMesh()
        replaceUInt32(79, at: 13, in: &badStride)
        try assertPuppetModelLoadFails(badStride, containing: "mesh block")

        var badIndices = makePuppetMesh()
        replaceUInt16(9, at: badIndices.count - 6, in: &badIndices)
        try assertPuppetModelLoadFails(badIndices, containing: "outside the mesh")

        var badIndexLength = makePuppetMesh()
        replaceUInt32(4, at: 257, in: &badIndexLength)
        try assertPuppetModelLoadFails(badIndexLength, containing: "mesh block")

        var nonFinite = makePuppetMesh()
        replaceUInt32(0x7fc00000, at: 17, in: &nonFinite)
        try assertPuppetModelLoadFails(nonFinite, containing: "non-finite")

        var unsupported = makePuppetMesh()
        unsupported.replaceSubrange(0..<8, with: Array("MDLV0099".utf8))
        try assertPuppetModelLoadFails(unsupported, containing: "Unsupported puppet model header")
    }

    private enum CommandMode {
        case copy
        case assetCopy
        case swap
        case invalidCopy
        case uninitializedRead
        case persistentRead
        case atomicPersistentFailure
        case proceduralClear
    }
    private struct Fixture {
        let root: URL
        let assets: URL
        let package: URL
    }

    private struct RuntimePipeline {
        let fixture: Fixture
        let runtime: WESceneRuntimeRef
        let model: WESceneModelRef
        let graph: WESceneGraphRef
        let frameGraph: WESceneFrameGraphRef
        let executor: WESceneFrameExecutorRef
    }

    private struct ExecutorIssueSnapshot: Equatable {
        let severity: Int
        let objectIndex: Int
        let objectId: Int32
        let operationIndex: Int
        let message: String
    }

    func testExecutorRendersPackageTextureAndReflectsPropertyRevision() throws {
        let loaded = try loadFixture(fragmentSource: validFragmentShader)
        defer { destroy(loaded) }

        XCTAssertEqual(we_scene_frame_executor_width(loaded.executor), 2)
        XCTAssertEqual(we_scene_frame_executor_height(loaded.executor), 2)
        XCTAssertEqual(we_scene_frame_executor_rgba8_byte_count(loaded.executor), 16)
        try render(loaded.executor)
        let initialRevision = try lastRevision(loaded.executor)
        XCTAssertEqual(try readPixels(loaded.executor), repeatedPixel([255, 0, 0, 255]))

        try setNumber(loaded.model, key: "amount", value: 1)
        try render(loaded.executor)
        XCTAssertGreaterThan(
            try lastRevision(loaded.executor),
            initialRevision
        )
        XCTAssertEqual(try readPixels(loaded.executor), repeatedPixel([0, 255, 0, 255]))
    }

    func testUploadedAssetTextureRemainsReadyWithoutBackingFileReread() throws {
        let loaded = try loadFixture(
            fragmentSource: validFragmentShader,
            textureInProjectDirectory: true
        )
        defer { destroy(loaded) }

        try render(loaded.executor, timeSeconds: 1)
        XCTAssertEqual(
            try readPixels(loaded.executor),
            repeatedPixel([255, 0, 0, 255])
        )

        try FileManager.default.removeItem(
            at: loaded.fixture.root
                .appendingPathComponent("materials", isDirectory: true)
                .appendingPathComponent("red.tex")
        )

        try render(loaded.executor, timeSeconds: 2)
        XCTAssertEqual(
            try readPixels(loaded.executor),
            repeatedPixel([255, 0, 0, 255]),
            "A frame must reuse the texture already uploaded to the executor instead of resolving and reading its immutable backing asset again"
        )
    }

    func testDynamicImageCloneExecutesAsDistinctRuntimeObject() throws {
        let loaded = try loadFixture(
            fragmentSource: validFragmentShader,
            textureAnimationScript: """
            export function init(value) {
                const config = thisScene.getInitialLayerConfig(thisLayer);
                thisScene.createLayer(config);
                return value;
            }
            """
        )
        defer { destroy(loaded) }

        try render(loaded.executor)
        try assertNoExecutorIssues(loaded.executor)
        XCTAssertEqual(
            try readPixels(loaded.executor),
            repeatedPixel([255, 0, 0, 255])
        )
    }

    func testLinuxCommonBuiltinContractIsBoundForImagePasses() throws {
        let loaded = try loadFixture(fragmentSource: commonBuiltinContractFragmentShader)
        defer { destroy(loaded) }

        try render(loaded.executor)
        try assertNoExecutorIssues(loaded.executor)
        XCTAssertEqual(
            try readPixels(loaded.executor),
            repeatedPixel([0, 255, 0, 255]),
            "The Linux common builtin contract must be populated before an image draw"
        )
    }

    func testWallpaperEngineImageBuiltinContractIsBoundForImagePasses() throws {
        let loaded = try loadFixture(fragmentSource: imageBuiltinContractFragmentShader)
        defer { destroy(loaded) }

        try render(loaded.executor)
        try assertNoExecutorIssues(loaded.executor)
        XCTAssertEqual(
            try readPixels(loaded.executor),
            repeatedPixel([0, 255, 0, 255]),
            "Metal must explicitly preserve the image uniforms supplied by the previous OpenGL path"
        )
    }

    func testLayerAlphaIsPackedIntoColor4ForVersionedImageShaders() throws {
        let loaded = try loadFixture(
            fragmentSource: """
            uniform float g_UserAlpha;
            uniform vec4 g_Color4;
            varying vec2 v_TexCoord;
            void main() {
                bool alphaMatches = abs(g_UserAlpha - 0.625) < 0.0001
                    && abs(g_Color4.a - 0.625) < 0.0001;
                gl_FragColor = alphaMatches
                    ? vec4(0.0, 1.0, 0.0, 1.0)
                    : vec4(1.0, 0.0, 0.0, 1.0);
            }
            """,
            scriptedPointerAlpha: true
        )
        defer { destroy(loaded) }

        try render(
            loaded.executor,
            pointerX: 0.5,
            pointerY: 0.5,
            timeSeconds: 1
        )
        XCTAssertEqual(
            try readPixels(loaded.executor),
            repeatedPixel([0, 255, 0, 255]),
            "Both legacy and versioned image shader opacity uniforms must carry the evaluated layer alpha"
        )
    }

    func testLinuxCommonUniformsOverrideMaterialDefaultsExceptCompositeColor() throws {
        let loaded = try loadFixture(
            fragmentSource: commonUniformPrecedenceFragmentShader
        )
        defer { destroy(loaded) }

        try render(loaded.executor)
        XCTAssertEqual(
            try readPixels(loaded.executor),
            repeatedPixel([0, 255, 0, 255]),
            "Linux common values must win after material setup while an authored composite color remains intact"
        )
    }

    func testPreviousPointerBuiltinTracksTheLastPublishedFrame() throws {
        let loaded = try loadFixture(fragmentSource: previousPointerFragmentShader)
        defer { destroy(loaded) }

        try render(
            loaded.executor,
            pointerX: 0.25,
            pointerY: 0.75,
            timeSeconds: 1
        )
        try render(
            loaded.executor,
            pointerX: 0.75,
            pointerY: 0.25,
            timeSeconds: 2
        )
        XCTAssertEqual(
            try readPixels(loaded.executor),
            repeatedPixel([191, 64, 64, 255]),
            "g_PointerPositionLast must expose the previous successful frame in Linux's top-left effect coordinates"
        )
    }

    func testPreviousPointerBuiltinSurvivesAnInvalidatedFrame() throws {
        let loaded = try loadFixture(fragmentSource: previousPointerFragmentShader)
        defer { destroy(loaded) }

        try render(
            loaded.executor,
            pointerX: 0.25,
            pointerY: 0.75,
            timeSeconds: 1
        )

        var error: WESceneRuntimeErrorRef?
        var invalidInputs = WESceneFrameInputs(
            pointer_x: 1,
            pointer_y: 1,
            time_seconds: -1,
            frame_time_seconds: 1.0 / 60.0
        )
        XCTAssertEqual(
            we_scene_frame_executor_render(
                loaded.executor, &invalidInputs, &error
            ),
            0
        )
        XCTAssertEqual(
            we_scene_runtime_error_code(error),
            WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT
        )
        we_scene_runtime_error_destroy(error)

        try render(
            loaded.executor,
            pointerX: 0.75,
            pointerY: 0.25,
            timeSeconds: 2
        )
        XCTAssertEqual(
            try readPixels(loaded.executor),
            repeatedPixel([191, 64, 64, 255]),
            "A frame failure must invalidate replay state without resetting the Linux pointer history"
        )
    }

    func testUnavailableLinuxAudioBuiltinSkipsOnlyItsObject() throws {
        let loaded = try loadFixture(
            fragmentSource: audioBuiltinFragmentShader,
            includeHealthySecondImage: true
        )
        defer { destroy(loaded) }

        try render(loaded.executor)
        XCTAssertEqual(
            try readPixels(loaded.executor),
            repeatedPixel([0, 255, 0, 255]),
            "Unavailable audio input must not suppress a later healthy object"
        )
        try assertSingleSkippedObjectIssue(
            loaded.executor,
            objectIndex: 0,
            objectId: 1,
            operationIndex: 0,
            messageContains: ["audio", "unavailable"]
        )
    }

    func testProvidedLinuxAudioSpectrumBindsBuiltinArray() throws {
        let loaded = try loadFixture(fragmentSource: audioBuiltinFragmentShader)
        defer { destroy(loaded) }

        try renderWithAudioSpectrum(
            loaded.executor,
            spectrum16Left: [Float](repeating: 0.5, count: 16)
        )
        try assertNoExecutorIssues(loaded.executor)
        XCTAssertEqual(
            try readPixels(loaded.executor),
            repeatedPixel([128, 0, 0, 255]),
            "A real host-provided spectrum frame must bind the Linux audio builtin instead of using a silent fallback"
        )
    }

    func testUniformAudioArrayMayBePassedToVertexFunction() throws {
        let loaded = try loadFixture(
            fragmentSource: """
            varying float v_Level;
            void main() {
                gl_FragColor = vec4(v_Level, 0.0, 0.0, 1.0);
            }
            """,
            vertexSource: """
            uniform mat4 g_ModelViewProjectionMatrix;
            uniform float g_AudioSpectrum16Left[16];
            attribute vec3 a_Position;
            attribute vec2 a_TexCoord;
            varying float v_Level;
            float firstSpectrumValue(float values[16]) {
                return values[0];
            }
            void main() {
                v_Level = firstSpectrumValue(g_AudioSpectrum16Left);
                gl_Position = g_ModelViewProjectionMatrix
                    * vec4(a_Position, 1.0);
            }
            """
        )
        defer { destroy(loaded) }

        try renderWithAudioSpectrum(
            loaded.executor,
            spectrum16Left: [Float](repeating: 0.5, count: 16)
        )
        try assertNoExecutorIssues(loaded.executor)
        XCTAssertEqual(
            try readPixels(loaded.executor),
            repeatedPixel([128, 0, 0, 255]),
            "An official-style uniform audio array parameter must preserve the Metal constant-buffer ABI"
        )
    }

    func testProvidedAudioSpectrumFlowsThroughSceneScriptIntoMaterialValue() throws {
        let loaded = try loadFixture(
            fragmentSource: validFragmentShader,
            scriptedAudioAmount: true
        )
        defer { destroy(loaded) }

        var left = [Float](repeating: 0, count: 16)
        left[0] = 1
        try renderWithAudioSpectrum(loaded.executor, spectrum16Left: left)
        try assertNoExecutorIssues(loaded.executor)
        XCTAssertEqual(
            try readPixels(loaded.executor),
            repeatedPixel([128, 128, 0, 255]),
            "The GL host spectrum must reach SceneGraph scripts before the material plan is built"
        )
    }

    func testAudioScriptMutatingOriginYKeepsRenderedXStableAcrossFrames() throws {
        let loaded = try loadFixture(
            fragmentSource: constantRedFragmentShader,
            projectionWidth: 8,
            projectionHeight: 8,
            imageOrigin: "2 2 0",
            imageSize: "2 2",
            scriptedAudioOrigin: true
        )
        defer { destroy(loaded) }

        try renderWithAudioSpectrum(
            loaded.executor,
            spectrum16Left: [Float](repeating: 0, count: 16)
        )
        let quiet = try XCTUnwrap(redPixelBounds(
            try readPixels(loaded.executor),
            width: 8
        ))

        var loudSpectrum = [Float](repeating: 0, count: 16)
        loudSpectrum[0] = 1
        try renderWithAudioSpectrum(
            loaded.executor,
            spectrum16Left: loudSpectrum
        )
        let loud = try XCTUnwrap(redPixelBounds(
            try readPixels(loaded.executor),
            width: 8
        ))

        XCTAssertEqual(loud.minX, quiet.minX)
        XCTAssertEqual(loud.maxX, quiet.maxX)
        XCTAssertNotEqual(loud.minY, quiet.minY)
        XCTAssertNotEqual(loud.maxY, quiet.maxY)
    }

    func testMediaSnapshotCopiesBorrowedStateAndDeduplicatesRevision() throws {
        let loaded = try loadFixture(
            fragmentSource: validFragmentShader,
            scriptedMediaAmount: true
        )
        defer { destroy(loaded) }

        try setMediaSnapshot(
            loaded.executor,
            revision: 1,
            playbackState: WE_SCENE_MEDIA_PLAYING,
            title: "Borrowed Song",
            artist: "Borrowed Artist",
            position: 1.25,
            duration: 4.5,
            hasThumbnail: true,
            overwriteBorrowedStringsAfterSet: true
        )
        try render(loaded.executor, timeSeconds: 1)
        XCTAssertEqual(
            try readPixels(loaded.executor),
            repeatedPixel([0, 255, 0, 255]),
            "The bridge must copy borrowed strings and deliver all media fields to SceneScript"
        )

        try setMediaSnapshot(
            loaded.executor,
            revision: 1,
            playbackState: WE_SCENE_MEDIA_STOPPED,
            title: "Same revision must be ignored",
            artist: "Changed",
            position: 0,
            duration: 0,
            hasThumbnail: false
        )
        try render(loaded.executor, timeSeconds: 2)
        XCTAssertEqual(
            try readPixels(loaded.executor),
            repeatedPixel([0, 255, 0, 255]),
            "A repeated media revision must not dispatch duplicate callbacks"
        )

        try setMediaSnapshot(
            loaded.executor,
            revision: 2,
            playbackState: WE_SCENE_MEDIA_PAUSED,
            title: "Borrowed Song",
            artist: "Borrowed Artist",
            position: 1.25,
            duration: 4.5,
            hasThumbnail: true
        )
        try render(loaded.executor, timeSeconds: 3)
        XCTAssertEqual(
            try readPixels(loaded.executor),
            repeatedPixel([128, 128, 0, 255]),
            "A new paused revision must run one real script evaluation"
        )
    }

    func testBoundAnimationsUseTheSameContractAcrossOwnersAndMediaEvents() throws {
        for owner in ["layer", "material"] {
            let loaded = try loadFixture(
                fragmentSource: constantRedFragmentShader,
                scriptedMediaBoundAlphaAnimation: owner == "layer",
                scriptedMediaBoundMaterialAnimation: owner == "material"
            )
            defer { destroy(loaded) }

            try setMediaSnapshot(
                loaded.executor,
                revision: 1,
                playbackState: WE_SCENE_MEDIA_PLAYING,
                title: "Bound animation",
                artist: "Contract test",
                position: 0,
                duration: 1,
                hasThumbnail: true
            )
            try render(loaded.executor, timeSeconds: 1)
            try assertNoExecutorIssues(loaded.executor)
        }
    }

    func testProvidedLinuxAudioSpectrumBindsHighestActiveElement() throws {
        let loaded = try loadFixture(
            fragmentSource: audioBuiltinLastElementFragmentShader
        )
        defer { destroy(loaded) }

        var spectrum = [Float](repeating: 0, count: 16)
        spectrum[15] = 0.75
        try renderWithAudioSpectrum(
            loaded.executor,
            spectrum16Left: spectrum
        )
        try assertNoExecutorIssues(loaded.executor)
        XCTAssertEqual(
            try readPixels(loaded.executor),
            repeatedPixel([191, 0, 0, 255]),
            "The complete host spectrum must be available when a shader activates the final array element"
        )
    }

    func testImageAnglesUseAuthoredRadiansWithoutASecondConversion() throws {
        let loaded = try loadFixture(
            fragmentSource: imageAngleContractFragmentShader,
            imageAngles: "0 0 1.5707963267948966"
        )
        defer { destroy(loaded) }

        try render(loaded.executor)
        XCTAssertEqual(
            try readPixels(loaded.executor),
            repeatedPixel([0, 255, 0, 255]),
            "Scene graph angles are already radians and must not be converted again"
        )
    }

    func testAlignedImageAnglesRotateAroundTheBakedSceneCenter() throws {
        let loaded = try loadFixture(
            fragmentSource: alignedImageAngleContractFragmentShader,
            imageAngles: "0 0 1.5707963267948966",
            imageAlignment: "topleft"
        )
        defer { destroy(loaded) }

        try render(loaded.executor)
        let pixels = try readPixels(loaded.executor)
        let greenPixels = Set(
            stride(from: 0, to: pixels.count, by: 4).compactMap { offset in
                pixels[offset] == 0 && pixels[offset + 1] == 255
                    ? offset / 4 : nil
            }
        )
        XCTAssertEqual(
            greenPixels,
            Set([3]),
            "Aligned images must rotate around the center of their baked scene geometry"
        )
    }

    func testAutomaticProjectionUsesTheSameBottomUpOriginAsFixedProjection() throws {
        let loaded = try loadFixture(
            fragmentSource: automaticOriginContractFragmentShader,
            projectionAuto: true,
            imageOrigin: "3 2 0",
            imageSize: "2 2"
        )
        defer { destroy(loaded) }

        try render(loaded.executor)
        let pixels = try readPixels(loaded.executor)
        XCTAssertEqual(we_scene_frame_executor_width(loaded.executor), 8)
        XCTAssertEqual(we_scene_frame_executor_height(loaded.executor), 6)
        let greenPixels = Set(
            stride(from: 0, to: pixels.count, by: 4).compactMap { offset in
                pixels[offset] == 0 && pixels[offset + 1] == 255
                    ? offset / 4 : nil
            }
        )
        XCTAssertEqual(
            greenPixels,
            Set([26, 27, 34, 35]),
            "An auto-projected image must bake its bottom-up origin into centered scene geometry"
        )
    }

    func testParallaxSmoothsPointerAcrossFramesAndMovesImageGeometry() throws {
        let loaded = try loadFixture(
            fragmentSource: textureOnlyFragmentShader,
            parallax: true,
            parallaxDepth: "1 0"
        )
        defer { destroy(loaded) }

        try render(
            loaded.executor,
            pointerX: 0.5,
            pointerY: 0.5,
            timeSeconds: 0,
            frameTimeSeconds: 1
        )
        XCTAssertEqual(
            try readPixels(loaded.executor),
            [255, 0, 0, 255, 0, 0, 0, 255,
             255, 0, 0, 255, 0, 0, 0, 255]
        )

        try render(
            loaded.executor,
            pointerX: 1,
            pointerY: 0.5,
            timeSeconds: 1,
            frameTimeSeconds: 0
        )
        XCTAssertEqual(
            try readPixels(loaded.executor),
            [255, 0, 0, 255, 0, 0, 0, 255,
             255, 0, 0, 255, 0, 0, 0, 255]
        )

        try render(
            loaded.executor,
            pointerX: 1,
            pointerY: 0.5,
            timeSeconds: 2,
            frameTimeSeconds: 2
        )
        XCTAssertEqual(
            try readPixels(loaded.executor),
            [0, 0, 0, 255, 255, 0, 0, 255,
             0, 0, 0, 255, 255, 0, 0, 255]
        )
    }

    func testParallaxBuiltinBindsDelayedNormalizedPositionInVertexStage() throws {
        let loaded = try loadFixture(
            fragmentSource: parallaxPositionContractFragmentShader,
            vertexSource: parallaxPositionContractVertexShader,
            parallax: true,
            imageOrigin: "1 1 0",
            imageSize: "2 2",
            parallaxDepth: "0 0"
        )
        defer { destroy(loaded) }

        try render(
            loaded.executor,
            pointerX: 0.5,
            pointerY: 0.5,
            timeSeconds: 0,
            frameTimeSeconds: 1
        )
        try render(
            loaded.executor,
            pointerX: 1,
            pointerY: 0,
            timeSeconds: 1,
            frameTimeSeconds: 1
        )

        XCTAssertEqual(
            try readPixels(loaded.executor),
            repeatedPixel([0, 255, 0, 255]),
            "g_ParallaxPosition must use the delayed global parallax displacement and be bound for vertex shaders"
        )
    }

    func testZeroDepthFullscreenImageDoesNotExposeClearColorDuringParallax() throws {
        let loaded = try loadFixture(
            fragmentSource: constantRedFragmentShader,
            parallax: true,
            imageOrigin: "1 1 0",
            imageSize: "2 2",
            parallaxDepth: "0 0"
        )
        defer { destroy(loaded) }

        try render(
            loaded.executor,
            pointerX: 1,
            pointerY: 0.5,
            timeSeconds: 1,
            frameTimeSeconds: 2
        )

        XCTAssertEqual(
            try readPixels(loaded.executor),
            repeatedPixel([255, 0, 0, 255]),
            "A zero-depth fullscreen background must remain fixed while parallax layers move"
        )
    }

    func testAspectFillMapsScriptAndShaderPointersToTheirLinuxCoordinates() throws {
        let loaded = try loadFixture(
            fragmentSource: pointerContractFragmentShader,
            projectionWidth: 5,
            projectionHeight: 2,
            imageSize: "5 2",
            scriptedPointerAlpha: true
        )
        defer { destroy(loaded) }

        var error: WESceneRuntimeErrorRef?
        var inputs = WESceneFrameInputs(
            pointer_x: 0,
            pointer_y: 0.25,
            time_seconds: 1,
            frame_time_seconds: 1.0 / 60.0
        )
        XCTAssertEqual(
            we_scene_frame_executor_render_for_drawable(
                loaded.executor,
                &inputs,
                2,
                2,
                WE_SCENE_PRESENTATION_ASPECT_FILL,
                &error
            ),
            1,
            errorMessage(error)
        )

        let scenePointer = (x: 1.0 / 5.0, y: 0.25)
        XCTAssertEqual(
            try readPixels(loaded.executor),
            repeatedPixel(pointerContractPixel(scenePointer), count: 10),
            "The crop must give scripts bottom-left (0.2, 0.25) while g_PointerPosition receives Linux's top-left (0.2, 0.75)"
        )
    }

    func testAspectFillMapsBottomLeftHostPointerToLinuxParallaxY() throws {
        let loaded = try loadFixture(
            fragmentSource: constantRedFragmentShader,
            parallax: true,
            projectionWidth: 2,
            projectionHeight: 5,
            imageOrigin: "1 3 0",
            imageSize: "2 1",
            parallaxDepth: "0 1"
        )
        defer { destroy(loaded) }

        var error: WESceneRuntimeErrorRef?
        var inputs = WESceneFrameInputs(
            pointer_x: 0.5,
            pointer_y: 1,
            time_seconds: 1,
            frame_time_seconds: 2
        )
        XCTAssertEqual(
            we_scene_frame_executor_render_for_drawable(
                loaded.executor,
                &inputs,
                2,
                2,
                WE_SCENE_PRESENTATION_ASPECT_FILL,
                &error
            ),
            1,
            errorMessage(error)
        )

        XCTAssertEqual(
            try readPixels(loaded.executor),
            repeatedPixel([0, 0, 0, 255], count: 2) +
                repeatedPixel([255, 0, 0, 255], count: 2) +
                repeatedPixel([0, 0, 0, 255], count: 6),
            "The crop maps the host pointer through Linux parallax while preserving the bottom-up scene Y axis"
        )
    }

    func testBottomLeftPointerMappingAcrossDirectStretchAndAspectFitRendering() throws {
        let loaded = try loadFixture(
            fragmentSource: pointerContractFragmentShader,
            projectionWidth: 5,
            projectionHeight: 2,
            imageSize: "5 2",
            scriptedPointerAlpha: true
        )
        defer { destroy(loaded) }

        var error: WESceneRuntimeErrorRef?
        var inputs = WESceneFrameInputs(
            pointer_x: 0.4,
            pointer_y: 0.25,
            time_seconds: 1,
            frame_time_seconds: 1.0 / 60.0
        )
        XCTAssertEqual(
            we_scene_frame_executor_render(loaded.executor, &inputs, &error),
            1,
            errorMessage(error)
        )
        XCTAssertEqual(
            try readPixels(loaded.executor),
            repeatedPixel(pointerContractPixel((x: 0.4, y: 0.25)), count: 10),
            "Direct rendering must keep the script pointer bottom-left and derive the shader's top-left Y"
        )

        inputs.pointer_x = -0.5
        inputs.pointer_y = 1.5
        inputs.time_seconds = 2
        XCTAssertEqual(
            we_scene_frame_executor_render_for_drawable(
                loaded.executor,
                &inputs,
                3,
                4,
                WE_SCENE_PRESENTATION_STRETCH,
                &error
            ),
            1,
            errorMessage(error)
        )
        XCTAssertEqual(
            try readPixels(loaded.executor),
            repeatedPixel(pointerContractPixel((x: 0, y: 1)), count: 10),
            "Stretch rendering must clamp the host/script pointer to bottom-left (0, 1) and the shader pointer to top-left (0, 0)"
        )

        inputs.pointer_x = 0.4
        inputs.pointer_y = 0
        inputs.time_seconds = 3
        XCTAssertEqual(
            we_scene_frame_executor_render_for_drawable(
                loaded.executor,
                &inputs,
                5,
                4,
                WE_SCENE_PRESENTATION_ASPECT_FIT,
                &error
            ),
            1,
            errorMessage(error)
        )
        XCTAssertEqual(
            try readPixels(loaded.executor),
            repeatedPixel(pointerContractPixel((x: 0.4, y: 0)), count: 10),
            "The lower aspect-fit bar must clamp scripts to bottom-left Y=0 and shaders to top-left Y=1"
        )

        inputs.pointer_x = 0.75
        inputs.time_seconds = 4
        XCTAssertEqual(
            we_scene_frame_executor_render_for_drawable(
                loaded.executor,
                &inputs,
                1,
                UInt32(Int32.max),
                WE_SCENE_PRESENTATION_ASPECT_FIT,
                &error
            ),
            1,
            errorMessage(error)
        )
        XCTAssertEqual(
            try readPixels(loaded.executor),
            repeatedPixel(pointerContractPixel((x: 0.75, y: 0)), count: 10),
            "Extreme aspect-fit letterboxing must preserve finite script and effect pointer coordinates"
        )
    }

    func testAutomaticProjectionReplayPreservesFrozenBottomLeftPointerOnlyForReplay() throws {
        let loaded = try loadFixture(
            fragmentSource: pointerContractFragmentShader,
            projectionAuto: true,
            drawableSizedSolidLayer: true,
            scriptedPointerAlpha: true
        )
        defer { destroy(loaded) }

        var error: WESceneRuntimeErrorRef?
        var inputs = WESceneFrameInputs(
            pointer_x: 0.25,
            pointer_y: 0.25,
            time_seconds: 1,
            frame_time_seconds: 1.0 / 60.0
        )
        XCTAssertEqual(
            we_scene_frame_executor_render_for_drawable(
                loaded.executor,
                &inputs,
                4,
                2,
                WE_SCENE_PRESENTATION_ASPECT_FILL,
                &error
            ),
            1,
            errorMessage(error)
        )
        XCTAssertEqual(
            try readPixels(loaded.executor),
            repeatedPixel(pointerContractPixel((x: 0.25, y: 0.25)), count: 8),
            "The initial frame must evaluate the bottom-left pointer (0.25, 0.25)"
        )

        XCTAssertEqual(
            we_scene_frame_executor_replay_for_drawable(
                loaded.executor, 2, 4, &error
            ),
            1,
            errorMessage(error)
        )
        XCTAssertEqual(we_scene_frame_executor_width(loaded.executor), 2)
        XCTAssertEqual(we_scene_frame_executor_height(loaded.executor), 4)
        XCTAssertEqual(
            try readPixels(loaded.executor),
            repeatedPixel(pointerContractPixel((x: 0.25, y: 0.25)), count: 8),
            "Replay must preserve the frozen bottom-left pointer while resizing the automatic projection"
        )

        inputs.pointer_x = 0.75
        inputs.pointer_y = 0.75
        inputs.time_seconds = 2
        XCTAssertEqual(
            we_scene_frame_executor_render_for_drawable(
                loaded.executor,
                &inputs,
                2,
                4,
                WE_SCENE_PRESENTATION_ASPECT_FILL,
                &error
            ),
            1,
            errorMessage(error)
        )
        XCTAssertEqual(
            try readPixels(loaded.executor),
            repeatedPixel(pointerContractPixel((x: 0.75, y: 0.75)), count: 8),
            "The first normal render after replay must evaluate the new bottom-left host pointer"
        )
    }

    func testResizeReplayPreservesFrozenIssueAndFailureInvalidatesIssueSnapshot() throws {
        let loaded = try loadFixture(
            fragmentSource: validFragmentShader,
            commandMode: .invalidCopy,
            projectionAuto: true,
            drawableSizedSolidLayer: true
        )
        defer { destroy(loaded) }

        var error: WESceneRuntimeErrorRef?
        var inputs = WESceneFrameInputs(
            pointer_x: 0.5,
            pointer_y: 0.5,
            time_seconds: 1,
            frame_time_seconds: 1.0 / 60.0
        )
        XCTAssertEqual(
            we_scene_frame_executor_render_for_drawable(
                loaded.executor, &inputs, 4, 2,
                WE_SCENE_PRESENTATION_ASPECT_FILL, &error
            ),
            1,
            errorMessage(error)
        )
        let initialIssue = try executorIssueSnapshot(loaded.executor)
        XCTAssertEqual(initialIssue.severity, Int(WE_SCENE_FRAME_ISSUE_SKIP_OBJECT.rawValue))
        XCTAssertEqual(initialIssue.objectIndex, 0)
        XCTAssertEqual(initialIssue.objectId, 1)
        XCTAssertEqual(initialIssue.operationIndex, 3)
        XCTAssertTrue(
            initialIssue.message.localizedCaseInsensitiveContains("shader")
        )

        // A replay must honor the frozen degraded-frame decision rather than
        // retrying object preparation. Repair the failed shader on disk so an
        // incorrect re-preflight would recover the object and clear the issue.
        try Data(validFragmentShader.utf8).write(
            to: loaded.fixture.assets
                .appendingPathComponent("shaders/broken.frag")
        )

        XCTAssertEqual(
            we_scene_frame_executor_replay_for_drawable(
                loaded.executor, 8, 4, &error
            ),
            1,
            errorMessage(error)
        )
        XCTAssertEqual(we_scene_frame_executor_width(loaded.executor), 8)
        XCTAssertEqual(we_scene_frame_executor_height(loaded.executor), 4)
        XCTAssertEqual(
            try executorIssueSnapshot(loaded.executor),
            initialIssue,
            "Resize replay must retain the exact issue frozen by the successful degraded frame"
        )

        XCTAssertEqual(
            we_scene_frame_executor_replay_for_drawable(
                loaded.executor, UInt32(Int32.max), 1, &error
            ),
            0
        )
        XCTAssertTrue(errorMessage(error).contains("exceed"))
        we_scene_runtime_error_destroy(error)

        var issueCount = 99
        error = nil
        XCTAssertEqual(
            we_scene_frame_executor_issue_count(
                loaded.executor, &issueCount, &error
            ),
            0
        )
        XCTAssertEqual(
            we_scene_runtime_error_code(error),
            WE_SCENE_RUNTIME_ERROR_FRAME_EXECUTOR_INVALID_STATE
        )
        XCTAssertEqual(issueCount, 99)
        we_scene_runtime_error_destroy(error)

        var issue = WESceneFrameExecutorIssueInfo()
        error = nil
        XCTAssertEqual(
            we_scene_frame_executor_issue_info(
                loaded.executor, 0, &issue, &error
            ),
            0
        )
        XCTAssertEqual(
            we_scene_runtime_error_code(error),
            WE_SCENE_RUNTIME_ERROR_FRAME_EXECUTOR_INVALID_STATE
        )
        we_scene_runtime_error_destroy(error)
    }

    func testHostOperationValidationFailureClearsPublishedFrameState() throws {
        let loaded = try loadFixture(
            fragmentSource: constantRedFragmentShader,
            includeSound: true
        )
        defer { destroy(loaded) }

        var error: WESceneRuntimeErrorRef?
        var inputs = WESceneFrameInputs(
            pointer_x: 0.5,
            pointer_y: 0.5,
            time_seconds: 1,
            frame_time_seconds: 1.0 / 60.0
        )
        func renderSuccess() throws {
            XCTAssertEqual(
                we_scene_frame_executor_render_for_drawable(
                    loaded.executor, &inputs, 2, 2,
                    WE_SCENE_PRESENTATION_ASPECT_FILL, &error
                ),
                1,
                errorMessage(error)
            )
            XCTAssertEqual(
                try readPixels(loaded.executor),
                repeatedPixel([255, 0, 0, 255])
            )
        }
        func assertPublishedStateIsInvalid() throws {
            XCTAssertEqual(
                try readPixels(loaded.executor),
                repeatedPixel([0, 0, 0, 0])
            )
            var revision: UInt64 = .max
            var soundCount = 99
            error = nil
            XCTAssertEqual(
                we_scene_frame_executor_last_model_revision(
                    loaded.executor, &revision, &error
                ),
                0
            )
            XCTAssertEqual(
                we_scene_runtime_error_code(error),
                WE_SCENE_RUNTIME_ERROR_FRAME_EXECUTOR_INVALID_STATE
            )
            we_scene_runtime_error_destroy(error)
            error = nil
            XCTAssertEqual(
                we_scene_frame_executor_sound_count(
                    loaded.executor, &soundCount, &error
                ),
                0
            )
            XCTAssertEqual(
                we_scene_runtime_error_code(error),
                WE_SCENE_RUNTIME_ERROR_FRAME_EXECUTOR_INVALID_STATE
            )
            we_scene_runtime_error_destroy(error)
            error = nil
        }

        try renderSuccess()
        XCTAssertEqual(
            we_scene_frame_executor_render_for_drawable(
                loaded.executor, &inputs, 2, 2,
                WEScenePresentationScaling(rawValue: 99), &error
            ),
            0
        )
        XCTAssertTrue(errorMessage(error).contains("scaling"))
        we_scene_runtime_error_destroy(error)
        error = nil
        try assertPublishedStateIsInvalid()

        inputs.time_seconds = 2
        try renderSuccess()
        XCTAssertEqual(
            we_scene_frame_executor_render_for_drawable(
                loaded.executor, &inputs, 0, 2,
                WE_SCENE_PRESENTATION_ASPECT_FILL, &error
            ),
            0
        )
        XCTAssertTrue(errorMessage(error).contains("dimensions"))
        we_scene_runtime_error_destroy(error)
        error = nil
        try assertPublishedStateIsInvalid()

        inputs.time_seconds = 3
        try renderSuccess()
        XCTAssertEqual(
            we_scene_frame_executor_replay_for_drawable(
                loaded.executor, UInt32.max, 2, &error
            ),
            0
        )
        XCTAssertTrue(errorMessage(error).contains("dimensions"))
        we_scene_runtime_error_destroy(error)
        error = nil
        try assertPublishedStateIsInvalid()
    }

    func testReplayPreservesCommittedParallaxDisplacement() throws {
        let loaded = try loadFixture(
            fragmentSource: constantRedFragmentShader,
            commandMode: .proceduralClear,
            parallax: true,
            projectionAuto: true,
            drawableSizedSolidLayer: true,
            parallaxDepth: "1 0"
        )
        defer { destroy(loaded) }

        var error: WESceneRuntimeErrorRef?
        var inputs = WESceneFrameInputs(
            pointer_x: 1,
            pointer_y: 0.5,
            time_seconds: 1,
            frame_time_seconds: 1
        )
        XCTAssertEqual(
            we_scene_frame_executor_render_for_drawable(
                loaded.executor, &inputs, 4, 4,
                WE_SCENE_PRESENTATION_ASPECT_FILL, &error
            ),
            1,
            errorMessage(error)
        )
        XCTAssertEqual(
            try readPixels(loaded.executor),
            repeatedRows(
                [[0, 0, 0, 255]] + Array(repeating: [255, 0, 0, 255], count: 3),
                count: 4
            )
        )

        XCTAssertEqual(
            we_scene_frame_executor_replay_for_drawable(
                loaded.executor, 8, 4, &error
            ),
            1,
            errorMessage(error)
        )
        XCTAssertEqual(
            try readPixels(loaded.executor),
            repeatedRows(
                Array(repeating: [0, 0, 0, 255], count: 2) +
                    Array(repeating: [255, 0, 0, 255], count: 6),
                count: 4
            ),
            "Replay must reuse the committed 0.25 displacement instead of advancing it toward the cached pointer"
        )
    }

    func testReplayFailureClearsOutputAndInvalidatesPublishedFrameState() throws {
        let loaded = try loadFixture(
            fragmentSource: constantRedFragmentShader,
            commandMode: .proceduralClear,
            projectionAuto: true,
            drawableSizedSolidLayer: true,
            includeSound: true
        )
        defer { destroy(loaded) }

        var error: WESceneRuntimeErrorRef?
        var inputs = WESceneFrameInputs(
            pointer_x: 0.5,
            pointer_y: 0.5,
            time_seconds: 1,
            frame_time_seconds: 1.0 / 60.0
        )
        XCTAssertEqual(
            we_scene_frame_executor_render_for_drawable(
                loaded.executor, &inputs, 4, 4,
                WE_SCENE_PRESENTATION_ASPECT_FILL, &error
            ),
            1,
            errorMessage(error)
        )
        XCTAssertEqual(try readPixels(loaded.executor), repeatedPixel([255, 0, 0, 255], count: 16))

        var revision: UInt64 = .max
        var soundCount = 0
        XCTAssertEqual(
            we_scene_frame_executor_last_model_revision(
                loaded.executor, &revision, &error
            ),
            1
        )
        XCTAssertEqual(revision, 0)
        XCTAssertEqual(
            we_scene_frame_executor_sound_count(
                loaded.executor, &soundCount, &error
            ),
            1
        )
        XCTAssertEqual(soundCount, 1)

        XCTAssertEqual(
            we_scene_frame_executor_replay_for_drawable(
                loaded.executor, UInt32(Int32.max), 1, &error
            ),
            0
        )
        XCTAssertTrue(errorMessage(error).contains("exceed"))
        we_scene_runtime_error_destroy(error)

        XCTAssertEqual(
            try readPixels(loaded.executor),
            [UInt8](repeating: 0, count: 4 * 4 * 4)
        )

        error = nil
        revision = .max
        XCTAssertEqual(
            we_scene_frame_executor_last_model_revision(
                loaded.executor, &revision, &error
            ),
            0
        )
        XCTAssertEqual(
            we_scene_runtime_error_code(error),
            WE_SCENE_RUNTIME_ERROR_FRAME_EXECUTOR_INVALID_STATE
        )
        XCTAssertEqual(revision, .max)
        we_scene_runtime_error_destroy(error)

        error = nil
        soundCount = 99
        XCTAssertEqual(
            we_scene_frame_executor_sound_count(
                loaded.executor, &soundCount, &error
            ),
            0
        )
        XCTAssertEqual(
            we_scene_runtime_error_code(error),
            WE_SCENE_RUNTIME_ERROR_FRAME_EXECUTOR_INVALID_STATE
        )
        XCTAssertEqual(soundCount, 99)
        we_scene_runtime_error_destroy(error)

        error = nil
        XCTAssertEqual(
            we_scene_frame_executor_replay_for_drawable(
                loaded.executor, 8, 4, &error
            ),
            0
        )
        XCTAssertEqual(
            we_scene_runtime_error_code(error),
            WE_SCENE_RUNTIME_ERROR_FRAME_EXECUTOR_INVALID_STATE
        )
        we_scene_runtime_error_destroy(error)
    }

    func testOrthographicCameraCancelsTranslatedEyeLikeUpstream() throws {
        let loaded = try loadFixture(
            fragmentSource: textureOnlyFragmentShader,
            cameraCenter: "1 0 -1",
            cameraEye: "1 0 0"
        )
        defer { destroy(loaded) }

        try render(loaded.executor)
        XCTAssertEqual(try readPixels(loaded.executor), repeatedPixel([255, 0, 0, 255]))
    }

    func testPerspectiveLayerUsesTheLinuxOrthographicFallback() throws {
        let loaded = try loadFixture(
            fragmentSource: textureOnlyFragmentShader,
            perspective: true
        )
        defer { destroy(loaded) }

        try render(loaded.executor)
        XCTAssertEqual(
            try readPixels(loaded.executor),
            repeatedPixel([255, 0, 0, 255])
        )
    }

    func testAutomaticImageExtentStaysFixedAcrossDrawableSizes() throws {
        let loaded = try loadFixture(
            fragmentSource: textureOnlyFragmentShader,
            projectionAuto: true
        )
        defer { destroy(loaded) }
        XCTAssertEqual(we_scene_frame_executor_width(loaded.executor), 4)
        XCTAssertEqual(we_scene_frame_executor_height(loaded.executor), 2)

        var inputs = WESceneFrameInputs(
            pointer_x: 0.5, pointer_y: 0.5, time_seconds: 0,
            frame_time_seconds: 1.0 / 60.0
        )
        var error: WESceneRuntimeErrorRef?
        XCTAssertEqual(
            we_scene_frame_executor_render_for_drawable(
                loaded.executor, &inputs, 40, 10,
                WE_SCENE_PRESENTATION_ASPECT_FILL, &error
            ),
            1,
            errorMessage(error)
        )
        XCTAssertEqual(we_scene_frame_executor_width(loaded.executor), 4)
        XCTAssertEqual(we_scene_frame_executor_height(loaded.executor), 2)
        XCTAssertEqual(
            try readPixels(loaded.executor),
            [0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255,
             255, 0, 0, 255, 255, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255]
        )
    }

    func testNonFiniteCameraUpdateFailsAndClearsPreviousFrame() throws {
        let loaded = try loadFixture(
            fragmentSource: textureOnlyFragmentShader,
            dynamicCameraCenter: true
        )
        defer { destroy(loaded) }
        try render(loaded.executor)
        XCTAssertEqual(try readPixels(loaded.executor), repeatedPixel([255, 0, 0, 255]))

        try setString(loaded.model, key: "camera_center", value: "1e309 0 -1")
        try assertRenderFailsAndClears(loaded.executor, containing: "Camera center")
    }

    func testParallelCameraUpUpdateFailsAndClearsPreviousFrame() throws {
        let loaded = try loadFixture(
            fragmentSource: textureOnlyFragmentShader,
            dynamicCameraUp: true
        )
        defer { destroy(loaded) }
        try render(loaded.executor)

        try setString(loaded.model, key: "camera_up", value: "0 0 -1")
        try assertRenderFailsAndClears(loaded.executor, containing: "zero-length direction")
    }

    func testBadShaderSkipsOnlyItsObjectAndLaterObjectStillRenders() throws {
        let loaded = try loadFixture(
            fragmentSource: "void main() { this is not valid GLSL; }",
            includeHealthySecondImage: true
        )
        defer { destroy(loaded) }

        try render(loaded.executor)
        XCTAssertEqual(
            try readPixels(loaded.executor),
            repeatedPixel([0, 255, 0, 255]),
            "A shader failure in object 1 must not suppress the later healthy object 2"
        )
        try assertSingleSkippedObjectIssue(
            loaded.executor,
            objectIndex: 0,
            objectId: 1,
            operationIndex: 0,
            messageContains: ["shader"]
        )
    }

    func testUnprovidedActiveUniformFailsBeforeMetalDrawValidation() throws {
        let loaded = try loadFixture(
            fragmentSource: """
            uniform float u_Unprovided;
            varying vec2 v_TexCoord;
            void main() {
                gl_FragColor = vec4(u_Unprovided, 0.0, 0.0, 1.0);
            }
            """
        )
        defer { destroy(loaded) }

        try assertRenderFailsAndClears(
            loaded.executor,
            containing: "Frame render pass does not provide active shader uniform 'u_Unprovided'"
        )
    }

    func testImageBuiltinUniformTypeFailureSkipsOnlyItsObject() throws {
        let loaded = try loadFixture(
            fragmentSource: """
            uniform vec2 g_Time;
            varying vec2 v_TexCoord;
            void main() {
                gl_FragColor = vec4(g_Time.x, g_Time.y, 0.0, 1.0);
            }
            """,
            includeHealthySecondImage: true
        )
        defer { destroy(loaded) }

        try render(loaded.executor)
        XCTAssertEqual(
            try readPixels(loaded.executor),
            repeatedPixel([0, 255, 0, 255]),
            "An authored type mismatch for an image builtin must skip only that object"
        )
        try assertSingleSkippedObjectIssue(
            loaded.executor,
            objectIndex: 0,
            objectId: 1,
            operationIndex: 0,
            messageContains: ["g_Time", "type"]
        )
    }

    func testImageBuiltinUniformArraySkipsOnlyItsObject() throws {
        let loaded = try loadFixture(
            fragmentSource: """
            uniform float g_Time[1];
            varying vec2 v_TexCoord;
            void main() {
                gl_FragColor = vec4(g_Time[0], 0.0, 0.0, 1.0);
            }
            """,
            includeHealthySecondImage: true
        )
        defer { destroy(loaded) }

        try render(loaded.executor)
        XCTAssertEqual(
            try readPixels(loaded.executor),
            repeatedPixel([0, 255, 0, 255]),
            "An authored builtin array must skip only that image object"
        )
        try assertSingleSkippedObjectIssue(
            loaded.executor,
            objectIndex: 0,
            objectId: 1,
            operationIndex: 0,
            messageContains: ["g_Time", "array"]
        )
    }

    func testCopyCommandUsesShaderPassAndProducesExactPixels() throws {
        let loaded = try loadFixture(fragmentSource: validFragmentShader, commandMode: .copy)
        defer { destroy(loaded) }

        try render(loaded.executor)
        XCTAssertEqual(try readPixels(loaded.executor), repeatedPixel([0, 255, 0, 255]))
    }

    func testCopyCommandAcceptsAssetTextureSource() throws {
        let loaded = try loadFixture(
            fragmentSource: validFragmentShader,
            commandMode: .assetCopy,
            includeUnboundGreenTexture: true
        )
        defer { destroy(loaded) }

        try render(loaded.executor)
        XCTAssertEqual(
            try readPixels(loaded.executor),
            repeatedPixel([0, 255, 0, 255])
        )
    }

    func testSwapCommandExchangesLogicalFramebufferBackings() throws {
        let loaded = try loadFixture(fragmentSource: validFragmentShader, commandMode: .swap)
        defer { destroy(loaded) }

        try render(loaded.executor)
        XCTAssertEqual(try readPixels(loaded.executor), repeatedPixel([0, 255, 0, 255]))
        // Aliases are reset at the start of every deterministic frame.
        try render(loaded.executor)
        XCTAssertEqual(try readPixels(loaded.executor), repeatedPixel([0, 255, 0, 255]))
    }

    func testBrokenEffectShaderSkipsItsObjectAndPublishesSuccessfulFrame() throws {
        let loaded = try loadFixture(fragmentSource: validFragmentShader, commandMode: .invalidCopy)
        defer { destroy(loaded) }

        let failingOperationIndex = try operationIndex(
            in: loaded.frameGraph,
            fragmentShaderPath: "shaders/broken.frag"
        )
        XCTAssertEqual(
            failingOperationIndex,
            3,
            "The fixture must keep the broken final effect pass at operation 3"
        )

        try render(loaded.executor)
        XCTAssertEqual(try readPixels(loaded.executor), repeatedPixel([0, 0, 0, 255]))
        try assertSingleSkippedObjectIssue(
            loaded.executor,
            objectIndex: 0,
            objectId: 1,
            operationIndex: failingOperationIndex,
            messageContains: ["shader"]
        )

        var revision: UInt64 = .max
        var error: WESceneRuntimeErrorRef?
        XCTAssertEqual(
            we_scene_frame_executor_last_model_revision(
                loaded.executor, &revision, &error
            ),
            1,
            errorMessage(error)
        )
        XCTAssertEqual(revision, 0)
        XCTAssertNil(error)
    }

    func testNewDescriptorBackedFramebufferStartsTransparentOverOpaqueSceneClear() throws {
        let loaded = try loadFixture(
            fragmentSource: validFragmentShader,
            commandMode: .uninitializedRead
        )
        defer { destroy(loaded) }

        var planError: WESceneRuntimeErrorRef?
        guard let plan = we_scene_frame_graph_plan_create(
            loaded.frameGraph, &planError
        ) else {
            throw failure("frame plan", planError)
        }
        defer { we_scene_frame_plan_destroy(plan) }
        var planInfo = WESceneFramePlanInfo()
        guard we_scene_frame_plan_info(plan, &planInfo, &planError) == 1 else {
            throw failure("frame plan info", planError)
        }
        XCTAssertEqual(planInfo.is_executable, 1)
        XCTAssertEqual(planInfo.operation_count, 2)
        XCTAssertEqual(planInfo.issue_count, 0)
        var finalPass = WESceneFrameOperationInfo()
        guard we_scene_frame_plan_operation_info(
            plan, 1, &finalPass, &planError
        ) == 1 else {
            throw failure("frame operation", planError)
        }
        XCTAssertEqual(finalPass.kind, WE_SCENE_FRAME_OPERATION_RENDER)
        var binding = WESceneFrameTextureBindingInfo()
        guard we_scene_frame_plan_texture_binding_info(
            plan, 1, 0, &binding, &planError
        ) == 1 else {
            throw failure("frame texture binding", planError)
        }
        XCTAssertEqual(
            binding.resource.logical_name.map(String.init(cString:)),
            "a"
        )

        try render(loaded.executor)
        XCTAssertEqual(try readPixels(loaded.executor), repeatedPixel([0, 0, 0, 255]))

        // The layer FBO remains a transparent input on later frames. The
        // published scene alpha stays opaque because Linux clears it to one.
        try render(loaded.executor, timeSeconds: 2)
        XCTAssertEqual(try readPixels(loaded.executor), repeatedPixel([0, 0, 0, 255]))
    }

    func testDescriptorBackedFramebufferContentsPersistAcrossFrames() throws {
        let loaded = try loadFixture(
            fragmentSource: validFragmentShader,
            commandMode: .persistentRead
        )
        defer { destroy(loaded) }

        try render(loaded.executor, timeSeconds: 1)
        XCTAssertEqual(
            try readPixels(loaded.executor),
            repeatedPixel([0, 255, 0, 255])
        )

        try setBoolean(loaded.model, key: "effect_enabled", value: false)
        XCTAssertFalse(
            try framebufferLogicalNames(loaded.frameGraph).contains("a")
        )
        try render(loaded.executor, timeSeconds: 1.5)
        XCTAssertEqual(
            try readPixels(loaded.executor),
            repeatedPixel([255, 0, 0, 255])
        )

        try setBoolean(loaded.model, key: "effect_enabled", value: true)
        XCTAssertTrue(
            try framebufferLogicalNames(loaded.frameGraph).contains("a")
        )
        // The FBO descriptor was absent from the middle frame. The writer now
        // discards every fragment, so green can only come from the retained
        // arena backing created on the first frame.
        try render(loaded.executor, timeSeconds: 2)
        XCTAssertEqual(
            try readPixels(loaded.executor),
            repeatedPixel([0, 255, 0, 255])
        )
    }

    func testFailedMultiPassObjectDoesNotCommitEarlierPersistentFramebufferWrites() throws {
        let loaded = try loadFixture(
            fragmentSource: validFragmentShader,
            commandMode: .atomicPersistentFailure
        )
        defer { destroy(loaded) }

        try render(loaded.executor, timeSeconds: 0.5)
        XCTAssertEqual(
            try readPixels(loaded.executor),
            repeatedPixel([0, 255, 0, 255])
        )

        try setBoolean(
            loaded.model,
            key: "atomic_failure_enabled",
            value: true
        )
        let failingOperationIndex = try operationIndex(
            in: loaded.frameGraph,
            fragmentShaderPath: "shaders/broken.frag"
        )
        let writerOperationIndex = try operationIndex(
            in: loaded.frameGraph,
            fragmentShaderPath: "shaders/atomic-writer.frag"
        )
        XCTAssertLessThan(
            writerOperationIndex,
            failingOperationIndex,
            "The persistent writer must precede the failing pass in the same object group"
        )
        try render(loaded.executor, timeSeconds: 1.5)
        XCTAssertEqual(
            try readPixels(loaded.executor),
            repeatedPixel([0, 0, 0, 255]),
            "A failed object must leave the opaque scene clear instead of executing any of its passes"
        )
        try assertSingleSkippedObjectIssue(
            loaded.executor,
            objectIndex: 0,
            objectId: 1,
            operationIndex: failingOperationIndex,
            messageContains: ["shader"]
        )

        try setBoolean(
            loaded.model,
            key: "atomic_failure_enabled",
            value: false
        )
        try render(loaded.executor, timeSeconds: 2.5)
        XCTAssertEqual(
            try readPixels(loaded.executor),
            repeatedPixel([0, 255, 0, 255]),
            "The failed frame's earlier red pass must not overwrite the persistent green FBO committed by the prior successful frame"
        )
    }

    func testProceduralClearLeavesOpaqueSceneClearPixels() throws {
        let loaded = try loadFixture(
            fragmentSource: validFragmentShader,
            commandMode: .proceduralClear
        )
        defer { destroy(loaded) }

        try render(loaded.executor)
        XCTAssertEqual(try readPixels(loaded.executor), repeatedPixel([0, 0, 0, 255]))
    }

    func testExecutorRendersCenteredSystemFontText() throws {
        let loaded = try loadTextFixture(font: "systemfont_arial")
        defer { destroy(loaded) }

        try render(loaded.executor)
        let pixels = try readPixels(loaded.executor)
        XCTAssertTrue(stride(from: 0, to: pixels.count, by: 4).contains { offset in
            pixels[offset] > 0
        })
    }

    func testTextAcceptsFiniteShaderRangeColorAndOpacity() throws {
        let loaded = try loadTextFixture(
            font: "systemfont_arial",
            color: "510 0 0 255",
            alpha: 1.5
        )
        defer { destroy(loaded) }

        try render(loaded.executor)
        try assertNoExecutorIssues(loaded.executor)
        let pixels = try readPixels(loaded.executor)
        XCTAssertTrue(stride(from: 0, to: pixels.count, by: 4).contains {
            pixels[$0] == 255
        })
    }

    func testTextOpacityEffectPreservesLayoutAndControlsCompositing() throws {
        let direct = try loadTextFixture(
            font: "systemfont_arial",
            origin: "32 32 0",
            scale: "0.5 0.5 1",
            effectAlpha: nil
        )
        defer { destroy(direct) }
        let visibleEffect = try loadTextFixture(
            font: "systemfont_arial",
            origin: "32 32 0",
            scale: "0.5 0.5 1",
            effectAlpha: 1
        )
        defer { destroy(visibleEffect) }
        let hiddenEffect = try loadTextFixture(
            font: "systemfont_arial",
            origin: "32 32 0",
            scale: "0.5 0.5 1",
            effectAlpha: 0
        )
        defer { destroy(hiddenEffect) }

        try render(direct.executor)
        try render(visibleEffect.executor)
        try render(hiddenEffect.executor)
        try assertNoExecutorIssues(direct.executor)
        try assertNoExecutorIssues(visibleEffect.executor)
        try assertNoExecutorIssues(hiddenEffect.executor)

        let directBounds = try XCTUnwrap(redPixelBounds(
            try readPixels(direct.executor),
            width: Int(we_scene_frame_executor_width(direct.executor))
        ))
        let effectBounds = try XCTUnwrap(redPixelBounds(
            try readPixels(visibleEffect.executor),
            width: Int(we_scene_frame_executor_width(visibleEffect.executor))
        ))
        XCTAssertEqual(effectBounds.minX, directBounds.minX, accuracy: 1)
        XCTAssertEqual(effectBounds.maxX, directBounds.maxX, accuracy: 1)
        XCTAssertEqual(effectBounds.minY, directBounds.minY, accuracy: 1)
        XCTAssertEqual(effectBounds.maxY, directBounds.maxY, accuracy: 1)
        let hiddenPixels = try readPixels(hiddenEffect.executor)
        let maximumHiddenRed = stride(
            from: 0, to: hiddenPixels.count, by: 4
        ).map { hiddenPixels[$0] }.max() ?? 0
        XCTAssertEqual(maximumHiddenRed, 0)
    }

    func testTextEffectFitsLongLeftTopAlignedContentWithPaddingAndRotation() throws {
        let arguments = (
            font: "systemfont_arial",
            horizontalAlignment: "left",
            verticalAlignment: "top",
            origin: "72 72 0",
            scale: "0.65 0.65 1",
            angles: "0 0 0.2",
            size: "24 18",
            padding: "12 8",
            text: "Dynamic title ABC 123",
            pointSize: 24.0,
            canvasWidth: 256,
            canvasHeight: 144
        )
        let direct = try loadTextFixture(
            font: arguments.font,
            horizontalAlignment: arguments.horizontalAlignment,
            verticalAlignment: arguments.verticalAlignment,
            origin: arguments.origin,
            scale: arguments.scale,
            angles: arguments.angles,
            size: arguments.size,
            padding: arguments.padding,
            text: arguments.text,
            pointSize: arguments.pointSize,
            effectAlpha: nil,
            canvasWidth: arguments.canvasWidth,
            canvasHeight: arguments.canvasHeight
        )
        defer { destroy(direct) }
        let effected = try loadTextFixture(
            font: arguments.font,
            horizontalAlignment: arguments.horizontalAlignment,
            verticalAlignment: arguments.verticalAlignment,
            origin: arguments.origin,
            scale: arguments.scale,
            angles: arguments.angles,
            size: arguments.size,
            padding: arguments.padding,
            text: arguments.text,
            pointSize: arguments.pointSize,
            effectAlpha: 1,
            canvasWidth: arguments.canvasWidth,
            canvasHeight: arguments.canvasHeight
        )
        defer { destroy(effected) }

        try render(direct.executor)
        try render(effected.executor)
        try assertNoExecutorIssues(direct.executor)
        try assertNoExecutorIssues(effected.executor)
        let directBounds = try XCTUnwrap(redPixelBounds(
            try readPixels(direct.executor),
            width: arguments.canvasWidth
        ))
        let effectBounds = try XCTUnwrap(redPixelBounds(
            try readPixels(effected.executor),
            width: arguments.canvasWidth
        ))
        XCTAssertGreaterThan(
            directBounds.maxX - directBounds.minX,
            80,
            "The raster must expand beyond the authored placeholder size"
        )
        XCTAssertEqual(effectBounds.minX, directBounds.minX, accuracy: 2)
        XCTAssertEqual(effectBounds.maxX, directBounds.maxX, accuracy: 2)
        XCTAssertEqual(effectBounds.minY, directBounds.minY, accuracy: 2)
        XCTAssertEqual(effectBounds.maxY, directBounds.maxY, accuracy: 2)
    }

    func testRasterizedTextReusesImmutableFontAssetAcrossFrames() throws {
        let fixtureFont = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .appendingPathComponent("SceneTextTests/Fixtures/Silkscreen-Regular.ttf")
        let fontPath = "fonts/fixture.ttf"
        let loaded = try loadTextFixture(
            font: fontPath,
            text: "CACHE",
            fontData: try Data(contentsOf: fixtureFont)
        )
        defer { destroy(loaded) }

        try render(loaded.executor, timeSeconds: 1)
        try assertNoExecutorIssues(loaded.executor)
        let firstFrame = try readPixels(loaded.executor)
        try FileManager.default.removeItem(
            at: loaded.fixture.root.appendingPathComponent(fontPath)
        )

        try render(loaded.executor, timeSeconds: 2)
        try assertNoExecutorIssues(loaded.executor)
        XCTAssertTrue(
            try readPixels(loaded.executor) == firstFrame,
            "Unchanged text must reuse its rasterized immutable font resource instead of resolving and rasterizing the font again every frame"
        )
    }

    func testUnknownSystemFontIdentifierFallsBackToRenderableSystemFont() throws {
        let loaded = try loadTextFixture(font: "systemfont_not_real")
        defer { destroy(loaded) }

        try render(loaded.executor)
        try assertNoExecutorIssues(loaded.executor)
        let pixels = try readPixels(loaded.executor)
        XCTAssertTrue(stride(from: 0, to: pixels.count, by: 4).contains { offset in
            pixels[offset] > 0
        })
    }

    func testTextHorizontalAlignmentUsesTheRenderedTextAsItsAnchor() throws {
        let left = try loadTextFixture(
            font: "systemfont_arial",
            horizontalAlignment: "left",
            origin: "32 32 0",
            scale: "0.25 0.25 1",
            size: "56 40"
        )
        defer { destroy(left) }
        let right = try loadTextFixture(
            font: "systemfont_arial",
            horizontalAlignment: "right",
            origin: "32 32 0",
            scale: "0.25 0.25 1",
            size: "56 40"
        )
        defer { destroy(right) }

        try render(left.executor)
        try render(right.executor)
        try assertNoExecutorIssues(left.executor)
        try assertNoExecutorIssues(right.executor)
        let leftBounds = try XCTUnwrap(
            redPixelBounds(
                try readPixels(left.executor),
                width: Int(we_scene_frame_executor_width(left.executor))
            )
        )
        let rightBounds = try XCTUnwrap(
            redPixelBounds(
                try readPixels(right.executor),
                width: Int(we_scene_frame_executor_width(right.executor))
            )
        )
        XCTAssertGreaterThan(leftBounds.minX, rightBounds.minX)
        XCTAssertGreaterThan(leftBounds.maxX, rightBounds.maxX)
    }

    func testTextVerticalAlignmentUsesWallpaperEngineAnchorDirection() throws {
        let top = try loadTextFixture(
            font: "systemfont_arial",
            verticalAlignment: "top",
            origin: "32 32 0",
            scale: "0.25 0.25 1",
            size: "40 56"
        )
        defer { destroy(top) }
        let bottom = try loadTextFixture(
            font: "systemfont_arial",
            verticalAlignment: "bottom",
            origin: "32 32 0",
            scale: "0.25 0.25 1",
            size: "40 56"
        )
        defer { destroy(bottom) }

        try render(top.executor)
        try render(bottom.executor)
        try assertNoExecutorIssues(top.executor)
        try assertNoExecutorIssues(bottom.executor)
        let topBounds = try XCTUnwrap(
            redPixelBounds(
                try readPixels(top.executor),
                width: Int(we_scene_frame_executor_width(top.executor))
            )
        )
        let bottomBounds = try XCTUnwrap(
            redPixelBounds(
                try readPixels(bottom.executor),
                width: Int(we_scene_frame_executor_width(bottom.executor))
            )
        )
        XCTAssertGreaterThan(topBounds.minY, bottomBounds.minY)
        XCTAssertGreaterThan(topBounds.maxY, bottomBounds.maxY)
        XCTAssertGreaterThanOrEqual(
            topBounds.minY,
            32,
            "Top-aligned text must extend below its authored top anchor"
        )
        XCTAssertLessThanOrEqual(
            bottomBounds.maxY,
            32,
            "Bottom-aligned text must extend above its authored bottom anchor"
        )
    }

    func testTextWithImplicitLayoutSizeStillRenders() throws {
        let loaded = try loadTextFixture(
            font: "systemfont_arial",
            origin: "32 32 0",
            size: "0 0"
        )
        defer { destroy(loaded) }

        try render(loaded.executor)
        try assertNoExecutorIssues(loaded.executor)
        XCTAssertNotNil(
            redPixelBounds(
                try readPixels(loaded.executor),
                width: Int(we_scene_frame_executor_width(loaded.executor))
            )
        )
    }

    func testTextRasterizationKeepsAuthoredScaleInsteadOfCancellingIt() throws {
        let smaller = try loadTextFixture(
            font: "systemfont_arial",
            origin: "32 32 0",
            scale: "0.25 0.25 1",
            size: "0 0",
            text: "WIDE"
        )
        defer { destroy(smaller) }
        let larger = try loadTextFixture(
            font: "systemfont_arial",
            origin: "32 32 0",
            scale: "0.5 0.5 1",
            size: "0 0",
            text: "WIDE"
        )
        defer { destroy(larger) }

        try render(smaller.executor)
        try render(larger.executor)
        try assertNoExecutorIssues(smaller.executor)
        try assertNoExecutorIssues(larger.executor)
        let smallerBounds = try XCTUnwrap(
            redPixelBounds(
                try readPixels(smaller.executor),
                width: Int(we_scene_frame_executor_width(smaller.executor))
            )
        )
        let largerBounds = try XCTUnwrap(
            redPixelBounds(
                try readPixels(larger.executor),
                width: Int(we_scene_frame_executor_width(larger.executor))
            )
        )
        XCTAssertGreaterThan(
            largerBounds.maxX - largerBounds.minX,
            smallerBounds.maxX - smallerBounds.minX + 8
        )
        XCTAssertGreaterThan(
            largerBounds.maxY - largerBounds.minY,
            smallerBounds.maxY - smallerBounds.minY + 4
        )
    }

    func testTextAppliesAuthoredRotationAroundItsOrigin() throws {
        let unrotated = try loadTextFixture(
            font: "systemfont_arial",
            origin: "32 32 0",
            scale: "0.25 0.25 1",
            size: "0 0",
            text: "WIDE"
        )
        defer { destroy(unrotated) }
        let rotated = try loadTextFixture(
            font: "systemfont_arial",
            origin: "32 32 0",
            scale: "0.25 0.25 1",
            angles: "0 0 1.5707963267948966",
            size: "0 0",
            text: "WIDE"
        )
        defer { destroy(rotated) }

        try render(unrotated.executor)
        try render(rotated.executor)
        let unrotatedBounds = try XCTUnwrap(
            redPixelBounds(
                try readPixels(unrotated.executor),
                width: Int(we_scene_frame_executor_width(unrotated.executor))
            )
        )
        let rotatedBounds = try XCTUnwrap(
            redPixelBounds(
                try readPixels(rotated.executor),
                width: Int(we_scene_frame_executor_width(rotated.executor))
            )
        )
        XCTAssertGreaterThan(
            unrotatedBounds.maxX - unrotatedBounds.minX,
            unrotatedBounds.maxY - unrotatedBounds.minY
        )
        XCTAssertGreaterThan(
            rotatedBounds.maxY - rotatedBounds.minY,
            rotatedBounds.maxX - rotatedBounds.minX
        )
    }

    func testTextExecutorAppliesWidthRowAndEllipsisLayoutControls() throws {
        let unbounded = try loadTextFixture(
            font: "systemfont_arial",
            origin: "32 32 0",
            scale: "0.25 0.25 1",
            size: "0 0",
            text: "WIDE WIDE WIDE",
            pointSize: 10
        )
        defer { destroy(unbounded) }
        let constrained = try loadTextFixture(
            font: "systemfont_arial",
            origin: "32 32 0",
            scale: "0.25 0.25 1",
            size: "0 0",
            text: "WIDE WIDE WIDE",
            pointSize: 10,
            maximumWidth: 100,
            maximumRows: 2,
            useEllipsis: true
        )
        defer { destroy(constrained) }

        try render(unbounded.executor)
        try render(constrained.executor)
        try assertNoExecutorIssues(unbounded.executor)
        try assertNoExecutorIssues(constrained.executor)
        let unboundedBounds = try XCTUnwrap(
            redPixelBounds(
                try readPixels(unbounded.executor),
                width: Int(we_scene_frame_executor_width(unbounded.executor))
            )
        )
        let constrainedBounds = try XCTUnwrap(
            redPixelBounds(
                try readPixels(constrained.executor),
                width: Int(we_scene_frame_executor_width(constrained.executor))
            )
        )
        XCTAssertLessThan(
            constrainedBounds.maxX - constrainedBounds.minX,
            unboundedBounds.maxX - unboundedBounds.minX
        )
        XCTAssertGreaterThan(
            constrainedBounds.maxY - constrainedBounds.minY,
            unboundedBounds.maxY - unboundedBounds.minY
        )
    }

    func testTextGlyphCoverageUsesWallpaperEngineTopLeftOrientation() throws {
        let loaded = try loadTextFixture(
            font: "systemfont_arial",
            origin: "32 32 0",
            size: "0 0",
            text: "F"
        )
        defer { destroy(loaded) }

        try render(loaded.executor)
        try assertNoExecutorIssues(loaded.executor)
        let pixels = try readPixels(loaded.executor)
        let width = Int(we_scene_frame_executor_width(loaded.executor))
        let bounds = try XCTUnwrap(redPixelBounds(pixels, width: width))
        let midpointX = (bounds.minX + bounds.maxX) / 2
        let midpointY = (bounds.minY + bounds.maxY) / 2
        var leftPixels = 0
        var rightPixels = 0
        var topPixels = 0
        var bottomPixels = 0
        for offset in stride(from: 0, to: pixels.count, by: 4) {
            guard pixels[offset] > 0 else { continue }
            let pixel = offset / 4
            let x = pixel % width
            let y = pixel / width
            if x <= midpointX { leftPixels += 1 } else { rightPixels += 1 }
            if y <= midpointY { topPixels += 1 } else { bottomPixels += 1 }
        }
        XCTAssertGreaterThan(leftPixels, rightPixels)
        XCTAssertGreaterThan(topPixels, bottomPixels)
    }

    func testAnimatedTextureUsesImageFramesAndFixedSHARightClosedBoundaries() throws {
        let texture = makeAnimatedRGBA8Texture2x2(
            images: [
                Array(repeating: [UInt8](arrayLiteral: 255, 0, 0, 255), count: 4).flatMap { $0 },
                Array(repeating: [UInt8](arrayLiteral: 0, 0, 255, 255), count: 4).flatMap { $0 },
            ],
            frames: [
                (image: 0, duration: 0.25, x: 0, y: 0, width: 2, widthAux: 0, heightAux: 0, height: 2),
                (image: 1, duration: 0.75, x: 0, y: 0, width: 2, widthAux: 0, heightAux: 0, height: 2),
                (image: 0, duration: 0, x: 0, y: 0, width: 2, widthAux: 0, heightAux: 0, height: 2),
            ]
        )
        let loaded = try loadFixture(
            fragmentSource: textureOnlyFragmentShader,
            textureData: texture,
            vertexSource: animatedVertexShader
        )
        defer { destroy(loaded) }

        try render(loaded.executor, timeSeconds: 0)
        XCTAssertEqual(try readPixels(loaded.executor), repeatedPixel([255, 0, 0, 255]))
        try render(loaded.executor, timeSeconds: 0.25)
        XCTAssertEqual(try readPixels(loaded.executor), repeatedPixel([255, 0, 0, 255]))
        try render(loaded.executor, timeSeconds: 0.250_001)
        XCTAssertEqual(try readPixels(loaded.executor), repeatedPixel([0, 0, 255, 255]))
        try render(loaded.executor, timeSeconds: 0.999_999)
        XCTAssertEqual(try readPixels(loaded.executor), repeatedPixel([0, 0, 255, 255]))
        try render(loaded.executor, timeSeconds: 1)
        XCTAssertEqual(try readPixels(loaded.executor), repeatedPixel([255, 0, 0, 255]))
    }

    func testAnimatedTextureAppliesCompleteFrameUVBasis() throws {
        let red: [UInt8] = [255, 0, 0, 255]
        let green: [UInt8] = [0, 255, 0, 255]
        let texture = makeAnimatedRGBA8Texture2x2(
            images: [[red, green, red, green].flatMap { $0 }],
            frames: [
                (image: 0, duration: 1, x: 1, y: 0, width: 1, widthAux: 0, heightAux: 0, height: 2),
            ]
        )
        let loaded = try loadFixture(
            fragmentSource: textureOnlyFragmentShader,
            textureData: texture,
            vertexSource: animatedVertexShader
        )
        defer { destroy(loaded) }

        try render(loaded.executor, timeSeconds: 0.5)
        XCTAssertEqual(try readPixels(loaded.executor), repeatedPixel([0, 255, 0, 255]))
    }

    func testSceneScriptTextureAnimationFrameControlsImageSource() throws {
        let texture = makeAnimatedRGBA8Texture2x2(
            images: [
                Array(repeating: [UInt8](arrayLiteral: 255, 0, 0, 255), count: 4).flatMap { $0 },
                Array(repeating: [UInt8](arrayLiteral: 0, 0, 255, 255), count: 4).flatMap { $0 },
            ],
            frames: [
                (image: 0, duration: 0.5, x: 0, y: 0, width: 2, widthAux: 0, heightAux: 0, height: 2),
                (image: 1, duration: 0.5, x: 0, y: 0, width: 2, widthAux: 0, heightAux: 0, height: 2),
            ]
        )
        let loaded = try loadFixture(
            fragmentSource: textureOnlyFragmentShader,
            textureData: texture,
            vertexSource: animatedVertexShader,
            textureAnimationScript: """
            const animation = thisLayer.getTextureAnimation();
            export function init() {
                animation.stop();
                animation.setFrame(1);
            }
            export function update(value) {
                if (thisScene.runtime >= 1) animation.join();
                return value;
            }
            """
        )
        defer { destroy(loaded) }

        try render(loaded.executor, timeSeconds: 0)
        XCTAssertEqual(try readPixels(loaded.executor), repeatedPixel([0, 0, 255, 255]))
        try render(loaded.executor, timeSeconds: 1.1)
        XCTAssertEqual(try readPixels(loaded.executor), repeatedPixel([255, 0, 0, 255]))
        try assertNoExecutorIssues(loaded.executor)
    }

    func testStaticTextureClearsAnimationUniformsFromEarlierDraw() throws {
        let texture = makeAnimatedRGBA8Texture2x2(
            images: [Array(repeating: [UInt8](arrayLiteral: 255, 0, 0, 255), count: 4).flatMap { $0 }],
            frames: [
                (image: 0, duration: 1, x: 1, y: 0, width: 1, widthAux: 0, heightAux: 0, height: 2),
            ]
        )
        let loaded = try loadFixture(
            fragmentSource: textureAnimationResetFragmentShader,
            textureData: texture,
            includeStaticSecondImageSameShader: true
        )
        defer { destroy(loaded) }

        try render(loaded.executor, timeSeconds: 0.5)
        XCTAssertEqual(
            try readPixels(loaded.executor),
            repeatedPixel([0, 255, 0, 255]),
            "A static draw must not inherit atlas translation or rotation from an earlier animated draw using the same program"
        )
    }

    func testNegativeAnimationTimeFailsAndClearsPreviousFrame() throws {
        let texture = makeAnimatedRGBA8Texture2x2(
            images: [Array(repeating: [UInt8](arrayLiteral: 255, 0, 0, 255), count: 4).flatMap { $0 }],
            frames: [
                (image: 0, duration: 1, x: 0, y: 0, width: 2, widthAux: 0, heightAux: 0, height: 2),
            ]
        )
        let loaded = try loadFixture(
            fragmentSource: textureOnlyFragmentShader,
            textureData: texture,
            vertexSource: animatedVertexShader
        )
        defer { destroy(loaded) }
        try render(loaded.executor, timeSeconds: 0)

        var error: WESceneRuntimeErrorRef?
        var inputs = WESceneFrameInputs(pointer_x: 0, pointer_y: 0, time_seconds: -0.01, frame_time_seconds: 0)
        XCTAssertEqual(we_scene_frame_executor_render(loaded.executor, &inputs, &error), 0)
        XCTAssertEqual(
            we_scene_runtime_error_code(error),
            WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT
        )
        XCTAssertTrue(errorMessage(error).contains("non-negative"))
        we_scene_runtime_error_destroy(error)
        XCTAssertEqual(try readPixels(loaded.executor), [UInt8](repeating: 0, count: 16))

        var staleRevision: UInt64 = .max
        error = nil
        XCTAssertEqual(
            we_scene_frame_executor_last_model_revision(
                loaded.executor, &staleRevision, &error
            ),
            0
        )
        XCTAssertEqual(
            we_scene_runtime_error_code(error),
            WE_SCENE_RUNTIME_ERROR_FRAME_EXECUTOR_INVALID_STATE
        )
        XCTAssertEqual(staleRevision, .max)
        we_scene_runtime_error_destroy(error)
    }

    func testInvalidTextureFrameTimingSkipsObject() throws {
        let texture = makeAnimatedRGBA8Texture2x2(
            images: [Array(repeating: [UInt8](arrayLiteral: 255, 0, 0, 255), count: 4).flatMap { $0 }],
            frames: [
                (image: 0, duration: -0.25, x: 0, y: 0, width: 2, widthAux: 0, heightAux: 0, height: 2),
            ]
        )
        let loaded = try loadFixture(
            fragmentSource: textureOnlyFragmentShader,
            textureData: texture,
            vertexSource: animatedVertexShader
        )
        defer { destroy(loaded) }

        try render(loaded.executor, timeSeconds: 0)
        XCTAssertEqual(try readPixels(loaded.executor), repeatedPixel([0, 0, 0, 255]))
        try assertSingleSkippedObjectIssue(
            loaded.executor,
            objectIndex: 0,
            objectId: 1,
            operationIndex: 0,
            messageContains: ["timing are invalid"]
        )
    }

    private var validFragmentShader: String {
        """
        uniform sampler2D g_Texture0;
        uniform float g_Amount; // {"material":"amount","default":0.0}
        varying vec2 v_TexCoord;
        void main() {
            gl_FragColor = mix(
                texture(g_Texture0, v_TexCoord),
                vec4(0.0, 1.0, 0.0, 1.0),
                g_Amount
            );
        }
        """
    }

    private var vertexShader: String {
        """
        attribute vec3 a_Position;
        attribute vec2 a_TexCoord;
        uniform mat4 g_ModelViewProjectionMatrix;
        varying vec2 v_TexCoord;
        void main() {
            v_TexCoord = a_TexCoord;
            gl_Position = g_ModelViewProjectionMatrix * vec4(a_Position, 1.0);
        }
        """
    }

    private var animatedVertexShader: String {
        """
        attribute vec3 a_Position;
        attribute vec2 a_TexCoord;
        uniform mat4 g_ModelViewProjectionMatrix;
        uniform vec2 g_Texture0Translation;
        uniform vec4 g_Texture0Rotation;
        varying vec2 v_TexCoord;
        void main() {
            v_TexCoord = g_Texture0Translation
                + a_TexCoord.x * g_Texture0Rotation.xy
                + a_TexCoord.y * g_Texture0Rotation.zw;
            gl_Position = g_ModelViewProjectionMatrix * vec4(a_Position, 1.0);
        }
        """
    }

    private var textureOnlyFragmentShader: String {
        """
        uniform sampler2D g_Texture0;
        varying vec2 v_TexCoord;
        void main() { gl_FragColor = texture(g_Texture0, v_TexCoord); }
        """
    }

    private var parallaxPositionContractVertexShader: String {
        """
        uniform mat4 g_ModelViewProjectionMatrix;
        uniform vec2 g_ParallaxPosition;
        attribute vec3 a_Position;
        attribute vec2 a_TexCoord;
        varying vec2 v_ParallaxPosition;
        void main() {
            v_ParallaxPosition = g_ParallaxPosition;
            gl_Position = g_ModelViewProjectionMatrix
                * vec4(a_Position, 1.0);
        }
        """
    }

    private var parallaxPositionContractFragmentShader: String {
        """
        varying vec2 v_ParallaxPosition;
        void main() {
            bool ok = all(lessThan(
                abs(v_ParallaxPosition - vec2(0.75, 0.75)),
                vec2(0.0001)
            ));
            gl_FragColor = ok ? vec4(0.0, 1.0, 0.0, 1.0)
                              : vec4(1.0, 0.0, 0.0, 1.0);
        }
        """
    }

    private var textureAnimationResetFragmentShader: String {
        """
        uniform vec2 g_Texture0Translation;
        uniform vec4 g_Texture0Rotation;
        varying vec2 v_TexCoord;
        void main() {
            bool reset = all(lessThan(abs(g_Texture0Translation), vec2(0.0001)))
                && all(lessThan(abs(g_Texture0Rotation), vec4(0.0001)));
            gl_FragColor = reset ? vec4(0.0, 1.0, 0.0, 1.0)
                                 : vec4(1.0, 0.0, 0.0, 1.0);
        }
        """
    }

    private var constantRedFragmentShader: String {
        """
        varying vec2 v_TexCoord;
        void main() { gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0); }
        """
    }

    private var constantGreenFragmentShader: String {
        """
        varying vec2 v_TexCoord;
        void main() { gl_FragColor = vec4(0.0, 1.0, 0.0, 1.0); }
        """
    }

    private var pointerContractFragmentShader: String {
        """
        uniform vec2 g_PointerPosition;
        uniform float g_Alpha;
        varying vec2 v_TexCoord;
        void main() {
            gl_FragColor = vec4(
                g_PointerPosition.x,
                g_PointerPosition.y,
                g_Alpha,
                1.0
            );
        }
        """
    }

    private var previousPointerFragmentShader: String {
        """
        uniform vec2 g_PointerPosition;
        uniform vec2 g_PointerPositionLast;
        varying vec2 v_TexCoord;
        void main() {
            gl_FragColor = vec4(
                g_PointerPosition.x,
                g_PointerPositionLast.x,
                g_PointerPositionLast.y,
                1.0
            );
        }
        """
    }

    private var commonBuiltinContractFragmentShader: String {
        """
        uniform float g_TextureReductionScale;
        uniform vec3 g_LightAmbientColor;
        uniform vec3 g_LightSkylightColor;
        uniform float g_Brightness;
        uniform float g_UserAlpha;
        uniform float g_Alpha;
        uniform vec3 g_Color;
        uniform vec4 g_Color4;
        uniform vec3 g_CompositeColor;
        uniform float g_Time;
        uniform float g_Frametime;
        uniform float g_Daytime;
        uniform vec3 g_Screen;
        uniform vec2 g_ParallaxPosition;
        uniform mat4 g_ModelViewProjectionMatrix;
        uniform mat4 g_ModelViewProjectionMatrixInverse;
        uniform mat4 g_EffectModelViewProjectionMatrix;
        uniform mat4 g_EffectModelViewProjectionMatrixInverse;
        uniform mat4 g_ModelMatrix;
        uniform mat4 g_EffectModelMatrix;
        uniform mat3 g_NormalModelMatrix;
        uniform mat4 g_ViewProjectionMatrix;
        uniform vec2 g_PointerPosition;
        uniform vec2 g_PointerPositionLast;
        uniform mat4 g_EffectTextureProjectionMatrix;
        uniform mat4 g_EffectTextureProjectionMatrixInverse;
        uniform vec2 g_TexelSize;
        uniform vec2 g_TexelSizeHalf;
        varying vec2 v_TexCoord;
        bool closeEnough(float lhs, float rhs) {
            return abs(lhs - rhs) < 0.0001;
        }
        void main() {
            mat4 identity = g_ModelViewProjectionMatrix
                * g_ModelViewProjectionMatrixInverse;
            bool ok = closeEnough(g_TextureReductionScale, 1.0)
                && closeEnough(g_LightAmbientColor.x, 0.0)
                && closeEnough(g_LightSkylightColor.x, 0.0)
                && closeEnough(g_Brightness, 1.0)
                && closeEnough(g_UserAlpha, 1.0)
                && closeEnough(g_Alpha, 1.0)
                && closeEnough(g_Color.x, 1.0)
                && closeEnough(g_Color4.w, 1.0)
                && closeEnough(g_CompositeColor.x, 1.0)
                && closeEnough(g_Time, 1.25)
                && closeEnough(g_Frametime, 1.0 / 60.0)
                && g_Daytime >= 0.0 && g_Daytime <= 1.0
                && closeEnough(g_Screen.x, 2.0)
                && closeEnough(g_Screen.y, 2.0)
                && closeEnough(g_Screen.z, 1.0)
                && closeEnough(g_ParallaxPosition.x, 0.5)
                && closeEnough(g_ParallaxPosition.y, 0.5)
                && closeEnough(identity[0][0], 1.0)
                && closeEnough(identity[1][1], 1.0)
                && closeEnough(g_EffectModelViewProjectionMatrix[0][0],
                               g_ModelViewProjectionMatrix[0][0])
                && closeEnough(
                    (g_EffectModelViewProjectionMatrix
                        * g_EffectModelViewProjectionMatrixInverse)[0][0],
                    1.0
                )
                && closeEnough(g_EffectModelMatrix[0][0], g_ModelMatrix[0][0])
                && closeEnough(g_NormalModelMatrix[0][0], 1.0)
                && closeEnough(g_ModelMatrix[3][0], -1.0)
                && closeEnough(g_ModelMatrix[3][1], -1.0)
                && closeEnough(g_ViewProjectionMatrix[0][0], 1.0)
                && closeEnough(g_ViewProjectionMatrix[1][1], 1.0)
                && closeEnough(g_ViewProjectionMatrix[3][0], 0.0)
                && closeEnough(g_ViewProjectionMatrix[3][1], 0.0)
                && closeEnough(g_PointerPosition.x, 0.5)
                && closeEnough(g_PointerPosition.y, 0.5)
                && closeEnough(g_PointerPositionLast.x, 0.0)
                && closeEnough(g_PointerPositionLast.y, 0.0)
                && closeEnough(g_EffectTextureProjectionMatrix[0][0], 1.0)
                && closeEnough(g_EffectTextureProjectionMatrixInverse[1][1], 1.0)
                && closeEnough(g_TexelSize.x, 0.5)
                && closeEnough(g_TexelSizeHalf.y, 0.25);
            gl_FragColor = ok ? vec4(0.0, 1.0, 0.0, 1.0)
                              : vec4(1.0, 0.0, 0.0, 1.0);
        }
        """
    }

    private var imageBuiltinContractFragmentShader: String {
        """
        uniform mat3 g_AltNormalModelMatrix;
        uniform vec3 g_EyePosition;
        varying vec2 v_TexCoord;
        bool closeEnough(float lhs, float rhs) {
            return abs(lhs - rhs) < 0.0001;
        }
        void main() {
            bool ok = closeEnough(g_AltNormalModelMatrix[0][0], 1.0)
                && closeEnough(g_AltNormalModelMatrix[1][1], 1.0)
                && closeEnough(g_AltNormalModelMatrix[2][2], 1.0)
                && closeEnough(g_EyePosition.x, 0.0)
                && closeEnough(g_EyePosition.y, 0.0)
                && closeEnough(g_EyePosition.z, 0.0);
            gl_FragColor = ok ? vec4(0.0, 1.0, 0.0, 1.0)
                              : vec4(1.0, 0.0, 0.0, 1.0);
        }
        """
    }

    private var commonUniformPrecedenceFragmentShader: String {
        """
        uniform float g_Brightness; // {"material":"test_brightness","default":0.25}
        uniform vec3 g_CompositeColor; // {"material":"test_composite","default":"0.25 0.5 0.75"}
        varying vec2 v_TexCoord;
        void main() {
            bool ok = abs(g_Brightness - 1.0) < 0.0001
                && all(lessThan(
                    abs(g_CompositeColor - vec3(0.25, 0.5, 0.75)),
                    vec3(0.0001)
                ));
            gl_FragColor = ok ? vec4(0.0, 1.0, 0.0, 1.0)
                              : vec4(1.0, 0.0, 0.0, 1.0);
        }
        """
    }

    private var audioBuiltinFragmentShader: String {
        """
        uniform float g_AudioSpectrum16Left[16];
        varying vec2 v_TexCoord;
        void main() {
            gl_FragColor = vec4(g_AudioSpectrum16Left[0], 0.0, 0.0, 1.0);
        }
        """
    }

    private var audioBuiltinLastElementFragmentShader: String {
        """
        uniform float g_AudioSpectrum16Left[16];
        varying vec2 v_TexCoord;
        void main() {
            gl_FragColor = vec4(g_AudioSpectrum16Left[15], 0.0, 0.0, 1.0);
        }
        """
    }

    private var imageAngleContractFragmentShader: String {
        """
        uniform mat4 g_ModelViewProjectionMatrix;
        uniform mat4 g_ModelMatrix;
        uniform mat4 g_ViewProjectionMatrix;
        varying vec2 v_TexCoord;
        void main() {
            bool ok = abs(g_ModelViewProjectionMatrix[0][0]) < 0.001
                && abs(g_ModelViewProjectionMatrix[0][1] + 1.0) < 0.001
                && abs(g_ModelViewProjectionMatrix[1][0] - 1.0) < 0.001
                && abs(g_ModelViewProjectionMatrix[1][1]) < 0.001
                && abs(g_ModelMatrix[0][0] - 1.0) < 0.001
                && abs(g_ModelMatrix[1][1] - 1.0) < 0.001
                && abs(g_ModelMatrix[3][0] + 1.0) < 0.001
                && abs(g_ModelMatrix[3][1] + 1.0) < 0.001
                && abs(g_ViewProjectionMatrix[0][0] - 1.0) < 0.001
                && abs(g_ViewProjectionMatrix[1][1] - 1.0) < 0.001;
            gl_FragColor = ok ? vec4(0.0, 1.0, 0.0, 1.0)
                              : vec4(1.0, 0.0, 0.0, 1.0);
        }
        """
    }

    private var alignedImageAngleContractFragmentShader: String {
        """
        uniform mat4 g_ModelViewProjectionMatrix;
        varying vec2 v_TexCoord;
        void main() {
            bool ok = abs(g_ModelViewProjectionMatrix[0][0]) < 0.001
                && abs(g_ModelViewProjectionMatrix[0][1] + 1.0) < 0.001
                && abs(g_ModelViewProjectionMatrix[1][0] - 1.0) < 0.001
                && abs(g_ModelViewProjectionMatrix[1][1]) < 0.001
                && abs(g_ModelViewProjectionMatrix[3][0]) < 0.001
                && abs(g_ModelViewProjectionMatrix[3][1] - 2.0) < 0.001;
            gl_FragColor = ok ? vec4(0.0, 1.0, 0.0, 1.0)
                              : vec4(1.0, 0.0, 0.0, 1.0);
        }
        """
    }

    private var automaticOriginContractFragmentShader: String {
        """
        varying vec2 v_TexCoord;
        void main() { gl_FragColor = vec4(0.0, 1.0, 0.0, 1.0); }
        """
    }

    private func loadFixture(
        fragmentSource: String,
        commandMode: CommandMode? = nil,
        textureData: Data? = nil,
        baseCombos: [String: Int] = [:],
        compositeTintColor: String? = nil,
        colorBlendMode: Int = 0,
        vertexSource: String? = nil,
        parallax: Bool = false,
        cameraCenter: String = "0 0 -1",
        cameraEye: String = "0 0 0",
        dynamicCameraCenter: Bool = false,
        dynamicCameraUp: Bool = false,
        projectionAuto: Bool = false,
        drawableSizedSolidLayer: Bool = false,
        includeSound: Bool = false,
        projectionWidth: Int = 2,
        projectionHeight: Int = 2,
        imageOrigin: String? = nil,
        imageSize: String? = nil,
        imageAngles: String? = nil,
        imageAlignment: String? = nil,
        parallaxDepth: String = "0 0",
        scriptedPointerAlpha: Bool = false,
        perspective: Bool = false,
        includeSecondTexture: Bool = false,
        includeUnboundGreenTexture: Bool = false,
        includeHealthySecondImage: Bool = false,
        includeStaticSecondImageSameShader: Bool = false,
        nearPlane: Double = 0,
        puppetData: Data? = nil,
        puppetAnimationLayer: [String: Any]? = nil,
        scriptedAudioAmount: Bool = false,
        scriptedAudioOrigin: Bool = false,
        scriptedMediaAmount: Bool = false,
        scriptedMediaBoundAlphaAnimation: Bool = false,
        scriptedMediaBoundMaterialAnimation: Bool = false,
        textureAnimationScript: String? = nil,
        sceneTexturePropertyKey: String? = nil,
        hostTextureNames: [String] = [],
        textureInProjectDirectory: Bool = false
    ) throws -> RuntimePipeline {
        let fixture = try makeFixture(
            fragmentSource: fragmentSource,
            commandMode: commandMode,
            textureData: textureData,
            baseCombos: baseCombos,
            compositeTintColor: compositeTintColor,
            colorBlendMode: colorBlendMode,
            vertexSource: vertexSource,
            parallax: parallax,
            cameraCenter: cameraCenter,
            cameraEye: cameraEye,
            dynamicCameraCenter: dynamicCameraCenter,
            dynamicCameraUp: dynamicCameraUp,
            projectionAuto: projectionAuto,
            drawableSizedSolidLayer: drawableSizedSolidLayer,
            includeSound: includeSound,
            projectionWidth: projectionWidth,
            projectionHeight: projectionHeight,
            imageOrigin: imageOrigin,
            imageSize: imageSize,
            imageAngles: imageAngles,
            imageAlignment: imageAlignment,
            parallaxDepth: parallaxDepth,
            scriptedPointerAlpha: scriptedPointerAlpha,
            perspective: perspective,
            includeSecondTexture: includeSecondTexture,
            includeUnboundGreenTexture: includeUnboundGreenTexture,
            includeHealthySecondImage: includeHealthySecondImage,
            includeStaticSecondImageSameShader: includeStaticSecondImageSameShader,
            nearPlane: nearPlane,
            puppetData: puppetData,
            puppetAnimationLayer: puppetAnimationLayer,
            scriptedAudioAmount: scriptedAudioAmount,
            scriptedAudioOrigin: scriptedAudioOrigin,
            scriptedMediaAmount: scriptedMediaAmount,
            scriptedMediaBoundAlphaAnimation: scriptedMediaBoundAlphaAnimation,
            scriptedMediaBoundMaterialAnimation: scriptedMediaBoundMaterialAnimation,
            textureAnimationScript: textureAnimationScript,
            sceneTexturePropertyKey: sceneTexturePropertyKey,
            hostTextureNames: hostTextureNames,
            textureInProjectDirectory: textureInProjectDirectory
        )
        do {
            var error: WESceneRuntimeErrorRef?
            guard let runtime = fixture.assets.path.withCString({ assetsPath in
                fixture.package.path.withCString { packagePath in
                    var configuration = WESceneRuntimeConfiguration(
                        assets_directory: assetsPath,
                        scene_package_path: packagePath
                    )
                    return we_scene_runtime_create(&configuration, &error)
                }
            }) else {
                throw failure("runtime", error)
            }
            do {
                guard let model = "project.json".withCString({
                    we_scene_runtime_model_create(runtime, $0, &error)
                }) else {
                    throw failure("model", error)
                }
                do {
                    guard let graph = we_scene_model_graph_create(model, &error) else {
                        throw failure("graph", error)
                    }
                    do {
                        guard let frameGraph = we_scene_graph_frame_graph_create(graph, &error) else {
                            throw failure("frame graph", error)
                        }
                        do {
                            guard let executor = we_scene_frame_executor_create(frameGraph, &error) else {
                                throw failure("executor", error)
                            }
                            return RuntimePipeline(
                                fixture: fixture,
                                runtime: runtime,
                                model: model,
                                graph: graph,
                                frameGraph: frameGraph,
                                executor: executor
                            )
                        } catch {
                            we_scene_frame_graph_destroy(frameGraph)
                            throw error
                        }
                    } catch {
                        we_scene_graph_destroy(graph)
                        throw error
                    }
                } catch {
                    we_scene_model_destroy(model)
                    throw error
                }
            } catch {
                we_scene_runtime_destroy(runtime)
                throw error
            }
        } catch {
            try? FileManager.default.removeItem(at: fixture.root)
            throw error
        }
    }

    private func loadTextFixture(
        font: String,
        horizontalAlignment: String = "center",
        verticalAlignment: String = "center",
        origin: String = "0 0 0",
        scale: String = "1 1 1",
        angles: String? = nil,
        size: String = "64 64",
        padding: String = "0 0",
        text: String = "I",
        pointSize: Double = 20,
        color: String = "255 0 0 255",
        alpha: Double = 1,
        spacing: String = "0 0",
        maximumWidth: Double? = nil,
        maximumRows: Int? = nil,
        useEllipsis: Bool = false,
        fontData: Data? = nil,
        effectAlpha: Double? = nil,
        canvasWidth: Int = 64,
        canvasHeight: Int = 64
    ) throws -> RuntimePipeline {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        let assets = root.appendingPathComponent("assets", isDirectory: true)
        let shaders = assets.appendingPathComponent("shaders", isDirectory: true)
        let package = root.appendingPathComponent("scene.pkg")
        try FileManager.default.createDirectory(at: shaders, withIntermediateDirectories: true)
        if let fontData {
            let fontURL = root.appendingPathComponent(font)
            try FileManager.default.createDirectory(
                at: fontURL.deletingLastPathComponent(),
                withIntermediateDirectories: true
            )
            try fontData.write(to: fontURL)
        }
        let project: [String: Any] = [
            "file": "scene.json", "title": "Text executor fixture",
            "type": "scene", "version": 2,
        ]
        var textObject: [String: Any] = [
            "alpha": alpha, "color": color, "font": font,
            "horizontalalign": horizontalAlignment, "id": 1, "name": "Text",
            "origin": origin, "padding": padding, "pointsize": pointSize,
            "scale": scale, "size": size, "spacing": spacing, "text": text,
            "verticalalign": verticalAlignment, "visible": true,
        ]
        if let angles {
            textObject["angles"] = angles
        }
        if let maximumWidth {
            textObject["limitwidth"] = true
            textObject["maxwidth"] = maximumWidth
        }
        if let maximumRows {
            textObject["limitrows"] = true
            textObject["maxrows"] = maximumRows
        }
        if let effectAlpha {
            textObject["effects"] = [[
                "file": "effects/opacity/effect.json",
                "id": 2,
                "passes": [[
                    "constantshadervalues": ["alpha": effectAlpha],
                ]],
                "visible": true,
            ]]
        }
        textObject["limituseellipsis"] = useEllipsis
        let scene: [String: Any] = [
            "camera": ["center": "0 0 -1", "eye": "0 0 0", "up": "0 1 0"],
            "general": [
                "clearcolor": "0 0 0 0",
                "orthogonalprojection": [
                    "height": canvasHeight,
                    "width": canvasWidth,
                ],
            ],
            "objects": [textObject],
            "version": 1,
        ]
        var entries: [(String, Data)] = [
            ("project.json", try json(project)),
            ("scene.json", try json(scene)),
        ]
        if effectAlpha != nil {
            entries.append(contentsOf: [
                (
                    "effects/opacity/effect.json",
                    try json([
                        "passes": [[
                            "material": "materials/effects/opacity.json",
                        ]],
                        "version": 1,
                    ])
                ),
                (
                    "materials/effects/opacity.json",
                    try json(["passes": [[
                        "blending": "normal",
                        "cullmode": "nocull",
                        "depthtest": "disabled",
                        "depthwrite": "disabled",
                        "shader": "text-opacity",
                    ]]])
                ),
                (
                    "materials/util/effectpassthrough.json",
                    try json(["passes": [[
                        "blending": "normal",
                        "cullmode": "nocull",
                        "depthtest": "disabled",
                        "depthwrite": "disabled",
                        "shader": "text-passthrough",
                    ]]])
                ),
            ])
            let vertex = """
            uniform mat4 g_ModelViewProjectionMatrix;
            attribute vec3 a_Position;
            attribute vec2 a_TexCoord;
            varying vec2 v_TexCoord;
            void main() {
                gl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix);
                v_TexCoord = a_TexCoord;
            }
            """
            let passthroughFragment = """
            uniform sampler2D g_Texture0;
            varying vec2 v_TexCoord;
            void main() { gl_FragColor = texture(g_Texture0, v_TexCoord); }
            """
            let opacityFragment = """
            uniform sampler2D g_Texture0;
            uniform float g_UserAlpha; // {"material":"alpha","default":1.0}
            varying vec2 v_TexCoord;
            void main() {
                vec4 color = texture(g_Texture0, v_TexCoord);
                color.a *= g_UserAlpha;
                gl_FragColor = color;
            }
            """
            for name in ["text-passthrough", "text-opacity"] {
                try Data(vertex.utf8).write(
                    to: shaders.appendingPathComponent("\(name).vert")
                )
            }
            try Data(passthroughFragment.utf8).write(
                to: shaders.appendingPathComponent("text-passthrough.frag")
            )
            try Data(opacityFragment.utf8).write(
                to: shaders.appendingPathComponent("text-opacity.frag")
            )
        }
        try makePackage(entries).write(to: package)
        let fixture = Fixture(root: root, assets: assets, package: package)
        do {
            var error: WESceneRuntimeErrorRef?
            guard let runtime = assets.path.withCString({ assetsPath in
                package.path.withCString { packagePath in
                    var configuration = WESceneRuntimeConfiguration(
                        assets_directory: assetsPath,
                        scene_package_path: packagePath
                    )
                    return we_scene_runtime_create(&configuration, &error)
                }
            }) else { throw failure("runtime", error) }
            guard let model = "project.json".withCString({
                we_scene_runtime_model_create(runtime, $0, &error)
            }) else {
                we_scene_runtime_destroy(runtime)
                throw failure("model", error)
            }
            guard let graph = we_scene_model_graph_create(model, &error) else {
                we_scene_model_destroy(model); we_scene_runtime_destroy(runtime)
                throw failure("graph", error)
            }
            guard let frameGraph = we_scene_graph_frame_graph_create(graph, &error) else {
                we_scene_graph_destroy(graph); we_scene_model_destroy(model)
                we_scene_runtime_destroy(runtime); throw failure("frame graph", error)
            }
            guard let executor = we_scene_frame_executor_create(frameGraph, &error) else {
                we_scene_frame_graph_destroy(frameGraph); we_scene_graph_destroy(graph)
                we_scene_model_destroy(model); we_scene_runtime_destroy(runtime)
                throw failure("executor", error)
            }
            return RuntimePipeline(
                fixture: fixture, runtime: runtime, model: model, graph: graph,
                frameGraph: frameGraph, executor: executor
            )
        } catch {
            try? FileManager.default.removeItem(at: root)
            throw error
        }
    }

    private func destroy(_ loaded: RuntimePipeline) {
        we_scene_frame_executor_destroy(loaded.executor)
        we_scene_frame_graph_destroy(loaded.frameGraph)
        we_scene_graph_destroy(loaded.graph)
        we_scene_model_destroy(loaded.model)
        we_scene_runtime_destroy(loaded.runtime)
        try? FileManager.default.removeItem(at: loaded.fixture.root)
    }

    private func render(
        _ executor: WESceneFrameExecutorRef,
        pointerX: Double = 0.5,
        pointerY: Double = 0.5,
        timeSeconds: Double = 1.25,
        frameTimeSeconds: Double = 1.0 / 60.0
    ) throws {
        var error: WESceneRuntimeErrorRef?
        var inputs = WESceneFrameInputs(
            pointer_x: pointerX, pointer_y: pointerY, time_seconds: timeSeconds,
            frame_time_seconds: frameTimeSeconds
        )
        guard we_scene_frame_executor_render(executor, &inputs, &error) == 1 else {
            throw failure("render", error)
        }
        XCTAssertNil(error)
    }

    private func setMediaSnapshot(
        _ executor: WESceneFrameExecutorRef,
        revision: UInt64,
        playbackState: WESceneMediaPlaybackState,
        title: String,
        artist: String,
        position: Double,
        duration: Double,
        hasThumbnail: Bool,
        overwriteBorrowedStringsAfterSet: Bool = false
    ) throws {
        let values = [
            title,
            artist,
            "music",
            "Borrowed Album",
            "Borrowed Subtitle",
            "Borrowed Album Artist",
            "test",
        ]
        var strings: [UnsafeMutablePointer<CChar>] = []
        defer {
            for pointer in strings { free(pointer) }
        }
        for value in values {
            guard let pointer = strdup(value) else {
                throw NSError(
                    domain: "FrameExecutorPixelTests",
                    code: 1,
                    userInfo: [
                        NSLocalizedDescriptionKey:
                            "Allocating media snapshot test input failed",
                    ]
                )
            }
            strings.append(pointer)
        }

        var snapshot = WESceneMediaSnapshot(
            status_revision: revision,
            metadata_revision: revision,
            playback_revision: revision,
            timeline_revision: revision,
            thumbnail_revision: revision,
            available: 1,
            playback_state: playbackState,
            title: UnsafePointer(strings[0]),
            artist: UnsafePointer(strings[1]),
            content_type: UnsafePointer(strings[2]),
            album_title: UnsafePointer(strings[3]),
            sub_title: UnsafePointer(strings[4]),
            album_artist: UnsafePointer(strings[5]),
            genres: UnsafePointer(strings[6]),
            position: position,
            duration: duration,
            has_thumbnail: hasThumbnail ? 1 : 0,
            primary_color: (0.1, 0.2, 0.3),
            secondary_color: (0.2, 0.4, 0.6),
            tertiary_color: (0.3, 0.5, 0.7),
            text_color: (0.4, 0.5, 0.6),
            high_contrast_color: (1, 1, 1)
        )
        var error: WESceneRuntimeErrorRef?
        guard we_scene_frame_executor_set_media_snapshot(
            executor, &snapshot, &error
        ) == 1 else {
            throw failure("media snapshot", error)
        }
        XCTAssertNil(error)

        if overwriteBorrowedStringsAfterSet {
            for pointer in strings {
                for index in 0..<strlen(pointer) {
                    pointer[index] = 88
                }
            }
        }
    }

    private func setMediaThumbnail(
        _ executor: WESceneFrameExecutorRef,
        revision: UInt64,
        pixel: [UInt8],
        overwriteBorrowedPixelsAfterSet: Bool = false
    ) throws {
        precondition(pixel.count == 4)
        let bytesPerRow = 12
        var pixels = [UInt8]()
        for _ in 0..<2 {
            pixels.append(contentsOf: pixel)
            pixels.append(contentsOf: pixel)
            pixels.append(contentsOf: [11, 22, 33, 44])
        }
        var error: WESceneRuntimeErrorRef?
        let succeeded = pixels.withUnsafeBufferPointer { storage in
            var thumbnail = WESceneMediaThumbnailRGBA8(
                revision: revision,
                width: 2,
                height: 2,
                bytes_per_row: UInt32(bytesPerRow),
                pixels: storage.baseAddress,
                pixel_length: storage.count
            )
            return we_scene_frame_executor_set_media_thumbnail_rgba8(
                executor, &thumbnail, &error
            )
        }
        guard succeeded == 1 else {
            throw failure("media thumbnail", error)
        }
        XCTAssertNil(error)
        if overwriteBorrowedPixelsAfterSet {
            pixels = [UInt8](repeating: 255, count: pixels.count)
        }
    }

    private func renderWithAudioSpectrum(
        _ executor: WESceneFrameExecutorRef,
        spectrum16Left: [Float],
        pointerX: Double = 0.5,
        pointerY: Double = 0.5,
        timeSeconds: Double = 1.25,
        frameTimeSeconds: Double = 1.0 / 60.0
    ) throws {
        XCTAssertEqual(spectrum16Left.count, 16)
        let spectrum16Right = [Float](repeating: 0, count: 16)
        let spectrum32Left = [Float](repeating: 0, count: 32)
        let spectrum32Right = [Float](repeating: 0, count: 32)
        let spectrum64Left = [Float](repeating: 0, count: 64)
        let spectrum64Right = [Float](repeating: 0, count: 64)
        var error: WESceneRuntimeErrorRef?
        var inputs = WESceneFrameInputs(
            pointer_x: pointerX,
            pointer_y: pointerY,
            time_seconds: timeSeconds,
            frame_time_seconds: frameTimeSeconds
        )
        let result = spectrum16Left.withUnsafeBufferPointer { left16 in
            spectrum16Right.withUnsafeBufferPointer { right16 in
                spectrum32Left.withUnsafeBufferPointer { left32 in
                    spectrum32Right.withUnsafeBufferPointer { right32 in
                        spectrum64Left.withUnsafeBufferPointer { left64 in
                            spectrum64Right.withUnsafeBufferPointer { right64 in
                                var audio = WESceneAudioSpectrumInputs(
                                    spectrum_16_left: left16.baseAddress,
                                    spectrum_16_right: right16.baseAddress,
                                    spectrum_32_left: left32.baseAddress,
                                    spectrum_32_right: right32.baseAddress,
                                    spectrum_64_left: left64.baseAddress,
                                    spectrum_64_right: right64.baseAddress
                                )
                                return we_scene_frame_executor_render_with_audio_spectrum(
                                    executor, &inputs, &audio, &error
                                )
                            }
                        }
                    }
                }
            }
        }
        guard result == 1 else { throw failure("audio spectrum render", error) }
        XCTAssertNil(error)
    }

    private func assertPuppetModelLoadFails(
        _ puppetData: Data,
        containing expectedMessage: String
    ) throws {
        let fixture = try makeFixture(
            fragmentSource: constantRedFragmentShader,
            puppetData: puppetData
        )
        defer { try? FileManager.default.removeItem(at: fixture.root) }

        var error: WESceneRuntimeErrorRef?
        guard let runtime = fixture.assets.path.withCString({ assetsPath in
            fixture.package.path.withCString { packagePath in
                var configuration = WESceneRuntimeConfiguration(
                    assets_directory: assetsPath,
                    scene_package_path: packagePath
                )
                return we_scene_runtime_create(&configuration, &error)
            }
        }) else {
            throw failure("runtime", error)
        }
        defer { we_scene_runtime_destroy(runtime) }

        let model = "project.json".withCString {
            we_scene_runtime_model_create(runtime, $0, &error)
        }
        XCTAssertNil(model)
        XCTAssertEqual(
            we_scene_runtime_error_code(error),
            WE_SCENE_RUNTIME_ERROR_SCENE_ASSET_FAILURE
        )
        XCTAssertTrue(errorMessage(error).contains(expectedMessage))
        we_scene_runtime_error_destroy(error)
    }

    private func readPixels(_ executor: WESceneFrameExecutorRef) throws -> [UInt8] {
        var pixels = [UInt8](
            repeating: 0x7f,
            count: we_scene_frame_executor_rgba8_byte_count(executor)
        )
        var error: WESceneRuntimeErrorRef?
        let result = pixels.withUnsafeMutableBytes { bytes in
            we_scene_frame_executor_read_rgba8(
                executor,
                bytes.baseAddress?.assumingMemoryBound(to: UInt8.self),
                bytes.count,
                &error
            )
        }
        guard result == 1 else { throw failure("readback", error) }
        XCTAssertNil(error)
        return pixels
    }

    private struct PixelBounds {
        var minX: Int
        var maxX: Int
        var minY: Int
        var maxY: Int
        var pixelCount: Int
    }

    private func redPixelBounds(_ pixels: [UInt8], width: Int) -> PixelBounds? {
        precondition(width > 0)
        var result: PixelBounds?
        for offset in stride(from: 0, to: pixels.count, by: 4) {
            guard pixels[offset] > 0 else { continue }
            let pixel = offset / 4
            let x = pixel % width
            let y = pixel / width
            var bounds = result ?? PixelBounds(
                minX: x, maxX: x, minY: y, maxY: y, pixelCount: 0
            )
            bounds.minX = min(bounds.minX, x)
            bounds.maxX = max(bounds.maxX, x)
            bounds.minY = min(bounds.minY, y)
            bounds.maxY = max(bounds.maxY, y)
            bounds.pixelCount += 1
            result = bounds
        }
        return result
    }

    private func lastRevision(_ executor: WESceneFrameExecutorRef) throws -> UInt64 {
        var revision: UInt64 = 0
        var error: WESceneRuntimeErrorRef?
        guard we_scene_frame_executor_last_model_revision(
            executor,
            &revision,
            &error
        ) == 1 else {
            throw failure("last model revision", error)
        }
        return revision
    }

    private func setNumber(
        _ model: WESceneModelRef,
        key: String,
        value: Double
    ) throws {
        var property = WEScenePropertyValue(
            type: WE_SCENE_VALUE_NUMBER,
            boolean_value: 0,
            integer_value: 0,
            number_value: value,
            string_value: nil,
            component_count: 0,
            vector_value: WESceneVector4()
        )
        var error: WESceneRuntimeErrorRef?
        let result = key.withCString {
            we_scene_model_set_property_value(model, $0, &property, &error)
        }
        guard result == 1 else { throw failure("property update", error) }
    }

    private func setBoolean(
        _ model: WESceneModelRef,
        key: String,
        value: Bool
    ) throws {
        var property = WEScenePropertyValue(
            type: WE_SCENE_VALUE_BOOLEAN,
            boolean_value: value ? 1 : 0,
            integer_value: 0,
            number_value: 0,
            string_value: nil,
            component_count: 0,
            vector_value: WESceneVector4()
        )
        var error: WESceneRuntimeErrorRef?
        let result = key.withCString {
            we_scene_model_set_property_value(model, $0, &property, &error)
        }
        guard result == 1 else { throw failure("property update", error) }
    }

    private func framebufferLogicalNames(
        _ frameGraph: WESceneFrameGraphRef
    ) throws -> [String] {
        var error: WESceneRuntimeErrorRef?
        guard let plan = we_scene_frame_graph_plan_create(frameGraph, &error) else {
            throw failure("frame plan", error)
        }
        defer { we_scene_frame_plan_destroy(plan) }
        var planInfo = WESceneFramePlanInfo()
        guard we_scene_frame_plan_info(plan, &planInfo, &error) == 1 else {
            throw failure("frame plan info", error)
        }
        return try (0..<planInfo.framebuffer_count).map { index in
            var framebuffer = WESceneFramebufferInfo()
            guard we_scene_frame_plan_framebuffer_info(
                plan, index, &framebuffer, &error
            ) == 1 else {
                throw failure("framebuffer info", error)
            }
            return framebuffer.resource.logical_name.map(String.init(cString:)) ?? ""
        }
    }

    private func operationIndex(
        in frameGraph: WESceneFrameGraphRef,
        fragmentShaderPath: String
    ) throws -> Int {
        var error: WESceneRuntimeErrorRef?
        guard let plan = we_scene_frame_graph_plan_create(frameGraph, &error) else {
            throw failure("frame plan", error)
        }
        defer { we_scene_frame_plan_destroy(plan) }

        var planInfo = WESceneFramePlanInfo()
        guard we_scene_frame_plan_info(plan, &planInfo, &error) == 1 else {
            throw failure("frame plan info", error)
        }
        var matches: [Int] = []
        for index in 0..<planInfo.operation_count {
            var operation = WESceneFrameOperationInfo()
            guard we_scene_frame_plan_operation_info(
                plan, index, &operation, &error
            ) == 1 else {
                throw failure("frame operation", error)
            }
            if operation.fragment_shader_path.map(String.init(cString:)) ==
                fragmentShaderPath {
                matches.append(index)
            }
        }
        XCTAssertEqual(
            matches.count,
            1,
            "Expected exactly one operation using \(fragmentShaderPath)"
        )
        return try XCTUnwrap(matches.first)
    }

    private func executorIssueSnapshot(
        _ executor: WESceneFrameExecutorRef
    ) throws -> ExecutorIssueSnapshot {
        var error: WESceneRuntimeErrorRef?
        var count = 0
        guard we_scene_frame_executor_issue_count(
            executor, &count, &error
        ) == 1 else {
            throw failure("executor issue count", error)
        }
        XCTAssertEqual(count, 1)

        var issue = WESceneFrameExecutorIssueInfo()
        guard we_scene_frame_executor_issue_info(
            executor, 0, &issue, &error
        ) == 1 else {
            throw failure("executor issue info", error)
        }
        return ExecutorIssueSnapshot(
            severity: Int(issue.severity.rawValue),
            objectIndex: issue.object_index,
            objectId: issue.object_id,
            operationIndex: issue.operation_index,
            message: issue.message.map(String.init(cString:)) ?? ""
        )
    }

    private func setString(
        _ model: WESceneModelRef,
        key: String,
        value: String
    ) throws {
        var error: WESceneRuntimeErrorRef?
        let result = value.withCString { stringValue in
            var property = WEScenePropertyValue(
                type: WE_SCENE_VALUE_STRING,
                boolean_value: 0,
                integer_value: 0,
                number_value: 0,
                string_value: stringValue,
                component_count: 0,
                vector_value: WESceneVector4()
            )
            return key.withCString {
                we_scene_model_set_property_value(model, $0, &property, &error)
            }
        }
        guard result == 1 else { throw failure("property update", error) }
    }

    private func assertRenderFailsAndClears(
        _ executor: WESceneFrameExecutorRef,
        containing expectedMessage: String
    ) throws {
        var error: WESceneRuntimeErrorRef?
        var inputs = WESceneFrameInputs(
            pointer_x: 0.5, pointer_y: 0.5, time_seconds: 1,
            frame_time_seconds: 1.0 / 60.0
        )
        XCTAssertEqual(we_scene_frame_executor_render(executor, &inputs, &error), 0)
        XCTAssertTrue(errorMessage(error).contains(expectedMessage))
        we_scene_runtime_error_destroy(error)
        XCTAssertEqual(try readPixels(executor), [UInt8](repeating: 0, count: 16))
    }

    private func makeFixture(
        fragmentSource: String,
        commandMode: CommandMode? = nil,
        textureData: Data? = nil,
        baseCombos: [String: Int] = [:],
        compositeTintColor: String? = nil,
        colorBlendMode: Int = 0,
        vertexSource: String? = nil,
        parallax: Bool = false,
        cameraCenter: String = "0 0 -1",
        cameraEye: String = "0 0 0",
        dynamicCameraCenter: Bool = false,
        dynamicCameraUp: Bool = false,
        projectionAuto: Bool = false,
        drawableSizedSolidLayer: Bool = false,
        includeSound: Bool = false,
        projectionWidth: Int = 2,
        projectionHeight: Int = 2,
        imageOrigin: String? = nil,
        imageSize: String? = nil,
        imageAngles: String? = nil,
        imageAlignment: String? = nil,
        parallaxDepth: String = "0 0",
        scriptedPointerAlpha: Bool = false,
        perspective: Bool = false,
        includeSecondTexture: Bool = false,
        includeUnboundGreenTexture: Bool = false,
        includeHealthySecondImage: Bool = false,
        includeStaticSecondImageSameShader: Bool = false,
        nearPlane: Double = 0,
        puppetData: Data? = nil,
        puppetAnimationLayer: [String: Any]? = nil,
        scriptedAudioAmount: Bool = false,
        scriptedAudioOrigin: Bool = false,
        scriptedMediaAmount: Bool = false,
        scriptedMediaBoundAlphaAnimation: Bool = false,
        scriptedMediaBoundMaterialAnimation: Bool = false,
        textureAnimationScript: String? = nil,
        sceneTexturePropertyKey: String? = nil,
        hostTextureNames: [String] = [],
        textureInProjectDirectory: Bool = false
    ) throws -> Fixture {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        let assets = root.appendingPathComponent("assets", isDirectory: true)
        let shaders = assets.appendingPathComponent("shaders", isDirectory: true)
        let effectShaders = assets.appendingPathComponent(
            "effects/commands/shaders", isDirectory: true
        )
        let package = root.appendingPathComponent("scene.pkg")
        try FileManager.default.createDirectory(at: shaders, withIntermediateDirectories: true)
        try FileManager.default.createDirectory(at: effectShaders, withIntermediateDirectories: true)
        try Data((vertexSource ?? vertexShader).utf8).write(
            to: shaders.appendingPathComponent("pixel.vert")
        )
        try Data(fragmentSource.utf8).write(to: shaders.appendingPathComponent("pixel.frag"))
        try Data(vertexShader.utf8).write(to: shaders.appendingPathComponent("green.vert"))
        try Data(vertexShader.utf8).write(to: shaders.appendingPathComponent("red.vert"))
        try Data("""
        varying vec2 v_TexCoord;
        void main() { gl_FragColor = vec4(0.0, 1.0, 0.0, 1.0); }
        """.utf8).write(to: shaders.appendingPathComponent("green.frag"))
        try Data("""
        varying vec2 v_TexCoord;
        void main() { gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0); }
        """.utf8).write(to: shaders.appendingPathComponent("red.frag"))
        for name in ["atomic-writer", "persistent"] {
            try Data(vertexShader.utf8).write(
                to: shaders.appendingPathComponent("\(name).vert")
            )
        }
        try Data("""
        uniform float g_Time;
        varying vec2 v_TexCoord;
        void main() {
            if (g_Time >= 2.0) {
                discard;
            }
            gl_FragColor = g_Time >= 1.0
                ? vec4(1.0, 0.0, 0.0, 1.0)
                : vec4(0.0, 1.0, 0.0, 1.0);
        }
        """.utf8).write(
            to: shaders.appendingPathComponent("atomic-writer.frag")
        )
        for name in ["pixel", "green", "red"] {
            try Data(vertexShader.utf8).write(
                to: effectShaders.appendingPathComponent("\(name).vert")
            )
        }
        try Data(fragmentSource.utf8).write(
            to: effectShaders.appendingPathComponent("pixel.frag")
        )
        try Data("varying vec2 v_TexCoord; void main() { gl_FragColor = vec4(0.0, 1.0, 0.0, 1.0); }".utf8)
            .write(to: effectShaders.appendingPathComponent("green.frag"))
        try Data("varying vec2 v_TexCoord; void main() { gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0); }".utf8)
            .write(to: effectShaders.appendingPathComponent("red.frag"))
        try Data("""
        uniform float g_Time;
        varying vec2 v_TexCoord;
        void main() {
            if (g_Time >= 2.0) {
                discard;
            }
            gl_FragColor = vec4(0.0, 1.0, 0.0, 1.0);
        }
        """.utf8).write(to: shaders.appendingPathComponent("persistent.frag"))
        try Data("void main() { invalid shader; }".utf8)
            .write(to: shaders.appendingPathComponent("broken.frag"))
        try Data(vertexShader.utf8).write(
            to: shaders.appendingPathComponent("broken.vert")
        )
        if compositeTintColor != nil {
            for name in ["composite-effect", "composite-tint", "composite-blend"] {
                try Data(vertexShader.utf8).write(
                    to: shaders.appendingPathComponent("\(name).vert")
                )
            }
            try Data(constantGreenFragmentShader.utf8).write(
                to: shaders.appendingPathComponent("composite-effect.frag")
            )
            try Data("""
            uniform sampler2D g_Texture0;
            uniform vec3 g_TintColor; // {"material":"color"}
            uniform float g_TintAlpha; // {"material":"alpha"}
            varying vec2 v_TexCoord;
            void main() {
            #if BLENDMODE == 30
                bool constantsAreLinuxValues = all(greaterThan(
                    g_TintColor, vec3(0.99, -0.01, 0.99)
                )) && g_TintColor.g < 0.01 && g_TintAlpha > 0.99;
                gl_FragColor = constantsAreLinuxValues
                    ? vec4(g_TintColor, 1.0)
                    : vec4(1.0, 0.0, 0.0, 1.0);
            #else
                gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0);
            #endif
            }
            """.utf8).write(
                to: shaders.appendingPathComponent("composite-tint.frag")
            )
            try Data("""
            uniform sampler2D g_Texture0;
            varying vec2 v_TexCoord;
            void main() {
                vec4 inputColor = texture(g_Texture0, v_TexCoord);
            #if BLENDMODE == 7
                bool tintRanFirst = inputColor.r > 0.99
                    && inputColor.g < 0.01 && inputColor.b > 0.99;
                gl_FragColor = tintRanFirst
                    ? vec4(0.0, 1.0, 1.0, 1.0)
                    : vec4(1.0, 0.0, 0.0, 1.0);
            #else
                gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0);
            #endif
            }
            """.utf8).write(
                to: shaders.appendingPathComponent("composite-blend.frag")
            )
        }

        var properties: [String: Any] = [
            "amount": [
                "fraction": true, "max": 1.0, "min": 0.0,
                "step": 1.0, "text": "Amount", "type": "slider",
                "value": 0.0,
            ],
        ]
        if dynamicCameraCenter {
            properties["camera_center"] = [
                "text": "Camera center", "type": "textinput", "value": cameraCenter,
            ]
        }
        if dynamicCameraUp {
            properties["camera_up"] = [
                "text": "Camera up", "type": "textinput", "value": "0 1 0",
            ]
        }
        if commandMode == .persistentRead {
            properties["effect_enabled"] = [
                "text": "Effect enabled", "type": "bool", "value": true,
            ]
        }
        if commandMode == .atomicPersistentFailure {
            properties["atomic_failure_enabled"] = [
                "text": "Atomic failure enabled", "type": "bool",
                "value": false,
            ]
        }
        if let sceneTexturePropertyKey {
            properties[sceneTexturePropertyKey] = [
                "text": "Texture",
                "type": "scenetexture",
                "value": "",
            ]
        }
        let project: [String: Any] = [
            "file": "scene.json",
            "general": [
                "properties": properties,
            ],
            "title": "Executor pixel fixture",
            "type": "scene",
            "version": 2,
        ]
        let boundTimelineAnimation: [String: Any] = [
            "c0": [
                ["frame": 0, "value": 0.0],
                ["frame": 10, "value": 1.0],
            ],
            "options": [
                "fps": 10,
                "length": 10,
                "mode": "single",
                "startpaused": true,
            ],
        ]
        let boundTimelineScript = """
        function animationContract(stage) {
            const animation = thisObject.getAnimation();
            if (animation.fps !== 10 || animation.frameCount !== 10 ||
                animation.duration !== 1) {
                throw new Error(stage + ': bound animation metadata mismatch');
            }
            return animation;
        }
        export function init(value) {
            animationContract('init').stop();
            return value;
        }
        export function update(value) {
            animationContract('update');
            return value;
        }
        export function mediaPlaybackChanged(event) {
            if (event.state !== MediaPlaybackEvent.PLAYBACK_PLAYING) return;
            animationContract('mediaPlaybackChanged').setFrame(2);
        }
        export function mediaThumbnailChanged(event) {
            if (!event.hasThumbnail) return;
            const animation = animationContract('mediaThumbnailChanged');
            animation.rate = 2;
            animation.play();
            if (!animation.isPlaying() || animation.getFrame() !== 2 ||
                animation.rate !== 2) {
                throw new Error('bound animation playback state mismatch');
            }
        }
        """
        let defaultOrigin = "\(Double(projectionWidth) * 0.5) \(Double(projectionHeight) * 0.5) 0"
        var image: [String: Any] = [
            "id": 1,
            "image": "models/pixel.json",
            "name": "Pixel image",
            "origin": imageOrigin ?? defaultOrigin,
            "size": imageSize ?? "2 2",
            "visible": true,
        ]
        if let textureAnimationScript {
            image["origin"] = [
                "value": imageOrigin ?? defaultOrigin,
                "script": textureAnimationScript,
            ]
        }
        if scriptedAudioOrigin {
            image["origin"] = [
                "value": imageOrigin ?? defaultOrigin,
                "script": """
                const audio = engine.registerAudioBuffers(16);
                export function update(value) {
                    value.y = audio.average[0] * 4.0 + 2.0;
                    return value;
                }
                """,
            ]
        }
        if colorBlendMode > 0 {
            image["colorBlendMode"] = colorBlendMode
        }
        if let compositeTintColor {
            image["effects"] = [[
                "file": "effects/composite/effect.json",
                "id": 70,
                "passes": [[
                    "combos": ["COMPOSITE": 2],
                    "constantshadervalues": [
                        "compositecolor": compositeTintColor,
                    ],
                ]],
                "visible": true,
            ]]
        }
        if let imageAngles {
            image["angles"] = imageAngles
        }
        if let imageAlignment {
            image["alignment"] = imageAlignment
        }
        if perspective {
            image["perspective"] = true
        }
        if let puppetAnimationLayer {
            image["animationlayers"] = [puppetAnimationLayer]
        }
        if projectionAuto && imageOrigin == nil {
            image["origin"] = "1 0 0"
        }
        if parallax {
            if imageOrigin == nil {
                image["origin"] = "0.5 1 0"
            }
            if imageSize == nil {
                image["size"] = "1 2"
            }
            image["parallaxDepth"] = parallaxDepth
        }
        if drawableSizedSolidLayer {
            image["origin"] = "0 0 0"
            image["size"] = "0 0"
        }
        if scriptedPointerAlpha {
            image["alpha"] = [
                "value": 0.0,
                "script": """
                export function update(value) {
                    return input.cursorScreenPosition.x
                        + input.cursorScreenPosition.y / 4.0;
                }
                """,
            ]
        }
        if scriptedMediaBoundAlphaAnimation {
            image["alpha"] = [
                "animation": boundTimelineAnimation,
                "script": boundTimelineScript,
                "value": 1.0,
            ]
        }
        if let commandMode, commandMode != .proceduralClear {
            let overrideCount: Int
            switch commandMode {
            case .swap: overrideCount = 3
            case .copy, .invalidCopy, .persistentRead,
                 .atomicPersistentFailure:
                overrideCount = 2
            case .assetCopy, .uninitializedRead: overrideCount = 1
            case .proceduralClear: preconditionFailure("procedural clear has no effect")
            }
            let effectVisible: Any
            if commandMode == .persistentRead {
                effectVisible = ["user": "effect_enabled", "value": true]
            } else {
                effectVisible = true
            }
            var effects: [[String: Any]] = [[
                "file": "effects/commands/effect.json",
                "id": 2,
                "passes": Array(repeating: [String: Any](), count: overrideCount),
                "visible": effectVisible,
            ]]
            if commandMode == .atomicPersistentFailure {
                effects.append([
                    "file": "effects/commands/failing-effect.json",
                    "id": 3,
                    "passes": [[String: Any]()],
                    "visible": [
                        "user": "atomic_failure_enabled",
                        "value": false,
                    ],
                ])
            }
            image["effects"] = effects
        }
        var general: [String: Any] = [
            "clearcolor": "0 0 0 0",
            "orthogonalprojection": projectionAuto
                ? ["auto": true]
                : ["height": projectionHeight, "width": projectionWidth],
        ]
        if parallax {
            general["cameraparallax"] = true
            general["cameraparallaxamount"] = 1.0
            general["cameraparallaxdelay"] = 0.5
            general["cameraparallaxmouseinfluence"] = 1.0
        }
        let resolvedCameraCenter: Any = dynamicCameraCenter
            ? ["user": "camera_center", "value": cameraCenter]
            : cameraCenter
        let resolvedCameraUp: Any = dynamicCameraUp
            ? ["user": "camera_up", "value": "0 1 0"]
            : "0 1 0"
        let camera: [String: Any] = [
            "center": resolvedCameraCenter,
            "eye": cameraEye,
            "nearz": nearPlane,
            "up": resolvedCameraUp,
        ]
        var objects = [image]
        if includeHealthySecondImage {
            objects.append([
                "id": 2,
                "image": "models/healthy.json",
                "name": "Healthy later image",
                "origin": imageOrigin ?? defaultOrigin,
                "size": imageSize ?? "2 2",
                "visible": true,
            ])
        }
        if includeStaticSecondImageSameShader {
            objects.append([
                "id": 2,
                "image": "models/static.json",
                "name": "Static later image",
                "origin": imageOrigin ?? defaultOrigin,
                "size": imageSize ?? "2 2",
                "visible": true,
            ])
        }
        if includeSound {
            objects.append([
                "id": 42,
                "name": "Sound",
                "playbackmode": "loop",
                "sound": ["sounds/first.mp3"],
                "visible": true,
                "volume": 0.25,
            ])
        }
        let scene: [String: Any] = [
            "camera": camera,
            "general": general,
            "objects": objects,
            "version": 1,
        ]
        let amountValue: Any
        if scriptedMediaBoundMaterialAnimation {
            amountValue = [
                "animation": boundTimelineAnimation,
                "script": boundTimelineScript,
                "value": 0.0,
            ]
        } else if scriptedAudioAmount {
            amountValue = [
                "value": 0.0,
                "script": """
                const audio = engine.registerAudioBuffers(16);
                export function update(value) { return audio.average[0]; }
                """,
            ]
        } else if scriptedMediaAmount {
            amountValue = [
                "value": 0.0,
                "script": """
                let enabled = false;
                let propertiesValid = false;
                let playbackState = MediaPlaybackEvent.PLAYBACK_STOPPED;
                let timelineValid = false;
                let thumbnailValid = false;

                export function mediaStatusChanged(event) {
                    enabled = event.enabled;
                }
                export function mediaPropertiesChanged(event) {
                    propertiesValid = event.title === 'Borrowed Song'
                        && event.artist === 'Borrowed Artist'
                        && event.contentType === 'music'
                        && event.albumTitle === 'Borrowed Album'
                        && event.subTitle === 'Borrowed Subtitle'
                        && event.albumArtist === 'Borrowed Album Artist'
                        && event.genres === 'test';
                }
                export function mediaPlaybackChanged(event) {
                    playbackState = event.state;
                }
                export function mediaTimelineChanged(event) {
                    timelineValid = event.position === 1.25
                        && event.duration === 4.5;
                }
                export function mediaThumbnailChanged(event) {
                    thumbnailValid = event.hasThumbnail === true
                        && event.primaryColor instanceof Vec3
                        && event.secondaryColor instanceof Vec3
                        && event.tertiaryColor instanceof Vec3
                        && event.textColor instanceof Vec3
                        && event.highContrastColor instanceof Vec3
                        && event.primaryColor.x === 0.1
                        && event.secondaryColor.y === 0.4
                        && event.tertiaryColor.z === 0.7
                        && event.textColor.x === 0.4
                        && event.highContrastColor.z === 1.0;
                }
                export function update(value) {
                    if (!enabled || !propertiesValid || !timelineValid
                        || !thumbnailValid) return 0.0;
                    if (playbackState === MediaPlaybackEvent.PLAYBACK_PLAYING) {
                        return 1.0;
                    }
                    if (playbackState === MediaPlaybackEvent.PLAYBACK_PAUSED) {
                        return 0.5;
                    }
                    return 0.0;
                }
                """,
            ]
        } else {
            amountValue = ["user": "amount", "value": 0.0]
        }
        var basePass: [String: Any] = [
                "blending": "normal",
                "constantshadervalues": [
                    "amount": amountValue,
                ],
                "cullmode": "nocull",
                "depthtest": "disabled",
                "depthwrite": "disabled",
                "shader": "pixel",
        ]
        if !baseCombos.isEmpty {
            basePass["combos"] = baseCombos
        }
        if commandMode != .proceduralClear {
            basePass["textures"] = includeSecondTexture ? ["red", "green"] : ["red"]
        }
        if let sceneTexturePropertyKey {
            basePass["usertextures"] = [[
                "name": sceneTexturePropertyKey,
                "type": "scenetexture",
            ]]
        } else if !hostTextureNames.isEmpty {
            basePass["usertextures"] = hostTextureNames
        }
        let material: [String: Any] = [
            "passes": [basePass],
        ]
        var model: [String: Any] = ["material": "materials/pixel.json"]
        if puppetData != nil {
            model["puppet"] = "models/pixel.mdl"
        }
        if commandMode == .proceduralClear {
            model["solidlayer"] = true
        }
        if drawableSizedSolidLayer {
            model["fullscreen"] = true
        }
        let primaryTexture = textureData ?? makeRGBA8Texture2x2(
            pixel: [255, 0, 0, 255]
        )
        var entries: [(String, Data)] = [
            ("materials/pixel.json", try json(material)),
            ("models/pixel.json", try json(model)),
            ("project.json", try json(project)),
            ("scene.json", try json(scene)),
        ]
        if textureInProjectDirectory {
            let materials = root.appendingPathComponent(
                "materials", isDirectory: true
            )
            try FileManager.default.createDirectory(
                at: materials,
                withIntermediateDirectories: true
            )
            try primaryTexture.write(
                to: materials.appendingPathComponent("red.tex")
            )
        } else {
            entries.append(("materials/red.tex", primaryTexture))
        }
        if let puppetData {
            entries.append(("models/pixel.mdl", puppetData))
        }
        if includeHealthySecondImage {
            entries.append(contentsOf: [
                ("materials/healthy.json", try json([
                    "passes": [[
                        "blending": "normal",
                        "cullmode": "nocull",
                        "depthtest": "disabled",
                        "depthwrite": "disabled",
                        "shader": "green",
                    ]],
                ])),
                ("models/healthy.json", try json([
                    "material": "materials/healthy.json",
                ])),
            ])
        }
        if includeStaticSecondImageSameShader {
            entries.append(contentsOf: [
                ("materials/static.json", try json([
                    "passes": [[
                        "blending": "normal",
                        "cullmode": "nocull",
                        "depthtest": "disabled",
                        "depthwrite": "disabled",
                        "shader": "pixel",
                        "textures": ["static"],
                    ]],
                ])),
                ("materials/static.tex", makeRGBA8Texture2x2(pixel: [0, 0, 255, 255])),
                ("models/static.json", try json([
                    "material": "materials/static.json",
                ])),
            ])
        }
        if includeSecondTexture || includeUnboundGreenTexture {
            entries.append((
                "materials/green.tex",
                makeRGBA8Texture2x2(pixel: [0, 255, 0, 255])
            ))
        }
        if let commandMode, commandMode != .proceduralClear {
            let solid: (String) -> [String: Any] = { shader in
                ["passes": [["blending": "normal", "shader": shader]]]
            }
            let finalShader = commandMode == .invalidCopy ? "broken" : "pixel"
            let finalPass: [String: Any] = [
                "blending": "normal",
                "shader": finalShader,
            ]
            let finalMaterial: [String: Any] = [
                "passes": [finalPass],
            ]
            let passes: [[String: Any]]
            switch commandMode {
            case .copy:
                passes = [
                    ["material": "materials/green.json", "target": "a"],
                    ["command": "copy", "source": "a", "target": "b"],
                    ["bind": [["index": 0, "name": "b"]], "material": "materials/final.json"],
                ]
            case .assetCopy:
                passes = [
                    ["command": "copy", "source": "green", "target": "b"],
                    ["bind": [["index": 0, "name": "b"]], "material": "materials/final.json"],
                ]
            case .swap:
                passes = [
                    ["material": "materials/red.json", "target": "a"],
                    ["material": "materials/green.json", "target": "b"],
                    ["command": "swap", "source": "a", "target": "b"],
                    ["bind": [["index": 0, "name": "a"]], "material": "materials/final.json"],
                ]
            case .invalidCopy:
                passes = [
                    ["material": "materials/green.json", "target": "a"],
                    ["command": "copy", "source": "a", "target": "b"],
                    ["bind": [["index": 0, "name": "b"]], "material": "materials/final.json"],
                ]
            case .uninitializedRead:
                passes = [
                    ["bind": [["index": 0, "name": "a"]], "material": "materials/final.json"],
                ]
            case .persistentRead:
                passes = [
                    ["material": "materials/persistent.json", "target": "a"],
                    ["bind": [["index": 0, "name": "a"]], "material": "materials/final.json"],
                ]
            case .atomicPersistentFailure:
                passes = [
                    ["material": "materials/atomic-writer.json", "target": "a"],
                    ["bind": [["index": 0, "name": "a"]], "material": "materials/final.json"],
                ]
            case .proceduralClear:
                preconditionFailure("procedural clear has no effect")
            }
            let effect: [String: Any] = [
                "fbos": [
                    ["format": "rgba8888", "name": "a", "scale": 1],
                    ["format": "rgba8888", "name": "b", "scale": 1],
                ],
                "passes": passes,
                "version": 1,
            ]
            entries.append(contentsOf: [
                ("effects/commands/effect.json", try json(effect)),
                ("materials/atomic-writer.json", try json(solid("atomic-writer"))),
                ("materials/final.json", try json(finalMaterial)),
                ("materials/green.json", try json(solid("green"))),
                ("materials/persistent.json", try json(solid("persistent"))),
                ("materials/red.json", try json(solid("red"))),
            ])
            if commandMode == .atomicPersistentFailure {
                entries.append(contentsOf: [
                    ("effects/commands/failing-effect.json", try json([
                        "fbos": [],
                        "passes": [[
                            "material": "materials/broken.json",
                        ]],
                        "version": 1,
                    ])),
                    ("materials/broken.json", try json(solid("broken"))),
                ])
            }
        }
        if compositeTintColor != nil {
            let compatibilityMaterial: (String) -> [String: Any] = { shader in
                ["passes": [[
                    "blending": "normal",
                    "cullmode": "nocull",
                    "depthtest": "disabled",
                    "depthwrite": "disabled",
                    "shader": shader,
                ]]]
            }
            entries.append(contentsOf: [
                (
                    "effects/composite/effect.json",
                    try json([
                        "fbos": [],
                        "passes": [[
                            "material": "materials/composite-effect.json",
                        ]],
                        "version": 1,
                    ])
                ),
                (
                    "materials/composite-effect.json",
                    try json(compatibilityMaterial("composite-effect"))
                ),
                (
                    "materials/effects/tint.json",
                    try json(compatibilityMaterial("composite-tint"))
                ),
                (
                    "materials/util/effectpassthrough.json",
                    try json(compatibilityMaterial("composite-blend"))
                ),
            ])
        }
        try makePackage(entries).write(to: package)
        return Fixture(root: root, assets: assets, package: package)
    }

    private func json(_ object: Any) throws -> Data {
        try JSONSerialization.data(withJSONObject: object, options: [.sortedKeys])
    }

    private func makeRGBA8Texture2x2(pixel: [UInt8]) -> Data {
        precondition(pixel.count == 4)
        return makeRGBA8Texture2x2(
            pixels: Array(repeating: pixel, count: 4).flatMap { $0 }
        )
    }

    private func makeRGBA8Texture2x2(pixels: [UInt8]) -> Data {
        precondition(pixels.count == 16)
        var result = Data()
        appendMagic("TEXV0005", to: &result)
        appendMagic("TEXI0001", to: &result)
        appendUInt32(0, to: &result) // ARGB8888, uploaded as WE's RGBA byte layout.
        appendUInt32(1, to: &result) // No interpolation, for exact pixel assertions.
        appendUInt32(2, to: &result)
        appendUInt32(2, to: &result)
        appendUInt32(2, to: &result)
        appendUInt32(2, to: &result)
        appendUInt32(0, to: &result)
        appendMagic("TEXB0003", to: &result)
        appendUInt32(1, to: &result)
        appendUInt32(UInt32.max, to: &result)
        appendUInt32(1, to: &result)
        appendUInt32(2, to: &result)
        appendUInt32(2, to: &result)
        appendUInt32(0, to: &result)
        appendUInt32(UInt32(pixels.count), to: &result)
        appendUInt32(UInt32(pixels.count), to: &result)
        result.append(contentsOf: pixels)
        return result
    }

    private func makeRawTexture2x2(format: UInt32, bytes: [UInt8]) -> Data {
        precondition(format == 8 || format == 9)
        let expectedByteCount = format == 8 ? 8 : 4
        precondition(bytes.count == expectedByteCount)
        var result = Data()
        appendMagic("TEXV0005", to: &result)
        appendMagic("TEXI0001", to: &result)
        appendUInt32(format, to: &result)
        appendUInt32(1, to: &result)
        appendUInt32(2, to: &result)
        appendUInt32(2, to: &result)
        appendUInt32(2, to: &result)
        appendUInt32(2, to: &result)
        appendUInt32(0, to: &result)
        appendMagic("TEXB0003", to: &result)
        appendUInt32(1, to: &result)
        appendUInt32(UInt32.max, to: &result)
        appendUInt32(1, to: &result)
        appendUInt32(2, to: &result)
        appendUInt32(2, to: &result)
        appendUInt32(0, to: &result)
        appendUInt32(UInt32(bytes.count), to: &result)
        appendUInt32(UInt32(bytes.count), to: &result)
        result.append(contentsOf: bytes)
        return result
    }

    private func makeEmbeddedPNGTexture2x2() -> Data {
        let png = Data(base64Encoded:
            "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAF0lEQVR4nGP8z8Dwn4GBgYGRgeE/mAEAKgMD/wc1Nu4AAAAASUVORK5CYII="
        )!
        var result = Data()
        appendMagic("TEXV0005", to: &result)
        appendMagic("TEXI0001", to: &result)
        appendUInt32(UInt32.max, to: &result)
        appendUInt32(0, to: &result)
        appendUInt32(2, to: &result)
        appendUInt32(2, to: &result)
        appendUInt32(2, to: &result)
        appendUInt32(2, to: &result)
        appendUInt32(0, to: &result)
        appendMagic("TEXB0003", to: &result)
        appendUInt32(1, to: &result)
        appendUInt32(13, to: &result)
        appendUInt32(1, to: &result)
        appendUInt32(2, to: &result)
        appendUInt32(2, to: &result)
        appendUInt32(0, to: &result)
        appendUInt32(0, to: &result)
        appendUInt32(UInt32(png.count), to: &result)
        result.append(png)
        return result
    }

    private typealias AnimatedFrame = (
        image: UInt32,
        duration: Float,
        x: Float,
        y: Float,
        width: Float,
        widthAux: Float,
        heightAux: Float,
        height: Float
    )

    private func makeAnimatedRGBA8Texture2x2(
        images: [[UInt8]],
        frames: [AnimatedFrame]
    ) -> Data {
        precondition(!images.isEmpty && !frames.isEmpty)
        precondition(images.allSatisfy { $0.count == 16 })
        var result = Data()
        appendMagic("TEXV0005", to: &result)
        appendMagic("TEXI0001", to: &result)
        appendUInt32(0, to: &result) // ARGB8888
        appendUInt32(5, to: &result) // No interpolation | IsGif
        appendUInt32(2, to: &result)
        appendUInt32(2, to: &result)
        appendUInt32(2, to: &result)
        appendUInt32(2, to: &result)
        appendUInt32(0, to: &result)
        appendMagic("TEXB0003", to: &result)
        appendUInt32(UInt32(images.count), to: &result)
        appendUInt32(UInt32.max, to: &result)
        for bytes in images {
            appendUInt32(1, to: &result)
            appendUInt32(2, to: &result)
            appendUInt32(2, to: &result)
            appendUInt32(0, to: &result)
            appendUInt32(UInt32(bytes.count), to: &result)
            appendUInt32(UInt32(bytes.count), to: &result)
            result.append(contentsOf: bytes)
        }
        appendMagic("TEXS0003", to: &result)
        appendUInt32(UInt32(frames.count), to: &result)
        appendUInt32(2, to: &result)
        appendUInt32(2, to: &result)
        for frame in frames {
            appendUInt32(frame.image, to: &result)
            appendFloat32(frame.duration, to: &result)
            appendFloat32(frame.x, to: &result)
            appendFloat32(frame.y, to: &result)
            appendFloat32(frame.width, to: &result)
            appendFloat32(frame.widthAux, to: &result)
            appendFloat32(frame.heightAux, to: &result)
            appendFloat32(frame.height, to: &result)
        }
        return result
    }

    private func makePuppetMesh(version: String = "MDLV0021") -> Data {
        let vertexStride: Int
        let uvOffset: Int
        switch version {
        case "MDLV0013":
            vertexStride = 52
            uvOffset = 44
        case "MDLV0021", "MDLV0023":
            vertexStride = 80
            uvOffset = 72
        default:
            preconditionFailure("Unsupported synthetic puppet model version")
        }
        let vertices: [(Float, Float, Float, Float, Float)] = [
            (-1, 1, 0, 0, 0),
            (1, 1, 0, 1, 0),
            (-1, -1, 0, 0, 1),
        ]
        var result = Data(version.utf8)
        result.append(0)
        appendUInt32(0, to: &result)
        appendUInt32(UInt32(vertices.count * vertexStride), to: &result)
        for (x, y, z, u, v) in vertices {
            let start = result.count
            appendFloat32(x, to: &result)
            appendFloat32(y, to: &result)
            appendFloat32(z, to: &result)
            result.append(Data(repeating: 0, count: uvOffset - 12))
            appendFloat32(u, to: &result)
            appendFloat32(v, to: &result)
            precondition(result.count - start == vertexStride)
        }
        appendUInt32(6, to: &result)
        appendUInt16(0, to: &result)
        appendUInt16(1, to: &result)
        appendUInt16(2, to: &result)
        result.append(contentsOf: Array("MDLS".utf8))
        return result
    }

    private func makeAnimatedPuppetMesh() -> Data {
        let vertices: [(Float, Float, Float, Float, Float)] = [
            (-1, 1, 0, 0, 0),
            (1, 1, 0, 1, 0),
            (-1, -1, 0, 0, 1),
        ]
        var result = Data("MDLV0021".utf8)
        result.append(0)
        appendUInt32(0, to: &result)
        appendUInt32(UInt32(vertices.count * 80), to: &result)
        for (x, y, z, u, v) in vertices {
            let start = result.count
            appendFloat32(x, to: &result)
            appendFloat32(y, to: &result)
            appendFloat32(z, to: &result)
            result.append(Data(repeating: 0, count: 28))
            for _ in 0..<4 { appendUInt32(0, to: &result) }
            appendFloat32(1, to: &result)
            for _ in 0..<3 { appendFloat32(0, to: &result) }
            appendFloat32(u, to: &result)
            appendFloat32(v, to: &result)
            precondition(result.count - start == 80)
        }
        appendUInt32(6, to: &result)
        appendUInt16(0, to: &result)
        appendUInt16(1, to: &result)
        appendUInt16(2, to: &result)

        appendMagic("MDLS0003", to: &result)
        let animationOffsetField = result.count
        appendUInt32(0, to: &result)
        appendUInt32(1, to: &result)
        result.append(0)
        appendUInt32(1, to: &result)
        appendUInt32(UInt32.max, to: &result)
        appendUInt32(64, to: &result)
        let identity: [Float] = [
            1, 0, 0, 0,
            0, 1, 0, 0,
            0, 0, 1, 0,
            0, 0, 0, 1,
        ]
        for component in identity { appendFloat32(component, to: &result) }
        result.append(0)

        let animationOffset = result.count
        replaceUInt32(UInt32(animationOffset), at: animationOffsetField, in: &result)
        appendMagic("MDLA0006", to: &result)
        let animationEndField = result.count
        appendUInt32(0, to: &result)
        appendUInt32(1, to: &result)
        appendUInt32(7, to: &result)
        appendUInt32(0, to: &result)
        result.append(contentsOf: Array("Translation".utf8))
        result.append(0)
        result.append(contentsOf: Array("loop".utf8))
        result.append(0)
        appendFloat32(1, to: &result)
        appendUInt32(1, to: &result)
        appendUInt32(0, to: &result)
        appendUInt32(1, to: &result)
        appendUInt32(0, to: &result)
        appendUInt32(72, to: &result)
        appendPuppetTransform(translationX: 0, to: &result)
        appendPuppetTransform(translationX: 1, to: &result)
        result.append(Data(repeating: 0, count: 36))
        replaceUInt32(UInt32(result.count - 1), at: animationEndField, in: &result)
        return result
    }

    private func appendPuppetTransform(translationX: Float, to data: inout Data) {
        appendFloat32(translationX, to: &data)
        appendFloat32(0, to: &data)
        appendFloat32(0, to: &data)
        for _ in 0..<3 { appendFloat32(0, to: &data) }
        for _ in 0..<3 { appendFloat32(1, to: &data) }
    }

    private func makePackage(_ entries: [(String, Data)]) -> Data {
        var table = Data()
        var payload = Data()
        appendUInt32(8, to: &table)
        table.append(contentsOf: Array("PKGV0001".utf8))
        appendUInt32(UInt32(entries.count), to: &table)
        for (path, bytes) in entries {
            let pathBytes = Array(path.utf8)
            appendUInt32(UInt32(pathBytes.count), to: &table)
            table.append(contentsOf: pathBytes)
            appendUInt32(UInt32(payload.count), to: &table)
            appendUInt32(UInt32(bytes.count), to: &table)
            payload.append(bytes)
        }
        table.append(payload)
        return table
    }

    private func appendMagic(_ value: String, to data: inout Data) {
        let bytes = Array(value.utf8)
        precondition(bytes.count == 8)
        data.append(contentsOf: bytes)
        data.append(0)
    }

    private func appendUInt16(_ value: UInt16, to data: inout Data) {
        data.append(UInt8(truncatingIfNeeded: value))
        data.append(UInt8(truncatingIfNeeded: value >> 8))
    }

    private func appendUInt32(_ value: UInt32, to data: inout Data) {
        data.append(UInt8(truncatingIfNeeded: value))
        data.append(UInt8(truncatingIfNeeded: value >> 8))
        data.append(UInt8(truncatingIfNeeded: value >> 16))
        data.append(UInt8(truncatingIfNeeded: value >> 24))
    }

    private func appendFloat32(_ value: Float, to data: inout Data) {
        appendUInt32(value.bitPattern, to: &data)
    }

    private func replaceUInt32(_ value: UInt32, at offset: Int, in data: inout Data) {
        var encoded = Data()
        appendUInt32(value, to: &encoded)
        data.replaceSubrange(offset..<(offset + 4), with: encoded)
    }

    private func replaceUInt16(_ value: UInt16, at offset: Int, in data: inout Data) {
        var encoded = Data()
        appendUInt16(value, to: &encoded)
        data.replaceSubrange(offset..<(offset + 2), with: encoded)
    }

    private func pointerContractPixel(
        _ bottomLeftPointer: (x: Double, y: Double)
    ) -> [UInt8] {
        func normalizedByte(_ value: Double) -> UInt8 {
            UInt8(clamping: Int((value * 255).rounded()))
        }
        return [
            normalizedByte(bottomLeftPointer.x),
            normalizedByte(1 - bottomLeftPointer.y),
            normalizedByte(
                bottomLeftPointer.x + bottomLeftPointer.y / 4
            ),
            255,
        ]
    }

    private func repeatedPixel(_ pixel: [UInt8], count: Int = 4) -> [UInt8] {
        Array(repeating: pixel, count: count).flatMap { $0 }
    }

    private func containsOpaqueRedPixel(_ pixels: [UInt8]) -> Bool {
        stride(from: 0, to: pixels.count, by: 4).contains { offset in
            pixels[offset] == 255 && pixels[offset + 1] == 0 &&
                pixels[offset + 2] == 0 && pixels[offset + 3] == 255
        }
    }

    private func repeatedRows(_ row: [[UInt8]], count: Int) -> [UInt8] {
        Array(repeating: row, count: count).flatMap { $0 }.flatMap { $0 }
    }

    private func errorMessage(_ error: WESceneRuntimeErrorRef?) -> String {
        we_scene_runtime_error_message(error).map(String.init(cString:)) ?? "No runtime error"
    }

    private func failure(_ phase: String, _ error: WESceneRuntimeErrorRef?) -> NSError {
        let message = errorMessage(error)
        we_scene_runtime_error_destroy(error)
        return NSError(
            domain: "FrameExecutorPixelTests",
            code: 1,
            userInfo: [NSLocalizedDescriptionKey: "\(phase) failed: \(message)"]
        )
    }
}
