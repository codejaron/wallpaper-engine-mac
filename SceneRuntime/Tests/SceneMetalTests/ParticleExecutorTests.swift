import Foundation
import Metal
import SceneMetalTestSupport
import SceneRuntimeBridge
import XCTest

final class ParticleExecutorTests: XCTestCase {
    private struct Fixture {
        let root: URL
        let assets: URL
        let package: URL
    }

    private struct Pipeline {
        let fixture: Fixture
        let runtime: WESceneRuntimeRef
        let model: WESceneModelRef
        let graph: WESceneGraphRef
        let frameGraph: WESceneFrameGraphRef
        let executor: WESceneFrameExecutorRef
    }

    func testDynamicParticleCloneKeepsIndependentRuntimeState() throws {
        let loaded = try loadPipeline(
            fixture: makeFixture(
                includeText: false,
                dynamicClone: true,
                particleVelocity: "0 0 0"
            ),
            context: nil
        )
        defer { destroy(loaded) }

        try render(loaded.executor, time: 1, delta: 1.0 / 120.0)
        try assertNoExecutionIssues(loaded.executor)
        let firstColumns = greenColumns(
            try readPixels(loaded.executor),
            width: 16
        )
        XCTAssertTrue(firstColumns.contains { $0 < 8 })
        XCTAssertTrue(firstColumns.contains { $0 >= 8 })

        try render(loaded.executor, time: 2, delta: 1.0 / 120.0)
        try assertNoExecutionIssues(loaded.executor)
        let secondColumns = greenColumns(
            try readPixels(loaded.executor),
            width: 16
        )
        XCTAssertTrue(secondColumns.contains { $0 < 8 })
        XCTAssertTrue(secondColumns.contains { $0 >= 8 })
    }

    func testHealthyParticleCommitsWhenLaterTextObjectIsSkipped() throws {
        let isolated = try loadPipeline(includeText: true)
        let control = try loadPipeline(includeText: true)
        defer {
            destroy(control)
            destroy(isolated)
        }

        try render(isolated.executor, time: 1, delta: 1.0 / 120.0)
        try render(control.executor, time: 1, delta: 1.0 / 120.0)
        XCTAssertEqual(
            try readPixels(isolated.executor),
            try readPixels(control.executor)
        )

        try setText(isolated.model, String(repeating: "W", count: 10_000))
        try render(isolated.executor, time: 500, delta: 1.0 / 120.0)
        try render(control.executor, time: 500, delta: 1.0 / 120.0)
        XCTAssertFalse(
            greenColumns(try readPixels(isolated.executor), width: 16).isEmpty,
            "The healthy particle object must still draw when the later text object fails preflight"
        )
        try assertSingleSkippedObjectIssue(
            isolated.executor,
            objectIndex: 1,
            objectId: 2,
            operationIndex: 1,
            messageContains: ["dimensions exceed the supported limit"]
        )

        try setText(isolated.model, "I")
        try render(isolated.executor, time: 1_000, delta: 0)
        try render(control.executor, time: 1_000, delta: 0)
        XCTAssertEqual(
            try readPixels(isolated.executor),
            try readPixels(control.executor),
            "Skipping the later text object must not roll back the healthy particle object's committed simulation"
        )
        var issueCount = 99
        var error: WESceneRuntimeErrorRef?
        XCTAssertEqual(
            we_scene_frame_executor_issue_count(
                isolated.executor, &issueCount, &error
            ),
            1,
            errorMessage(error)
        )
        XCTAssertEqual(issueCount, 0)
        XCTAssertNil(error)
    }

    func testSkippedParticleAfterAdvanceDoesNotCommitSimulationState() throws {
        let isolated = try loadPipeline(includeText: false)
        let control = try loadPipeline(includeText: false)
        defer {
            destroy(control)
            destroy(isolated)
        }

        // Finite frame input is accepted globally, then fails particle g_Time
        // narrowing only after the candidate simulation has advanced.
        try render(
            isolated.executor,
            time: 1e100,
            delta: 1.0 / 120.0
        )
        try assertSingleSkippedObjectIssue(
            isolated.executor,
            objectIndex: 0,
            objectId: 1,
            operationIndex: 0,
            messageContains: [
                "Particle frame time",
                "non-finite or out-of-range",
            ]
        )

        try render(isolated.executor, time: 2, delta: 1.0 / 120.0)
        try render(control.executor, time: 2, delta: 1.0 / 120.0)
        let isolatedPixels = try readPixels(isolated.executor)
        let controlPixels = try readPixels(control.executor)
        XCTAssertFalse(
            greenColumns(isolatedPixels, width: 16).isEmpty,
            "The recovered particle must be visible without relying on a text object"
        )
        XCTAssertEqual(
            isolatedPixels,
            controlPixels,
            "A particle advance from a skipped object must not be committed"
        )

        var issueCount = 99
        var error: WESceneRuntimeErrorRef?
        XCTAssertEqual(
            we_scene_frame_executor_issue_count(
                isolated.executor, &issueCount, &error
            ),
            1,
            errorMessage(error)
        )
        XCTAssertEqual(issueCount, 0)
        XCTAssertNil(error)
    }

    func testParticleBuiltinUniformTypeFailureSkipsOnlyParticleObject() throws {
        let loaded = try loadPipeline(
            fixture: makeFixture(
                includeText: true,
                particleFragmentShaderSource: invalidParticleTimeFragmentShader
            ),
            context: nil
        )
        defer { destroy(loaded) }

        try render(loaded.executor, time: 1, delta: 1.0 / 120.0)
        let pixels = try readPixels(loaded.executor)
        XCTAssertTrue(
            stride(from: 0, to: pixels.count, by: 4).contains { offset in
                pixels[offset] > 0 && pixels[offset + 1] == 0
            },
            "A particle builtin type mismatch must not suppress the later text object"
        )
        XCTAssertTrue(
            greenColumns(pixels, width: 16).isEmpty,
            "The particle with an incompatible builtin uniform must be skipped"
        )
        try assertSingleSkippedObjectIssue(
            loaded.executor,
            objectIndex: 0,
            objectId: 1,
            operationIndex: 0,
            messageContains: ["g_Time", "type"]
        )
    }

    func testTextAnglesApplyTheAuthoredLayerRotation() throws {
        let unrotated = try loadPipeline(
            fixture: makeFixture(
                includeText: true,
                particleInstantaneousCount: 0,
                textValue: "L",
                textOrigin: "8 4 0"
            ),
            context: nil
        )
        let authoredRotation = try loadPipeline(
            fixture: makeFixture(
                includeText: true,
                particleInstantaneousCount: 0,
                textValue: "L",
                textOrigin: "8 4 0",
                textAngles: "0 0 1.5707963267948966"
            ),
            context: nil
        )
        defer {
            destroy(authoredRotation)
            destroy(unrotated)
        }

        try render(unrotated.executor, time: 1, delta: 0)
        try render(authoredRotation.executor, time: 1, delta: 0)
        XCTAssertNotEqual(
            try readPixels(authoredRotation.executor),
            try readPixels(unrotated.executor),
            "Text layers use the same authored transform contract as other scene layers"
        )
    }

    func testNonAtlasParticleRenderVarCarriesRealTextureAspectRatio() throws {
        let fixture = try makeFixture(
            includeText: false,
            particleTextureSize: (4, 2),
            particleVertexShaderSource: minimalParticleVertexShader,
            particleFragmentShaderSource: particleAspectFragmentShader
        )
        let loaded = try loadPipeline(fixture: fixture, context: nil)
        defer { destroy(loaded) }

        try render(loaded.executor, time: 1, delta: 1.0 / 120.0)
        let pixels = try readPixels(loaded.executor)
        XCTAssertTrue(
            stride(from: 0, to: pixels.count, by: 4).contains {
                pixels[$0] == 0 && pixels[$0 + 1] == 255
            },
            "A non-atlas particle must receive realHeight/realWidth in g_RenderVar1.w"
        )
        XCTAssertFalse(
            stride(from: 0, to: pixels.count, by: 4).contains {
                pixels[$0] == 255 && pixels[$0 + 1] == 0
            }
        )
    }

    func testResolutionOnlyParticleTextureSlotUsesPrimaryTextureProvider() throws {
        let fixture = try makeFixture(
            includeText: false,
            particleVertexShaderSource: """
            attribute vec3 a_Position;
            attribute vec4 a_TexCoordVec4;
            uniform mat4 g_ModelViewProjectionMatrix;
            uniform vec4 g_Texture0Resolution;
            uniform vec4 g_Texture1Resolution;
            varying vec2 v_TexCoord;
            varying vec4 v_Color;
            varying vec4 v_ResolutionDelta;
            void main() {
                vec2 corner = a_TexCoordVec4.xy - vec2(0.5);
                gl_Position = g_ModelViewProjectionMatrix
                    * vec4(a_Position.xy + corner * 2.0, a_Position.z, 1.0);
                v_TexCoord = a_TexCoordVec4.xy;
                v_Color = vec4(1.0);
                v_ResolutionDelta = g_Texture1Resolution
                    - g_Texture0Resolution;
            }
            """,
            particleFragmentShaderSource: """
            varying vec4 v_ResolutionDelta;
            void main() {
                bool ok = all(lessThan(
                    abs(v_ResolutionDelta),
                    vec4(0.0001)
                ));
                gl_FragColor = ok ? vec4(0.0, 1.0, 0.0, 1.0)
                                  : vec4(1.0, 0.0, 0.0, 1.0);
            }
            """
        )
        let loaded = try loadPipeline(fixture: fixture, context: nil)
        defer { destroy(loaded) }

        try render(loaded.executor, time: 1, delta: 1.0 / 120.0)
        try assertNoExecutionIssues(loaded.executor)
        let pixels = try readPixels(loaded.executor)
        XCTAssertTrue(
            hasPixel([0, 255, 0, 255], in: pixels),
            "A resolution-only particle slot must inherit the real primary texture"
        )
        XCTAssertFalse(hasPixel([255, 0, 0, 255], in: pixels))
    }

