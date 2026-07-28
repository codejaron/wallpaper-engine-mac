#ifndef WE_SCENE_SCRIPT_SCENE_SCRIPT_HPP
#define WE_SCENE_SCRIPT_SCENE_SCRIPT_HPP

#include <SceneCore/AudioSpectrum.hpp>
#include <SceneModel/SceneModel.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace we::scene::script {

enum class ScriptErrorCode : std::int32_t {
    module = 1,
    exception = 2,
    invalidResultType = 3,
    nonFiniteResult = 4,
    audioInputUnavailable = 5,
    resourceLimit = 6,
};

class ScriptError final : public std::runtime_error {
public:
    ScriptError(ScriptErrorCode code, std::string message);

    [[nodiscard]] ScriptErrorCode code() const noexcept;

private:
    ScriptErrorCode code_;
};

enum class ScriptCursorEventType : std::int32_t {
    enter = 0,
    leave = 1,
    move = 2,
    down = 3,
    up = 4,
    click = 5,
};

struct ScriptCursorEvent final {
    ScriptCursorEventType type = ScriptCursorEventType::move;
    int layerId = 0;
    double worldX = 0;
    double worldY = 0;
    double worldZ = 0;
    double localX = 0;
    double localY = 0;
    double localZ = 0;
    std::optional<std::string> hitBox;
};

// Scene-level values exposed through Linux Wallpaper Engine's `thisScene`
// object.  The host supplies one coherent snapshot for a frame; keeping the
// values typed here prevents the JavaScript adapter from inventing defaults or
// re-parsing authored strings independently of SceneGraph's DynamicValue
// semantics.
struct ScriptSceneSnapshot final {
    bool bloom = false;
    std::int32_t bloomStrength = 0;
    std::int32_t bloomThreshold = 0;
    bool clearEnabled = false;
    std::array<double, 3> clearColor{0.0, 0.0, 0.0};
    std::array<double, 3> ambientColor{0.0, 0.0, 0.0};
    std::array<double, 3> skylightColor{0.0, 0.0, 0.0};
    double fieldOfView = 50.0;
    double nearZ = 0.0;
    double farZ = 1000.0;
    bool cameraFade = false;
    bool cameraShake = false;
    double cameraShakeSpeed = 0.0;
    double cameraShakeAmplitude = 0.0;
    double cameraShakeRoughness = 0.0;
    bool cameraParallax = false;
    double cameraParallaxAmount = 1.0;
    double cameraParallaxDelay = 0.0;
    double cameraParallaxMouseInfluence = 1.0;
};

enum class ScriptMediaPlaybackState : std::int32_t {
    stopped = 0,
    playing = 1,
    paused = 2,
};

struct ScriptMediaSnapshot final {
    // Media sources update independently. Keeping one revision for the whole
    // snapshot makes a timeline tick spuriously retrigger metadata, playback,
    // and thumbnail callbacks. Each revision identifies the immutable fields
    // consumed by the corresponding official SceneScript event.
    std::uint64_t statusRevision = 0;
    std::uint64_t metadataRevision = 0;
    std::uint64_t playbackRevision = 0;
    std::uint64_t timelineRevision = 0;
    std::uint64_t thumbnailRevision = 0;
    bool available = false;
    ScriptMediaPlaybackState playbackState = ScriptMediaPlaybackState::stopped;
    std::string title;
    std::string artist;
    std::string contentType;
    std::string albumTitle;
    std::string subTitle;
    std::string albumArtist;
    std::string genres;
    double position = 0;
    double duration = 0;
    bool hasThumbnail = false;
    std::array<double, 3> primaryColor{0, 0, 0};
    std::array<double, 3> secondaryColor{0, 0, 0};
    std::array<double, 3> tertiaryColor{0, 0, 0};
    std::array<double, 3> textColor{0, 0, 0};
    std::array<double, 3> highContrastColor{0, 0, 0};
};

struct ScriptUserPropertiesSnapshot final {
    std::map<std::string, RuntimeValue> values;
};

