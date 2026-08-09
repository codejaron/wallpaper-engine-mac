#ifndef WE_C_SCENE_AUDIO_REALTIME_H
#define WE_C_SCENE_AUDIO_REALTIME_H

#include <CoreAudio/CoreAudioTypes.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WEStereoPCMRingBuffer WEStereoPCMRingBuffer;

/// Allocates the ring storage outside the Core Audio callback.
WEStereoPCMRingBuffer *WEAudioRingBufferCreate(uint32_t capacityFrames);
void WEAudioRingBufferDestroy(WEStereoPCMRingBuffer *buffer);

/// Decodes and appends one mono/stereo linear-PCM buffer without allocating or
/// blocking. Returns false for unsupported formats, malformed input, or when
/// the consumer has fallen far enough behind that the complete batch does not
/// fit. Dropping a complete batch preserves window continuity.
bool WEAudioRingBufferWrite(
    WEStereoPCMRingBuffer *buffer,
    const AudioBufferList *input,
    uint32_t frameCount,
    const AudioStreamBasicDescription *format
);

/// Reads the newest complete window and discards older complete windows. This
/// bounds analysis work even after a temporarily delayed consumer catches up.
bool WEAudioRingBufferReadLatest(
    WEStereoPCMRingBuffer *buffer,
    float *left,
    float *right,
    uint32_t frameCount,
    double *sampleRate
);

uint64_t WEAudioRingBufferDroppedFrames(const WEStereoPCMRingBuffer *buffer);
void WEAudioRingBufferReset(WEStereoPCMRingBuffer *buffer);

/// Shared non-allocating decoder used by tests and by the live ring-buffer
/// writer, so supported PCM semantics have one implementation.
bool WEAudioDecodeStereoPCM(
    const AudioBufferList *input,
    uint32_t frameCount,
    const AudioStreamBasicDescription *format,
    float *left,
    float *right
);

#ifdef __cplusplus
}
#endif

#endif
