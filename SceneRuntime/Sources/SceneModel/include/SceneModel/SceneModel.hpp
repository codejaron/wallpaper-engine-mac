#ifndef WE_SCENE_MODEL_SCENE_MODEL_HPP
#define WE_SCENE_MODEL_SCENE_MODEL_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace we::scene {

class AssetResolver;
class Runtime;
struct PuppetMesh;

enum class SceneModelErrorCode : std::int32_t {
    invalidJson = 1,
    missingField = 2,
    typeMismatch = 3,
    invalidValue = 4,
    unsupportedProject = 5,
    unsupportedObject = 6,
    duplicateId = 7,
    danglingReference = 8,
    referenceCycle = 9,
    assetFailure = 10,
};

class SceneModelError final : public std::runtime_error {
public:
    SceneModelError(
        SceneModelErrorCode code,
        std::string assetPath,
        std::string jsonPointer,
        std::vector<std::string> referenceChain,
        std::string message
    );

    [[nodiscard]] SceneModelErrorCode code() const noexcept;
    [[nodiscard]] const std::string& assetPath() const noexcept;
    [[nodiscard]] const std::string& jsonPointer() const noexcept;
    [[nodiscard]] const std::vector<std::string>& referenceChain()
        const noexcept;

private:
    SceneModelErrorCode code_;
    std::string assetPath_;
    std::string jsonPointer_;
    std::vector<std::string> referenceChain_;
};

struct Value {
    using Array = std::vector<Value>;
    using Object = std::map<std::string, Value>;
    using Storage = std::variant<
        std::nullptr_t,
        bool,
        std::int64_t,
        double,
        std::string,
        Array,
        Object
    >;

    Storage storage = nullptr;

    [[nodiscard]] bool isNull() const noexcept;
    [[nodiscard]] friend bool operator==(
        const Value& lhs,
        const Value& rhs
    ) = default;
};

enum class RuntimeValueType {
    null = 0,
    vector4 = 4,
    vector3 = 5,
    vector2 = 6,
    floating = 7,
    integer = 8,
    boolean = 9,
    string = 10,
};

// Wallpaper Engine assigns every DynamicValue a tagged underlying type while
// updating all scalar and vector projections at the same time. Runtime
// consumers must read these stored projections instead of reinterpreting the
// authored JSON independently.
class RuntimeValue final {
public:
    [[nodiscard]] static RuntimeValue null();
    [[nodiscard]] static RuntimeValue floating(double value);
    [[nodiscard]] static RuntimeValue integer(std::int64_t value);
    [[nodiscard]] static RuntimeValue boolean(bool value);
    [[nodiscard]] static RuntimeValue condition(
        std::string source,
        std::string_view expected
    );
    [[nodiscard]] static RuntimeValue string(std::string value);
    [[nodiscard]] static RuntimeValue initialString(std::string value);
    [[nodiscard]] static RuntimeValue colorString(std::string value);
    [[nodiscard]] static RuntimeValue vector(
        const std::array<double, 4>& components,
        std::size_t componentCount
    );
    [[nodiscard]] static RuntimeValue color(
        const std::array<double, 4>& components
    );
    [[nodiscard]] static RuntimeValue fromValue(const Value& value);
    [[nodiscard]] RuntimeValue updatedToNull() const;

    [[nodiscard]] RuntimeValueType type() const noexcept;
    [[nodiscard]] bool isVector() const noexcept;
    [[nodiscard]] std::size_t componentCount() const noexcept;
    [[nodiscard]] const std::array<double, 4>& vector() const noexcept;
    [[nodiscard]] double number() const noexcept;
    [[nodiscard]] std::int64_t integer() const noexcept;
    [[nodiscard]] bool boolean() const noexcept;
    [[nodiscard]] const std::string& string() const noexcept;
    [[nodiscard]] std::string toString() const;

    [[nodiscard]] friend bool operator==(
        const RuntimeValue& lhs,
        const RuntimeValue& rhs
    ) = default;

private:
    RuntimeValueType type_ = RuntimeValueType::null;
    std::array<double, 4> vector_{};
    double number_ = 0.0;
    std::int64_t integer_ = 0;
    bool boolean_ = false;
    std::string string_;
};

struct UserBinding {
    std::string property;
    std::optional<std::string> condition;
};

