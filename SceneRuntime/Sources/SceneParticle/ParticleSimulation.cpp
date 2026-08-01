#include <SceneParticle/ParticleSimulation.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <unordered_set>

namespace we::scene::particle {
namespace {

constexpr double pi = 3.14159265358979323846264338327950288;
constexpr double twoPi = 2.0 * pi;
constexpr std::uint64_t fnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t fnvPrime = 1099511628211ULL;
constexpr int particleControlPointSlotCount = 8;
constexpr std::size_t defaultParticleCapacity = 1000;
// Prewarm is exact fixed-step simulation. Reject impractical authored work
// instead of silently truncating it or blocking scene construction indefinitely.
constexpr std::uint64_t maximumPrewarmSteps = 1'000'000;

[[nodiscard]] bool finite(double value) noexcept {
    return std::isfinite(value);
}

[[nodiscard]] bool finite(Vector3 value) noexcept {
    return finite(value.x) && finite(value.y) && finite(value.z);
}

[[nodiscard]] double checkedAdd(
    double first,
    double second,
    const char* name
) {
    const double result = first + second;
    if (!finite(result)) {
        throw std::overflow_error(std::string(name) + " overflowed");
    }
    return result;
}

[[nodiscard]] long double sizeExclusiveUpperBound() noexcept {
    return std::ldexp(
        1.0L,
        std::numeric_limits<std::size_t>::digits
    );
}

[[nodiscard]] double fixedStepRoundingTolerance(double step) noexcept {
    constexpr double ulpBudget = 128.0;
    return std::min(
        step * 0.25,
        step * std::numeric_limits<double>::epsilon() * ulpBudget
    );
}

[[nodiscard]] Vector3 add(Vector3 lhs, Vector3 rhs) noexcept {
    return {lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z};
}

[[nodiscard]] Vector3 subtract(Vector3 lhs, Vector3 rhs) noexcept {
    return {lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z};
}

[[nodiscard]] Vector3 multiply(Vector3 lhs, Vector3 rhs) noexcept {
    return {lhs.x * rhs.x, lhs.y * rhs.y, lhs.z * rhs.z};
}

[[nodiscard]] Vector3 multiply(Vector3 value, double scalar) noexcept {
    return {value.x * scalar, value.y * scalar, value.z * scalar};
}

[[nodiscard]] double length(Vector3 value) noexcept {
    return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
}

[[nodiscard]] double dot(Vector3 lhs, Vector3 rhs) noexcept {
    return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
}

[[nodiscard]] Vector3 cross(Vector3 lhs, Vector3 rhs) noexcept {
    return {
        lhs.y * rhs.z - lhs.z * rhs.y,
        lhs.z * rhs.x - lhs.x * rhs.z,
        lhs.x * rhs.y - lhs.y * rhs.x,
    };
}

[[nodiscard]] Vector3 normalized(Vector3 value) noexcept {
    const double magnitude = length(value);
    if (magnitude <= 0.0) {
        return {0.0, 1.0, 0.0};
    }
    return multiply(value, 1.0 / magnitude);
}

[[nodiscard]] Vector3 rotateAroundAxis(
    Vector3 value,
    Vector3 axis,
    double angle
) noexcept {
    axis = normalized(axis);
    const double cosine = std::cos(angle);
    const double sine = std::sin(angle);
    return add(
        add(multiply(value, cosine), multiply(cross(axis, value), sine)),
        multiply(axis, dot(axis, value) * (1.0 - cosine))
    );
}

constexpr std::array<int, 256> perlinPermutation = {
    151, 160, 137, 91, 90, 15, 131, 13, 201, 95, 96, 53, 194, 233, 7, 225,
    140, 36, 103, 30, 69, 142, 8, 99, 37, 240, 21, 10, 23, 190, 6, 148,
    247, 120, 234, 75, 0, 26, 197, 62, 94, 252, 219, 203, 117, 35, 11, 32,
    57, 177, 33, 88, 237, 149, 56, 87, 174, 20, 125, 136, 171, 168, 68,
    175, 74, 165, 71, 134, 139, 48, 27, 166, 77, 146, 158, 231, 83, 111,
    229, 122, 60, 211, 133, 230, 220, 105, 92, 41, 55, 46, 245, 40, 244,
    102, 143, 54, 65, 25, 63, 161, 1, 216, 80, 73, 209, 76, 132, 187,
    208, 89, 18, 169, 200, 196, 135, 130, 116, 188, 159, 86, 164, 100,
    109, 198, 173, 186, 3, 64, 52, 217, 226, 250, 124, 123, 5, 202, 38,
    147, 118, 126, 255, 82, 85, 212, 207, 206, 59, 227, 47, 16, 58, 17,
    182, 189, 28, 42, 223, 183, 170, 213, 119, 248, 152, 2, 44, 154,
    163, 70, 221, 153, 101, 155, 167, 43, 172, 9, 129, 22, 39, 253, 19,
    98, 108, 110, 79, 113, 224, 232, 178, 185, 112, 104, 218, 246, 97,
    228, 251, 34, 242, 193, 238, 210, 144, 12, 191, 179, 162, 241, 81,
    51, 145, 235, 249, 14, 239, 107, 49, 192, 214, 31, 181, 199, 106,
    157, 184, 84, 204, 176, 115, 121, 50, 45, 127, 4, 150, 254, 138,
    236, 205, 93, 222, 114, 67, 29, 24, 72, 243, 141, 128, 195, 78, 66,
    215, 61, 156, 180,
};

[[nodiscard]] int perlinAt(int index) noexcept {
    return perlinPermutation[static_cast<std::size_t>(index & 255)];
}

[[nodiscard]] double perlinGradient(
    int hash,
    double x,
    double y,
    double z
) noexcept {
    switch (hash & 0x0f) {
        case 0x0: return x + y;
        case 0x1: return -x + y;
        case 0x2: return x - y;
        case 0x3: return -x - y;
        case 0x4: return x + z;
        case 0x5: return -x + z;
        case 0x6: return x - z;
        case 0x7: return -x - z;
        case 0x8: return y + z;
        case 0x9: return -y + z;
        case 0xa: return y - z;
        case 0xb: return -y - z;
        case 0xc: return y + x;
        case 0xd: return -y + z;
        case 0xe: return y - x;
        case 0xf: return -y - z;
        default: return 0.0;
    }
}

[[nodiscard]] double perlinEase(double value) noexcept {
    return value * value * value *
        (value * (value * 6.0 - 15.0) + 10.0);
}

[[nodiscard]] double perlinLerp(
    double amount,
    double first,
    double second
) noexcept {
    return first + amount * (second - first);
}

[[nodiscard]] double perlinNoise(
    double x,
    double y,
    double z
) noexcept {
    const double floorX = std::floor(x);
    const double floorY = std::floor(y);
    const double floorZ = std::floor(z);
    const int cellX = static_cast<int>(floorX) & 255;
    const int cellY = static_cast<int>(floorY) & 255;
    const int cellZ = static_cast<int>(floorZ) & 255;
    x -= floorX;
    y -= floorY;
    z -= floorZ;

    const double u = perlinEase(x);
    const double v = perlinEase(y);
    const double w = perlinEase(z);
    const int a = perlinAt(cellX) + cellY;
    const int aa = perlinAt(a) + cellZ;
    const int ab = perlinAt(a + 1) + cellZ;
    const int b = perlinAt(cellX + 1) + cellY;
    const int ba = perlinAt(b) + cellZ;
    const int bb = perlinAt(b + 1) + cellZ;

    return perlinLerp(
        w,
        perlinLerp(
            v,
            perlinLerp(
                u,
                perlinGradient(perlinAt(aa), x, y, z),
                perlinGradient(perlinAt(ba), x - 1.0, y, z)
            ),
            perlinLerp(
                u,
                perlinGradient(perlinAt(ab), x, y - 1.0, z),
                perlinGradient(perlinAt(bb), x - 1.0, y - 1.0, z)
            )
        ),
        perlinLerp(
            v,
            perlinLerp(
                u,
                perlinGradient(perlinAt(aa + 1), x, y, z - 1.0),
                perlinGradient(perlinAt(ba + 1), x - 1.0, y, z - 1.0)
            ),
            perlinLerp(
                u,
                perlinGradient(perlinAt(ab + 1), x, y - 1.0, z - 1.0),
                perlinGradient(perlinAt(bb + 1), x - 1.0, y - 1.0, z - 1.0)
            )
        )
    );
}

[[nodiscard]] Vector3 perlinNoiseVector(Vector3 point) noexcept {
    return {
        perlinNoise(point.x, point.y, point.z),
        perlinNoise(point.x + 89.2, point.y + 33.1, point.z + 57.3),
        perlinNoise(point.x + 100.3, point.y + 120.1, point.z + 142.2),
    };
}

[[nodiscard]] Vector3 curlNoise(Vector3 point) noexcept {
    constexpr double epsilon = 1.0e-4;
    const Vector3 dx{epsilon, 0.0, 0.0};
    const Vector3 dy{0.0, epsilon, 0.0};
    const Vector3 dz{0.0, 0.0, epsilon};
    const Vector3 x0 = perlinNoiseVector({
        point.x - dx.x, point.y - dx.y, point.z - dx.z,
    });
    const Vector3 x1 = perlinNoiseVector({
        point.x + dx.x, point.y + dx.y, point.z + dx.z,
    });
    const Vector3 y0 = perlinNoiseVector({
        point.x - dy.x, point.y - dy.y, point.z - dy.z,
    });
    const Vector3 y1 = perlinNoiseVector({
        point.x + dy.x, point.y + dy.y, point.z + dy.z,
    });
    const Vector3 z0 = perlinNoiseVector({
        point.x - dz.x, point.y - dz.y, point.z - dz.z,
    });
    const Vector3 z1 = perlinNoiseVector({
        point.x + dz.x, point.y + dz.y, point.z + dz.z,
    });
    const double scale = 1.0 / (2.0 * epsilon);
    return {
        ((y1.z - y0.z) - (z1.y - z0.y)) * scale,
        ((z1.x - z0.x) - (x1.z - x0.z)) * scale,
        ((x1.y - x0.y) - (y1.x - y0.x)) * scale,
    };
}

[[nodiscard]] double interpolate(double first, double second, double unit) noexcept {
    return first + unit * (second - first);
}

// The Linux runtime samples oscillator phase from [phaseMin, phaseMax + 2π]
// (and swaps reversed authored bounds before sampling).  Keeping the sample
// as a stable unit lets copied simulations remain deterministic while
// preserving that range exactly.
[[nodiscard]] double oscillatorRandomRange(
    double minimum,
    double maximum,
    double unit,
    bool includeExtraCycle = false
) noexcept {
    if (includeExtraCycle) {
        maximum += twoPi;
    }
    if (maximum < minimum) {
        std::swap(minimum, maximum);
    }
    return interpolate(minimum, maximum, unit);
}

[[nodiscard]] Vector3 interpolate(
    Vector3 first,
    Vector3 second,
    Vector3 unit
) noexcept {
    return {
        interpolate(first.x, second.x, unit.x),
        interpolate(first.y, second.y, unit.y),
        interpolate(first.z, second.z, unit.z),
    };
}

[[nodiscard]] double wrappedAngle(double value) noexcept {
    return std::remainder(value, 2.0 * pi);
}

[[nodiscard]] double fadeValue(
    double life,
    double startTime,
    double endTime,
    double startValue,
    double endValue
) noexcept {
    if (life <= startTime) {
        return startValue;
    }
    if (life >= endTime) {
        return endValue;
    }
    const double amount = (life - startTime) / (endTime - startTime);
    return startValue + amount * (endValue - startValue);
}

void requireFinite(double value, const char* name) {
    if (!finite(value)) {
        throw std::invalid_argument(std::string(name) + " must be finite");
    }
}

void requireFinite(Vector3 value, const char* name) {
    if (!finite(value)) {
        throw std::invalid_argument(std::string(name) + " must be finite");
    }
}

[[nodiscard]] const ParticleInstanceOverrides& concreteOverrides(
    const Configuration& configuration
) noexcept {
    return configuration.overrides;
}

[[nodiscard]] bool sameTopology(
    const std::vector<Emitter>& lhs,
    const std::vector<Emitter>& rhs
) noexcept {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t index = 0; index < lhs.size(); ++index) {
        if (lhs[index].index() != rhs[index].index()) {
            return false;
        }
    }
    return true;
}

template <typename Variant>
[[nodiscard]] bool sameTopology(
    const std::vector<Variant>& lhs,
    const std::vector<Variant>& rhs
) noexcept {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t index = 0; index < lhs.size(); ++index) {
        if (lhs[index].index() != rhs[index].index()) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool sameControlPointTopology(
    const std::vector<ControlPoint>& lhs,
    const std::vector<ControlPoint>& rhs
) noexcept {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t index = 0; index < lhs.size(); ++index) {
        if (lhs[index].id != rhs[index].id ||
            lhs[index].linkedToPointer != rhs[index].linkedToPointer) {
            return false;
        }
    }
    return true;
}

}  // namespace

