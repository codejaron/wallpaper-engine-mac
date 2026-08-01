import SceneParticleTestSupport
import XCTest

final class ParticleSimulationTests: XCTestCase {
    private final class Handle {
        let raw: WESceneParticleTestHandleRef

        init(
            scenario: String,
            objectId: Int32 = 17,
            assetPath: String = "particles/example.json",
            explicitSeed: UInt64? = nil
        ) throws {
            var result: WESceneParticleTestHandleRef?
            var error = [CChar](repeating: 0, count: 512)
            let created = scenario.withCString { scenarioPointer in
                assetPath.withCString { assetPointer in
                    we_scene_particle_test_create(
                        scenarioPointer,
                        objectId,
                        assetPointer,
                        explicitSeed == nil ? 0 : 1,
                        explicitSeed ?? 0,
                        &result,
                        &error,
                        error.count
                    )
                }
            }
            guard created == 1, let result else {
                throw TestError.runtime(String(cString: error))
            }
            raw = result
        }

        init(copying source: Handle) throws {
            var result: WESceneParticleTestHandleRef?
            var error = [CChar](repeating: 0, count: 512)
            let copied = we_scene_particle_test_copy(
                source.raw,
                &result,
                &error,
                error.count
            )
            guard copied == 1, let result else {
                throw TestError.runtime(String(cString: error))
            }
            raw = result
        }

        deinit {
            we_scene_particle_test_destroy(raw)
        }

        func advance(_ elapsed: Double) throws {
            var error = [CChar](repeating: 0, count: 512)
            guard we_scene_particle_test_advance(
                raw,
                elapsed,
                &error,
                error.count
            ) == 1 else {
                throw TestError.runtime(String(cString: error))
            }
        }

        func advance(
            _ elapsed: Double,
            overrides: (
                enabled: Bool,
                alpha: Double,
                size: Double,
                lifetime: Double,
                rate: Double,
                speed: Double,
                count: Double,
                color: (Double, Double, Double),
                colorMultiplier: (Double, Double, Double)
            )
        ) throws {
            var error = [CChar](repeating: 0, count: 512)
            guard we_scene_particle_test_advance_with_overrides(
                raw,
                elapsed,
                overrides.enabled ? 1 : 0,
                overrides.alpha,
                overrides.size,
                overrides.lifetime,
                overrides.rate,
                overrides.speed,
                overrides.count,
                overrides.color.0,
                overrides.color.1,
                overrides.color.2,
                overrides.colorMultiplier.0,
                overrides.colorMultiplier.1,
                overrides.colorMultiplier.2,
                &error,
                error.count
            ) == 1 else {
                throw TestError.runtime(String(cString: error))
            }
        }

        var count: Int {
            Int(we_scene_particle_test_count(raw))
        }

        var seed: UInt64 {
            we_scene_particle_test_seed(raw)
        }

        var flags: UInt32 {
            we_scene_particle_test_flags(raw)
        }

        func particle(_ index: Int) throws -> WESceneParticleTestParticleInfo {
            var result = WESceneParticleTestParticleInfo()
            guard we_scene_particle_test_particle(raw, index, &result) == 1 else {
                throw TestError.runtime("Particle index \(index) is unavailable")
            }
            return result
        }

        func snapshot() -> String {
            let required = we_scene_particle_test_snapshot(raw, nil, 0)
            var buffer = [CChar](repeating: 0, count: required)
            _ = we_scene_particle_test_snapshot(raw, &buffer, buffer.count)
            return String(cString: buffer)
        }
    }

    private enum TestError: Error {
        case runtime(String)
    }