struct TimelineAnimationTangent {
    bool enabled = false;
    double x = 0.0;
    double y = 0.0;

    [[nodiscard]] friend bool operator==(
        const TimelineAnimationTangent& lhs,
        const TimelineAnimationTangent& rhs
    ) = default;
};

struct TimelineAnimationKeyframe {
    double frame = 0.0;
    double value = 0.0;
    TimelineAnimationTangent front;
    TimelineAnimationTangent back;

    [[nodiscard]] friend bool operator==(
        const TimelineAnimationKeyframe& lhs,
        const TimelineAnimationKeyframe& rhs
    ) = default;
};

enum class TimelineAnimationMode {
    loop,
    mirror,
    single,
};

struct TimelineAnimation {
    std::array<std::vector<TimelineAnimationKeyframe>, 4> curves;
    double fps = 30.0;
    double length = 0.0;
    TimelineAnimationMode mode = TimelineAnimationMode::loop;
    std::string name;
    bool startPaused = false;
    bool wrapLoop = false;
    bool relative = false;
    std::optional<std::string> parent;
    std::vector<std::string> children;

    [[nodiscard]] friend bool operator==(
        const TimelineAnimation& lhs,
        const TimelineAnimation& rhs
    ) = default;
};

struct DynamicValue {
    RuntimeValue value;
    std::optional<UserBinding> user;
    std::optional<std::string> script;
    std::map<std::string, DynamicValue> scriptProperties;
    std::optional<TimelineAnimation> animation;

    [[nodiscard]] bool isDynamic() const noexcept {
        return user.has_value() || script.has_value() || animation.has_value();
    }
};

enum class PropertyType {
    boolean,
    slider,
    combo,
    color,
    text,
    sceneTexture,
    file,
    directory,
    textInput,
    userShortcut,
    group,
};

struct PropertyOption {
    std::string value;
    std::string label;
};

struct ProjectProperty {
    std::string name;
    PropertyType type = PropertyType::group;
    std::string text;
    std::optional<Value> value;
    std::vector<PropertyOption> options;
    std::optional<int> index;
    std::optional<int> order;
    std::optional<double> minimum;
    std::optional<double> maximum;
    std::optional<double> step;
    std::optional<int> precision;
    std::optional<bool> fraction;
};

enum class BlendingMode { normal, translucent, additive };
enum class CullingMode { normal, disabled };
enum class DepthMode { disabled, enabled };
enum class EffectCommand { copy, swap };

struct TextureSlot {
    std::optional<std::string> name;
    std::optional<Value::Object> metadata;
};

using TextureSlots = std::vector<TextureSlot>;
using ComboMap = std::map<std::string, int>;
using ConstantMap = std::map<std::string, DynamicValue>;

struct MaterialPass {
    BlendingMode blending = BlendingMode::normal;
    CullingMode culling = CullingMode::disabled;
    DepthMode depthTest = DepthMode::disabled;
    DepthMode depthWrite = DepthMode::disabled;
    std::string shader;
    TextureSlots textures;
    TextureSlots userTextures;
    ComboMap combos;
    ConstantMap constants;
};

struct Material {
    std::string assetPath;
    std::vector<MaterialPass> passes;
};

struct Model {
    std::string assetPath;
    std::shared_ptr<const Material> material;
    bool solidLayer = false;
    bool fullscreen = false;
    bool passthrough = false;
    bool projectLayer = false;
    bool autoSize = false;
    bool noPadding = false;
    std::optional<int> width;
    std::optional<int> height;
    // Decoded once while loading the model. Frame planning and GL execution
    // consume this immutable descriptor instead of resolving the asset again.
    std::shared_ptr<const PuppetMesh> puppetMesh;
    std::optional<DynamicValue> cropOffset;
};

struct FramebufferDefinition {
    std::string name;
    std::string format = "rgba8888";
    double scale = 1.0;
    bool unique = false;
    std::optional<int> fit;
    std::optional<int> width;
    std::optional<int> height;
    std::optional<std::string> uvs;
};

struct EffectBind {
    int index = 0;
    std::string name;
};

struct EffectPass {
    std::shared_ptr<const Material> material;
    std::vector<EffectBind> binds;
    std::optional<EffectCommand> command;
    std::optional<std::string> source;
    std::optional<std::string> target;
    bool compose = false;
};