double ParticleInstance::lifetimePosition() const noexcept {
    return lifetime > 0.0 ? age / lifetime : 1.0;
}

bool ParticleInstance::alive() const noexcept {
    return age < lifetime;
}

ParticleSimulation::ParticleSimulation(
    Configuration configuration,
    int objectId,
    std::string_view assetIdentity,
    std::optional<std::uint64_t> explicitSeed
)
    : configuration_(std::move(configuration)),
      emitterStates_(configuration_.emitters.size()),
      initializerStates_(configuration_.initializers.size()),
      operatorStates_(configuration_.operators.size()),
      seed_(explicitSeed.value_or(stableSeed(objectId, assetIdentity))) {
    validateConfiguration(configuration_);
    particles_.reserve(configuration_.maxCount);

    randomState_ = 0;
    randomIncrement_ = 0xda3e39cb94b95bdbULL;
    (void)nextRandomBits();
    randomState_ += seed_;
    (void)nextRandomBits();
    initializeOperatorStates();
    prewarm();
}

void ParticleSimulation::advance(
    double elapsedSeconds,
    const Configuration& configuration
) {
    requireFinite(elapsedSeconds, "Particle elapsed time");
    if (elapsedSeconds < 0.0) {
        throw std::invalid_argument("Particle elapsed time must not be negative");
    }
    validateConfiguration(configuration);
    requireMatchingTopology(configuration);
    applyConcreteConfiguration(configuration);
    if (elapsedSeconds == 0.0) {
        return;
    }

    const double nextAccumulator = accumulatorSeconds_ + elapsedSeconds;
    if (!finite(nextAccumulator)) {
        throw std::overflow_error("Particle fixed-step accumulator overflowed");
    }
    accumulatorSeconds_ = nextAccumulator;
    const double step = configuration_.fixedStepSeconds;
    const double tolerance = fixedStepRoundingTolerance(step);
    while (accumulatorSeconds_ >= step ||
           step - accumulatorSeconds_ <= tolerance) {
        simulateStep(step);
        accumulatorSeconds_ -= step;
    }
    // Subtracting a fixed step can leave a signed ulp-sized residue (for
    // example, 0.3 - 3 * 0.1 becomes -2.77e-17).  The accumulator represents
    // elapsed time that has not yet been simulated, so both signs are the
    // same exact fixed-step boundary and must serialize identically.  Only
    // canonicalize within the already-established numerical tolerance; a
    // larger negative value remains an explicit invariant violation.
    if (std::abs(accumulatorSeconds_) <= tolerance) {
        accumulatorSeconds_ = 0.0;
    }
}

void ParticleSimulation::advance(double elapsedSeconds) {
    advance(elapsedSeconds, configuration_);
}

const Configuration& ParticleSimulation::configuration() const noexcept {
    return configuration_;
}

const std::vector<ParticleInstance>& ParticleSimulation::particles() const noexcept {
    return particles_;
}

double ParticleSimulation::accumulatorSeconds() const noexcept {
    return accumulatorSeconds_;
}

double ParticleSimulation::simulationTimeSeconds() const noexcept {
    return simulationTimeSeconds_;
}

std::uint64_t ParticleSimulation::seed() const noexcept {
    return seed_;
}

