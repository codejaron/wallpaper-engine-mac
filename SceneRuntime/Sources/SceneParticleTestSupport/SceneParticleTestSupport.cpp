#include <SceneParticleTestSupport/SceneParticleTestSupport.h>

#include <SceneParticle/ParticleSimulation.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

using we::scene::particle::AlphaFadeOperator;
using we::scene::particle::AlphaRandomInitializer;
using we::scene::particle::AngularMovementOperator;
using we::scene::particle::AngularVelocityRandomInitializer;
using we::scene::particle::BoxRandomEmitter;
using we::scene::particle::ColorRandomInitializer;
using we::scene::particle::Configuration;
using we::scene::particle::ControlPoint;
using we::scene::particle::ControlPointAttractOperator;
using we::scene::particle::LifetimeRandomInitializer;
using we::scene::particle::MovementOperator;
using we::scene::particle::OscillateAlphaOperator;
using we::scene::particle::OscillatePositionOperator;
using we::scene::particle::ParticleSimulation;
using we::scene::particle::RotationRandomInitializer;
using we::scene::particle::SizeRandomInitializer;
using we::scene::particle::SphereRandomEmitter;
using we::scene::particle::TurbulentVelocityRandomInitializer;
using we::scene::particle::Vector3;
using we::scene::particle::VelocityRandomInitializer;

struct WESceneParticleTestHandle {
    explicit WESceneParticleTestHandle(ParticleSimulation value)
        : simulation(std::move(value)) {}

    ParticleSimulation simulation;
};