struct Effect {
    std::string assetPath;
    std::string name;
    std::string description;
    std::string group;
    std::string preview;
    std::vector<std::string> dependencies;
    std::vector<EffectPass> passes;
    std::vector<FramebufferDefinition> framebuffers;
};

struct EffectPassOverride {
    std::optional<int> id;
    ComboMap combos;
    ConstantMap constants;
    TextureSlots textures;
    TextureSlots userTextures;
};

struct ImageEffect {
    std::optional<int> id;
    std::string name;
    DynamicValue visible;
    std::vector<EffectPassOverride> passOverrides;
    std::shared_ptr<const Effect> effect;
};

// Wallpaper Engine uses both a shorthand integer dependency and an expanded
// authoring form carrying the role/index used by particle systems. Retain the
// metadata here even when the current render stage only needs the object id;
// later particle/frame graph stages must not have to reparse scene.json.
struct ObjectDependency {
    int id = 0;
    std::optional<int> index;
    std::optional<std::string> type;
};

struct ObjectBase {
    int id = 0;
    std::string name;
    std::optional<int> parent;
    std::vector<ObjectDependency> dependencies;
    DynamicValue origin;
    DynamicValue scale;
    DynamicValue angles;
    DynamicValue visible;
    bool disablePropagation = false;
    bool solid = false;
    bool lockTransforms = false;
};

struct ImageObject {
    std::shared_ptr<const Model> model;
    // Linux appends materials/util/effectpassthrough.json when the authored
    // color blend mode can become non-zero. Keeping the parsed material on the
    // image lets FrameGraph add the real compatibility pass without reparsing
    // assets or maintaining a second material implementation.
    std::shared_ptr<const Material> colorBlendMaterial;
    // Linux injects materials/effects/tint.json when an effect exposes the
    // magenta COMPOSITE=2 compatibility marker. Visibility and the marker's
    // DynamicValue are evaluated per frame, so retain the parsed material for
    // FrameGraph.
    std::shared_ptr<const Material> magentaCompositeTintMaterial;
    DynamicValue alpha;
    DynamicValue color;
    DynamicValue size;
    DynamicValue parallaxDepth;
    DynamicValue brightness;
    DynamicValue colorBlendMode;
    std::string horizontalAlignment = "center";
    bool copyBackground = false;
    bool perspective = false;
    bool ledSource = false;
    TextureSlots instanceTextures;
    TextureSlots instanceUserTextures;
    std::vector<ImageEffect> effects;
};

struct TextObject {
    // Text is rasterized by SceneGL, then fed through the same authored
    // effect graph as an image layer. The synthetic source model carries the
    // official passthrough material used to enter that graph without adding a
    // second shader implementation for text.
    std::shared_ptr<const Model> effectSourceModel;
    std::shared_ptr<const Material> magentaCompositeTintMaterial;
    std::vector<ImageEffect> effects;
    DynamicValue text;
    std::string font;
    DynamicValue pointSize;
    DynamicValue size;
    DynamicValue color;
    DynamicValue alpha;
    DynamicValue padding;
    DynamicValue spacing;
    std::string horizontalAlignment;
    std::string verticalAlignment;
    bool perspective = false;
    bool limitRows = false;
    bool limitUseEllipsis = false;
    bool limitWidth = false;
    int maxRows = 0;
    double maxWidth = 0.0;
};

enum class SoundPlaybackMode {
    loop,
    random,
    single,
};

struct SoundObject {
    std::vector<std::string> sounds;
    SoundPlaybackMode playbackMode = SoundPlaybackMode::loop;
    DynamicValue volume;
    bool startSilent = false;
    bool muteInEditor = false;
    double minimumTime = 0.0;
    double maximumTime = 0.0;
};

struct ParticleVector3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;

    [[nodiscard]] friend bool operator==(
        const ParticleVector3& lhs,
        const ParticleVector3& rhs
    ) = default;
};

struct ParticleEmitterBase {
    std::optional<int> id;
    ParticleVector3 directions{1.0, 1.0, 0.0};
    ParticleVector3 distanceMin{};
    ParticleVector3 distanceMax{256.0, 256.0, 0.0};
    ParticleVector3 origin{};
    std::uint32_t instantaneous = 0;
    double rate = 10.0;
    double delay = 0.0;
    double duration = 0.0;
    double minimumPeriodicDelay = 1.0;
    double maximumPeriodicDelay = 2.0;
    double minimumPeriodicDuration = 2.0;
    double maximumPeriodicDuration = 3.0;
    std::uint32_t maximumToEmitPerPeriod = 0;
    int controlPoint = 0;
    std::uint32_t flags = 0;
};