    func testStableAndExplicitSeedsReplayExactly() throws {
        let first = try Handle(scenario: "replay")
        let second = try Handle(scenario: "replay")
        try first.advance(0.5)
        try second.advance(0.5)

        XCTAssertEqual(first.seed, second.seed)
        XCTAssertEqual(first.snapshot(), second.snapshot())

        let explicitA = try Handle(
            scenario: "replay",
            objectId: 1,
            assetPath: "particles/a.json",
            explicitSeed: 0x1234_5678_9abc_def0
        )
        let explicitB = try Handle(
            scenario: "replay",
            objectId: 999,
            assetPath: "particles/b.json",
            explicitSeed: 0x1234_5678_9abc_def0
        )
        try explicitA.advance(0.5)
        try explicitB.advance(0.5)
        XCTAssertEqual(explicitA.snapshot(), explicitB.snapshot())

        let firstParticle = try explicitA.particle(0)
        XCTAssertEqual(firstParticle.spawnId, 1)
        XCTAssertEqual(firstParticle.positionX, 3.16776770441798, accuracy: 1e-15)
        XCTAssertEqual(firstParticle.positionY, -12.352056718879981, accuracy: 1e-15)
        XCTAssertEqual(firstParticle.positionZ, -2.982550352288365, accuracy: 1e-15)
        XCTAssertEqual(firstParticle.velocityX, 2.818748438181349, accuracy: 1e-15)
        XCTAssertEqual(firstParticle.velocityY, -8.106326236523398, accuracy: 1e-15)
        XCTAssertEqual(firstParticle.velocityZ, 1.7628782211687346, accuracy: 1e-15)
        XCTAssertEqual(firstParticle.alpha, 0.8848261952400207, accuracy: 1e-15)
        XCTAssertEqual(firstParticle.size, 2.217197898210485, accuracy: 1e-15)
        XCTAssertEqual(firstParticle.lifetime, 1.28791481256485, accuracy: 1e-15)

        let secondParticle = try explicitA.particle(1)
        XCTAssertEqual(secondParticle.spawnId, 2)
        XCTAssertEqual(secondParticle.positionX, 5.22462888408942, accuracy: 1e-15)
        XCTAssertEqual(secondParticle.positionY, -8.28017954479973, accuracy: 1e-15)
        XCTAssertEqual(secondParticle.positionZ, -3.020695261102273, accuracy: 1e-15)
        XCTAssertEqual(secondParticle.velocityX, 0.4184077345346553, accuracy: 1e-15)
        XCTAssertEqual(secondParticle.velocityY, -7.590310002066071, accuracy: 1e-15)
        XCTAssertEqual(secondParticle.velocityZ, 0.2921674712382467, accuracy: 1e-15)
        XCTAssertEqual(secondParticle.alpha, 0.6061639904975892, accuracy: 1e-15)
        XCTAssertEqual(secondParticle.size, 3.522357903732881, accuracy: 1e-15)
        XCTAssertEqual(secondParticle.lifetime, 1.1893990635871887, accuracy: 1e-15)
    }

    func testBoxEmitterUsesCenteredRangesAndFlipsYBoundary() throws {
        let handle = try Handle(scenario: "box", explicitSeed: 42)
        try handle.advance(1.0 / 120.0)
        XCTAssertEqual(handle.count, 64)

        for index in 0..<handle.count {
            let particle = try handle.particle(index)
            XCTAssertTrue((1.0...4.0).contains(abs(particle.positionX - 10.0)))
            XCTAssertTrue((4.0...10.0).contains(abs(particle.positionY + 20.0)))
            XCTAssertTrue((1.5...3.0).contains(abs(particle.positionZ - 30.0)))
        }
    }

    func testSphereEmitterHonorsAuthoredZDirection() throws {
        let orthographic = try Handle(scenario: "sphere2d", explicitSeed: 73)
        try orthographic.advance(1.0 / 120.0)
        XCTAssertEqual(orthographic.count, 128)
        for index in 0..<orthographic.count {
            let particle = try orthographic.particle(index)
            let x = particle.positionX
            let y = particle.positionY + 4.0
            let radial = hypot(x, y)
            XCTAssertGreaterThanOrEqual(x, 0.0)
            XCTAssertLessThanOrEqual(y, 0.0)
            XCTAssertTrue((2.0...5.0).contains(radial))
            XCTAssertEqual(particle.positionZ, 0.0, accuracy: 1e-12)
        }

        let authoredZ = try Handle(scenario: "sphere3axis", explicitSeed: 73)
        try authoredZ.advance(1.0 / 120.0)
        XCTAssertEqual(authoredZ.count, 128)
        for index in 0..<authoredZ.count {
            let particle = try authoredZ.particle(index)
            let x = particle.positionX
            let y = particle.positionY + 4.0
            let radial = hypot(x, y)
            XCTAssertGreaterThanOrEqual(x, 0.0)
            XCTAssertLessThanOrEqual(y, 0.0)
            XCTAssertTrue((2.0...5.0).contains(radial))
            XCTAssertTrue((-5.0...5.0).contains(particle.positionZ))
        }
    }

