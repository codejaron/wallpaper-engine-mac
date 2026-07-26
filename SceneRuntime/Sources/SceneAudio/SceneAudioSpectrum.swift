import Accelerate
import AudioToolbox
import Foundation

/// The six fixed-size spectrum arrays exposed by Wallpaper Engine shaders.
/// Values are deliberately kept as signed floating point values. The Linux
/// recorder does not clamp the logarithmic result at zero, so negative values
/// are part of the wire contract.
public struct SceneAudioSpectrumFrame: Equatable, Sendable {
    public static let zero = SceneAudioSpectrumFrame(
        spectrum16Left: Array(repeating: 0, count: 16),
        spectrum16Right: Array(repeating: 0, count: 16),
        spectrum32Left: Array(repeating: 0, count: 32),
        spectrum32Right: Array(repeating: 0, count: 32),
        spectrum64Left: Array(repeating: 0, count: 64),
        spectrum64Right: Array(repeating: 0, count: 64)
    )

    public let spectrum16Left: [Float]
    public let spectrum16Right: [Float]
    public let spectrum32Left: [Float]
    public let spectrum32Right: [Float]
    public let spectrum64Left: [Float]
    public let spectrum64Right: [Float]

    public init(
        spectrum16Left: [Float],
        spectrum16Right: [Float],
        spectrum32Left: [Float],
        spectrum32Right: [Float],
        spectrum64Left: [Float],
        spectrum64Right: [Float]
    ) {
        precondition(spectrum16Left.count == 16)
        precondition(spectrum16Right.count == 16)
        precondition(spectrum32Left.count == 32)
        precondition(spectrum32Right.count == 32)
        precondition(spectrum64Left.count == 64)
        precondition(spectrum64Right.count == 64)
        self.spectrum16Left = spectrum16Left
        self.spectrum16Right = spectrum16Right
        self.spectrum32Left = spectrum32Left
        self.spectrum32Right = spectrum32Right
        self.spectrum64Left = spectrum64Left
        self.spectrum64Right = spectrum64Right
    }
}

/// Linux-compatible FFT reduction and attack/release smoothing.
///
/// `push(samples:)` intentionally returns the values *before* the newly
/// received FFT destination is applied. This is the order used by the pinned
/// linux-wallpaperengine recorder: move the current arrays first, then compute
/// the destination for the next update.
public final class LinuxAudioSpectrumAnalyzer: @unchecked Sendable {
    public static let sampleCount = 1024

    private let transform: vDSP.DiscreteFourierTransform<Float>
    private var current16 = [Float](repeating: 0, count: 16)
    private var current32 = [Float](repeating: 0, count: 32)
    private var current64 = [Float](repeating: 0, count: 64)
    private var destination16 = [Float](repeating: 0, count: 16)
    private var destination32 = [Float](repeating: 0, count: 32)
    private var destination64 = [Float](repeating: 0, count: 64)

    public init() {
        do {
            transform = try vDSP.DiscreteFourierTransform(
                count: Self.sampleCount,
                direction: .forward,
                transformType: .complexComplex,
                ofType: Float.self
            )
        } catch {
            // The count is a power of two and therefore cannot fail on a
            // supported Accelerate implementation. Keep the failure explicit
            // if a future SDK changes that invariant.
            preconditionFailure("Unable to create 1024-point audio FFT: \(error)")
        }
    }

