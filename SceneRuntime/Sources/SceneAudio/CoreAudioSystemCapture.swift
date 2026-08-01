import CoreAudio
import Foundation

func makeSystemAudioTapDescription(
    excluding processObjectID: AudioObjectID,
    uuid: UUID
) -> CATapDescription {
    let description = CATapDescription(
        stereoGlobalTapButExcludeProcesses: [processObjectID]
    )
    description.name = "Open Wallpaper Engine System Audio"
    description.uuid = uuid
    description.isPrivate = true
    description.muteBehavior = .unmuted
    return description
}

func makeSystemAudioAggregateDescription(
    tapUUID: UUID,
    aggregateUUID: UUID
) -> [String: Any] {
    [
        kAudioAggregateDeviceNameKey: "Open Wallpaper Engine System Audio",
        kAudioAggregateDeviceUIDKey: aggregateUUID.uuidString,
        kAudioAggregateDeviceIsPrivateKey: true,
        kAudioAggregateDeviceTapAutoStartKey: false,
        kAudioAggregateDeviceTapListKey: [[
            kAudioSubTapUIDKey: tapUUID.uuidString,
            kAudioSubTapDriftCompensationKey: true,
        ]],
    ]
}

final class CoreAudioSystemCapture: @unchecked Sendable {
    private static let bufferFrameSize = UInt32(
        WallpaperEngineAudioSpectrumAnalyzer.sampleCount
    )

    private struct Resources {
        let tapID: AudioObjectID
        var aggregateDeviceID = AudioObjectID(kAudioObjectUnknown)
        var ioProcID: AudioDeviceIOProcID?
        var deviceStarted = false
    }

    private let ioQueue = DispatchQueue(
        label: "com.winddog.wallpaper-engine.system-audio-tap",
        qos: .userInteractive
    )
    private let sampleHandler: @Sendable (CapturedAudioSamples) -> Void
    private let lock = NSLock()
    private var resources: Resources?

    init(sampleHandler: @escaping @Sendable (CapturedAudioSamples) -> Void) {
        self.sampleHandler = sampleHandler
    }

    deinit {
        stop()
    }

    func start() throws {
        guard currentResources() == nil else { return }

        let processObjectID = try currentProcessAudioObjectID()
        let tapUUID = UUID()
        let tapDescription = makeSystemAudioTapDescription(
            excluding: processObjectID,
            uuid: tapUUID
        )

        var tapID = AudioObjectID(kAudioObjectUnknown)
        try requireNoError(
            AudioHardwareCreateProcessTap(tapDescription, &tapID),
            operation: "Creating the system-audio process tap"
        )
        guard tapID != kAudioObjectUnknown else {
            throw CoreAudioSystemCaptureError.missingResource("process tap")
        }

        var nextResources = Resources(tapID: tapID)
        do {
            let format = try audioFormat(for: tapID)
            let aggregateDescription = makeSystemAudioAggregateDescription(
                tapUUID: tapUUID,
                aggregateUUID: UUID()
            )

            try requireNoError(
                AudioHardwareCreateAggregateDevice(
                    aggregateDescription as CFDictionary,
                    &nextResources.aggregateDeviceID
                ),
                operation: "Creating the private aggregate audio device"
            )
            guard nextResources.aggregateDeviceID != kAudioObjectUnknown else {
                throw CoreAudioSystemCaptureError.missingResource("aggregate audio device")
            }
            try waitUntilAudioDeviceIsAlive(nextResources.aggregateDeviceID)
            try configureAggregateDevice(
                nextResources.aggregateDeviceID,
                sampleRate: format.mSampleRate,
                bufferFrameSize: Self.bufferFrameSize
            )

            var ioProcID: AudioDeviceIOProcID?
            try requireNoError(
                AudioDeviceCreateIOProcIDWithBlock(
                    &ioProcID,
                    nextResources.aggregateDeviceID,
                    ioQueue
                ) { [weak self] _, inputData, _, _, _ in
                    self?.consume(inputData, format: format)
                },
                operation: "Creating the system-audio IO procedure"
            )
            guard let ioProcID else {
                throw CoreAudioSystemCaptureError.missingResource("audio IO procedure")
            }
            nextResources.ioProcID = ioProcID

            try requireNoError(
                AudioDeviceStart(nextResources.aggregateDeviceID, ioProcID),
                operation: "Starting system-audio capture"
            )
            nextResources.deviceStarted = true
            setResources(nextResources)
        } catch {
            cleanUp(nextResources, logFailures: true)
            throw error
        }
    }