    func testMovementFadeSizeAndYFlipsFollowUpstreamOrder() throws {
        let handle = try Handle(scenario: "motionFade", explicitSeed: 5)
        try handle.advance(0.125)
        let particle = try handle.particle(0)

        XCTAssertEqual(particle.positionX, 1.0, accuracy: 1e-12)
        XCTAssertEqual(particle.positionY, -0.75, accuracy: 1e-12)
        XCTAssertEqual(particle.velocityX, 8.0, accuracy: 1e-12)
        XCTAssertEqual(particle.velocityY, -6.5, accuracy: 1e-12)
        XCTAssertEqual(particle.alpha, 0.4, accuracy: 1e-12)
        XCTAssertEqual(particle.size, 5.0, accuracy: 1e-12)
        XCTAssertEqual(particle.age, 0.125, accuracy: 1e-12)
    }

    func testConcreteInstanceOverridesAffectTheSimulationPath() throws {
        let handle = try Handle(scenario: "overrides", explicitSeed: 12)
        try handle.advance(0.1)

        XCTAssertEqual(handle.count, 2)
        let particle = try handle.particle(0)
        XCTAssertEqual(particle.alpha, 0.4, accuracy: 1e-12)
        XCTAssertEqual(particle.size, 10.0, accuracy: 1e-12)
        XCTAssertEqual(particle.lifetime, 6.0, accuracy: 1e-12)
        XCTAssertEqual(particle.velocityX, 3.0, accuracy: 1e-12)
        XCTAssertEqual(particle.velocityY, -6.0, accuracy: 1e-12)
        XCTAssertEqual(particle.colorR, 0.1, accuracy: 1e-12)
        XCTAssertEqual(particle.colorG, 0.8, accuracy: 1e-12)
        XCTAssertEqual(particle.colorB, 0.6, accuracy: 1e-12)

        try handle.advance(0.0, overrides: (
            enabled: true,
            alpha: 1.0,
            size: 1.0,
            lifetime: 1.0,
            rate: 1.0,
            speed: 1.0,
            count: 1.0,
            color: (1.0, 1.0, 1.0),
            colorMultiplier: (1.0, 1.0, 1.0)
        ))
        let updated = try handle.particle(0)
        XCTAssertEqual(updated.alpha, 0.8, accuracy: 1e-12)
        XCTAssertEqual(updated.size, 5.0, accuracy: 1e-12)
        XCTAssertEqual(updated.lifetime, 2.0, accuracy: 1e-12)
        XCTAssertEqual(updated.velocityX, 1.0, accuracy: 1e-12)
        XCTAssertEqual(updated.velocityY, -2.0, accuracy: 1e-12)
        XCTAssertEqual(updated.colorR, 0.2, accuracy: 1e-12)
        XCTAssertEqual(updated.colorG, 0.4, accuracy: 1e-12)
        XCTAssertEqual(updated.colorB, 0.6, accuracy: 1e-12)

        let disabled = try Handle(scenario: "disabled", explicitSeed: 12)
        XCTAssertEqual(disabled.count, 0)
        try disabled.advance(0.1)
        XCTAssertEqual(disabled.count, 4)
        XCTAssertEqual(try disabled.particle(0).alpha, 0.08, accuracy: 1e-12)
        XCTAssertEqual(try disabled.particle(0).lifetime, 0.2, accuracy: 1e-12)
    }

    func testSubstepOverrideUpdatePreservesDerivedOperatorOutput() throws {
        let handle = try Handle(scenario: "motionFade", explicitSeed: 5)
        try handle.advance(0.125)
        XCTAssertEqual(try handle.particle(0).alpha, 0.4, accuracy: 1e-12)

        try handle.advance(0.0, overrides: (
            enabled: true,
            alpha: 1.0,
            size: 1.0,
            lifetime: 1.0,
            rate: 2.0,
            speed: 1.0,
            count: 1.0,
            color: (1.0, 1.0, 1.0),
            colorMultiplier: (1.0, 1.0, 1.0)
        ))
        XCTAssertEqual(try handle.particle(0).alpha, 0.4, accuracy: 1e-12)

        try handle.advance(0.0, overrides: (
            enabled: true,
            alpha: 1.0,
            size: 1.0,
            lifetime: 0.1,
            rate: 2.0,
            speed: 1.0,
            count: 1.0,
            color: (1.0, 1.0, 1.0),
            colorMultiplier: (1.0, 1.0, 1.0)
        ))
        XCTAssertEqual(handle.count, 0)
    }