    /// Feed one complete mono window. Incomplete windows are rejected rather
    /// than padded with fabricated samples.
    @discardableResult
    public func push(samples: [Float]) -> SceneAudioSpectrumFrame {
        precondition(samples.count == Self.sampleCount)

        moveTowards(&current16, destination16)
        moveTowards(&current32, destination32)
        moveTowards(&current64, destination64)

        let imaginary = [Float](repeating: 0, count: Self.sampleCount)
        var realOutput = [Float](repeating: 0, count: Self.sampleCount)
        var imaginaryOutput = [Float](repeating: 0, count: Self.sampleCount)
        transform.transform(
            inputReal: samples,
            inputImaginary: imaginary,
            outputReal: &realOutput,
            outputImaginary: &imaginaryOutput
        )

        for band in 0..<64 {
            let index = band * 2
            let real = realOutput[index]
            let imag = imaginaryOutput[index]
            let magnitudeSquared = real * real + imag * imag
            let logarithmicMagnitude: Float = magnitudeSquared > 0
                ? 0.35 * log10(magnitudeSquared)
                : 0

            destination64[band] = min(
                1,
                logarithmicMagnitude *
                    (2 - exp(1 - Float(band) / 63 - 0.5))
            )
            // The upstream implementation deliberately overwrites these
            // buckets as it walks the 64-band result (band >> n); this is not
            // an averaging operation.
            destination32[band >> 1] = min(
                1,
                logarithmicMagnitude *
                    (2 - exp(1 - Float(band) / 31 - 0.5))
            )
            destination16[band >> 2] = min(
                1,
                logarithmicMagnitude *
                    (2 - exp(1 - Float(band) / 15 - 0.5))
            )
        }

        return SceneAudioSpectrumFrame(
            spectrum16Left: current16,
            spectrum16Right: current16,
            spectrum32Left: current32,
            spectrum32Right: current32,
            spectrum64Left: current64,
            spectrum64Right: current64
        )
    }

    private func moveTowards(_ current: inout [Float], _ destination: [Float]) {
        for index in current.indices {
            let delta = destination[index] - current[index]
            if abs(delta) <= 0.3 {
                current[index] = destination[index]
            } else {
                current[index] += delta > 0 ? 0.3 : -0.3
            }
        }
    }
}

public enum SceneAudioCaptureStatus: Equatable, Sendable {
    case idle
    case starting
    case running
    case unavailable(String)
}

public struct SceneAudioCapturePolicy: Equatable, Sendable {
    public var isCaptureAllowed: Bool
    public var isSuspended: Bool
    public var demandCount: Int

    public init(
        isCaptureAllowed: Bool,
        isSuspended: Bool,
        demandCount: Int
    ) {
        self.isCaptureAllowed = isCaptureAllowed
        self.isSuspended = isSuspended
        self.demandCount = demandCount
    }

    public var shouldRun: Bool {
        isCaptureAllowed && !isSuspended && demandCount > 0
    }
}

/// One explicit claim on the process-wide system-audio stream. Scene rendering
/// and playback-policy detection share the same capture, so neither consumer
/// may stop it while the other one is still active.
@MainActor
public final class SceneAudioCaptureLease {
    private weak var provider: SceneSystemAudioSpectrumProvider?
    private let id: UUID
    private var isActive = true

    fileprivate init(provider: SceneSystemAudioSpectrumProvider, id: UUID) {
        self.provider = provider
        self.id = id
    }

    public func cancel() {
        guard isActive else { return }
        isActive = false
        provider?.releaseLease(id)
    }

    deinit {
        guard isActive else { return }
        let provider = provider
        let id = id
        Task { @MainActor in
            provider?.releaseLease(id)
        }
    }
}

/// One process-wide Core Audio tap. Multiple scene windows consume
/// the same latest spectrum, avoiding one permission prompt and one capture
/// stream per monitor.
public final class SceneSystemAudioSpectrumProvider: @unchecked Sendable {
    public static let shared = SceneSystemAudioSpectrumProvider()

    private let lock = NSLock()
    private var capture: CoreAudioSystemCapture?
    private var statusStorage: SceneAudioCaptureStatus = .idle
    private var pendingSamples: [Float] = []
    private var latestFrameStorage = SceneAudioSpectrumFrame.zero
    private var lastSignalUptime: TimeInterval?
    private let analyzer = LinuxAudioSpectrumAnalyzer()
    @MainActor private var activeLeaseIDs: Set<UUID> = []
    @MainActor private var captureAllowed = false
    @MainActor private var captureSuspended = false
    @MainActor private var startTask: Task<Void, Error>?
    @MainActor private var startGeneration: UUID?

    private init() {}

    public var status: SceneAudioCaptureStatus {
        withLock { statusStorage }
    }

