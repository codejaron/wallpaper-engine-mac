import Foundation
import OpenGL
import OpenGL.GL3
import SceneGLTestSupport
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

        try setText(isolated.model, String(repeating: "W", count: 200))
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
            messageContains: ["exceeds its authored layout size"]
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

    func testParticleVertexArrayAndBuffersAreReleasedWithBorrowedContextExecutor() throws {
        let fixture = try makeFixture(includeText: false)
        let context = try makeContext()
        let original = CGLGetCurrentContext()
        XCTAssertEqual(CGLSetCurrentContext(context), kCGLNoError)
        defer {
            CGLSetCurrentContext(original)
            CGLDestroyContext(context)
            try? FileManager.default.removeItem(at: fixture.root)
        }

        let loaded = try loadPipeline(fixture: fixture, context: context)
        var executorDestroyed = false
        defer {
            if !executorDestroyed { we_scene_frame_executor_destroy(loaded.executor) }
            we_scene_frame_graph_destroy(loaded.frameGraph)
            we_scene_graph_destroy(loaded.graph)
            we_scene_model_destroy(loaded.model)
            we_scene_runtime_destroy(loaded.runtime)
        }

        try render(loaded.executor, time: 1, delta: 1.0 / 120.0)
        var objects = WESceneGLTestParticleObjects(
            vertex_array: 0,
            vertex_buffer: 0,
            element_buffer: 0
        )
        XCTAssertEqual(we_scene_gl_test_current_particle_objects(&objects), 1)
        var exist: Int32 = 0
        XCTAssertEqual(we_scene_gl_test_particle_objects_exist(&objects, &exist), 1)
        XCTAssertEqual(exist, 1)

        we_scene_frame_executor_destroy(loaded.executor)
        executorDestroyed = true
        XCTAssertEqual(CGLGetCurrentContext(), context)
        XCTAssertEqual(we_scene_gl_test_particle_objects_exist(&objects, &exist), 1)
        XCTAssertEqual(exist, 0)
    }

    func testEmptyParticleBatchDoesNotLeakDepthClampInBorrowedContext() throws {
        let fixture = try makeFixture(
            includeText: false,
            particleInstantaneousCount: 0,
            includeDiscardImageBeforeParticle: true
        )
        let context = try makeContext()
        let original = CGLGetCurrentContext()
        XCTAssertEqual(CGLSetCurrentContext(context), kCGLNoError)
        defer {
            CGLSetCurrentContext(original)
            CGLDestroyContext(context)
            try? FileManager.default.removeItem(at: fixture.root)
        }

        let loaded = try loadPipeline(fixture: fixture, context: context)
        defer { destroy(loaded) }

        try render(loaded.executor, time: 1, delta: 1.0 / 120.0)
        XCTAssertEqual(CGLGetCurrentContext(), context)
        XCTAssertEqual(
            glIsEnabled(GLenum(GL_DEPTH_CLAMP)),
            GLboolean(GL_FALSE),
            "An empty particle batch must not leave depth clamp enabled"
        )
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

    func testBorrowedContextRenderResetsHostScissorState() throws {
        let fixture = try makeFixture(includeText: false)
        let context = try makeContext()
        let original = CGLGetCurrentContext()
        XCTAssertEqual(CGLSetCurrentContext(context), kCGLNoError)
        defer {
            CGLSetCurrentContext(original)
            CGLDestroyContext(context)
            try? FileManager.default.removeItem(at: fixture.root)
        }

        let loaded = try loadPipeline(fixture: fixture, context: context)
        defer { destroy(loaded) }

        glEnable(GLenum(GL_SCISSOR_TEST))
        glScissor(0, 0, 0, 0)
        glColorMask(0, 0, 0, 0)

        try render(
            loaded.executor,
            time: 1,
            delta: 1.0 / 120.0,
            drawableWidth: 16,
            drawableHeight: 8
        )
        XCTAssertEqual(
            glIsEnabled(GLenum(GL_DEPTH_CLAMP)),
            GLboolean(GL_FALSE),
            "A non-empty particle draw must restore depth clamp state"
        )
        let pixels = try readPixels(loaded.executor)
        XCTAssertTrue(
            stride(from: 0, to: pixels.count, by: 4).contains { offset in
                pixels[offset] != 0 || pixels[offset + 1] != 0 || pixels[offset + 2] != 0
            },
            "Borrowed-context rendering must not inherit the host scissor rectangle"
        )
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

    func testParticleSpritesheetOnceEncodesLifetimeIntoFirstVertex() throws {
        let fixture = try makeFixture(
            includeText: false,
            animationMode: "once",
            sequenceMultiplier: 2,
            spritesheet: true
        )
        let context = try makeContext()
        let original = CGLGetCurrentContext()
        XCTAssertEqual(CGLSetCurrentContext(context), kCGLNoError)
        defer {
            CGLSetCurrentContext(original)
            CGLDestroyContext(context)
            try? FileManager.default.removeItem(at: fixture.root)
        }

        let loaded = try loadPipeline(fixture: fixture, context: context)
        defer { destroy(loaded) }
        try render(loaded.executor, time: 1, delta: 0.25)

        var objects = WESceneGLTestParticleObjects(
            vertex_array: 0,
            vertex_buffer: 0,
            element_buffer: 0
        )
        XCTAssertEqual(we_scene_gl_test_current_particle_objects(&objects), 1)
        var lifetime: Float = 0
        XCTAssertEqual(
            we_scene_gl_test_particle_first_lifetime(&objects, &lifetime),
            1
        )
        XCTAssertEqual(lifetime, 0.05, accuracy: 0.0001)
    }

    func testParticleSpritesheetSequenceDurationAndRandomFrameAreStable() throws {
        let sequenceFixture = try makeFixture(
            includeText: false,
            animationMode: "sequence",
            sequenceMultiplier: 2,
            spritesheet: true
        )
        let randomFixture = try makeFixture(
            includeText: false,
            animationMode: "randomframe",
            spritesheet: true
        )
        let context = try makeContext()
        let original = CGLGetCurrentContext()
        XCTAssertEqual(CGLSetCurrentContext(context), kCGLNoError)
        defer {
            CGLSetCurrentContext(original)
            CGLDestroyContext(context)
            try? FileManager.default.removeItem(at: sequenceFixture.root)
            try? FileManager.default.removeItem(at: randomFixture.root)
        }

        let sequence = try loadPipeline(fixture: sequenceFixture, context: context)
        defer { destroy(sequence) }
        try render(sequence.executor, time: 1, delta: 0.25)
        var sequenceObjects = WESceneGLTestParticleObjects(
            vertex_array: 0,
            vertex_buffer: 0,
            element_buffer: 0
        )
        XCTAssertEqual(we_scene_gl_test_current_particle_objects(&sequenceObjects), 1)
        var sequenceLifetime: Float = 0
        XCTAssertEqual(
            we_scene_gl_test_particle_first_lifetime(&sequenceObjects, &sequenceLifetime),
            1
        )
        XCTAssertEqual(sequenceLifetime, 0.25, accuracy: 0.0001)

        let random = try loadPipeline(fixture: randomFixture, context: context)
        defer { destroy(random) }
        try render(random.executor, time: 1, delta: 1.0 / 120.0)
        var randomObjects = WESceneGLTestParticleObjects(
            vertex_array: 0,
            vertex_buffer: 0,
            element_buffer: 0
        )
        XCTAssertEqual(we_scene_gl_test_current_particle_objects(&randomObjects), 1)
        var randomLifetime: Float = 0
        XCTAssertEqual(
            we_scene_gl_test_particle_first_lifetime(&randomObjects, &randomLifetime),
            1
        )
        XCTAssertTrue(
            [0.125, 0.375, 0.625, 0.875].contains {
                abs(Double(randomLifetime) - $0) < 0.0001
            }
        )
    }

    func testParticleSpritesheetSequenceSelectsAtlasFrameInRenderedPixels() throws {
        let loaded = try loadPipeline(
            fixture: makeFixture(
                includeText: false,
                animationMode: "sequence",
                sequenceMultiplier: 2,
                spritesheet: true,
                particleVelocity: "0 0 0"
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
        XCTAssertTrue(
            visibleOffsets.allSatisfy {
                Array(pixels[$0 ..< $0 + 4]) == [0, 255, 0, 255]
            },
            "Sequence lifetime 0.25 must select the green second frame from the colored atlas"
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

    func testPerspectiveParticleRejectsInvalidSignedClippingPlanes() throws {
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

            var inputs = WESceneFrameInputs(
                pointer_x: 0.5,
                pointer_y: 0.5,
                time_seconds: 1,
                frame_time_seconds: 1.0 / 120.0
            )
            var error: WESceneRuntimeErrorRef?
            XCTAssertEqual(
                we_scene_frame_executor_render(loaded.executor, &inputs, &error),
                0,
                "Expected near=\(planes.near), far=\(planes.far) to fail"
            )
            XCTAssertEqual(
                we_scene_runtime_error_code(error),
                WE_SCENE_RUNTIME_ERROR_GL_RESOURCE_VALIDATION
            )
            XCTAssertTrue(errorMessage(error).contains("perspective clipping planes"))
            we_scene_runtime_error_destroy(error)
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
        // (0.65 + 0.6) * ((1 - 0.5) * 0.6) * 16 = 6 world pixels.
        let translatedControl = try loadPipeline(
            fixture: makeFixture(
                includeText: false,
                particleOrigin: "7 4 0",
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

    private func loadPipeline(
        includeText: Bool,
        context: CGLContextObj? = nil
    ) throws -> Pipeline {
        try loadPipeline(fixture: makeFixture(includeText: includeText), context: context)
    }

    private func loadPipeline(
        fixture: Fixture,
        context: CGLContextObj?
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
                            we_scene_frame_executor_create_with_cgl_context(
                                frameGraph, UnsafeMutableRawPointer($0), &error
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

    private func makeContext() throws -> CGLContextObj {
        var attributes: [CGLPixelFormatAttribute] = [
            kCGLPFAOpenGLProfile,
            CGLPixelFormatAttribute(rawValue: kCGLOGLPVersion_GL4_Core.rawValue),
            CGLPixelFormatAttribute(rawValue: 0),
        ]
        var pixelFormat: CGLPixelFormatObj?
        var count: GLint = 0
        guard CGLChoosePixelFormat(&attributes, &pixelFormat, &count) == kCGLNoError,
              let pixelFormat else {
            throw NSError(domain: "ParticleExecutorTests", code: 1)
        }
        defer { CGLReleasePixelFormat(pixelFormat) }
        var context: CGLContextObj?
        guard CGLCreateContext(pixelFormat, nil, &context) == kCGLNoError,
              let context else {
            throw NSError(domain: "ParticleExecutorTests", code: 2)
        }
        return context
    }

    private func makeFixture(
        includeText: Bool,
        scriptedOrigin: Bool = false,
        stochasticParticles: Bool = false,
        projectionAuto: Bool = false,
        animationMode: String = "sequence",
        sequenceMultiplier: Double = 1,
        spritesheet: Bool = false,
        perspective: Bool = false,
        particleOrigin: String = "4 4 0",
        particleScale: String = "1 1 1",
        parallaxEnabled: Bool = false,
        parallaxAmount: Double = 1,
        parallaxDelay: Double = 0,
        parallaxDepth: String = "0 0",
        particleVelocity: String = "120 0 0",
        particleInstanceOverride: [String: Any]? = nil,
        particleTextureName: String = "dot",
        particleTextureSize: (width: UInt32, height: UInt32) = (2, 2),
        particleVertexShaderSource: String? = nil,
        particleFragmentShaderSource: String? = nil,
        particleInstantaneousCount: Int = 1,
        includeDiscardImageBeforeParticle: Bool = false,
        cameraNearPlane: Double? = nil,
        cameraFarPlane: Double? = nil
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
            to: shaders.appendingPathComponent("genericparticle.vert")
        )
        try Data(
            (particleFragmentShaderSource ?? particleFragmentShader).utf8
        ).write(
            to: shaders.appendingPathComponent("genericparticle.frag")
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
                        "value": "I",
                    ],
                ],
            ],
            "title": "Particle executor fixture",
            "type": "scene",
            "version": 2,
        ]
        let evaluatedParticleOrigin: Any = scriptedOrigin ? [
            "value": particleOrigin,
            "script": """
            let invocation = { count: 0 };
            export function update(value) {
                invocation.count += 1;
                return { x: value.x + invocation.count, y: value.y, z: value.z };
            }
            """,
        ] : particleOrigin
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
            objects.append([
                "alpha": 1,
                "color": "255 0 0 255",
                "font": "systemfont_arial",
                "horizontalalign": "center",
                "id": 2,
                "name": "Later text",
                "origin": "5 0 0",
                "padding": "0 0",
                "pointsize": 4,
                "size": "6 8",
                "spacing": "0 0",
                "text": ["user": "label", "value": "I"],
                "verticalalign": "center",
                "visible": true,
            ])
        }
        var general: [String: Any] = [
            "cameraparallax": parallaxEnabled,
            "cameraparallaxamount": parallaxAmount,
            "cameraparallaxdelay": parallaxDelay,
            "cameraparallaxmouseinfluence": 1,
            "clearcolor": "0 0 0 0",
            "orthogonalprojection": projectionAuto
                ? ["auto": true]
                : ["height": 8, "width": 16],
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
                "max": "0 255 0",
                "min": stochasticParticles ? "0 128 0" : "0 255 0",
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
            "emitter": [emitter],
            "flags": perspective ? 4 : 0,
            "initializer": initializer,
            "material": "materials/particle.json",
            "maxcount": stochasticParticles ? 16 : 1,
            "operator": [[
                "drag": 0,
                "gravity": "0 0 0",
                "name": "movement",
            ]],
            "sequencemultiplier": sequenceMultiplier,
            "starttime": 0,
        ]
        let material: [String: Any] = [
            "passes": [[
                "blending": "translucent",
                "cullmode": "nocull",
                "depthtest": "disabled",
                "depthwrite": "disabled",
                "shader": "genericparticle",
                "textures": [particleTextureName],
            ]],
        ]
        var entries: [(String, Data)] = [
            (
                "materials/dot.tex",
                makeRGBA8Texture2x2(
                    atlas: spritesheet,
                    width: particleTextureSize.width,
                    height: particleTextureSize.height
                )
            ),
            ("materials/particle.json", try json(material)),
            ("particles/test.json", try json(definition)),
            ("project.json", try json(project)),
            ("scene.json", try json(scene)),
        ]
        if spritesheet {
            entries.append(
                (
                    "materials/dot.tex-json",
                    try json([
                        "spritesheetsequences": [
                            [
                                "duration": 2,
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

    private var particleFragmentShader: String {
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
