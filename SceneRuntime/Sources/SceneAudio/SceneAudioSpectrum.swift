import Accelerate
import AudioToolbox
import CSceneAudioRealtime
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

    private final class ChannelState {
        var spectrum16 = [Float](repeating: 0, count: 16)
        var spectrum32 = [Float](repeating: 0, count: 32)
        var spectrum64 = [Float](repeating: 0, count: 64)
    }

    private let transform: vDSP.DiscreteFourierTransform<Float>
    private let window: [Float]
    private let magnitudeNormalization: Float
    private var windowed: [Float]
    private let imaginaryInput: [Float]
    private var realOutput: [Float]
    private var imaginaryOutput: [Float]
    private var magnitudes: [Float]
    private var bandRanges64: [Range<Int>] = []
    private var bandRangeSampleRate: Double?
    private let leftState = ChannelState()
    private let rightState = ChannelState()
    private let leftDestination = ChannelState()
    private let rightDestination = ChannelState()

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
        windowed = [Float](repeating: 0, count: Self.sampleCount)
        imaginaryInput = [Float](repeating: 0, count: Self.sampleCount)
        realOutput = [Float](repeating: 0, count: Self.sampleCount)
        imaginaryOutput = [Float](repeating: 0, count: Self.sampleCount)
        magnitudes = [Float](repeating: 0, count: Self.sampleCount / 2)
    }

    public func reset() {
        for state in [leftState, rightState, leftDestination, rightDestination] {
            state.spectrum16 = [Float](repeating: 0, count: 16)
            state.spectrum32 = [Float](repeating: 0, count: 32)
            state.spectrum64 = [Float](repeating: 0, count: 64)
        }
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

        prepareBandRanges(sampleRate: sampleRate)
        analyze(samples: left, destination: leftDestination)
        analyze(samples: right, destination: rightDestination)
        let windowDuration = Double(Self.sampleCount) / sampleRate
        smooth(
            state: leftState,
            destination: leftDestination,
            windowDuration: windowDuration
        )
        smooth(
            state: rightState,
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
        destination: ChannelState
    ) {
        vDSP.multiply(samples, window, result: &windowed)
        transform.transform(
            inputReal: windowed,
            inputImaginary: imaginaryInput,
            outputReal: &realOutput,
            outputImaginary: &imaginaryOutput
        )

        let positiveBinCount = Self.sampleCount / 2
        magnitudes[0] = 0
        for index in 1..<positiveBinCount {
            magnitudes[index] = hypot(
                realOutput[index], imaginaryOutput[index]
            ) * magnitudeNormalization
        }
        for band in 0..<64 {
            var peak: Float = 0
            for index in bandRanges64[band] {
                peak = max(peak, magnitudes[index])
            }
            destination.spectrum64[band] = spectrumLevel(for: peak)
        }
        for band in 0..<32 {
            let first = band * 2
            destination.spectrum32[band] = max(
                destination.spectrum64[first],
                destination.spectrum64[first + 1]
            )
        }
        for band in 0..<16 {
            let first = band * 4
            destination.spectrum16[band] = max(
                max(
                    destination.spectrum64[first],
                    destination.spectrum64[first + 1]
                ),
                max(
                    destination.spectrum64[first + 2],
                    destination.spectrum64[first + 3]
                )
            )
        }
    }

    private func prepareBandRanges(
        sampleRate: Double
    ) {
        if bandRangeSampleRate == sampleRate { return }
        let nyquist = sampleRate * 0.5
        let highestFrequency = min(20_000, nyquist)
        let binWidth = sampleRate / Double(Self.sampleCount)
        let lowestFrequency = max(20, binWidth)
        guard highestFrequency > lowestFrequency else {
            bandRanges64 = Array(repeating: 0..<0, count: 64)
            bandRangeSampleRate = sampleRate
            return
        }
        let ratio = highestFrequency / lowestFrequency
        bandRanges64 = (0..<64).map { band in
            let lowerFrequency = lowestFrequency * pow(
                ratio,
                Double(band) / 64
            )
            let upperFrequency = lowestFrequency * pow(
                ratio,
                Double(band + 1) / 64
            )
            let lowerBin = min(
                max(Int(floor(lowerFrequency / binWidth)), 1),
                magnitudes.count - 1
            )
            let upperBin = min(
                max(Int(ceil(upperFrequency / binWidth)), lowerBin + 1),
                magnitudes.count
            )
            return lowerBin..<upperBin
        }
        bandRangeSampleRate = sampleRate
    }

    private func spectrumLevel(for peak: Float) -> Float {
        guard peak > 0, peak.isFinite else { return 0 }
        // A -80 dB noise floor maps to zero and full scale maps to one. Do not
        // upper-clamp: Wallpaper Engine permits levels above one.
        return max(0, (20 * log10(peak) + 80) / 80)
    }

    private func smooth(
        state: ChannelState,
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

/// Single-producer/single-consumer PCM storage. Core Audio only calls `write`;
/// the analysis queue is the sole reader. All storage is allocated at init.
final class RealtimeStereoPCMBuffer: @unchecked Sendable {
    private let storage: OpaquePointer

    init?(capacityFrames: Int) {
        guard capacityFrames > WallpaperEngineAudioSpectrumAnalyzer.sampleCount,
              let storage = WEAudioRingBufferCreate(UInt32(capacityFrames)) else {
            return nil
        }
        self.storage = storage
    }

    deinit {
        WEAudioRingBufferDestroy(storage)
    }

    @discardableResult
    func write(
        _ input: UnsafePointer<AudioBufferList>,
        frameCount: Int,
        format: AudioStreamBasicDescription
    ) -> Bool {
        guard frameCount > 0, frameCount <= Int(UInt32.max) else { return false }
        var format = format
        return WEAudioRingBufferWrite(
            storage,
            input,
            UInt32(frameCount),
            &format
        )
    }

    func readLatest(
        left: inout [Float],
        right: inout [Float]
    ) -> Double? {
        precondition(!left.isEmpty && left.count == right.count)
        var sampleRate = 0.0
        let didRead = left.withUnsafeMutableBufferPointer { leftBuffer in
            right.withUnsafeMutableBufferPointer { rightBuffer in
                WEAudioRingBufferReadLatest(
                    storage,
                    leftBuffer.baseAddress,
                    rightBuffer.baseAddress,
                    UInt32(leftBuffer.count),
                    &sampleRate
                )
            }
        }
        return didRead ? sampleRate : nil
    }

    var droppedFrameCount: UInt64 {
        WEAudioRingBufferDroppedFrames(storage)
    }

    func reset() {
        WEAudioRingBufferReset(storage)
    }
}

/// Bounded-rate spectrum worker. It consumes the newest complete PCM window,
/// so a delayed worker never performs a burst of stale FFTs to catch up.
private final class RealtimeAudioSpectrumPipeline: @unchecked Sendable {
    typealias Publisher = @Sendable (
        SceneAudioSpectrumFrame,
        Float,
        Double
    ) -> Void

    let id: UUID
    let ringBuffer: RealtimeStereoPCMBuffer
    private let publisher: Publisher
    private let workerQueue = DispatchQueue(
        label: "com.winddog.wallpaper-engine.audio-spectrum",
        qos: .userInitiated
    )
    private let analyzer = WallpaperEngineAudioSpectrumAnalyzer()
    private var left = [Float](
        repeating: 0,
        count: WallpaperEngineAudioSpectrumAnalyzer.sampleCount
    )
    private var right = [Float](
        repeating: 0,
        count: WallpaperEngineAudioSpectrumAnalyzer.sampleCount
    )
    private var timer: DispatchSourceTimer?
    private var lastReportedDroppedFrames: UInt64 = 0

    init?(id: UUID, publisher: @escaping Publisher) {
        guard let ringBuffer = RealtimeStereoPCMBuffer(capacityFrames: 16_384) else {
            return nil
        }
        self.id = id
        self.ringBuffer = ringBuffer
        self.publisher = publisher
    }

    func start() {
        precondition(timer == nil)
        let nextTimer = DispatchSource.makeTimerSource(queue: workerQueue)
        nextTimer.schedule(
            deadline: .now(),
            repeating: .nanoseconds(16_666_667),
            leeway: .milliseconds(2)
        )
        nextTimer.setEventHandler { [weak self] in
            self?.processLatestWindow()
        }
        timer = nextTimer
        nextTimer.resume()
    }

    func stop() {
        timer?.setEventHandler {}
        timer?.cancel()
        timer = nil
    }

    /// Called only after Core Audio has stopped producing into this pipeline.
    func finishAfterCaptureStops() {
        workerQueue.sync {
            ringBuffer.reset()
            analyzer.reset()
        }
    }

    private func processLatestWindow() {
        guard let sampleRate = ringBuffer.readLatest(
            left: &left,
            right: &right
        ) else { return }

        var sumOfSquares: Float = 0
        for index in left.indices {
            sumOfSquares += left[index] * left[index]
            sumOfSquares += right[index] * right[index]
        }
        let meanSquare = sumOfSquares / Float(left.count * 2)
        let frame = analyzer.push(
            left: left,
            right: right,
            sampleRate: sampleRate
        )
        publisher(frame, meanSquare.squareRoot(), sampleRate)

        let droppedFrames = ringBuffer.droppedFrameCount
        if droppedFrames != lastReportedDroppedFrames {
            lastReportedDroppedFrames = droppedFrames
            NSLog(
                "[SceneAudio] Dropped %llu PCM frames because spectrum analysis fell behind",
                droppedFrames
            )
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
    private var spectrumPipeline: RealtimeAudioSpectrumPipeline?
    private var activePipelineID: UUID?
    private var statusStorage: SceneAudioCaptureStatus = .idle
    private var latestFrameStorage = SceneAudioSpectrumFrame.zero
    private var lastSignalUptime: TimeInterval?
    private var didReportPCMInput = false
    private var didReportActiveSpectrum = false
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
        let nextPipelineID = UUID()
        guard let nextPipeline = RealtimeAudioSpectrumPipeline(
            id: nextPipelineID,
            publisher: { [weak self] frame, rms, sampleRate in
                self?.publish(
                    frame,
                    rms: rms,
                    sampleRate: sampleRate,
                    pipelineID: nextPipelineID
                )
            }
        ) else {
            let message = "Allocating the real-time PCM ring buffer failed"
            withLock { statusStorage = .unavailable(message) }
            throw SceneAudioCaptureError.startFailed(message)
        }
        do {
            let nextCapture = try await performSystemAudioCaptureStartup {
                let capture = CoreAudioSystemCapture(
                    ringBuffer: nextPipeline.ringBuffer
                )
                try capture.start()
                return capture
            }

            guard startGeneration == generation,
                  !Task.isCancelled,
                  capturePolicy.shouldRun else {
                await Task.detached(priority: .utility) {
                    nextCapture.stop()
                    nextPipeline.finishAfterCaptureStops()
                }.value
                throw CancellationError()
            }

            withLock {
                capture = nextCapture
                spectrumPipeline = nextPipeline
                activePipelineID = nextPipeline.id
                statusStorage = .running
            }
            nextPipeline.start()
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
        let current = withLock {
            () -> (CoreAudioSystemCapture?, RealtimeAudioSpectrumPipeline?) in
            let currentCapture = capture
            let currentPipeline = spectrumPipeline
            capture = nil
            spectrumPipeline = nil
            activePipelineID = nil
            statusStorage = .idle
            latestFrameStorage = .zero
            lastSignalUptime = nil
            didReportPCMInput = false
            didReportActiveSpectrum = false
            return (currentCapture, currentPipeline)
        }
        current.1?.stop()
        if let currentCapture = current.0 {
            let currentPipeline = current.1
            Task.detached(priority: .utility) {
                currentCapture.stop()
                currentPipeline?.finishAfterCaptureStops()
            }
        } else {
            current.1?.finishAfterCaptureStops()
        }
    }

    private func publish(
        _ frame: SceneAudioSpectrumFrame,
        rms: Float,
        sampleRate: Double,
        pipelineID: UUID
    ) {
        var firstPCMInput = false
        var firstActiveSpectrumPeak: Float?
        withLock {
            guard activePipelineID == pipelineID else { return }
            if !didReportPCMInput {
                didReportPCMInput = true
                firstPCMInput = true
            }
            if rms >= 0.0025 {
                lastSignalUptime = ProcessInfo.processInfo.systemUptime
            }
            latestFrameStorage = frame
            if !didReportActiveSpectrum {
                let peak = maximumSpectrumLevel(frame)
                if peak > 0.001 {
                    didReportActiveSpectrum = true
                    firstActiveSpectrumPeak = peak
                }
            }
        }
        if firstPCMInput {
            NSLog(
                "[SceneAudio] PCM input active: %.0f Hz, %d-frame analysis window",
                sampleRate,
                Int32(WallpaperEngineAudioSpectrumAnalyzer.sampleCount)
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
    guard frameCount > 0,
          frameCount <= Int(UInt32.max) else { return nil }
    let audioBufferList = list.unsafeMutablePointer
    var left = [Float](repeating: 0, count: frameCount)
    var right = [Float](repeating: 0, count: frameCount)
    var format = asbd
    let decoded = left.withUnsafeMutableBufferPointer { leftBuffer in
        right.withUnsafeMutableBufferPointer { rightBuffer in
            WEAudioDecodeStereoPCM(
                audioBufferList,
                UInt32(frameCount),
                &format,
                leftBuffer.baseAddress,
                rightBuffer.baseAddress
            )
        }
    }
    guard decoded else { return nil }
    return CapturedAudioSamples(
        sampleRate: asbd.mSampleRate,
        left: left,
        right: right
    )
}