namespace {

void writeError(const std::string& message, char* buffer, std::size_t capacity) {
    if (buffer == nullptr || capacity == 0) {
        return;
    }
    const std::size_t count = std::min(capacity - 1, message.size());
    std::memcpy(buffer, message.data(), count);
    buffer[count] = '\0';
}

Configuration replayConfiguration() {
    Configuration configuration;
    configuration.maxCount = 8;
    configuration.fixedStepSeconds = 1.0 / 60.0;
    configuration.emitters.push_back(BoxRandomEmitter{.base = {
        .directions = {1.0, 0.5, 1.0},
        .distanceMin = {1.0, 2.0, 0.5},
        .distanceMax = {4.0, 6.0, 2.0},
        .origin = {3.0, 7.0, -2.0},
        .instantaneous = 2,
        .rate = 3.0,
    }});
    configuration.initializers = {
        LifetimeRandomInitializer{0.8, 1.4},
        SizeRandomInitializer{4.0, 12.0, 1.5},
        ColorRandomInitializer{{0.2, 0.4, 0.6}, {1.0, 0.9, 0.8}},
        AlphaRandomInitializer{0.4, 1.0},
        VelocityRandomInitializer{{-4.0, 2.0, -1.0}, {5.0, 8.0, 3.0}},
        RotationRandomInitializer{{-1.0, -2.0, -3.0}, {1.0, 2.0, 3.0}},
    };
    configuration.operators = {
        MovementOperator{.drag = 0.2, .gravity = {0.0, 9.0, 0.0}},
        AlphaFadeOperator{.fadeInTime = 0.2, .fadeOutTime = 0.75},
    };
    return configuration;
}

Configuration boxConfiguration() {
    Configuration configuration;
    configuration.maxCount = 64;
    configuration.emitters.push_back(BoxRandomEmitter{.base = {
        .directions = {1.0, 2.0, 0.5},
        .distanceMin = {1.0, 2.0, 3.0},
        .distanceMax = {4.0, 5.0, 6.0},
        .origin = {10.0, 20.0, 30.0},
        .instantaneous = 64,
        .rate = 0.0,
    }});
    configuration.initializers.push_back(LifetimeRandomInitializer{2.0, 2.0});
    return configuration;
}

Configuration sphereConfiguration(bool authoredZ) {
    Configuration configuration;
    configuration.maxCount = 128;
    configuration.emitters.push_back(SphereRandomEmitter{
        .base = {
            .directions = {1.0, 1.0, authoredZ ? 1.0 : 0.0},
            .distanceMin = {2.0, 0.0, 0.0},
            .distanceMax = {5.0, 0.0, 0.0},
            .origin = {0.0, 4.0, 0.0},
            .instantaneous = 128,
            .rate = 0.0,
        },
        .sign = {1.0, -1.0, 0.0},
        .speedMin = 0.0,
        .speedMax = 0.0,
    });
    configuration.initializers.push_back(LifetimeRandomInitializer{2.0, 2.0});
    return configuration;
}

Configuration motionFadeConfiguration() {
    Configuration configuration;
    configuration.maxCount = 1;
    configuration.fixedStepSeconds = 0.125;
    configuration.emitters.push_back(BoxRandomEmitter{.base = {
        .directions = {0.0, 0.0, 0.0},
        .distanceMin = {},
        .distanceMax = {},
        .origin = {},
        .instantaneous = 1,
        .rate = 0.0,
    }});
    configuration.initializers = {
        LifetimeRandomInitializer{1.0, 1.0},
        VelocityRandomInitializer{{8.0, 6.0, 0.0}, {8.0, 6.0, 0.0}},
        AlphaRandomInitializer{0.8, 0.8},
        SizeRandomInitializer{10.0, 10.0, 1.0},
    };
    configuration.operators = {
        MovementOperator{.drag = 0.0, .gravity = {0.0, 4.0, 0.0}},
        AlphaFadeOperator{.fadeInTime = 0.25, .fadeOutTime = 0.75},
    };
    return configuration;
}

Configuration recycleConfiguration() {
    Configuration configuration;
    configuration.maxCount = 3;
    configuration.fixedStepSeconds = 0.05;
    configuration.emitters.push_back(BoxRandomEmitter{.base = {
        .directions = {0.0, 0.0, 0.0},
        .distanceMin = {},
        .distanceMax = {},
        .origin = {},
        .instantaneous = 3,
        .rate = 20.0,
    }});
    configuration.initializers.push_back(LifetimeRandomInitializer{0.1, 0.3});
    return configuration;
}

Configuration overrideConfiguration() {
    Configuration configuration;
    configuration.maxCount = 4;
    configuration.fixedStepSeconds = 0.1;
    configuration.overrides = {
        .enabled = true,
        .alpha = 0.5,
        .size = 2.0,
        .lifetime = 3.0,
        .rate = 2.0,
        .speed = 3.0,
        .count = 0.5,
        .color = {0.25, 0.5, 1.0},
        .colorMultiplier = {0.5, 2.0, 1.0},
    };
    configuration.emitters.push_back(BoxRandomEmitter{.base = {
        .directions = {},
        .distanceMin = {},
        .distanceMax = {},
        .origin = {},
        .instantaneous = 4,
        .rate = 0.0,
    }});
    configuration.initializers = {
        LifetimeRandomInitializer{2.0, 2.0},
        SizeRandomInitializer{10.0, 10.0, 1.0},
        ColorRandomInitializer{{0.2, 0.4, 0.6}, {0.2, 0.4, 0.6}},
        AlphaRandomInitializer{0.8, 0.8},
        VelocityRandomInitializer{{1.0, 2.0, 0.0}, {1.0, 2.0, 0.0}},
    };
    return configuration;
}

Configuration disabledConfiguration() {
    Configuration configuration;
    configuration.maxCount = 4;
    configuration.fixedStepSeconds = 0.1;
    configuration.overrides = {
        .enabled = false,
        .alpha = 0.1,
        .size = 0.1,
        .lifetime = 0.1,
        .rate = 0.1,
        .speed = 0.1,
        .count = 0.1,
        .color = {0.1, 0.1, 0.1},
        .colorMultiplier = {0.1, 0.1, 0.1},
    };
    configuration.emitters.push_back(BoxRandomEmitter{.base = {
        .directions = {},
        .distanceMin = {},
        .distanceMax = {},
        .origin = {},
        .instantaneous = 4,
        .rate = 0.0,
    }});
    configuration.initializers = {
        LifetimeRandomInitializer{2.0, 2.0},
        AlphaRandomInitializer{0.8, 0.8},
    };
    return configuration;
}

Configuration prewarmConfiguration() {
    Configuration configuration;
    configuration.maxCount = 10;
    configuration.fixedStepSeconds = 0.25;
    configuration.startTime = 1.0;
    configuration.overrides.rate = 2.0;
    configuration.emitters.push_back(BoxRandomEmitter{.base = {
        .directions = {},
        .distanceMin = {},
        .distanceMax = {},
        .origin = {},
        .instantaneous = 0,
        .rate = 1.0,
    }});
    configuration.initializers.push_back(LifetimeRandomInitializer{10.0, 10.0});
    return configuration;
}

Configuration substepPrewarmConfiguration() {
    Configuration configuration;
    configuration.maxCount = 1;
    configuration.fixedStepSeconds = 0.25;
    configuration.startTime =
        configuration.fixedStepSeconds * (1.0 - 0.5e-9);
    configuration.emitters.push_back(BoxRandomEmitter{.base = {
        .directions = {},
        .distanceMin = {},
        .distanceMax = {},
        .origin = {},
        .instantaneous = 1,
        .rate = 0.0,
    }});
    configuration.initializers.push_back(LifetimeRandomInitializer{10.0, 10.0});
    return configuration;
}

Configuration excessivePrewarmConfiguration() {
    Configuration configuration;
    configuration.fixedStepSeconds = 0.25;
    configuration.startTime = 1.0e12;
    return configuration;
}

Configuration overflowingPrewarmConfiguration() {
    Configuration configuration;
    configuration.fixedStepSeconds = 1.0;
    configuration.startTime = 18'446'744'073'709'551'616.0;
    return configuration;
}

Configuration perspectiveSphereConfiguration() {
    Configuration configuration;
    configuration.maxCount = 256;
    configuration.flags = 4;
    configuration.emitters.push_back(SphereRandomEmitter{
        .base = {
            .directions = {1.0, 1.0, 1.0},
            .distanceMin = {2.0, 0.0, 0.0},
            .distanceMax = {5.0, 0.0, 0.0},
            .origin = {},
            .instantaneous = 256,
            .rate = 0.0,
        },
        .sign = {},
        .speedMin = 0.0,
        .speedMax = 0.0,
    });
    configuration.initializers.push_back(LifetimeRandomInitializer{2.0, 2.0});
    return configuration;
}

Configuration limitedEmitterConfiguration() {
    Configuration configuration;
    configuration.maxCount = 10;
    configuration.fixedStepSeconds = 1.0;
    configuration.emitters.push_back(BoxRandomEmitter{.base = {
        .directions = {},
        .distanceMin = {},
        .distanceMax = {},
        .origin = {},
        .instantaneous = 0,
        .rate = 10.0,
        .controlPoint = -1,
        .flags = 2,
    }});
    configuration.initializers.push_back(LifetimeRandomInitializer{2.0, 2.0});
    return configuration;
}

Configuration delayedDurationConfiguration() {
    Configuration configuration;
    configuration.maxCount = 10;
    configuration.fixedStepSeconds = 0.25;
    configuration.emitters.push_back(BoxRandomEmitter{.base = {
        .directions = {},
        .distanceMin = {},
        .distanceMax = {},
        .origin = {},
        .instantaneous = 0,
        .rate = 4.0,
        .controlPoint = -1,
        .flags = 0,
        .delay = 0.5,
        .duration = 0.5,
    }});
    configuration.initializers.push_back(LifetimeRandomInitializer{10.0, 10.0});
    return configuration;
}

Configuration periodicEmitterConfiguration() {
    Configuration configuration;
    configuration.maxCount = 20;
    configuration.fixedStepSeconds = 0.25;
    configuration.emitters.push_back(SphereRandomEmitter{
        .base = {
            .directions = {},
            .distanceMin = {},
            .distanceMax = {},
            .origin = {},
            .instantaneous = 0,
            .rate = 8.0,
            .controlPoint = -1,
            .flags = 4,
            .delay = 0.0,
            .duration = 0.0,
            .minPeriodicDelay = 0.5,
            .maxPeriodicDelay = 0.5,
            .minPeriodicDuration = 0.5,
            .maxPeriodicDuration = 0.5,
            .maxToEmitPerPeriod = 3,
        },
    });
    configuration.initializers.push_back(LifetimeRandomInitializer{10.0, 10.0});
    return configuration;
}

Configuration capacityClockConfiguration() {
    Configuration configuration;
    configuration.maxCount = 1;
    configuration.fixedStepSeconds = 0.1;
    configuration.emitters.push_back(BoxRandomEmitter{.base = {
        .directions = {},
        .distanceMin = {},
        .distanceMax = {},
        .origin = {},
        .instantaneous = 1,
        .rate = 10.0,
        .controlPoint = -1,
        .flags = 0,
        .delay = 0.0,
        .duration = 0.15,
    }});
    configuration.initializers.push_back(LifetimeRandomInitializer{0.2, 0.2});
    return configuration;
}

Configuration periodicCapacityConfiguration() {
    Configuration configuration;
    configuration.maxCount = 2;
    configuration.fixedStepSeconds = 0.1;
    configuration.emitters.push_back(BoxRandomEmitter{.base = {
        .directions = {},
        .distanceMin = {},
        .distanceMax = {},
        .origin = {},
        .instantaneous = 2,
        .rate = 10.0,
        .controlPoint = -1,
        .flags = 4,
        .delay = 0.0,
        .duration = 0.0,
        .minPeriodicDelay = 1.0,
        .maxPeriodicDelay = 1.0,
        .minPeriodicDuration = 1.0,
        .maxPeriodicDuration = 1.0,
        .maxToEmitPerPeriod = 1,
    }});
    configuration.initializers.push_back(LifetimeRandomInitializer{0.2, 0.2});
    return configuration;
}

Configuration compatibleParticleFlagsConfiguration() {
    Configuration configuration;
    configuration.maxCount = 1;
    configuration.flags = 248;
    configuration.emitters.push_back(BoxRandomEmitter{.base = {
        .directions = {},
        .distanceMin = {},
        .distanceMax = {},
        .origin = {},
        .instantaneous = 1,
        .rate = 0.0,
    }});
    configuration.initializers.push_back(LifetimeRandomInitializer{2.0, 2.0});
    return configuration;
}

Configuration angularConfiguration() {
    Configuration configuration;
    configuration.maxCount = 1;
    configuration.fixedStepSeconds = 0.25;
    configuration.emitters.push_back(BoxRandomEmitter{.base = {
        .directions = {},
        .distanceMin = {},
        .distanceMax = {},
        .origin = {},
        .instantaneous = 1,
        .rate = 0.0,
    }});
    configuration.initializers = {
        LifetimeRandomInitializer{2.0, 2.0},
        AngularVelocityRandomInitializer{
            .minimum = {0.0, 0.0, 2.0},
            .maximum = {0.0, 0.0, 2.0},
            .exponent = 1.0,
        },
    };
    configuration.operators.push_back(AngularMovementOperator{
        .drag = 0.0,
        .force = {0.0, 0.0, 4.0},
    });
    return configuration;
}

Configuration attractConfiguration() {
    Configuration configuration;
    configuration.maxCount = 1;
    configuration.fixedStepSeconds = 0.25;
    configuration.emitters.push_back(BoxRandomEmitter{.base = {
        .directions = {},
        .distanceMin = {},
        .distanceMax = {},
        .origin = {},
        .instantaneous = 1,
        .rate = 0.0,
    }});
    configuration.initializers.push_back(LifetimeRandomInitializer{2.0, 2.0});
    configuration.controlPoints.push_back(ControlPoint{.id = 1, .position = {10.0, 0.0, 0.0}});
    configuration.operators.push_back(ControlPointAttractOperator{
        .controlPoint = 1,
        .origin = {},
        .scale = -4.0,
        .threshold = 100.0,
    });
    return configuration;
}

Configuration controlPointEmitterConfiguration() {
    Configuration configuration;
    configuration.maxCount = 1;
    configuration.fixedStepSeconds = 0.1;
    configuration.controlPoints.push_back(ControlPoint{
        .id = 1,
        .position = {4.0, -3.0, 2.0},
    });
    configuration.emitters.push_back(BoxRandomEmitter{.base = {
        .directions = {},
        .distanceMin = {},
        .distanceMax = {},
        .origin = {},
        .instantaneous = 1,
        .rate = 0.0,
        .controlPoint = 1,
    }});
    configuration.initializers.push_back(LifetimeRandomInitializer{2.0, 2.0});
    return configuration;
}

Configuration oscillatorConfiguration() {
    Configuration configuration;
    configuration.maxCount = 2;
    configuration.fixedStepSeconds = 0.05;
    configuration.emitters.push_back(BoxRandomEmitter{.base = {
        .directions = {},
        .distanceMin = {},
        .distanceMax = {},
        .origin = {},
        .instantaneous = 2,
        .rate = 0.0,
    }});
    configuration.initializers = {
        LifetimeRandomInitializer{5.0, 5.0},
        AlphaRandomInitializer{1.0, 1.0},
    };
    configuration.operators = {
        OscillatePositionOperator{
            .frequencyMinimum = 0.5,
            .frequencyMaximum = 2.0,
            .scaleMinimum = 1.0,
            .scaleMaximum = 3.0,
            .phaseMinimum = 0.0,
            .phaseMaximum = 1.0,
            .mask = {1.0, 1.0, 0.0},
        },
        OscillateAlphaOperator{
            .frequencyMinimum = 1.0,
            .frequencyMaximum = 3.0,
            .scaleMinimum = 0.25,
            .scaleMaximum = 0.75,
            .phaseMinimum = 0.0,
            .phaseMaximum = 1.0,
        },
    };
    return configuration;
}

Configuration turbulentConfiguration() {
    Configuration configuration;
    configuration.maxCount = 16;
    configuration.emitters.push_back(BoxRandomEmitter{.base = {
        .directions = {},
        .distanceMin = {},
        .distanceMax = {},
        .origin = {},
        .instantaneous = 16,
        .rate = 0.0,
    }});
    configuration.initializers = {
        LifetimeRandomInitializer{2.0, 2.0},
        TurbulentVelocityRandomInitializer{
            .speedMinimum = 10.0,
            .speedMaximum = 10.0,
            .scale = 1.0,
            .offset = 0.25,
            .forward = {0.0, 1.0, 0.0},
            .timeScale = 1.0,
            .phaseMinimum = 0.0,
            .phaseMaximum = 1.0,
            .right = {0.0, 0.0, 1.0},
        },
    };
    return configuration;
}

Configuration reverseRangeConfiguration() {
    Configuration configuration;
    configuration.maxCount = 16;
    configuration.emitters.push_back(BoxRandomEmitter{.base = {
        .directions = {},
        .distanceMin = {},
        .distanceMax = {},
        .origin = {},
        .instantaneous = 16,
        .rate = 0.0,
    }});
    configuration.initializers = {
        LifetimeRandomInitializer{2.0, 2.0},
        VelocityRandomInitializer{{10.0, 8.0, 6.0}, {-10.0, -8.0, -6.0}},
        ColorRandomInitializer{{1.0, 0.9, 0.8}, {0.2, 0.3, 0.4}},
    };
    return configuration;
}

Configuration invalidFadeConfiguration() {
    Configuration configuration;
    configuration.operators.push_back(AlphaFadeOperator{
        .fadeInTime = 0.8,
        .fadeOutTime = 0.2,
    });
    return configuration;
}

Configuration negativeDragConfiguration() {
    Configuration configuration;
    configuration.operators.push_back(MovementOperator{
        .drag = -0.1,
        .gravity = {},
    });
    return configuration;
}

Configuration invalidSignConfiguration() {
    Configuration configuration;
    configuration.emitters.push_back(SphereRandomEmitter{
        .base = {
            .directions = {1.0, 1.0, 0.0},
            .distanceMin = {},
            .distanceMax = {1.0, 1.0, 0.0},
            .origin = {},
            .instantaneous = 1,
            .rate = 0.0,
        },
        .sign = {2.0, 0.0, 0.0},
        .speedMin = 0.0,
        .speedMax = 0.0,
    });
    return configuration;
}

Configuration unsupportedEmitterFlagsConfiguration() {
    Configuration configuration;
    configuration.emitters.push_back(BoxRandomEmitter{.base = {
        .directions = {},
        .distanceMin = {},
        .distanceMax = {},
        .origin = {},
        .instantaneous = 0,
        .rate = 0.0,
        .controlPoint = -1,
        .flags = 8,
    }});
    return configuration;
}

Configuration negativeEmitterTimingConfiguration() {
    Configuration configuration;
    configuration.emitters.push_back(BoxRandomEmitter{.base = {
        .directions = {},
        .distanceMin = {},
        .distanceMax = {},
        .origin = {},
        .instantaneous = 0,
        .rate = 0.0,
        .controlPoint = -1,
        .flags = 0,
        .delay = -1.0,
    }});
    return configuration;
}

Configuration duplicateControlPointConfiguration() {
    Configuration configuration;
    configuration.controlPoints = {
        ControlPoint{.id = 1, .position = {}},
        ControlPoint{.id = 1, .position = {1.0, 0.0, 0.0}},
    };
    return configuration;
}

Configuration outOfRangeEmitterControlPointConfiguration() {
    Configuration configuration;
    configuration.emitters.push_back(BoxRandomEmitter{.base = {
        .directions = {},
        .distanceMin = {},
        .distanceMax = {},
        .origin = {},
        .instantaneous = 1,
        .rate = 0.0,
        .controlPoint = 8,
    }});
    return configuration;
}

Configuration outOfRangeAttractControlPointConfiguration() {
    Configuration configuration;
    configuration.operators.push_back(ControlPointAttractOperator{
        .controlPoint = 8,
        .origin = {},
        .scale = 1.0,
        .threshold = 1.0,
    });
    return configuration;
}

Configuration linkedControlPointZeroConfiguration() {
    Configuration configuration;
    configuration.maxCount = 1;
    configuration.fixedStepSeconds = 0.1;
    configuration.controlPoints.push_back(ControlPoint{
        .id = 0,
        .position = {4.0, -3.0, 2.0},
        .linkedToPointer = true,
    });
    configuration.emitters.push_back(BoxRandomEmitter{.base = {
        .directions = {},
        .distanceMin = {},
        .distanceMax = {},
        .origin = {},
        .instantaneous = 1,
        .rate = 0.0,
        .controlPoint = -1,
    }});
    configuration.initializers.push_back(
        LifetimeRandomInitializer{2.0, 2.0}
    );
    return configuration;
}

Configuration implicitControlPointConfiguration() {
    Configuration configuration;
    configuration.maxCount = 1;
    configuration.fixedStepSeconds = 0.25;
    configuration.emitters.push_back(BoxRandomEmitter{.base = {
        .directions = {},
        .distanceMin = {},
        .distanceMax = {},
        .origin = {1.0, 0.0, 0.0},
        .instantaneous = 1,
        .rate = 0.0,
        .controlPoint = 0,
    }});
    configuration.initializers.push_back(LifetimeRandomInitializer{2.0, 2.0});
    configuration.operators.push_back(ControlPointAttractOperator{
        .controlPoint = 7,
        .origin = {},
        .scale = 4.0,
        .threshold = 100.0,
    });
    return configuration;
}

Configuration negativeThresholdConfiguration() {
    Configuration configuration;
    configuration.controlPoints.push_back(ControlPoint{.id = 0, .position = {}});
    configuration.operators.push_back(ControlPointAttractOperator{
        .controlPoint = 0,
        .origin = {},
        .scale = -1.0,
        .threshold = -1.0,
    });
    return configuration;
}

Configuration configurationFor(const std::string& scenario) {
    if (scenario == "replay") {
        return replayConfiguration();
    }
    if (scenario == "box") {
        return boxConfiguration();
    }
    if (scenario == "sphere2d") {
        return sphereConfiguration(false);
    }
    if (scenario == "sphere3axis") {
        return sphereConfiguration(true);
    }
    if (scenario == "motionFade") {
        return motionFadeConfiguration();
    }
    if (scenario == "recycle") {
        return recycleConfiguration();
    }
    if (scenario == "overrides") {
        return overrideConfiguration();
    }
    if (scenario == "disabled") {
        return disabledConfiguration();
    }
    if (scenario == "prewarm") {
        return prewarmConfiguration();
    }
    if (scenario == "substepPrewarm") {
        return substepPrewarmConfiguration();
    }
    if (scenario == "excessivePrewarm") {
        return excessivePrewarmConfiguration();
    }
    if (scenario == "overflowingPrewarm") {
        return overflowingPrewarmConfiguration();
    }
    if (scenario == "perspectiveSphere") {
        return perspectiveSphereConfiguration();
    }
    if (scenario == "limitedEmitter") {
        return limitedEmitterConfiguration();
    }
    if (scenario == "delayedDuration") {
        return delayedDurationConfiguration();
    }
    if (scenario == "periodicEmitter") {
        return periodicEmitterConfiguration();
    }
    if (scenario == "capacityClock") {
        return capacityClockConfiguration();
    }
    if (scenario == "periodicCapacity") {
        return periodicCapacityConfiguration();
    }
    if (scenario == "compatibleParticleFlags") {
        return compatibleParticleFlagsConfiguration();
    }
    if (scenario == "angular") {
        return angularConfiguration();
    }
    if (scenario == "attract") {
        return attractConfiguration();
    }
    if (scenario == "controlPointEmitter") {
        return controlPointEmitterConfiguration();
    }
    if (scenario == "oscillators") {
        return oscillatorConfiguration();
    }
    if (scenario == "turbulent") {
        return turbulentConfiguration();
    }
    if (scenario == "reverseRanges") {
        return reverseRangeConfiguration();
    }
    if (scenario == "invalidFade") {
        return invalidFadeConfiguration();
    }
    if (scenario == "negativeDrag") {
        return negativeDragConfiguration();
    }
    if (scenario == "invalidSign") {
        return invalidSignConfiguration();
    }
    if (scenario == "unsupportedEmitterFlags") {
        return unsupportedEmitterFlagsConfiguration();
    }
    if (scenario == "negativeEmitterTiming") {
        return negativeEmitterTimingConfiguration();
    }
    if (scenario == "duplicateControlPoint") {
        return duplicateControlPointConfiguration();
    }
    if (scenario == "outOfRangeEmitterControlPoint") {
        return outOfRangeEmitterControlPointConfiguration();
    }
    if (scenario == "outOfRangeAttractControlPoint") {
        return outOfRangeAttractControlPointConfiguration();
    }
    if (scenario == "linkedControlPointZero") {
        return linkedControlPointZeroConfiguration();
    }
    if (scenario == "implicitControlPoint") {
        return implicitControlPointConfiguration();
    }
    if (scenario == "negativeThreshold") {
        return negativeThresholdConfiguration();
    }
    throw std::invalid_argument("Unknown particle test scenario: " + scenario);
}

std::string snapshot(const ParticleSimulation& simulation) {
    std::ostringstream stream;
    stream.precision(17);
    stream << "seed=" << simulation.seed()
           << ";acc=" << simulation.accumulatorSeconds();
    for (const auto& particle : simulation.particles()) {
        stream << "|" << particle.spawnId
               << ":" << particle.position.x
               << "," << particle.position.y
               << "," << particle.position.z
               << ":" << particle.velocity.x
               << "," << particle.velocity.y
               << "," << particle.velocity.z
               << ":" << particle.rotation.x
               << "," << particle.rotation.y
               << "," << particle.rotation.z
               << ":" << particle.angularVelocity.x
               << "," << particle.angularVelocity.y
               << "," << particle.angularVelocity.z
               << ":" << particle.color.x
               << "," << particle.color.y
               << "," << particle.color.z
               << ":" << particle.alpha
               << ":" << particle.size
               << ":" << particle.lifetime
               << ":" << particle.age
               << ":" << particle.randomFrameUnit;
    }
    return stream.str();
}

}  // namespace