std::uint64_t ParticleSimulation::stableSeed(
    int objectId,
    std::string_view assetIdentity
) noexcept {
    std::uint64_t hash = fnvOffset;
    const auto append = [&hash](std::uint8_t byte) {
        hash ^= byte;
        hash *= fnvPrime;
    };
    for (const unsigned char character : assetIdentity) {
        append(character);
    }
    append(0xff);
    const std::uint32_t idBits = static_cast<std::uint32_t>(objectId);
    for (unsigned int shift = 0; shift < 32; shift += 8) {
        append(static_cast<std::uint8_t>((idBits >> shift) & 0xffU));
    }
    return hash;
}

void ParticleSimulation::validateConfiguration(
    const Configuration& configuration
) const {
    requireFinite(configuration.fixedStepSeconds, "Particle fixed step");
    if (configuration.fixedStepSeconds <= 0.0) {
        throw std::invalid_argument("Particle fixed step must be greater than zero");
    }
    requireFinite(configuration.startTime, "Particle start time");
    if (configuration.startTime < 0.0) {
        throw std::invalid_argument("Particle start time must not be negative");
    }
    const ParticleInstanceOverrides& overrides = concreteOverrides(configuration);
    requireFinite(overrides.alpha, "Particle alpha override");
    requireFinite(overrides.size, "Particle size override");
    requireFinite(overrides.lifetime, "Particle lifetime override");
    requireFinite(overrides.rate, "Particle rate override");
    requireFinite(overrides.speed, "Particle speed override");
    requireFinite(overrides.count, "Particle count override");
    requireFinite(overrides.color, "Particle color override");
    requireFinite(overrides.colorMultiplier, "Particle color multiplier override");
    if (overrides.count < 0.0) {
        throw std::invalid_argument(
            "Particle count override must be nonnegative"
        );
    }
    const long double adjustedCount =
        static_cast<long double>(configuration.maxCount) * overrides.count;
    if (!std::isfinite(adjustedCount) ||
        adjustedCount >= sizeExclusiveUpperBound()) {
        throw std::invalid_argument("Particle count override exceeds size_t capacity");
    }

    std::unordered_set<int> controlPointIds;
    for (const ControlPoint& controlPoint : configuration.controlPoints) {
        if (controlPoint.id < 0 ||
            controlPoint.id >= particleControlPointSlotCount) {
            throw std::invalid_argument(
                "Particle control point id must be between 0 and 7"
            );
        }
        if (!controlPointIds.insert(controlPoint.id).second) {
            throw std::invalid_argument("Particle control point id is duplicated");
        }
        requireFinite(controlPoint.position, "Particle control point position");
    }

    for (const Emitter& emitter : configuration.emitters) {
        std::visit([](const auto& concrete) {
            const EmitterBase& base = concrete.base;
            requireFinite(base.directions, "Particle emitter directions");
            requireFinite(base.distanceMin, "Particle emitter minimum distance");
            requireFinite(base.distanceMax, "Particle emitter maximum distance");
            requireFinite(base.origin, "Particle emitter origin");
            requireFinite(base.rate, "Particle emitter rate");
            requireFinite(base.delay, "Particle emitter delay");
            requireFinite(base.duration, "Particle emitter duration");
            requireFinite(
                base.minPeriodicDelay,
                "Particle emitter minimum periodic delay"
            );
            requireFinite(
                base.maxPeriodicDelay,
                "Particle emitter maximum periodic delay"
            );
            requireFinite(
                base.minPeriodicDuration,
                "Particle emitter minimum periodic duration"
            );
            requireFinite(
                base.maxPeriodicDuration,
                "Particle emitter maximum periodic duration"
            );
            if constexpr (std::is_same_v<std::decay_t<decltype(concrete)>, SphereRandomEmitter>) {
                requireFinite(concrete.sign, "Particle sphere emitter sign");
                requireFinite(concrete.speedMin, "Particle sphere emitter minimum speed");
                requireFinite(concrete.speedMax, "Particle sphere emitter maximum speed");
            }
        }, emitter);
    }

    for (const Initializer& initializer : configuration.initializers) {
        std::visit([](const auto& concrete) {
            using T = std::decay_t<decltype(concrete)>;
            if constexpr (std::is_same_v<T, TurbulentVelocityRandomInitializer>) {
                requireFinite(concrete.speedMinimum, "Particle turbulent minimum speed");
                requireFinite(concrete.speedMaximum, "Particle turbulent maximum speed");
                requireFinite(concrete.scale, "Particle turbulent scale");
                requireFinite(concrete.offset, "Particle turbulent offset");
                requireFinite(concrete.forward, "Particle turbulent forward direction");
                requireFinite(concrete.timeScale, "Particle turbulent time scale");
                requireFinite(concrete.phaseMinimum, "Particle turbulent minimum phase");
                requireFinite(concrete.phaseMaximum, "Particle turbulent maximum phase");
                requireFinite(concrete.right, "Particle turbulent right direction");
            } else if constexpr (std::is_same_v<T, MapSequenceAroundControlPointInitializer>) {
                requireFinite(concrete.speedMinimum, "Particle map sequence minimum speed");
                requireFinite(concrete.speedMaximum, "Particle map sequence maximum speed");
            } else if constexpr (
                std::is_same_v<T, ColorRandomInitializer> ||
                std::is_same_v<T, VelocityRandomInitializer> ||
                std::is_same_v<T, RotationRandomInitializer>
            ) {
                requireFinite(concrete.minimum, "Particle initializer minimum");
                requireFinite(concrete.maximum, "Particle initializer maximum");
            } else {
                requireFinite(concrete.minimum, "Particle initializer minimum");
                requireFinite(concrete.maximum, "Particle initializer maximum");
                if constexpr (std::is_same_v<T, SizeRandomInitializer>) {
                    requireFinite(concrete.exponent, "Particle size exponent");
                } else if constexpr (std::is_same_v<T, AngularVelocityRandomInitializer>) {
                    requireFinite(concrete.exponent, "Particle angular velocity exponent");
                }
            }
        }, initializer);
        if (const auto* mapSequence = std::get_if<
                MapSequenceAroundControlPointInitializer
            >(&initializer); mapSequence != nullptr && mapSequence->count <= 0) {
            throw std::invalid_argument(
                "Particle map sequence count must be greater than zero"
            );
        }
    }

    for (const Operator& operation : configuration.operators) {
        std::visit([](const auto& concrete) {
            using T = std::decay_t<decltype(concrete)>;
            if constexpr (std::is_same_v<T, MovementOperator>) {
                requireFinite(concrete.drag, "Particle movement drag");
                requireFinite(concrete.gravity, "Particle movement gravity");
            } else if constexpr (std::is_same_v<T, AlphaFadeOperator>) {
                requireFinite(concrete.fadeInTime, "Particle alpha fade-in time");
                requireFinite(concrete.fadeOutTime, "Particle alpha fade-out time");
            } else if constexpr (std::is_same_v<T, AngularMovementOperator>) {
                requireFinite(concrete.drag, "Particle angular movement drag");
                requireFinite(concrete.force, "Particle angular movement force");
            } else if constexpr (std::is_same_v<T, OscillatePositionOperator>) {
                requireFinite(concrete.frequencyMinimum, "Particle position oscillator minimum frequency");
                requireFinite(concrete.frequencyMaximum, "Particle position oscillator maximum frequency");
                requireFinite(concrete.scaleMinimum, "Particle position oscillator minimum scale");
                requireFinite(concrete.scaleMaximum, "Particle position oscillator maximum scale");
                requireFinite(concrete.phaseMinimum, "Particle position oscillator minimum phase");
                requireFinite(concrete.phaseMaximum, "Particle position oscillator maximum phase");
                requireFinite(concrete.mask, "Particle position oscillator mask");
            } else if constexpr (std::is_same_v<T, OscillateAlphaOperator>) {
                requireFinite(concrete.frequencyMinimum, "Particle alpha oscillator minimum frequency");
                requireFinite(concrete.frequencyMaximum, "Particle alpha oscillator maximum frequency");
                requireFinite(concrete.scaleMinimum, "Particle alpha oscillator minimum scale");
                requireFinite(concrete.scaleMaximum, "Particle alpha oscillator maximum scale");
                requireFinite(concrete.phaseMinimum, "Particle alpha oscillator minimum phase");
                requireFinite(concrete.phaseMaximum, "Particle alpha oscillator maximum phase");
            } else if constexpr (std::is_same_v<T, OscillateSizeOperator>) {
                requireFinite(concrete.frequencyMinimum, "Particle size oscillator minimum frequency");
                requireFinite(concrete.frequencyMaximum, "Particle size oscillator maximum frequency");
                requireFinite(concrete.scaleMinimum, "Particle size oscillator minimum scale");
                requireFinite(concrete.scaleMaximum, "Particle size oscillator maximum scale");
                requireFinite(concrete.phaseMinimum, "Particle size oscillator minimum phase");
                requireFinite(concrete.phaseMaximum, "Particle size oscillator maximum phase");
            } else if constexpr (std::is_same_v<T, ControlPointAttractOperator>) {
                requireFinite(concrete.origin, "Particle attract origin");
                requireFinite(concrete.scale, "Particle attract scale");
                requireFinite(concrete.threshold, "Particle attract threshold");
            } else if constexpr (std::is_same_v<T, SizeChangeOperator>) {
                requireFinite(concrete.startTime, "Particle size-change start time");
                requireFinite(concrete.endTime, "Particle size-change end time");
                requireFinite(concrete.startValue, "Particle size-change start value");
                requireFinite(concrete.endValue, "Particle size-change end value");
            } else if constexpr (std::is_same_v<T, AlphaChangeOperator>) {
                requireFinite(concrete.startTime, "Particle alpha-change start time");
                requireFinite(concrete.endTime, "Particle alpha-change end time");
                requireFinite(concrete.startValue, "Particle alpha-change start value");
                requireFinite(concrete.endValue, "Particle alpha-change end value");
            } else if constexpr (std::is_same_v<T, ColorChangeOperator>) {
                requireFinite(concrete.startTime, "Particle color-change start time");
                requireFinite(concrete.endTime, "Particle color-change end time");
                requireFinite(concrete.startValue, "Particle color-change start value");
                requireFinite(concrete.endValue, "Particle color-change end value");
            } else if constexpr (std::is_same_v<T, TurbulenceOperator>) {
                requireFinite(concrete.scale, "Particle turbulence scale");
                requireFinite(concrete.speedMinimum, "Particle turbulence minimum speed");
                requireFinite(concrete.speedMaximum, "Particle turbulence maximum speed");
                requireFinite(concrete.timeScale, "Particle turbulence time scale");
                requireFinite(concrete.mask, "Particle turbulence mask");
                requireFinite(concrete.phaseMinimum, "Particle turbulence minimum phase");
                requireFinite(concrete.phaseMaximum, "Particle turbulence maximum phase");
            } else if constexpr (std::is_same_v<T, VortexOperator>) {
                requireFinite(concrete.axis, "Particle vortex axis");
                requireFinite(concrete.offset, "Particle vortex offset");
                requireFinite(concrete.distanceInner, "Particle vortex inner distance");
                requireFinite(concrete.distanceOuter, "Particle vortex outer distance");
                requireFinite(concrete.speedInner, "Particle vortex inner speed");
                requireFinite(concrete.speedOuter, "Particle vortex outer speed");
                requireFinite(concrete.centerForce, "Particle vortex center force");
                requireFinite(concrete.ringRadius, "Particle vortex ring radius");
                requireFinite(concrete.ringWidth, "Particle vortex ring width");
                requireFinite(concrete.ringPullDistance, "Particle vortex ring pull distance");
                requireFinite(concrete.ringPullForce, "Particle vortex ring pull force");
            }
        }, operation);
    }
}