struct ScriptFrameInputs {
    double runtimeSeconds = 0;
    double frameTimeSeconds = 0;
    // Local-day fraction in [0, 1]. A missing value is an explicit host
    // integration gap; scripts must not observe a fabricated default.
    std::optional<double> timeOfDay;
    // The host must explicitly identify whether this instance is running in
    // Wallpaper Engine's screensaver mode.  An absent value is an integration
    // error, not a fabricated desktop/wallpaper default, when a script asks
    // engine.isScreensaver() or engine.isWallpaper().
    std::optional<bool> isScreensaver;
    // A missing frame is distinct from silence. Scripts that register audio
    // buffers fail with audioInputUnavailable instead of seeing zero-filled
    // data when the host capture path is not connected.
    std::optional<AudioSpectrumFrame> audioSpectrum;
    // Absent means the host has not connected scene-level state. Getters on
    // `thisScene` fail explicitly in that case; they never expose fabricated
    // values that could silently drive a wallpaper in the wrong state.
    std::optional<ScriptSceneSnapshot> sceneSnapshot;
    // Project-level exposed properties are independent from a DynamicValue's
    // local scriptProperties. The graph shares one immutable snapshot across
    // every script instance evaluated for the frame.
    std::shared_ptr<const ScriptUserPropertiesSnapshot> userProperties;
    double pointerX = 0;
    double pointerY = 0;
    // Absolute top-left-origin scene pixels resolved by SceneFrameGraph from
    // the same projection used to build the frame. Standalone SceneGraph
    // callers may leave this absent; accessing input.cursorWorldPosition then
    // fails explicitly instead of exposing a fabricated zero vector.
    std::optional<std::array<double, 3>> cursorWorldPosition;
    bool pointerLeftDown = false;
    // Cursor events are supplied by the host after hit testing.  The script
    // instance filters them to its owner layer before dispatching the
    // corresponding exported callback, so the public input contract does not
    // need to expose a second layer/object store.
    std::vector<ScriptCursorEvent> cursorEvents;
    std::optional<ScriptMediaSnapshot> mediaSnapshot;
};

struct ScriptLimits {
    std::size_t memoryBytes = 32 * 1024 * 1024;
    std::size_t stackBytes = 512 * 1024;
    std::chrono::nanoseconds executionTime = std::chrono::milliseconds(10);
};

// A scene-layer view shared by all script instances in one SceneGraph.  The
// registry deliberately carries RuntimeValue projections instead of exposing
// SceneModel or QuickJS objects across the boundary.  Hosts refresh the base
// descriptors at the beginning of a frame; script writes remain in the
// registry as the frame's authoritative overlay and are consumed by later
// graph/material planning.
enum class ScriptLayerType : std::int32_t {
    image = 1,
    text = 2,
    particle = 3,
    sound = 4,
    group = 5,
};

struct ScriptTextureAnimationMetadata final {
    std::string assetIdentity;
    std::vector<double> frameDurations;
};

struct ScriptTextureAnimationSnapshot final {
    int layerId = 0;
    std::string assetIdentity;
    std::size_t frame = 0;
    double timeSeconds = 0.0;
    double rate = 1.0;
    bool joined = true;
    bool playing = true;
};

enum class ScriptSoundCommandAction : std::int32_t {
    play = 1,
    pause = 2,
    stop = 3,
};

struct ScriptSoundCommand final {
    ScriptSoundCommandAction action = ScriptSoundCommandAction::play;
    std::uint64_t generation = 0;
};

enum class ScriptSoundRuntimeState : std::int32_t {
    stopped = 0,
    playing = 1,
    paused = 2,
    ended = 3,
};

struct ScriptSoundRuntimeSnapshot final {
    int layerId = 0;
    ScriptSoundRuntimeState state = ScriptSoundRuntimeState::stopped;
    double positionSeconds = 0.0;
};

struct ScriptSoundSnapshot final {
    int layerId = 0;
    std::optional<ScriptSoundCommand> command;
    ScriptSoundRuntimeState runtimeState = ScriptSoundRuntimeState::stopped;
    double positionSeconds = 0.0;
};

struct ScriptLayerDescriptor final {
    int id = 0;
    std::string name;
    ScriptLayerType type = ScriptLayerType::image;
    std::size_t sourceObjectIndex = 0;
    std::optional<int> parent;
    bool disablePropagation = false;
    bool dynamic = false;
    std::map<std::string, RuntimeValue> properties;
    std::map<std::string, TimelineAnimation> propertyAnimations;
    Value initialConfig;
    // Image layers expose getTextureAnimation() for their primary albedo
    // texture. Production descriptors provide the stable asset identity and
    // resolve metadata lazily; test hosts may provide immutable metadata
    // directly without constructing a texture package.
    std::optional<std::string> textureAssetIdentity;
    std::optional<ScriptTextureAnimationMetadata> textureAnimation;
    bool soundStartsAutomatically = false;
};