    func testStartTimePrewarmsExactlyOnceWithTheOverriddenRate() throws {
        let handle = try Handle(scenario: "prewarm", explicitSeed: 7)
        XCTAssertEqual(handle.count, 2)
        XCTAssertEqual(try handle.particle(0).age, 0.75, accuracy: 1e-12)
        XCTAssertEqual(try handle.particle(1).age, 0.25, accuracy: 1e-12)

        let initial = handle.snapshot()
        try handle.advance(0.0)
        XCTAssertEqual(handle.snapshot(), initial)
    }

    func testSubstepTimeIsNeverConsumedAsAFullFixedStep() throws {
        let step = 1.0 / 60.0
        let substep = step * (1.0 - 0.5e-9)
        let boundary = try Handle(scenario: "replay", explicitSeed: 99)
        try boundary.advance(substep)
        XCTAssertEqual(boundary.count, 0)

        let whole = try Handle(scenario: "delayedDuration", explicitSeed: 73)
        let chunks = try Handle(scenario: "delayedDuration", explicitSeed: 73)
        try whole.advance(3.0 * (0.25 * (1.0 - 0.5e-9)))
        for _ in 0..<3 {
            try chunks.advance(0.25 * (1.0 - 0.5e-9))
        }
        XCTAssertEqual(whole.count, chunks.count)
        XCTAssertEqual(whole.count, 0)
    }

    func testSubstepStartTimeDoesNotPrewarmPastItsBoundary() throws {
        let handle = try Handle(scenario: "substepPrewarm", explicitSeed: 7)
        XCTAssertEqual(handle.count, 0)
    }

    func testPerspectiveSphereUsesAThreeDimensionalShell() throws {
        let handle = try Handle(scenario: "perspectiveSphere", explicitSeed: 73)
        try handle.advance(1.0 / 120.0)
        XCTAssertEqual(handle.count, 256)

        var hasDepth = false
        for index in 0..<handle.count {
            let particle = try handle.particle(index)
            let radius = sqrt(
                particle.positionX * particle.positionX +
                    particle.positionY * particle.positionY +
                    particle.positionZ * particle.positionZ
            )
            XCTAssertTrue((2.0...5.0).contains(radius))
            hasDepth = hasDepth || abs(particle.positionZ) > 0.1
        }
        XCTAssertTrue(hasDepth)
    }

    func testEmitterLimitFlagCapsRateEmissionPerStep() throws {
        let handle = try Handle(scenario: "limitedEmitter", explicitSeed: 73)
        try handle.advance(1.0)
        XCTAssertEqual(handle.count, 1)
    }

    func testEmitterDelayAndDurationBoundTheActiveWindow() throws {
        let handle = try Handle(scenario: "delayedDuration", explicitSeed: 73)
        try handle.advance(0.5)
        XCTAssertEqual(handle.count, 0)

        try handle.advance(0.5)
        XCTAssertEqual(handle.count, 2)

        try handle.advance(1.0)
        XCTAssertEqual(handle.count, 2)
    }

    func testPeriodicEmitterRepeatsAndResetsItsPerPeriodLimit() throws {
        let handle = try Handle(scenario: "periodicEmitter", explicitSeed: 73)
        try handle.advance(0.5)
        XCTAssertEqual(handle.count, 3)

        try handle.advance(0.5)
        XCTAssertEqual(handle.count, 3)

        try handle.advance(0.25)
        XCTAssertEqual(handle.count, 5)

        let chunks = try Handle(copying: handle)
        try handle.advance(1.0)
        for _ in 0..<4 {
            try chunks.advance(0.25)
        }
        XCTAssertEqual(chunks.snapshot(), handle.snapshot())
    }

    func testFullCapacityDoesNotPauseEmitterDuration() throws {
        let handle = try Handle(scenario: "capacityClock", explicitSeed: 73)
        try handle.advance(0.3)
        XCTAssertEqual(handle.count, 0)
    }

    func testPeriodicLimitCountsOnlyRateParticlesThatActuallySpawn() throws {
        let handle = try Handle(scenario: "periodicCapacity", explicitSeed: 73)
        try handle.advance(0.3)
        XCTAssertEqual(handle.count, 1)
        XCTAssertEqual(try handle.particle(0).spawnId, 3)
    }