void ParticleSimulation::requireMatchingTopology(
    const Configuration& configuration
) const {
    if (configuration.maxCount != configuration_.maxCount ||
        configuration.fixedStepSeconds != configuration_.fixedStepSeconds ||
        configuration.startTime != configuration_.startTime ||
        configuration.flags != configuration_.flags ||
        !sameTopology(configuration.emitters, configuration_.emitters) ||
        !sameTopology(configuration.initializers, configuration_.initializers) ||
        !sameTopology(configuration.operators, configuration_.operators) ||
        !sameControlPointTopology(
            configuration.controlPoints,
            configuration_.controlPoints
        )) {
        throw std::invalid_argument(
            "Particle configuration topology changed after simulation construction"
        );
    }
}

void ParticleSimulation::prewarm() {
    if (configuration_.startTime == 0.0) {
        return;
    }

    const double step = configuration_.fixedStepSeconds;
    long double quotient =
        static_cast<long double>(configuration_.startTime) /
        static_cast<long double>(step);
    const long double nearest = std::round(quotient);
    const long double quotientTolerance =
        std::numeric_limits<long double>::epsilon() *
        std::max(1.0L, std::abs(quotient)) * 128.0L;
    if (std::abs(quotient - nearest) <= quotientTolerance) {
        quotient = nearest;
    }
    const long double stepCountValue = std::floor(quotient);
    if (!std::isfinite(stepCountValue) ||
        stepCountValue >= std::ldexp(
            1.0L,
            std::numeric_limits<std::uint64_t>::digits
        )) {
        throw std::overflow_error("Particle prewarm step count overflowed");
    }
    const std::uint64_t stepCount = static_cast<std::uint64_t>(stepCountValue);
    if (stepCount > maximumPrewarmSteps) {
        throw std::invalid_argument(
            "Particle prewarm requires too many fixed steps"
        );
    }
    for (std::uint64_t index = 0; index < stepCount; ++index) {
        simulateStep(step);
    }
    accumulatorSeconds_ = std::fma(
        -static_cast<double>(stepCount),
        step,
        configuration_.startTime
    );
}

void ParticleSimulation::initializeOperatorStates() {
    for (std::size_t index = 0; index < configuration_.operators.size(); ++index) {
        const auto* turbulence = std::get_if<TurbulenceOperator>(
            &configuration_.operators[index]
        );
        if (turbulence == nullptr) {
            continue;
        }
        OperatorState& state = operatorStates_[index];
        // Linux samples these two values once when the operator is built,
        // rather than once per particle or once per frame.  Keep that
        // lifetime explicit so concrete property updates do not resample it.
        state.turbulencePhase = randomRange(
            turbulence->phaseMinimum,
            turbulence->phaseMaximum
        );
        state.turbulenceSpeed = randomRange(
            turbulence->speedMinimum,
            turbulence->speedMaximum
        );
        state.turbulenceInitialized = true;
    }
}

void ParticleSimulation::applyConcreteConfiguration(
    const Configuration& configuration
) {
    configuration_ = configuration;
    for (ParticleInstance& particle : particles_) {
        refreshConcreteParticleValues(particle);
    }
    // Operators derive the visible values from the simulation state. Reapply
    // them at zero delta after a concrete override update so a sub-step frame
    // cannot expose the unfiltered base values.
    applyOperators(0.0);
    reclaimDeadParticles();
    const std::size_t maximum = effectiveMaxCount();
    if (particles_.size() > maximum) {
        particles_.resize(maximum);
    }
}

void ParticleSimulation::simulateStep(double stepSeconds) {
    const std::size_t maximum = effectiveMaxCount();
    if (particles_.size() > maximum) {
        particles_.resize(maximum);
    }
    for (std::size_t index = 0; index < configuration_.emitters.size(); ++index) {
        emit(index, stepSeconds);
    }
    for (ParticleInstance& particle : particles_) {
        refreshConcreteParticleValues(particle);
        particle.age = checkedAdd(
            particle.age,
            stepSeconds,
            "Particle age"
        );
    }
    applyOperators(stepSeconds);
    for (ParticleInstance& particle : particles_) {
        particle.velocity = multiply(
            particle.simulationVelocity,
            concreteOverrides(configuration_).speed
        );
        particle.angularVelocity = multiply(
            particle.simulationAngularVelocity,
            concreteOverrides(configuration_).speed
        );
    }
    simulationTimeSeconds_ = checkedAdd(
        simulationTimeSeconds_,
        stepSeconds,
        "Particle simulation time"
    );
    reclaimDeadParticles();
}

