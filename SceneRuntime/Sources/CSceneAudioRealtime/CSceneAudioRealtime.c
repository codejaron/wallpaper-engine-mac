#include "CSceneAudioRealtime.h"

#include <math.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

struct WEStereoPCMRingBuffer {
    uint32_t capacity;
    float *left;
    float *right;
    _Atomic uint64_t readFrame;
    _Atomic uint64_t writeFrame;
    _Atomic uint64_t droppedFrames;
    _Atomic uint64_t sampleRateBits;
};

typedef struct WEPCMLayout {
    const uint8_t *channelData[2];
    uint32_t channelStride[2];
    uint32_t bytesPerSample;
    uint32_t bitsPerChannel;
    bool isFloat;
    bool isSignedInteger;
    bool isBigEndian;
    uint32_t channelCount;
} WEPCMLayout;

static uint16_t WEReadU16(const uint8_t *bytes, bool bigEndian) {
    if (bigEndian) {
        return ((uint16_t)bytes[0] << 8U) | (uint16_t)bytes[1];
    }
    return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8U);
}

static uint32_t WEReadU32(const uint8_t *bytes, bool bigEndian) {
    if (bigEndian) {
        return ((uint32_t)bytes[0] << 24U) |
            ((uint32_t)bytes[1] << 16U) |
            ((uint32_t)bytes[2] << 8U) |
            (uint32_t)bytes[3];
    }
    return (uint32_t)bytes[0] |
        ((uint32_t)bytes[1] << 8U) |
        ((uint32_t)bytes[2] << 16U) |
        ((uint32_t)bytes[3] << 24U);
}

static uint64_t WEReadU64(const uint8_t *bytes, bool bigEndian) {
    uint64_t result = 0;
    if (bigEndian) {
        for (uint32_t index = 0; index < 8; ++index) {
            result = (result << 8U) | bytes[index];
        }
    } else {
        for (uint32_t index = 0; index < 8; ++index) {
            result |= (uint64_t)bytes[index] << (index * 8U);
        }
    }
    return result;
}

static bool WEDecodeSample(
    const uint8_t *bytes,
    const WEPCMLayout *layout,
    float *result
) {
    if (layout->isFloat) {
        if (layout->bytesPerSample == 4) {
            const uint32_t bits = WEReadU32(bytes, layout->isBigEndian);
            float value = 0.0F;
            memcpy(&value, &bits, sizeof(value));
            if (!isfinite(value)) return false;
            *result = value;
            return true;
        }
        if (layout->bytesPerSample == 8) {
            const uint64_t bits = WEReadU64(bytes, layout->isBigEndian);
            double value = 0.0;
            memcpy(&value, &bits, sizeof(value));
            if (!isfinite(value)) return false;
            *result = (float)value;
            return isfinite(*result);
        }
        return false;
    }

    if (!layout->isSignedInteger) {
        if (layout->bytesPerSample != 1) return false;
        *result = ((float)bytes[0] - 128.0F) / 128.0F;
        return true;
    }

    switch (layout->bytesPerSample) {
    case 1:
        *result = (float)(int8_t)bytes[0] / 127.0F;
        return true;
    case 2:
        *result = (float)(int16_t)WEReadU16(bytes, layout->isBigEndian) / 32767.0F;
        return true;
    case 3: {
        uint32_t value = layout->isBigEndian
            ? ((uint32_t)bytes[0] << 16U) | ((uint32_t)bytes[1] << 8U) | bytes[2]
            : (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8U) |
                ((uint32_t)bytes[2] << 16U);
        if ((value & 0x00800000U) != 0) value |= 0xFF000000U;
        *result = (float)(int32_t)value / 8388607.0F;
        return true;
    }
    case 4:
        *result = (float)(int32_t)WEReadU32(bytes, layout->isBigEndian) /
            2147483647.0F;
        return true;
    case 8:
        *result = (float)((double)(int64_t)WEReadU64(bytes, layout->isBigEndian) /
            9223372036854775807.0);
        return true;
    default:
        return false;
    }
}