    func testAnimatedAtlasRenderVarUsesAuthoredFrameAspectRatio() throws {
        let pixels = Array(
            repeating: [UInt8](arrayLiteral: 255, 255, 255, 255),
            count: 4
        ).flatMap { $0 }
        let atlas = makeAnimatedParticleTexture(
            images: [pixels],
            frames: [
                (0, 0.5, 0, 0, 2, 0, 0, 1),
                (0, 0.5, 0, 1, 2, 0, 0, 1),
            ],
            gifWidth: 2,
            gifHeight: 1
        )
        let loaded = try loadPipeline(
            fixture: makeFixture(
                includeText: false,
                particleTextureData: atlas,
                particleVertexShaderSource: minimalParticleVertexShader,
                particleFragmentShaderSource: particleAspectFragmentShader
            ),
            context: nil
        )
        defer { destroy(loaded) }

        try render(loaded.executor, time: 1, delta: 1.0 / 120.0)
        let rendered = try readPixels(loaded.executor)
        XCTAssertTrue(
            stride(from: 0, to: rendered.count, by: 4).contains {
                rendered[$0] == 0 && rendered[$0 + 1] == 255
            },
            "Animated particle geometry must use the authored frame height/width, not the padded atlas ratio"
        )
        try assertNoExecutionIssues(loaded.executor)
    }

    func testParticleBindsLinuxCommonAndInverseModelUniforms() throws {
        let fixture = try makeFixture(
            includeText: false,
            particleFragmentShaderSource: particleBuiltinContractFragmentShader
        )
        let loaded = try loadPipeline(fixture: fixture, context: nil)
        defer { destroy(loaded) }

        try render(loaded.executor, time: 1, delta: 1.0 / 120.0)
        let pixels = try readPixels(loaded.executor)
        XCTAssertTrue(
            stride(from: 0, to: pixels.count, by: 4).contains {
                pixels[$0] == 0 && pixels[$0 + 1] == 255
            }
        )
        XCTAssertFalse(
            stride(from: 0, to: pixels.count, by: 4).contains {
                pixels[$0] == 255 && pixels[$0 + 1] == 0
            },
            "Particle common and inverse-model builtins must use the prepared frame state"
        )
    }

    func testParticleRawTextureFormatOverridesAuthoredTextureZeroCombo() throws {
        let fixture = try makeFixture(
            includeText: false,
            particleTextureData: makeRawParticleTexture2x2(
                format: 9,
                bytes: [255, 255, 255, 255]
            ),
            particleMaterialCombos: ["TEX0FORMAT": 8],
            particleVertexShaderSource: minimalParticleVertexShader,
            particleFragmentShaderSource: """
            varying vec2 v_TexCoord;
            varying vec4 v_Color;
            void main() {
            #if TEX0FORMAT == 9
                gl_FragColor = vec4(0.0, 1.0, 0.0, 1.0);
            #else
                gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0);
            #endif
            }
            """
        )
        let loaded = try loadPipeline(fixture: fixture, context: nil)
        defer { destroy(loaded) }

        try render(loaded.executor, time: 1, delta: 1.0 / 120.0)
        let pixels = try readPixels(loaded.executor)
        XCTAssertTrue(
            stride(from: 0, to: pixels.count, by: 4).contains {
                pixels[$0] == 0 && pixels[$0 + 1] == 255
            }
        )
        XCTAssertFalse(
            stride(from: 0, to: pixels.count, by: 4).contains {
                pixels[$0] == 255 && pixels[$0 + 1] == 0
            },
            "The particle primary texture format must replace an authored TEX0FORMAT value"
        )
    }

    func testParticleColorOverrideIsAppliedOnceThroughLinuxCommonUniform() throws {
        let fixture = try makeFixture(
            includeText: false,
            particleVelocity: "0 0 0",
            particleInstanceOverride: [
                "enabled": true,
                "color": "0.5 0.5 0.5",
                "colorn": "1 0.5 1",
            ],
            particleFragmentShaderSource: particleColorOverrideFragmentShader
        )
        let loaded = try loadPipeline(fixture: fixture, context: nil)
        defer { destroy(loaded) }

        try render(loaded.executor, time: 1, delta: 1.0 / 120.0)
        let pixels = try readPixels(loaded.executor)
        XCTAssertTrue(
            stride(from: 0, to: pixels.count, by: 4).contains {
                pixels[$0] == 0 && pixels[$0 + 1] == 64 &&
                    pixels[$0 + 2] == 0 && pixels[$0 + 3] == 255
            },
            "colorn must tint the vertex once and color must be applied once through g_Color"
        )
        XCTAssertFalse(
            stride(from: 0, to: pixels.count, by: 4).contains {
                pixels[$0] == 0 && pixels[$0 + 1] == 32 &&
                    pixels[$0 + 2] == 0
            },
            "instanceoverride.color must not be multiplied into both the vertex and g_Color"
        )
    }