void ParticleSimulation::emit(std::size_t emitterIndex, double stepSeconds) {
    EmitterState& state = emitterStates_[emitterIndex];
    const Emitter& emitter = configuration_.emitters[emitterIndex];
    const EmitterBase& base = std::visit(
        [](const auto& concrete) -> const EmitterBase& { return concrete.base; },
        emitter
    );

    const double emitterTime = state.elapsedSeconds;
    state.elapsedSeconds = checkedAdd(
        state.elapsedSeconds,
        stepSeconds,
        "Particle emitter elapsed time"
    );
    const double tolerance = fixedStepRoundingTolerance(stepSeconds);
    const auto reached = [tolerance](double elapsed, double target) noexcept {
        return elapsed >= target || target - elapsed <= tolerance;
    };
    if (!reached(emitterTime, base.delay)) {
        return;
    }
    const double activeElapsed = emitterTime > base.delay
        ? emitterTime - base.delay
        : 0.0;
    if (base.duration > 0.0 && reached(activeElapsed, base.duration)) {
        return;
    }

    if ((base.flags & 4U) != 0U) {
        if (!state.periodicInitialized) {
            state.periodicInitialized = true;
            state.periodicEmitting = true;
            state.periodicDurationSeconds = randomRange(
                base.minPeriodicDuration,
                base.maxPeriodicDuration
            );
            state.periodicRateEmissionCount = 0;
        } else if (state.periodicEmitting &&
                   reached(
                       state.periodicElapsedSeconds,
                       state.periodicDurationSeconds
                   )) {
            state.periodicEmitting = false;
            state.periodicElapsedSeconds = 0.0;
            state.periodicDelaySeconds = randomRange(
                base.minPeriodicDelay,
                base.maxPeriodicDelay
            );
        } else if (!state.periodicEmitting &&
                   reached(
                       state.periodicElapsedSeconds,
                       state.periodicDelaySeconds
                   )) {
            state.periodicEmitting = true;
            state.periodicElapsedSeconds = 0.0;
            state.periodicDurationSeconds = randomRange(
                base.minPeriodicDuration,
                base.maxPeriodicDuration
            );
            state.periodicRateEmissionCount = 0;
        }

        state.periodicElapsedSeconds = checkedAdd(
            state.periodicElapsedSeconds,
            stepSeconds,
            "Particle periodic emitter elapsed time"
        );
        if (!state.periodicEmitting) {
            return;
        }
    }

    const std::size_t maximum = effectiveMaxCount();
    const auto spawnRequested = [this, &emitter, maximum](
        std::size_t requested
    ) -> std::size_t {
        const std::size_t available = particles_.size() < maximum
            ? maximum - particles_.size()
            : 0U;
        const std::size_t actual = std::min(requested, available);
        for (std::size_t index = 0; index < actual; ++index) {
            std::visit([this](const auto& concrete) { spawn(concrete); }, emitter);
        }
        return actual;
    };

    if (base.instantaneous > 0 && !state.instantaneousEmitted) {
        state.instantaneousEmitted = true;
        (void)spawnRequested(static_cast<std::size_t>(base.instantaneous));
    }
    const double rate = base.rate * concreteOverrides(configuration_).rate;
    if (rate > 0.0) {
        state.emissionAccumulator = checkedAdd(
            state.emissionAccumulator,
            stepSeconds * rate,
            "Particle emitter accumulator"
        );
        const double whole = std::floor(state.emissionAccumulator);
        std::size_t rateCount = 0;
        if (static_cast<long double>(whole) >= sizeExclusiveUpperBound()) {
            rateCount = maximum;
        } else {
            rateCount = std::min(
                static_cast<std::size_t>(whole),
                maximum
            );
        }
        state.emissionAccumulator -= whole;
        if ((base.flags & 2U) != 0U && rateCount > 1) {
            rateCount = 1U;
        }
        if ((base.flags & 4U) != 0U &&
            base.maxToEmitPerPeriod > 0U) {
            const std::uint64_t remaining =
                state.periodicRateEmissionCount >= base.maxToEmitPerPeriod
                ? 0U
                : static_cast<std::uint64_t>(base.maxToEmitPerPeriod) -
                    state.periodicRateEmissionCount;
            rateCount = std::min(
                rateCount,
                static_cast<std::size_t>(remaining)
            );
        }
        const std::size_t emitted = spawnRequested(rateCount);
        if ((base.flags & 4U) != 0U &&
            base.maxToEmitPerPeriod > 0U) {
            state.periodicRateEmissionCount +=
                static_cast<std::uint32_t>(emitted);
        }
    }
}

void ParticleSimulation::spawn(const BoxRandomEmitter& emitter) {
    Vector3 origin = emitter.base.origin;
    origin.y = -origin.y;
    if (const auto controlPoint = controlPointPosition(emitter.base.controlPoint)) {
        origin = add(origin, *controlPoint);
    }
    Vector3 directions = emitter.base.directions;
    directions.y = -directions.y;

    Vector3 offset;
    double* components[] = {&offset.x, &offset.y, &offset.z};
    const double minimum[] = {
        emitter.base.distanceMin.x,
        emitter.base.distanceMin.y,
        emitter.base.distanceMin.z,
    };
    const double maximum[] = {
        emitter.base.distanceMax.x,
        emitter.base.distanceMax.y,
        emitter.base.distanceMax.z,
    };
    for (std::size_t axis = 0; axis < 3; ++axis) {
        double distance = randomRange(minimum[axis], maximum[axis]);
        if (randomUnit() < 0.5) {
            distance = -distance;
        }
        *components[axis] = distance;
    }
    offset = multiply(offset, directions);

    ParticleInstance particle;
    particle.spawnId = nextSpawnId_++;
    particle.position = add(origin, offset);
    initialize(particle);
    particles_.push_back(particle);
}

void ParticleSimulation::spawn(const SphereRandomEmitter& emitter) {
    Vector3 origin = emitter.base.origin;
    origin.y = -origin.y;
    if (const auto controlPoint = controlPointPosition(emitter.base.controlPoint)) {
        origin = add(origin, *controlPoint);
    }
    Vector3 offset;
    const double minimumRadius = emitter.base.distanceMin.x;
    const double maximumRadius = emitter.base.distanceMax.x;

    if ((configuration_.flags & 4U) != 0U) {
        const double angle = randomRange(0.0, 2.0 * pi);
        const double cosine = randomRange(-1.0, 1.0);
        const double sine = std::sqrt(std::max(0.0, 1.0 - cosine * cosine));
        const double radius = std::cbrt(randomRange(
            minimumRadius * minimumRadius * minimumRadius,
            maximumRadius * maximumRadius * maximumRadius
        ));
        offset = {
            radius * sine * std::cos(angle),
            radius * sine * std::sin(angle),
            radius * cosine,
        };
    } else {
        const double angle = randomRange(0.0, 2.0 * pi);
        const double radius = std::sqrt(randomRange(
            minimumRadius * minimumRadius,
            maximumRadius * maximumRadius
        ));
        offset = {
            radius * std::cos(angle),
            radius * std::sin(angle),
            randomRange(-std::abs(maximumRadius), std::abs(maximumRadius)),
        };
    }
    offset = multiply(offset, emitter.base.directions);

    double* components[] = {&offset.x, &offset.y, &offset.z};
    const double signs[] = {emitter.sign.x, emitter.sign.y, emitter.sign.z};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (signs[axis] == 1.0) {
            *components[axis] = std::abs(*components[axis]);
        } else if (signs[axis] == -1.0) {
            *components[axis] = -std::abs(*components[axis]);
        }
    }

    ParticleInstance particle;
    particle.spawnId = nextSpawnId_++;
    particle.position = add(origin, offset);
    if (emitter.speedMax > 0.0 || emitter.speedMin != 0.0) {
        particle.simulationVelocity = multiply(
            normalized(offset),
            randomRange(emitter.speedMin, emitter.speedMax)
        );
    }
    initialize(particle);
    particles_.push_back(particle);
}