static bool WECreateLayout(
    const AudioBufferList *input,
    uint32_t frameCount,
    const AudioStreamBasicDescription *format,
    WEPCMLayout *layout
) {
    if (input == NULL || format == NULL || layout == NULL || frameCount == 0 ||
        format->mFormatID != kAudioFormatLinearPCM ||
        !isfinite(format->mSampleRate) || format->mSampleRate <= 0.0 ||
        (format->mChannelsPerFrame != 1 && format->mChannelsPerFrame != 2)) {
        return false;
    }

    memset(layout, 0, sizeof(*layout));
    layout->channelCount = format->mChannelsPerFrame;
    layout->bitsPerChannel = format->mBitsPerChannel;
    layout->bytesPerSample = (format->mBitsPerChannel + 7U) / 8U;
    if (layout->bytesPerSample == 0) layout->bytesPerSample = 1;
    layout->isFloat = (format->mFormatFlags & kAudioFormatFlagIsFloat) != 0;
    layout->isSignedInteger =
        (format->mFormatFlags & kAudioFormatFlagIsSignedInteger) != 0;
    layout->isBigEndian =
        (format->mFormatFlags & kAudioFormatFlagIsBigEndian) != 0;

    const bool nonInterleaved =
        (format->mFormatFlags & kAudioFormatFlagIsNonInterleaved) != 0 ||
        input->mNumberBuffers > 1;
    if (nonInterleaved) {
        if (input->mNumberBuffers < layout->channelCount) return false;
        const uint32_t stride = format->mBytesPerFrame > layout->bytesPerSample
            ? format->mBytesPerFrame
            : layout->bytesPerSample;
        for (uint32_t channel = 0; channel < layout->channelCount; ++channel) {
            const AudioBuffer *audioBuffer = &input->mBuffers[channel];
            if (audioBuffer->mData == NULL || audioBuffer->mNumberChannels != 1 ||
                stride == 0 || audioBuffer->mDataByteSize / stride < frameCount) {
                return false;
            }
            layout->channelData[channel] = audioBuffer->mData;
            layout->channelStride[channel] = stride;
        }
    } else {
        if (input->mNumberBuffers != 1) return false;
        const AudioBuffer *audioBuffer = &input->mBuffers[0];
        const uint32_t minimumStride =
            layout->bytesPerSample * layout->channelCount;
        const uint32_t stride = format->mBytesPerFrame > minimumStride
            ? format->mBytesPerFrame
            : minimumStride;
        if (audioBuffer->mData == NULL ||
            audioBuffer->mNumberChannels < layout->channelCount || stride == 0 ||
            audioBuffer->mDataByteSize / stride < frameCount) {
            return false;
        }
        for (uint32_t channel = 0; channel < layout->channelCount; ++channel) {
            layout->channelData[channel] =
                (const uint8_t *)audioBuffer->mData +
                channel * layout->bytesPerSample;
            layout->channelStride[channel] = stride;
        }
    }
    return true;
}

static bool WEDecodeFrame(
    const WEPCMLayout *layout,
    uint32_t frame,
    float *left,
    float *right
) {
    if (!WEDecodeSample(
            layout->channelData[0] + frame * layout->channelStride[0],
            layout,
            left
        )) {
        return false;
    }
    if (layout->channelCount == 1) {
        *right = *left;
        return true;
    }
    return WEDecodeSample(
        layout->channelData[1] + frame * layout->channelStride[1],
        layout,
        right
    );
}

bool WEAudioDecodeStereoPCM(
    const AudioBufferList *input,
    uint32_t frameCount,
    const AudioStreamBasicDescription *format,
    float *left,
    float *right
) {
    if (left == NULL || right == NULL) return false;
    WEPCMLayout layout;
    if (!WECreateLayout(input, frameCount, format, &layout)) return false;
    for (uint32_t frame = 0; frame < frameCount; ++frame) {
        if (!WEDecodeFrame(&layout, frame, &left[frame], &right[frame])) {
            return false;
        }
    }
    return true;
}

WEStereoPCMRingBuffer *WEAudioRingBufferCreate(uint32_t capacityFrames) {
    if (capacityFrames < 2) return NULL;
    WEStereoPCMRingBuffer *result = calloc(1, sizeof(*result));
    if (result == NULL) return NULL;
    result->left = calloc(capacityFrames, sizeof(float));
    result->right = calloc(capacityFrames, sizeof(float));
    if (result->left == NULL || result->right == NULL) {
        WEAudioRingBufferDestroy(result);
        return NULL;
    }
    result->capacity = capacityFrames;
    return result;
}