struct ScriptTimelineAnimationSnapshot final {
    int layerId = 0;
    std::string property;
    std::string name;
    double fps = 0.0;
    double frameCount = 0.0;
    double durationSeconds = 0.0;
    double frame = 0.0;
    double rate = 1.0;
    bool playing = false;
};

enum class ScriptPropertyObjectType : std::int32_t {
    effect = 1,
    material = 2,
};

// Non-layer SceneScript owners are registered by the frame component that
// constructs their effective runtime state. A material descriptor therefore
// contains the same merged shader constants that rendering will consume,
// rather than a second approximation reconstructed inside QuickJS.
struct ScriptPropertyObjectDescriptor final {
    std::string id;
    ScriptPropertyObjectType type = ScriptPropertyObjectType::material;
    std::string name;
    std::map<std::string, RuntimeValue> properties;
    std::vector<std::string> materialIds;
};

enum class ScriptPropertyOwnerType : std::int32_t {
    none = 0,
    layer = 1,
    effect = 2,
    material = 3,
};

// `thisLayer` and `thisObject` describe two different relationships. Every
// property below a scene object keeps the outer layer id, while `objectId`
// identifies the precise effect/material instance that owns the bound value.
struct ScriptPropertyOwner final {
    std::optional<int> layerId;
    ScriptPropertyOwnerType type = ScriptPropertyOwnerType::none;
    std::string objectId;
    std::string property;
};

class ScriptPropertyObjectRegistry final {
public:
    ScriptPropertyObjectRegistry();
    ~ScriptPropertyObjectRegistry();

    ScriptPropertyObjectRegistry(const ScriptPropertyObjectRegistry&) = delete;
    ScriptPropertyObjectRegistry& operator=(
        const ScriptPropertyObjectRegistry&
    ) = delete;