void ParticleSimulation::initialize(ParticleInstance& particle) {
    particle.initialColor = {1.0, 1.0, 1.0};
    particle.initialAlpha = 1.0;
    particle.initialSize = 20.0;
    particle.initialLifetime = 1.0;
    particle.age = 0.0;
    particle.randomFrameUnit = stableParticleUnit(
        particle.spawnId,
        0x72616e646f6d6672ULL
    );
    particle.oscillatorStates.resize(configuration_.operators.size());
    for (std::size_t index = 0;
         index < particle.oscillatorStates.size();
         ++index) {
        ParticleOscillatorState& state = particle.oscillatorStates[index];
        const std::uint64_t stream =
            0x6f7363696c6c0000ULL + static_cast<std::uint64_t>(index) * 16ULL;
        state.frequencyUnit = {
            stableParticleUnit(particle.spawnId, stream + 0ULL),
            stableParticleUnit(particle.spawnId, stream + 1ULL),
            stableParticleUnit(particle.spawnId, stream + 2ULL),
        };
        state.scaleUnit = {
            stableParticleUnit(particle.spawnId, stream + 3ULL),
            stableParticleUnit(particle.spawnId, stream + 4ULL),
            stableParticleUnit(particle.spawnId, stream + 5ULL),
        };
        state.phaseUnit = {
            stableParticleUnit(particle.spawnId, stream + 6ULL),
            stableParticleUnit(particle.spawnId, stream + 7ULL),
            stableParticleUnit(particle.spawnId, stream + 8ULL),
        };
    }

    for (std::size_t initializerIndex = 0;
         initializerIndex < configuration_.initializers.size();
         ++initializerIndex) {
        const Initializer& initializer = configuration_.initializers[initializerIndex];
        std::visit([this, &particle, initializerIndex](const auto& concrete) {
            using T = std::decay_t<decltype(concrete)>;
            if constexpr (std::is_same_v<T, LifetimeRandomInitializer>) {
                particle.initialLifetime = randomRange(
                    concrete.minimum,
                    concrete.maximum
                );
            } else if constexpr (std::is_same_v<T, SizeRandomInitializer>) {
                const double amount = std::pow(randomUnit(), concrete.exponent);
                particle.initialSize = interpolate(
                    concrete.minimum,
                    concrete.maximum,
                    amount
                ) / 2.0;
            } else if constexpr (std::is_same_v<T, ColorRandomInitializer>) {
                particle.initialColor = randomRange(
                    concrete.minimum,
                    concrete.maximum
                );
            } else if constexpr (std::is_same_v<T, AlphaRandomInitializer>) {
                particle.initialAlpha = randomRange(
                    concrete.minimum,
                    concrete.maximum
                );
            } else if constexpr (std::is_same_v<T, VelocityRandomInitializer>) {
                Vector3 velocity = randomRange(concrete.minimum, concrete.maximum);
                velocity.y = -velocity.y;
                particle.simulationVelocity = add(
                    particle.simulationVelocity,
                    velocity
                );
            } else if constexpr (std::is_same_v<T, RotationRandomInitializer>) {
                particle.rotation = multiply(
                    randomRange(concrete.minimum, concrete.maximum),
                    concreteOverrides(configuration_).speed
                );
            } else if constexpr (std::is_same_v<T, AngularVelocityRandomInitializer>) {
                const Vector3 unit{
                    std::pow(randomUnit(), concrete.exponent),
                    std::pow(randomUnit(), concrete.exponent),
                    std::pow(randomUnit(), concrete.exponent),
                };
                particle.simulationAngularVelocity = interpolate(
                    concrete.minimum,
                    concrete.maximum,
                    unit
                );
            } else if constexpr (std::is_same_v<T, TurbulentVelocityRandomInitializer>) {
                Vector3 forward = concrete.forward;
                Vector3 right = concrete.right;
                forward.y = -forward.y;
                right.y = -right.y;
                forward = length(forward) > 0.0001
                    ? normalized(forward)
                    : Vector3{0.0, 1.0, 0.0};
                right = length(right) > 0.0001
                    ? normalized(right)
                    : Vector3{1.0, 0.0, 0.0};

                // Match Linux's setup order: speed is sampled before the
                // per-particle phase.
                const double speed = randomRange(
                    concrete.speedMinimum,
                    concrete.speedMaximum
                );
                const double phase = randomRange(
                    concrete.phaseMinimum,
                    concrete.phaseMaximum
                );
                Vector3 sample = add(
                    multiply(particle.position, 0.1),
                    {
                        simulationTimeSeconds_ * concrete.timeScale + phase,
                        simulationTimeSeconds_ * concrete.timeScale + phase * 0.7,
                        simulationTimeSeconds_ * concrete.timeScale + phase * 1.3,
                    }
                );
                Vector3 direction = curlNoise(sample);
                direction = length(direction) > 0.0001
                    ? normalized(direction)
                    : forward;

                if (concrete.scale < 2.0) {
                    const double angle = std::acos(std::clamp(
                        dot(direction, forward),
                        -1.0,
                        1.0
                    ));
                    const double maximumAngle = concrete.scale * pi / 2.0;
                    if (angle > maximumAngle && maximumAngle > 0.0001) {
                        const Vector3 axis = cross(direction, forward);
                        if (length(axis) > 0.0001) {
                            direction = rotateAroundAxis(
                                direction,
                                axis,
                                angle - maximumAngle
                            );
                        }
                    }
                }
                if (std::abs(concrete.offset) > 0.0001) {
                    direction = rotateAroundAxis(
                        direction,
                        right,
                        -concrete.offset
                    );
                }
                if ((configuration_.flags & 4U) == 0U) {
                    direction.z = 0.0;
                    direction = length(direction) > 0.0001
                        ? normalized(direction)
                        : Vector3{0.0, 1.0, 0.0};
                }
                particle.simulationVelocity = add(
                    particle.simulationVelocity,
                    multiply(direction, speed)
                );
            } else if constexpr (std::is_same_v<
                                     T,
                                     MapSequenceAroundControlPointInitializer
                                 >) {
                const int count = concrete.count;
                if (count <= 0) {
                    throw std::invalid_argument(
                        "Particle map sequence count must be greater than zero"
                    );
                }
                InitializerState& state = initializerStates_[initializerIndex];
                const double angle =
                    (static_cast<double>(state.mapSequenceIndex %
                                         static_cast<std::uint64_t>(count)) /
                     static_cast<double>(count)) * 2.0 * pi;
                state.mapSequenceIndex =
                    (state.mapSequenceIndex + 1U) %
                    static_cast<std::uint64_t>(count);

                particle.position = controlPointPosition(concrete.controlPoint)
                    .value_or(Vector3{});
                Vector3 speed = randomRange(
                    concrete.speedMinimum,
                    concrete.speedMaximum
                );
                speed.y = -speed.y;
                // GLM's column-major matrix in the pinned implementation
                // yields x' = cos*x + sin*y, y' = -sin*x + cos*y.
                const double cosine = std::cos(angle);
                const double sine = std::sin(angle);
                particle.simulationVelocity = {
                    cosine * speed.x + sine * speed.y,
                    -sine * speed.x + cosine * speed.y,
                    speed.z,
                };
            }
        }, initializer);
    }
    refreshConcreteParticleValues(particle);
}

