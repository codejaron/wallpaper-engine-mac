#ifndef WE_SCENE_PARTICLE_PARTICLE_SIMULATION_HPP
#define WE_SCENE_PARTICLE_PARTICLE_SIMULATION_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace we::scene::particle {

struct Vector3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;

    [[nodiscard]] friend bool operator==(
        const Vector3& lhs,
        const Vector3& rhs
    ) = default;
};

struct EmitterBase {
    Vector3 directions{1.0, 1.0, 0.0};
    Vector3 distanceMin{};
    Vector3 distanceMax{256.0, 256.0, 0.0};
    Vector3 origin{};
    std::uint32_t instantaneous = 0;
    double rate = 10.0;
    int controlPoint = -1;
    std::uint32_t flags = 0;
    double delay = 0.0;
    double duration = 0.0;
    double minPeriodicDelay = 1.0;
    double maxPeriodicDelay = 2.0;
    double minPeriodicDuration = 2.0;
    double maxPeriodicDuration = 3.0;
    std::uint32_t maxToEmitPerPeriod = 0;
};

struct BoxRandomEmitter {
    EmitterBase base;
};

struct SphereRandomEmitter {
    EmitterBase base;
    Vector3 sign{};
    double speedMin = 0.0;
    double speedMax = 0.0;
};

using Emitter = std::variant<BoxRandomEmitter, SphereRandomEmitter>;

struct LifetimeRandomInitializer {
    double minimum = 1.0;
    double maximum = 1.0;
};

struct SizeRandomInitializer {
    double minimum = 20.0;
    double maximum = 20.0;
    double exponent = 1.0;
};

struct ColorRandomInitializer {
    Vector3 minimum{1.0, 1.0, 1.0};
    Vector3 maximum{1.0, 1.0, 1.0};
};

struct AlphaRandomInitializer {
    double minimum = 1.0;
    double maximum = 1.0;
};

struct VelocityRandomInitializer {
    Vector3 minimum{};
    Vector3 maximum{};
};

struct RotationRandomInitializer {
    Vector3 minimum{};
    Vector3 maximum{};
};

struct AngularVelocityRandomInitializer {
    Vector3 minimum{0.0, 0.0, -5.0};
    Vector3 maximum{0.0, 0.0, 5.0};
    double exponent = 1.0;
};

struct TurbulentVelocityRandomInitializer {
    double speedMinimum = 100.0;
    double speedMaximum = 250.0;
    double scale = 1.0;
    double offset = 0.0;
    Vector3 forward{0.0, 1.0, 0.0};
    double timeScale = 1.0;
    double phaseMinimum = 0.0;
    double phaseMaximum = 0.1;
    Vector3 right{0.0, 0.0, 1.0};
};

// The pinned Linux runtime keeps this initializer as a per-emitter sequence:
// every spawned particle receives the next angular slot and the counter wraps
// at `count`.  The concrete simulation stores the counter separately so a
// copied/replayed simulation remains deterministic.
struct MapSequenceAroundControlPointInitializer {
    int controlPoint = 0;
    int count = 1;
    Vector3 speedMinimum{};
    Vector3 speedMaximum{100.0, 100.0, 100.0};
};

using Initializer = std::variant<
    LifetimeRandomInitializer,
    SizeRandomInitializer,
    ColorRandomInitializer,
    AlphaRandomInitializer,
    VelocityRandomInitializer,
    RotationRandomInitializer,
    AngularVelocityRandomInitializer,
    TurbulentVelocityRandomInitializer,
    MapSequenceAroundControlPointInitializer
>;

struct MovementOperator {
    double drag = 0.0;
    Vector3 gravity{};
};

struct AlphaFadeOperator {
    double fadeInTime = 0.0;
    double fadeOutTime = 1.0;
};

struct AngularMovementOperator {
    double drag = 0.0;
    Vector3 force{};
};

struct OscillatePositionOperator {
    double frequencyMinimum = 0.0;
    double frequencyMaximum = 5.0;
    double scaleMinimum = 0.0;
    double scaleMaximum = 10.0;
    double phaseMinimum = 0.0;
    double phaseMaximum = 6.283185307179586;
    Vector3 mask{1.0, 1.0, 0.0};
};

struct OscillateAlphaOperator {
    double frequencyMinimum = 0.0;
    double frequencyMaximum = 10.0;
    double scaleMinimum = 0.0;
    double scaleMaximum = 1.0;
    double phaseMinimum = 0.0;
    double phaseMaximum = 6.283185307179586;
};