int we_scene_particle_test_create(
    const char* scenario,
    int objectId,
    const char* assetPath,
    int hasExplicitSeed,
    std::uint64_t explicitSeed,
    WESceneParticleTestHandleRef* outHandle,
    char* errorBuffer,
    std::size_t errorCapacity
) {
    if (outHandle == nullptr) {
        writeError("Particle test output handle is null", errorBuffer, errorCapacity);
        return 0;
    }
    *outHandle = nullptr;
    try {
        const std::string scenarioValue = scenario == nullptr ? "" : scenario;
        const std::string assetValue = assetPath == nullptr ? "" : assetPath;
        std::optional<std::uint64_t> seed;
        if (hasExplicitSeed != 0) {
            seed = explicitSeed;
        }
        auto handle = std::make_unique<WESceneParticleTestHandle>(ParticleSimulation(
            configurationFor(scenarioValue),
            objectId,
            assetValue,
            seed
        ));
        *outHandle = handle.release();
        return 1;
    } catch (const std::exception& error) {
        writeError(error.what(), errorBuffer, errorCapacity);
        return 0;
    }
}

int we_scene_particle_test_copy(
    WESceneParticleTestHandleRef source,
    WESceneParticleTestHandleRef* outHandle,
    char* errorBuffer,
    std::size_t errorCapacity
) {
    if (source == nullptr || outHandle == nullptr) {
        writeError("Particle test copy argument is null", errorBuffer, errorCapacity);
        return 0;
    }
    *outHandle = nullptr;
    try {
        auto handle = std::make_unique<WESceneParticleTestHandle>(source->simulation);
        *outHandle = handle.release();
        return 1;
    } catch (const std::exception& error) {
        writeError(error.what(), errorBuffer, errorCapacity);
        return 0;
    }
}