    func stop() {
        guard let current = takeResources() else { return }
        cleanUp(current, logFailures: true)
    }

    private func consume(
        _ inputData: UnsafePointer<AudioBufferList>,
        format: AudioStreamBasicDescription
    ) {
        let list = UnsafeMutableAudioBufferListPointer(
            UnsafeMutablePointer(mutating: inputData)
        )
        guard let frameCount = frameCount(for: list, format: format),
              let samples = decodePCMBufferList(
                  list,
                  frameCount: frameCount,
                  format: format
              ) else { return }
        sampleHandler(samples)
    }

    private func cleanUp(_ resources: Resources, logFailures: Bool) {
        if resources.deviceStarted, let ioProcID = resources.ioProcID {
            logIfNeeded(
                AudioDeviceStop(resources.aggregateDeviceID, ioProcID),
                operation: "Stopping system-audio capture",
                enabled: logFailures
            )
        }
        if let ioProcID = resources.ioProcID {
            logIfNeeded(
                AudioDeviceDestroyIOProcID(resources.aggregateDeviceID, ioProcID),
                operation: "Destroying the system-audio IO procedure",
                enabled: logFailures
            )
        }
        if resources.aggregateDeviceID != kAudioObjectUnknown {
            logIfNeeded(
                AudioHardwareDestroyAggregateDevice(resources.aggregateDeviceID),
                operation: "Destroying the private aggregate audio device",
                enabled: logFailures
            )
        }
        logIfNeeded(
            AudioHardwareDestroyProcessTap(resources.tapID),
            operation: "Destroying the system-audio process tap",
            enabled: logFailures
        )
    }

    private func currentResources() -> Resources? {
        lock.lock()
        defer { lock.unlock() }
        return resources
    }

    private func setResources(_ resources: Resources) {
        lock.lock()
        self.resources = resources
        lock.unlock()
    }

    private func takeResources() -> Resources? {
        lock.lock()
        defer { lock.unlock() }
        let current = resources
        resources = nil
        return current
    }
}

private func currentProcessAudioObjectID() throws -> AudioObjectID {
    var address = AudioObjectPropertyAddress(
        mSelector: kAudioHardwarePropertyTranslatePIDToProcessObject,
        mScope: kAudioObjectPropertyScopeGlobal,
        mElement: kAudioObjectPropertyElementMain
    )
    var pid = getpid()
    var processObjectID = AudioObjectID(kAudioObjectUnknown)
    var size = UInt32(MemoryLayout<AudioObjectID>.size)
    let status = withUnsafePointer(to: &pid) { pidPointer in
        AudioObjectGetPropertyData(
            AudioObjectID(kAudioObjectSystemObject),
            &address,
            UInt32(MemoryLayout<pid_t>.size),
            pidPointer,
            &size,
            &processObjectID
        )
    }
    try requireNoError(
        status,
        operation: "Resolving the current Core Audio process"
    )
    guard processObjectID != kAudioObjectUnknown else {
        throw CoreAudioSystemCaptureError.currentProcessUnavailable
    }
    return processObjectID
}

private func audioFormat(for tapID: AudioObjectID) throws -> AudioStreamBasicDescription {
    var address = AudioObjectPropertyAddress(
        mSelector: kAudioTapPropertyFormat,
        mScope: kAudioObjectPropertyScopeGlobal,
        mElement: kAudioObjectPropertyElementMain
    )
    var format = AudioStreamBasicDescription()
    var size = UInt32(MemoryLayout<AudioStreamBasicDescription>.size)
    try requireNoError(
        AudioObjectGetPropertyData(
            tapID,
            &address,
            0,
            nil,
            &size,
            &format
        ),
        operation: "Reading the system-audio tap format"
    )
    guard format.mFormatID == kAudioFormatLinearPCM,
          format.mSampleRate.isFinite,
          format.mSampleRate > 0,
          format.mBytesPerFrame > 0,
          format.mChannelsPerFrame == 1 || format.mChannelsPerFrame == 2 else {
        throw CoreAudioSystemCaptureError.unsupportedFormat(format.mFormatID)
    }
    return format
}