    // Updates host-owned values for one stable effect/material instance while
    // retaining script overlays for properties that still exist.
    void setBaseObject(ScriptPropertyObjectDescriptor object);
    [[nodiscard]] std::optional<ScriptPropertyObjectDescriptor> find(
        std::string_view id
    ) const;
    [[nodiscard]] std::optional<RuntimeValue> read(
        std::string_view id,
        std::string_view property
    ) const;
    void write(std::string_view id, std::string property, RuntimeValue value);
    [[nodiscard]] std::optional<RuntimeValue> takePendingWrite(
        std::string_view id,
        std::string_view property
    );
    void commit(
        std::string_view id,
        std::string_view property,
        RuntimeValue value
    );

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class ScriptLayerRegistry final {
public:
    ScriptLayerRegistry();
    ~ScriptLayerRegistry();

    ScriptLayerRegistry(const ScriptLayerRegistry&) = delete;
    ScriptLayerRegistry& operator=(const ScriptLayerRegistry&) = delete;

    // Replaces host-owned values while retaining script-owned overlays for
    // layers that still exist.  Ordering is preserved for enumerateLayers().
    void setBaseLayers(std::vector<ScriptLayerDescriptor> layers);
    void setRuntimeSeconds(double runtimeSeconds);
    void setTextureAnimationResolver(
        std::function<std::optional<ScriptTextureAnimationMetadata>(
            std::string_view
        )> resolver
    );

    [[nodiscard]] std::vector<ScriptLayerDescriptor> enumerate() const;
    [[nodiscard]] std::optional<ScriptLayerDescriptor> find(int id) const;
    [[nodiscard]] std::optional<ScriptLayerDescriptor> find(
        std::string_view name
    ) const;
    [[nodiscard]] std::optional<RuntimeValue> read(
        int id,
        std::string_view property
    ) const;
    [[nodiscard]] std::optional<Value> initialLayerConfig(int id) const;
    [[nodiscard]] std::optional<ScriptLayerDescriptor> initialLayerDescriptor(
        int id
    ) const;
    [[nodiscard]] ScriptLayerDescriptor createLayer(
        int templateId,
        std::map<std::string, RuntimeValue> propertyOverrides
    );
    [[nodiscard]] bool destroyLayer(int id);
    [[nodiscard]] std::size_t layerCount() const;
    [[nodiscard]] std::optional<std::size_t> layerIndex(int id) const;
    [[nodiscard]] bool sortLayer(int id, std::size_t index);

    // A setter is a real host write. Unknown layers/properties are rejected
    // explicitly instead of manufacturing an empty/fake layer object.
    void write(int id, std::string property, RuntimeValue value);

    // Returns and clears the mutation generated by the most recent script
    // invocation for this property. The effective overlay remains readable by
    // subsequent scripts and frame planning.
    [[nodiscard]] std::optional<RuntimeValue> takePendingWrite(
        int id,
        std::string_view property
    );

    // Commits a script return value without marking it as a layer mutation.
    void commit(int id, std::string_view property, RuntimeValue value);

    [[nodiscard]] std::optional<ScriptTextureAnimationMetadata>
    textureAnimationMetadata(int id);
    [[nodiscard]] ScriptTextureAnimationSnapshot textureAnimationSnapshot(
        int id
    );
    [[nodiscard]] std::vector<ScriptTextureAnimationSnapshot>
    textureAnimationSnapshots();
    void setTextureAnimationRate(int id, double rate);
    void playTextureAnimation(int id);
    void pauseTextureAnimation(int id);
    void stopTextureAnimation(int id);
    void setTextureAnimationFrame(int id, std::size_t frame);
    void joinTextureAnimation(int id);

    [[nodiscard]] ScriptTimelineAnimationSnapshot timelineAnimationSnapshot(
        int id,
        std::string_view name
    );
    void setTimelineAnimationRate(
        int id,
        std::string_view name,
        double rate
    );
    void playTimelineAnimation(int id, std::string_view name);
    void pauseTimelineAnimation(int id, std::string_view name);
    void stopTimelineAnimation(int id, std::string_view name);
    void setTimelineAnimationFrame(
        int id,
        std::string_view name,
        double frame
    );

    [[nodiscard]] ScriptSoundSnapshot soundSnapshot(int id) const;
    [[nodiscard]] std::vector<ScriptSoundSnapshot> soundSnapshots() const;
    void setSoundRuntimeStates(
        const std::vector<ScriptSoundRuntimeSnapshot>& states
    );
    void playSound(int id);
    void pauseSound(int id);
    void stopSound(int id);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class ScriptInstance;

class ScriptRuntime final {
public:
    explicit ScriptRuntime(ScriptLimits limits = {});
    ~ScriptRuntime();

    ScriptRuntime(const ScriptRuntime&) = delete;
    ScriptRuntime& operator=(const ScriptRuntime&) = delete;

    [[nodiscard]] std::unique_ptr<ScriptInstance> createInstance(
        std::string moduleSource,
        RuntimeValue initialValue,
        std::map<std::string, RuntimeValue> properties = {},
        std::optional<std::string> condition = std::nullopt,
        std::shared_ptr<ScriptLayerRegistry> layerRegistry = nullptr,
        std::shared_ptr<ScriptPropertyObjectRegistry> propertyObjectRegistry =
            nullptr,
        ScriptPropertyOwner owner = {}
    );

private:
    friend class ScriptInstance;
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

class ScriptInstance final {
public:
    ~ScriptInstance();

    ScriptInstance(const ScriptInstance&) = delete;
    ScriptInstance& operator=(const ScriptInstance&) = delete;

    // Evaluates one synchronous frame. Every instance exposes stable
    // `thisLayer` and `thisScene` objects to its module. Reading or writing
    // `thisLayer.text` opts that instance into the Linux text-layer contract:
    // text mutations persist across frames, while a string returned from
    // update() takes precedence. `thisScene` exposes the current runtime and
    // frame-time values (plus Linux-compatible time/currentTime, dt and fps
    // aliases). `engine.time` is also a read-only runtime alias. Host cursor
    // events and changed media snapshots are delivered synchronously after
    // init and before the frame's update callback.
    [[nodiscard]] RuntimeValue evaluate(const ScriptFrameInputs& inputs);
    [[nodiscard]] RuntimeValue currentValue() const;
    // True after module/init evaluation finds at least one cursor callback
    // export. The module source is immutable for the lifetime of the instance.
    [[nodiscard]] bool hasCursorCallbacks() const;

    // Mirrors an upstream DynamicValue connection update. This replaces the
    // script's current value without recreating its module or rerunning init.
    void updateCurrent(RuntimeValue value);

    // Replaces the host-owned script-property values without replacing the
    // JavaScript scriptProperties object. Scripts may safely retain a
    // reference to that object across frames.
    void updateProperties(std::map<std::string, RuntimeValue> properties);

    // System audio capture is intentionally outside the migration goal.
    [[noreturn]] void registerAudioBuffers();

private:
    friend class ScriptRuntime;
    struct Impl;
    explicit ScriptInstance(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

}  // namespace we::scene::script

#endif