struct ParticleBoxRandomEmitter {
    ParticleEmitterBase base;
};

struct ParticleSphereRandomEmitter {
    ParticleEmitterBase base;
    ParticleVector3 sign{};
    double speedMin = 0.0;
    double speedMax = 0.0;
};

using ParticleEmitter = std::variant<
    ParticleBoxRandomEmitter,
    ParticleSphereRandomEmitter
>;

struct ParticleLifetimeRandomInitializer {
    std::optional<int> id;
    DynamicValue minimum;
    DynamicValue maximum;
};

struct ParticleSizeRandomInitializer {
    std::optional<int> id;
    DynamicValue minimum;
    DynamicValue maximum;
    DynamicValue exponent;
};

struct ParticleColorRandomInitializer {
    std::optional<int> id;
    DynamicValue minimum;
    DynamicValue maximum;
};

struct ParticleAlphaRandomInitializer {
    std::optional<int> id;
    DynamicValue minimum;
    DynamicValue maximum;
};

struct ParticleVelocityRandomInitializer {
    std::optional<int> id;
    DynamicValue minimum;
    DynamicValue maximum;
};

struct ParticleRotationRandomInitializer {
    std::optional<int> id;
    DynamicValue minimum;
    DynamicValue maximum;
};

struct ParticleAngularVelocityRandomInitializer {
    std::optional<int> id;
    DynamicValue minimum;
    DynamicValue maximum;
    DynamicValue exponent;
};

struct ParticleTurbulentVelocityRandomInitializer {
    std::optional<int> id;
    DynamicValue speedMinimum;
    DynamicValue speedMaximum;
    DynamicValue scale;
    DynamicValue offset;
    DynamicValue forward;
    DynamicValue timeScale;
    DynamicValue phaseMinimum;
    DynamicValue phaseMaximum;
    DynamicValue right;
};

struct ParticleMapSequenceAroundControlPointInitializer {
    std::optional<int> id;
    DynamicValue controlPoint;
    DynamicValue count;
    DynamicValue speedMinimum;
    DynamicValue speedMaximum;
};

using ParticleInitializer = std::variant<
    ParticleLifetimeRandomInitializer,
    ParticleSizeRandomInitializer,
    ParticleColorRandomInitializer,
    ParticleAlphaRandomInitializer,
    ParticleVelocityRandomInitializer,
    ParticleRotationRandomInitializer,
    ParticleAngularVelocityRandomInitializer,
    ParticleTurbulentVelocityRandomInitializer,
    ParticleMapSequenceAroundControlPointInitializer
>;

struct ParticleMovementOperator {
    std::optional<int> id;
    DynamicValue drag;
    DynamicValue gravity;
};

struct ParticleAlphaFadeOperator {
    std::optional<int> id;
    DynamicValue fadeInTime;
    DynamicValue fadeOutTime;
};

struct ParticleAngularMovementOperator {
    std::optional<int> id;
    DynamicValue drag;
    DynamicValue force;
};

struct ParticleOscillatePositionOperator {
    std::optional<int> id;
    DynamicValue frequencyMinimum;
    DynamicValue frequencyMaximum;
    DynamicValue scaleMinimum;
    DynamicValue scaleMaximum;
    DynamicValue phaseMinimum;
    DynamicValue phaseMaximum;
    DynamicValue mask;
};

struct ParticleOscillateAlphaOperator {
    std::optional<int> id;
    DynamicValue frequencyMinimum;
    DynamicValue frequencyMaximum;
    DynamicValue scaleMinimum;
    DynamicValue scaleMaximum;
    DynamicValue phaseMinimum;
    DynamicValue phaseMaximum;
};

struct ParticleOscillateSizeOperator {
    std::optional<int> id;
    DynamicValue frequencyMinimum;
    DynamicValue frequencyMaximum;
    DynamicValue scaleMinimum;
    DynamicValue scaleMaximum;
    DynamicValue phaseMinimum;
    DynamicValue phaseMaximum;
};