    func testCountOverrideRejectsTheExclusiveSizeLimit() throws {
        let handle = try Handle(scenario: "overrides", explicitSeed: 12)
        XCTAssertThrowsError(try handle.advance(0.0, overrides: (
            enabled: true,
            alpha: 1.0,
            size: 1.0,
            lifetime: 1.0,
            rate: 1.0,
            speed: 1.0,
            count: 4_611_686_018_427_387_904.0,
            color: (1.0, 1.0, 1.0),
            colorMultiplier: (1.0, 1.0, 1.0)
        ))) { error in
            guard case let TestError.runtime(message) = error else {
                return XCTFail("Unexpected error: \(error)")
            }
            XCTAssertTrue(message.contains("size_t capacity"), message)
        }
    }

    func testUninterpretedParticleFlagsRemainCompatibleAndPreserved() throws {
        let handle = try Handle(
            scenario: "compatibleParticleFlags",
            explicitSeed: 73
        )
        XCTAssertEqual(handle.flags, 248)
        try handle.advance(1.0 / 120.0)
        XCTAssertEqual(handle.count, 1)
    }

    func testAngularInitializerAndMovementUpdateRotation() throws {
        let handle = try Handle(scenario: "angular", explicitSeed: 3)
        try handle.advance(0.25)
        let particle = try handle.particle(0)

        XCTAssertEqual(particle.rotationZ, 0.5, accuracy: 1e-12)
        XCTAssertEqual(particle.angularVelocityZ, 3.0, accuracy: 1e-12)
    }

    func testControlPointsDriveEmittersAndAllowRepellingAttractors() throws {
        let emitter = try Handle(scenario: "controlPointEmitter", explicitSeed: 3)
        try emitter.advance(0.1)
        let spawned = try emitter.particle(0)
        XCTAssertEqual(spawned.positionX, 4.0, accuracy: 1e-12)
        XCTAssertEqual(spawned.positionY, -3.0, accuracy: 1e-12)
        XCTAssertEqual(spawned.positionZ, 2.0, accuracy: 1e-12)

        let attract = try Handle(scenario: "attract", explicitSeed: 3)
        try attract.advance(0.25)
        XCTAssertEqual(try attract.particle(0).velocityX, -1.0, accuracy: 1e-12)
    }

    func testUnauthoredControlPointSlotsDefaultToTheParticleOrigin() throws {
        let handle = try Handle(scenario: "implicitControlPoint", explicitSeed: 3)
        try handle.advance(0.25)
        let particle = try handle.particle(0)
        XCTAssertEqual(particle.positionX, 1.0, accuracy: 1e-12)
        XCTAssertEqual(particle.velocityX, -1.0, accuracy: 1e-12)
    }

    func testOscillatorAndRandomFrameStateRemainStableAcrossChunking() throws {
        let whole = try Handle(scenario: "oscillators", explicitSeed: 99)
        try whole.advance(0.2)
        let first = try whole.particle(0)
        let second = try whole.particle(1)
        XCTAssertTrue((0.0..<1.0).contains(first.randomFrameUnit))
        XCTAssertTrue((0.0..<1.0).contains(second.randomFrameUnit))
        XCTAssertNotEqual(first.randomFrameUnit, second.randomFrameUnit)
        XCTAssertNotEqual(first.positionX, second.positionX)
        XCTAssertTrue((10.0...30.0).contains(first.size))
        XCTAssertTrue((10.0...30.0).contains(second.size))
        XCTAssertNotEqual(first.size, second.size)

        let chunks = try Handle(copying: whole)
        try whole.advance(0.4)
        for _ in 0..<8 {
            try chunks.advance(0.05)
        }
        XCTAssertEqual(chunks.snapshot(), whole.snapshot())
    }

    func testTurbulentVelocityIsFiniteAndStaysInTheOrthographicPlane() throws {
        let handle = try Handle(scenario: "turbulent", explicitSeed: 51)
        try handle.advance(1.0 / 120.0)
        XCTAssertEqual(handle.count, 16)
        for index in 0..<handle.count {
            let particle = try handle.particle(index)
            let speed = sqrt(
                particle.velocityX * particle.velocityX +
                    particle.velocityY * particle.velocityY +
                    particle.velocityZ * particle.velocityZ
            )
            XCTAssertEqual(speed, 10.0, accuracy: 1e-9)
            XCTAssertEqual(particle.velocityZ, 0.0, accuracy: 1e-12)
        }
    }