    public var latestFrame: SceneAudioSpectrumFrame {
        withLock { latestFrameStorage }
    }

    /// The Core Audio tap excludes this process, so this signal represents
    /// audio emitted by another application. A short hold avoids policy
    /// flapping between sparse audio packets.
    public var isOtherApplicationPlayingAudio: Bool {
        let now = ProcessInfo.processInfo.systemUptime
        return withLock {
            guard let lastSignalUptime else { return false }
            return now - lastSignalUptime <= 0.75
        }
    }

    @MainActor
    public var capturePolicy: SceneAudioCapturePolicy {
        SceneAudioCapturePolicy(
            isCaptureAllowed: captureAllowed,
            isSuspended: captureSuspended,
            demandCount: activeLeaseIDs.count
        )
    }

    @MainActor
    public func setCaptureAllowed(_ allowed: Bool) {
        guard captureAllowed != allowed else { return }
        captureAllowed = allowed
        reconcileCapturePolicy()
    }

    @MainActor
    public func setCaptureSuspended(_ suspended: Bool) {
        guard captureSuspended != suspended else { return }
        captureSuspended = suspended
        reconcileCapturePolicy()
    }

    /// Acquires shared capture ownership. Concurrent callers await the same
    /// in-flight Core Audio startup instead of observing `.starting` as
    /// if capture were already usable.
    @MainActor
    public func acquire() async throws -> SceneAudioCaptureLease {
        guard captureAllowed else {
            throw SceneAudioCaptureError.disabled
        }
        let id = UUID()
        activeLeaseIDs.insert(id)
        let lease = SceneAudioCaptureLease(provider: self, id: id)
        guard !captureSuspended else { return lease }
        do {
            try await ensureRunning()
        } catch {
            if captureAllowed,
               captureSuspended,
               activeLeaseIDs.contains(id) {
                return lease
            }
            activeLeaseIDs.remove(id)
            stopIfUnused()
            if !captureAllowed {
                throw SceneAudioCaptureError.disabled
            }
            throw error
        }
        guard captureAllowed, activeLeaseIDs.contains(id) else {
            throw SceneAudioCaptureError.disabled
        }
        return lease
    }

    @MainActor
    fileprivate func releaseLease(_ id: UUID) {
        guard activeLeaseIDs.remove(id) != nil else { return }
        stopIfUnused()
    }

    @MainActor
    private func ensureRunning() async throws {
        guard captureAllowed else {
            throw SceneAudioCaptureError.disabled
        }
        guard !captureSuspended, !activeLeaseIDs.isEmpty else { return }
        if withLock({ capture != nil && statusStorage == .running }) {
            return
        }
        if let startTask {
            try await startTask.value
            return
        }

        let generation = UUID()
        startGeneration = generation
        withLock { statusStorage = .starting }
        let task = Task { @MainActor in
            try await self.startCapture(generation: generation)
        }
        startTask = task
        do {
            try await task.value
            if startGeneration == generation {
                startTask = nil
                startGeneration = nil
            }
        } catch {
            if startGeneration == generation {
                startTask = nil
                startGeneration = nil
            }
            throw error
        }
    }

    @MainActor
    private func startCapture(generation: UUID) async throws {

        do {
            let nextCapture = CoreAudioSystemCapture { [weak self] samples in
                self?.append(samples: samples)
            }
            try nextCapture.start()

            guard startGeneration == generation,
                  !Task.isCancelled,
                  capturePolicy.shouldRun else {
                nextCapture.stop()
                throw CancellationError()
            }

            withLock {
                capture = nextCapture
                statusStorage = .running
            }
        } catch is CancellationError {
            withLock {
                if capture == nil { statusStorage = .idle }
            }
            throw CancellationError()
        } catch {
            guard startGeneration == generation, !Task.isCancelled else {
                withLock {
                    if capture == nil { statusStorage = .idle }
                }
                throw CancellationError()
            }
            withLock { statusStorage = .unavailable(error.localizedDescription) }
            throw SceneAudioCaptureError.startFailed(error.localizedDescription)
        }
    }

