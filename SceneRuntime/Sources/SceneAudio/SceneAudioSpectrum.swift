import Accelerate
import AudioToolbox
import Foundation

/// The six fixed-size spectrum arrays exposed by Wallpaper Engine shaders.
/// Wallpaper Engine defines every array as a positive low-to-high frequency
/// spectrum with independent left and right channels. Values usually sit in
/// 0...1 but are intentionally not upper-clamped.
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

private func maximumSpectrumLevel(
    _ frame: SceneAudioSpectrumFrame
) -> Float {
    var peak: Float = 0
    for values in [
        frame.spectrum16Left,
        frame.spectrum16Right,
        frame.spectrum32Left,
        frame.spectrum32Right,
        frame.spectrum64Left,
        frame.spectrum64Right,
    ] {
        peak = max(peak, values.max() ?? 0)
    }
    return peak
}

/// Produces Wallpaper Engine's documented stereo 16/32/64-band contract from
/// PCM windows. Every resolution divides the complete audible spectrum rather
/// than truncating the FFT or overwriting coarser buckets.
public final class WallpaperEngineAudioSpectrumAnalyzer: @unchecked Sendable {
    public static let sampleCount = 1024

    private struct ChannelState {
        var spectrum16 = [Float](repeating: 0, count: 16)
        var spectrum32 = [Float](repeating: 0, count: 32)
        var spectrum64 = [Float](repeating: 0, count: 64)
    }

    private let transform: vDSP.DiscreteFourierTransform<Float>
    private let window: [Float]
    private let magnitudeNormalization: Float
    private var leftState = ChannelState()
    private var rightState = ChannelState()

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
        window = (0..<Self.sampleCount).map { index in
            let phase = 2 * Float.pi * Float(index) /
                Float(Self.sampleCount - 1)
            return 0.5 - 0.5 * cos(phase)
        }
        magnitudeNormalization = 2 / window.reduce(0, +)
    }

    public func reset() {
        leftState = ChannelState()
        rightState = ChannelState()
    }

    /// Feed one complete stereo window. A genuinely mono source is duplicated
    /// by the decoder before this boundary; incomplete or mismatched windows
    /// are programming errors and are never padded with fabricated samples.
    @discardableResult
    public func push(
        left: [Float],
        right: [Float],
        sampleRate: Double
    ) -> SceneAudioSpectrumFrame {
        precondition(left.count == Self.sampleCount)
        precondition(right.count == Self.sampleCount)
        precondition(sampleRate.isFinite && sampleRate > 0)

        let leftDestination = analyze(samples: left, sampleRate: sampleRate)
        let rightDestination = analyze(samples: right, sampleRate: sampleRate)
        let windowDuration = Double(Self.sampleCount) / sampleRate
        smooth(
            state: &leftState,
            destination: leftDestination,
            windowDuration: windowDuration
        )
        smooth(
            state: &rightState,
            destination: rightDestination,
            windowDuration: windowDuration
        )

        return SceneAudioSpectrumFrame(
            spectrum16Left: leftState.spectrum16,
            spectrum16Right: rightState.spectrum16,
            spectrum32Left: leftState.spectrum32,
            spectrum32Right: rightState.spectrum32,
            spectrum64Left: leftState.spectrum64,
            spectrum64Right: rightState.spectrum64
        )
    }

    private func analyze(
        samples: [Float],
        sampleRate: Double
    ) -> ChannelState {
        var windowed = [Float](repeating: 0, count: Self.sampleCount)
        vDSP.multiply(samples, window, result: &windowed)
        let imaginary = [Float](repeating: 0, count: Self.sampleCount)
        var realOutput = [Float](repeating: 0, count: Self.sampleCount)
        var imaginaryOutput = [Float](repeating: 0, count: Self.sampleCount)
        transform.transform(
            inputReal: windowed,
            inputImaginary: imaginary,
            outputReal: &realOutput,
            outputImaginary: &imaginaryOutput
        )

        let positiveBinCount = Self.sampleCount / 2
        var magnitudes = [Float](repeating: 0, count: positiveBinCount)
        for index in 1..<positiveBinCount {
            magnitudes[index] = hypot(
                realOutput[index], imaginaryOutput[index]
            ) * magnitudeNormalization
        }
        return ChannelState(
            spectrum16: reduceBands(
                magnitudes: magnitudes,
                count: 16,
                sampleRate: sampleRate
            ),
            spectrum32: reduceBands(
                magnitudes: magnitudes,
                count: 32,
                sampleRate: sampleRate
            ),
            spectrum64: reduceBands(
                magnitudes: magnitudes,
                count: 64,
                sampleRate: sampleRate
            )
        )
    }

    private func reduceBands(
        magnitudes: [Float],
        count: Int,
        sampleRate: Double
    ) -> [Float] {
        let nyquist = sampleRate * 0.5
        let highestFrequency = min(20_000, nyquist)
        let binWidth = sampleRate / Double(Self.sampleCount)
        let lowestFrequency = max(20, binWidth)
        guard highestFrequency > lowestFrequency else {
            return [Float](repeating: 0, count: count)
        }
        let ratio = highestFrequency / lowestFrequency
        return (0..<count).map { band in
            let lowerFrequency = lowestFrequency * pow(
                ratio,
                Double(band) / Double(count)
            )
            let upperFrequency = lowestFrequency * pow(
                ratio,
                Double(band + 1) / Double(count)
            )
            let lowerBin = min(
                max(Int(floor(lowerFrequency / binWidth)), 1),
                magnitudes.count - 1
            )
            let upperBin = min(
                max(Int(ceil(upperFrequency / binWidth)), lowerBin + 1),
                magnitudes.count
            )
            let peak = magnitudes[lowerBin..<upperBin].max() ?? 0
            guard peak > 0, peak.isFinite else { return 0 }
            // A -80 dB noise floor maps to zero and full scale maps to one.
            // Do not upper-clamp: Wallpaper Engine explicitly permits levels
            // greater than one for loud mixed sources.
            return max(0, (20 * log10(peak) + 80) / 80)
        }
    }

    private func smooth(
        state: inout ChannelState,
        destination: ChannelState,
        windowDuration: Double
    ) {
        smooth(
            &state.spectrum16,
            toward: destination.spectrum16,
            windowDuration: windowDuration
        )
        smooth(
            &state.spectrum32,
            toward: destination.spectrum32,
            windowDuration: windowDuration
        )
        smooth(
            &state.spectrum64,
            toward: destination.spectrum64,
            windowDuration: windowDuration
        )
    }

    private func smooth(
        _ current: inout [Float],
        toward destination: [Float],
        windowDuration: Double
    ) {
        let attack = Float(1 - exp(-windowDuration / 0.03))
        let release = Float(1 - exp(-windowDuration / 0.18))
        for index in current.indices {
            let coefficient = destination[index] >= current[index]
                ? attack
                : release
            current[index] += (destination[index] - current[index]) * coefficient
        }
    }
}