int we_scene_particle_test_advance(
    WESceneParticleTestHandleRef handle,
    double elapsedSeconds,
    char* errorBuffer,
    std::size_t errorCapacity
) {
    if (handle == nullptr) {
        writeError("Particle test handle is null", errorBuffer, errorCapacity);
        return 0;
    }
    try {
        handle->simulation.advance(elapsedSeconds);
        return 1;
    } catch (const std::exception& error) {
        writeError(error.what(), errorBuffer, errorCapacity);
        return 0;
    }
}

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
    std::size_t errorCapacity
) {
    if (handle == nullptr) {
        writeError("Particle test handle is null", errorBuffer, errorCapacity);
        return 0;
    }
    try {
        Configuration configuration = handle->simulation.configuration();
        configuration.overrides = {
            .enabled = enabled != 0,
            .alpha = alpha,
            .size = size,
            .lifetime = lifetime,
            .rate = rate,
            .speed = speed,
            .count = count,
            .color = {colorR, colorG, colorB},
            .colorMultiplier = {
                colorMultiplierR,
                colorMultiplierG,
                colorMultiplierB,
            },
        };
        handle->simulation.advance(elapsedSeconds, configuration);
        return 1;
    } catch (const std::exception& error) {
        writeError(error.what(), errorBuffer, errorCapacity);
        return 0;
    }
}