    @MainActor
    private func stopIfUnused() {
        guard activeLeaseIDs.isEmpty else { return }
        stopCapture()
    }

    @MainActor
    private func reconcileCapturePolicy() {
        guard capturePolicy.shouldRun else {
            stopCapture()
            return
        }
        guard startTask == nil,
              !withLock({ capture != nil && statusStorage == .running }) else {
            return
        }
        Task { @MainActor [weak self] in
            do {
                try await self?.ensureRunning()
            } catch {
                // The provider status carries the failure. Consumers surface it
                // without an automatic permission or startup retry loop.
            }
        }
    }

    @MainActor
    private func stopCapture() {
        startTask?.cancel()
        startTask = nil
        startGeneration = nil
        let currentCapture = withLock { () -> CoreAudioSystemCapture? in
            let currentCapture = capture
            capture = nil
            statusStorage = .idle
            pendingSamples.removeAll(keepingCapacity: true)
            latestFrameStorage = .zero
            lastSignalUptime = nil
            return currentCapture
        }
        currentCapture?.stop()
    }

    private func append(samples: [Float]) {
        guard !samples.isEmpty else { return }
        withLock {
            let meanSquare = samples.reduce(Float.zero) { partial, sample in
                partial + sample * sample
            } / Float(samples.count)
            if meanSquare.squareRoot() >= 0.0025 {
                lastSignalUptime = ProcessInfo.processInfo.systemUptime
            }
            pendingSamples.append(contentsOf: samples)
            while pendingSamples.count >= LinuxAudioSpectrumAnalyzer.sampleCount {
                let window = Array(pendingSamples.prefix(LinuxAudioSpectrumAnalyzer.sampleCount))
                pendingSamples.removeFirst(LinuxAudioSpectrumAnalyzer.sampleCount)
                latestFrameStorage = analyzer.push(samples: window)
            }
        }
    }

    private func withLock<T>(_ body: () -> T) -> T {
        lock.lock()
        defer { lock.unlock() }
        return body()
    }
}

public enum SceneAudioCaptureError: LocalizedError, Equatable {
    case disabled
    case startFailed(String)

    public var errorDescription: String? {
        switch self {
        case .disabled: return "System audio capture is disabled in Settings"
        case .startFailed(let message): return "Starting system-audio capture failed: \(message)"
        }
    }
}

/// Decodes and downmixes every channel in an AudioBufferList so planar and
/// interleaved layouts share one validated implementation.
func decodePCMBufferList(
    _ list: UnsafeMutableAudioBufferListPointer,
    frameCount: Int,
    format asbd: AudioStreamBasicDescription
) -> [Float]? {
    guard frameCount > 0 else { return nil }
    let isFloat = (asbd.mFormatFlags & kAudioFormatFlagIsFloat) != 0
    let isSignedInteger = (asbd.mFormatFlags & kAudioFormatFlagIsSignedInteger) != 0
    let bytesPerSample = max(Int((asbd.mBitsPerChannel + 7) / 8), 1)
    let nonInterleaved = (asbd.mFormatFlags & kAudioFormatFlagIsNonInterleaved) != 0 ||
        list.count > 1
    var decoded = [Float](repeating: 0, count: frameCount)
    var decodedChannelCount = 0

    for buffer in list {
        guard let data = buffer.mData else { continue }
        let bufferChannels = max(Int(buffer.mNumberChannels), 1)
        let channelsInBuffer = nonInterleaved ? 1 : bufferChannels
        let bytesPerFrame = nonInterleaved
            ? bytesPerSample
            : max(Int(asbd.mBytesPerFrame), bytesPerSample * bufferChannels)
        let byteCount = Int(buffer.mDataByteSize)
        guard bytesPerFrame > 0, byteCount > 0 else { continue }
        let availableFrames = min(frameCount, byteCount / bytesPerFrame)
        guard availableFrames == frameCount else { return nil }

        for frame in 0..<availableFrames {
            let frameBase = data.advanced(by: frame * bytesPerFrame)
            for channel in 0..<channelsInBuffer {
                let offset = nonInterleaved ? 0 : channel * bytesPerSample
                guard offset + bytesPerSample <= bytesPerFrame else { continue }
                guard let sample = decodeAudioSample(
                    frameBase.advanced(by: offset),
                    bytesPerSample: bytesPerSample,
                    bitsPerChannel: Int(asbd.mBitsPerChannel),
                    isFloat: isFloat,
                    isSignedInteger: isSignedInteger,
                    isBigEndian: (asbd.mFormatFlags & kAudioFormatFlagIsBigEndian) != 0
                ) else { return nil }
                decoded[frame] += sample
            }
        }
        decodedChannelCount += channelsInBuffer
    }

    guard decodedChannelCount > 0 else { return nil }
    let divisor = Float(decodedChannelCount)
    for index in decoded.indices {
        decoded[index] = max(-1, min(1, decoded[index] / divisor))
    }
    return decoded
}