struct CapturedAudioSamples: Equatable, Sendable {
    let sampleRate: Double
    let left: [Float]
    let right: [Float]

    init(sampleRate: Double, left: [Float], right: [Float]) {
        precondition(sampleRate.isFinite && sampleRate > 0)
        precondition(!left.isEmpty && left.count == right.count)
        precondition(left.allSatisfy(\.isFinite))
        precondition(right.allSatisfy(\.isFinite))
        self.sampleRate = sampleRate
        self.left = left
        self.right = right
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

func performSystemAudioCaptureStartup<T: Sendable>(
    _ operation: @escaping @Sendable () throws -> T
) async throws -> T {
    try await Task.detached(
        priority: .userInitiated,
        operation: operation
    ).value
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
    private var pendingLeftSamples: [Float] = []
    private var pendingRightSamples: [Float] = []
    private var pendingSampleRate: Double?
    private var latestFrameStorage = SceneAudioSpectrumFrame.zero
    private var lastSignalUptime: TimeInterval?
    private var didReportPCMInput = false
    private var didReportActiveSpectrum = false
    private let analyzer = WallpaperEngineAudioSpectrumAnalyzer()
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
            withLock {
                if statusStorage == .idle { statusStorage = .starting }
            }
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
            let nextCapture = try await performSystemAudioCaptureStartup { [weak self] in
                let capture = CoreAudioSystemCapture { [weak self] samples in
                    self?.append(samples)
                }
                try capture.start()
                return capture
            }

            guard startGeneration == generation,
                  !Task.isCancelled,
                  capturePolicy.shouldRun else {
                await Task.detached(priority: .utility) {
                    nextCapture.stop()
                }.value
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
            guard startGeneration == generation,
                  !Task.isCancelled,
                  capturePolicy.shouldRun else {
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
        // Core Audio startup is not cancellable. Keep the in-flight task as
        // the sole startup operation; it will observe the latest policy before
        // publishing its capture and clean up off the main thread otherwise.
        if startTask == nil { startGeneration = nil }
        let currentCapture = withLock { () -> CoreAudioSystemCapture? in
            let currentCapture = capture
            capture = nil
            statusStorage = .idle
            pendingLeftSamples.removeAll(keepingCapacity: true)
            pendingRightSamples.removeAll(keepingCapacity: true)
            pendingSampleRate = nil
            analyzer.reset()
            latestFrameStorage = .zero
            lastSignalUptime = nil
            didReportPCMInput = false
            didReportActiveSpectrum = false
            return currentCapture
        }
        if let currentCapture {
            Task.detached(priority: .utility) {
                currentCapture.stop()
            }
        }
    }

    private func append(_ samples: CapturedAudioSamples) {
        var firstPCMInput: (sampleRate: Double, frameCount: Int)?
        var firstActiveSpectrumPeak: Float?
        withLock {
            if !didReportPCMInput {
                didReportPCMInput = true
                firstPCMInput = (samples.sampleRate, samples.left.count)
            }
            if let pendingSampleRate, pendingSampleRate != samples.sampleRate {
                pendingLeftSamples.removeAll(keepingCapacity: true)
                pendingRightSamples.removeAll(keepingCapacity: true)
                analyzer.reset()
            }
            pendingSampleRate = samples.sampleRate
            let sumOfSquares = zip(samples.left, samples.right).reduce(
                Float.zero
            ) { partial, pair in
                partial + pair.0 * pair.0 + pair.1 * pair.1
            }
            let meanSquare = sumOfSquares / Float(samples.left.count * 2)
            if meanSquare.squareRoot() >= 0.0025 {
                lastSignalUptime = ProcessInfo.processInfo.systemUptime
            }
            pendingLeftSamples.append(contentsOf: samples.left)
            pendingRightSamples.append(contentsOf: samples.right)
            while pendingLeftSamples.count >=
                    WallpaperEngineAudioSpectrumAnalyzer.sampleCount {
                let count = WallpaperEngineAudioSpectrumAnalyzer.sampleCount
                let left = Array(pendingLeftSamples.prefix(count))
                let right = Array(pendingRightSamples.prefix(count))
                pendingLeftSamples.removeFirst(count)
                pendingRightSamples.removeFirst(count)
                latestFrameStorage = analyzer.push(
                    left: left,
                    right: right,
                    sampleRate: samples.sampleRate
                )
                if !didReportActiveSpectrum {
                    let peak = maximumSpectrumLevel(latestFrameStorage)
                    if peak > 0.001 {
                        didReportActiveSpectrum = true
                        firstActiveSpectrumPeak = peak
                    }
                }
            }
        }
        if let firstPCMInput {
            NSLog(
                "[SceneAudio] PCM input active: %.0f Hz, %d frames in first batch",
                firstPCMInput.sampleRate,
                Int32(firstPCMInput.frameCount)
            )
        }
        if let firstActiveSpectrumPeak {
            NSLog(
                "[SceneAudio] Wallpaper Engine spectrum active: peak %.3f",
                Double(firstActiveSpectrumPeak)
            )
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

/// Decodes a Core Audio mono/stereo PCM buffer without destroying channel
/// separation. Mono is duplicated because Wallpaper Engine always exposes two
/// channel arrays; any unsupported channel layout fails explicitly.
func decodePCMBufferList(
    _ list: UnsafeMutableAudioBufferListPointer,
    frameCount: Int,
    format asbd: AudioStreamBasicDescription
) -> CapturedAudioSamples? {
    let expectedChannels = Int(asbd.mChannelsPerFrame)
    guard frameCount > 0,
          asbd.mSampleRate.isFinite,
          asbd.mSampleRate > 0,
          expectedChannels == 1 || expectedChannels == 2 else { return nil }
    let isFloat = (asbd.mFormatFlags & kAudioFormatFlagIsFloat) != 0
    let isSignedInteger = (asbd.mFormatFlags & kAudioFormatFlagIsSignedInteger) != 0
    let bytesPerSample = max(Int((asbd.mBitsPerChannel + 7) / 8), 1)
    let nonInterleaved = (asbd.mFormatFlags & kAudioFormatFlagIsNonInterleaved) != 0 ||
        list.count > 1
    var decodedChannels: [[Float]] = []
    decodedChannels.reserveCapacity(expectedChannels)

    for buffer in list {
        guard let data = buffer.mData else { continue }
        let bufferChannels = max(Int(buffer.mNumberChannels), 1)
        guard !nonInterleaved || bufferChannels == 1 else { return nil }
        let channelsInBuffer = nonInterleaved ? 1 : bufferChannels
        let bytesPerFrame = nonInterleaved
            ? max(Int(asbd.mBytesPerFrame), bytesPerSample)
            : max(Int(asbd.mBytesPerFrame), bytesPerSample * bufferChannels)
        let byteCount = Int(buffer.mDataByteSize)
        guard bytesPerFrame > 0, byteCount > 0 else { continue }
        let availableFrames = min(frameCount, byteCount / bytesPerFrame)
        guard availableFrames == frameCount else { return nil }

        for channel in 0..<channelsInBuffer {
            var decoded = [Float](repeating: 0, count: frameCount)
            for frame in 0..<availableFrames {
                let frameBase = data.advanced(by: frame * bytesPerFrame)
                let offset = nonInterleaved ? 0 : channel * bytesPerSample
                guard offset + bytesPerSample <= bytesPerFrame else { return nil }
                guard let sample = decodeAudioSample(
                    frameBase.advanced(by: offset),
                    bytesPerSample: bytesPerSample,
                    bitsPerChannel: Int(asbd.mBitsPerChannel),
                    isFloat: isFloat,
                    isSignedInteger: isSignedInteger,
                    isBigEndian: (asbd.mFormatFlags & kAudioFormatFlagIsBigEndian) != 0
                ) else { return nil }
                decoded[frame] = sample
            }
            decodedChannels.append(decoded)
        }
    }

    guard decodedChannels.count == expectedChannels else { return nil }
    let left = decodedChannels[0]
    let right = expectedChannels == 2 ? decodedChannels[1] : left
    return CapturedAudioSamples(
        sampleRate: asbd.mSampleRate,
        left: left,
        right: right
    )
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