    func testMapSequenceInitializerAssignsDeterministicControlPointSlots() throws {
        let handle = try Handle(scenario: "mapSequence", explicitSeed: 11)
        try handle.advance(0.1)
        XCTAssertEqual(handle.count, 4)
        for index in 0..<handle.count {
            let particle = try handle.particle(index)
            XCTAssertEqual(particle.positionX, 10.0, accuracy: 1e-12)
            XCTAssertEqual(particle.positionY, 20.0, accuracy: 1e-12)
            XCTAssertEqual(particle.positionZ, 0.0, accuracy: 1e-12)
        }
        XCTAssertEqual(try handle.particle(0).velocityX, 1.0, accuracy: 1e-12)
        XCTAssertEqual(try handle.particle(0).velocityY, 0.0, accuracy: 1e-12)
        XCTAssertEqual(try handle.particle(1).velocityX, 0.0, accuracy: 1e-12)
        XCTAssertEqual(try handle.particle(1).velocityY, -1.0, accuracy: 1e-12)
        XCTAssertEqual(try handle.particle(2).velocityX, -1.0, accuracy: 1e-12)
        XCTAssertEqual(try handle.particle(2).velocityY, 0.0, accuracy: 1e-12)
        XCTAssertEqual(try handle.particle(3).velocityX, 0.0, accuracy: 1e-12)
        XCTAssertEqual(try handle.particle(3).velocityY, 1.0, accuracy: 1e-12)
    }

    func testChangeOperatorsUseLifetimeInterpolationForSizeAlphaAndColor() throws {
        let handle = try Handle(scenario: "changes", explicitSeed: 11)
        try handle.advance(0.25)
        var particle = try handle.particle(0)
        XCTAssertEqual(particle.size, 7.5, accuracy: 1e-12)
        XCTAssertEqual(particle.alpha, 0.75, accuracy: 1e-12)
        XCTAssertEqual(particle.colorR, 0.75, accuracy: 1e-12)
        XCTAssertEqual(particle.colorG, 0.25, accuracy: 1e-12)
        XCTAssertEqual(particle.colorB, 0.25, accuracy: 1e-12)

        try handle.advance(0.25)
        particle = try handle.particle(0)
        XCTAssertEqual(particle.size, 10.0, accuracy: 1e-12)
        XCTAssertEqual(particle.alpha, 0.5, accuracy: 1e-12)
        XCTAssertEqual(particle.colorR, 0.5, accuracy: 1e-12)
        XCTAssertEqual(particle.colorG, 0.5, accuracy: 1e-12)
        XCTAssertEqual(particle.colorB, 0.5, accuracy: 1e-12)
    }

    func testTurbulenceOperatorIsDeterministicAndSamplesItsRangeOnce() throws {
        let whole = try Handle(scenario: "turbulenceOperator", explicitSeed: 23)
        let chunks = try Handle(scenario: "turbulenceOperator", explicitSeed: 23)
        try whole.advance(0.3)
        for _ in 0..<3 {
            try chunks.advance(0.1)
        }
        XCTAssertEqual(whole.snapshot(), chunks.snapshot())
        let particle = try whole.particle(0)
        XCTAssertTrue(particle.velocityX.isFinite)
        XCTAssertTrue(particle.velocityY.isFinite)
        XCTAssertNotEqual(abs(particle.velocityX) + abs(particle.velocityY), 0.0)
    }

    func testVortexAudioModeIsAnExplicitNoOpWithoutSpectrum() throws {
        let active = try Handle(scenario: "vortex", explicitSeed: 31)
        let audio = try Handle(scenario: "vortexAudio", explicitSeed: 31)
        try active.advance(0.1)
        try audio.advance(0.1)
        XCTAssertEqual(try active.particle(0).velocityY, 0.2, accuracy: 1e-12)
        XCTAssertEqual(try audio.particle(0).velocityX, 0.0, accuracy: 1e-12)
        XCTAssertEqual(try audio.particle(0).velocityY, 0.0, accuracy: 1e-12)
    }