private func decodeAudioSample(
    _ pointer: UnsafeMutableRawPointer,
    bytesPerSample: Int,
    bitsPerChannel: Int,
    isFloat: Bool,
    isSignedInteger: Bool,
    isBigEndian: Bool
) -> Float? {
    guard bytesPerSample > 0 else { return nil }

    if isFloat {
        switch bytesPerSample {
        case 4:
            let bits = pointer.loadUnaligned(as: UInt32.self)
            let ordered = isBigEndian ? bits.byteSwapped : bits
            let value = Float(bitPattern: ordered)
            return value.isFinite ? value : nil
        case 8:
            let bits = pointer.loadUnaligned(as: UInt64.self)
            let ordered = isBigEndian ? bits.byteSwapped : bits
            let value = Float(Double(bitPattern: ordered))
            return value.isFinite ? value : nil
        default:
            return nil
        }
    }

    guard isSignedInteger || (asbdUnsignedIntegerFormat(bytesPerSample: bytesPerSample)) else {
        return nil
    }

    if isSignedInteger {
        switch bytesPerSample {
        case 1:
            let raw = pointer.loadUnaligned(as: UInt8.self)
            return Float(Int8(bitPattern: raw)) / 127
        case 2:
            let raw = pointer.loadUnaligned(as: UInt16.self)
            let ordered = isBigEndian ? raw.byteSwapped : raw
            return Float(Int16(bitPattern: ordered)) / Float(Int16.max)
        case 3:
            let bytes = pointer.assumingMemoryBound(to: UInt8.self)
            var value = isBigEndian
                ? (UInt32(bytes[0]) << 16) | (UInt32(bytes[1]) << 8) | UInt32(bytes[2])
                : UInt32(bytes[0]) | (UInt32(bytes[1]) << 8) | (UInt32(bytes[2]) << 16)
            if value & 0x80_0000 != 0 { value |= 0xFF_00_0000 }
            return Float(Int32(bitPattern: value)) / 8_388_607
        case 4:
            let raw = pointer.loadUnaligned(as: UInt32.self)
            let ordered = isBigEndian ? raw.byteSwapped : raw
            return Float(Int32(bitPattern: ordered)) / Float(Int32.max)
        case 8:
            let raw = pointer.loadUnaligned(as: UInt64.self)
            let ordered = isBigEndian ? raw.byteSwapped : raw
            return Float(Double(Int64(bitPattern: ordered)) / Double(Int64.max))
        default:
            return nil
        }
    }

    // The only unsigned PCM format accepted by the supported capture path is
    // 8-bit unsigned PCM. Keep other formats explicit rather than treating
    // arbitrary bytes as valid audio.
    guard bytesPerSample == 1 else { return nil }
    return (Float(pointer.loadUnaligned(as: UInt8.self)) - 128) / 128
}

private func asbdUnsignedIntegerFormat(bytesPerSample: Int) -> Bool {
    // Kept as a named predicate to make the supported unsigned branch above
    // explicit and easy to extend when Core Audio adds another tap format.
    bytesPerSample == 1
}