struct ParticleControlPointAttractOperator {
    std::optional<int> id;
    int controlPoint = 0;
    DynamicValue origin;
    DynamicValue scale;
    DynamicValue threshold;
};

struct ParticleSizeChangeOperator {
    std::optional<int> id;
    DynamicValue startTime;
    DynamicValue endTime;
    DynamicValue startValue;
    DynamicValue endValue;
};

struct ParticleAlphaChangeOperator {
    std::optional<int> id;
    DynamicValue startTime;
    DynamicValue endTime;
    DynamicValue startValue;
    DynamicValue endValue;
};

struct ParticleColorChangeOperator {
    std::optional<int> id;
    DynamicValue startTime;
    DynamicValue endTime;
    DynamicValue startValue;
    DynamicValue endValue;
};

struct ParticleTurbulenceOperator {
    std::optional<int> id;
    DynamicValue scale;
    DynamicValue speedMinimum;
    DynamicValue speedMaximum;
    DynamicValue timeScale;
    DynamicValue mask;
    DynamicValue phaseMinimum;
    DynamicValue phaseMaximum;
    DynamicValue audioProcessingMode;
    DynamicValue audioProcessingBounds;
    DynamicValue audioProcessingExponent;
    DynamicValue audioProcessingFrequencyStart;
    DynamicValue audioProcessingFrequencyEnd;
};

struct ParticleVortexOperator {
    std::optional<int> id;
    int controlPoint = 0;
    std::uint32_t flags = 0;
    DynamicValue axis;
    DynamicValue offset;
    DynamicValue distanceInner;
    DynamicValue distanceOuter;
    DynamicValue speedInner;
    DynamicValue speedOuter;
    DynamicValue centerForce;
    DynamicValue ringRadius;
    DynamicValue ringWidth;
    DynamicValue ringPullDistance;
    DynamicValue ringPullForce;
    DynamicValue audioProcessingMode;
    DynamicValue audioProcessingBounds;
};

using ParticleOperator = std::variant<
    ParticleMovementOperator,
    ParticleAlphaFadeOperator,
    ParticleAngularMovementOperator,
    ParticleOscillatePositionOperator,
    ParticleOscillateAlphaOperator,
    ParticleOscillateSizeOperator,
    ParticleControlPointAttractOperator,
    ParticleSizeChangeOperator,
    ParticleAlphaChangeOperator,
    ParticleColorChangeOperator,
    ParticleTurbulenceOperator,
    ParticleVortexOperator
>;

// Particle renderer metadata is deliberately kept in the model instead of
// being rediscovered by the GL executor.  Linux uses the first authored
// renderer and supports four concrete paths: screen sprites, sprite trails,
// rope, and rope trails.  The orientation/axis fields are retained for the
// script/model bridge even though the pinned Linux renderer uses screen-facing
// sprites for the plain `sprite` path.
struct ParticleSpriteRenderer {
    std::optional<int> id;
    std::string name = "sprite";
    std::string orientation = "screen";
    ParticleVector3 axis{};
    double length = 0.05;
    double maxLength = 10.0;
    double minLength = 0.0;
    double subdivision = 1.0;
    double segments = 4.0;
    double uvScale = 1.0;
    bool uvScrolling = false;
    bool uvSmoothing = true;
    bool fadeAlpha = false;
    bool fadeSize = false;
};

struct ParticleControlPoint {
    int id = -1;
    std::uint32_t flags = 0;
    ParticleVector3 offset{};
    bool lockToPointer = false;
};

struct ParticleInstanceOverride {
    std::optional<int> id;
    DynamicValue enabled;
    DynamicValue alpha;
    DynamicValue size;
    DynamicValue lifetime;
    DynamicValue rate;
    DynamicValue speed;
    DynamicValue count;
    DynamicValue color;
    DynamicValue colorMultiplier;
};

// Linux currently parses child systems and carries their metadata on the
// particle definition, but does not instantiate a second runtime simulation.
// Keep the metadata lossless without inventing child-spawn behavior here.
struct ParticleChild {
    std::string type = "static";
    std::string name;
    int maxCount = 20;
    int controlPointStartIndex = 0;
    double probability = 1.0;
    ParticleVector3 angles{};
    ParticleVector3 origin{};
    ParticleVector3 scale{1.0, 1.0, 1.0};
    std::string particlePath;
};