struct OscillateSizeOperator {
    double frequencyMinimum = 0.0;
    double frequencyMaximum = 10.0;
    double scaleMinimum = 0.8;
    double scaleMaximum = 1.2;
    double phaseMinimum = 0.0;
    double phaseMaximum = 6.283185307179586;
};

struct ControlPointAttractOperator {
    int controlPoint = 0;
    Vector3 origin{};
    double scale = 100.0;
    double threshold = 1000.0;
};

struct SizeChangeOperator {
    double startTime = 0.0;
    double endTime = 1.0;
    double startValue = 1.0;
    double endValue = 0.0;
};

struct AlphaChangeOperator {
    double startTime = 0.0;
    double endTime = 1.0;
    double startValue = 1.0;
    double endValue = 0.0;
};

struct ColorChangeOperator {
    double startTime = 0.0;
    double endTime = 1.0;
    Vector3 startValue{1.0, 1.0, 1.0};
    Vector3 endValue{1.0, 1.0, 1.0};
};

struct TurbulenceOperator {
    double scale = 0.005;
    double speedMinimum = 500.0;
    double speedMaximum = 1000.0;
    double timeScale = 0.01;
    Vector3 mask{1.0, 1.0, 0.0};
    double phaseMinimum = 0.0;
    double phaseMaximum = 0.0;
    // Retained for model parity; the pinned Linux implementation currently
    // leaves turbulence audio modulation as a TODO and ignores this value.
    int audioProcessingMode = 0;
};

struct VortexOperator {
    int controlPoint = 0;
    std::uint32_t flags = 0;
    Vector3 axis{0.0, 0.0, 1.0};
    Vector3 offset{};
    double distanceInner = 500.0;
    double distanceOuter = 650.0;
    double speedInner = 2500.0;
    double speedOuter = 0.0;
    double centerForce = 1.0;
    double ringRadius = 300.0;
    double ringWidth = 50.0;
    double ringPullDistance = 50.0;
    double ringPullForce = 10.0;
    int audioProcessingMode = 0;
};

using Operator = std::variant<
    MovementOperator,
    AlphaFadeOperator,
    AngularMovementOperator,
    OscillatePositionOperator,
    OscillateAlphaOperator,
    OscillateSizeOperator,
    ControlPointAttractOperator,
    SizeChangeOperator,
    AlphaChangeOperator,
    ColorChangeOperator,
    TurbulenceOperator,
    VortexOperator
>;

struct ControlPoint {
    int id = 0;
    // Already expressed in particle-local simulation coordinates.
    Vector3 position{};
    bool linkedToPointer = false;
};

struct ParticleInstanceOverrides {
    // Retained for model/bridge observability. The Linux runtime applies the
    // authored override values regardless of this editor metadata flag.
    bool enabled = true;
    double alpha = 1.0;
    double size = 1.0;
    double lifetime = 1.0;
    double rate = 1.0;
    double speed = 1.0;
    double count = 1.0;
    Vector3 color{1.0, 1.0, 1.0};
    Vector3 colorMultiplier{1.0, 1.0, 1.0};
};

// Contains only concrete values. DynamicValue evaluation remains owned by the
// frame graph, so simulation copies are transactional and property-source
// changes cannot create a second state store here.
struct Configuration {
    std::uint32_t maxCount = 100;
    double fixedStepSeconds = 1.0 / 120.0;
    double startTime = 0.0;
    std::uint32_t flags = 0;
    ParticleInstanceOverrides overrides;
    std::vector<Emitter> emitters;
    std::vector<Initializer> initializers;
    std::vector<Operator> operators;
    std::vector<ControlPoint> controlPoints;
};

struct ParticleOscillatorState {
    // Stable per-particle interpolation factors. Concrete operator ranges may
    // change each frame without re-sampling or introducing another state store.
    Vector3 frequencyUnit{};
    Vector3 scaleUnit{};
    Vector3 phaseUnit{};
};

struct ParticleInstance {
    std::uint64_t spawnId = 0;
    Vector3 position{};
    Vector3 velocity{};
    Vector3 simulationVelocity{};
    Vector3 rotation{};
    Vector3 angularVelocity{};
    Vector3 simulationAngularVelocity{};
    Vector3 color{1.0, 1.0, 1.0};
    double alpha = 1.0;
    double size = 20.0;
    double lifetime = 1.0;
    double age = 0.0;
    Vector3 initialColor{1.0, 1.0, 1.0};
    double initialAlpha = 1.0;
    double initialSize = 20.0;
    double initialLifetime = 1.0;
    // The renderer maps this stable unit interval value to its actual atlas
    // frame count for random-frame animation.
    double randomFrameUnit = 0.0;
    std::vector<ParticleOscillatorState> oscillatorStates;