void ParticleSimulation::applyOperators(double stepSeconds) {
    for (std::size_t operationIndex = 0;
         operationIndex < configuration_.operators.size();
         ++operationIndex) {
        const Operator& operation = configuration_.operators[operationIndex];
        std::visit([this, stepSeconds, operationIndex](const auto& concrete) {
            using T = std::decay_t<decltype(concrete)>;
            if constexpr (std::is_same_v<T, MovementOperator>) {
                Vector3 gravity = concrete.gravity;
                gravity.y = -gravity.y;
                const double dragFactor = std::max(0.0, 1.0 - concrete.drag * stepSeconds);
                for (ParticleInstance& particle : particles_) {
                    particle.position = add(
                        particle.position,
                        multiply(
                            particle.simulationVelocity,
                            stepSeconds * concreteOverrides(configuration_).speed
                        )
                    );
                    particle.simulationVelocity = add(
                        particle.simulationVelocity,
                        multiply(gravity, stepSeconds)
                    );
                    particle.simulationVelocity = multiply(
                        particle.simulationVelocity,
                        dragFactor
                    );
                }
            } else if constexpr (std::is_same_v<T, AlphaFadeOperator>) {
                for (ParticleInstance& particle : particles_) {
                    const double life = particle.lifetimePosition();
                    const double baseAlpha = particle.initialAlpha *
                        concreteOverrides(configuration_).alpha;
                    if (life <= concrete.fadeInTime) {
                        particle.alpha = baseAlpha * fadeValue(
                            life,
                            0.0,
                            concrete.fadeInTime,
                            0.0,
                            1.0
                        );
                    } else if (life > concrete.fadeOutTime) {
                        particle.alpha = baseAlpha * (1.0 - fadeValue(
                            life,
                            concrete.fadeOutTime,
                            1.0,
                            0.0,
                            1.0
                        ));
                    } else {
                        particle.alpha = baseAlpha;
                    }
                }
            } else if constexpr (std::is_same_v<T, SizeChangeOperator>) {
                for (ParticleInstance& particle : particles_) {
                    const double life = particle.lifetimePosition();
                    const double multiplier = fadeValue(
                        life,
                        concrete.startTime,
                        concrete.endTime,
                        concrete.startValue,
                        concrete.endValue
                    );
                    particle.size = particle.initialSize *
                        concreteOverrides(configuration_).size * multiplier;
                }
            } else if constexpr (std::is_same_v<T, AlphaChangeOperator>) {
                for (ParticleInstance& particle : particles_) {
                    const double life = particle.lifetimePosition();
                    const double multiplier = fadeValue(
                        life,
                        concrete.startTime,
                        concrete.endTime,
                        concrete.startValue,
                        concrete.endValue
                    );
                    particle.alpha = particle.initialAlpha *
                        concreteOverrides(configuration_).alpha * multiplier;
                }
            } else if constexpr (std::is_same_v<T, ColorChangeOperator>) {
                for (ParticleInstance& particle : particles_) {
                    const double life = particle.lifetimePosition();
                    const Vector3 color = {
                        fadeValue(
                            life,
                            concrete.startTime,
                            concrete.endTime,
                            concrete.startValue.x,
                            concrete.endValue.x
                        ),
                        fadeValue(
                            life,
                            concrete.startTime,
                            concrete.endTime,
                            concrete.startValue.y,
                            concrete.endValue.y
                        ),
                        fadeValue(
                            life,
                            concrete.startTime,
                            concrete.endTime,
                            concrete.startValue.z,
                            concrete.endValue.z
                        ),
                    };
                    particle.color = multiply(
                        multiply(
                            particle.initialColor,
                            concreteOverrides(configuration_).colorMultiplier
                        ),
                        color
                    );
                }
            } else if constexpr (std::is_same_v<T, AngularMovementOperator>) {
                const double dragFactor = std::max(
                    0.0,
                    1.0 - concrete.drag * stepSeconds
                );
                for (ParticleInstance& particle : particles_) {
                    particle.rotation = add(
                        particle.rotation,
                        multiply(
                            particle.simulationAngularVelocity,
                            stepSeconds * concreteOverrides(configuration_).speed
                        )
                    );
                    particle.simulationAngularVelocity = add(
                        particle.simulationAngularVelocity,
                        multiply(concrete.force, stepSeconds)
                    );
                    particle.simulationAngularVelocity = multiply(
                        particle.simulationAngularVelocity,
                        dragFactor
                    );
                    particle.rotation = {
                        wrappedAngle(particle.rotation.x),
                        wrappedAngle(particle.rotation.y),
                        wrappedAngle(particle.rotation.z),
                    };
                }
            } else if constexpr (std::is_same_v<T, OscillatePositionOperator>) {
                for (ParticleInstance& particle : particles_) {
                    const ParticleOscillatorState& state =
                        particle.oscillatorStates[operationIndex];
                    const Vector3 frequency{
                        oscillatorRandomRange(
                            concrete.frequencyMinimum,
                            concrete.frequencyMaximum,
                            state.frequencyUnit.x
                        ),
                        oscillatorRandomRange(
                            concrete.frequencyMinimum,
                            concrete.frequencyMaximum,
                            state.frequencyUnit.y
                        ),
                        oscillatorRandomRange(
                            concrete.frequencyMinimum,
                            concrete.frequencyMaximum,
                            state.frequencyUnit.z
                        ),
                    };
                    const Vector3 scale{
                        oscillatorRandomRange(
                            concrete.scaleMinimum,
                            concrete.scaleMaximum,
                            state.scaleUnit.x
                        ),
                        oscillatorRandomRange(
                            concrete.scaleMinimum,
                            concrete.scaleMaximum,
                            state.scaleUnit.y
                        ),
                        oscillatorRandomRange(
                            concrete.scaleMinimum,
                            concrete.scaleMaximum,
                            state.scaleUnit.z
                        ),
                    };
                    const Vector3 phase{
                        oscillatorRandomRange(
                            concrete.phaseMinimum,
                            concrete.phaseMaximum,
                            state.phaseUnit.x,
                            true
                        ),
                        oscillatorRandomRange(
                            concrete.phaseMinimum,
                            concrete.phaseMaximum,
                            state.phaseUnit.y,
                            true
                        ),
                        oscillatorRandomRange(
                            concrete.phaseMinimum,
                            concrete.phaseMaximum,
                            state.phaseUnit.z,
                            true
                        ),
                    };
                    const Vector3 movement{
                        -scale.x * frequency.x *
                            std::sin(frequency.x * particle.age + phase.x),
                        -scale.y * frequency.y *
                            std::sin(frequency.y * particle.age + phase.y),
                        -scale.z * frequency.z *
                            std::sin(frequency.z * particle.age + phase.z),
                    };
                    particle.position = add(
                        particle.position,
                        multiply(
                            multiply(movement, concrete.mask),
                            stepSeconds * concreteOverrides(configuration_).speed
                        )
                    );
                }
            } else if constexpr (std::is_same_v<T, OscillateAlphaOperator>) {
                for (ParticleInstance& particle : particles_) {
                    const ParticleOscillatorState& state =
                        particle.oscillatorStates[operationIndex];
                    const double frequency = oscillatorRandomRange(
                        concrete.frequencyMinimum,
                        concrete.frequencyMaximum,
                        state.frequencyUnit.x
                    );
                    const double phase = oscillatorRandomRange(
                        concrete.phaseMinimum,
                        concrete.phaseMaximum,
                        state.phaseUnit.x,
                        true
                    );
                    const double wave =
                        (std::cos(frequency * particle.age + phase) + 1.0) * 0.5;
                    particle.alpha *= interpolate(
                        concrete.scaleMinimum,
                        concrete.scaleMaximum,
                        wave
                    );
                }
            } else if constexpr (std::is_same_v<T, OscillateSizeOperator>) {
                for (ParticleInstance& particle : particles_) {
                    const ParticleOscillatorState& state =
                        particle.oscillatorStates[operationIndex];
                    const double frequency = oscillatorRandomRange(
                        concrete.frequencyMinimum,
                        concrete.frequencyMaximum,
                        state.frequencyUnit.x
                    );
                    const double phase = oscillatorRandomRange(
                        concrete.phaseMinimum,
                        concrete.phaseMaximum,
                        state.phaseUnit.x,
                        true
                    );
                    const double wave =
                        (std::cos(frequency * particle.age + phase) + 1.0) * 0.5;
                    particle.size *= interpolate(
                        concrete.scaleMinimum,
                        concrete.scaleMaximum,
                        wave
                    );
                }
            } else if constexpr (std::is_same_v<T, ControlPointAttractOperator>) {
                const auto controlPoint = controlPointPosition(
                    concrete.controlPoint
                );
                if (!controlPoint) {
                    return;
                }
                const Vector3 center = add(*controlPoint, concrete.origin);
                const double threshold = concrete.threshold / 2.0;
                for (ParticleInstance& particle : particles_) {
                    const Vector3 towardCenter = subtract(
                        center,
                        particle.position
                    );
                    const double distance = length(towardCenter);
                    if (distance > 0.001 && distance < threshold) {
                        particle.simulationVelocity = add(
                            particle.simulationVelocity,
                            multiply(
                                normalized(towardCenter),
                                concrete.scale * stepSeconds
                            )
                        );
                    }
                }
            } else if constexpr (std::is_same_v<T, TurbulenceOperator>) {
                const OperatorState& state = operatorStates_[operationIndex];
                if (!state.turbulenceInitialized ||
                    state.turbulenceSpeed <= 0.0001) {
                    return;
                }
                const double noiseScale = concrete.scale * 2.0;
                const double currentTime = simulationTimeSeconds_ + stepSeconds;
                for (ParticleInstance& particle : particles_) {
                    Vector3 noisePosition = particle.position;
                    noisePosition.x += state.turbulencePhase +
                        concrete.timeScale * currentTime;
                    noisePosition = multiply(noisePosition, noiseScale);
                    Vector3 direction = curlNoise(noisePosition);
                    const double magnitude = length(direction);
                    if (magnitude > 0.0001) {
                        direction = multiply(
                            direction,
                            state.turbulenceSpeed / magnitude
                        );
                    }
                    direction = multiply(direction, concrete.mask);
                    particle.simulationVelocity = add(
                        particle.simulationVelocity,
                        multiply(direction, stepSeconds)
                    );
                }
            } else if constexpr (std::is_same_v<T, VortexOperator>) {
                // Linux leaves audio-modulated vortex operators inert when no
                // spectrum is available. Never synthesize an amplitude here.
                if (concrete.audioProcessingMode > 0) {
                    return;
                }
                Vector3 axis = concrete.axis;
                axis = length(axis) > 0.0 ? normalized(axis) : Vector3{0.0, 0.0, 1.0};
                const Vector3 center = add(
                    controlPointPosition(concrete.controlPoint).value_or(Vector3{}),
                    concrete.offset
                );
                const bool infiniteAxis = (concrete.flags & 1U) != 0U;
                const bool maintainDistance = (concrete.flags & 2U) != 0U;
                const bool ringShape = (concrete.flags & 4U) != 0U;
                for (ParticleInstance& particle : particles_) {
                    const Vector3 toParticle = subtract(particle.position, center);
                    double axialDistance = 0.0;
                    Vector3 radialVector = toParticle;
                    if (infiniteAxis) {
                        axialDistance = dot(toParticle, axis);
                        radialVector = subtract(
                            toParticle,
                            multiply(axis, axialDistance)
                        );
                    }
                    (void)axialDistance;
                    const double distance = length(radialVector);
                    Vector3 tangent = cross(axis, radialVector);
                    const double tangentLength = length(tangent);
                    if (tangentLength <= 0.001) {
                        continue;
                    }
                    tangent = multiply(tangent, 1.0 / tangentLength);

                    double speed = 0.0;
                    Vector3 radialForce{};
                    if (ringShape) {
                        const double ringInner = concrete.ringRadius -
                            concrete.ringWidth * 0.5;
                        const double ringOuter = concrete.ringRadius +
                            concrete.ringWidth * 0.5;
                        if (distance < ringInner) {
                            speed = 0.0;
                        } else if (distance <= ringOuter) {
                            const double t = (distance - ringInner) /
                                concrete.ringWidth;
                            speed = concrete.speedInner + t *
                                (concrete.speedOuter - concrete.speedInner);
                        } else if (distance <= ringOuter + concrete.ringPullDistance) {
                            const double pullT = (distance - ringOuter) /
                                concrete.ringPullDistance;
                            speed = concrete.speedOuter * (1.0 - pullT);
                            if (distance > 0.001) {
                                radialForce = multiply(
                                    multiply(normalized(radialVector), -1.0),
                                    concrete.ringPullForce * pullT
                                );
                            }
                        }
                    } else {
                        const double disMid = concrete.distanceOuter -
                            concrete.distanceInner + 0.1;
                        if (disMid < 0.0 || distance < concrete.distanceInner) {
                            speed = concrete.speedInner;
                        } else if (distance > concrete.distanceOuter) {
                            speed = concrete.speedOuter;
                        } else {
                            const double t = (distance - concrete.distanceInner) /
                                disMid;
                            speed = concrete.speedInner + t *
                                (concrete.speedOuter - concrete.speedInner);
                        }
                    }

                    particle.simulationVelocity = add(
                        particle.simulationVelocity,
                        multiply(tangent, speed * stepSeconds)
                    );
                    particle.simulationVelocity = add(
                        particle.simulationVelocity,
                        multiply(radialForce, stepSeconds)
                    );
                    if (maintainDistance && distance > 0.001) {
                        particle.simulationVelocity = add(
                            particle.simulationVelocity,
                            multiply(
                                multiply(normalized(radialVector), -1.0),
                                concrete.centerForce * stepSeconds
                            )
                        );
                    }
                }
            }
        }, operation);
    }
}