private func configureAggregateDevice(
    _ deviceID: AudioObjectID,
    sampleRate: Double,
    bufferFrameSize: UInt32
) throws {
    var sampleRateAddress = AudioObjectPropertyAddress(
        mSelector: kAudioDevicePropertyNominalSampleRate,
        mScope: kAudioObjectPropertyScopeGlobal,
        mElement: kAudioObjectPropertyElementMain
    )
    var configuredSampleRate = Float64(sampleRate)
    try requireNoError(
        AudioObjectSetPropertyData(
            deviceID,
            &sampleRateAddress,
            0,
            nil,
            UInt32(MemoryLayout<Float64>.size),
            &configuredSampleRate
        ),
        operation: "Configuring the aggregate audio sample rate"
    )

    var bufferSizeAddress = AudioObjectPropertyAddress(
        mSelector: kAudioDevicePropertyBufferFrameSize,
        mScope: kAudioObjectPropertyScopeGlobal,
        mElement: kAudioObjectPropertyElementMain
    )
    var configuredBufferFrameSize = bufferFrameSize
    try requireNoError(
        AudioObjectSetPropertyData(
            deviceID,
            &bufferSizeAddress,
            0,
            nil,
            UInt32(MemoryLayout<UInt32>.size),
            &configuredBufferFrameSize
        ),
        operation: "Configuring the aggregate audio buffer frame size"
    )
}

private func waitUntilAudioDeviceIsAlive(_ deviceID: AudioObjectID) throws {
    var address = AudioObjectPropertyAddress(
        mSelector: kAudioDevicePropertyDeviceIsAlive,
        mScope: kAudioObjectPropertyScopeGlobal,
        mElement: kAudioObjectPropertyElementMain
    )
    for _ in 0..<20 {
        var isAlive: UInt32 = 0
        var size = UInt32(MemoryLayout<UInt32>.size)
        let status = AudioObjectGetPropertyData(
            deviceID,
            &address,
            0,
            nil,
            &size,
            &isAlive
        )
        if status == noErr, isAlive != 0 { return }
        if status != noErr {
            throw CoreAudioSystemCaptureError.operationFailed(
                operation: "Checking whether the aggregate audio device is ready",
                status: status
            )
        }
        Thread.sleep(forTimeInterval: 0.1)
    }
    throw CoreAudioSystemCaptureError.deviceNotReady
}

private func frameCount(
    for list: UnsafeMutableAudioBufferListPointer,
    format: AudioStreamBasicDescription
) -> Int? {
    let bytesPerFrame = Int(format.mBytesPerFrame)
    guard bytesPerFrame > 0 else { return nil }
    let counts = list.compactMap { buffer -> Int? in
        guard buffer.mData != nil, buffer.mDataByteSize > 0 else { return nil }
        return Int(buffer.mDataByteSize) / bytesPerFrame
    }
    guard let count = counts.min(), count > 0 else { return nil }
    return count
}

private func requireNoError(_ status: OSStatus, operation: String) throws {
    guard status == noErr else {
        throw CoreAudioSystemCaptureError.operationFailed(
            operation: operation,
            status: status
        )
    }
}

private func logIfNeeded(_ status: OSStatus, operation: String, enabled: Bool) {
    guard enabled, status != noErr else { return }
    let error = CoreAudioSystemCaptureError.operationFailed(
        operation: operation,
        status: status
    )
    NSLog("[SceneAudio] %@", error.localizedDescription)
}

private enum CoreAudioSystemCaptureError: LocalizedError {
    case currentProcessUnavailable
    case deviceNotReady
    case missingResource(String)
    case unsupportedFormat(AudioFormatID)
    case operationFailed(operation: String, status: OSStatus)

    var errorDescription: String? {
        switch self {
        case .currentProcessUnavailable:
            return "Core Audio did not expose an object for the current process"
        case .deviceNotReady:
            return "The aggregate audio device did not become ready"
        case .missingResource(let resource):
            return "Core Audio reported success without creating the \(resource)"
        case .unsupportedFormat(let formatID):
            return "The system-audio tap returned unsupported format \(formatID)"
        case .operationFailed(let operation, let status):
            let systemMessage = NSError(
                domain: NSOSStatusErrorDomain,
                code: Int(status)
            ).localizedDescription
            return "\(operation) failed with OSStatus \(status): \(systemMessage)"
        }
    }
}