    func testAuthoredReverseVectorRangesRemainValid() throws {
        let handle = try Handle(scenario: "reverseRanges", explicitSeed: 81)
        try handle.advance(1.0 / 120.0)
        XCTAssertEqual(handle.count, 16)
        for index in 0..<handle.count {
            let particle = try handle.particle(index)
            XCTAssertTrue((-10.0...10.0).contains(particle.velocityX))
            XCTAssertTrue((-8.0...8.0).contains(particle.velocityY))
            XCTAssertTrue((-6.0...6.0).contains(particle.velocityZ))
            XCTAssertTrue((0.2...1.0).contains(particle.colorR))
            XCTAssertTrue((0.3...0.9).contains(particle.colorG))
            XCTAssertTrue((0.4...0.8).contains(particle.colorB))
        }
    }

    func testZeroDeltaFreezesAndChunkingMatchesCopiedState() throws {
        let whole = try Handle(scenario: "replay", explicitSeed: 99)
        try whole.advance(0.2)
        let frozen = whole.snapshot()
        try whole.advance(0.0)
        XCTAssertEqual(whole.snapshot(), frozen)

        let chunks = try Handle(copying: whole)
        try whole.advance(0.3)
        for _ in 0..<6 {
            try chunks.advance(0.05)
        }
        XCTAssertEqual(chunks.snapshot(), whole.snapshot())
    }

    func testReclamationPreservesSpawnOrderAndHonorsMaxCount() throws {
        let handle = try Handle(scenario: "recycle", explicitSeed: 234)
        for _ in 0..<20 {
            try handle.advance(0.05)
            XCTAssertLessThanOrEqual(handle.count, 3)
            let identifiers = try (0..<handle.count).map {
                try handle.particle($0).spawnId
            }
            XCTAssertEqual(identifiers, identifiers.sorted())
        }
    }

    func testUnsafeConfigurationFailsExplicitly() {
        func assertCreationFails(
            _ scenario: String,
            containing expectedMessage: String,
            file: StaticString = #filePath,
            line: UInt = #line
        ) {
            var handle: WESceneParticleTestHandleRef?
            var error = [CChar](repeating: 0, count: 512)
            let created = scenario.withCString { scenarioPointer in
                "particles/invalid.json".withCString { assetPointer in
                    we_scene_particle_test_create(
                        scenarioPointer,
                        1,
                        assetPointer,
                        0,
                        0,
                        &handle,
                        &error,
                        error.count
                    )
                }
            }
            if let handle {
                we_scene_particle_test_destroy(handle)
            }
            XCTAssertEqual(created, 0, file: file, line: line)
            XCTAssertTrue(
                String(cString: error).contains(expectedMessage),
                "Unexpected error: \(String(cString: error))",
                file: file,
                line: line
            )
        }

        assertCreationFails("duplicateControlPoint", containing: "control point id")
        assertCreationFails("excessivePrewarm", containing: "too many fixed steps")
        assertCreationFails("overflowingPrewarm", containing: "step count overflowed")
    }

    func testLinuxPermissiveParticleRangesRemainExecutable() throws {
        for scenario in [
            "invalidFade",
            "negativeDrag",
            "invalidSign",
            "negativeEmitterTiming",
            "negativeThreshold",
        ] {
            let handle = try Handle(scenario: scenario, explicitSeed: 123)
            try handle.advance(0.1)
        }
    }

    func testUnknownEmitterFlagBitsAreIgnoredLikeLinux() throws {
        let handle = try Handle(
            scenario: "unsupportedEmitterFlags",
            explicitSeed: 123
        )
        try handle.advance(0.1)
    }

    func testOutOfRangeControlPointReferencesAreNoOpsLikeLinux() throws {
        for scenario in [
            "outOfRangeEmitterControlPoint",
            "outOfRangeAttractControlPoint",
        ] {
            let handle = try Handle(scenario: scenario, explicitSeed: 123)
            try handle.advance(0.1)
        }
    }

    func testImplicitEmitterControlPointUsesLinkedSlotZeroLikeLinux() throws {
        let handle = try Handle(
            scenario: "linkedControlPointZero",
            explicitSeed: 123
        )
        try handle.advance(0.1)

        let particle = try handle.particle(0)
        XCTAssertEqual(particle.positionX, 4, accuracy: 1e-12)
        XCTAssertEqual(particle.positionY, -3, accuracy: 1e-12)
        XCTAssertEqual(particle.positionZ, 2, accuracy: 1e-12)
    }
}