void ParticleSimulation::refreshConcreteParticleValues(
    ParticleInstance& particle
) const {
    particle.color = multiply(
        particle.initialColor,
        concreteOverrides(configuration_).colorMultiplier
    );
    particle.alpha =
        particle.initialAlpha * concreteOverrides(configuration_).alpha;
    particle.size =
        particle.initialSize * concreteOverrides(configuration_).size;
    particle.lifetime =
        particle.initialLifetime * concreteOverrides(configuration_).lifetime;
    particle.velocity = multiply(
        particle.simulationVelocity,
        concreteOverrides(configuration_).speed
    );
    particle.angularVelocity = multiply(
        particle.simulationAngularVelocity,
        concreteOverrides(configuration_).speed
    );
}

void ParticleSimulation::reclaimDeadParticles() {
    std::size_t writeIndex = 0;
    for (std::size_t readIndex = 0; readIndex < particles_.size(); ++readIndex) {
        if (particles_[readIndex].alive()) {
            if (writeIndex != readIndex) {
                particles_[writeIndex] = particles_[readIndex];
            }
            ++writeIndex;
        }
    }
    particles_.resize(writeIndex);
}

std::size_t ParticleSimulation::effectiveMaxCount() const {
    const long double adjusted = std::floor(
        static_cast<long double>(configuration_.maxCount) *
        concreteOverrides(configuration_).count
    );
    if (!std::isfinite(adjusted) || adjusted < 0.0L ||
        adjusted >= sizeExclusiveUpperBound()) {
        throw std::overflow_error(
            "Particle effective count exceeds size_t capacity"
        );
    }
    const std::size_t count = static_cast<std::size_t>(adjusted);
    return count == 0 ? defaultParticleCapacity : count;
}

std::optional<Vector3> ParticleSimulation::controlPointPosition(int id) const {
    if (id == -1) {
        const auto linkedControlPointZero = std::find_if(
            configuration_.controlPoints.begin(),
            configuration_.controlPoints.end(),
            [](const ControlPoint& controlPoint) {
                return controlPoint.id == 0 &&
                    controlPoint.linkedToPointer;
            }
        );
        if (linkedControlPointZero ==
            configuration_.controlPoints.end()) {
            return std::nullopt;
        }
        id = 0;
    }
    if (id < 0 || id >= particleControlPointSlotCount) {
        return std::nullopt;
    }
    for (const ControlPoint& controlPoint : configuration_.controlPoints) {
        if (controlPoint.id == id) {
            return controlPoint.position;
        }
    }
    if (id < particleControlPointSlotCount) {
        return Vector3{};
    }
    return std::nullopt;
}

double ParticleSimulation::stableParticleUnit(
    std::uint64_t spawnId,
    std::uint64_t stream
) const noexcept {
    std::uint64_t value = seed_ ^ stream ^
        (spawnId * 0x9e3779b97f4a7c15ULL);
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    value ^= value >> 31U;
    constexpr double inverse53Bits = 1.0 / 9007199254740992.0;
    return static_cast<double>(value >> 11U) * inverse53Bits;
}

std::uint32_t ParticleSimulation::nextRandomBits() noexcept {
    const std::uint64_t oldState = randomState_;
    randomState_ = oldState * 6364136223846793005ULL + (randomIncrement_ | 1ULL);
    const std::uint32_t xorShifted = static_cast<std::uint32_t>(
        ((oldState >> 18U) ^ oldState) >> 27U
    );
    const std::uint32_t rotation = static_cast<std::uint32_t>(oldState >> 59U);
    return (xorShifted >> rotation) |
           (xorShifted << ((0U - rotation) & 31U));
}

double ParticleSimulation::randomUnit() noexcept {
    constexpr double inverse24Bits = 1.0 / 16777216.0;
    return static_cast<double>(nextRandomBits() >> 8U) * inverse24Bits;
}

double ParticleSimulation::randomRange(double minimum, double maximum) noexcept {
    if (maximum < minimum) {
        std::swap(minimum, maximum);
    }
    return minimum + randomUnit() * (maximum - minimum);
}

Vector3 ParticleSimulation::randomRange(Vector3 minimum, Vector3 maximum) noexcept {
    return {
        randomRange(minimum.x, maximum.x),
        randomRange(minimum.y, maximum.y),
        randomRange(minimum.z, maximum.z),
    };
}

}  // namespace we::scene::particle