struct ParticleDefinition {
    std::string assetPath;
    std::shared_ptr<const Material> material;
    std::uint32_t maxCount = 100;
    double startTime = 0.0;
    std::uint32_t flags = 0;
    std::string animationMode = "sequence";
    double sequenceMultiplier = 1.0;
    std::vector<ParticleEmitter> emitters;
    std::vector<ParticleInitializer> initializers;
    std::vector<ParticleOperator> operators;
    std::vector<ParticleControlPoint> controlPoints;
    std::vector<ParticleChild> children;
    ParticleSpriteRenderer renderer;
    // Keep every authored renderer losslessly.  The Linux runtime currently
    // consumes the first entry, while retaining the collection lets later
    // frame-graph stages validate/diagnose additional entries without
    // reparsing the asset.
    std::vector<ParticleSpriteRenderer> renderers;
};

struct ParticleObject {
    std::shared_ptr<const ParticleDefinition> definition;
    DynamicValue parallaxDepth;
    ParticleInstanceOverride instanceOverride;
};

struct GroupObject {};

using SceneObjectData =
    std::variant<
        GroupObject,
        ImageObject,
        TextObject,
        SoundObject,
        ParticleObject
    >;

struct SceneObject {
    ObjectBase base;
    SceneObjectData data;
    // The public getInitialLayerConfig() contract returns the authored layer
    // configuration, including fields that are intentionally immutable in
    // SceneModel. Retain that source once instead of reconstructing a partial
    // configuration from renderer state.
    Value initialConfig;
};

struct SceneCamera {
    DynamicValue center;
    DynamicValue eye;
    DynamicValue up;
    DynamicValue nearPlane;
    DynamicValue farPlane;
    DynamicValue fieldOfView;
    bool preview = false;
    bool projectionAuto = false;
    int projectionWidth = 0;
    int projectionHeight = 0;
};

struct Scene {
    std::string assetPath;
    SceneCamera camera;
    std::map<std::string, DynamicValue> generalValues;
    std::vector<SceneObject> objects;
    // Compatibility resources synthesized by linux-wallpaperengine for the
    // camera bloom post-process. The JSON wrappers are virtual upstream; the
    // referenced utility materials remain ordinary official assets.
    std::shared_ptr<const Model> bloomModel;
    std::shared_ptr<const Effect> bloomEffect;
};

struct SceneProject {
    std::string assetPath;
    std::string title;
    std::optional<std::string> workshopId;
    bool supportsAudioProcessing = false;
    std::map<std::string, ProjectProperty> properties;
    Scene scene;
};

struct PropertyStateSnapshot {
    std::uint64_t revision = 0;
    std::map<std::string, Value> values;
};

class SceneModelLoader final {
public:
    [[nodiscard]] static SceneProject load(
        const AssetResolver& resolver,
        std::string_view projectPath = "project.json"
    );
};

// Owns the immutable parsed project and the mutable user-property state. The
// runtime is retained so graphs and renderers can safely resolve assets even
// if the original C handle is destroyed first.
class SceneModel final {
public:
    [[nodiscard]] static std::shared_ptr<SceneModel> load(
        std::shared_ptr<const Runtime> runtime,
        std::string_view projectPath = "project.json"
    );

    SceneModel(const SceneModel&) = delete;
    SceneModel& operator=(const SceneModel&) = delete;
    ~SceneModel();

    [[nodiscard]] const SceneProject& project() const noexcept;
    [[nodiscard]] std::shared_ptr<const Runtime> runtime() const noexcept;
    [[nodiscard]] const std::vector<std::string>& propertyKeys() const noexcept;
    [[nodiscard]] std::optional<Value> propertyValue(
        std::string_view property
    ) const;
    [[nodiscard]] std::map<std::string, Value> propertyValues() const;
    [[nodiscard]] PropertyStateSnapshot propertyState() const;
    void setPropertyValue(std::string_view property, Value value);
    void setPropertyValues(std::vector<std::pair<std::string, Value>> values);
    [[nodiscard]] std::uint64_t revision() const noexcept;

private:
    struct State;
    explicit SceneModel(std::unique_ptr<State> state);

    std::unique_ptr<State> state_;
};

}  // namespace we::scene

#endif