void WEAudioRingBufferDestroy(WEStereoPCMRingBuffer *buffer) {
    if (buffer == NULL) return;
    free(buffer->left);
    free(buffer->right);
    free(buffer);
}

bool WEAudioRingBufferWrite(
    WEStereoPCMRingBuffer *buffer,
    const AudioBufferList *input,
    uint32_t frameCount,
    const AudioStreamBasicDescription *format
) {
    if (buffer == NULL || format == NULL || frameCount == 0) return false;
    WEPCMLayout layout;
    if (!WECreateLayout(input, frameCount, format, &layout)) return false;

    const uint64_t writeFrame = atomic_load_explicit(
        &buffer->writeFrame, memory_order_relaxed
    );
    const uint64_t readFrame = atomic_load_explicit(
        &buffer->readFrame, memory_order_acquire
    );
    const uint64_t used = writeFrame - readFrame;
    if (used > buffer->capacity || frameCount > buffer->capacity - used) {
        atomic_fetch_add_explicit(
            &buffer->droppedFrames, frameCount, memory_order_relaxed
        );
        return false;
    }

    for (uint32_t frame = 0; frame < frameCount; ++frame) {
        const uint32_t destination =
            (uint32_t)((writeFrame + frame) % buffer->capacity);
        if (!WEDecodeFrame(
                &layout,
                frame,
                &buffer->left[destination],
                &buffer->right[destination]
            )) {
            atomic_fetch_add_explicit(
                &buffer->droppedFrames, frameCount, memory_order_relaxed
            );
            return false;
        }
    }
    uint64_t sampleRateBits = 0;
    memcpy(&sampleRateBits, &format->mSampleRate, sizeof(sampleRateBits));
    atomic_store_explicit(
        &buffer->sampleRateBits, sampleRateBits, memory_order_relaxed
    );
    atomic_store_explicit(
        &buffer->writeFrame, writeFrame + frameCount, memory_order_release
    );
    return true;
}

bool WEAudioRingBufferReadLatest(
    WEStereoPCMRingBuffer *buffer,
    float *left,
    float *right,
    uint32_t frameCount,
    double *sampleRate
) {
    if (buffer == NULL || left == NULL || right == NULL || sampleRate == NULL ||
        frameCount == 0 || frameCount > buffer->capacity) {
        return false;
    }
    const uint64_t writeFrame = atomic_load_explicit(
        &buffer->writeFrame, memory_order_acquire
    );
    const uint64_t readFrame = atomic_load_explicit(
        &buffer->readFrame, memory_order_relaxed
    );
    if (writeFrame - readFrame < frameCount) return false;

    const uint64_t start = writeFrame - frameCount;
    for (uint32_t frame = 0; frame < frameCount; ++frame) {
        const uint32_t source = (uint32_t)((start + frame) % buffer->capacity);
        left[frame] = buffer->left[source];
        right[frame] = buffer->right[source];
    }
    const uint64_t sampleRateBits = atomic_load_explicit(
        &buffer->sampleRateBits, memory_order_relaxed
    );
    memcpy(sampleRate, &sampleRateBits, sizeof(*sampleRate));
    atomic_store_explicit(&buffer->readFrame, writeFrame, memory_order_release);
    return isfinite(*sampleRate) && *sampleRate > 0.0;
}

uint64_t WEAudioRingBufferDroppedFrames(const WEStereoPCMRingBuffer *buffer) {
    if (buffer == NULL) return 0;
    return atomic_load_explicit(&buffer->droppedFrames, memory_order_relaxed);
}

void WEAudioRingBufferReset(WEStereoPCMRingBuffer *buffer) {
    if (buffer == NULL) return;
    atomic_store_explicit(&buffer->readFrame, 0, memory_order_relaxed);
    atomic_store_explicit(&buffer->writeFrame, 0, memory_order_relaxed);
    atomic_store_explicit(&buffer->droppedFrames, 0, memory_order_relaxed);
    atomic_store_explicit(&buffer->sampleRateBits, 0, memory_order_relaxed);
}