std::uint64_t we_scene_particle_test_seed(WESceneParticleTestHandleRef handle) {
    return handle == nullptr ? 0 : handle->simulation.seed();
}

std::size_t we_scene_particle_test_count(WESceneParticleTestHandleRef handle) {
    return handle == nullptr ? 0 : handle->simulation.particles().size();
}

std::uint32_t we_scene_particle_test_flags(WESceneParticleTestHandleRef handle) {
    return handle == nullptr ? 0 : handle->simulation.configuration().flags;
}

double we_scene_particle_test_accumulator(WESceneParticleTestHandleRef handle) {
    return handle == nullptr ? 0.0 : handle->simulation.accumulatorSeconds();
}

int we_scene_particle_test_particle(
    WESceneParticleTestHandleRef handle,
    std::size_t index,
    WESceneParticleTestParticleInfo* outParticle
) {
    if (handle == nullptr || outParticle == nullptr ||
        index >= handle->simulation.particles().size()) {
        return 0;
    }
    const auto& particle = handle->simulation.particles()[index];
    *outParticle = {
        .spawnId = particle.spawnId,
        .positionX = particle.position.x,
        .positionY = particle.position.y,
        .positionZ = particle.position.z,
        .velocityX = particle.velocity.x,
        .velocityY = particle.velocity.y,
        .velocityZ = particle.velocity.z,
        .rotationX = particle.rotation.x,
        .rotationY = particle.rotation.y,
        .rotationZ = particle.rotation.z,
        .angularVelocityX = particle.angularVelocity.x,
        .angularVelocityY = particle.angularVelocity.y,
        .angularVelocityZ = particle.angularVelocity.z,
        .colorR = particle.color.x,
        .colorG = particle.color.y,
        .colorB = particle.color.z,
        .alpha = particle.alpha,
        .size = particle.size,
        .lifetime = particle.lifetime,
        .age = particle.age,
        .randomFrameUnit = particle.randomFrameUnit,
    };
    return 1;
}

std::size_t we_scene_particle_test_snapshot(
    WESceneParticleTestHandleRef handle,
    char* buffer,
    std::size_t capacity
) {
    if (handle == nullptr) {
        return 0;
    }
    const std::string value = snapshot(handle->simulation);
    const std::size_t required = value.size() + 1;
    if (buffer != nullptr && capacity > 0) {
        const std::size_t count = std::min(capacity - 1, value.size());
        std::memcpy(buffer, value.data(), count);
        buffer[count] = '\0';
    }
    return required;
}

void we_scene_particle_test_destroy(WESceneParticleTestHandleRef handle) {
    delete handle;
}
