#ifndef WE_SCENE_PARTICLE_TEST_SUPPORT_H
#define WE_SCENE_PARTICLE_TEST_SUPPORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WESceneParticleTestHandle* WESceneParticleTestHandleRef;

typedef struct WESceneParticleTestParticleInfo {
    uint64_t spawnId;
    double positionX;
    double positionY;
    double positionZ;
    double velocityX;
    double velocityY;
    double velocityZ;
    double rotationX;
    double rotationY;
    double rotationZ;
    double angularVelocityX;
    double angularVelocityY;
    double angularVelocityZ;
    double colorR;
    double colorG;
    double colorB;
    double alpha;
    double size;
    double lifetime;
    double age;
    double randomFrameUnit;
} WESceneParticleTestParticleInfo;

int we_scene_particle_test_create(
    const char* scenario,
    int objectId,
    const char* assetPath,
    int hasExplicitSeed,
    uint64_t explicitSeed,
    WESceneParticleTestHandleRef* outHandle,
    char* errorBuffer,
    size_t errorCapacity
);

int we_scene_particle_test_copy(
    WESceneParticleTestHandleRef source,
    WESceneParticleTestHandleRef* outHandle,
    char* errorBuffer,
    size_t errorCapacity
);

int we_scene_particle_test_advance(
    WESceneParticleTestHandleRef handle,
    double elapsedSeconds,
    char* errorBuffer,
    size_t errorCapacity
);

int we_scene_particle_test_advance_with_overrides(
    WESceneParticleTestHandleRef handle,
    double elapsedSeconds,
    int enabled,
    double alpha,
    double size,
    double lifetime,
    double rate,
    double speed,
    double count,
    double colorR,
    double colorG,
    double colorB,
    double colorMultiplierR,
    double colorMultiplierG,
    double colorMultiplierB,
    char* errorBuffer,
    size_t errorCapacity
);

uint64_t we_scene_particle_test_seed(WESceneParticleTestHandleRef handle);
size_t we_scene_particle_test_count(WESceneParticleTestHandleRef handle);
uint32_t we_scene_particle_test_flags(WESceneParticleTestHandleRef handle);
double we_scene_particle_test_accumulator(WESceneParticleTestHandleRef handle);

int we_scene_particle_test_particle(
    WESceneParticleTestHandleRef handle,
    size_t index,
    WESceneParticleTestParticleInfo* outParticle
);

// Returns the required UTF-8 byte count including the trailing null byte.
size_t we_scene_particle_test_snapshot(
    WESceneParticleTestHandleRef handle,
    char* buffer,
    size_t capacity
);

void we_scene_particle_test_destroy(WESceneParticleTestHandleRef handle);

#ifdef __cplusplus
}
#endif

#endif