    func testParticleAttributesAreBoundOnlyWhenActiveLikeLinux() throws {
        let fixture = try makeFixture(
            includeText: false,
            particleVertexShaderSource: minimalParticleVertexShader,
            particleFragmentShaderSource: constantGreenParticleFragmentShader
        )
        let loaded = try loadPipeline(fixture: fixture, context: nil)
        defer { destroy(loaded) }

        try render(loaded.executor, time: 1, delta: 1.0 / 120.0)
        XCTAssertFalse(
            greenColumns(try readPixels(loaded.executor), width: 16).isEmpty
        )
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

    func testMissingParticleTextureSkipsParticleAndLaterTextStillRenders() throws {
        let loaded = try loadPipeline(
            fixture: makeFixture(
                includeText: true,
                particleTextureName: "missing"
            ),
            context: nil
        )
        defer { destroy(loaded) }

        try render(loaded.executor, time: 1, delta: 1.0 / 120.0)
        let pixels = try readPixels(loaded.executor)
        XCTAssertTrue(
            stride(from: 0, to: pixels.count, by: 4).contains { offset in
                pixels[offset] > 0 && pixels[offset + 1] == 0
            },
            "The healthy text object after the bad particle must remain visible"
        )
        XCTAssertTrue(greenColumns(pixels, width: 16).isEmpty)
        try assertSingleSkippedObjectIssue(
            loaded.executor,
            objectIndex: 0,
            objectId: 1,
            operationIndex: 0,
            messageContains: ["missing"]
        )
    }

    func testEmptyParticleBatchPreservesOutput() throws {
        let loaded = try loadPipeline(fixture: makeFixture(
            includeText: false,
            particleInstantaneousCount: 0,
            includeDiscardImageBeforeParticle: true
        ), context: nil)
        defer { destroy(loaded) }

        try render(loaded.executor, time: 1, delta: 1.0 / 120.0)
        let pixels = try readPixels(loaded.executor)
        XCTAssertTrue(
            stride(from: 0, to: pixels.count, by: 4).allSatisfy {
                Array(pixels[$0 ..< $0 + 4]) == [0, 0, 0, 255]
            },
            "The priming image must discard and the empty particle batch must preserve the opaque scene clear"
        )
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

    func testReplayDoesNotAdvanceScriptParticleOrRandomContinuation() throws {
        let replayed = try loadPipeline(
            fixture: makeFixture(
                includeText: false,
                scriptedOrigin: true,
                stochasticParticles: true,
                projectionAuto: true
            ),
            context: nil
        )
        let control = try loadPipeline(
            fixture: makeFixture(
                includeText: false,
                scriptedOrigin: true,
                stochasticParticles: true,
                projectionAuto: true
            ),
            context: nil
        )
        defer {
            destroy(control)
            destroy(replayed)
        }

        try render(
            replayed.executor, time: 1, delta: 1.0 / 120.0,
            drawableWidth: 10, drawableHeight: 6
        )
        try render(
            control.executor, time: 1, delta: 1.0 / 120.0,
            drawableWidth: 10, drawableHeight: 6
        )
        let evaluatedFrame = try readPixels(replayed.executor)
        XCTAssertEqual(evaluatedFrame, try readPixels(control.executor))

        var error: WESceneRuntimeErrorRef?
        XCTAssertEqual(
            we_scene_frame_executor_replay_for_drawable(replayed.executor, 20, 5, &error),
            1,
            errorMessage(error)
        )
        XCTAssertEqual(we_scene_frame_executor_width(replayed.executor), 20)
        XCTAssertEqual(we_scene_frame_executor_height(replayed.executor), 5)
        XCTAssertEqual(we_scene_frame_executor_rgba8_byte_count(replayed.executor), 400)
        XCTAssertEqual(
            we_scene_frame_executor_replay_for_drawable(replayed.executor, 10, 6, &error),
            1,
            errorMessage(error)
        )
        XCTAssertEqual(
            try readPixels(replayed.executor),
            evaluatedFrame,
            "A replay may redraw a cached frame, but must not evaluate scripts or advance particles"
        )

        try render(
            replayed.executor, time: 2, delta: 1.0 / 120.0,
            drawableWidth: 20, drawableHeight: 5
        )
        try render(
            control.executor, time: 2, delta: 1.0 / 120.0,
            drawableWidth: 20, drawableHeight: 5
        )
        XCTAssertEqual(
            try readPixels(replayed.executor),
            try readPixels(control.executor),
            "Replay must preserve the same script, particle, and RNG continuation as no replay"
        )
    }

    func testOfficialExampleParticleRendersWithGenericParticleShader() throws {
        guard let assetsPath = ProcessInfo.processInfo.environment["WE_ASSETS_DIR"],
              !assetsPath.isEmpty else {
            throw XCTSkip("WE_ASSETS_DIR is required")
        }
        let fixture = try makeOfficialFixture(
            assets: URL(fileURLWithPath: assetsPath, isDirectory: true)
        )
        let loaded = try loadPipeline(fixture: fixture, context: nil)
        defer { destroy(loaded) }

        try render(loaded.executor, time: 0.2, delta: 0.2)
        let pixels = try readPixels(loaded.executor)
        XCTAssertTrue(stride(from: 0, to: pixels.count, by: 4).contains { offset in
            pixels[offset] != 0 || pixels[offset + 1] != 0 || pixels[offset + 2] != 0
        })
    }

    func testSpriteTrailRendererUsesTrailComboAndRenders() throws {
        let loaded = try loadPipeline(
            fixture: makeFixture(
                includeText: false,
                particleRendererName: "spritetrail",
                particleRendererParameters: [
                    "length": 0.2,
                    "maxlength": 4.0,
                    "minlength": 0.1,
                ],
                particleVelocity: "40 0 0"
            ),
            context: nil
        )
        defer { destroy(loaded) }

        try render(loaded.executor, time: 1, delta: 1.0 / 120.0)
        let pixels = try readPixels(loaded.executor)
        XCTAssertFalse(greenColumns(pixels, width: 16).isEmpty)
        try assertNoExecutionIssues(loaded.executor)
    }

    func testSpriteRendererExecutesAuthoredCustomShader() throws {
        let loaded = try loadPipeline(
            fixture: makeFixture(
                includeText: false,
                particleShaderName: "customparticle",
                particleVelocity: "0 0 0",
                particleVertexShaderSource: minimalParticleVertexShader,
                particleFragmentShaderSource: constantGreenParticleFragmentShader
            ),
            context: nil
        )
        defer { destroy(loaded) }

        try render(loaded.executor, time: 1, delta: 1.0 / 120.0)
        XCTAssertFalse(greenColumns(try readPixels(loaded.executor), width: 16).isEmpty)
        try assertNoExecutionIssues(loaded.executor)
    }

    func testSpriteParticleWithBackfaceCullingRendersFrontFacingQuad() throws {
        let loaded = try loadPipeline(
            fixture: makeFixture(
                includeText: false,
                particleCullMode: "normal"
            ),
            context: nil
        )
        defer { destroy(loaded) }

        try render(loaded.executor, time: 1, delta: 1.0 / 120.0)
        XCTAssertFalse(
            greenColumns(try readPixels(loaded.executor), width: 16).isEmpty,
            "Sprite particles must remain front-facing when their material enables backface culling"
        )
        try assertNoExecutionIssues(loaded.executor)
    }

    func testRopeRendererBuildsThickGeometryAndUsesRopeShader() throws {
        let loaded = try loadPipeline(
            fixture: makeFixture(
                includeText: false,
                stochasticParticles: true,
                particleRendererName: "rope",
                particleRendererParameters: [
                    "subdivision": 2,
                    "uvscale": 1.5,
                ],
                particleMaxCount: 8
            ),
            context: nil
        )
        defer { destroy(loaded) }

        try render(loaded.executor, time: 1, delta: 0.1)
        let pixels = try readPixels(loaded.executor)
        var hasVisiblePixel = false
        for offset in stride(from: 0, to: pixels.count, by: 4) {
            if pixels[offset] != 0 || pixels[offset + 1] != 0 || pixels[offset + 2] != 0 {
                hasVisiblePixel = true
                break
            }
        }
        XCTAssertTrue(hasVisiblePixel)
        try assertNoExecutionIssues(loaded.executor)
    }

    func testRopeTrailRendererUsesTrailComboAndRenders() throws {
        let loaded = try loadPipeline(
            fixture: makeFixture(
                includeText: false,
                stochasticParticles: true,
                particleRendererName: "ropetrail",
                particleRendererParameters: [
                    "segments": 5,
                    "subdivision": 2,
                    "uvscale": 1.25,
                    "uvscrolling": true,
                ],
                particleMaxCount: 8
            ),
            context: nil
        )
        defer { destroy(loaded) }

        try render(loaded.executor, time: 0.1, delta: 0.1)
        try render(loaded.executor, time: 0.2, delta: 0.1)
        let pixels = try readPixels(loaded.executor)
        XCTAssertTrue(
            stride(from: 0, to: pixels.count, by: 4).contains {
                pixels[$0] != 0 || pixels[$0 + 1] != 0 || pixels[$0 + 2] != 0
            }
        )
        try assertNoExecutionIssues(loaded.executor)
    }

    func testRopeTrailGeometrySurvivesFollowingSpriteUpload() throws {
        let loaded = try loadPipeline(
            fixture: makeFixture(
                includeText: false,
                particleRendererName: "ropetrail",
                particleRendererParameters: [
                    "length": 0.4,
                    "segments": 4,
                    "subdivision": 1,
                ],
                particleOrigin: "4 4 0",
                particleVelocity: "10 0 0",
                includeStaticAnimationResetParticle: true,
                followingParticleRendererName: "sprite",
                particleVertexShaderSource: minimalParticleVertexShader,
                particleFragmentShaderSource: constantGreenParticleFragmentShader
            ),
            context: nil
        )
        defer { destroy(loaded) }

        try render(loaded.executor, time: 0.1, delta: 0.1)
        try render(loaded.executor, time: 0.2, delta: 0.1)
        let columns = greenColumns(
            try readPixels(loaded.executor),
            width: 16
        )
        XCTAssertTrue(
            columns.contains { $0 < 8 },
            "The earlier rope draw must keep its own vertex layout and frame geometry"
        )
        XCTAssertTrue(
            columns.contains { $0 >= 8 },
            "The following sprite draw must use its own frame geometry slice"
        )
        try assertNoExecutionIssues(loaded.executor)
    }

    func testRopeTrailKeepsIndependentHistoryPerParticle() throws {
        let fixedEmitter: (String) -> [String: Any] = { origin in
            [
                "directions": "1 1 0",
                "distancemax": 0,
                "distancemin": 0,
                "instantaneous": 1,
                "name": "boxrandom",
                "origin": origin,
                "rate": 0,
            ]
        }
        let loaded = try loadPipeline(
            fixture: makeFixture(
                includeText: false,
                particleRendererName: "ropetrail",
                particleRendererParameters: [
                    "length": 0.4,
                    "segments": 4,
                    "subdivision": 1,
                ],
                particleMaxCount: 2,
                particleOrigin: "8 2 0",
                particleVelocity: "0 10 0",
                particleEmitters: [
                    fixedEmitter("-4 0 0"),
                    fixedEmitter("4 0 0"),
                ]
            ),
            context: nil
        )
        defer { destroy(loaded) }

        for frame in 1 ... 4 {
            try render(
                loaded.executor,
                time: Double(frame) * 0.1,
                delta: 0.1
            )
        }
        let columns = greenColumns(
            try readPixels(loaded.executor),
            width: 16
        )
        XCTAssertFalse(columns.isEmpty)
        XCTAssertFalse(
            columns.contains(8),
            "Rope trails must not connect unrelated particles across their spawn gap"
        )
        try assertNoExecutionIssues(loaded.executor)
    }

    func testRopeTrailPresentsAccumulatorBetweenFixedSimulationSteps() throws {
        let loaded = try loadPipeline(
            fixture: makeFixture(
                includeText: false,
                particleRendererName: "ropetrail",
                particleRendererParameters: [
                    "length": 0.4,
                    "segments": 4,
                    "subdivision": 1,
                ],
                particleMaxCount: 1,
                particleOrigin: "4 4 0",
                particleVelocity: "240 0 0"
            ),
            context: nil
        )
        defer { destroy(loaded) }

        let fixedStep = 1.0 / 120.0
        try render(loaded.executor, time: fixedStep, delta: fixedStep)
        try render(loaded.executor, time: fixedStep * 2, delta: fixedStep)
        let fixedBoundary = try readPixels(loaded.executor)

        try render(
            loaded.executor,
            time: fixedStep * 2.5,
            delta: fixedStep * 0.5
        )
        let betweenSteps = try readPixels(loaded.executor)

        XCTAssertNotEqual(
            betweenSteps,
            fixedBoundary,
            "Rope trails must use the presentation accumulator instead of freezing until the next fixed simulation step"
        )
        try assertNoExecutionIssues(loaded.executor)
    }

    func testRopeTrailSegmentCountDoesNotThrottleHistoryFrameRate() throws {
        let loaded = try loadPipeline(
            fixture: makeFixture(
                includeText: false,
                particleRendererName: "ropetrail",
                particleRendererParameters: [
                    "length": 0.4,
                    "segments": 4,
                    "subdivision": 1,
                ],
                particleMaxCount: 1,
                particleOrigin: "10 8 0",
                particleVelocity: "120 0 0",
                projectionWidth: 128,
                projectionHeight: 16
            ),
            context: nil
        )
        defer { destroy(loaded) }

        let frameDuration = 1.0 / 60.0
        for frame in 1 ... 30 {
            try render(
                loaded.executor,
                time: Double(frame) * frameDuration,
                delta: frameDuration
            )
        }

        var trailStarts: [Int] = []
        for frame in 31 ... 35 {
            try render(
                loaded.executor,
                time: Double(frame) * frameDuration,
                delta: frameDuration
            )
            let columns = greenColumns(
                try readPixels(loaded.executor),
                width: 128
            )
            trailStarts.append(try XCTUnwrap(columns.min()))
        }

        XCTAssertEqual(
            Set(trailStarts).count,
            trailStarts.count,
            "Authored Rope Trail segments must control geometry density, not reduce history motion to length / segments"
        )
        try assertNoExecutionIssues(loaded.executor)
    }

    func testAnimatedParticleTextureSelectsImageAndAppliesFrameUVBasis() throws {
        let red = Array(repeating: [UInt8](arrayLiteral: 255, 0, 0, 255), count: 4)
            .flatMap { $0 }
        let yellow: [UInt8] = [255, 255, 0, 255]
        let blue: [UInt8] = [0, 0, 255, 255]
        let transformedSecondImage = [yellow, blue, yellow, blue].flatMap { $0 }
        let texture = makeAnimatedParticleTexture(
            images: [red, transformedSecondImage],
            frames: [
                (0, 0.25, 0, 0, 2, 0, 0, 2),
                (1, 0.75, 1, 0, 1, 0, 0, 2),
            ]
        )
        let loaded = try loadPipeline(
            fixture: makeFixture(
                includeText: false,
                particleVelocity: "0 0 0",
                particleTextureData: texture,
                particleVertexShaderSource: animatedParticleVertexShader,
                particleFragmentShaderSource: particleFragmentShader
            ),
            context: nil
        )
        defer { destroy(loaded) }

        try render(loaded.executor, time: 0.25, delta: 1.0 / 120.0)
        XCTAssertTrue(hasPixel([255, 0, 0, 255], in: try readPixels(loaded.executor)))

        try render(loaded.executor, time: 0.250_001, delta: 0)
        XCTAssertTrue(hasPixel([0, 0, 255, 255], in: try readPixels(loaded.executor)))

        try render(loaded.executor, time: 1, delta: 0)
        XCTAssertTrue(hasPixel([255, 0, 0, 255], in: try readPixels(loaded.executor)))
        try assertNoExecutionIssues(loaded.executor)
    }

    func testAnimatedSecondaryParticleTextureAlwaysUsesImageZero() throws {
        let red = Array(repeating: [UInt8](arrayLiteral: 255, 0, 0, 255), count: 4)
            .flatMap { $0 }
        let blue = Array(repeating: [UInt8](arrayLiteral: 0, 0, 255, 255), count: 4)
            .flatMap { $0 }
        let animatedSecondary = makeAnimatedParticleTexture(
            images: [red, blue],
            frames: [
                (0, 0.25, 0, 0, 2, 0, 0, 2),
                (1, 0.75, 0, 0, 2, 0, 0, 2),
            ]
        )
        let loaded = try loadPipeline(
            fixture: makeFixture(
                includeText: false,
                particleVelocity: "0 0 0",
                particleSecondaryTextureData: animatedSecondary,
                particleVertexShaderSource: minimalParticleVertexShader,
                particleFragmentShaderSource: secondaryTextureParticleFragmentShader
            ),
            context: nil
        )
        defer { destroy(loaded) }

        try render(loaded.executor, time: 0.5, delta: 1.0 / 120.0)
        XCTAssertTrue(
            hasPixel([255, 0, 0, 255], in: try readPixels(loaded.executor)),
            "Secondary particle texture slots must remain on image zero even when their TEXS timeline selects image one"
        )
        try render(loaded.executor, time: 1.5, delta: 0)
        XCTAssertTrue(
            hasPixel([255, 0, 0, 255], in: try readPixels(loaded.executor)),
            "Secondary particle texture slots must not inherit slot-zero animation state"
        )
        try assertNoExecutionIssues(loaded.executor)
    }

    func testUnavailableParticleUserTextureFallsBackToMaterialTexture() throws {
        let loaded = try loadPipeline(
            fixture: makeFixture(
                includeText: false,
                particleVelocity: "0 0 0",
                particleSecondaryTextureData: makeRGBA8Texture2x2(),
                particleUserTextureName: "texture_that_is_not_available",
                particleVertexShaderSource: minimalParticleVertexShader,
                particleFragmentShaderSource: """
                uniform sampler2D g_Texture1;
                varying vec2 v_TexCoord;
                void main() {
                    vec4 sampled = texture(g_Texture1, v_TexCoord);
                    bool ready = all(greaterThan(sampled, vec4(0.9)));
                    gl_FragColor = ready
                        ? vec4(0.0, 1.0, 0.0, 1.0)
                        : vec4(1.0, 0.0, 0.0, 1.0);
                }
                """
            ),
            context: nil
        )
        defer { destroy(loaded) }

        try render(loaded.executor, time: 1, delta: 1.0 / 120.0)
        XCTAssertFalse(
            greenColumns(try readPixels(loaded.executor), width: 16).isEmpty,
            "An unavailable high-priority particle provider must leave the ordinary material texture available"
        )
        try assertNoExecutionIssues(loaded.executor)
    }

    func testAnimatedParticleTextureClearsUVBasisForFollowingStaticParticle() throws {
        let firstImage = Array(repeating: [UInt8](arrayLiteral: 255, 255, 255, 255), count: 4)
            .flatMap { $0 }
        let secondImage = Array(repeating: [UInt8](arrayLiteral: 0, 255, 255, 255), count: 4)
            .flatMap { $0 }
        let animatedTexture = makeAnimatedParticleTexture(
            images: [firstImage, secondImage],
            frames: [
                (0, 0.25, 0, 0, 2, 0, 0, 2),
                (1, 0.75, 1, 0, 1, 0, 0, 2),
            ]
        )
        let loaded = try loadPipeline(
            fixture: makeFixture(
                includeText: false,
                particleVelocity: "0 0 0",
                particleTextureData: animatedTexture,
                includeStaticAnimationResetParticle: true,
                particleVertexShaderSource: minimalParticleVertexShader,
                particleFragmentShaderSource: animationUniformResetParticleFragmentShader
            ),
            context: nil
        )
        defer { destroy(loaded) }

        try render(loaded.executor, time: 0.5, delta: 1.0 / 120.0)
        let pixels = try readPixels(loaded.executor)
        XCTAssertTrue(
            hasPixel([255, 0, 0, 255], in: pixels),
            "The animated particle must receive a non-zero frame UV basis"
        )
        XCTAssertTrue(
            hasPixel([0, 255, 0, 255], in: pixels),
            "A following static particle must receive cleared frame UV uniforms"
        )
        try assertNoExecutionIssues(loaded.executor)
    }

    func testAnimatedSingleImageParticleAtlasUsesNormalizedParticleLifetime() throws {
        let atlasPixels: [[UInt8]] = [
            [255, 0, 0, 255],
            [0, 255, 0, 255],
            [0, 0, 255, 255],
            [255, 255, 0, 255],
        ]
        let animatedAtlas = makeAnimatedParticleTexture(
            images: [atlasPixels.flatMap { $0 }],
            frames: [
                (0, 0.5, 0, 0, 1, 0, 0, 1),
                (0, 0.5, 1, 0, 1, 0, 0, 1),
                (0, 0.5, 0, 1, 1, 0, 0, 1),
                (0, 0.5, 1, 1, 1, 0, 0, 1),
            ]
        )
        let loaded = try loadPipeline(
            fixture: makeFixture(
                includeText: false,
                animationMode: "sequence",
                sequenceMultiplier: 2,
                particleVelocity: "0 0 0",
                particleColor: "255 255 255",
                particleTextureData: animatedAtlas
            ),
            context: nil
        )
        defer { destroy(loaded) }

        try render(loaded.executor, time: 1, delta: 0.25)
        let pixels = try readPixels(loaded.executor)
        let renderedOffsets = stride(from: 0, to: pixels.count, by: 4).filter {
            pixels[$0] != 0 || pixels[$0 + 1] != 0 || pixels[$0 + 2] != 0
        }
        XCTAssertFalse(renderedOffsets.isEmpty)
        let renderedColors = Set(renderedOffsets.map {
            Array(pixels[$0 ..< $0 + 4])
        })
        XCTAssertTrue(
            renderedOffsets.allSatisfy {
                Array(pixels[$0 ..< $0 + 4]) == [0, 0, 255, 255]
            },
            "Animated single-image atlases must select frames from normalized particle lifetime; got \(renderedColors)"
        )
        try assertNoExecutionIssues(loaded.executor)
    }

    func testRefractParticleSamplesSceneSnapshotWithoutFramebufferFeedback() throws {
        let loaded = try loadPipeline(
            fixture: makeFixture(
                includeText: false,
                particleVelocity: "0 0 0",
                particleMaterialCombos: ["REFRACT": 1],
                particleFragmentShaderSource: refractParticleFragmentShader,
                clearColor: "1 0 0 1"
            ),
            context: nil
        )
        defer { destroy(loaded) }

        try render(loaded.executor, time: 1, delta: 1.0 / 120.0)
        let pixels = try readPixels(loaded.executor)
        XCTAssertTrue(
            hasPixel([0, 255, 0, 255], in: pixels),
            "REFRACT must sample the red scene snapshot and draw green instead of reading the live destination"
        )
        try assertNoExecutionIssues(loaded.executor)
    }



    func testParticleSpritesheetSequenceSelectsAtlasFrameInRenderedPixels() throws {
        let loaded = try loadPipeline(
            fixture: makeFixture(
                includeText: false,
                animationMode: "sequence",
                sequenceMultiplier: 2,
                spritesheet: true,
                particleVelocity: "0 0 0",
                particleColor: "255 255 255"
            ),
            context: nil
        )
        defer { destroy(loaded) }

        try render(loaded.executor, time: 1, delta: 0.25)
        let pixels = try readPixels(loaded.executor)
        let visibleOffsets = stride(from: 0, to: pixels.count, by: 4).filter {
            pixels[$0] != 0 || pixels[$0 + 1] != 0 || pixels[$0 + 2] != 0
        }
        XCTAssertFalse(visibleOffsets.isEmpty)
        let visibleColors = Set(visibleOffsets.map {
            Array(pixels[$0 ..< $0 + 4])
        })
        XCTAssertTrue(
            visibleOffsets.allSatisfy {
                Array(pixels[$0 ..< $0 + 4]) == [0, 0, 255, 255]
            },
            "Sequence animation must use normalized particle lifetime rather than TEX cycle duration; got \(visibleColors)"
        )
    }

    func testPerspectiveParticleRendersWithDefaultCameraNearPlane() throws {
        let loaded = try loadPipeline(
            fixture: makeFixture(includeText: false, perspective: true),
            context: nil
        )
        defer { destroy(loaded) }

        try render(loaded.executor, time: 1, delta: 1.0 / 120.0)
        let pixels = try readPixels(loaded.executor)
        XCTAssertTrue(
            stride(from: 0, to: pixels.count, by: 4).contains { offset in
                pixels[offset] != 0 || pixels[offset + 1] != 0 || pixels[offset + 2] != 0
            })
    }

    func testPerspectiveParticleAcceptsZeroNearAndExpandsPositiveFar() throws {
        let loaded = try loadPipeline(
            fixture: makeFixture(
                includeText: false,
                perspective: true,
                cameraNearPlane: 0,
                cameraFarPlane: 1
            ),
            context: nil
        )
        defer { destroy(loaded) }

        try render(loaded.executor, time: 1, delta: 1.0 / 120.0)
        let pixels = try readPixels(loaded.executor)
        XCTAssertTrue(
            stride(from: 0, to: pixels.count, by: 4).contains { offset in
                pixels[offset + 3] > 0
            }
        )
    }

    func testPerspectiveParticleUsesOrthographicReverseZClippingPlanes() throws {
        let invalidPlanes: [(near: Double, far: Double)] = [
            (-0.01, 1_000),
            (0, -1),
            (0, 0),
        ]

        for planes in invalidPlanes {
            let loaded = try loadPipeline(
                fixture: makeFixture(
                    includeText: false,
                    perspective: true,
                    cameraNearPlane: planes.near,
                    cameraFarPlane: planes.far
                ),
                context: nil
            )
            defer { destroy(loaded) }

            try render(loaded.executor, time: 1, delta: 1.0 / 120.0)
            let pixels = try readPixels(loaded.executor)
            XCTAssertTrue(
                stride(from: 0, to: pixels.count, by: 4)
                    .contains { offset in
                        return pixels[offset + 3] > 0
                    }
            )
        }
    }

    func testParticleParallaxOffsetRemainsInWorldSpaceUnderObjectScale() throws {
        let parallax = try loadPipeline(
            fixture: makeFixture(
                includeText: false,
                particleOrigin: "1 4 0",
                particleScale: "2 1 1",
                parallaxEnabled: true,
                parallaxAmount: 0.6,
                parallaxDelay: 120,
                parallaxDepth: "0 0"
            ),
            context: nil
        )
        // 0.65 * ((1 - 0.5) * 0.6) * 16 = 3.12 world pixels.
        let translatedControl = try loadPipeline(
            fixture: makeFixture(
                includeText: false,
                particleOrigin: "4 4 0",
                particleScale: "2 1 1"
            ),
            context: nil
        )
        defer {
            destroy(translatedControl)
            destroy(parallax)
        }

        try render(
            parallax.executor,
            time: 1,
            delta: 1.0 / 120.0,
            pointerX: 1,
            pointerY: 0.5
        )
        try render(
            translatedControl.executor,
            time: 1,
            delta: 1.0 / 120.0,
            pointerX: 1,
            pointerY: 0.5
        )
        let parallaxPixels = try readPixels(parallax.executor)
        let controlPixels = try readPixels(translatedControl.executor)
        let parallaxColumns = greenColumns(parallaxPixels, width: 16)
        let controlColumns = greenColumns(controlPixels, width: 16)
        XCTAssertFalse(controlColumns.isEmpty)
        XCTAssertEqual(parallaxColumns, controlColumns)
        XCTAssertEqual(
            parallaxPixels,
            controlPixels,
            "A zero authored depth must use the 0.65 particle minimum and translate before object scale"
        )
    }

    func testParticleOriginUsesBottomUpSceneCoordinates() throws {
        let loaded = try loadPipeline(
            fixture: makeFixture(
                includeText: false,
                particleOrigin: "4 2 0",
                particleVertexShaderSource: minimalParticleVertexShader,
                particleFragmentShaderSource: constantGreenParticleFragmentShader
            ),
            context: nil
        )
        defer { destroy(loaded) }

        try render(loaded.executor, time: 1, delta: 1.0 / 120.0)
        try assertNoExecutionIssues(loaded.executor)
        let pixels = try readPixels(loaded.executor)
        let rows = greenRows(pixels, width: 16)
        XCTAssertFalse(rows.isEmpty)
        XCTAssertTrue(
            rows.allSatisfy { $0 >= 4 },
            "A particle authored near the scene bottom must remain in the user-visible bottom half; rows=\(rows)"
        )
    }

    func testParticleVelocityUsesTopDownSceneCoordinates() throws {
        let loaded = try loadPipeline(
            fixture: makeFixture(
                includeText: false,
                particleOrigin: "8 4 0",
                particleVelocity: "0 -120 0",
                particleVertexShaderSource: minimalParticleVertexShader,
                particleFragmentShaderSource: constantGreenParticleFragmentShader
            ),
            context: nil
        )
        defer { destroy(loaded) }

        try render(loaded.executor, time: 1, delta: 1.0 / 60.0)
        try assertNoExecutionIssues(loaded.executor)
        let rows = greenRows(try readPixels(loaded.executor), width: 16)
        XCTAssertFalse(rows.isEmpty)
        XCTAssertTrue(
            rows.allSatisfy { $0 < 4 },
            "Negative authored Y velocity must move a particle upward after presentation; rows=\(rows)"
        )
    }

    private func loadPipeline(
        includeText: Bool,
        context: MTLDevice? = nil
    ) throws -> Pipeline {
        try loadPipeline(fixture: makeFixture(includeText: includeText), context: context)
    }

    private func loadPipeline(
        fixture: Fixture,
        context: MTLDevice?
    ) throws -> Pipeline {
        var error: WESceneRuntimeErrorRef?
        guard let runtime = fixture.assets.path.withCString({ assets in
            fixture.package.path.withCString { package in
                var configuration = WESceneRuntimeConfiguration(
                    assets_directory: assets,
                    scene_package_path: package
                )
                return we_scene_runtime_create(&configuration, &error)
            }
        }) else { throw failure("runtime", error) }
        do {
            guard let model = "project.json".withCString({
                we_scene_runtime_model_create(runtime, $0, &error)
            }) else { throw failure("model", error) }
            do {
                guard let graph = we_scene_model_graph_create(model, &error) else {
                    throw failure("graph", error)
                }
                do {
                    guard let frameGraph = we_scene_graph_frame_graph_create(graph, &error) else {
                        throw failure("frame graph", error)
                    }
                    do {
                        let executor = context.map {
                            we_scene_frame_executor_create_with_metal_device(
                                frameGraph,
                                Unmanaged.passUnretained($0).toOpaque(),
                                &error
                            )
                        } ?? we_scene_frame_executor_create(frameGraph, &error)
                        guard let executor else { throw failure("executor", error) }
                        return Pipeline(
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
    }

    private func destroy(_ loaded: Pipeline) {
        we_scene_frame_executor_destroy(loaded.executor)
        we_scene_frame_graph_destroy(loaded.frameGraph)
        we_scene_graph_destroy(loaded.graph)
        we_scene_model_destroy(loaded.model)
        we_scene_runtime_destroy(loaded.runtime)
        try? FileManager.default.removeItem(at: loaded.fixture.root)
    }

    private func render(
        _ executor: WESceneFrameExecutorRef,
        time: Double,
        delta: Double,
        drawableWidth: UInt32? = nil,
        drawableHeight: UInt32? = nil,
        pointerX: Double = 0.5,
        pointerY: Double = 0.5
    ) throws {
        var inputs = WESceneFrameInputs(
            pointer_x: pointerX,
            pointer_y: pointerY,
            time_seconds: time,
            frame_time_seconds: delta
        )
        var error: WESceneRuntimeErrorRef?
        let rendered: Int32
        if let drawableWidth, let drawableHeight {
            rendered = we_scene_frame_executor_render_for_drawable(
                executor, &inputs, drawableWidth, drawableHeight,
                WE_SCENE_PRESENTATION_ASPECT_FILL, &error
            )
        } else {
            rendered = we_scene_frame_executor_render(executor, &inputs, &error)
        }
        guard rendered == 1 else {
            throw failure("render", error)
        }
        XCTAssertNil(error)
    }

    private func readPixels(_ executor: WESceneFrameExecutorRef) throws -> [UInt8] {
        var pixels = [UInt8](
            repeating: 0,
            count: we_scene_frame_executor_rgba8_byte_count(executor)
        )
        var error: WESceneRuntimeErrorRef?
        let succeeded = pixels.withUnsafeMutableBytes { bytes in
            we_scene_frame_executor_read_rgba8(
                executor,
                bytes.baseAddress?.assumingMemoryBound(to: UInt8.self),
                bytes.count,
                &error
            )
        }
        guard succeeded == 1 else { throw failure("readback", error) }
        return pixels
    }

    private func assertNoExecutionIssues(
        _ executor: WESceneFrameExecutorRef,
        file: StaticString = #filePath,
        line: UInt = #line
    ) throws {
        var count = 0
        var error: WESceneRuntimeErrorRef?
        XCTAssertEqual(
            we_scene_frame_executor_issue_count(executor, &count, &error),
            1,
            errorMessage(error),
            file: file,
            line: line
        )
        defer { we_scene_runtime_error_destroy(error) }
        XCTAssertEqual(count, 0, file: file, line: line)
    }

    private func setText(_ model: WESceneModelRef, _ text: String) throws {
        var error: WESceneRuntimeErrorRef?
        let result = text.withCString { value in
            var property = WEScenePropertyValue(
                type: WE_SCENE_VALUE_STRING,
                boolean_value: 0,
                integer_value: 0,
                number_value: 0,
                string_value: value,
                component_count: 0,
                vector_value: WESceneVector4()
            )
            return "label".withCString {
                we_scene_model_set_property_value(model, $0, &property, &error)
            }
        }
        guard result == 1 else { throw failure("text property", error) }
    }

    private func greenColumns(_ pixels: [UInt8], width: Int) -> Set<Int> {
        Set(stride(from: 0, to: pixels.count, by: 4).compactMap { offset in
            let red = Int(pixels[offset])
            let green = Int(pixels[offset + 1])
            return green > red + 50 && green > 80 ? (offset / 4) % width : nil
        })
    }

    private func greenRows(_ pixels: [UInt8], width: Int) -> Set<Int> {
        Set(stride(from: 0, to: pixels.count, by: 4).compactMap { offset in
            let red = Int(pixels[offset])
            let green = Int(pixels[offset + 1])
            return green > red + 50 && green > 80
                ? (offset / 4) / width : nil
        })
    }

    private func hasPixel(_ expected: [UInt8], in pixels: [UInt8]) -> Bool {
        precondition(expected.count == 4)
        return stride(from: 0, to: pixels.count, by: 4).contains { offset in
            Array(pixels[offset ..< offset + 4]) == expected
        }
    }

    private func makeFixture(
        includeText: Bool,
        scriptedOrigin: Bool = false,
        dynamicClone: Bool = false,
        stochasticParticles: Bool = false,
        projectionAuto: Bool = false,
        animationMode: String = "sequence",
        sequenceMultiplier: Double = 1,
        spritesheet: Bool = false,
        spritesheetDuration: Double = 2,
        particleRendererName: String = "sprite",
        particleRendererParameters: [String: Any] = [:],
        particleShaderName: String = "genericparticle",
        particleMaxCount: Int = 1,
        perspective: Bool = false,
        particleOrigin: String = "4 4 0",
        particleScale: String = "1 1 1",
        parallaxEnabled: Bool = false,
        parallaxAmount: Double = 1,
        parallaxDelay: Double = 0,
        parallaxDepth: String = "0 0",
        particleVelocity: String = "120 0 0",
        particleColor: String = "0 255 0",
        particleInstanceOverride: [String: Any]? = nil,
        particleTextureName: String = "dot",
        particleTextureSize: (width: UInt32, height: UInt32) = (2, 2),
        particleTextureData: Data? = nil,
        particleSecondaryTextureName: String = "secondary",
        particleSecondaryTextureData: Data? = nil,
        particleUserTextureName: String? = nil,
        includeStaticAnimationResetParticle: Bool = false,
        followingParticleRendererName: String? = nil,
        particleMaterialCombos: [String: Int] = [:],
        particleCullMode: String = "nocull",
        particleVertexShaderSource: String? = nil,
        particleFragmentShaderSource: String? = nil,
        particleInstantaneousCount: Int = 1,
        particleEmitters: [[String: Any]]? = nil,
        textValue: String = "I",
        textOrigin: String = "5 0 0",
        textAngles: String? = nil,
        includeDiscardImageBeforeParticle: Bool = false,
        clearColor: String = "0 0 0 0",
        cameraNearPlane: Double? = nil,
        cameraFarPlane: Double? = nil,
        projectionWidth: Double = 16,
        projectionHeight: Double = 8
    ) throws -> Fixture {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        let assets = root.appendingPathComponent("assets", isDirectory: true)
        let shaders = assets.appendingPathComponent("shaders", isDirectory: true)
        let package = root.appendingPathComponent("scene.pkg")
        try FileManager.default.createDirectory(
            at: shaders,
            withIntermediateDirectories: true
        )
        try Data(
            (particleVertexShaderSource ?? particleVertexShader).utf8
        ).write(
            to: shaders.appendingPathComponent("\(particleShaderName).vert")
        )
        try Data(
            (particleFragmentShaderSource ?? particleFragmentShader).utf8
        ).write(
            to: shaders.appendingPathComponent("\(particleShaderName).frag")
        )
        try Data(ropeParticleVertexShader.utf8).write(
            to: shaders.appendingPathComponent("genericropeparticle.vert")
        )
        try Data(ropeParticleFragmentShader.utf8).write(
            to: shaders.appendingPathComponent("genericropeparticle.frag")
        )
        if includeDiscardImageBeforeParticle {
            try Data(discardImageVertexShader.utf8).write(
                to: shaders.appendingPathComponent("depthclampdiscard.vert")
            )
            try Data(discardImageFragmentShader.utf8).write(
                to: shaders.appendingPathComponent("depthclampdiscard.frag")
            )
        }

        let project: [String: Any] = [
            "file": "scene.json",
            "general": [
                "properties": [
                    "label": [
                        "text": "Label",
                        "type": "textinput",
                        "value": textValue,
                    ],
                ],
            ],
            "title": "Particle executor fixture",
            "type": "scene",
            "version": 2,
        ]
        let evaluatedParticleOrigin: Any
        if dynamicClone {
            evaluatedParticleOrigin = [
                "value": particleOrigin,
                "script": """
                export function init(value) {
                    const config = thisScene.getInitialLayerConfig(thisLayer);
                    config.origin.value = new Vec3(12, 4, 0);
                    thisScene.createLayer(config);
                    return value;
                }
                """,
            ]
        } else if scriptedOrigin {
            evaluatedParticleOrigin = [
                "value": particleOrigin,
                "script": """
                let invocation = { count: 0 };
                export function update(value) {
                    invocation.count += 1;
                    return { x: value.x + invocation.count, y: value.y, z: value.z };
                }
                """,
            ]
        } else {
            evaluatedParticleOrigin = particleOrigin
        }
        var objects: [[String: Any]] = [[
            "id": 1,
            "name": "Particle",
            "origin": evaluatedParticleOrigin,
            "parallaxDepth": parallaxDepth,
            "particle": "particles/test.json",
            "scale": particleScale,
            "visible": true,
        ]]
        if let particleInstanceOverride {
            objects[0]["instanceoverride"] = particleInstanceOverride
        }
        if includeStaticAnimationResetParticle {
            objects.append([
                "id": 3,
                "name": "Static animation reset particle",
                "origin": "12 4 0",
                "parallaxDepth": parallaxDepth,
                "particle": "particles/static-test.json",
                "scale": particleScale,
                "visible": true,
            ])
        }
        if includeDiscardImageBeforeParticle {
            objects.insert([
                "id": 99,
                "image": "models/depthclampdiscard.json",
                "name": "Depth clamp priming image",
                "origin": "8 4 0",
                "size": "16 8",
                "visible": true,
            ], at: 0)
        }
        if includeText {
            var textObject: [String: Any] = [
                "alpha": 1,
                "color": "255 0 0 255",
                "font": "systemfont_arial",
                "horizontalalign": "center",
                "id": 2,
                "name": "Later text",
                "origin": textOrigin,
                "padding": "0 0",
                "pointsize": 4,
                "size": "6 8",
                "spacing": "0 0",
                "text": ["user": "label", "value": textValue],
                "verticalalign": "center",
                "visible": true,
            ]
            if let textAngles {
                textObject["angles"] = textAngles
            }
            objects.append(textObject)
        }
        let general: [String: Any] = [
            "cameraparallax": parallaxEnabled,
            "cameraparallaxamount": parallaxAmount,
            "cameraparallaxdelay": parallaxDelay,
            "cameraparallaxmouseinfluence": 1,
            "clearcolor": clearColor,
            "orthogonalprojection": projectionAuto
                ? ["auto": true]
                : ["height": projectionHeight, "width": projectionWidth],
        ]
        var camera: [String: Any] = [
            "center": "0 0 -1", "eye": "0 0 0", "up": "0 1 0",
        ]
        if let cameraNearPlane {
            camera["nearz"] = cameraNearPlane
        }
        if let cameraFarPlane {
            camera["farz"] = cameraFarPlane
        }
        let scene: [String: Any] = [
            "camera": camera,
            "general": general,
            "objects": objects,
            "version": 1,
        ]
        let emitter: [String: Any] = [
            "directions": "1 1 0",
            "distancemax": 0,
            "distancemin": 0,
            "instantaneous": particleInstantaneousCount,
            "name": "boxrandom",
            "origin": "0 0 0",
            "rate": stochasticParticles ? 120 : 0,
        ]
        let initializer: [[String: Any]] = [
            [
                "max": stochasticParticles ? 2 : 10,
                "min": stochasticParticles ? 2 : 10,
                "name": "lifetimerandom",
            ],
            [
                "max": stochasticParticles ? 2.5 : 2,
                "min": stochasticParticles ? 1.0 : 2,
                "name": "sizerandom",
            ],
            [
                "max": particleColor,
                "min": stochasticParticles ? "0 128 0" : particleColor,
                "name": "colorrandom",
            ],
            ["max": 1, "min": 1, "name": "alpharandom"],
            [
                "max": stochasticParticles ? "120 60 0" : particleVelocity,
                "min": stochasticParticles ? "30 -60 0" : particleVelocity,
                "name": "velocityrandom",
            ],
            [
                "max": stochasticParticles ? "0 0 6.283185307179586" : "0 0 0",
                "min": "0 0 0",
                "name": "rotationrandom",
            ],
        ]
        let definition: [String: Any] = [
            "animationmode": animationMode,
            "emitter": particleEmitters ?? [emitter],
            "flags": perspective ? 4 : 0,
            "initializer": initializer,
            "material": "materials/particle.json",
            "maxcount": stochasticParticles ? 16 : particleMaxCount,
            "operator": [[
                "drag": 0,
                "gravity": "0 0 0",
                "name": "movement",
            ]],
            "sequencemultiplier": sequenceMultiplier,
            "starttime": 0,
        ]
        var particleDefinition = definition
        if particleRendererName != "sprite" || !particleRendererParameters.isEmpty {
            var renderer = particleRendererParameters
            renderer["name"] = particleRendererName
            particleDefinition["renderer"] = [renderer]
        }
        var textureNames = [particleTextureName]
        if particleSecondaryTextureData != nil {
            textureNames.append(particleSecondaryTextureName)
        }
        var materialPass: [String: Any] = [
            "blending": "translucent",
            "cullmode": particleCullMode,
            "depthtest": "disabled",
            "depthwrite": "disabled",
            "shader": particleShaderName,
            "textures": textureNames,
        ]
        if let particleUserTextureName {
            materialPass["usertextures"] = [
                NSNull(),
                particleUserTextureName,
            ]
        }
        if !particleMaterialCombos.isEmpty {
            materialPass["combos"] = particleMaterialCombos
        }
        let material: [String: Any] = ["passes": [materialPass]]
        let staticResetMaterial: [String: Any] = [
            "passes": [[
                "blending": "translucent",
                "cullmode": "nocull",
                "depthtest": "disabled",
                "depthwrite": "disabled",
                "shader": particleShaderName,
                "textures": ["static-reset"],
            ]],
        ]
        var staticResetDefinition = particleDefinition
        staticResetDefinition["material"] = "materials/static-reset.json"
        if let followingParticleRendererName {
            staticResetDefinition["renderer"] = [[
                "name": followingParticleRendererName,
            ]]
        }
        var entries: [(String, Data)] = [
            (
                "materials/dot.tex",
                particleTextureData ?? makeRGBA8Texture2x2(
                    atlas: spritesheet,
                    width: particleTextureSize.width,
                    height: particleTextureSize.height
                )
            ),
            ("materials/particle.json", try json(material)),
            ("particles/test.json", try json(particleDefinition)),
            ("project.json", try json(project)),
            ("scene.json", try json(scene)),
        ]
        if includeStaticAnimationResetParticle {
            entries.append(contentsOf: [
                ("materials/static-reset.json", try json(staticResetMaterial)),
                ("materials/static-reset.tex", makeRGBA8Texture2x2()),
                ("particles/static-test.json", try json(staticResetDefinition)),
            ])
        }
        if let particleSecondaryTextureData {
            entries.append(
                (
                    "materials/\(particleSecondaryTextureName).tex",
                    particleSecondaryTextureData
                )
            )
        }
        if spritesheet {
            entries.append(
                (
                    "materials/dot.tex-json",
                    try json([
                        "spritesheetsequences": [
                            [
                                "duration": spritesheetDuration,
                                "frames": 4,
                                "height": 1,
                                "width": 1,
                            ]
                        ]
                    ])
                )
            )
        }
        if includeDiscardImageBeforeParticle {
            entries.append(contentsOf: [
                (
                    "materials/depthclampdiscard.json",
                    try json([
                        "passes": [[
                            "blending": "normal",
                            "cullmode": "nocull",
                            "depthtest": "disabled",
                            "depthwrite": "disabled",
                            "shader": "depthclampdiscard",
                        ]],
                    ])
                ),
                (
                    "models/depthclampdiscard.json",
                    try json([
                        "material": "materials/depthclampdiscard.json",
                    ])
                ),
            ])
        }
        try makePackage(entries).write(to: package)
        return Fixture(root: root, assets: assets, package: package)
    }

    private func makeOfficialFixture(assets: URL) throws -> Fixture {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        let package = root.appendingPathComponent("scene.pkg")
        try FileManager.default.createDirectory(
            at: root,
            withIntermediateDirectories: true
        )
        let project: [String: Any] = [
            "file": "scene.json",
            "title": "Official particle executor fixture",
            "type": "scene",
            "version": 2,
        ]
        let scene: [String: Any] = [
            "camera": ["center": "0 0 -1", "eye": "0 0 0", "up": "0 1 0"],
            "general": [
                "clearcolor": "0 0 0 0",
                "orthogonalprojection": ["height": 1_024, "width": 1_024],
            ],
            "objects": [[
                "id": 1,
                "name": "Official example",
                "origin": "512 512 0",
                "particle": "particles/example.json",
                "visible": true,
            ]],
            "version": 1,
        ]
        try makePackage([
            ("project.json", try json(project)),
            ("scene.json", try json(scene)),
        ]).write(to: package)
        return Fixture(root: root, assets: assets, package: package)
    }

    private var particleVertexShader: String {
        """
        attribute vec3 a_Position;
        attribute vec4 a_TexCoordVec4;
        attribute vec4 a_Color;
        attribute vec4 a_TexCoordVec4C1;
        attribute vec2 a_TexCoordC2;
        uniform mat4 g_ModelViewProjectionMatrix;
        uniform vec3 g_OrientationUp;
        uniform vec3 g_OrientationRight;
        uniform vec4 g_RenderVar1;
        uniform vec4 g_Texture0Resolution;
        varying vec2 v_TexCoord;
        varying vec4 v_Color;
        void main() {
            vec3 rotation = vec3(a_TexCoordC2, a_TexCoordVec4.z);
            vec3 cosine = cos(rotation);
            vec3 sine = sin(rotation);
            vec3 right = vec3(cosine.z, sine.z, 0.0) * g_OrientationRight;
            vec3 up = vec3(-sine.z, cosine.z, 0.0) * g_OrientationUp;
        #if SPRITESHEET
            float ratio = g_RenderVar1.w;
        #else
            float ratio = g_Texture0Resolution.y / g_Texture0Resolution.x;
        #endif
            vec3 position = a_Position
                + a_TexCoordVec4.w * right * (a_TexCoordVec4.x - 0.5)
                - a_TexCoordVec4.w * up * (a_TexCoordVec4.y - 0.5) * ratio;
            gl_Position = g_ModelViewProjectionMatrix * vec4(position, 1.0);
        #if SPRITESHEET
            float frame = floor(fract(a_TexCoordVec4C1.w) * g_RenderVar1.z);
            vec2 atlasOrigin = vec2(
                fract(frame * g_RenderVar1.x),
                floor(frame * g_RenderVar1.x) * g_RenderVar1.y
            );
            v_TexCoord = atlasOrigin + a_TexCoordVec4.xy * g_RenderVar1.xy;
        #else
            v_TexCoord = a_TexCoordVec4.xy;
        #endif
            v_Color = a_Color;
        }
        """
    }

    private var minimalParticleVertexShader: String {
        """
        attribute vec3 a_Position;
        attribute vec4 a_TexCoordVec4;
        uniform mat4 g_ModelViewProjectionMatrix;
        varying vec2 v_TexCoord;
        varying vec4 v_Color;
        void main() {
            vec2 corner = a_TexCoordVec4.xy - vec2(0.5);
            gl_Position = g_ModelViewProjectionMatrix
                * vec4(a_Position.xy + corner * 2.0, a_Position.z, 1.0);
            v_TexCoord = a_TexCoordVec4.xy;
            v_Color = vec4(1.0);
        }
        """
    }

    private var animatedParticleVertexShader: String {
        """
        attribute vec3 a_Position;
        attribute vec4 a_TexCoordVec4;
        uniform mat4 g_ModelViewProjectionMatrix;
        uniform vec2 g_Texture0Translation;
        uniform vec4 g_Texture0Rotation;
        varying vec2 v_TexCoord;
        varying vec4 v_Color;
        void main() {
            vec2 corner = a_TexCoordVec4.xy - vec2(0.5);
            gl_Position = g_ModelViewProjectionMatrix
                * vec4(a_Position.xy + corner * 2.0, a_Position.z, 1.0);
            v_TexCoord = g_Texture0Translation
                + a_TexCoordVec4.x * g_Texture0Rotation.xy
                + a_TexCoordVec4.y * g_Texture0Rotation.zw;
            v_Color = vec4(1.0);
        }
        """
    }

    private var secondaryTextureParticleFragmentShader: String {
        """
        uniform sampler2D g_Texture1;
        varying vec2 v_TexCoord;
        void main() { gl_FragColor = texture(g_Texture1, v_TexCoord); }
        """
    }

    private var animationUniformResetParticleFragmentShader: String {
        """
        uniform vec2 g_Texture0Translation;
        uniform vec4 g_Texture0Rotation;
        void main() {
            bool reset = all(lessThan(abs(g_Texture0Translation), vec2(0.0001)))
                && all(lessThan(
                    abs(g_Texture0Rotation - vec4(1.0, 0.0, 0.0, 1.0)),
                    vec4(0.0001)
                ));
            gl_FragColor = reset
                ? vec4(0.0, 1.0, 0.0, 1.0)
                : vec4(1.0, 0.0, 0.0, 1.0);
        }
        """
    }

    private var particleFragmentShader: String {
        """
        uniform sampler2D g_Texture0;
        varying vec2 v_TexCoord;
        varying vec4 v_Color;
        void main() { gl_FragColor = texture(g_Texture0, v_TexCoord) * v_Color; }
        """
    }

    private var refractParticleFragmentShader: String {
        """
        uniform sampler2D g_Texture3; // {"default":"_rt_FullFrameBuffer"}
        uniform float g_RefractAmount;
        varying vec2 v_TexCoord;
        varying vec4 v_Color;
        void main() {
            vec4 behind = texture(g_Texture3, v_TexCoord);
            bool snapshotIsRed = behind.r > 0.9 && behind.g < 0.1;
            bool amountIsLinuxDefault = abs(g_RefractAmount - 0.05) < 0.001;
            gl_FragColor = snapshotIsRed && amountIsLinuxDefault
                ? vec4(0.0, 1.0, 0.0, 1.0)
                : vec4(0.0, 0.0, 1.0, 1.0);
        }
        """
    }

    private var ropeParticleVertexShader: String {
        """
        attribute vec4 a_PositionVec4;
        attribute vec4 a_TexCoordVec4;
        attribute vec4 a_TexCoordVec4C1;
        attribute vec4 a_TexCoordVec4C2;
        attribute vec4 a_TexCoordVec4C3;
        attribute vec2 a_TexCoordC4;
        attribute vec4 a_Color;
        uniform mat4 g_ModelViewProjectionMatrix;
        varying vec2 v_TexCoord;
        varying vec4 v_Color;
        void main() {
            vec3 position = mix(a_PositionVec4.xyz, a_TexCoordVec4.xyz, a_TexCoordC4.y);
            vec2 direction = a_TexCoordVec4.xy - a_PositionVec4.xy;
            float magnitude = length(direction);
            vec2 normal = magnitude > 0.0001
                ? vec2(-direction.y, direction.x) / magnitude
                : vec2(0.0, 1.0);
            float width = mix(a_PositionVec4.w, a_TexCoordVec4C2.w, a_TexCoordC4.y);
            position.xy += normal * (a_TexCoordC4.x - 0.5) * width;
            gl_Position = g_ModelViewProjectionMatrix * vec4(position, 1.0);
            v_TexCoord = a_TexCoordC4;
            v_Color = mix(a_Color, a_TexCoordVec4C3, a_TexCoordC4.y);
        }
        """
    }

    private var ropeParticleFragmentShader: String {
        """
        uniform sampler2D g_Texture0;
        varying vec2 v_TexCoord;
        varying vec4 v_Color;
        void main() { gl_FragColor = texture(g_Texture0, v_TexCoord) * v_Color; }
        """
    }

    private var constantGreenParticleFragmentShader: String {
        """
        varying vec2 v_TexCoord;
        varying vec4 v_Color;
        void main() { gl_FragColor = vec4(0.0, 1.0, 0.0, 1.0); }
        """
    }

    private var particleAspectFragmentShader: String {
        """
        uniform vec4 g_RenderVar1;
        varying vec2 v_TexCoord;
        varying vec4 v_Color;
        void main() {
            bool ok = abs(g_RenderVar1.w - 0.5) < 0.001;
            gl_FragColor = ok ? vec4(0.0, 1.0, 0.0, 1.0)
                              : vec4(1.0, 0.0, 0.0, 1.0);
        }
        """
    }

    private var particleBuiltinContractFragmentShader: String {
        """
        uniform mat4 g_ModelMatrix;
        uniform mat4 g_ModelMatrixInverse;
        uniform float g_Alpha;
        uniform vec3 g_Color;
        uniform vec4 g_Color4;
        uniform vec2 g_TexelSize;
        uniform vec2 g_PointerPositionLast;
        varying vec2 v_TexCoord;
        varying vec4 v_Color;
        void main() {
            mat4 identity = g_ModelMatrix * g_ModelMatrixInverse;
            bool ok = abs(identity[0][0] - 1.0) < 0.001
                && abs(identity[1][1] - 1.0) < 0.001
                && abs(identity[3][0]) < 0.001
                && abs(g_Alpha - 1.0) < 0.001
                && abs(g_Color.x - 1.0) < 0.001
                && abs(g_Color4.w - 1.0) < 0.001
                && abs(g_TexelSize.x - 0.0625) < 0.001
                && abs(g_TexelSize.y - 0.125) < 0.001
                && abs(g_PointerPositionLast.x) < 0.001;
            gl_FragColor = ok ? vec4(0.0, 1.0, 0.0, 1.0)
                              : vec4(1.0, 0.0, 0.0, 1.0);
        }
        """
    }

    private var particleColorOverrideFragmentShader: String {
        """
        uniform vec3 g_Color;
        varying vec2 v_TexCoord;
        varying vec4 v_Color;
        void main() {
            gl_FragColor = vec4(v_Color.rgb * g_Color, v_Color.a);
        }
        """
    }

    private var invalidParticleTimeFragmentShader: String {
        """
        uniform sampler2D g_Texture0;
        uniform vec2 g_Time;
        varying vec2 v_TexCoord;
        varying vec4 v_Color;
        void main() {
            gl_FragColor = texture(g_Texture0, v_TexCoord) * v_Color
                + vec4(g_Time, 0.0, 0.0);
        }
        """
    }

    private var discardImageVertexShader: String {
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

    private var discardImageFragmentShader: String {
        """
        uniform float g_Time;
        varying vec2 v_TexCoord;
        void main() {
            if (g_Time >= 0.0) {
                discard;
            }
            gl_FragColor = vec4(v_TexCoord, 0.0, 1.0);
        }
        """
    }

    private func makeRGBA8Texture2x2(
        atlas: Bool = false,
        width: UInt32 = 2,
        height: UInt32 = 2
    ) -> Data {
        let pixelCount = Int(width) * Int(height)
        let pixels: [[UInt8]] = atlas
            ? [
                [255, 0, 0, 255],
                [0, 255, 0, 255],
                [0, 0, 255, 255],
                [255, 255, 0, 255],
            ]
            : Array(repeating: [255, 255, 255, 255], count: pixelCount)
        let bytes = pixels.flatMap { $0 }
        var result = Data()
        appendMagic("TEXV0005", to: &result)
        appendMagic("TEXI0001", to: &result)
        appendUInt32(0, to: &result)
        appendUInt32(1, to: &result)
        appendUInt32(width, to: &result)
        appendUInt32(height, to: &result)
        appendUInt32(width, to: &result)
        appendUInt32(height, to: &result)
        appendUInt32(0, to: &result)
        appendMagic("TEXB0003", to: &result)
        appendUInt32(1, to: &result)
        appendUInt32(UInt32.max, to: &result)
        appendUInt32(1, to: &result)
        appendUInt32(width, to: &result)
        appendUInt32(height, to: &result)
        appendUInt32(0, to: &result)
        appendUInt32(UInt32(bytes.count), to: &result)
        appendUInt32(UInt32(bytes.count), to: &result)
        result.append(contentsOf: bytes)
        return result
    }

    private func makeRawParticleTexture2x2(
        format: UInt32,
        bytes: [UInt8]
    ) -> Data {
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

    private typealias AnimatedParticleFrame = (
        image: UInt32,
        duration: Float,
        x: Float,
        y: Float,
        width: Float,
        widthAux: Float,
        heightAux: Float,
        height: Float
    )

    private func makeAnimatedParticleTexture(
        images: [[UInt8]],
        frames: [AnimatedParticleFrame],
        gifWidth: UInt32 = 2,
        gifHeight: UInt32 = 2
    ) -> Data {
        precondition(!images.isEmpty && !frames.isEmpty)
        precondition(images.allSatisfy { $0.count == 16 })
        var result = Data()
        appendMagic("TEXV0005", to: &result)
        appendMagic("TEXI0001", to: &result)
        appendUInt32(0, to: &result)
        appendUInt32(5, to: &result)
        appendUInt32(2, to: &result)
        appendUInt32(2, to: &result)
        appendUInt32(2, to: &result)
        appendUInt32(2, to: &result)
        appendUInt32(0, to: &result)
        appendMagic("TEXB0003", to: &result)
        appendUInt32(UInt32(images.count), to: &result)
        appendUInt32(UInt32.max, to: &result)
        for image in images {
            appendUInt32(1, to: &result)
            appendUInt32(2, to: &result)
            appendUInt32(2, to: &result)
            appendUInt32(0, to: &result)
            appendUInt32(UInt32(image.count), to: &result)
            appendUInt32(UInt32(image.count), to: &result)
            result.append(contentsOf: image)
        }
        appendMagic("TEXS0003", to: &result)
        appendUInt32(UInt32(frames.count), to: &result)
        appendUInt32(gifWidth, to: &result)
        appendUInt32(gifHeight, to: &result)
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

    private func json(_ value: Any) throws -> Data {
        try JSONSerialization.data(withJSONObject: value, options: [.sortedKeys])
    }

    private func appendMagic(_ value: String, to data: inout Data) {
        data.append(contentsOf: Array(value.utf8))
        data.append(0)
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

    private func errorMessage(_ error: WESceneRuntimeErrorRef?) -> String {
        we_scene_runtime_error_message(error).map(String.init(cString:)) ?? "No error"
    }

    private func failure(_ phase: String, _ error: WESceneRuntimeErrorRef?) -> NSError {
        let message = errorMessage(error)
        we_scene_runtime_error_destroy(error)
        return NSError(
            domain: "ParticleExecutorTests",
            code: 1,
            userInfo: [NSLocalizedDescriptionKey: "\(phase): \(message)"]
        )
    }
}