    [[nodiscard]] double lifetimePosition() const noexcept;
    [[nodiscard]] bool alive() const noexcept;

    [[nodiscard]] friend bool operator==(
        const ParticleInstance& lhs,
        const ParticleInstance& rhs
    ) = default;
};

class ParticleSimulation final {
public:
    ParticleSimulation(
        Configuration configuration,
        int objectId,
        // Must be stable and wallpaper-scoped. The caller should compose the
        // scene identity with the particle asset identity when relative asset
        // names are not globally unique.
        std::string_view assetIdentity,
        std::optional<std::uint64_t> explicitSeed = std::nullopt
    );

    ParticleSimulation(const ParticleSimulation&) = default;
    ParticleSimulation& operator=(const ParticleSimulation&) = default;
    ParticleSimulation(ParticleSimulation&&) noexcept = default;
    ParticleSimulation& operator=(ParticleSimulation&&) noexcept = default;
    ~ParticleSimulation() = default;

    // Applies a concrete per-frame configuration. Its topology must match the
    // construction configuration; mismatch is an explicit error.
    void advance(double elapsedSeconds, const Configuration& configuration);
    void advance(double elapsedSeconds);

    [[nodiscard]] const Configuration& configuration() const noexcept;
    [[nodiscard]] const std::vector<ParticleInstance>& particles() const noexcept;
    [[nodiscard]] double accumulatorSeconds() const noexcept;
    [[nodiscard]] double simulationTimeSeconds() const noexcept;
    [[nodiscard]] std::uint64_t seed() const noexcept;

    [[nodiscard]] static std::uint64_t stableSeed(
        int objectId,
        std::string_view assetIdentity
    ) noexcept;

private:
    struct EmitterState {
        double emissionAccumulator = 0.0;
        bool instantaneousEmitted = false;
        double elapsedSeconds = 0.0;
        double periodicElapsedSeconds = 0.0;
        double periodicDurationSeconds = 0.0;
        double periodicDelaySeconds = 0.0;
        std::uint32_t periodicRateEmissionCount = 0;
        bool periodicInitialized = false;
        bool periodicEmitting = false;
    };

    Configuration configuration_;
    std::vector<EmitterState> emitterStates_;
    struct InitializerState {
        std::uint64_t mapSequenceIndex = 0;
    };
    struct OperatorState {
        double turbulencePhase = 0.0;
        double turbulenceSpeed = 0.0;
        bool turbulenceInitialized = false;
    };
    std::vector<InitializerState> initializerStates_;
    std::vector<OperatorState> operatorStates_;
    std::vector<ParticleInstance> particles_;
    double accumulatorSeconds_ = 0.0;
    double simulationTimeSeconds_ = 0.0;
    std::uint64_t seed_ = 0;
    std::uint64_t randomState_ = 0;
    std::uint64_t randomIncrement_ = 0;
    std::uint64_t nextSpawnId_ = 1;

    void validateConfiguration(const Configuration& configuration) const;
    void requireMatchingTopology(const Configuration& configuration) const;
    void prewarm();
    void initializeOperatorStates();
    void applyConcreteConfiguration(const Configuration& configuration);
    void simulateStep(double stepSeconds);
    void emit(std::size_t emitterIndex, double stepSeconds);
    void spawn(const BoxRandomEmitter& emitter);
    void spawn(const SphereRandomEmitter& emitter);
    void initialize(ParticleInstance& particle);
    void applyOperators(double stepSeconds);
    void refreshConcreteParticleValues(ParticleInstance& particle) const;
    void reclaimDeadParticles();

    [[nodiscard]] std::size_t effectiveMaxCount() const;
    [[nodiscard]] std::optional<Vector3> controlPointPosition(int id) const;
    [[nodiscard]] double stableParticleUnit(
        std::uint64_t spawnId,
        std::uint64_t stream
    ) const noexcept;

    [[nodiscard]] std::uint32_t nextRandomBits() noexcept;
    [[nodiscard]] double randomUnit() noexcept;
    [[nodiscard]] double randomRange(double minimum, double maximum) noexcept;
    [[nodiscard]] Vector3 randomRange(Vector3 minimum, Vector3 maximum) noexcept;
};

}  // namespace we::scene::particle

#endif
